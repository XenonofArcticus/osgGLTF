/* GLTFReader.h — standalone OSG glTF 2.0 reader, no osgEarth dependency.
 * Derived from osgEarth's GLTFReader (Pelican Mapping, LGPL 2+).
 * Stripped: URI class, InstanceBuilder, StateTransition, shaderGenerator,
 *           DiscardAlphaFragments, Mutexed<UnorderedMap>, OWT_state extension.
 * Replaced: osgEarth::URI image loading -> osgDB::readImageFile,
 *           osgEarth logging macros -> OSG_WARN/GLTF_NOTIFY,
 *           osgEarth mutex wrapper -> std::mutex + std::lock_guard.
 *
 * IMPORTANT: Do NOT include this header before tiny_gltf.h.
 * The including .cpp must define TINYGLTF_IMPLEMENTATION (and the STB
 * implementation macros) and include tiny_gltf.h first, then include this
 * file. This is the standard pattern for single-header STB-style libraries —
 * stb_image.h must only be instantiated once per link target.
 */
#pragma once

#include <osg/Node>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osg/CullFace>
#include <osg/Notify>
#include <osgDB/FileNameUtils>
#include <osgDB/ReadFile>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgUtil/SmoothingVisitor>

#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
#include <string>

// tiny_gltf.h is intentionally NOT included here — see file comment above.

#ifndef GL_SRGB8
#  define GL_SRGB8 0x8C41
#endif
#ifndef GL_SRGB8_ALPHA8
#  define GL_SRGB8_ALPHA8 0x8C43
#endif

// Change this to GLTF_NOTIFY or GLTF_NOTIFY to reduce verbosity.
#define GLTF_NOTIFY OSG_NOTICE

class GLTFReader
{
public:
    struct TextureCache
    {
        std::mutex                                               mutex;
        std::unordered_map<std::string, osg::ref_ptr<osg::Texture2D>> map;
    };

    struct Env
    {
        Env(const std::string& loc, const osgDB::Options* opt)
            : referrer(loc), readOptions(opt) {}
        std::string            referrer;
        const osgDB::Options*  readOptions;
    };

    mutable TextureCache* _texCache = nullptr;

    void setTextureCache(TextureCache* cache) const { _texCache = cache; }

    static std::string ExpandFilePath(const std::string& filepath, void* userData)
    {
        const std::string& referrer = *(const std::string*)userData;
        std::string path = osgDB::getRealPath(
            osgDB::isAbsolutePath(filepath) ? filepath :
            osgDB::concatPaths(osgDB::getFilePath(referrer), filepath));
        return tinygltf::ExpandFilePath(path, userData);
    }

    osgDB::ReaderWriter::ReadResult read(
        const std::string& location, bool isBinary,
        const osgDB::Options* readOptions) const
    {
        std::string err, warn;
        tinygltf::Model    model;
        tinygltf::TinyGLTF loader;

        tinygltf::FsCallbacks fs;
        fs.FileExists     = &tinygltf::FileExists;
        fs.ExpandFilePath = &GLTFReader::ExpandFilePath;
        fs.ReadWholeFile  = &tinygltf::ReadWholeFile;
        fs.WriteWholeFile = &tinygltf::WriteWholeFile;
        fs.user_data      = (void*)&location;
        loader.SetFsCallbacks(fs);

        GLTF_NOTIFY << "[GLTFReader] loading " << location << std::endl;
        bool ok = isBinary
            ? loader.LoadBinaryFromFile(&model, &err, &warn, location)
            : loader.LoadASCIIFromFile (&model, &err, &warn, location);

        if (!warn.empty())
            OSG_WARN << "[GLTFReader] " << location << ": " << warn << std::endl;
        if (!ok || !err.empty()) {
            OSG_WARN << "[GLTFReader] failed to load " << location << ": " << err << std::endl;
            return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
        }

        GLTF_NOTIFY << "[GLTFReader] parsed ok — "
                 << model.meshes.size()     << " mesh(es), "
                 << model.accessors.size()  << " accessor(s), "
                 << model.bufferViews.size()<< " bufferView(s), "
                 << model.buffers.size()    << " buffer(s), "
                 << model.images.size()     << " image(s)\n";

        Env env(location, readOptions);
        return makeNodeFromModel(model, env);
    }

