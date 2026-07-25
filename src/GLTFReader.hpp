// GLTFReader.h - standalone OSG glTF 2.0 reader, no osgEarth dependency.
// Derived from osgEarth's GLTFReader (Pelican Mapping, LGPL 2+).
//
// Stripped: URI class, InstanceBuilder, StateTransition, shaderGenerator,
// DiscardAlphaFragments, Mutexed<UnorderedMap>, OWT_state extension.
//
// Replaced: osgEarth::URI image loading -> osgDB image loading,
// osgEarth logging macros -> OSG_WARN/GLTF_NOTIFY.

// Private implementation detail: TinyGLTF.cpp instantiates tinygltf/STB. Reader.cpp includes the
// tinygltf declarations before this file. Consumers use osgGLTF/Reader.hpp and never include this
// implementation.

#pragma once

#include <osg/Node>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/CullFace>

#include <osgUtil/SmoothingVisitor>

#include <osgDB/FileNameUtils>
#include <osgDB/Options>
#include <osgDB/ReaderWriter>

#include <osgGLTF/Shader.hpp>
#include <osgGLTF/Reader.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <string>
#include <typeinfo>
#include <vector>

// tiny_gltf.h is intentionally NOT included here - see file comment above.

#include "Accessor.hpp"
#include "Animation.hpp"
#include "Log.hpp"
#include "Material.hpp"
#include "Skin.hpp"
#include "Texture.hpp"

class GLTFReader {
public:
	using Stage = osgGLTF::Reader::Stage;
	using ProgressCallback = osgGLTF::Reader::ProgressCallback;

	using TextureCache = osgGLTF::detail::TextureCache;

	struct Env {
		Env(const std::string& loc, const osgDB::Options* opt):
		_referrer(loc),
		_readOptions(opt) {}

		std::string _referrer;
		const osgDB::Options* _readOptions;
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
	// Not const-qualified since _imagesLoaded is ticked from inside the callback.
	struct ImageLoadContext {
		const ProgressCallback* _progress;
		size_t _imagesLoaded = 0;
		size_t _totalImages = 0;
	};

