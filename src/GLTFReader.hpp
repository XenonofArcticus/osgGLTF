// GLTFReader.h - standalone OSG glTF 2.0 reader, no osgEarth dependency.
// Derived from osgEarth's GLTFReader (Pelican Mapping, LGPL 2+).
//
// Stripped: URI class, InstanceBuilder, StateTransition, shaderGenerator,
// DiscardAlphaFragments, Mutexed<UnorderedMap>, OWT_state extension.
//
// Replaced: osgEarth::URI image loading -> osgDB::readImageFile,
// osgEarth logging macros -> OSG_WARN/GLTF_NOTIFY,
// osgEarth mutex wrapper -> std::mutex + std::lock_guard.

// Private implementation detail: TinyGLTF.cpp instantiates tinygltf/STB. Reader.cpp includes the
// tinygltf declarations before this file. Consumers use osgGLTF/Reader.hpp and never include this
// implementation.

#pragma once

#include <osg/Node>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osg/CullFace>
#include <osg/BlendFunc>
#include <osg/BufferIndexBinding>
#include <osg/BufferObject>
#include <osg/Depth>
#include <osg/Math>

#include <osgUtil/SmoothingVisitor>

#include <osgDB/FileNameUtils>
#include <osgDB/ReadFile>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>

#include <osgGLTF/Shader.hpp>
#include <osgGLTF/Reader.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
#include <string>

// tiny_gltf.h is intentionally NOT included here - see file comment above.

#ifndef GL_SRGB8
# define GL_SRGB8 0x8C41
#endif
#ifndef GL_SRGB8_ALPHA8
# define GL_SRGB8_ALPHA8 0x8C43
#endif

#include "Animation.hpp"
#include "Log.hpp"
#include "Skin.hpp"

class GLTFReader {
public:
	using Stage = osgGLTF::Reader::Stage;
	using ProgressCallback = osgGLTF::Reader::ProgressCallback;

	struct TextureCache {
		std::mutex mutex;
		std::unordered_map<std::string, osg::ref_ptr<osg::Texture2D>> map;
	};

	struct Env {
		Env(const std::string& loc, const osgDB::Options* opt):
		referrer(loc),
		readOptions(opt) {}

		std::string referrer;
		const osgDB::Options* readOptions;
	};

	mutable TextureCache* _texCache = nullptr;

	void setTextureCache(TextureCache* cache) const { _texCache = cache; }

	static std::string ExpandFilePath(const std::string& filepath, void* userData) {
		const std::string& referrer = *static_cast<const std::string*>(userData);

		std::string path = osgDB::getRealPath(
			osgDB::isAbsolutePath(filepath) ? filepath :
			osgDB::concatPaths(osgDB::getFilePath(referrer), filepath)
		);

		return tinygltf::ExpandFilePath(path, userData);
	}

private:
	// Context threaded through tinygltf's C-function-pointer image loader via its
	// void* user_pointer param (same pattern FsCallbacks.user_data already uses above).
	// Not const-qualified since imagesLoaded is ticked from inside the callback.
	struct ImageLoadContext {
		const ProgressCallback* progress;
		size_t imagesLoaded = 0;
		size_t totalImages = 0;
	};

	// No-op image loader used only for the metadata-only pre-pass below - skips decode
	// entirely, just lets tinygltf finish parsing the JSON model (including model.images,
	// which is populated from the JSON regardless of whether pixels get decoded).
	static bool skipImageLoad(
		tinygltf::Image*,
		const int,
		std::string*,
		std::string*,
		int,
		int,
		const unsigned char*,
		int,
		void*
	) {
		return true;
	}

	// Real (non-skipping) image loader: decodes via tinygltf's own default LoadImageData,
	// then ticks LoadingTextures progress. IMPORTANT: passes nullptr as LoadImageData's own
	// user_data, NOT our ImageLoadContext - tinygltf::LoadImageData() reinterpret_casts a
	// non-null user_data to LoadImageDataOption*, so forwarding our context there would read
	// garbage. nullptr matches the behavior tinygltf uses by default when SetImageLoader() is
	// never called at all, so this is not a behavior change vs. the old unhooked path.
	static bool realImageLoader(
		tinygltf::Image* image,
		const int imageIdx,
		std::string* err,
		std::string* warn,
		int reqWidth,
		int reqHeight,
		const unsigned char* bytes,
		int size,
		void* userData
	) {
		auto* ctx = static_cast<ImageLoadContext*>(userData);

		bool ok = tinygltf::LoadImageData(
			image, imageIdx, err, warn, reqWidth, reqHeight, bytes, size, nullptr
		);

		if(ctx && ctx->progress && *ctx->progress) {
			ctx->imagesLoaded++;
			(*ctx->progress)(Stage::LoadingTextures, ctx->imagesLoaded, ctx->totalImages);
		}

		return ok;
	}

public:
	osgDB::ReaderWriter::ReadResult read(
		const std::string& location,
		bool isBinary,
		const osgDB::Options* readOptions,
		const ProgressCallback& progress=nullptr
	) const {
		std::string err, warn;
		tinygltf::Model model;
		tinygltf::TinyGLTF loader;
		ImageLoadContext imageLoadContext;

		tinygltf::FsCallbacks fs;

		fs.FileExists = &tinygltf::FileExists;
		fs.ExpandFilePath = &GLTFReader::ExpandFilePath;
		fs.ReadWholeFile = &tinygltf::ReadWholeFile;
		fs.WriteWholeFile = &tinygltf::WriteWholeFile;
		fs.user_data = const_cast<std::string*>(&location);

		loader.SetFsCallbacks(fs);

		GLTF_NOTIFY(0) << "loading " << location << std::endl;

		if(progress) progress(Stage::Parsing, 0, 1);

		// Cheap metadata-only pre-pass: parses the JSON header (and resolves buffer/image
		// URIs) without decoding any image bytes, purely to learn the real image count up
		// front so LoadingTextures below can report a real total instead of an
		// indeterminate one. Reuses the same no-op image loader osgGLTF-python.cpp's
		// inspect(load_images=False) already relies on for the identical reason.
		{
			tinygltf::Model countModel;
			tinygltf::TinyGLTF counter;
			std::string countErr, countWarn;

			counter.SetFsCallbacks(fs);
			counter.SetImageLoader(&skipImageLoad, nullptr);

			bool countOk = isBinary
				? counter.LoadBinaryFromFile(&countModel, &countErr, &countWarn, location)
				: counter.LoadASCIIFromFile (&countModel, &countErr, &countWarn, location)
			;

			imageLoadContext.totalImages = countOk ? countModel.images.size() : 0;
		}

		if(progress) progress(Stage::Parsing, 1, 1);

		imageLoadContext.progress = &progress;
		imageLoadContext.imagesLoaded = 0;

		if(progress) progress(Stage::LoadingTextures, 0, imageLoadContext.totalImages);

		loader.SetImageLoader(&realImageLoader, &imageLoadContext);

		// tinygltf's own file/buffer decode is one opaque blocking call, but image decode
		// (the real bottleneck for texture-heavy assets) is now hooked via realImageLoader
		// above, so LoadingTextures progress ticks in real time as each image finishes.
		bool ok = isBinary
			? loader.LoadBinaryFromFile(&model, &err, &warn, location)
			: loader.LoadASCIIFromFile (&model, &err, &warn, location)
		;

		if(!warn.empty()) OSG_WARN << "" << location << ": " << warn << std::endl;

		if(!ok || !err.empty()) {
			OSG_WARN << "failed to load " << location << ": " << err << std::endl;

			return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
		}

		GLTF_NOTIFY(0)
			<< model.meshes.size() << " mesh(es), "
			<< model.accessors.size() << " accessor(s), "
			<< model.bufferViews.size()<< " bufferView(s), "
			<< model.buffers.size() << " buffer(s), "
			<< model.images.size() << " image(s)" << std::endl
		;

		logAnimationBits(model);

		Env env(location, readOptions);

		return makeNodeFromModel(model, env, progress);
	}