    osg::Node* makeNodeFromModel(const tinygltf::Model& model, const Env& env) const
    {
        NodeBuilder builder(this, model, env);

        // glTF is Y-up; rotate to Z-up unless caller passes "gltfZUp"
        bool zUp = env.readOptions &&
            env.readOptions->getOptionString().find("gltfZUp") != std::string::npos;

        osg::MatrixTransform* root = new osg::MatrixTransform;
        if (!zUp)
            root->setMatrix(osg::Matrixd::rotate(
                osg::Vec3d(0, 1, 0), osg::Vec3d(0, 0, 1)));

        for (auto& scene : model.scenes)
            for (int idx : scene.nodes)
                if (osg::Node* n = builder.createNode(model.nodes[idx]))
                    root->addChild(n);

        root->getOrCreateStateSet()->setAttributeAndModes(
            new osg::CullFace(osg::CullFace::BACK), osg::StateAttribute::ON);

        return root;
    }

    // ------------------------------------------------------------------ //
    struct NodeBuilder
    {
        const GLTFReader*      reader;
        const tinygltf::Model& model;
        const Env&             env;
        std::vector<osg::ref_ptr<osg::Array>> arrays;

        NodeBuilder(const GLTFReader* r, const tinygltf::Model& m, const Env& e)
            : reader(r), model(m), env(e)
        {
            GLTF_NOTIFY << "[GLTFReader] extractArrays — " << m.accessors.size() << " accessor(s)\n";
            extractArrays();
            GLTF_NOTIFY << "[GLTFReader] extractArrays done — " << arrays.size() << " array(s) built\n";
        }

        // ---- node hierarchy ------------------------------------------ //
        osg::Node* createNode(const tinygltf::Node& node) const
        {
            GLTF_NOTIFY << "[GLTFReader] createNode '" << node.name << "'"
                     << " mesh=" << node.mesh
                     << " children=" << node.children.size() << std::endl;
            osg::MatrixTransform* mt = new osg::MatrixTransform;

            if (node.matrix.size() == 16)
            {
                osg::Matrixd mat;
                mat.set(node.matrix.data());
                mt->setMatrix(mat);
            }

            if (mt->getMatrix().isIdentity())
            {
                osg::Matrixd S, R, T;
                if (node.scale.size() == 3)
                    S = osg::Matrixd::scale(
                        node.scale[0], node.scale[1], node.scale[2]);
                if (node.rotation.size() == 4)
                    R.makeRotate(osg::Quat(
                        node.rotation[0], node.rotation[1],
                        node.rotation[2], node.rotation[3]));
                if (node.translation.size() == 3)
                    T = osg::Matrixd::translate(
                        node.translation[0], node.translation[1], node.translation[2]);
                mt->setMatrix(S * R * T);
            }

            if (node.mesh >= 0)
                mt->addChild(makeMesh(model.meshes[node.mesh]));

            for (int childIdx : node.children)
                if (osg::Node* c = createNode(model.nodes[childIdx]))
                    mt->addChild(c);

            mt->setName(node.name);
            return mt;
        }

