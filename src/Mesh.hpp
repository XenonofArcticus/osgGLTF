#pragma once

#include <osg/Array>
#include <osg/Group>
#include <osg/ref_ptr>

#include <vector>

namespace osgDB { class Options; }
namespace tinygltf {
class Mesh;
class Model;
}

namespace osgGLTF::detail {

class MaterialBuilder;
struct Skin;

class MeshBuilder {
public:
	MeshBuilder(
		const tinygltf::Model& model,
		const osgDB::Options* readOptions,
		MaterialBuilder& materialBuilder,
		const std::vector<osg::ref_ptr<osg::Array>>& arrays,
		const std::vector<osg::ref_ptr<Skin>>& skins
	);

	osg::Group* makeMesh(const tinygltf::Mesh& mesh, int skinIndex) const;

private:
	const tinygltf::Model& _model;
	const osgDB::Options* _readOptions;
	MaterialBuilder& _materialBuilder;
	const std::vector<osg::ref_ptr<osg::Array>>& _arrays;
	const std::vector<osg::ref_ptr<Skin>>& _skins;

	static int _primitiveMode(int gltfMode);
};

}
