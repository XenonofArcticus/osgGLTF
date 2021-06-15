# osgGLTF

Support for the GLTF format currently exists in 2 different places: a standalone
codebase (*1.0 ONLY*) and a newer implementation included with osgEarth.

Neither of these offer complete solutions, but combined they would facilitate
creating a more robust, **official** reader/writer that could be included with
OSG itself.

## osgGLTF (TODO)

- [x] Determine quality of the
  [jesesun](https://github.com/jesesun/osgdb_gltf) reference implementation.

- [x] Determine quality of the
  [osgEarth](https://github.com/gwaldron/osgearth/tree/master/src/osgEarthDrivers/gltf)
  reference implementation.

- [x] Include testing models wholesale, or use
  [the Khronos Samples](https://github.com/KhronosGroup/glTF-Sample-Models) as a submodule.

- [x] Evaluate the differences between
  [tinygltf](https://github.com/syoyo/tinygltf) and
  [tinygltfloader](https://github.com/syoyo/tinygltfloader); they have the same author?
  > **NOTE**: tinygltfloader is GLTF 1.0 *ONLY*, and has been deprecated.

- [ ] Determine what would be involved for fully supporting PBR with a new
  reader/writer implementation.

- [ ] Should any potential *new* code be generic enough to support both OpenGL
  and Vulkan?

### Jesesun Reference Implementation

This code uses the deprecated **tinygltfloader** library, which only supports
the GLTF 1.0 format. However, unlike the osgEarth codebase, this implementation
*does* have animation support (although it still needs to be updated in your
code every frame with the timestamp delta). It compiles without issue, and loads
most of the 1.0 GLTF models in the official **glTF-Sample-Models** submodule.

> **NOTE**: This project doesn't have any clear indication of its license. It
> can only be used "as-is" or as "inspiration" for new code.

### osgEarth Reference Implementation

The osgEarth loader uses a *modified* version of the **tinygltf** library, which
means it does--to some degree--support loading most of the sample models.
However, there is no animation support, and it uses an "alternative" JSON
library than the default (**rapidjson**), to maintain consistency with the rest
of osgEarth.

This code would be the optimal base for creating a newer, complete GLTF reader.
Ideally, we would remove the **rapidjson** dependency, break out the
osgEarth-only components (such as B3DM), and use the **jesesun** animation code
as inspiration for a complete osgAnimation implementation.

I was required to "comment out" small parts of this implementation in order to
get it to compile and link. As mentioned above, it uses a *modified* version of
the **tinygltf** library (2.4.1 + some custom changes related to texturing).

### Testing Models

TODO

> **NOTE**: Without a proper, custom rendering/shader pipeline, *any* model
> loaded--with either of the two exisiting projects--looks bland and generic.

### PBR

TODO