        // ---- mesh → group of geometries ------------------------------ //
        osg::Group* makeMesh(const tinygltf::Mesh& mesh) const
        {
            GLTF_NOTIFY << "[GLTFReader] makeMesh '" << mesh.name
                     << "' — " << mesh.primitives.size() << " primitive(s)\n";
            osg::Group* group = new osg::Group;
            group->setName(mesh.name);

            int primIdx = 0;
            for (auto& primitive : mesh.primitives)
            {
                GLTF_NOTIFY << "[GLTFReader]   primitive[" << primIdx << "]"
                         << " mode=" << primitive.mode
                         << " indices=" << primitive.indices
                         << " material=" << primitive.material
                         << " attrs=" << primitive.attributes.size() << std::endl;

                osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;
                geom->setName(typeid(*this).name());
                geom->setUseVertexBufferObjects(true);

                osg::Vec4 baseColorFactor(1, 1, 1, 1);

                // vertex attributes — parsed before material application since
                // texture-unit binding needs to know which UV set (TEXCOORD_n)
                // each texture actually asks for.
                GLTF_NOTIFY << "[GLTFReader]     attributes:\n";
                std::map<int, osg::Array*> texCoordSets;
                for (auto& [attrName, accessorIdx] : primitive.attributes)
                {
                    bool valid = accessorIdx >= 0 && accessorIdx < (int)arrays.size()
                                 && arrays[accessorIdx].valid();
                    GLTF_NOTIFY << "[GLTFReader]       " << attrName
                             << " -> accessor[" << accessorIdx << "]"
                             << (valid ? " OK" : " NULL/INVALID") << std::endl;
                    if (!valid) continue;

                    if      (attrName == "POSITION")   geom->setVertexArray(arrays[accessorIdx].get());
                    else if (attrName == "NORMAL")     geom->setNormalArray(arrays[accessorIdx].get());
                    else if (attrName == "COLOR_0")    geom->setColorArray(arrays[accessorIdx].get());
                    else if (attrName == "TANGENT") {
                        arrays[accessorIdx]->setBinding(osg::Array::BIND_PER_VERTEX);
                        geom->setVertexAttribArray(7, arrays[accessorIdx].get());
                    }
                    else if (attrName.rfind("TEXCOORD_", 0) == 0) {
                        int uvSet = std::atoi(attrName.c_str() + 9);
                        texCoordSets[uvSet] = arrays[accessorIdx].get();
                    }
                }

                if (primitive.material >= 0 &&
                    primitive.material < (int)model.materials.size())
                {
                    GLTF_NOTIFY << "[GLTFReader]     applyMaterial " << primitive.material << std::endl;
                    applyMaterial(primitive.material, baseColorFactor, geom.get(), texCoordSets);
                }

                // fall-back solid color if no COLOR_0
                if (!geom->getColorArray())
                {
                    auto*  verts  = static_cast<osg::Vec3Array*>(geom->getVertexArray());
                    size_t count  = verts ? verts->size() : 1;
                    auto*  colors = new osg::Vec4Array(count);
                    std::fill(colors->begin(), colors->end(), baseColorFactor);
                    geom->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
                }

                // index primitive set — handles uint8, uint16, uint32
                if (primitive.indices >= 0 &&
                    primitive.indices < (int)arrays.size() &&
                    arrays[primitive.indices].valid())
                {
                    int glMode = primitiveMode(primitive.mode);
                    const tinygltf::Accessor& idxAcc = model.accessors[primitive.indices];

                    switch (idxAcc.componentType)
                    {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    {
                        auto* src = static_cast<osg::UByteArray*>(arrays[primitive.indices].get());
                        auto* de  = new osg::DrawElementsUByte(glMode, idxAcc.count);
                        std::copy(src->begin(), src->end(), de->begin());
                        geom->addPrimitiveSet(de);
                        break;
                    }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    {
                        auto* src = static_cast<osg::UShortArray*>(arrays[primitive.indices].get());
                        geom->addPrimitiveSet(
                            new osg::DrawElementsUShort(glMode, src->begin(), src->end()));
                        break;
                    }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    {
                        auto* src = static_cast<osg::UIntArray*>(arrays[primitive.indices].get());
                        geom->addPrimitiveSet(
                            new osg::DrawElementsUInt(glMode, src->begin(), src->end()));
                        break;
                    }
                    default:
                        OSG_WARN << "[GLTFReader] unsupported index component type "
                                 << idxAcc.componentType << std::endl;
                    }
                }
                else
                {
                    // non-indexed: draw all vertices
                    auto* verts = static_cast<osg::Vec3Array*>(geom->getVertexArray());
                    if (verts)
                        geom->addPrimitiveSet(new osg::DrawArrays(
                            primitiveMode(primitive.mode), 0, verts->size()));
                }

                // Auto-generate normals for triangle primitives that don't supply them.
                // SmoothingVisitor assumes triangles — never call it on points/lines.
                bool isTriangles = (primitive.mode == TINYGLTF_MODE_TRIANGLES ||
                                    primitive.mode == TINYGLTF_MODE_TRIANGLE_STRIP ||
                                    primitive.mode == TINYGLTF_MODE_TRIANGLE_FAN);
                bool skipNormals = env.readOptions &&
                    env.readOptions->getOptionString().find("gltfSkipNormals") != std::string::npos;
                osg::Geode* geode = new osg::Geode;
                geode->addDrawable(geom);

                if (isTriangles && !skipNormals && !geom->getNormalArray())
                {
                    GLTF_NOTIFY << "[GLTFReader]     generating normals via SmoothingVisitor\n";
                    osgUtil::SmoothingVisitor sv;
                    geode->accept(sv);
                }

                GLTF_NOTIFY << "[GLTFReader]     addChild geode to mesh group\n";
                group->addChild(geode);
                ++primIdx;
            }
            return group;
        }

