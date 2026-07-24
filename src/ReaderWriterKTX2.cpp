#include <osg/GL>
#include <osg/Image>
#include <osg/TextureCubeMap>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>

#include <ktx.h>

// GL constants not always defined in older OSG GL headers
#ifndef GL_HALF_FLOAT
# define GL_HALF_FLOAT 0x140B
#endif
#ifndef GL_RGB16F
# define GL_RGB16F 0x881B
#endif
#ifndef GL_RGBA16F
# define GL_RGBA16F 0x881A
#endif
#ifndef GL_RGB32F
# define GL_RGB32F 0x8815
#endif
#ifndef GL_RGBA32F
# define GL_RGBA32F 0x8814
#endif
#ifndef GL_SRGB8_ALPHA8
# define GL_SRGB8_ALPHA8 0x8C43
#endif
// BC/BPTC compressed formats (GL 4.2 core)
#ifndef GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT
# define GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT 0x8E8F
#endif
#ifndef GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT
# define GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT 0x8E8E
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
# define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif

// Vulkan VkFormat constants used by KTX2.
// Some builds of libktx / OSG may expose only the numeric vkFormat field
// without pulling in Vulkan headers.
#ifndef VK_FORMAT_R8G8B8_UNORM
# define VK_FORMAT_R8G8B8_UNORM 23
#endif

#ifndef VK_FORMAT_R8G8B8A8_UNORM
# define VK_FORMAT_R8G8B8A8_UNORM 37
#endif

#ifndef VK_FORMAT_R8G8B8A8_SRGB
# define VK_FORMAT_R8G8B8A8_SRGB 43
#endif

#ifndef VK_FORMAT_R16G16B16_SFLOAT
# define VK_FORMAT_R16G16B16_SFLOAT 90
#endif

#ifndef VK_FORMAT_R16G16B16A16_SFLOAT
# define VK_FORMAT_R16G16B16A16_SFLOAT 97
#endif

#ifndef VK_FORMAT_R32G32B32_SFLOAT
# define VK_FORMAT_R32G32B32_SFLOAT 106
#endif

#ifndef VK_FORMAT_R32G32B32A32_SFLOAT
# define VK_FORMAT_R32G32B32A32_SFLOAT 109
#endif

#ifndef VK_FORMAT_BC6H_UFLOAT_BLOCK
# define VK_FORMAT_BC6H_UFLOAT_BLOCK 143
#endif

#ifndef VK_FORMAT_BC6H_SFLOAT_BLOCK
# define VK_FORMAT_BC6H_SFLOAT_BLOCK 144
#endif

#ifndef VK_FORMAT_BC7_UNORM_BLOCK
# define VK_FORMAT_BC7_UNORM_BLOCK 145
#endif

#ifndef VK_FORMAT_BC7_SRGB_BLOCK
# define VK_FORMAT_BC7_SRGB_BLOCK 146
#endif

// TODO: Should we just use this instead?
// #include <vulkan/vulkan_core.h>

// ---------------------------------------------------------------------------
// VkFormat <-> GL mappings
// ---------------------------------------------------------------------------

struct GLFormat { GLenum internal, pixel, type; bool compressed; };