	void logAnimationBits(const tinygltf::Model& model) const {
		if(model.skins.empty() && model.animations.empty()) return;

		GLTF_NOTIFY(0)
			<< model.skins.size() << " skin(s), "
			<< model.animations.size() << " animation(s)" << std::endl
		;

		for(size_t skinIdx = 0; skinIdx < model.skins.size(); skinIdx++) {
			const auto& skin = model.skins[skinIdx];

			GLTF_NOTIFY(1)
				<< "skin[" << skinIdx << "] '" << skin.name << "'"
				<< " joints=" << skin.joints.size()
				<< " skeleton=" << skin.skeleton
				<< " inverseBindMatrices=" << skin.inverseBindMatrices << std::endl
			;

			for(size_t jointIdx = 0; jointIdx < skin.joints.size(); jointIdx++) {
				int nodeIdx = skin.joints[jointIdx];
				const char* nodeName =
					nodeIdx >= 0 && nodeIdx < static_cast<int>(model.nodes.size())
					? model.nodes[nodeIdx].name.c_str()
					: ""
				;

				GLTF_NOTIFY(2)
					<< "joint[" << jointIdx << "]"
					<< " node=" << nodeIdx
					<< " '" << nodeName << "'" << std::endl
				;
			}
		}

		for(size_t animIdx = 0; animIdx < model.animations.size(); animIdx++) {
			const auto& animation = model.animations[animIdx];

			GLTF_NOTIFY(1)
				<< "animation[" << animIdx << "] '" << animation.name << "'"
				<< " channels=" << animation.channels.size()
				<< " samplers=" << animation.samplers.size() << std::endl
			;

			for(size_t samplerIdx = 0; samplerIdx < animation.samplers.size(); samplerIdx++) {
				const auto& sampler = animation.samplers[samplerIdx];

				GLTF_NOTIFY(2)
					<< "sampler[" << samplerIdx << "]"
					<< " input=" << sampler.input
					<< " output=" << sampler.output
					<< " interpolation=" << sampler.interpolation << std::endl
				;
			}

			for(size_t channelIdx = 0; channelIdx < animation.channels.size(); channelIdx++) {
				const auto& channel = animation.channels[channelIdx];
				const char* nodeName =
					channel.target_node >= 0 &&
					channel.target_node < static_cast<int>(model.nodes.size())
					? model.nodes[channel.target_node].name.c_str()
					: ""
				;

				GLTF_NOTIFY(2)
					<< "channel[" << channelIdx << "]"
					<< " sampler=" << channel.sampler
					<< " targetNode=" << channel.target_node
					<< " '" << nodeName << "'"
					<< " path=" << channel.target_path << std::endl
				;
			}
		}
	}

	osg::Node* makeNodeFromModel(
		const tinygltf::Model& model,
		const Env& env,
		const ProgressCallback& progress=nullptr
	) const {
		NodeBuilder builder(this, model, env, progress);

		// glTF is Y-up; rotate to Z-up unless caller passes "gltfZUp"
		bool zUp =
			env.readOptions &&
			env.readOptions->getOptionString().find("gltfZUp") != std::string::npos
		;

		osg::MatrixTransform* root = new osg::MatrixTransform();

		if(!zUp) root->setMatrix(osg::Matrixd::rotate(
			osg::Vec3d(0, 1, 0),
			osg::Vec3d(0, 0, 1)
		));

		for(auto& scene : model.scenes) {
			for(int idx : scene.nodes) {
				if(osg::Node* n = builder.createNode(idx)) root->addChild(n);
			}
		}

		builder.resolveSkinJointNodes();
		builder.installAnimationCallback(root);
		builder.installSkinPaletteCallbacks();

		root->getOrCreateStateSet()->setAttributeAndModes(
			new osg::CullFace(osg::CullFace::BACK),
			osg::StateAttribute::ON
		);

		return root;
	}

	struct NodeBuilder {
		const GLTFReader* reader;
		const tinygltf::Model& model;
		const Env& env;
		ProgressCallback progress;
		std::vector<osg::ref_ptr<osg::Array>> arrays;
		std::vector<osg::ref_ptr<osgGLTF::detail::Skin>> skins;
		mutable std::vector<osg::observer_ptr<osg::MatrixTransform>> nodeTransforms;
		mutable size_t nodesBuilt = 0;

		NodeBuilder(
			const GLTFReader* r,
			const tinygltf::Model& m,
			const Env& e,
			const ProgressCallback& p=nullptr
		):
		reader(r),
		model(m),
		env(e),
		progress(p) {
			nodeTransforms.resize(m.nodes.size());

			GLTF_NOTIFY(0) << "extractArrays - " << m.accessors.size() << " accessor(s)" << std::endl;

			extractArrays();

			GLTF_NOTIFY(0) << "extractArrays done - " << arrays.size() << " array(s) built" << std::endl;

			skins = osgGLTF::detail::prepareSkins(model, arrays);
		}

		osg::Node* createNode(int nodeIdx, unsigned depth = 0) const {
			if(nodeIdx < 0 || nodeIdx >= static_cast<int>(model.nodes.size())) return nullptr;

			const tinygltf::Node& node = model.nodes[nodeIdx];

			GLTF_NOTIFY(depth)
				<< "createNode '" << node.name << "'"
				<< " node=" << nodeIdx
				<< " mesh=" << node.mesh
				<< " skin=" << node.skin
				<< " children=" << node.children.size() << std::endl
			;

			// One tick per node regardless of depth - model.nodes.size() covers every
			// node in the file, not just ones reachable from the active scene, so this
			// is an upper-bound denominator (progress may not hit exactly 100% for a
			// model with unreferenced nodes, which is rare and harmless for a bar).
			nodesBuilt++;

			if(progress) progress(Stage::BuildingNodes, nodesBuilt, model.nodes.size());

			osg::MatrixTransform* mt = new osg::MatrixTransform();

			if(node.matrix.size() == 16) mt->setMatrix(osg::Matrixd(node.matrix.data()));

			if(mt->getMatrix().isIdentity()) {
				osg::Matrixd S, R, T;

				if(node.scale.size() == 3) S = osg::Matrixd::scale(
					node.scale[0],
					node.scale[1],
					node.scale[2]
				);

				if(node.rotation.size() == 4) R.makeRotate(osg::Quat(
					node.rotation[0],
					node.rotation[1],
					node.rotation[2],
					node.rotation[3]
				));

				if(node.translation.size() == 3) T = osg::Matrixd::translate(
					node.translation[0],
					node.translation[1],
					node.translation[2]
				);

				mt->setMatrix(S * R * T);
			}

			nodeTransforms[nodeIdx] = mt;

			if(
				node.skin >= 0 &&
				node.skin < static_cast<int>(skins.size()) &&
				skins[node.skin].valid()
			) skins[node.skin]->skinnedNodes.push_back(mt);

			if(node.mesh >= 0) mt->addChild(makeMesh(model.meshes[node.mesh], node.skin));

			for(int childIdx : node.children) {
				if(osg::Node* c = createNode(childIdx, depth + 1)) mt->addChild(c);
			}

			mt->setName(node.name);

			return mt;
		}

		void resolveSkinJointNodes() {
			osgGLTF::detail::resolveSkinJointNodes(model, nodeTransforms, skins);
		}

		void installSkinPaletteCallbacks() {
			osgGLTF::detail::installSkinPaletteCallbacks(skins);
		}

		void installAnimationCallback(osg::Node* root) const {
			const bool skipAnimation =
				env.readOptions &&
				env.readOptions->getOptionString().find("gltfSkipAnimation") != std::string::npos
			;

			osgGLTF::detail::installAnimationCallback(
				model,
				arrays,
				nodeTransforms,
				root,
				skipAnimation
			);
		}