        // ---- material ------------------------------------------------ //
        // Fixed-function multitexturing ties "which GL texture unit" to "which
        // TexCoordArray is bound to that unit" — so each texture channel gets a
        // fixed unit (base/diffuse=0, normal=1, MR/specGloss=2, emissive=3), but
        // the UV data bound to that unit must match what the texture actually
        // requests via textureInfo.texCoord, not just whatever happened to be
        // parsed as TEXCOORD_0/1.
        void applyMaterial(int matIdx, osg::Vec4& baseColorFactor,
                           osg::Geometry* geom,
                           const std::map<int, osg::Array*>& texCoordSets) const
        {
            const tinygltf::Material& mat = model.materials[matIdx];
            const auto& pbr = mat.pbrMetallicRoughness;

            // sRGB: per the glTF spec, baseColor/diffuse and emissive textures
            // are authored in sRGB gamma space; normal and ORM (occlusion/
            // roughness/metallic) textures are linear data, not color, and
            // must never be gamma-decoded.
            auto bindTexture = [&](int unit, int texIdx, int texCoord, bool sRGB)
            {
                osg::Texture2D* tex = getOrCreateTexture(texIdx, sRGB);
                if (!tex) return;
                geom->getOrCreateStateSet()->setTextureAttributeAndModes(unit, tex);
                auto it = texCoordSets.find(texCoord);
                if (it != texCoordSets.end())
                    geom->setTexCoordArray(unit, it->second);
            };

            if (pbr.baseColorFactor.size() == 4)
                baseColorFactor.set(pbr.baseColorFactor[0], pbr.baseColorFactor[1],
                                    pbr.baseColorFactor[2], pbr.baseColorFactor[3]);

            bool haveCoreBaseColor = pbr.baseColorTexture.index >= 0;

            if (haveCoreBaseColor)
                bindTexture(0, pbr.baseColorTexture.index, pbr.baseColorTexture.texCoord, true);

            if (mat.normalTexture.index >= 0)
                bindTexture(1, mat.normalTexture.index, mat.normalTexture.texCoord, false);

            // metallicRoughnessTexture and occlusionTexture are often the same
            // image (R=occlusion, G=roughness, B=metallic). Bind once to unit 2.
            if (pbr.metallicRoughnessTexture.index >= 0)
                bindTexture(2, pbr.metallicRoughnessTexture.index, pbr.metallicRoughnessTexture.texCoord, false);

            if (mat.emissiveTexture.index >= 0)
                bindTexture(3, mat.emissiveTexture.index, mat.emissiveTexture.texCoord, true);

            // KHR_materials_pbrSpecularGlossiness — legacy but still valid, real
            // Sketchfab-era content uses it. Only fills in the base-color slot
            // (diffuseTexture) and reuses the MR slot for specularGlossinessTexture
            // when the core pbrMetallicRoughness path didn't already supply a
            // base color texture. This does not attempt a full spec/gloss ->
            // metallic/roughness BRDF conversion — it just makes the diffuse
            // texture visible and UV-correct.
            if (!haveCoreBaseColor)
            {
                auto extIt = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
                if (extIt != mat.extensions.end())
                {
                    const tinygltf::Value& sg = extIt->second;

                    if (sg.Has("diffuseFactor"))
                    {
                        const tinygltf::Value& df = sg.Get("diffuseFactor");
                        if (df.IsArray() && df.ArrayLen() == 4)
                            baseColorFactor.set(
                                (float)df.Get(0).GetNumberAsDouble(),
                                (float)df.Get(1).GetNumberAsDouble(),
                                (float)df.Get(2).GetNumberAsDouble(),
                                (float)df.Get(3).GetNumberAsDouble());
                    }

                    if (sg.Has("diffuseTexture"))
                    {
                        const tinygltf::Value& dt = sg.Get("diffuseTexture");
                        int idx      = dt.Has("index")    ? dt.Get("index").GetNumberAsInt()    : -1;
                        int texCoord = dt.Has("texCoord")  ? dt.Get("texCoord").GetNumberAsInt() : 0;
                        if (idx >= 0)
                            bindTexture(0, idx, texCoord, true);
                    }

                    if (sg.Has("specularGlossinessTexture"))
                    {
                        const tinygltf::Value& sgt = sg.Get("specularGlossinessTexture");
                        int idx      = sgt.Has("index")   ? sgt.Get("index").GetNumberAsInt()   : -1;
                        int texCoord = sgt.Has("texCoord") ? sgt.Get("texCoord").GetNumberAsInt() : 0;
                        // RGB = specular color (sRGB per spec), A = glossiness
                        // (linear). GL_SRGB8_ALPHA8 decodes RGB only and always
                        // leaves alpha untouched, so this is correct for both
                        // channels at once.
                        if (idx >= 0)
                            bindTexture(2, idx, texCoord, true);
                    }
                }
            }

            // Export metallicFactor/roughnessFactor as uniforms for downstream
            // PBR shaders (e.g. pyosg-lighting/09-ibl.py) that sample the ORM
            // texture directly — tinygltf defaults both to 1.0 per spec even
            // when the glTF JSON omits pbrMetallicRoughness entirely, so this
            // is always a sane value.
            geom->getOrCreateStateSet()->addUniform(
                new osg::Uniform("metallicFactor", (float)pbr.metallicFactor));
            geom->getOrCreateStateSet()->addUniform(
                new osg::Uniform("roughnessFactor", (float)pbr.roughnessFactor));

            if (mat.alphaMode == "BLEND" || mat.alphaMode == "MASK")
            {
                geom->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
                geom->getOrCreateStateSet()->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
            }
        }

