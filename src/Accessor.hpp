#pragma once

#include <osg/Array>
#include <osg/ref_ptr>

#include <vector>

namespace tinygltf { class Model; }

namespace osgGLTF::detail {

std::vector<osg::ref_ptr<osg::Array>> extractArrays(const tinygltf::Model& model);

}
