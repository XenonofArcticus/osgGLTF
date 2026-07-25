#define TINYGLTF_NOEXCEPTION

#include "tiny_gltf.h"
#include "GLTFReader.hpp"

#include <osgGLTF/Reader.hpp>

#include <utility>

namespace osgGLTF {

struct Reader::TextureCache::Implementation {
	GLTFReader::TextureCache cache;
};

Reader::TextureCache::TextureCache():
_implementation(std::make_unique<Implementation>()) {}

Reader::TextureCache::~TextureCache() = default;
Reader::TextureCache::TextureCache(TextureCache&&) noexcept = default;
Reader::TextureCache& Reader::TextureCache::operator=(TextureCache&&) noexcept = default;

osgDB::ReaderWriter::ReadResult Reader::read(
	const std::string& location,
	bool isBinary,
	const osgDB::Options* options,
	const ProgressCallback& progress
) const {
	GLTFReader reader;

	if(_textureCache) reader.setTextureCache(&_textureCache->_implementation->cache);

	return reader.read(location, isBinary, options, progress);
}

}