        // ---- texture ------------------------------------------------- //
        // sRGB must be known per-texture-*use*, not per-image-file: the same
        // image could in principle be referenced once as a color texture and
        // once as linear data, so the cache key includes the color-space flag
        // to avoid one use silently reusing the other's decode setting.
        osg::Texture2D* getOrCreateTexture(int texIdx, bool sRGB) const
        {
            if (texIdx < 0 || texIdx >= (int)model.textures.size())
                return nullptr;

            const tinygltf::Texture& tex = model.textures[texIdx];
            if (tex.source < 0 || tex.source >= (int)model.images.size())
                return nullptr;

            const tinygltf::Image& image = model.images[tex.source];
            bool embedded = image.image.size() > 0 ||
                            (!image.uri.empty() && tinygltf::IsDataURI(image.uri));

            // Cache key: resolved file path for external images, empty for embedded.
            std::string cacheKey;
            if (!embedded && !image.uri.empty())
                cacheKey = osgDB::getRealPath(osgDB::concatPaths(
                    osgDB::getFilePath(env.referrer), image.uri))
                    + (sRGB ? "|sRGB" : "|linear");

            TextureCache* tc = reader->_texCache;
            if (tc && !cacheKey.empty())
            {
                std::lock_guard<std::mutex> lk(tc->mutex);
                auto it = tc->map.find(cacheKey);
                if (it != tc->map.end())
                    return it->second.get();
            }

            osg::ref_ptr<osg::Image> img;

            if (image.image.size() > 0)
            {
                // Image data already decoded by tiny_gltf (embedded or preloaded).
                GLenum fmt  = (image.component == 4) ? GL_RGBA    : GL_RGB;
                GLenum ifmt = (image.component == 4) ? GL_RGBA8   : GL_RGB8;
                auto*  data = new unsigned char[image.image.size()];
                memcpy(data, image.image.data(), image.image.size());
                img = new osg::Image;
                img->setImage(image.width, image.height, 1,
                              ifmt, fmt, GL_UNSIGNED_BYTE, data,
                              osg::Image::USE_NEW_DELETE);
            }
            else if (!image.uri.empty() && !tinygltf::IsDataURI(image.uri))
            {
                std::string path = osgDB::concatPaths(
                    osgDB::getFilePath(env.referrer), image.uri);
                img = osgDB::readImageFile(path, env.readOptions);
                if (img.valid()) img->flipVertical();
            }

            if (!img.valid())
                return nullptr;

            if (img->getPixelFormat() == GL_RGB)
                img->setInternalTextureFormat(sRGB ? GL_SRGB8 : GL_RGB8);
            if (img->getPixelFormat() == GL_RGBA)
                img->setInternalTextureFormat(sRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8);

            osg::ref_ptr<osg::Texture2D> osgTex = new osg::Texture2D(img.get());
            osgTex->setResizeNonPowerOfTwoHint(false);
            osgTex->setDataVariance(osg::Object::STATIC);
            osgTex->setUnRefImageDataAfterApply(embedded);

            if (tex.sampler >= 0 && tex.sampler < (int)model.samplers.size())
            {
                const tinygltf::Sampler& s = model.samplers[tex.sampler];
                // Force mipmap min-filter regardless of what the sampler says,
                // since we don't generate mipmaps on load.
                osgTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
                osgTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
                osgTex->setWrap(osg::Texture::WRAP_S, (osg::Texture::WrapMode)s.wrapS);
                osgTex->setWrap(osg::Texture::WRAP_T, (osg::Texture::WrapMode)s.wrapT);
            }
            else
            {
                osgTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
                osgTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
                osgTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
                osgTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            }

            if (tc && !cacheKey.empty())
            {
                std::lock_guard<std::mutex> lk(tc->mutex);
                tc->map[cacheKey] = osgTex;
            }

            return osgTex.release();
        }

