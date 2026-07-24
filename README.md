# osgGLTF

Basic glTF 2.0 mesh/texture support for OpenSceneGraph. As of 6/1/26, works for
all of the [the Khronos samples](https://github.com/KhronosGroup/glTF-Sample-Models),
with more to potentially come.

## Rendering Comparison

![osgGLTF render compared with BabylonJS](ext/github/compare-babylonjs.png)

The screenshot compares osgGLTF's current PBR/IBL rendering against BabylonJS using
the same model and environment lighting. The goal is not pixel-perfect matching, but
matching the material response closely enough that reflections, roughness, and HDR
environment lighting behave like a modern glTF renderer should.

## Notes

This code is a patched version of the [osgEarth](https://github.com/gwaldron/osgearth/tree/master/src/osgEarthDrivers/gltf)
reference implementation. Many thanks to [Pelican
Mapping](https://www.pelicanmapping.com/) for doing 95% of the work!

Be sure and call `osgDB::Registry::instance()->addFileExtensionAlias("glb", "gltf");`
if you want to support GLB loading easily!

## Shader Interface

osgGLTF loads geometry and materials without imposing a particular renderer. The public
`osgGLTF/Shader.hpp` header defines the attribute locations, buffer bindings, texture units,
uniform names, and canonical GLSL material declaration populated by the loader.

Custom renderers can use the GLSL declaration directly and apply the matching program setup:

```cpp
#include <osgGLTF/Shader.hpp>

auto program = new osg::Program();
auto stateSet = model->getOrCreateStateSet();

// Add shaders using osgGLTF::shader::MATERIAL_INPUTS as part of the fragment source.
osgGLTF::shader::configureProgram(*program);
osgGLTF::shader::configureStateSet(*stateSet);

stateSet->setAttributeAndModes(program);
```

`configureProgram()` maps the tangent and skinning inputs to the locations used by the loader.
`configureStateSet()` maps the material samplers to the loader's base-color, normal, ORM, and
emissive texture units. These helpers are optional; the named constants in the same header can be
used when an application needs different program or StateSet ownership.

The Python bindings expose the same constants, GLSL source, and helpers under `osgGLTF.shader`.

## IBL Baking

The GPU IBL baker is available as both a command-line tool and a small C++ API.

```bash
osggltf-iblbake-gpu input.hdr output.ktx2 --prefilter-size 128
```

Applications can link `osgGLTF` and use:

```cpp
#include <osgGLTF/IBLBaker.hpp>

osgGLTF::IBLBakeOptions options;
options.prefilterSize = 128;

auto specularEnv = osgGLTF::bakeSpecularIBL("input.hdr", options);
```

The API returns an `osg::TextureCubeMap` with a full GGX-prefiltered mip chain.
Use `bakeSpecularIBLToKTX2()` when you want the library to write the KTX2 file directly.

Python bindings for `bakeSpecularIBL` are planned via
[OpenSceneGraph.py](https://github.com/cubicool/OpenSceneGraph.py), which will allow
calling the bake pipeline from Python with a single line and receiving a
`TextureCubeMap` that drops directly into an OSG scene graph.

Future work: a dynamic reflection-probe API similar in spirit to Unreal Engine's
Reflection Capture Actors and Unity's Reflection Probes. The current baker is
synchronous and bakes from an equirectangular HDR image. A later API will support
`ASYNC` capture directly from a live scene — attaching a probe to an existing viewer
and updating the prefiltered cubemap over normal render frames without stalling the
application.
