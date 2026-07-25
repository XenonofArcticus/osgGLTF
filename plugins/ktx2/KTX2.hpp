#pragma once

#include <osgx/Warnings.hpp>

OSGX_DISABLE_WARNINGS

#include <osgDB/ReaderWriter>

OSGX_ENABLE_WARNINGS

#include <string>

namespace osg {
class Image;
class TextureCubeMap;
}

namespace osgKTX2 {

osgDB::ReaderWriter::ReadResult read(const std::string& file);

osgDB::ReaderWriter::WriteResult write(
	const osg::TextureCubeMap& texture,
	const std::string& file
);

osgDB::ReaderWriter::WriteResult write(
	const osg::Image& image,
	const std::string& file
);

}