        static int primitiveMode(int gltfMode)
        {
            switch (gltfMode)
            {
            case TINYGLTF_MODE_POINTS:         return GL_POINTS;
            case TINYGLTF_MODE_LINE:           return GL_LINES;
            case TINYGLTF_MODE_LINE_LOOP:      return GL_LINE_LOOP;
            case TINYGLTF_MODE_LINE_STRIP:     return GL_LINE_STRIP;
            case TINYGLTF_MODE_TRIANGLES:      return GL_TRIANGLES;
            case TINYGLTF_MODE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
            case TINYGLTF_MODE_TRIANGLE_FAN:   return GL_TRIANGLE_FAN;
            default:                           return GL_TRIANGLES;
            }
        }

        // ---- array extraction ---------------------------------------- //
        // Parameterized on OSG array type + glTF component/accessor types
        // so the compiler can produce a single fast memcpy per combination.
        template<typename OSGArray, int ComponentType, int AccessorType>
        struct ArrayBuilder
        {
            static OSGArray* make(const tinygltf::Buffer&     buf,
                                  const tinygltf::BufferView& bv,
                                  const tinygltf::Accessor&   acc)
            {
                auto*       arr      = new OSGArray(acc.count);
                int32_t     compSize = tinygltf::GetComponentSizeInBytes(ComponentType);
                int32_t     numComp  = tinygltf::GetNumComponentsInType(AccessorType);
                const auto* src      = buf.data.data() + bv.byteOffset + acc.byteOffset;

                if (bv.byteStride == 0)
                    memcpy(&(*arr)[0], src, compSize * numComp * acc.count);
                else
                    for (size_t i = 0; i < acc.count; ++i, src += bv.byteStride)
                        memcpy(&(*arr)[i], src, compSize * numComp);

                return arr;
            }
        };

