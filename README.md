# osgGLTF

Basic glTF 2.0 mesh/texture support for OpenSceneGraph. As of 6/1/26, works for
all of the [the Khronos samples](https://github.com/KhronosGroup/glTF-Sample-Models),
with more to potentially come.

## Notes

This code is a patched version of the [osgEarth](https://github.com/gwaldron/osgearth/tree/master/src/osgEarthDrivers/gltf)
reference implementation. Many thanks to [Pelican
Mapping](https://www.pelicanmapping.com/) for doing 95% of the work!

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

Future work: expose a runtime probe/capture API similar in spirit to Unreal Engine's
Reflection Capture Actors and Unity's Reflection Probes. The current baker is a
synchronous utility; a later API should distinguish explicit `SYNC` and `ASYNC` bake
modes so applications can either block for an immediate cubemap or attach an
environment/reflection probe to an existing viewer and update it over normal frames.
