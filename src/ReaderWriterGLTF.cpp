#include <osgGLTF/Reader.hpp>

#include <osgDB/FileNameUtils>
#include <osgDB/Registry>

class ReaderWriterGLTF: public osgDB::ReaderWriter {
public:
	mutable osgGLTF::Reader::TextureCache _cache;

	ReaderWriterGLTF() {
		supportsExtension("gltf", "glTF 2.0 ASCII");
		supportsExtension("glb", "glTF 2.0 binary");
	}

	const char* className() const override { return "glTF 2.0 plugin"; }

	ReadResult readObject(
		const std::string& location,
		const osgDB::Options* options
	) const override {
		return readNode(location, options);
	}

	ReadResult readNode(
		const std::string& location,
		const osgDB::Options* options
	) const override {
		const std::string ext = osgDB::getLowerCaseFileExtension(location);

		if(!acceptsExtension(ext)) return ReadResult::FILE_NOT_HANDLED;

		osgGLTF::Reader reader;

		reader.setTextureCache(&_cache);

		return reader.read(location, ext == "glb", options);
	}
};

REGISTER_OSGPLUGIN(gltf, ReaderWriterGLTF)