        void extractArrays()
        {
            int accIdx = 0;
            for (auto& acc : model.accessors)
            {
                GLTF_NOTIFY << "[GLTFReader]   accessor[" << accIdx << "]"
                         << " componentType=" << acc.componentType
                         << " type=" << acc.type
                         << " count=" << acc.count
                         << " bufferView=" << acc.bufferView << std::endl;

                // Accessors without a bufferView are valid (e.g. sparse base
                // data is implicitly zero). Push a null placeholder so indices
                // into the arrays vector stay in sync with accessor indices.
                if (acc.bufferView < 0 ||
                    acc.bufferView >= (int)model.bufferViews.size())
                {
                    GLTF_NOTIFY << "[GLTFReader]   -> no bufferView, skipping\n";
                    arrays.push_back({});
                    ++accIdx;
                    continue;
                }

                const auto& bv  = model.bufferViews[acc.bufferView];
                const auto& buf = model.buffers[bv.buffer];
                osg::ref_ptr<osg::Array> a;

#define MAKE(OsgT, Comp, Type) \
    ArrayBuilder<OsgT, Comp, Type>::make(buf, bv, acc)

                switch (acc.componentType)
                {
                case TINYGLTF_COMPONENT_TYPE_BYTE:
                    switch (acc.type) {
                    case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::ByteArray,  TINYGLTF_COMPONENT_TYPE_BYTE, TINYGLTF_TYPE_SCALAR); break;
                    case TINYGLTF_TYPE_VEC2:   a = MAKE(osg::Vec2bArray, TINYGLTF_COMPONENT_TYPE_BYTE, TINYGLTF_TYPE_VEC2);   break;
                    case TINYGLTF_TYPE_VEC3:   a = MAKE(osg::Vec3bArray, TINYGLTF_COMPONENT_TYPE_BYTE, TINYGLTF_TYPE_VEC3);   break;
                    case TINYGLTF_TYPE_VEC4:   a = MAKE(osg::Vec4bArray, TINYGLTF_COMPONENT_TYPE_BYTE, TINYGLTF_TYPE_VEC4);   break;
                    default: break; } break;

                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    switch (acc.type) {
                    case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::UByteArray,  TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, TINYGLTF_TYPE_SCALAR); break;
                    case TINYGLTF_TYPE_VEC2:   a = MAKE(osg::Vec2ubArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, TINYGLTF_TYPE_VEC2);   break;
                    case TINYGLTF_TYPE_VEC3:   a = MAKE(osg::Vec3ubArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, TINYGLTF_TYPE_VEC3);   break;
                    case TINYGLTF_TYPE_VEC4:   a = MAKE(osg::Vec4ubArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, TINYGLTF_TYPE_VEC4);   break;
                    default: break; } break;

                case TINYGLTF_COMPONENT_TYPE_SHORT:
                    switch (acc.type) {
                    case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::ShortArray,  TINYGLTF_COMPONENT_TYPE_SHORT, TINYGLTF_TYPE_SCALAR); break;
                    case TINYGLTF_TYPE_VEC2:   a = MAKE(osg::Vec2sArray,  TINYGLTF_COMPONENT_TYPE_SHORT, TINYGLTF_TYPE_VEC2);   break;
                    case TINYGLTF_TYPE_VEC3:   a = MAKE(osg::Vec3sArray,  TINYGLTF_COMPONENT_TYPE_SHORT, TINYGLTF_TYPE_VEC3);   break;
                    case TINYGLTF_TYPE_VEC4:   a = MAKE(osg::Vec4sArray,  TINYGLTF_COMPONENT_TYPE_SHORT, TINYGLTF_TYPE_VEC4);   break;
                    default: break; } break;

                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    switch (acc.type) {
                    case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::UShortArray,  TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_SCALAR); break;
                    case TINYGLTF_TYPE_VEC2:   a = MAKE(osg::Vec2usArray,  TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_VEC2);   break;
                    case TINYGLTF_TYPE_VEC3:   a = MAKE(osg::Vec3usArray,  TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_VEC3);   break;
                    case TINYGLTF_TYPE_VEC4:   a = MAKE(osg::Vec4usArray,  TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_VEC4);   break;
                    default: break; } break;

