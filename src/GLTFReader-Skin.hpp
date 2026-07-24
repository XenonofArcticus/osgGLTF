#pragma once

#include <osg/BufferIndexBinding>
#include <osg/BufferObject>
#include <osg/Callback>
#include <osg/MatrixTransform>
#include <osg/Transform>

#include <osgGLTF/Shader.hpp>

#include <algorithm>
#include <string>
#include <vector>

struct GLTFSkin: public osg::Referenced {
	int index = -1;
	std::string name;
	std::vector<int> joints;
	std::vector<osg::Matrixf> inverseBindMatrices;
	std::vector<osg::observer_ptr<osg::MatrixTransform>> jointNodes;
	std::vector<osg::observer_ptr<osg::MatrixTransform>> skinnedNodes;
	int skeleton = -1;
	osg::ref_ptr<osg::MatrixfArray> paletteMatrices;

	// Index (into `joints`/`jointNodes`) of each joint's parent joint WITHIN THIS SKIN, or -1 if
	// its parent lies outside the skin's own joint set -- a "root" joint for this skin, normally
	// just one or a small few, never the whole joint count. Computed once at load time in
	// GLTFReader::NodeBuilder::resolveSkinJointNodes() from the glTF node hierarchy.
	std::vector<int> parentJointIndex;

	// Scratch buffers reused every updatePalette() call to avoid per-frame allocation.
	std::vector<osg::Matrixd> jointWorldCache;
	std::vector<char> jointWorldComputed;

	void initPalette() {
		paletteMatrices = new osg::MatrixfArray(inverseBindMatrices.size());

		std::fill(
			paletteMatrices->begin(),
			paletteMatrices->end(),
			osg::Matrixf::identity()
		);

		paletteMatrices->setBufferObject(new osg::ShaderStorageBufferObject());
	}

	// Computes joint `i`'s world matrix by walking ONLY its ancestor chain within this skin's own
	// joints, memoized so each joint's world is computed at most once per updatePalette() call.
	// Falls back to the real (expensive) Node::getWorldMatrices() only for joints whose parent
	// lies outside the skin -- the skin's root joint(s), typically just one. This is what replaces
	// calling getWorldMatrices() once PER JOINT: that call re-walks the full NodePathList from the
	// node to the scene root every time, so doing it per-joint per-frame was
	// O(joints x depth-to-root) with heap churn on every call -- the actual performance bug.
	osg::Matrixd computeJointWorld(size_t i) {
		if(jointWorldComputed[i]) return jointWorldCache[i];

		osg::MatrixTransform* joint = jointNodes[i].get();
		osg::Matrixd local = joint ? joint->getMatrix() : osg::Matrixd::identity();
		int parent = parentJointIndex[i];
		osg::Matrixd world;

		if(parent >= 0) world = local * computeJointWorld(static_cast<size_t>(parent));

		else {
			osg::MatrixList worlds = joint ? joint->getWorldMatrices() : osg::MatrixList();

			world = worlds.empty() ? local : worlds.front();
		}

		jointWorldCache[i] = world;
		jointWorldComputed[i] = 1;

		return world;
	}

	bool updatePalette(osg::Node* skinnedNode) {
		if(!paletteMatrices || !skinnedNode) return false;

		osg::Matrixd meshWorld;
		osg::MatrixList meshWorlds = skinnedNode->getWorldMatrices();

		if(!meshWorlds.empty()) meshWorld = meshWorlds.front();

		osg::Matrixd worldToMesh;

		worldToMesh.invert(meshWorld);

		std::fill(jointWorldComputed.begin(), jointWorldComputed.end(), 0);

		bool anyUpdated = false;

		for(size_t i = 0; i < jointNodes.size() && i < paletteMatrices->size(); i++) {
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
				<< osgGLTF::shader::JOINT_MATRICES_SSBO_BINDING << std::endl
			;
		}

		traverse(node, nv);
	}

private:
	osg::ref_ptr<GLTFSkin> _skin;
	bool _loggedOnce = false;
};
