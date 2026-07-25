#pragma once

#include <osgx/Warnings.hpp>

OSGX_DISABLE_WARNINGS

#include <osg/Node>

OSGX_ENABLE_WARNINGS

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
