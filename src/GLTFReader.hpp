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
#include <osg/MatrixTransform>
#include <osg/CullFace>

#include <osgDB/FileNameUtils>
#include <osgDB/Options>
#include <osgDB/ReaderWriter>

#include <osgGLTF/Reader.hpp>

#include <cstddef>
#include <string>
#include <vector>

// tiny_gltf.h is intentionally NOT included here - see file comment above.

#include "Accessor.hpp"
#include "Animation.hpp"
#include "Log.hpp"
#include "Material.hpp"
#include "Mesh.hpp"
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
		osgGLTF::detail::MeshBuilder _meshBuilder;
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
		_progress(p),
		_meshBuilder(m, e._readOptions, _materialBuilder, _arrays, _skins) {
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

			if(node.mesh >= 0) mt->addChild(_meshBuilder.makeMesh(
				_model.meshes[node.mesh],
				node.skin
			));

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

	};
};