static bool vkFormatToGL(ktx_uint32_t vk, GLFormat& out) {
	switch (vk) {
		// 8-bit unorm
		case VK_FORMAT_R8G8B8_UNORM: out = {GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, false}; return true;
		case VK_FORMAT_R8G8B8A8_UNORM: out = {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false}; return true;
		case VK_FORMAT_R8G8B8A8_SRGB: out = {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, false}; return true;
		// 16-bit float <- primary IBL format
		case VK_FORMAT_R16G16B16_SFLOAT: out = {GL_RGB16F, GL_RGB, GL_HALF_FLOAT, false}; return true;
		case VK_FORMAT_R16G16B16A16_SFLOAT: out = {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, false}; return true;
		// 32-bit float
		case VK_FORMAT_R32G32B32_SFLOAT: out = {GL_RGB32F, GL_RGB, GL_FLOAT, false}; return true;
		case VK_FORMAT_R32G32B32A32_SFLOAT: out = {GL_RGBA32F, GL_RGBA, GL_FLOAT, false}; return true;
		// BC6H / BC7 (GPU-compressed HDR / LDR env maps)
		case VK_FORMAT_BC6H_UFLOAT_BLOCK: out = {GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT, GL_RGB, 0, true}; return true;
		case VK_FORMAT_BC6H_SFLOAT_BLOCK: out = {GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT, GL_RGB, 0, true}; return true;
		case VK_FORMAT_BC7_UNORM_BLOCK: out = {GL_COMPRESSED_RGBA_BPTC_UNORM, GL_RGBA, 0, true}; return true;
		default: return false;
	}
}

// GL internal format -> VkFormat (write path)
static bool glToVkFormat(GLenum internal, ktx_uint32_t& out) {
	switch (internal) {
		case GL_RGB8: out = VK_FORMAT_R8G8B8_UNORM; return true;
		case GL_RGBA8: out = VK_FORMAT_R8G8B8A8_UNORM; return true;
		case GL_RGB16F: out = VK_FORMAT_R16G16B16_SFLOAT; return true;
		case GL_RGBA16F: out = VK_FORMAT_R16G16B16A16_SFLOAT; return true;
		case GL_RGB32F: out = VK_FORMAT_R32G32B32_SFLOAT; return true;
		case GL_RGBA32F: out = VK_FORMAT_R32G32B32A32_SFLOAT; return true;
		default: return false;
	}
}

// ---------------------------------------------------------------------------
// Build a single osg::Image for one face (mip 0 ... numLevels-1)
// ---------------------------------------------------------------------------

static osg::ref_ptr<osg::Image> buildFaceImage(
	ktxTexture2* ktx,
	uint32_t face,
	const GLFormat& fmt
) {
	const uint32_t numLevels = ktx->numLevels;
	const uint8_t* src = ktxTexture_GetData(reinterpret_cast<ktxTexture*>(ktx));

	// Total buffer: sum of this face's footprint across all mip levels
	size_t totalSize = 0;

	for(uint32_t mip = 0; mip < numLevels; ++mip) {
		totalSize += ktxTexture_GetImageSize(reinterpret_cast<ktxTexture*>(ktx), mip);
	}

	unsigned char* buf = new unsigned char[totalSize];
	osg::Image::MipmapDataType mipmapOffsets;
	size_t dstOff = 0;

	for(uint32_t mip = 0; mip < numLevels; ++mip) {
		ktx_size_t srcOff = 0;

		ktxTexture_GetImageOffset(reinterpret_cast<ktxTexture*>(ktx), mip, 0, face, &srcOff);

		size_t mipSize = ktxTexture_GetImageSize(reinterpret_cast<ktxTexture*>(ktx), mip);

		memcpy(buf + dstOff, src + srcOff, mipSize);

		if(mip > 0) mipmapOffsets.push_back(static_cast<unsigned int>(dstOff));

		dstOff += mipSize;
	}

	int w = static_cast<int>(std::max(1u, ktx->baseWidth));
	int h = static_cast<int>(std::max(1u, ktx->baseHeight));

	osg::ref_ptr<osg::Image> img = new osg::Image();

	img->setImage(
		w,
		h,
		1,
		static_cast<GLint>(fmt.internal),
		fmt.pixel,
		fmt.type,
		buf,
		osg::Image::USE_NEW_DELETE
	);

	if(!mipmapOffsets.empty()) img->setMipmapLevels(mipmapOffsets);

	return img;
}

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