                case TINYGLTF_COMPONENT_TYPE_INT:
                    switch (acc.type) {
                    case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::IntArray,   TINYGLTF_COMPONENT_TYPE_INT, TINYGLTF_TYPE_SCALAR); break;
                    case TINYGLTF_TYPE_VEC2:   a = MAKE(osg::Vec2iArray, TINYGLTF_COMPONENT_TYPE_INT, TINYGLTF_TYPE_VEC2);   break;
                    case TINYGLTF_TYPE_VEC3:   a = MAKE(osg::Vec3iArray, TINYGLTF_COMPONENT_TYPE_INT, TINYGLTF_TYPE_VEC3);   break;
                    case TINYGLTF_TYPE_VEC4:   a = MAKE(osg::Vec4iArray, TINYGLTF_COMPONENT_TYPE_INT, TINYGLTF_TYPE_VEC4);   break;
                    default: break; } break;

                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    switch (acc.type) {
                    case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::UIntArray,   TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_SCALAR); break;
                    case TINYGLTF_TYPE_VEC2:   a = MAKE(osg::Vec2uiArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_VEC2);   break;
                    case TINYGLTF_TYPE_VEC3:   a = MAKE(osg::Vec3uiArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_VEC3);   break;
                    case TINYGLTF_TYPE_VEC4:   a = MAKE(osg::Vec4uiArray, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_VEC4);   break;
                    default: break; } break;

                case TINYGLTF_COMPONENT_TYPE_FLOAT:
                    switch (acc.type) {
                    case TINYGLTF_TYPE_SCALAR: a = MAKE(osg::FloatArray, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_SCALAR); break;
                    case TINYGLTF_TYPE_VEC2:   a = MAKE(osg::Vec2Array,  TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2);   break;
                    case TINYGLTF_TYPE_VEC3:   a = MAKE(osg::Vec3Array,  TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3);   break;
                    case TINYGLTF_TYPE_VEC4:   a = MAKE(osg::Vec4Array,  TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC4);   break;
                    default: break; } break;

                default:
                    GLTF_NOTIFY << "[GLTFReader] unknown component type "
                              << acc.componentType << std::endl;
                    break;
                }

#undef MAKE

                if (a.valid())
                {
                    a->setBinding(osg::Array::BIND_PER_VERTEX);
                    a->setNormalize(acc.normalized);
                    GLTF_NOTIFY << "[GLTFReader]   -> built array, " << a->getNumElements() << " element(s)\n";
                }
                else
                {
                    GLTF_NOTIFY << "[GLTFReader]   -> no array built (unhandled type combination)\n";
                }
                arrays.push_back(a);
                ++accIdx;
            }
        }
    };
};