	// No-op image loader used only for the metadata-only pre-pass below - skips decode
	// entirely, just lets tinygltf finish parsing the JSON model (including model.images,
	// which is populated from the JSON regardless of whether pixels get decoded).
	static bool _skipImageLoad(
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
	static bool _realImageLoader(
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

		if(ctx && ctx->_progress && *ctx->_progress) {
			ctx->_imagesLoaded++;
			(*ctx->_progress)(Stage::LoadingTextures, ctx->_imagesLoaded, ctx->_totalImages);
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
			counter.SetImageLoader(&_skipImageLoad, nullptr);

			bool countOk = isBinary
				? counter.LoadBinaryFromFile(&countModel, &countErr, &countWarn, location)
				: counter.LoadASCIIFromFile (&countModel, &countErr, &countWarn, location)
			;

			imageLoadContext._totalImages = countOk ? countModel.images.size() : 0;
		}

		if(progress) progress(Stage::Parsing, 1, 1);

		imageLoadContext._progress = &progress;
		imageLoadContext._imagesLoaded = 0;

		if(progress) progress(Stage::LoadingTextures, 0, imageLoadContext._totalImages);

		loader.SetImageLoader(&_realImageLoader, &imageLoadContext);

		// tinygltf's own file/buffer decode is one opaque blocking call, but image decode
	// (the real bottleneck for texture-heavy assets) is now hooked via _realImageLoader
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
			env._readOptions &&
			env._readOptions->getOptionString().find("gltfZUp") != std::string::npos
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
		const tinygltf::Model& _model;
		const Env& _env;
		osgGLTF::detail::TextureLoader _textureLoader;
		osgGLTF::detail::MaterialBuilder _materialBuilder;
		ProgressCallback _progress;
		std::vector<osg::ref_ptr<osg::Array>> _arrays;
		std::vector<osg::ref_ptr<osgGLTF::detail::Skin>> _skins;
		mutable std::vector<osg::observer_ptr<osg::MatrixTransform>> _nodeTransforms;
		mutable size_t _nodesBuilt = 0;

		NodeBuilder(
			const GLTFReader* r,
			const tinygltf::Model& m,
			const Env& e,
			const ProgressCallback& p=nullptr
		):
		_model(m),
		_env(e),
		_textureLoader(m, e._referrer, e._readOptions, r->_texCache),
		_materialBuilder(m, e._referrer, e._readOptions, _textureLoader),
		_progress(p) {
			_nodeTransforms.resize(m.nodes.size());

			_arrays = osgGLTF::detail::extractArrays(_model);

			_skins = osgGLTF::detail::prepareSkins(_model, _arrays);
		}

		osg::Node* createNode(int nodeIdx, unsigned depth = 0) const {
			if(nodeIdx < 0 || nodeIdx >= static_cast<int>(_model.nodes.size())) return nullptr;

			const tinygltf::Node& node = _model.nodes[nodeIdx];

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
			_nodesBuilt++;

			if(_progress) _progress(Stage::BuildingNodes, _nodesBuilt, _model.nodes.size());

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

			_nodeTransforms[nodeIdx] = mt;

			if(
				node.skin >= 0 &&
				node.skin < static_cast<int>(_skins.size()) &&
				_skins[node.skin].valid()
			) _skins[node.skin]->skinnedNodes.push_back(mt);

			if(node.mesh >= 0) mt->addChild(makeMesh(_model.meshes[node.mesh], node.skin));

			for(int childIdx : node.children) {
				if(osg::Node* c = createNode(childIdx, depth + 1)) mt->addChild(c);
			}

			mt->setName(node.name);

			return mt;
		}

		void resolveSkinJointNodes() {
			osgGLTF::detail::resolveSkinJointNodes(_model, _nodeTransforms, _skins);
		}

		void installSkinPaletteCallbacks() {
			osgGLTF::detail::installSkinPaletteCallbacks(_skins);
		}

		void installAnimationCallback(osg::Node* root) const {
			const bool skipAnimation =
				_env._readOptions &&
				_env._readOptions->getOptionString().find("gltfSkipAnimation") != std::string::npos
			;

			osgGLTF::detail::installAnimationCallback(
				_model,
				_arrays,
				_nodeTransforms,
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
						accessorIdx < static_cast<int>(_arrays.size()) &&
						_arrays[accessorIdx].valid()
					;

					GLTF_NOTIFY(4)
						<< "" << attrName
						<< " -> accessor[" << accessorIdx << "]"
						<< (valid ? " OK" : " NULL/INVALID") << std::endl
					;

					if(!valid) continue;

					if(attrName == "POSITION") geom->setVertexArray(_arrays[accessorIdx].get());
					else if(attrName == "NORMAL") geom->setNormalArray(_arrays[accessorIdx].get());
					else if(attrName == "COLOR_0") geom->setColorArray(_arrays[accessorIdx].get());
					else if(attrName == "TANGENT") {
						_arrays[accessorIdx]->setBinding(osg::Array::BIND_PER_VERTEX);

						geom->setVertexAttribArray(
							osgGLTF::shader::TANGENT_ATTRIBUTE,
							_arrays[accessorIdx].get()
						);
					}
					else if(attrName.rfind("TEXCOORD_", 0) == 0) {
						int uvSet = std::atoi(attrName.c_str() + 9);

						texCoordSets[uvSet] = _arrays[accessorIdx].get();
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

					if(skinIdx >= 0 && skinIdx < static_cast<int>(_skins.size())) {
						if(jointsAccessor >= 0) {
							_arrays[jointsAccessor]->setBinding(osg::Array::BIND_PER_VERTEX);
							_arrays[jointsAccessor]->setPreserveDataType(true);
							geom->setVertexAttribArray(
								osgGLTF::shader::JOINT_INDICES_ATTRIBUTE,
								_arrays[jointsAccessor].get()
							);
						}

						if(weightsAccessor >= 0) {
							_arrays[weightsAccessor]->setBinding(osg::Array::BIND_PER_VERTEX);
							geom->setVertexAttribArray(
								osgGLTF::shader::JOINT_WEIGHTS_ATTRIBUTE,
								_arrays[weightsAccessor].get()
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
					primitive.material < static_cast<int>(_model.materials.size())
				) {
					GLTF_NOTIFY(3) << "applyMaterial " << primitive.material << std::endl;

					_materialBuilder.applyMaterial(
						primitive.material,
						baseColorFactor,
						geom,
						texCoordSets
					);
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
					primitive.indices < static_cast<int>(_arrays.size()) &&
					_arrays[primitive.indices].valid()
				) {
					int glMode = primitiveMode(primitive.mode);
					const tinygltf::Accessor& idxAcc = _model.accessors[primitive.indices];

					switch(idxAcc.componentType) {
						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
							auto* src = static_cast<osg::UByteArray*>(_arrays[primitive.indices].get());
							auto* de = new osg::DrawElementsUByte(glMode, idxAcc.count);

							std::copy(src->begin(), src->end(), de->begin());

							geom->addPrimitiveSet(de);

							break;
						}

						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
							auto* src = static_cast<osg::UShortArray*>(_arrays[primitive.indices].get());

							geom->addPrimitiveSet(new osg::DrawElementsUShort(glMode, src->begin(), src->end()));

							break;
						}

						case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
							auto* src = static_cast<osg::UIntArray*>(_arrays[primitive.indices].get());

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
					_env._readOptions &&
					_env._readOptions->getOptionString().find("gltfSkipNormals") != std::string::npos
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

	};
};