class ReaderWriterKTX2: public osgDB::ReaderWriter {
	static ReadResult loadKTX2(const std::string& file) {
		ktxTexture2* ktx = nullptr;

		KTX_error_code rc = ktxTexture2_CreateFromNamedFile(
			file.c_str(),
			KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
			&ktx
		);

		if(rc != KTX_SUCCESS) {
			OSG_WARN
				<< "ReaderWriterKTX2: failed to open " << file
				<< ": " << ktxErrorString(rc) << std::endl
			;

			return ReadResult::ERROR_IN_READING_FILE;
		}

		struct Guard {
			ktxTexture2* p;
			~Guard() { ktxTexture_Destroy(reinterpret_cast<ktxTexture*>(p)); }
		} g{ktx};

		// Transcode Basis Universal (ETC1S / UASTC) to RGBA8.
		// Note: HDR cubemaps should never be Basis-encoded; this is a safety net.
		if(ktxTexture2_NeedsTranscoding(ktx)) {
			OSG_WARN
				<< "ReaderWriterKTX2: " << file
				<< " is Basis-encoded; transcoding to RGBA8 (HDR range will be clamped)"
				<< std::endl
			;

			rc = ktxTexture2_TranscodeBasis(ktx, KTX_TTF_RGBA32, 0);

			if(rc != KTX_SUCCESS) {
				OSG_WARN
					<< "ReaderWriterKTX2: TranscodeBasis failed: "
					<< ktxErrorString(rc) << std::endl
				;

				return ReadResult::ERROR_IN_READING_FILE;
			}
		}

		GLFormat fmt;

		if(!vkFormatToGL(ktx->vkFormat, fmt)) {
			OSG_WARN
				<< "ReaderWriterKTX2: unsupported VkFormat " << ktx->vkFormat
				<< " in " << file << std::endl
			;

			return ReadResult::FILE_NOT_HANDLED;
		}

		if(fmt.compressed) {
			OSG_WARN
				<< "ReaderWriterKTX2: BC-compressed KTX2 not yet supported: "
				<< file << std::endl
			;

			return ReadResult::FILE_NOT_HANDLED;
		}

		const uint32_t numFaces = ktx->numFaces;

		if(numFaces == 6) {
			osg::ref_ptr<osg::TextureCubeMap> texcm = new osg::TextureCubeMap();

			for(uint32_t f = 0; f < 6; ++f) {
				auto img = buildFaceImage(ktx, f, fmt);

				if(!img) return ReadResult::ERROR_IN_READING_FILE;

				texcm->setImage(f, img.get());
			}

			texcm->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
			texcm->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
			texcm->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
			texcm->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
			texcm->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);

			return texcm.get();
		}

		// 2D / BRDF LUT / equirect
		auto img = buildFaceImage(ktx, 0, fmt);

		if(!img) return ReadResult::ERROR_IN_READING_FILE;

		img->setFileName(file);

