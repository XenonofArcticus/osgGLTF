#pragma once

#include <osg/Node>

#include <osgGLTF/Reader.hpp>

#include <string>

namespace osgDB { class Options; }
namespace tinygltf { class Model; }

namespace osgGLTF::detail {

class TextureCache;

osg::Node* buildScene(
	const tinygltf::Model& model,
	const std::string& referrer,
	const osgDB::Options* readOptions,
	TextureCache* textureCache,
	const Reader::ProgressCallback& progress
);

}
