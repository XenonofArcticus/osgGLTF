#pragma once

#include <osg/Callback>
#include <osg/Matrixd>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/Quat>
#include <osg/Vec3d>
#include <osg/observer_ptr>

#include <osgGLTF/SimplePlayer.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace tinygltf { class Node; }

namespace osgGLTF::detail {

struct TRS {
	osg::Vec3d translation = osg::Vec3d(0.0, 0.0, 0.0);
	osg::Quat rotation;
	osg::Vec3d scale = osg::Vec3d(1.0, 1.0, 1.0);

	osg::Matrixd matrix() const;
};

TRS nodeBaseTRS(const tinygltf::Node& node);

class AnimationCallback:
	public osg::NodeCallback,
	public SimplePlayerControl {
public:
	enum class Path {
		Translation,
		Rotation,
		Scale
	};

	struct Channel {
		osg::observer_ptr<osg::MatrixTransform> target;
		int targetNode = -1;
		Path path = Path::Translation;
		std::string interpolation = "LINEAR";
		std::vector<float> times;
		std::vector<osg::Vec3d> vec3Values;
		std::vector<osg::Quat> quatValues;
	};

	struct Clip {
		std::string name;
		std::vector<Channel> channels;
		double duration = 0.0;
	};

	std::vector<Clip> clips;
	std::map<int, TRS> baseTRS;

	std::size_t getNumAnimations() const override;
	std::string getAnimationName(std::size_t index) const override;
	bool playAnimation(std::size_t index) override;
	bool playAnimation(const std::string& name) override;
	std::size_t getCurrentAnimationIndex() const override;
	std::string getCurrentAnimationName() const override;
	void setPlaying(bool playing) override;
	bool getPlaying() const override;
	void restart() override;
	void operator()(osg::Node* node, osg::NodeVisitor* nv) override;

private:
	bool _started = false;
	bool _playing = true;
	bool _restartRequested = false;
	std::size_t _activeClip = 0;
	double _playTime = 0.0;
	double _lastSimulationTime = 0.0;

	void restoreBasePose();
	static std::size_t sampleIndex(const std::vector<float>& times, double t, double& mix);
	static osg::Vec3d sampleVec3(const Channel& channel, double t);
	static osg::Quat sampleQuat(const Channel& channel, double t);
};

}
