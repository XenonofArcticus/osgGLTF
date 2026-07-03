#pragma once

#include <osg/BufferIndexBinding>
#include <osg/BufferObject>
#include <osg/Callback>
#include <osg/MatrixTransform>
#include <osg/Transform>

#include <algorithm>
#include <string>
#include <vector>

static constexpr GLuint GLTF_JOINT_MATRICES_BINDING = 2;

struct GLTFSkin: public osg::Referenced {
	int index = -1;
	std::string name;
	std::vector<int> joints;
	std::vector<osg::Matrixf> inverseBindMatrices;
	std::vector<osg::observer_ptr<osg::MatrixTransform>> jointNodes;
	std::vector<osg::observer_ptr<osg::MatrixTransform>> skinnedNodes;
	int skeleton = -1;
	osg::ref_ptr<osg::MatrixfArray> paletteMatrices;

	void initPalette() {
		paletteMatrices = new osg::MatrixfArray(inverseBindMatrices.size());

		std::fill(
			paletteMatrices->begin(),
			paletteMatrices->end(),
			osg::Matrixf::identity()
		);

		paletteMatrices->setBufferObject(new osg::ShaderStorageBufferObject());
	}

	bool updatePalette(osg::Node* skinnedNode) {
		if(!paletteMatrices || !skinnedNode) return false;

		osg::Matrixd meshWorld;
		osg::MatrixList meshWorlds = skinnedNode->getWorldMatrices();

		if(!meshWorlds.empty()) meshWorld = meshWorlds.front();

		osg::Matrixd worldToMesh;

		worldToMesh.invert(meshWorld);

		bool anyUpdated = false;

		for(size_t i = 0; i < jointNodes.size() && i < paletteMatrices->size(); ++i) {
			osg::MatrixTransform* joint = jointNodes[i].get();

			if(!joint) continue;

			osg::MatrixList jointWorlds = joint->getWorldMatrices();

			if(jointWorlds.empty()) continue;

			osg::Matrixd jointWorld = jointWorlds.front();
			osg::Matrixd inverseBind = inverseBindMatrices[i];
			osg::Matrixd jointMatrix = inverseBind * jointWorld * worldToMesh;

			(*paletteMatrices)[i] = osg::Matrixf(jointMatrix);
			anyUpdated = true;
		}

		if(anyUpdated) paletteMatrices->dirty();

		return anyUpdated;
	}
};

class GLTFSkinPaletteCallback: public osg::NodeCallback {
public:
	GLTFSkinPaletteCallback(GLTFSkin* skin):
	_skin(skin) {}

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override {
		if(_skin.valid() && _skin->updatePalette(node) && !_loggedOnce) {
			_loggedOnce = true;

			GLTF_NOTIFY(1)
				<< "updated skin[" << _skin->index << "] palette "
				<< _skin->paletteMatrices->size()
				<< " matrix/matrices at SSBO binding "
				<< GLTF_JOINT_MATRICES_BINDING << std::endl
			;
		}

		traverse(node, nv);
	}

private:
	osg::ref_ptr<GLTFSkin> _skin;
	bool _loggedOnce = false;
};