		osg::Group* makeMesh(const tinygltf::Mesh& mesh, int skinIdx) const {
			GLTF_NOTIFY(1)
				<< "makeMesh '" << mesh.name
				<< "' skin=" << skinIdx
				<< " - " << mesh.primitives.size() << " primitive(s)" << std::endl
			;

			osg::Group* group = new osg::Group();

			group->setName(mesh.name);

			int primIdx = 0;

			for(auto& primitive : mesh.primitives) {
				GLTF_NOTIFY(2)
					<< "primitive[" << primIdx << "]"
					<< " mode=" << primitive.mode
					<< " indices=" << primitive.indices
					<< " material=" << primitive.material
					<< " attrs=" << primitive.attributes.size() << std::endl
				;

				osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();

				geom->setName(typeid(*this).name());
				geom->setUseVertexBufferObjects(true);

				osg::Vec4 baseColorFactor(1, 1, 1, 1);

				// vertex attributes - parsed before material application since
				// texture-unit binding needs to know which UV set (TEXCOORD_n)
				// each texture actually asks for.
				GLTF_NOTIFY(3) << "attributes:" << std::endl;

				std::map<int, osg::Array*> texCoordSets;
				int jointsAccessor = -1;
				int weightsAccessor = -1;

				for(auto& [attrName, accessorIdx] : primitive.attributes) {
					bool valid =
						accessorIdx >= 0 &&
						accessorIdx < static_cast<int>(arrays.size()) &&
						arrays[accessorIdx].valid()
					;

					GLTF_NOTIFY(4)
						<< "" << attrName
						<< " -> accessor[" << accessorIdx << "]"
						<< (valid ? " OK" : " NULL/INVALID") << std::endl
					;

					if(!valid) continue;

					if(attrName == "POSITION") geom->setVertexArray(arrays[accessorIdx].get());
					else if(attrName == "NORMAL") geom->setNormalArray(arrays[accessorIdx].get());
					else if(attrName == "COLOR_0") geom->setColorArray(arrays[accessorIdx].get());
					else if(attrName == "TANGENT") {
						arrays[accessorIdx]->setBinding(osg::Array::BIND_PER_VERTEX);

						geom->setVertexAttribArray(
							osgGLTF::shader::TANGENT_ATTRIBUTE,
							arrays[accessorIdx].get()
						);
					}
					else if(attrName.rfind("TEXCOORD_", 0) == 0) {
						int uvSet = std::atoi(attrName.c_str() + 9);

						texCoordSets[uvSet] = arrays[accessorIdx].get();
					}

					else if(attrName == "JOINTS_0") jointsAccessor = accessorIdx;
					else if(attrName == "WEIGHTS_0") weightsAccessor = accessorIdx;
				}

				if(jointsAccessor >= 0 || weightsAccessor >= 0) {
					GLTF_NOTIFY(3)
						<< "skinning attrs:"
						<< " JOINTS_0=" << jointsAccessor
						<< " WEIGHTS_0=" << weightsAccessor << std::endl
					;

					if(skinIdx >= 0 && skinIdx < static_cast<int>(skins.size())) {
						if(jointsAccessor >= 0) {
							arrays[jointsAccessor]->setBinding(osg::Array::BIND_PER_VERTEX);
							arrays[jointsAccessor]->setPreserveDataType(true);
							geom->setVertexAttribArray(
								osgGLTF::shader::JOINT_INDICES_ATTRIBUTE,
								arrays[jointsAccessor].get()
							);
						}

						if(weightsAccessor >= 0) {
							arrays[weightsAccessor]->setBinding(osg::Array::BIND_PER_VERTEX);
							geom->setVertexAttribArray(
								osgGLTF::shader::JOINT_WEIGHTS_ATTRIBUTE,
								arrays[weightsAccessor].get()
							);
						}
					}

					else {
						GLTF_NOTIFY(3)
							<< "skinning attrs present, but node has no valid skin; not binding them"
							<< std::endl
						;
					}
				}

				if(
					primitive.material >= 0 &&
					primitive.material < static_cast<int>(model.materials.size())
				) {
					GLTF_NOTIFY(3) << "applyMaterial " << primitive.material << std::endl;

					applyMaterial(primitive.material, baseColorFactor, geom.get(), texCoordSets);
				}

				// fall-back solid color if no COLOR_0
				if(!geom->getColorArray()) {
					auto* verts = static_cast<osg::Vec3Array*>(geom->getVertexArray());
					size_t count = verts ? verts->size() : 1;
					auto* colors = new osg::Vec4Array(count);

					std::fill(colors->begin(), colors->end(), baseColorFactor);

					geom->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
				}

				// index primitive set - handles uint8, uint16, uint32
				if(
					primitive.indices >= 0 &&
					primitive.indices < static_cast<int>(arrays.size()) &&
					arrays[primitive.indices].valid()
				) {
					int glMode = primitiveMode(primitive.mode);
					const tinygltf::Accessor& idxAcc = model.accessors[primitive.indices];

					switch(idxAcc.componentType) {
						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
							auto* src = static_cast<osg::UByteArray*>(arrays[primitive.indices].get());
							auto* de = new osg::DrawElementsUByte(glMode, idxAcc.count);

							std::copy(src->begin(), src->end(), de->begin());

							geom->addPrimitiveSet(de);

							break;
						}

						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
							auto* src = static_cast<osg::UShortArray*>(arrays[primitive.indices].get());

							geom->addPrimitiveSet(new osg::DrawElementsUShort(glMode, src->begin(), src->end()));

							break;
						}

						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
							auto* src = static_cast<osg::UIntArray*>(arrays[primitive.indices].get());

							geom->addPrimitiveSet( new osg::DrawElementsUInt(glMode, src->begin(), src->end()));

							break;
						}

						default:
							OSG_WARN
								<< "unsupported index component type "
								<< idxAcc.componentType << std::endl
							;
					}
				}

				else {
					// non-indexed: draw all vertices
					auto* verts = static_cast<osg::Vec3Array*>(geom->getVertexArray());

					if(verts) geom->addPrimitiveSet(new osg::DrawArrays(
						primitiveMode(primitive.mode),
						0,
						verts->size()
					));
				}

				// Auto-generate normals for triangle primitives that don't supply them.
				// SmoothingVisitor assumes triangles - never call it on points/lines.
				bool isTriangles = (
					primitive.mode == TINYGLTF_MODE_TRIANGLES ||
					primitive.mode == TINYGLTF_MODE_TRIANGLE_STRIP ||
					primitive.mode == TINYGLTF_MODE_TRIANGLE_FAN
				);

				bool skipNormals =
					env.readOptions &&
					env.readOptions->getOptionString().find("gltfSkipNormals") != std::string::npos
				;

				osg::Geode* geode = new osg::Geode();

				geode->addDrawable(geom);

				if(isTriangles && !skipNormals && !geom->getNormalArray()) {
					GLTF_NOTIFY(3) << "generating normals via SmoothingVisitor" << std::endl;

					osgUtil::SmoothingVisitor sv;

					geode->accept(sv);
				}

				GLTF_NOTIFY(3) << "addChild geode to mesh group" << std::endl;

				group->addChild(geode);

