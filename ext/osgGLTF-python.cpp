//vimrun! ./test.py

#define TINYGLTF_NOEXCEPTION

#include "osgGLTF/Reader.hpp"
#include "osgGLTF/Shader.hpp"
#include "osgGLTF/SimplePlayer.hpp"

#include <osg/Image>
#include <osgDB/FileNameUtils>

#include "pyosg/pyosg.hpp"

#include "pybind11x.hpp"

#include "tiny_gltf.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace py = pybind11;
namespace pyx = pybind11x;

namespace {

bool skipImageLoad(
	tinygltf::Image*,
	const int,
	std::string*,
	std::string*,
	int,
	int,
	const unsigned char*,
	int,
	void*
) {
	return true;
}

const char* componentTypeName(int componentType) {
	switch(componentType) {
		case TINYGLTF_COMPONENT_TYPE_BYTE: return "BYTE";
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return "UNSIGNED_BYTE";
		case TINYGLTF_COMPONENT_TYPE_SHORT: return "SHORT";
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return "UNSIGNED_SHORT";
		case TINYGLTF_COMPONENT_TYPE_INT: return "INT";
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: return "UNSIGNED_INT";
		case TINYGLTF_COMPONENT_TYPE_FLOAT: return "FLOAT";
		case TINYGLTF_COMPONENT_TYPE_DOUBLE: return "DOUBLE";
		default: return "UNKNOWN";
	}
}

const char* accessorTypeName(int type) {
	switch(type) {
		case TINYGLTF_TYPE_SCALAR: return "SCALAR";
		case TINYGLTF_TYPE_VEC2: return "VEC2";
		case TINYGLTF_TYPE_VEC3: return "VEC3";
		case TINYGLTF_TYPE_VEC4: return "VEC4";
		case TINYGLTF_TYPE_MAT2: return "MAT2";
		case TINYGLTF_TYPE_MAT3: return "MAT3";
		case TINYGLTF_TYPE_MAT4: return "MAT4";
		default: return "UNKNOWN";
	}
}

py::object maybeString(const std::string& value) {
	return value.empty() ? py::none() : py::cast(value);
}

py::object maybeNodeName(const tinygltf::Model& model, int nodeIdx) {
	if(nodeIdx < 0 || nodeIdx >= static_cast<int>(model.nodes.size())) return py::none();

	return maybeString(model.nodes[nodeIdx].name);
}

py::dict accessorInfo(const tinygltf::Model& model, int accessorIdx) {
	py::dict out;

	out["index"] = accessorIdx;

	if(accessorIdx < 0 || accessorIdx >= static_cast<int>(model.accessors.size())) {
		out["valid"] = false;
		return out;
	}

	const tinygltf::Accessor& accessor = model.accessors[accessorIdx];

	out["valid"] = true;
	out["name"] = maybeString(accessor.name);
	out["count"] = accessor.count;
	out["componentType"] = componentTypeName(accessor.componentType);
	out["componentTypeValue"] = accessor.componentType;
	out["type"] = accessorTypeName(accessor.type);
	out["typeValue"] = accessor.type;
	out["normalized"] = accessor.normalized;
	out["bufferView"] = accessor.bufferView;
	out["byteOffset"] = accessor.byteOffset;

	py::list minValues;
	for(double value : accessor.minValues) minValues.append(value);
	out["min"] = minValues;

	py::list maxValues;
	for(double value : accessor.maxValues) maxValues.append(value);
	out["max"] = maxValues;

	return out;
}

py::dict textureInfo(const tinygltf::Model& model, int textureIdx, int texCoord) {
	py::dict out;

	out["index"] = textureIdx;
	out["texCoord"] = texCoord;

	if(textureIdx >= 0 && textureIdx < static_cast<int>(model.textures.size())) {
		const tinygltf::Texture& texture = model.textures[textureIdx];

		out["valid"] = true;
		out["name"] = maybeString(texture.name);
		out["source"] = texture.source;
		out["sampler"] = texture.sampler;

		if(texture.source >= 0 && texture.source < static_cast<int>(model.images.size())) {
			const tinygltf::Image& image = model.images[texture.source];

			out["imageName"] = maybeString(image.name);
			out["imageUri"] = maybeString(image.uri);
			out["imageWidth"] = image.width;
			out["imageHeight"] = image.height;
			out["embedded"] = !image.image.empty();
		}
	}

	else out["valid"] = false;

	return out;
}

py::object textureInfoOrNone(const tinygltf::Model& model, const tinygltf::TextureInfo& info) {
	if(info.index < 0) return py::none();

	return textureInfo(model, info.index, info.texCoord);
}

py::object normalTextureInfoOrNone(
	const tinygltf::Model& model,
	const tinygltf::NormalTextureInfo& info
) {
	if(info.index < 0) return py::none();

	py::dict out = textureInfo(model, info.index, info.texCoord);

	out["scale"] = info.scale;

	return out;
}

py::object occlusionTextureInfoOrNone(
	const tinygltf::Model& model,
	const tinygltf::OcclusionTextureInfo& info
) {
	if(info.index < 0) return py::none();

	py::dict out = textureInfo(model, info.index, info.texCoord);

	out["strength"] = info.strength;

	return out;
}

py::list numberList(const std::vector<double>& values) {
	py::list out;

	for(double value : values) out.append(value);

	return out;
}

py::dict materialInfo(const tinygltf::Model& model, int materialIdx) {
	const tinygltf::Material& material = model.materials[materialIdx];
	const tinygltf::PbrMetallicRoughness& pbr = material.pbrMetallicRoughness;

	py::dict out;

	out["index"] = materialIdx;
	out["name"] = maybeString(material.name);
	out["alphaMode"] = material.alphaMode;
	out["alphaCutoff"] = material.alphaCutoff;
	out["doubleSided"] = material.doubleSided;
	out["emissiveFactor"] = numberList(material.emissiveFactor);
	out["normalTexture"] = normalTextureInfoOrNone(model, material.normalTexture);
	out["occlusionTexture"] = occlusionTextureInfoOrNone(model, material.occlusionTexture);
	out["emissiveTexture"] = textureInfoOrNone(model, material.emissiveTexture);

	py::dict pbrDict;

	pbrDict["baseColorFactor"] = numberList(pbr.baseColorFactor);
	pbrDict["metallicFactor"] = pbr.metallicFactor;
	pbrDict["roughnessFactor"] = pbr.roughnessFactor;
	pbrDict["baseColorTexture"] = textureInfoOrNone(model, pbr.baseColorTexture);
	pbrDict["metallicRoughnessTexture"] = textureInfoOrNone(model, pbr.metallicRoughnessTexture);

	out["pbrMetallicRoughness"] = pbrDict;

	bool hasSpecGloss =
		material.extensions.find("KHR_materials_pbrSpecularGlossiness") != material.extensions.end()
	;

	out["hasSpecGloss"] = hasSpecGloss;
	out["requiresSpecGlossBake"] = hasSpecGloss;
	out["hasBaseColorMap"] = pbr.baseColorTexture.index >= 0;
	out["hasMetallicRoughnessMap"] = pbr.metallicRoughnessTexture.index >= 0;
	out["hasNormalMap"] = material.normalTexture.index >= 0;
	out["hasOcclusionMap"] = material.occlusionTexture.index >= 0;
	out["hasEmissiveMap"] = material.emissiveTexture.index >= 0;
	out["factorOnlyPBR"] =
		pbr.baseColorTexture.index < 0 &&
		pbr.metallicRoughnessTexture.index < 0 &&
		!hasSpecGloss
	;
	out["textureDrivenPBR"] =
		pbr.baseColorTexture.index >= 0 ||
		pbr.metallicRoughnessTexture.index >= 0 ||
		hasSpecGloss
	;
	out["usesLowRoughnessFactor"] = pbr.roughnessFactor < 0.35;
	out["likelyReflectiveFromFactors"] =
		pbr.roughnessFactor < 0.35 &&
		pbr.metallicFactor > 0.5
	;

	py::list extensions;

	for(const auto& [name, value] : material.extensions) extensions.append(name);

	out["extensions"] = extensions;

	return out;
}

py::dict primitiveInfo(const tinygltf::Model& model, const tinygltf::Primitive& primitive, int primitiveIdx) {
	py::dict out;
	py::dict attributes;

	out["index"] = primitiveIdx;
	out["mode"] = primitive.mode;
	out["indices"] = accessorInfo(model, primitive.indices);
	out["material"] = primitive.material;

	if(primitive.material >= 0 && primitive.material < static_cast<int>(model.materials.size())) {
		out["materialName"] = maybeString(model.materials[primitive.material].name);
	}

	for(const auto& [name, accessorIdx] : primitive.attributes) {
		attributes[py::str(name)] = accessorInfo(model, accessorIdx);
	}

	out["attributes"] = attributes;
	out["hasJoints0"] = primitive.attributes.find("JOINTS_0") != primitive.attributes.end();
	out["hasWeights0"] = primitive.attributes.find("WEIGHTS_0") != primitive.attributes.end();
	out["hasPosition"] = primitive.attributes.find("POSITION") != primitive.attributes.end();
	out["hasNormal"] = primitive.attributes.find("NORMAL") != primitive.attributes.end();
	out["hasTangent"] = primitive.attributes.find("TANGENT") != primitive.attributes.end();

	return out;
}

py::dict meshInfo(const tinygltf::Model& model, int meshIdx) {
	const tinygltf::Mesh& mesh = model.meshes[meshIdx];
	py::dict out;
	py::list primitives;

	out["index"] = meshIdx;
	out["name"] = maybeString(mesh.name);

	for(size_t i = 0; i < mesh.primitives.size(); i++) {
		primitives.append(primitiveInfo(model, mesh.primitives[i], static_cast<int>(i)));
	}

	out["primitives"] = primitives;
	out["primitiveCount"] = mesh.primitives.size();

	return out;
}

py::dict skinInfo(const tinygltf::Model& model, int skinIdx) {
	const tinygltf::Skin& skin = model.skins[skinIdx];
	py::dict out;
	py::list joints;
	py::list users;

	out["index"] = skinIdx;
	out["name"] = maybeString(skin.name);
	out["skeleton"] = skin.skeleton;
	out["skeletonName"] = maybeNodeName(model, skin.skeleton);
	out["inverseBindMatrices"] = accessorInfo(model, skin.inverseBindMatrices);

	for(size_t jointIdx = 0; jointIdx < skin.joints.size(); jointIdx++) {
		int nodeIdx = skin.joints[jointIdx];
		py::dict joint;

		joint["index"] = jointIdx;
		joint["node"] = nodeIdx;
		joint["nodeName"] = maybeNodeName(model, nodeIdx);

		joints.append(joint);
	}

	for(size_t nodeIdx = 0; nodeIdx < model.nodes.size(); nodeIdx++) {
		const tinygltf::Node& node = model.nodes[nodeIdx];

		if(node.skin != skinIdx) continue;

		py::dict user;

		user["node"] = nodeIdx;
		user["nodeName"] = maybeString(node.name);
		user["mesh"] = node.mesh;

		if(node.mesh >= 0 && node.mesh < static_cast<int>(model.meshes.size())) {
			user["meshName"] = maybeString(model.meshes[node.mesh].name);
		}

		users.append(user);
	}

	out["joints"] = joints;
	out["jointCount"] = skin.joints.size();
	out["users"] = users;
	out["userCount"] = py::len(users);

	const auto ibm = model.accessors.size() > static_cast<size_t>(std::max(skin.inverseBindMatrices, 0))
		? skin.inverseBindMatrices
		: -1
	;

	out["inverseBindMatricesMatchJointCount"] =
		ibm >= 0 &&
		model.accessors[ibm].count == skin.joints.size()
	;

	return out;
}

py::dict animationInfo(const tinygltf::Model& model, int animationIdx) {
	const tinygltf::Animation& animation = model.animations[animationIdx];
	py::dict out;
	py::list samplers;
	py::list channels;
	std::set<std::string> paths;
	std::set<std::string> interpolations;
	double duration = 0.0;

	out["index"] = animationIdx;
	out["name"] = maybeString(animation.name);

	for(size_t samplerIdx = 0; samplerIdx < animation.samplers.size(); samplerIdx++) {
		const tinygltf::AnimationSampler& sampler = animation.samplers[samplerIdx];
		py::dict item;

		item["index"] = samplerIdx;
		item["input"] = accessorInfo(model, sampler.input);
		item["output"] = accessorInfo(model, sampler.output);
		item["interpolation"] = sampler.interpolation.empty() ? "LINEAR" : sampler.interpolation;

		if(
			sampler.input >= 0 &&
			sampler.input < static_cast<int>(model.accessors.size()) &&
			!model.accessors[sampler.input].maxValues.empty()
		) {
			duration = std::max(duration, model.accessors[sampler.input].maxValues[0]);
			item["endTime"] = model.accessors[sampler.input].maxValues[0];
		}

		interpolations.insert(sampler.interpolation.empty() ? "LINEAR" : sampler.interpolation);
		samplers.append(item);
	}

	for(size_t channelIdx = 0; channelIdx < animation.channels.size(); channelIdx++) {
		const tinygltf::AnimationChannel& channel = animation.channels[channelIdx];
		py::dict item;

		item["index"] = channelIdx;
		item["sampler"] = channel.sampler;
		item["targetNode"] = channel.target_node;
		item["targetNodeName"] = maybeNodeName(model, channel.target_node);
		item["targetPath"] = channel.target_path;
		item["supportedByCurrentLoader"] =
			channel.target_path == "translation" ||
			channel.target_path == "rotation" ||
			channel.target_path == "scale"
		;

		paths.insert(channel.target_path);
		channels.append(item);
	}

	py::list pathList;
	for(const auto& path : paths) pathList.append(path);

	py::list interpolationList;
	for(const auto& interpolation : interpolations) interpolationList.append(interpolation);

	out["samplers"] = samplers;
	out["samplerCount"] = animation.samplers.size();
	out["channels"] = channels;
	out["channelCount"] = animation.channels.size();
	out["targetPaths"] = pathList;
	out["interpolations"] = interpolationList;
	out["duration"] = duration;
	out["hasMorphTargetAnimation"] = paths.find("weights") != paths.end();
	out["hasCubicSpline"] = interpolations.find("CUBICSPLINE") != interpolations.end();

	return out;
}

py::dict inspectGLTF(const std::string& path, bool loadImages) {
	std::string err;
	std::string warn;
	tinygltf::Model model;
	tinygltf::TinyGLTF loader;
	const std::string ext = osgDB::getLowerCaseFileExtension(path);

	if(!loadImages) loader.SetImageLoader(skipImageLoad, nullptr);

	bool ok = ext == "glb"
		? loader.LoadBinaryFromFile(&model, &err, &warn, path)
		: loader.LoadASCIIFromFile(&model, &err, &warn, path)
	;

	if(!ok || !err.empty()) {
		throw std::runtime_error("failed to load " + path + ": " + err);
	}

	py::dict out;
	py::dict asset;
	py::dict counts;
	py::dict intent;
	py::list warnings;
	py::list scenes;
	py::list nodes;
	py::list meshes;
	py::list materials;
	py::list skins;
	py::list animations;

	if(!warn.empty()) warnings.append(warn);

	asset["version"] = model.asset.version;
	asset["minVersion"] = maybeString(model.asset.minVersion);
	asset["generator"] = maybeString(model.asset.generator);
	asset["copyright"] = maybeString(model.asset.copyright);

	counts["scenes"] = model.scenes.size();
	counts["nodes"] = model.nodes.size();
	counts["meshes"] = model.meshes.size();
	counts["materials"] = model.materials.size();
	counts["textures"] = model.textures.size();
	counts["images"] = model.images.size();
	counts["skins"] = model.skins.size();
	counts["animations"] = model.animations.size();
	counts["accessors"] = model.accessors.size();
	counts["bufferViews"] = model.bufferViews.size();
	counts["buffers"] = model.buffers.size();

	for(size_t sceneIdx = 0; sceneIdx < model.scenes.size(); sceneIdx++) {
		const tinygltf::Scene& scene = model.scenes[sceneIdx];
		py::dict sceneDict;
		py::list sceneNodes;

		sceneDict["index"] = sceneIdx;
		sceneDict["name"] = maybeString(scene.name);

		for(int nodeIdx : scene.nodes) sceneNodes.append(nodeIdx);

		sceneDict["nodes"] = sceneNodes;

		scenes.append(sceneDict);
	}

	for(size_t nodeIdx = 0; nodeIdx < model.nodes.size(); nodeIdx++) {
		const tinygltf::Node& node = model.nodes[nodeIdx];
		py::dict nodeDict;
		py::list children;

		nodeDict["index"] = nodeIdx;
		nodeDict["name"] = maybeString(node.name);
		nodeDict["mesh"] = node.mesh;
		nodeDict["skin"] = node.skin;

		for(int childIdx : node.children) children.append(childIdx);

		nodeDict["children"] = children;

		nodeDict["hasMatrix"] = node.matrix.size() == 16;
		nodeDict["hasTranslation"] = node.translation.size() == 3;
		nodeDict["hasRotation"] = node.rotation.size() == 4;
		nodeDict["hasScale"] = node.scale.size() == 3;

		nodes.append(nodeDict);
	}

	for(size_t meshIdx = 0; meshIdx < model.meshes.size(); meshIdx++) {
		meshes.append(meshInfo(model, static_cast<int>(meshIdx)));
	}

	for(size_t materialIdx = 0; materialIdx < model.materials.size(); materialIdx++) {
		materials.append(materialInfo(model, static_cast<int>(materialIdx)));
	}

	for(size_t skinIdx = 0; skinIdx < model.skins.size(); skinIdx++) {
		skins.append(skinInfo(model, static_cast<int>(skinIdx)));
	}

	for(size_t animationIdx = 0; animationIdx < model.animations.size(); animationIdx++) {
		animations.append(animationInfo(model, static_cast<int>(animationIdx)));
	}

	bool hasSpecGloss = false;
	bool hasJoints0 = false;
	bool hasWeights0 = false;
	bool hasMorphTargets = false;
	bool hasPBRTextures = false;

	for(const auto& material : model.materials) {
		hasSpecGloss = hasSpecGloss ||
			material.extensions.find("KHR_materials_pbrSpecularGlossiness") != material.extensions.end()
		;

		hasPBRTextures = hasPBRTextures ||
			material.pbrMetallicRoughness.baseColorTexture.index >= 0 ||
			material.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0 ||
			material.normalTexture.index >= 0 ||
			material.occlusionTexture.index >= 0 ||
			material.emissiveTexture.index >= 0
		;
	}

	for(const auto& mesh : model.meshes) {
		for(const auto& primitive : mesh.primitives) {
			hasJoints0 = hasJoints0 || primitive.attributes.find("JOINTS_0") != primitive.attributes.end();
			hasWeights0 = hasWeights0 || primitive.attributes.find("WEIGHTS_0") != primitive.attributes.end();
			hasMorphTargets = hasMorphTargets || !primitive.targets.empty();
		}
	}

	intent["hasSkinning"] = !model.skins.empty() || hasJoints0 || hasWeights0;
	intent["hasAnimation"] = !model.animations.empty();
	intent["hasMorphTargets"] = hasMorphTargets;
	intent["hasSpecGloss"] = hasSpecGloss;
	intent["hasPBRTextures"] = hasPBRTextures;
	intent["hasJoints0"] = hasJoints0;
	intent["hasWeights0"] = hasWeights0;

	out["path"] = path;
	out["asset"] = asset;
	out["counts"] = counts;
	out["intent"] = intent;
	out["warnings"] = warnings;
	out["defaultScene"] = model.defaultScene;
	out["scenes"] = scenes;
	out["nodes"] = nodes;
	out["meshes"] = meshes;
	out["materials"] = materials;
	out["skins"] = skins;
	out["animations"] = animations;

	return out;
}

std::string inspectGLTFJson(const std::string& path, bool loadImages, int indent) {
	py::object json = py::module_::import("json");

	return py::str(json.attr("dumps")(
		inspectGLTF(path, loadImages),
		"indent"_a=indent,
		"sort_keys"_a=true
	));
}

// Async glTF load: releases the GIL and calls osgGLTF::Reader directly (bypassing the generic
// osgDB::readNodeFile plugin dispatch, which has no hook for a progress callback), so a
// caller can run this via asyncio.to_thread(...) while the viewer keeps rendering. Progress
// (and the final node) are delivered through the same loop/queue call_soon_threadsafe bridge
// as pyosg_async_task_example -- see pybind11x::put_nowait.
//
// Cancellation via `stop` is cooperative and can only take effect between osgGLTF::Reader's own
// checkpoints (see osgGLTF::Reader::ProgressCallback) -- it cannot interrupt tinygltf's own file
// parse/decode, which is a single opaque blocking call. If a stop was requested by the time
// read() returns, the result is discarded (not attached to the scene) and "complete" is
// delivered with None instead of the loaded node.
osg::ref_ptr<osg::Node> readNodeFileAsync(
	std::string location,
	pyx::StopEvent* stop,
	py::object loop,
	py::object queue,
	size_t job_id
) {
	py::gil_scoped_release release;

	// Same texture-dedup cache ReaderWriterGLTF::readNode() wires up for the normal
	// osgDB::readNodeFile() path. Without this, the reader's three cache-checking call sites
	// (occlusion/metallic-roughness bake, base color, normal map) all skip their cache lookup and
	// silently reload+redecode any texture referenced by more than one material in the
	// model -- measured 4.5x slower (13.5s vs 2.96s) on a real multi-material asset
	// before this was added.
	static osgGLTF::Reader::TextureCache s_asyncTextureCache;

	const std::string ext = osgDB::getLowerCaseFileExtension(location);
	const bool isBinary = ext == "glb";

	osgGLTF::Reader reader;

	reader.setTextureCache(&s_asyncTextureCache);

	osgGLTF::Reader::ProgressCallback onProgress = [&](
		osgGLTF::Reader::Stage stage,
		size_t current,
		size_t total
	) {
		pyx::put_nowait(
			loop,
			queue,
			"progress",
			job_id,
			std::string(osgGLTF::Reader::stageName(stage)),
			current,
			total
		);
	};

	auto result = reader.read(location, isBinary, nullptr, onProgress);

	if(stop && stop->stop.load(std::memory_order_relaxed)) {
		pyx::put_nowait(loop, queue, "complete", job_id, py::none());

		return nullptr;
	}

	osg::ref_ptr<osg::Node> node = result.validNode() ? result.getNode() : nullptr;

	pyx::put_nowait(loop, queue, "complete", job_id, node);

	return node;
}

}

