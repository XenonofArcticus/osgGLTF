#pragma once

#include <osg/Callback>
#include <osg/FrameStamp>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

struct GLTFTRS {
	osg::Vec3d translation = osg::Vec3d(0.0, 0.0, 0.0);
	osg::Quat rotation;
	osg::Vec3d scale = osg::Vec3d(1.0, 1.0, 1.0);

	osg::Matrixd matrix() const {
		return
			osg::Matrixd::scale(scale) *
			osg::Matrixd::rotate(rotation) *
			osg::Matrixd::translate(translation)
		;
	}
};

inline GLTFTRS gltfNodeBaseTRS(const tinygltf::Node& node) {
	GLTFTRS trs;

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

class GLTFAnimationCallback: public osg::NodeCallback {
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

	std::string name;
	std::vector<Channel> channels;
	std::map<int, GLTFTRS> baseTRS;
	double duration = 0.0;

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override {
		if(channels.empty() || duration <= 0.0) {
			traverse(node, nv);
			return;
		}

		double simTime = 0.0;

		if(nv && nv->getFrameStamp()) simTime = nv->getFrameStamp()->getSimulationTime();

		if(!_started) {
			_started = true;
			_startTime = simTime;

			GLTF_NOTIFY(1)
				<< "playing animation '" << name << "'"
				<< " duration=" << duration
				<< " channel(s)=" << channels.size() << std::endl
			;
		}

		double t = std::fmod(std::max(0.0, simTime - _startTime), duration);
		std::map<int, GLTFTRS> current = baseTRS;

		for(const Channel& channel : channels) {
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
			osg::MatrixTransform* target = nullptr;

			for(const Channel& channel : channels) {
				if(channel.targetNode == nodeIdx) {
					target = channel.target.get();
					break;
				}
			}

			if(target) target->setMatrix(trs.matrix());
		}

		traverse(node, nv);
	}

private:
	bool _started = false;
	double _startTime = 0.0;

	static size_t sampleIndex(const std::vector<float>& times, double t, double& mix) {
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

		size_t i0 = static_cast<size_t>(std::distance(times.begin(), hi) - 1);
		float t0 = times[i0];
		float t1 = times[i0 + 1];

		mix = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0;

		return i0;
	}

	static osg::Vec3d sampleVec3(const Channel& channel, double t) {
		if(channel.vec3Values.empty()) return osg::Vec3d();

		double mix = 0.0;
		size_t i = sampleIndex(channel.times, t, mix);

		if(channel.interpolation == "STEP" || i + 1 >= channel.vec3Values.size())
			return channel.vec3Values[std::min(i, channel.vec3Values.size() - 1)];

		const osg::Vec3d& a = channel.vec3Values[i];
		const osg::Vec3d& b = channel.vec3Values[i + 1];

		return a * (1.0 - mix) + b * mix;
	}

	static osg::Quat sampleQuat(const Channel& channel, double t) {
		if(channel.quatValues.empty()) return osg::Quat();

		double mix = 0.0;
		size_t i = sampleIndex(channel.times, t, mix);

		if(channel.interpolation == "STEP" || i + 1 >= channel.quatValues.size())
			return channel.quatValues[std::min(i, channel.quatValues.size() - 1)];

		osg::Quat q;

		q.slerp(mix, channel.quatValues[i], channel.quatValues[i + 1]);

		return q;
	}
};
