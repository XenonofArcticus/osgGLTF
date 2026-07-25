#include "Skin.hpp"

#include "Log.hpp"

#include <osg/BufferObject>

#include <osgGLTF/Shader.hpp>

#include <algorithm>

namespace osgGLTF::detail {

void Skin::initPalette() {
	paletteMatrices = new osg::MatrixfArray(inverseBindMatrices.size());

	std::fill(
		paletteMatrices->begin(),
		paletteMatrices->end(),
		osg::Matrixf::identity()
	);

	paletteMatrices->setBufferObject(new osg::ShaderStorageBufferObject());
}

osg::Matrixd Skin::computeJointWorld(std::size_t index) {
	if(jointWorldComputed[index]) return jointWorldCache[index];

	osg::ref_ptr<osg::MatrixTransform> joint;

	jointNodes[index].lock(joint);

	osg::Matrixd local = joint ? joint->getMatrix() : osg::Matrixd::identity();
	int parent = parentJointIndex[index];
	osg::Matrixd world;

	if(parent >= 0) {
		world = local * computeJointWorld(static_cast<std::size_t>(parent));
	}
	else {
		osg::MatrixList worlds = joint ? joint->getWorldMatrices() : osg::MatrixList();

		world = worlds.empty() ? local : worlds.front();
	}

	jointWorldCache[index] = world;
	jointWorldComputed[index] = 1;

	return world;
}

bool Skin::updatePalette(osg::Node* skinnedNode) {
	if(!paletteMatrices || !skinnedNode) return false;

	osg::Matrixd meshWorld;
	osg::MatrixList meshWorlds = skinnedNode->getWorldMatrices();

	if(!meshWorlds.empty()) meshWorld = meshWorlds.front();

	osg::Matrixd worldToMesh;

	worldToMesh.invert(meshWorld);

	std::fill(jointWorldComputed.begin(), jointWorldComputed.end(), 0);

	bool anyUpdated = false;

	for(
		std::size_t i = 0;
		i < jointNodes.size() && i < paletteMatrices->size();
		i++
	) {
		if(!jointNodes[i].valid()) continue;

		osg::Matrixd jointWorld = computeJointWorld(i);
		osg::Matrixd inverseBind = inverseBindMatrices[i];
		osg::Matrixd jointMatrix = inverseBind * jointWorld * worldToMesh;

		(*paletteMatrices)[i] = osg::Matrixf(jointMatrix);
		anyUpdated = true;
	}

	if(anyUpdated) paletteMatrices->dirty();

	return anyUpdated;
}

SkinPaletteCallback::SkinPaletteCallback(Skin* skin):
_skin(skin) {}

void SkinPaletteCallback::operator()(osg::Node* node, osg::NodeVisitor* nv) {
	if(_skin.valid() && _skin->updatePalette(node) && !_loggedOnce) {
		_loggedOnce = true;

		GLTF_NOTIFY(1)
			<< "updated skin[" << _skin->index << "] palette "
			<< _skin->paletteMatrices->size()
			<< " matrix/matrices at SSBO binding "
			<< shader::JOINT_MATRICES_SSBO_BINDING << std::endl
		;
	}

	traverse(node, nv);
}

}