				primIdx++;
			}

			return group;
		}

		// ---- material ------------------------------------------------ //
		// Fixed-function multitexturing ties "which GL texture unit" to "which
		// TexCoordArray is bound to that unit" - so each texture channel gets a
		// fixed unit (base/diffuse=0, normal=1, MR/specGloss=2, emissive=3), but
		// the UV data bound to that unit must match what the texture actually
		// requests via textureInfo.texCoord, not just whatever happened to be
		// parsed as TEXCOORD_0/1.
		void applyMaterial(
			int matIdx,
			osg::Vec4& baseColorFactor,
			osg::Geometry* geom,
			const std::map<int, osg::Array*>& texCoordSets
		) const {
			const tinygltf::Material& mat = model.materials[matIdx];
			const auto& pbr = mat.pbrMetallicRoughness;

			// sRGB: per the glTF spec, baseColor/diffuse and emissive textures
			// are authored in sRGB gamma space; normal and ORM (occlusion/
			// roughness/metallic) textures are linear data, not color, and
			// must never be gamma-decoded.
			auto bindTexture = [&](int unit, int texIdx, int texCoord, bool sRGB) {
				osg::Texture2D* tex = getOrCreateTexture(texIdx, sRGB);

				if(!tex) return;

				geom->getOrCreateStateSet()->setTextureAttributeAndModes(unit, tex);

				auto it = texCoordSets.find(texCoord);

				if(it != texCoordSets.end()) geom->setTexCoordArray(unit, it->second);
			};

			if(pbr.baseColorFactor.size() == 4) baseColorFactor.set(
				pbr.baseColorFactor[0],
				pbr.baseColorFactor[1],
				pbr.baseColorFactor[2],
				pbr.baseColorFactor[3]
			);

			bool haveCoreBaseColor = pbr.baseColorTexture.index >= 0;

			if(haveCoreBaseColor) bindTexture(
				osgGLTF::shader::BASE_COLOR_TEXTURE_UNIT,
				pbr.baseColorTexture.index,
				pbr.baseColorTexture.texCoord,
				true
			);

			bool haveNormalMap = mat.normalTexture.index >= 0;

			if(haveNormalMap) bindTexture(
				osgGLTF::shader::NORMAL_TEXTURE_UNIT,
				mat.normalTexture.index,
				mat.normalTexture.texCoord,
				false
			);

			// metallicRoughnessTexture and occlusionTexture are often the same
			// image (R=occlusion, G=roughness, B=metallic) - when they share
			// the same texture index, the plain bind below already carries
			// correct AO in R. When occlusionTexture is a genuinely separate
			// image (e.g. SciFiHelmet), bake the two together so real
			// per-pixel AO isn't silently dropped - downstream shaders
			// (09-ibl.py) gate their AO read on the hasOcclusion uniform
			// (exported below) rather than trusting an "unused" R channel
			// when no occlusionTexture is present at all.
			bool haveOcclusion = mat.occlusionTexture.index >= 0;
			bool sameOcclusionImage =
				haveOcclusion &&
				mat.occlusionTexture.index == pbr.metallicRoughnessTexture.index
			;
			bool haveMetallicRoughnessMap = pbr.metallicRoughnessTexture.index >= 0;

			if(haveOcclusion && !sameOcclusionImage) {
				std::string bakeKey = env.referrer + "|orm-occlusion|" + std::to_string(matIdx);
				TextureCache* tc = reader->_texCache;

				osg::ref_ptr<osg::Texture2D> ormTex;

				if(tc) {
					std::lock_guard<std::mutex> lk(tc->mutex);

					auto it = tc->map.find(bakeKey);

					if(it != tc->map.end()) ormTex = it->second;
				}

				if(!ormTex.valid()) {
					osg::ref_ptr<osg::Image> occImg = loadRawImage(mat.occlusionTexture.index);
					osg::ref_ptr<osg::Image> mrImg = loadRawImage(pbr.metallicRoughnessTexture.index);

					if(mat.occlusionTexture.texCoord != pbr.metallicRoughnessTexture.texCoord) {
						GLTF_NOTIFY(3)
							<< "material " << matIdx
							<< ": occlusionTexture and metallicRoughnessTexture use"
							<< " different UV sets - occlusion bake assumes they"
							<< " share the same UV space; result may be UV-mismatched" << std::endl
						;
					}

					osg::ref_ptr<osg::Image> bakedOrm;

					bakeOcclusionIntoOrm(
						occImg.get(),
						static_cast<float>(mat.occlusionTexture.strength),
						mrImg.get(),
						bakedOrm
					);

					int samplerIdx = -1;

					if(
						pbr.metallicRoughnessTexture.index >= 0 &&
						pbr.metallicRoughnessTexture.index < static_cast<int>(model.textures.size())
					) samplerIdx = model.textures[pbr.metallicRoughnessTexture.index].sampler;

					else if(mat.occlusionTexture.index < static_cast<int>(model.textures.size()))
						samplerIdx = model.textures[mat.occlusionTexture.index].sampler;

					ormTex = new osg::Texture2D(bakedOrm.get());

					applyTextureFormatAndSampler(ormTex.get(), bakedOrm.get(), false, samplerIdx);

					ormTex->setUnRefImageDataAfterApply(true);

					if(tc) {
						std::lock_guard<std::mutex> lk(tc->mutex);

						tc->map[bakeKey] = ormTex;
					}
				}

				geom->getOrCreateStateSet()->setTextureAttributeAndModes(
					osgGLTF::shader::ORM_TEXTURE_UNIT,
					ormTex.get()
				);

				auto occTexCoordIt = texCoordSets.find(mat.occlusionTexture.texCoord);

				if(occTexCoordIt != texCoordSets.end()) geom->setTexCoordArray(
					osgGLTF::shader::ORM_TEXTURE_UNIT,
					occTexCoordIt->second
				);
			}

			else if(pbr.metallicRoughnessTexture.index >= 0) bindTexture(
				osgGLTF::shader::ORM_TEXTURE_UNIT,
				pbr.metallicRoughnessTexture.index,
				pbr.metallicRoughnessTexture.texCoord,
				false
			);

			if(mat.emissiveTexture.index >= 0) bindTexture(
				osgGLTF::shader::EMISSIVE_TEXTURE_UNIT,
				mat.emissiveTexture.index,
				mat.emissiveTexture.texCoord,
				true
			);

			// KHR_materials_pbrSpecularGlossiness - legacy but still valid,
			// real Sketchfab-era content uses it, sometimes extension-only
			// with no core pbrMetallicRoughness fallback. Converted to the
			// core metallic-roughness workflow at load time (see
			// bakeSpecGlossToMetalRough) rather than binding
			// specularGlossinessTexture straight into the ORM slot - that
			// texture's channels (RGB=specular color/F0, A=glossiness) don't
			// mean the same thing as ORM's (R=AO, G=roughness, B=metallic),
			// so a direct bind previously fed the shader's Cook-Torrance path
			// garbage roughness/metallic values.
			if(!haveCoreBaseColor) {
				auto extIt = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");

				if(extIt != mat.extensions.end()) {
					const tinygltf::Value& sg = extIt->second;

					osg::Vec4 diffuseFactor(1, 1, 1, 1);

					if(sg.Has("diffuseFactor")) {
						const tinygltf::Value& df = sg.Get("diffuseFactor");

						if(df.IsArray() && df.ArrayLen() == 4) diffuseFactor.set(
							static_cast<float>(df.Get(0).GetNumberAsDouble()),
							static_cast<float>(df.Get(1).GetNumberAsDouble()),
							static_cast<float>(df.Get(2).GetNumberAsDouble()),
							static_cast<float>(df.Get(3).GetNumberAsDouble())
						);
					}

					baseColorFactor = diffuseFactor;

					osg::Vec3 specularFactor(1, 1, 1);

					if(sg.Has("specularFactor")) {
						const tinygltf::Value& sf = sg.Get("specularFactor");

						if(sf.IsArray() && sf.ArrayLen() == 3) specularFactor.set(
							static_cast<float>(sf.Get(0).GetNumberAsDouble()),
							static_cast<float>(sf.Get(1).GetNumberAsDouble()),
							static_cast<float>(sf.Get(2).GetNumberAsDouble())
						);
					}

					float glossinessFactor = 1.0f;

					if(sg.Has("glossinessFactor"))
						glossinessFactor = static_cast<float>(sg.Get("glossinessFactor").GetNumberAsDouble());

					int diffuseIdx = -1, diffuseTexCoord = 0;

					if(sg.Has("diffuseTexture")) {
						const tinygltf::Value& dt = sg.Get("diffuseTexture");

						diffuseIdx = dt.Has("index") ? dt.Get("index").GetNumberAsInt()	: -1;
						diffuseTexCoord = dt.Has("texCoord") ? dt.Get("texCoord").GetNumberAsInt() : 0;
					}

					int specGlossIdx = -1, specGlossTexCoord = 0;

					if(sg.Has("specularGlossinessTexture")) {
						const tinygltf::Value& sgt = sg.Get("specularGlossinessTexture");

						specGlossIdx = sgt.Has("index") ? sgt.Get("index").GetNumberAsInt() : -1;
						specGlossTexCoord = sgt.Has("texCoord") ? sgt.Get("texCoord").GetNumberAsInt() : 0;
					}

					// gltfSkipSpecGlossBake opts out of the metal-rough bake
					// below and falls back to a raw pass-through of
					// diffuseTexture/specularGlossinessTexture, for callers
					// that implement their own spec-gloss BRDF shader branch
					// (Sketchfab's own viewer takes this approach - its
					// Model Inspector panel lists Albedo/Specular/Glossiness
					// as native channels, not a converted metallic-roughness
					// pair) and would rather skip the bake's load-time cost
					// and lossy conversion entirely.
					bool skipSpecGlossBake =
						env.readOptions &&
						env.readOptions->getOptionString().find("gltfSkipSpecGlossBake") != std::string::npos
					;

					if(skipSpecGlossBake) {
						if(diffuseIdx >= 0) bindTexture(
							osgGLTF::shader::BASE_COLOR_TEXTURE_UNIT,
							diffuseIdx,
							diffuseTexCoord,
							true
						);
						if(specGlossIdx >= 0) bindTexture(
							osgGLTF::shader::ORM_TEXTURE_UNIT,
							specGlossIdx,
							specGlossTexCoord,
							true
						);
					}

					else {
						// Every primitive that references this material would
						// otherwise redo the full per-pixel bake from
						// scratch - for a mesh whose parts all share one
						// material (the common case), that's an N-way
						// redundant multi-second cost for identical output.
						// Cache by referrer+matIdx, same TextureCache the
						// texIdx-keyed path already uses.
						std::string bakeKey = env.referrer + "|specgloss|" + std::to_string(matIdx);
						TextureCache* tc = reader->_texCache;

						osg::ref_ptr<osg::Texture2D> bcTex, ormTex;

						if(tc) {
							std::lock_guard<std::mutex> lk(tc->mutex);

							auto bcIt = tc->map.find(bakeKey + "|bc");
							auto ormIt = tc->map.find(bakeKey + "|orm");

							if(bcIt != tc->map.end() && ormIt != tc->map.end()) {
								bcTex = bcIt->second;
								ormTex = ormIt->second;
							}
						}

						if(!bcTex.valid() || !ormTex.valid()) {
							// The bake combines diffuse and specGloss pixel-
							// for-pixel, which only makes sense if both
							// textures share the same UV layout. texCoord
							// index alone doesn't prove that, but it's the
							// cheapest signal we have without comparing UV
							// accessor data - warn instead of silently
							// mis-rendering.
							if(
								diffuseIdx >= 0 &&
								specGlossIdx >= 0 &&
								diffuseTexCoord != specGlossTexCoord
							) {
								GLTF_NOTIFY(3)
									<< "material " << matIdx
									<< ": diffuseTexture and specularGlossinessTexture use"
									" different UV sets (" << diffuseTexCoord << " vs "
									<< specGlossTexCoord << ") - spec-gloss bake assumes they"
									" share the same UV space; result may be UV-mismatched"
									<< std::endl
								;
							}

							osg::ref_ptr<osg::Image> diffuseImg = loadRawImage(diffuseIdx);
							osg::ref_ptr<osg::Image> specGlossImg = loadRawImage(specGlossIdx);
							osg::ref_ptr<osg::Image> bakedBaseColor, bakedOrm;

							// Hint for next time someone sees a multi-second stall between
							// "building nodes" ticks: this CPU per-pixel bake (single-threaded,
							// no SIMD) is the culprit, not I/O or a hang. Cost scales with the
							// larger of diffuseImg/specGlossImg - a 2048x2048 pair alone runs
							// ~2s. TODO: row-parallelize (std::thread/OpenMP) once this becomes
							// a real bottleneck for more assets.
							GLTF_NOTIFY(3)
								<< "material " << matIdx << ": baking specGloss -> metal-rough ("
								<< (diffuseImg.valid() ? diffuseImg->s() : 0) << "x"
								<< (diffuseImg.valid() ? diffuseImg->t() : 0) << " diffuse, "
								<< (specGlossImg.valid() ? specGlossImg->s() : 0) << "x"
								<< (specGlossImg.valid() ? specGlossImg->t() : 0) << " specGloss)"
								" - single-threaded CPU bake, may take a moment" << std::endl
							;

							bakeSpecGlossToMetalRough(
								diffuseImg.get(),
								diffuseFactor,
								specGlossImg.get(),
								specularFactor,
								glossinessFactor,
								bakedBaseColor,
								bakedOrm
							);

							int samplerIdx = -1;

							if(diffuseIdx >= 0 && diffuseIdx < static_cast<int>(model.textures.size()))
								samplerIdx = model.textures[diffuseIdx].sampler;

							else if(specGlossIdx >= 0 && specGlossIdx < static_cast<int>(model.textures.size()))
								samplerIdx = model.textures[specGlossIdx].sampler;

							// Baked images are already linear (converted, not
							// merely re-encoded), so bind them with
							// sRGB=false - GPU sRGB decode must not run twice.
							bcTex = new osg::Texture2D(bakedBaseColor.get());

							applyTextureFormatAndSampler(bcTex.get(), bakedBaseColor.get(), false, samplerIdx);
							bcTex->setUnRefImageDataAfterApply(true);

							ormTex = new osg::Texture2D(bakedOrm.get());

							applyTextureFormatAndSampler(ormTex.get(), bakedOrm.get(), false, samplerIdx);
							ormTex->setUnRefImageDataAfterApply(true);

							if(tc) {
								std::lock_guard<std::mutex> lk(tc->mutex);

								tc->map[bakeKey + "|bc"] = bcTex;
								tc->map[bakeKey + "|orm"] = ormTex;
							}
						}

						geom->getOrCreateStateSet()->setTextureAttributeAndModes(
							osgGLTF::shader::BASE_COLOR_TEXTURE_UNIT,
							bcTex.get()
						);
						geom->getOrCreateStateSet()->setTextureAttributeAndModes(
							osgGLTF::shader::ORM_TEXTURE_UNIT,
							ormTex.get()
						);

						// The bake always produces a real (at-least-1x1) baseColor
						// + ORM texture even for factor-only spec-gloss materials
						// - see bakeSpecGlossToMetalRough's comment -- so both
						// slots are genuinely populated from here on, regardless
						// of what the core pbrMetallicRoughness JSON block did or
						// didn't declare.
						haveCoreBaseColor = true;
						haveMetallicRoughnessMap = true;

						int bakeTexCoord = (diffuseIdx >= 0) ? diffuseTexCoord : specGlossTexCoord;
						auto texCoordIt = texCoordSets.find(bakeTexCoord);

						if(texCoordIt != texCoordSets.end()) {
							geom->setTexCoordArray(
								osgGLTF::shader::BASE_COLOR_TEXTURE_UNIT,
								texCoordIt->second
							);
							geom->setTexCoordArray(
								osgGLTF::shader::ORM_TEXTURE_UNIT,
								texCoordIt->second
							);
						}

						// metallicFactor/roughnessFactor uniforms (added
						// below) default to 1.0 for extension-only materials
						// - tinygltf always populates pbrMetallicRoughness
						// with spec defaults even without a core JSON block
						// present, so the baked-in per-pixel metallic/
						// roughness above won't get double-multiplied.
					}
				}
			}

			// Export the material as a single osgGLTF_Material UBO for downstream PBR shaders
			// (e.g. pyosg-lighting/09-ibl.py) instead of one osg::Uniform per field - this is
			// this plugin's own extension to the material interface, not part of OSG's osg_*
			// built-in uniform set, so it's namespaced (block name + binding) to avoid colliding
			// with an unrelated shader's own material uniforms.
			//
			// std140 layout (must match the GLSL `layout(std140, binding = N) uniform
			// osgGLTF_Material { ... }` block exactly - see 09-ibl.py):
			//
			// vec4 baseColorFactor offset 0 (16 bytes)
			// float roughnessFactor offset 16
			// float metallicFactor offset 20
			// float hasBaseColorMap offset 24
			// float hasMetallicRoughnessMap offset 28
			// float hasOcclusion offset 32
			// float hasNormalMap offset 36
			// (2 floats padding to round the block up to a multiple of 16) offset 40, 44
			//
			// haveOcclusion/haveMetallicRoughnessMap/haveCoreBaseColor/haveNormalMap are gates so
			// a factor-only material (no texture at all - e.g. Fox's roughnessFactor=0.58 with
			// no metallicRoughnessTexture) doesn't get its authored factor silently discarded by
			// an unconditional texture() read of an unbound unit.
			osg::ref_ptr<osg::FloatArray> materialData = new osg::FloatArray(12);

			(*materialData)[0] = baseColorFactor.x();
			(*materialData)[1] = baseColorFactor.y();
			(*materialData)[2] = baseColorFactor.z();
			(*materialData)[3] = baseColorFactor.w();
			(*materialData)[4] = static_cast<float>(pbr.roughnessFactor);
			(*materialData)[5] = static_cast<float>(pbr.metallicFactor);
			(*materialData)[6] = haveCoreBaseColor ? 1.0f : 0.0f;
			(*materialData)[7] = haveMetallicRoughnessMap ? 1.0f : 0.0f;
			(*materialData)[8] = haveOcclusion ? 1.0f : 0.0f;
			(*materialData)[9] = haveNormalMap ? 1.0f : 0.0f;
			(*materialData)[10] = 0.0f;
			(*materialData)[11] = 0.0f;

			materialData->setBufferObject(new osg::UniformBufferObject());

			geom->getOrCreateStateSet()->setAttributeAndModes(
				new osg::UniformBufferBinding(
					osgGLTF::shader::MATERIAL_UBO_BINDING,
					materialData.get(),
					0,
					static_cast<GLsizeiptr>(materialData->getTotalDataSize())
				),
				osg::StateAttribute::ON
			);


			// Alpha coverage is a core glTF material property, but osgGLTF deliberately
			// does not impose a particular PBR shader. Export its values as namespaced
			// uniforms so downstream shaders can apply the required fragment discard
			// (MASK) or write the source alpha (BLEND). alphaMode is encoded as:
			// 0 = OPAQUE, 1 = MASK, 2 = BLEND. The glTF default alphaCutoff is 0.5.
			float alphaMode = osgGLTF::shader::ALPHA_MODE_OPAQUE;
			if(mat.alphaMode == "MASK") alphaMode = osgGLTF::shader::ALPHA_MODE_MASK;
			else if(mat.alphaMode == "BLEND") alphaMode = osgGLTF::shader::ALPHA_MODE_BLEND;

			auto* stateSet = geom->getOrCreateStateSet();
			stateSet->addUniform(new osg::Uniform(
				osgGLTF::shader::ALPHA_MODE_UNIFORM,
				alphaMode
			));
			stateSet->addUniform(new osg::Uniform(
				osgGLTF::shader::ALPHA_CUTOFF_UNIFORM,
				static_cast<float>(mat.alphaCutoff)
			));

			if(mat.alphaMode == "BLEND") {
				// glTF BLEND uses conventional non-premultiplied source-over alpha.
				// Render it after opaque geometry and leave depth testing enabled while
				// preventing a transparent surface from occluding later transparent draws.
				stateSet->setMode(GL_BLEND, osg::StateAttribute::ON);
				stateSet->setAttributeAndModes(
					new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA),
					osg::StateAttribute::ON
				);
				stateSet->setAttributeAndModes(
					new osg::Depth(osg::Depth::LEQUAL, 0.0, 1.0, false),
					osg::StateAttribute::ON
				);
				stateSet->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
			}

			// Per the glTF spec, doubleSided disables backface culling for THIS
			// material specifically (thin single-sided sheets like capes/cloth/leaves
			// with no back geometry, meant to be visible and lit from both sides).
			// readTopNode() enables GL_CULL_FACE unconditionally at the root; this
			// per-geometry override on the child StateSet takes precedence over that
			// ancestor mode (no OVERRIDE flag was used at the root, so normal OSG
			// StateSet inheritance already lets a more-specific child win). The
			// matching back-face normal flip belongs in whatever fragment shader
			// consumes this geometry (gl_FrontFacing-based) -- not this loader's
			// concern, since osgGLTF doesn't ship its own PBR shader.
			if(mat.doubleSided) {
				geom->getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
			}
		}

		// Decodes a glTF texture's source image to raw pixel data - no GL
		// format/sRGB tagging, no caching. Shared by getOrCreateTexture()
		// and the spec-gloss->metal-rough bake below, which both need pixel
		// access independent of how the image ends up being sampled.
		osg::Image* loadRawImage(int texIdx) const {
			if(texIdx < 0 || texIdx >= static_cast<int>(model.textures.size())) return nullptr;

			const tinygltf::Texture& tex = model.textures[texIdx];

			if(tex.source < 0 || tex.source >= static_cast<int>(model.images.size())) return nullptr;

			const tinygltf::Image& image = model.images[tex.source];

			osg::ref_ptr<osg::Image> img;

			if(image.image.size() > 0) {
				// Image data already decoded by tiny_gltf (embedded or preloaded).
				GLenum fmt = (image.component == 4) ? GL_RGBA : GL_RGB;
				GLenum ifmt = (image.component == 4) ? GL_RGBA8 : GL_RGB8;
				auto* data = new unsigned char[image.image.size()];

				memcpy(data, image.image.data(), image.image.size());

				img = new osg::Image();

				img->setImage(
					image.width,
					image.height,
					1,
					ifmt,
					fmt,
					GL_UNSIGNED_BYTE,
					data,
					osg::Image::USE_NEW_DELETE
				);
			}

			else if(!image.uri.empty() && !tinygltf::IsDataURI(image.uri)) {
				std::string path = osgDB::concatPaths(
					osgDB::getFilePath(env.referrer),
					image.uri
				);

				img = osgDB::readImageFile(path, env.readOptions);

				if(img.valid()) img->flipVertical();
			}

			return img.release();
		}

		// Sets internal (sRGB-aware) format and sampler filter/wrap state on
		// a Texture2D already constructed from `img`. Shared between
		// texIdx-addressed textures and synthetic (baked) images.
		void applyTextureFormatAndSampler(
			osg::Texture2D* osgTex,
			osg::Image* img,
			bool sRGB,
			int samplerIdx
		) const {
			if(img->getPixelFormat() == GL_RGB) img->setInternalTextureFormat(sRGB ? GL_SRGB8 : GL_RGB8);
			if(img->getPixelFormat() == GL_RGBA) img->setInternalTextureFormat(sRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8);

			osgTex->setResizeNonPowerOfTwoHint(false);
			osgTex->setDataVariance(osg::Object::STATIC);

			if(samplerIdx >= 0 && samplerIdx < static_cast<int>(model.samplers.size())) {
				const tinygltf::Sampler& s = model.samplers[samplerIdx];
				// Force mipmap min-filter regardless of what the sampler says,
				// since we don't generate mipmaps on load.
				osgTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
				osgTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
				osgTex->setWrap(osg::Texture::WRAP_S, static_cast<osg::Texture::WrapMode>(s.wrapS));
				osgTex->setWrap(osg::Texture::WRAP_T, static_cast<osg::Texture::WrapMode>(s.wrapT));
			}

			else {
				osgTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
				osgTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
				osgTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
				osgTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
			}
		}

		// sRGB must be known per-texture-*use*, not per-image-file: the same
		// image could in principle be referenced once as a color texture and
		// once as linear data, so the cache key includes the color-space flag
		// to avoid one use silently reusing the other's decode setting.
		osg::Texture2D* getOrCreateTexture(int texIdx, bool sRGB) const {
			if(texIdx < 0 || texIdx >= static_cast<int>(model.textures.size())) return nullptr;

			const tinygltf::Texture& tex = model.textures[texIdx];

			if(tex.source < 0 || tex.source >= static_cast<int>(model.images.size())) return nullptr;

			const tinygltf::Image& image = model.images[tex.source];
			bool embedded = image.image.size() > 0 || (!image.uri.empty() && tinygltf::IsDataURI(image.uri));

			// Cache key: resolved file path for external images, empty for embedded.
			std::string cacheKey;

			if(!embedded && !image.uri.empty()) cacheKey = osgDB::getRealPath(osgDB::concatPaths(
				osgDB::getFilePath(env.referrer), image.uri))
				+ (sRGB ? "|sRGB" : "|linear")
			;

			TextureCache* tc = reader->_texCache;

			if(tc && !cacheKey.empty()) {
				std::lock_guard<std::mutex> lk(tc->mutex);

				auto it = tc->map.find(cacheKey);

				if(it != tc->map.end()) return it->second.get();
			}

			osg::ref_ptr<osg::Image> img = loadRawImage(texIdx);

			if(!img.valid()) return nullptr;

			osg::ref_ptr<osg::Texture2D> osgTex = new osg::Texture2D(img.get());

			applyTextureFormatAndSampler(osgTex.get(), img.get(), sRGB, tex.sampler);
			osgTex->setUnRefImageDataAfterApply(embedded);

			if(tc && !cacheKey.empty()) {
				std::lock_guard<std::mutex> lk(tc->mutex);

				tc->map[cacheKey] = osgTex;
			}

			return osgTex.release();
		}

		// KHR_materials_pbrSpecularGlossiness is legacy, but real Sketchfab-era content still uses
		// it, sometimes extension-only with no core pbrMetallicRoughness fallback. Rather than give
		// a shader a second BRDF path to maintain, convert to the core metallic-roughness workflow
		// at load time using the standard reference formula from the (archived) extension spec /
		// glTF-Sample-Viewer / three.js - the same approach those engines use when they don't
		// carry a native spec-gloss BRDF. Must be per-pixel, not per-factor: real content (e.g.
		// Sketchfab's "Dead Space" suit) carries per-part variation in the specular/glossiness
		// texture, not just flat material factors.
		static float srgbToLinear(float c) {
			return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
		}

		static float solveMetallic(float diffuse, float specular, float oneMinusSpecularStrength) {
			const float dielectricSpecular = 0.04f;

			if(specular < dielectricSpecular) return 0.0f;

			float a = dielectricSpecular;
			float b = diffuse * oneMinusSpecularStrength / (1.0f - dielectricSpecular) + specular - 2.0f * dielectricSpecular;
			float c = dielectricSpecular - specular;
			float D = b * b - 4.0f * a * c;

			if(D < 0.0f) return 0.0f;

			return osg::clampBetween((-b + std::sqrt(D)) / (2.0f * a), 0.0f, 1.0f);
		}

		// Bakes new baseColor (linear, for unit 0) + ORM-style (unit 2:
		// R=AO placeholder - spec-gloss has no occlusion channel, G=roughness,
		// B=metallic) images. Either source image may be null (factor-only
		// material); output is always at least 1x1 so callers can bind
		// unconditionally.
		void bakeSpecGlossToMetalRough(
			osg::Image* diffuseImg,
			const osg::Vec4& diffuseFactor,
			osg::Image* specGlossImg,
			const osg::Vec3& specularFactor,
			float glossinessFactor,
			osg::ref_ptr<osg::Image>& outBaseColor,
			osg::ref_ptr<osg::Image>& outOrm
		) const {
			const float epsilon = 1e-6f;
			const float dielectricSpecular = 0.04f;

			int w = 1, h = 1;

			if(diffuseImg) { w = std::max(w, diffuseImg->s()); h = std::max(h, diffuseImg->t()); }
			if(specGlossImg) { w = std::max(w, specGlossImg->s()); h = std::max(h, specGlossImg->t()); }

			osg::ref_ptr<osg::Image> diffuseR = diffuseImg;

			if(diffuseImg && (diffuseImg->s() != w || diffuseImg->t() != h)) {
				diffuseR = new osg::Image(*diffuseImg);

				diffuseR->scaleImage(w, h, 1);
			}

			osg::ref_ptr<osg::Image> specGlossR = specGlossImg;

			if(specGlossImg && (specGlossImg->s() != w || specGlossImg->t() != h)) {
				specGlossR = new osg::Image(*specGlossImg);

				specGlossR->scaleImage(w, h, 1);
			}

			auto* baseColorData = new unsigned char[static_cast<size_t>(w) * h * 4];
			auto* ormData = new unsigned char[static_cast<size_t>(w) * h * 3];

			for(int y = 0; y < h; y++) {
				for(int x = 0; x < w; x++) {
					osg::Vec4 dTex = diffuseR ? diffuseR->getColor(x, y) : osg::Vec4(1, 1, 1, 1);
					osg::Vec4 sgTex = specGlossR ? specGlossR->getColor(x, y) : osg::Vec4(1, 1, 1, 1);

					// diffuse.rgb and specular.rgb are sRGB-encoded color;
					// glossiness (specGloss alpha) and the factors are linear.
					float dR = srgbToLinear(dTex.x()) * diffuseFactor.x();
					float dG = srgbToLinear(dTex.y()) * diffuseFactor.y();
					float dB = srgbToLinear(dTex.z()) * diffuseFactor.z();
					float sR = srgbToLinear(sgTex.x()) * specularFactor.x();
					float sG = srgbToLinear(sgTex.y()) * specularFactor.y();
					float sB = srgbToLinear(sgTex.z()) * specularFactor.z();
					float glossiness = sgTex.w() * glossinessFactor;

					float specularStrength = std::max(sR, std::max(sG, sB));
					float oneMinusSpecularStrength = 1.0f - specularStrength;
					float maxDiffuse = std::max(dR, std::max(dG, dB));
					float metallic = solveMetallic(maxDiffuse, specularStrength, oneMinusSpecularStrength);

					float invOneMinusMetallic = 1.0f / std::max(1.0f - metallic, epsilon);
					float invMetallic = 1.0f / std::max(metallic, epsilon);
					float diffuseScale = oneMinusSpecularStrength / (1.0f - dielectricSpecular);

					float bcFromDiffuseR = dR * diffuseScale * invOneMinusMetallic;
					float bcFromDiffuseG = dG * diffuseScale * invOneMinusMetallic;
					float bcFromDiffuseB = dB * diffuseScale * invOneMinusMetallic;

					float bcFromSpecR = (sR - dielectricSpecular * (1.0f - metallic)) * invMetallic;
					float bcFromSpecG = (sG - dielectricSpecular * (1.0f - metallic)) * invMetallic;
					float bcFromSpecB = (sB - dielectricSpecular * (1.0f - metallic)) * invMetallic;

					float t = metallic * metallic;
					float baseR = osg::clampBetween(bcFromDiffuseR * (1.0f - t) + bcFromSpecR * t, 0.0f, 1.0f);
					float baseG = osg::clampBetween(bcFromDiffuseG * (1.0f - t) + bcFromSpecG * t, 0.0f, 1.0f);
					float baseB = osg::clampBetween(bcFromDiffuseB * (1.0f - t) + bcFromSpecB * t, 0.0f, 1.0f);
					float roughness = osg::clampBetween(1.0f - glossiness, 0.0f, 1.0f);

					size_t bi = (static_cast<size_t>(y) * w + x) * 4;
					baseColorData[bi + 0] = static_cast<unsigned char>(baseR * 255.0f + 0.5f);
					baseColorData[bi + 1] = static_cast<unsigned char>(baseG * 255.0f + 0.5f);
					baseColorData[bi + 2] = static_cast<unsigned char>(baseB * 255.0f + 0.5f);
					baseColorData[bi + 3] = static_cast<unsigned char>(dTex.w() * diffuseFactor.w() * 255.0f + 0.5f);

					size_t oi = (static_cast<size_t>(y) * w + x) * 3;
					ormData[oi + 0] = 255; // AO: spec-gloss carries no occlusion channel
					ormData[oi + 1] = static_cast<unsigned char>(roughness * 255.0f + 0.5f);
					ormData[oi + 2] = static_cast<unsigned char>(metallic * 255.0f + 0.5f);
				}
			}

			outBaseColor = new osg::Image();
			outBaseColor->setImage(
				w,
				h,
				1,
				GL_RGBA8,
				GL_RGBA,
				GL_UNSIGNED_BYTE,
				baseColorData,
				osg::Image::USE_NEW_DELETE
			);

			outOrm = new osg::Image();
			outOrm->setImage(
				w,
				h,
				1,
				GL_RGB8,
				GL_RGB,
				GL_UNSIGNED_BYTE,
				ormData,
				osg::Image::USE_NEW_DELETE
			);
		}

		// ---- separate occlusionTexture merge --------------------------- //
		// Only needed when occlusionTexture is a genuinely distinct image
		// from metallicRoughnessTexture (e.g. SciFiHelmet) - the common
		// "packed ORM" convention (same texture index for both) already
		// carries correct AO in R via the plain metallicRoughnessTexture
		// bind and never reaches this function. `strength` is baked in
		// directly per the spec formula (occludedColor = lerp(1, r,
		// strength)) so no extra uniform is needed downstream.
		void bakeOcclusionIntoOrm(
			osg::Image* occlusionImg,
			float strength,
			osg::Image* metalRoughImg,
			osg::ref_ptr<osg::Image>& outOrm
		) const {
			int w = 1, h = 1;

			if(occlusionImg) {
				w = std::max(w, occlusionImg->s());
				h = std::max(h, occlusionImg->t());
			}

			if(metalRoughImg) {
				w = std::max(w, metalRoughImg->s());
				h = std::max(h, metalRoughImg->t());
			}

			osg::ref_ptr<osg::Image> occR = occlusionImg;

			if(occlusionImg && (occlusionImg->s() != w || occlusionImg->t() != h)) {
				occR = new osg::Image(*occlusionImg);

				occR->scaleImage(w, h, 1);
			}

			osg::ref_ptr<osg::Image> mrR = metalRoughImg;

			if(metalRoughImg && (metalRoughImg->s() != w || metalRoughImg->t() != h)) {
				mrR = new osg::Image(*metalRoughImg);

				mrR->scaleImage(w, h, 1);
			}

			auto* ormData = new unsigned char[static_cast<size_t>(w) * h * 3];

			for(int y = 0; y < h; y++) {
				for(int x = 0; x < w; x++) {
					osg::Vec4 occTex = occR ? occR->getColor(x, y) : osg::Vec4(1, 1, 1, 1);
					osg::Vec4 mrTex = mrR ? mrR->getColor(x, y) : osg::Vec4(1, 1, 1, 1);
					float ao = 1.0f + strength * (occTex.x() - 1.0f);
					size_t oi = (static_cast<size_t>(y) * w + x) * 3;

					ormData[oi + 0] = static_cast<unsigned char>(osg::clampBetween(ao, 0.0f, 1.0f) * 255.0f + 0.5f);
					ormData[oi + 1] = static_cast<unsigned char>(mrTex.y() * 255.0f + 0.5f);
					ormData[oi + 2] = static_cast<unsigned char>(mrTex.z() * 255.0f + 0.5f);
				}
			}

			outOrm = new osg::Image();
			outOrm->setImage(
				w,
				h,
				1,
				GL_RGB8,
				GL_RGB,
				GL_UNSIGNED_BYTE,
				ormData,
				osg::Image::USE_NEW_DELETE
			);
		}

		static int primitiveMode(int gltfMode) {
			switch(gltfMode) {
				case TINYGLTF_MODE_POINTS: return GL_POINTS;
				case TINYGLTF_MODE_LINE: return GL_LINES;
				case TINYGLTF_MODE_LINE_LOOP: return GL_LINE_LOOP;
				case TINYGLTF_MODE_LINE_STRIP: return GL_LINE_STRIP;
				case TINYGLTF_MODE_TRIANGLES: return GL_TRIANGLES;
				case TINYGLTF_MODE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
				case TINYGLTF_MODE_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
				default: return GL_TRIANGLES;
			}
		}

		// Parameterized on OSG array type + glTF component/accessor types
		// so the compiler can produce a single fast memcpy per combination.
		template<typename OSGArray, int ComponentType, int AccessorType>
		struct ArrayBuilder {
			static OSGArray* make(
				const tinygltf::Buffer& buf,
				const tinygltf::BufferView& bv,
				const tinygltf::Accessor& acc
			) {
				auto* arr = new OSGArray(acc.count);
				int32_t compSize = tinygltf::GetComponentSizeInBytes(ComponentType);
				int32_t numComp = tinygltf::GetNumComponentsInType(AccessorType);
				const auto* src = buf.data.data() + bv.byteOffset + acc.byteOffset;

				if(bv.byteStride == 0) memcpy(&(*arr)[0], src, compSize * numComp * acc.count);

				else {
					for(size_t i = 0; i < acc.count; i++, src += bv.byteStride) memcpy(
						&(*arr)[i],
						src,
						compSize * numComp
					);
				}

				return arr;
			}
		};

		void extractArrays() {
			int accIdx = 0;

			for(auto& acc : model.accessors) {
				GLTF_NOTIFY(1)
					<< "accessor[" << accIdx << "]"
					<< " componentType=" << acc.componentType
					<< " type=" << acc.type
					<< " count=" << acc.count
					<< " bufferView=" << acc.bufferView << std::endl
				;

				// Accessors without a bufferView are valid (e.g. sparse base
				// data is implicitly zero). Push a null placeholder so indices
				// into the arrays vector stay in sync with accessor indices.
				if(
					acc.bufferView < 0 ||
					acc.bufferView >= static_cast<int>(model.bufferViews.size())
				) {
					GLTF_NOTIFY(2) << "-> no bufferView, skipping" << std::endl;

					arrays.push_back({});

					accIdx++;

					continue;
				}

				const auto& bv = model.bufferViews[acc.bufferView];
				const auto& buf = model.buffers[bv.buffer];
				osg::ref_ptr<osg::Array> a;

// TODO: I HATE THIS CODE!
#define MAKE(OsgT, Comp, Type) ArrayBuilder<OsgT, Comp, Type>::make(buf, bv, acc)

				// TODO: I HATE THIS CODE!
				switch(acc.componentType) {
				case TINYGLTF_COMPONENT_TYPE_BYTE:
					switch(acc.type) {
					case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::ByteArray, TINYGLTF_COMPONENT_TYPE_BYTE, TINYGLTF_TYPE_SCALAR); break;
					case TINYGLTF_TYPE_VEC2: a = MAKE(osg::Vec2bArray, TINYGLTF_COMPONENT_TYPE_BYTE, TINYGLTF_TYPE_VEC2); break;
					case TINYGLTF_TYPE_VEC3: a = MAKE(osg::Vec3bArray, TINYGLTF_COMPONENT_TYPE_BYTE, TINYGLTF_TYPE_VEC3); break;
					case TINYGLTF_TYPE_VEC4: a = MAKE(osg::Vec4bArray, TINYGLTF_COMPONENT_TYPE_BYTE, TINYGLTF_TYPE_VEC4); break;
					default: break; } break;

				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
					switch(acc.type) {
					case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::UByteArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, TINYGLTF_TYPE_SCALAR); break;
					case TINYGLTF_TYPE_VEC2: a = MAKE(osg::Vec2ubArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, TINYGLTF_TYPE_VEC2); break;
					case TINYGLTF_TYPE_VEC3: a = MAKE(osg::Vec3ubArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, TINYGLTF_TYPE_VEC3); break;
					case TINYGLTF_TYPE_VEC4: a = MAKE(osg::Vec4ubArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, TINYGLTF_TYPE_VEC4); break;
					default: break; } break;

				case TINYGLTF_COMPONENT_TYPE_SHORT:
					switch(acc.type) {
					case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::ShortArray, TINYGLTF_COMPONENT_TYPE_SHORT, TINYGLTF_TYPE_SCALAR); break;
					case TINYGLTF_TYPE_VEC2: a = MAKE(osg::Vec2sArray, TINYGLTF_COMPONENT_TYPE_SHORT, TINYGLTF_TYPE_VEC2); break;
					case TINYGLTF_TYPE_VEC3: a = MAKE(osg::Vec3sArray, TINYGLTF_COMPONENT_TYPE_SHORT, TINYGLTF_TYPE_VEC3); break;
					case TINYGLTF_TYPE_VEC4: a = MAKE(osg::Vec4sArray, TINYGLTF_COMPONENT_TYPE_SHORT, TINYGLTF_TYPE_VEC4); break;
					default: break; } break;

				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
					switch(acc.type) {
					case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::UShortArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_SCALAR); break;
					case TINYGLTF_TYPE_VEC2: a = MAKE(osg::Vec2usArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_VEC2); break;
					case TINYGLTF_TYPE_VEC3: a = MAKE(osg::Vec3usArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_VEC3); break;
					case TINYGLTF_TYPE_VEC4: a = MAKE(osg::Vec4usArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_VEC4); break;
					default: break; } break;

				case TINYGLTF_COMPONENT_TYPE_INT:
					switch(acc.type) {
					case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::IntArray, TINYGLTF_COMPONENT_TYPE_INT, TINYGLTF_TYPE_SCALAR); break;
					case TINYGLTF_TYPE_VEC2: a = MAKE(osg::Vec2iArray, TINYGLTF_COMPONENT_TYPE_INT, TINYGLTF_TYPE_VEC2); break;
					case TINYGLTF_TYPE_VEC3: a = MAKE(osg::Vec3iArray, TINYGLTF_COMPONENT_TYPE_INT, TINYGLTF_TYPE_VEC3); break;
					case TINYGLTF_TYPE_VEC4: a = MAKE(osg::Vec4iArray, TINYGLTF_COMPONENT_TYPE_INT, TINYGLTF_TYPE_VEC4); break;
					default: break; } break;

				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
					switch(acc.type) {
					case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::UIntArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_SCALAR); break;
					case TINYGLTF_TYPE_VEC2: a = MAKE(osg::Vec2uiArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_VEC2); break;
					case TINYGLTF_TYPE_VEC3: a = MAKE(osg::Vec3uiArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_VEC3); break;
					case TINYGLTF_TYPE_VEC4: a = MAKE(osg::Vec4uiArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_VEC4); break;
					default: break; } break;

				case TINYGLTF_COMPONENT_TYPE_FLOAT:
					switch(acc.type) {
					case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::FloatArray, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_SCALAR); break;
					case TINYGLTF_TYPE_VEC2: a = MAKE(osg::Vec2Array, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2); break;
					case TINYGLTF_TYPE_VEC3: a = MAKE(osg::Vec3Array, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3); break;
					case TINYGLTF_TYPE_VEC4: a = MAKE(osg::Vec4Array, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC4); break;
					case TINYGLTF_TYPE_MAT4: a = MAKE(osg::MatrixfArray, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_MAT4); break;
					default: break; } break;

				default:
					GLTF_NOTIFY(2)
						<< "unknown component type "
						<< acc.componentType << std::endl
					;

					break;
				}

#undef MAKE

				if(a.valid()) {
					a->setBinding(osg::Array::BIND_PER_VERTEX);
					a->setNormalize(acc.normalized);

					GLTF_NOTIFY(2) << "-> built array, " << a->getNumElements() << " element(s)" << std::endl;
				}

				else {
					GLTF_NOTIFY(2) << "-> no array built (unhandled type combination)" << std::endl;
				}

				arrays.push_back(a);

				accIdx++;
			}
		}
	};
};