PYBIND11_MODULE(osgGLTF, m) {
	auto py_osg = py::module_::import("OpenSceneGraph");
	auto m_shader = m.def_submodule(
		"shader",
		"Shader inputs and setup matching the scene state populated by the osgGLTF loader"
	);

	m_shader.attr("TANGENT_ATTRIBUTE") = osgGLTF::shader::TANGENT_ATTRIBUTE;
	m_shader.attr("JOINT_INDICES_ATTRIBUTE") = osgGLTF::shader::JOINT_INDICES_ATTRIBUTE;
	m_shader.attr("JOINT_WEIGHTS_ATTRIBUTE") = osgGLTF::shader::JOINT_WEIGHTS_ATTRIBUTE;
	m_shader.attr("TANGENT_ATTRIBUTE_NAME") = py::str(osgGLTF::shader::TANGENT_ATTRIBUTE_NAME);
	m_shader.attr("JOINT_INDICES_ATTRIBUTE_NAME") =
		py::str(osgGLTF::shader::JOINT_INDICES_ATTRIBUTE_NAME);
	m_shader.attr("JOINT_WEIGHTS_ATTRIBUTE_NAME") =
		py::str(osgGLTF::shader::JOINT_WEIGHTS_ATTRIBUTE_NAME);
	m_shader.attr("MATERIAL_UBO_BINDING") = osgGLTF::shader::MATERIAL_UBO_BINDING;
	m_shader.attr("JOINT_MATRICES_SSBO_BINDING") =
		osgGLTF::shader::JOINT_MATRICES_SSBO_BINDING;
	m_shader.attr("BASE_COLOR_TEXTURE_UNIT") = osgGLTF::shader::BASE_COLOR_TEXTURE_UNIT;
	m_shader.attr("NORMAL_TEXTURE_UNIT") = osgGLTF::shader::NORMAL_TEXTURE_UNIT;
	m_shader.attr("ORM_TEXTURE_UNIT") = osgGLTF::shader::ORM_TEXTURE_UNIT;
	m_shader.attr("EMISSIVE_TEXTURE_UNIT") = osgGLTF::shader::EMISSIVE_TEXTURE_UNIT;
	m_shader.attr("BASE_COLOR_SAMPLER") = py::str(osgGLTF::shader::BASE_COLOR_SAMPLER);
	m_shader.attr("NORMAL_SAMPLER") = py::str(osgGLTF::shader::NORMAL_SAMPLER);
	m_shader.attr("ORM_SAMPLER") = py::str(osgGLTF::shader::ORM_SAMPLER);
	m_shader.attr("EMISSIVE_SAMPLER") = py::str(osgGLTF::shader::EMISSIVE_SAMPLER);
	m_shader.attr("ALPHA_MODE_UNIFORM") = py::str(osgGLTF::shader::ALPHA_MODE_UNIFORM);
	m_shader.attr("ALPHA_CUTOFF_UNIFORM") = py::str(osgGLTF::shader::ALPHA_CUTOFF_UNIFORM);
	m_shader.attr("ALPHA_MODE_OPAQUE") = osgGLTF::shader::ALPHA_MODE_OPAQUE;
	m_shader.attr("ALPHA_MODE_MASK") = osgGLTF::shader::ALPHA_MODE_MASK;
	m_shader.attr("ALPHA_MODE_BLEND") = osgGLTF::shader::ALPHA_MODE_BLEND;
	m_shader.attr("MATERIAL_INPUTS") = py::str(osgGLTF::shader::MATERIAL_INPUTS);

	m_shader
		.def("configureProgram", &osgGLTF::shader::configureProgram, "program"_a)
		.def("configureStateSet", &osgGLTF::shader::configureStateSet, "stateSet"_a)
	;

	py::class_<osgGLTF::SimplePlayer>(m, "SimplePlayer")
		.def(py::init<osg::Node*>(), "model"_a)
		.def("__bool__", [](const osgGLTF::SimplePlayer& player) {
			return static_cast<bool>(player);
		})
		.def_property_readonly(
			"numAnimations",
			&osgGLTF::SimplePlayer::getNumAnimations
		)
		.def("getAnimationName", &osgGLTF::SimplePlayer::getAnimationName, "index"_a)
		.def(
			"playAnimation",
			py::overload_cast<std::size_t>(&osgGLTF::SimplePlayer::playAnimation),
			"index"_a
		)
		.def(
			"playAnimation",
			py::overload_cast<const std::string&>(&osgGLTF::SimplePlayer::playAnimation),
			"name"_a
		)
		.def_property_readonly(
			"currentAnimationIndex",
			[](const osgGLTF::SimplePlayer& player) -> py::object {
				const std::size_t index = player.getCurrentAnimationIndex();
				return index == osgGLTF::SimplePlayer::NoAnimation
					? py::none()
					: py::cast(index);
			}
		)
		.def_property_readonly(
			"currentAnimationName",
			&osgGLTF::SimplePlayer::getCurrentAnimationName
		)
		.def_property(
			"playing",
			&osgGLTF::SimplePlayer::getPlaying,
			&osgGLTF::SimplePlayer::setPlaying
		)
		.def("togglePlaying", &osgGLTF::SimplePlayer::togglePlaying)
		.def("restart", &osgGLTF::SimplePlayer::restart)
	;

	m
		.def(
			"readNodeFileAsync",
			&readNodeFileAsync,
			"location"_a,
			"stop_event"_a,
			"loop"_a,
			"queue"_a,
			"job_id"_a,
			"Load a glTF/GLB file off the GIL, reporting (stage, current, total) progress and "
			"the final node through loop/queue via call_soon_threadsafe. Call via "
			"asyncio.to_thread(...); see examples/pyosg-async.py for the queue-draining pattern."
		)
	;

	m
		.def(
			"inspect",
			&inspectGLTF,
			"path"_a,
			"load_images"_a=false,
			"Parse a glTF/GLB file and return a structured Python summary."
		)
		.def(
			"inspect_json",
			&inspectGLTFJson,
			"path"_a,
			"load_images"_a=false,
			"indent"_a=2,
			"Parse a glTF/GLB file and return a JSON summary string."
		)
	;

	m.doc() = "osgGLTF - OpenSceneGraph + GLTF/KTX2 (https://github.com/XenonOfArcticus/osgGLTF)";

	py::dict info;

	info["version"] = py::make_tuple(
		0, // OSGGLTF_VERSION_MAJOR,
		0, // OSGGLTF_VERSION_MINOR,
		1 // OSGGLTF_VERSION_PATCH
	);

	pyx::build_info(m, info);
}
