#pragma once

#include <osgDB/ReaderWriter>

#include <osgGLTF/Reader.hpp>

#include <string>

namespace osgDB { class Options; }

namespace osgGLTF::detail {

class TextureCache;

class ReaderImpl {
public:
	void setTextureCache(TextureCache* textureCache) { _textureCache = textureCache; }

	osgDB::ReaderWriter::ReadResult read(
		const std::string& location,
		bool isBinary,
		const osgDB::Options* readOptions,
		const Reader::ProgressCallback& progress
	) const;

private:
	TextureCache* _textureCache = nullptr;
};

}