		return img.get();
	}

	// getMipmapData(N) = _data + getMipmapOffset(N); getMipmapOffset(0) = 0, so N=0 -> _data.
	static const unsigned char* getMipPtr(const osg::Image* img, uint32_t mip) {
		return img->getMipmapData(mip);
	}

	static WriteResult saveCubeMap(const osg::TextureCubeMap* texcm, const std::string& file) {
		const osg::Image* face0 = texcm->getImage(0);

		if(!face0 || !face0->data()) {
			OSG_WARN << "ReaderWriterKTX2: write: face 0 has no CPU image data" << std::endl;

			return WriteResult::ERROR_IN_WRITING_FILE;
		}

		ktx_uint32_t vkFmt;

		if(!glToVkFormat(face0->getInternalTextureFormat(), vkFmt)) {
			OSG_WARN
				<< "ReaderWriterKTX2: write: unsupported GL internal format "
				<< face0->getInternalTextureFormat() << std::endl
			;

			return WriteResult::ERROR_IN_WRITING_FILE;
		}

		uint32_t numMips = std::max(1u, static_cast<uint32_t>(face0->getNumMipmapLevels()));

		ktxTextureCreateInfo ci = {};

		ci.vkFormat = vkFmt;
		ci.baseWidth = static_cast<uint32_t>(face0->s());
		ci.baseHeight = static_cast<uint32_t>(face0->t());
		ci.baseDepth = 1;
		ci.numDimensions = 2;
		ci.numLevels = numMips;
		ci.numLayers = 1;
		ci.numFaces = 6;
		ci.isArray = KTX_FALSE;
		ci.generateMipmaps = KTX_FALSE;

		ktxTexture2* ktx = nullptr;
		KTX_error_code rc = ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktx);

		if(rc != KTX_SUCCESS) {
			OSG_WARN << "ReaderWriterKTX2: ktxTexture2_Create: " << ktxErrorString(rc) << std::endl;

			return WriteResult::ERROR_IN_WRITING_FILE;
		}

		struct Guard {
			ktxTexture2* p;
			~Guard() { ktxTexture_Destroy(reinterpret_cast<ktxTexture*>(p)); }
		} g{ktx};

		for(uint32_t face = 0; face < 6; ++face) {
			const osg::Image* img = texcm->getImage(face);

			if(!img || !img->data()) {
				OSG_WARN
					<< "ReaderWriterKTX2: write: face " << face
					<< " has no CPU data" << std::endl
				;

				return WriteResult::ERROR_IN_WRITING_FILE;
			}

			for(uint32_t mip = 0; mip < numMips; ++mip) {
				ktx_size_t mipBytes = ktxTexture_GetImageSize(reinterpret_cast<ktxTexture*>(ktx), mip);

				rc = ktxTexture_SetImageFromMemory(
					reinterpret_cast<ktxTexture*>(ktx),
					mip,
					0,
					face,
					getMipPtr(img, mip),
					mipBytes
				);

				if(rc != KTX_SUCCESS) {
					OSG_WARN
						<< "ReaderWriterKTX2: SetImageFromMemory mip=" << mip
						<< " face=" << face << ": " << ktxErrorString(rc) << std::endl
					;

					return WriteResult::ERROR_IN_WRITING_FILE;
				}
			}
		}

		rc = ktxTexture_WriteToNamedFile(reinterpret_cast<ktxTexture*>(ktx), file.c_str());

		if(rc != KTX_SUCCESS) {
			OSG_WARN << "ReaderWriterKTX2: WriteToNamedFile: " << ktxErrorString(rc) << std::endl;

			return WriteResult::ERROR_IN_WRITING_FILE;
		}

		OSG_NOTICE << "ReaderWriterKTX2: wrote cubemap KTX2 " << file << std::endl;

		return WriteResult::FILE_SAVED;
	}

	static WriteResult save2DImage(const osg::Image* img, const std::string& file) {
		if(!img->data()) {
			OSG_WARN << "ReaderWriterKTX2: write: image has no CPU data" << std::endl;

			return WriteResult::ERROR_IN_WRITING_FILE;
		}

		ktx_uint32_t vkFmt;

		if(!glToVkFormat(img->getInternalTextureFormat(), vkFmt)) {
			OSG_WARN
				<< "ReaderWriterKTX2: write: unsupported GL internal format "
				<< img->getInternalTextureFormat() << std::endl
			;

			return WriteResult::ERROR_IN_WRITING_FILE;
		}

		uint32_t numMips = std::max(1u, static_cast<uint32_t>(img->getNumMipmapLevels()));

		ktxTextureCreateInfo ci = {};

		ci.vkFormat = vkFmt;
		ci.baseWidth = static_cast<uint32_t>(img->s());
		ci.baseHeight = static_cast<uint32_t>(img->t());
		ci.baseDepth = 1;
		ci.numDimensions = 2;
		ci.numLevels = numMips;
		ci.numLayers = 1;
		ci.numFaces = 1;
		ci.isArray = KTX_FALSE;
		ci.generateMipmaps = KTX_FALSE;

		ktxTexture2* ktx = nullptr;
		KTX_error_code rc = ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktx);

		if(rc != KTX_SUCCESS) {
			OSG_WARN << "ReaderWriterKTX2: ktxTexture2_Create: " << ktxErrorString(rc) << std::endl;

			return WriteResult::ERROR_IN_WRITING_FILE;
		}

		struct Guard {
			ktxTexture2* p;
			~Guard() { ktxTexture_Destroy(reinterpret_cast<ktxTexture*>(p)); }
		} g{ktx};

		for(uint32_t mip = 0; mip < numMips; ++mip) {
			ktx_size_t mipBytes = ktxTexture_GetImageSize(reinterpret_cast<ktxTexture*>(ktx), mip);
			rc = ktxTexture_SetImageFromMemory(
				reinterpret_cast<ktxTexture*>(ktx),
				mip,
				0,
				0,
				getMipPtr(img, mip),
				mipBytes
			);

			if(rc != KTX_SUCCESS) {
				OSG_WARN
					<< "ReaderWriterKTX2: SetImageFromMemory mip=" << mip
					<< ": " << ktxErrorString(rc) << std::endl
				;

				return WriteResult::ERROR_IN_WRITING_FILE;
			}
		}

		rc = ktxTexture_WriteToNamedFile(reinterpret_cast<ktxTexture*>(ktx), file.c_str());

		if(rc != KTX_SUCCESS) {
			OSG_WARN << "ReaderWriterKTX2: WriteToNamedFile: " << ktxErrorString(rc) << std::endl;

			return WriteResult::ERROR_IN_WRITING_FILE;
		}

		OSG_NOTICE << "ReaderWriterKTX2: wrote 2D KTX2 " << file << std::endl;

		return WriteResult::FILE_SAVED;
	}

