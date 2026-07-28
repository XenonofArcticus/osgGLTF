#pragma once

#include <osgx/Warnings.hpp>

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Group>
#include <osg/Node>
#include <osg/Texture2D>
#include <osg/TextureCubeMap>
#include <osg/Uniform>
#include <osg/Vec3>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <string>
#include <string_view>

#include <osgx/LambertianBake.hpp>

namespace osgGLTF::pbr {

// GLSL helpers that interpret the exact material interface declared by Shader.hpp. These are
// glTF-specific adapters over the renderer-independent snippets provided by osgx::pbr.
extern const char GET_MATERIAL[];
extern const char SHADING_NORMAL[];
extern const char EMISSIVE[];
extern const char ALPHA_COVERAGE[];

// Registers the osgGLTF shader catalog used by `#pragma osgGLTF ...`. Registration is idempotent.
void registerShaderLibs();

// Registers the generic osgx PBR/IBL catalogs plus osgGLTF's catalog, then expands them together.
// Keeping registration and resolution in this component avoids cross-shared-library registry
// assumptions for Python and plugin consumers.
std::string resolveShaderLibs(std::string_view source);

// Prepared IBL resources. `root`, when present, contains the PRE_RENDER passes that populate the
// generated BRDF LUT and diffuse cubemap; add it to a rendered scene graph before using them.
// Pre-baked resources have no preparation root and can leave it null.
struct PBRIBLEnvironment {
	osg::ref_ptr<osg::Group> root;
	osg::ref_ptr<osg::Camera> lutCamera;
	osg::ref_ptr<osg::Group> diffuseBakeRoot;
	// Only present when the specular cubemap came from preparePBRIBLEnvironment(hdrPath, ...) --
	// the pre-baked-KTX2 overload has no bake to drive and leaves this null.
	osg::ref_ptr<osg::Group> specularBakeRoot;
	osg::ref_ptr<osg::TextureCubeMap> envMap;
	osg::ref_ptr<osg::Texture2D> brdfLUT;
	osg::ref_ptr<osg::TextureCubeMap> diffuseEnv;
	// KTX/OpenGL cubemap lookup basis, expressed relative to osgGLTF's Z-up world.
	osg::Vec3 iblAxisX{0.0f, 0.0f, 1.0f};
	osg::Vec3 iblAxisY{0.0f, 1.0f, 0.0f};
	osg::Vec3 iblAxisZ{-1.0f, 0.0f, 0.0f};

	bool valid() const;
};

// Pre-baked specular path: loads a finished GGX-prefiltered KTX2 from disk, still bakes diffuse
// irradiance and the BRDF LUT live from `hdrPath`. Kept for the Khronos-parity harness and for
// callers with an existing offline `osggltf-iblbake-gpu` bake to reuse.
PBRIBLEnvironment preparePBRIBLEnvironment(const std::string& ktx2Path, const std::string& hdrPath, int lutSize=1024);

// Fully dynamic path: bakes the GGX-prefiltered specular cubemap live, in memory, from `hdrPath`
// alone -- the same osgx::ibl::createGGXPrefilterScene() workflow osggltf-iblbake-gpu already
// wraps to write a KTX2 to disk, called directly instead of round-tripping through a file. Frame-
// driven like the existing diffuse/LUT bakes: envMap is a valid, bindable texture immediately, but
// its contents only become correct once specularBakeRoot's passes have actually run a few frames.
PBRIBLEnvironment preparePBRIBLEnvironment(const std::string& hdrPath, int lutSize=1024);

struct PBRIBLScene {
	osg::ref_ptr<osg::Node> node;
	osg::ref_ptr<osg::Uniform> debugMode;
	osg::ref_ptr<osg::Uniform> disableNormalMap;
	osg::ref_ptr<osg::Uniform> disableRoughnessMap;
	osg::ref_ptr<osg::Uniform> disableSpecularAA;

	bool valid() const;
};

// Applies osgGLTF's renderer to a node using reusable prepared resources.
PBRIBLScene createPBRIBLScene(
	osg::Node* node,
	const PBRIBLEnvironment& environment,
	float iblIntensity=1.0f,
	bool diagnostics=false
);

}
