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

#include <osgDB/FileNameUtils>
#include <osgDB/Options>
#include <osgDB/ReaderWriter>

#include <osgGLTF/Reader.hpp>

#include <cstddef>
#include <string>

// tiny_gltf.h is intentionally NOT included here - see file comment above.

#include "Log.hpp"
#include "Scene.hpp"
#include "Texture.hpp"

class GLTFReader {
public:
	using Stage = osgGLTF::Reader::Stage;
	using ProgressCallback = osgGLTF::Reader::ProgressCallback;

	using TextureCache = osgGLTF::detail::TextureCache;

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

		return osgGLTF::detail::buildScene(
			model,
			location,
			readOptions,
			_texCache,
			progress
		);
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

};
