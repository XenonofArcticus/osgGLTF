#pragma once

#include <osgx/Warnings.hpp>

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Group>
#include <osg/Node>
#include <osg/Texture2D>
#include <osg/TextureCubeMap>
#include <osg/Uniform>
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

// Everything returned by createPBRIBLScene(). lutCamera and diffuseBakeRoot must be added to a
// rendered scene graph so their PRE_RENDER passes can populate the BRDF LUT and diffuse cubemap.
// The textures remain available for reuse by other renderer-owned state, such as a skybox.
struct PBRIBLScene {
	osg::ref_ptr<osg::Camera> lutCamera;
	osg::ref_ptr<osg::Group> diffuseBakeRoot;
	osg::ref_ptr<osg::TextureCubeMap> envMap;
	osg::ref_ptr<osg::Texture2D> brdfLUT;
	osg::ref_ptr<osg::TextureCubeMap> diffuseEnv;

	// These are null unless diagnostics were requested.
	osg::ref_ptr<osg::Uniform> debugMode;
	osg::ref_ptr<osg::Uniform> disableNormalMap;
	osg::ref_ptr<osg::Uniform> disableRoughnessMap;

	bool valid() const;
};

// Applies osgGLTF's osgx-powered PBR/IBL renderer to an already-loaded glTF node. The caller owns
// the node and must add the returned lutCamera and diffuseBakeRoot to a rendered scene graph.
PBRIBLScene createPBRIBLScene(
	osg::Node* node,
	const std::string& ktx2Path,
	const std::string& hdrPath,
	float iblIntensity=1.0f,
	int lutSize=1024,
	bool diagnostics=false
);

}