public:
	ReaderWriterKTX2() { supportsExtension("ktx2", "KTX 2.0 texture"); }

	const char* className() const override { return "KTX2 Texture Reader/Writer"; }

	ReadResult readObject(const std::string& file, const osgDB::Options* options) const override {
		if(!acceptsExtension(osgDB::getLowerCaseFileExtension(file)))
			return ReadResult::FILE_NOT_HANDLED;

		const std::string found = osgDB::findDataFile(file, options);

		if(found.empty()) return ReadResult::FILE_NOT_FOUND;

		return loadKTX2(found);
	}

	ReadResult readImage(const std::string& file, const osgDB::Options* options) const override {
		ReadResult rr = readObject(file, options);

		if(!rr.success()) return rr;
		if(rr.validImage()) return rr;

		OSG_WARN
			<< "ReaderWriterKTX2: readImage() called on a cubemap KTX2 -- use readObject() instead"
			<< std::endl
		;

		return ReadResult::FILE_NOT_HANDLED;
	}

	WriteResult writeObject(
		const osg::Object& obj,
		const std::string& file,
		const osgDB::Options*
	) const override {
		if(!acceptsExtension(osgDB::getLowerCaseFileExtension(file)))
			return WriteResult::FILE_NOT_HANDLED;

		if(const auto* texcm = dynamic_cast<const osg::TextureCubeMap*>(&obj))
			return saveCubeMap(texcm, file);

		if(const auto* img = dynamic_cast<const osg::Image*>(&obj))
			return save2DImage(img, file);

		return WriteResult::FILE_NOT_HANDLED;
	}

	WriteResult writeImage(
		const osg::Image& img,
		const std::string& file,
		const osgDB::Options*
	) const override {
		if(!acceptsExtension(osgDB::getLowerCaseFileExtension(file)))
			return WriteResult::FILE_NOT_HANDLED;

		return save2DImage(&img, file);
	}
};

REGISTER_OSGPLUGIN(ktx2, ReaderWriterKTX2)
