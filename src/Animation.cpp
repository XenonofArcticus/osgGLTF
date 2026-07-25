#define TINYGLTF_NOEXCEPTION

#include "tiny_gltf.h"

#include "Animation.hpp"
#include "Log.hpp"

#include <osg/FrameStamp>

#include <algorithm>
#include <cmath>

namespace osgGLTF::detail {

osg::Matrixd TRS::matrix() const {
	return
		osg::Matrixd::scale(scale) *
		osg::Matrixd::rotate(rotation) *
		osg::Matrixd::translate(translation)
	;
}

TRS nodeBaseTRS(const tinygltf::Node& node) {
	TRS trs;

	if(node.matrix.size() == 16) {
		osg::Quat so;
		osg::Matrixd matrix(node.matrix.data());

		matrix.decompose(trs.translation, trs.rotation, trs.scale, so);
	}

	if(node.translation.size() == 3) trs.translation.set(
		node.translation[0],
		node.translation[1],
		node.translation[2]
	);

	if(node.rotation.size() == 4) trs.rotation.set(
		node.rotation[0],
		node.rotation[1],
		node.rotation[2],
		node.rotation[3]
	);

	if(node.scale.size() == 3) trs.scale.set(
		node.scale[0],
		node.scale[1],
		node.scale[2]
	);

	return trs;
}

std::size_t AnimationCallback::getNumAnimations() const {
	return clips.size();
}

std::string AnimationCallback::getAnimationName(std::size_t index) const {
	return index < clips.size() ? clips[index].name : std::string();
}

bool AnimationCallback::playAnimation(std::size_t index) {
	if(index >= clips.size()) return false;

	_activeClip = index;
	_playing = true;
	_restartRequested = true;

	return true;
}

bool AnimationCallback::playAnimation(const std::string& name) {
	for(std::size_t i = 0; i < clips.size(); i++) {
		if(clips[i].name == name) return playAnimation(i);
	}

	return false;
}

std::size_t AnimationCallback::getCurrentAnimationIndex() const {
	return _activeClip < clips.size() ? _activeClip : SimplePlayer::NoAnimation;
}

std::string AnimationCallback::getCurrentAnimationName() const {
	return getAnimationName(getCurrentAnimationIndex());
}

void AnimationCallback::setPlaying(bool playing) {
	_playing = playing;
}

bool AnimationCallback::getPlaying() const {
	return _playing;
}

void AnimationCallback::restart() {
	_restartRequested = true;
}

void AnimationCallback::operator()(osg::Node* node, osg::NodeVisitor* nv) {
	if(_activeClip >= clips.size()) {
		traverse(node, nv);

		return;
	}

	const Clip& clip = clips[_activeClip];

	if(clip.channels.empty() || clip.duration <= 0.0) {
		traverse(node, nv);

		return;
	}

	double simTime = 0.0;

	if(nv && nv->getFrameStamp()) simTime = nv->getFrameStamp()->getSimulationTime();

	if(!_started || _restartRequested) {
		_started = true;
		_restartRequested = false;
		_playTime = 0.0;
		_lastSimulationTime = simTime;
		restoreBasePose();

		GLTF_NOTIFY(1)
			<< "playing animation '" << clip.name << "'"
			<< " duration=" << clip.duration
			<< " channel(s)=" << clip.channels.size() << std::endl
		;
	}

	if(_playing) _playTime += std::max(0.0, simTime - _lastSimulationTime);

	_lastSimulationTime = simTime;

	if(!_playing) {
		traverse(node, nv);

		return;
	}

	double t = std::fmod(_playTime, clip.duration);
	std::map<int, TRS> current = baseTRS;

	for(const Channel& channel : clip.channels) {
		auto it = current.find(channel.targetNode);

		if(it == current.end()) continue;

		if(channel.path == Path::Rotation) it->second.rotation = sampleQuat(channel, t);
		else {
			osg::Vec3d v = sampleVec3(channel, t);

			if(channel.path == Path::Translation) it->second.translation = v;
			else if(channel.path == Path::Scale) it->second.scale = v;
		}
	}

	for(const auto& [nodeIdx, trs] : current) {
		osg::ref_ptr<osg::MatrixTransform> target;

		for(const Channel& channel : clip.channels) {
			if(channel.targetNode == nodeIdx) {
				channel.target.lock(target);

				break;
			}
		}

		if(target) target->setMatrix(trs.matrix());
	}

	traverse(node, nv);
}

void AnimationCallback::restoreBasePose() {
	for(const Clip& clip : clips) {
		for(const Channel& channel : clip.channels) {
			auto it = baseTRS.find(channel.targetNode);

			if(it != baseTRS.end() && channel.target.valid()) {
				channel.target->setMatrix(it->second.matrix());
			}
		}
	}
}

std::size_t AnimationCallback::sampleIndex(
	const std::vector<float>& times,
	double t,
	double& mix
) {
	if(times.size() < 2) {
		mix = 0.0;

		return 0;
	}

	auto hi = std::upper_bound(times.begin(), times.end(), static_cast<float>(t));

	if(hi == times.begin()) {
		mix = 0.0;

		return 0;
	}

	if(hi == times.end()) {
		mix = 0.0;

		return times.size() - 1;
	}

	std::size_t i0 = static_cast<std::size_t>(std::distance(times.begin(), hi) - 1);
	float t0 = times[i0];
	float t1 = times[i0 + 1];

	mix = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0;

	return i0;
}

osg::Vec3d AnimationCallback::sampleVec3(const Channel& channel, double t) {
	if(channel.vec3Values.empty()) return osg::Vec3d();

	double mix = 0.0;
	std::size_t i = sampleIndex(channel.times, t, mix);

	if(channel.interpolation == "STEP" || i + 1 >= channel.vec3Values.size()) {
		return channel.vec3Values[std::min(i, channel.vec3Values.size() - 1)];
	}

	const osg::Vec3d& a = channel.vec3Values[i];
	const osg::Vec3d& b = channel.vec3Values[i + 1];

	return a * (1.0 - mix) + b * mix;
}

osg::Quat AnimationCallback::sampleQuat(const Channel& channel, double t) {
	if(channel.quatValues.empty()) return osg::Quat();

	double mix = 0.0;
	std::size_t i = sampleIndex(channel.times, t, mix);

	if(channel.interpolation == "STEP" || i + 1 >= channel.quatValues.size()) {
		return channel.quatValues[std::min(i, channel.quatValues.size() - 1)];
	}

	osg::Quat q;

	q.slerp(mix, channel.quatValues[i], channel.quatValues[i + 1]);

	return q;
}

}
