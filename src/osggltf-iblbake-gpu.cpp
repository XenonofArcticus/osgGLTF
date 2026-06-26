// ibl-bake — bake an equirectangular HDR into a GGX-prefiltered KTX2 cubemap.
//
// Usage: ibl-bake <input.hdr> <output.ktx2> [--cube-size N] [--prefilter-size N]
//
// Renders via OSG offscreen FBO (same pipeline as pyosg-lighting-9-ibl-dynamicfbo.py):
//   1. Equirect HDR → TextureCubeMap (6 PRE_RENDER cameras)
//   2. TextureCubeMap → GGX-prefiltered cubemap (48 PRE_RENDER cameras for 128px/8mips)
//
// After 2 frames of rendering, a post-draw callback reads back every face×mip
// from the GPU via glGetTexImage, then osgDB::writeObjectFile saves as KTX2.

#include <osg/GL>
#include <osg/Camera>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Texture2D>
#include <osg/TextureCubeMap>
#include <osg/Viewport>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgViewer/Viewer>

#include <cstring>
#include <memory>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <algorithm>

#ifndef GL_RGB16F
#  define GL_RGB16F  0x881B
#endif
#ifndef GL_HALF_FLOAT
#  define GL_HALF_FLOAT 0x140B
#endif
#ifndef GL_COLOR_BUFFER_BIT
#  define GL_COLOR_BUFFER_BIT 0x00004000
#endif
#ifndef GL_DEPTH_BUFFER_BIT
#  define GL_DEPTH_BUFFER_BIT 0x00000100
#endif

// ---------------------------------------------------------------------------
// Shaders  (straight port from pyosg-lighting-9-ibl-dynamicfbo.py)
// ---------------------------------------------------------------------------

static const char* FULLSCREEN_VERT = R"glsl(
#version 460 core
in vec4 osg_Vertex;
in vec2 osg_MultiTexCoord0;
out vec2 vUV;
void main() {
    vUV = osg_MultiTexCoord0;
    gl_Position = vec4(osg_Vertex.xy, 0.0, 1.0);
}
)glsl";

static const char* CUBE_BAKE_FRAG = R"glsl(
#version 460 core
const float PI = 3.14159265359;
uniform sampler2D equirectTex;
uniform int faceIndex;
in vec2 vUV;
out vec4 fragColor;

vec2 equirect_uv(vec3 dir) {
    vec3 d      = vec3(dir.x, dir.z, -dir.y);
    float phi   = atan(d.z, d.x) - PI / 2.0;
    float theta = acos(clamp(d.y, -1.0, 1.0));
    return vec2(mod(phi / (2.0 * PI) + 0.5, 1.0), 1.0 - theta / PI);
}

void main() {
    vec2 uv = vUV * 2.0 - 1.0;
    vec3 dir;
    if      (faceIndex == 0) dir = normalize(vec3( 1.0, -uv.y, -uv.x));
    else if (faceIndex == 1) dir = normalize(vec3(-1.0, -uv.y,  uv.x));
    else if (faceIndex == 2) dir = normalize(vec3( uv.x,  1.0,  uv.y));
    else if (faceIndex == 3) dir = normalize(vec3( uv.x, -1.0, -uv.y));
    else if (faceIndex == 4) dir = normalize(vec3( uv.x, -uv.y,  1.0));
    else                     dir = normalize(vec3(-uv.x, -uv.y, -1.0));
    vec3 dir_zup = vec3(dir.x, -dir.z, dir.y);
    fragColor = vec4(texture(equirectTex, equirect_uv(dir_zup)).rgb, 1.0);
}
)glsl";

static const char* PREFILTER_FRAG = R"glsl(
#version 460 core
const float PI = 3.14159265359;
uniform sampler2D equirectTex;
uniform int   faceIndex;
uniform float roughness;
uniform int   equirectWidth;
uniform int   equirectHeight;
in vec2 vUV;
out vec4 fragColor;

float RadicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint N) { return vec2(float(i)/float(N), RadicalInverse_VdC(i)); }

vec3 importanceSampleGGX(vec2 Xi, vec3 N, float rough) {
    float a        = rough * rough;
    float phi      = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    vec3 H         = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    vec3 up        = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 T         = normalize(cross(up, N));
    vec3 B         = cross(N, T);
    return normalize(T * H.x + B * H.y + N * H.z);
}

// GL Y-up → Z-up (same convention as CUBE_BAKE_FRAG)
vec3 dir_gl_to_zup(vec3 d) { return vec3(d.x, -d.z, d.y); }

vec2 equirect_uv(vec3 dir_zup) {
    vec3 d      = vec3(dir_zup.x, dir_zup.z, -dir_zup.y);
    float phi   = atan(d.z, d.x) - PI / 2.0;
    float theta = acos(clamp(d.y, -1.0, 1.0));
    return vec2(mod(phi / (2.0 * PI) + 0.5, 1.0), 1.0 - theta / PI);
}

void main() {
    const uint NUM_SAMPLES = 1024u;
    vec2 uv = vUV * 2.0 - 1.0;
    vec3 N;
    if      (faceIndex == 0) N = normalize(vec3( 1.0, -uv.y, -uv.x));
    else if (faceIndex == 1) N = normalize(vec3(-1.0, -uv.y,  uv.x));
    else if (faceIndex == 2) N = normalize(vec3( uv.x,  1.0,  uv.y));
    else if (faceIndex == 3) N = normalize(vec3( uv.x, -1.0, -uv.y));
    else if (faceIndex == 4) N = normalize(vec3( uv.x, -uv.y,  1.0));
    else                     N = normalize(vec3(-uv.x, -uv.y, -1.0));

    vec3 V = N;
    vec3 prefilteredColor = vec3(0.0);
    float totalWeight = 0.0;

    // Average solid angle per equirect pixel: 4π total sphere / (W × H pixels)
    float saTexel = 4.0 * PI / float(equirectWidth * equirectHeight);

    for (uint i = 0u; i < NUM_SAMPLES; ++i) {
        vec2 Xi = hammersley(i, NUM_SAMPLES);
        vec3 H  = importanceSampleGGX(Xi, N, roughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            float NdotH = max(dot(N, H), 0.0);
            float VdotH = max(dot(V, H), 0.0);
            float a     = roughness * roughness;
            float a2    = a * a;
            float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
            float D     = a2 / (PI * denom * denom);
            float pdf   = max(D * NdotH / (4.0 * VdotH), 0.0001);
            float saSample = 1.0 / (float(NUM_SAMPLES) * pdf);
            float mip   = roughness == 0.0 ? 0.0 : max(0.5 * log2(saSample / saTexel), 0.0);
            vec2 eqUV   = equirect_uv(dir_gl_to_zup(L));
            prefilteredColor += textureLod(equirectTex, eqUV, mip).rgb * NdotL;
            totalWeight += NdotL;
        }
    }
    fragColor = vec4(prefilteredColor / max(totalWeight, 0.001), 1.0);
}
)glsl";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static osg::ref_ptr<osg::Geode> makeFullscreenQuad()
{
    auto* quad = osg::createTexturedQuadGeometry(
        osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0));
    auto* geode = new osg::Geode();
    geode->addDrawable(quad);
    return geode;
}

static osg::ref_ptr<osg::Program> makeProgram(const char* vert, const char* frag)
{
    auto* p = new osg::Program();
    p->addShader(new osg::Shader(osg::Shader::VERTEX,   vert));
    p->addShader(new osg::Shader(osg::Shader::FRAGMENT, frag));
    return p;
}

// ---------------------------------------------------------------------------
// makeCubemapBake — equirect Texture2D → TextureCubeMap (6 PRE_RENDER cameras)
// ---------------------------------------------------------------------------

static osg::ref_ptr<osg::TextureCubeMap> makeCubemapBake(
    osg::Texture2D* envTex, osg::Group* root, int cubeSize = 512)
{
    auto* cubeTex = new osg::TextureCubeMap();
    cubeTex->setDataVariance(osg::Object::DYNAMIC);
    cubeTex->setTextureSize(cubeSize, cubeSize);
    cubeTex->setInternalFormat(GL_RGB16F);
    cubeTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
    cubeTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    int cubeNumMips = 0; for (int s = cubeSize; s >= 1; s >>= 1) ++cubeNumMips;
    cubeTex->setNumMipmapLevels(cubeNumMips);
    cubeTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    cubeTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    cubeTex->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);

    auto prog   = makeProgram(FULLSCREEN_VERT, CUBE_BAKE_FRAG);
    auto quad   = makeFullscreenQuad();
    auto* group = new osg::Group();

    for (int f = 0; f < 6; ++f) {
        auto* cam = new osg::Camera();
        cam->setName("CubeBake" + std::to_string(f));
        cam->setRenderOrder(osg::Camera::PRE_RENDER, 0);
        cam->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        cam->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        cam->setClearMask(GL_COLOR_BUFFER_BIT);   // no depth attachment → don't clear depth
        cam->setViewport(0, 0, cubeSize, cubeSize);
        cam->setProjectionMatrix(osg::Matrix::identity());
        cam->setViewMatrix(osg::Matrix::identity());
        cam->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
        cam->setCullingMode(osg::Camera::NO_CULLING);
        cam->attach(osg::Camera::COLOR_BUFFER0, cubeTex, 0, f, false);

        auto* ss = cam->getOrCreateStateSet();
        ss->setAttributeAndModes(prog.get());
        ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        ss->setMode(GL_CULL_FACE,  osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        ss->addUniform(new osg::Uniform("equirectTex", 0));
        ss->addUniform(new osg::Uniform("faceIndex", f));
        ss->setTextureAttributeAndModes(0, envTex, osg::StateAttribute::ON);

        // After the last face renders (and before prefilter cameras start at order=1),
        // generate mipmaps from the fully-baked cube so PREFILTER_FRAG can use textureLod.
        if (f == 5) {
            struct MipGenCB : public osg::Camera::DrawCallback {
                osg::ref_ptr<osg::TextureCubeMap> tex;
                MipGenCB(osg::TextureCubeMap* t) : tex(t) {}
                void operator()(osg::RenderInfo& ri) const override {
                    osg::Texture::TextureObject* obj = tex->getTextureObject(ri.getContextID());
                    if (!obj) return;
                    glBindTexture(GL_TEXTURE_CUBE_MAP, obj->id());
                    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
                    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
                }
            };
            cam->setPostDrawCallback(new MipGenCB(cubeTex));
        }

        cam->addChild(quad.get());
        group->addChild(cam);
    }

    root->addChild(group);
    return cubeTex;
}

// ---------------------------------------------------------------------------
// makePrefilterBake — TextureCubeMap → GGX-prefiltered cubemap (mips × 6 faces)
// ---------------------------------------------------------------------------

static osg::ref_ptr<osg::TextureCubeMap> makePrefilterBake(
    osg::Texture2D* srcEquirect, osg::Group* root, int prefilterSize = 128, int eqW = 3200, int eqH = 1600)
{
    int numMips = 0;
    for (int s = prefilterSize; s >= 1; s >>= 1) ++numMips;

    auto* prefilterTex = new osg::TextureCubeMap();
    prefilterTex->setDataVariance(osg::Object::DYNAMIC);
    prefilterTex->setTextureSize(prefilterSize, prefilterSize);
    prefilterTex->setInternalFormat(GL_RGB16F);
    prefilterTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
    prefilterTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    prefilterTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    prefilterTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    prefilterTex->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
    prefilterTex->setUseHardwareMipMapGeneration(false);
    prefilterTex->setNumMipmapLevels(numMips);

    auto prog   = makeProgram(FULLSCREEN_VERT, PREFILTER_FRAG);
    auto quad   = makeFullscreenQuad();
    auto* group = new osg::Group();

    for (int mip = 0; mip < numMips; ++mip) {
        float roughness = (numMips > 1) ? float(mip) / float(numMips - 1) : 0.0f;
        int   mipSize   = std::max(1, prefilterSize >> mip);

        for (int f = 0; f < 6; ++f) {
            auto* cam = new osg::Camera();
            cam->setName("Prefilter_m" + std::to_string(mip) + "_f" + std::to_string(f));
            cam->setRenderOrder(osg::Camera::PRE_RENDER, 1);
            cam->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
            cam->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
            cam->setClearMask(GL_COLOR_BUFFER_BIT);
            cam->setViewport(0, 0, mipSize, mipSize);
            cam->setProjectionMatrix(osg::Matrix::identity());
            cam->setViewMatrix(osg::Matrix::identity());
            cam->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
            cam->setCullingMode(osg::Camera::NO_CULLING);
            cam->attach(osg::Camera::COLOR_BUFFER0, prefilterTex, mip, f, false);

            auto* ss = cam->getOrCreateStateSet();
            ss->setAttributeAndModes(prog.get());
            ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            ss->setMode(GL_CULL_FACE,  osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            ss->addUniform(new osg::Uniform("equirectTex",    0));
            ss->addUniform(new osg::Uniform("faceIndex",      f));
            ss->addUniform(new osg::Uniform("roughness",      roughness));
            ss->addUniform(new osg::Uniform("equirectWidth",  eqW));
            ss->addUniform(new osg::Uniform("equirectHeight", eqH));
            ss->setTextureAttributeAndModes(0, srcEquirect, osg::StateAttribute::ON);

            cam->addChild(quad.get());
            group->addChild(cam);
        }
    }

    root->addChild(group);
    return prefilterTex;
}

// ---------------------------------------------------------------------------
// ReadbackCallback — reads prefilter_tex from GPU after bake is done.
// Attach as postDrawCallback on the main camera; fires every frame but only
// acts once (on frame >= triggerFrame).
// ---------------------------------------------------------------------------

class ReadbackCallback : public osg::Camera::DrawCallback
{
public:
    osg::TextureCubeMap* srcTex;
    int prefilterSize;
    int numMips;
    int triggerFrame;   // don't read back until this frame

    // Output: CPU-side face images populated by operator()
    osg::ref_ptr<osg::TextureCubeMap> result;
    bool done = false;

    ReadbackCallback(osg::TextureCubeMap* tex, int size, int mips, int trigger)
        : srcTex(tex), prefilterSize(size), numMips(mips), triggerFrame(trigger)
    {
        result = new osg::TextureCubeMap();
    }

    void operator()(osg::RenderInfo& ri) const override
    {
        auto* self = const_cast<ReadbackCallback*>(this);
        if (self->done) return;

        static int frameCount = 0;
        if (++frameCount < triggerFrame) return;

        auto* texObj = srcTex->getTextureObject(ri.getContextID());
        if (!texObj) {
            OSG_WARN << "ibl-bake: prefilter_tex not on GPU yet — retrying next frame\n";
            return;
        }

        glFinish();
        glBindTexture(GL_TEXTURE_CUBE_MAP, texObj->id());

        // GL_PACK_ALIGNMENT=4 by default; RGB16F rows with odd pixel counts (e.g. 1×1 = 6 bytes)
        // are not 4-byte aligned → driver writes 8 bytes into a 6-byte buffer → overrun.
        GLint prevAlign = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlign);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        for (int face = 0; face < 6; ++face) {
            // Compute total CPU buffer for this face across all mips
            size_t totalBytes = 0;
            for (int mip = 0; mip < numMips; ++mip) {
                int s = std::max(1, prefilterSize >> mip);
                totalBytes += (size_t)s * s * 3 * 2;   // RGB × fp16
            }

            auto* buf = new unsigned char[totalBytes];
            osg::Image::MipmapDataType offsets;
            size_t off = 0;

            for (int mip = 0; mip < numMips; ++mip) {
                int s = std::max(1, prefilterSize >> mip);
                size_t mipBytes = (size_t)s * s * 3 * 2;
                if (mip > 0) offsets.push_back((unsigned int)off);

                glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, mip,
                    GL_RGB, GL_HALF_FLOAT, buf + off);
                off += mipBytes;
            }

            auto* img = new osg::Image();
            img->setImage(prefilterSize, prefilterSize, 1,
                GL_RGB16F, GL_RGB, GL_HALF_FLOAT,
                buf, osg::Image::USE_NEW_DELETE);
            if (!offsets.empty()) img->setMipmapLevels(offsets);
            self->result->setImage(face, img);
        }

        glPixelStorei(GL_PACK_ALIGNMENT, prevAlign);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        self->done = true;
        OSG_NOTICE << "ibl-bake: readback complete\n";
    }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    if (argc < 3) {
        fprintf(stderr,
            "Usage: ibl-bake <input.hdr> <output.ktx2> [--cube-size N] [--prefilter-size N]\n");
        return 1;
    }

    std::string hdrPath    = argv[1];
    std::string outputPath = argv[2];
    int cubeSize           = 512;
    int prefilterSize      = 128;

    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--cube-size"      && i+1 < argc) cubeSize      = std::atoi(argv[++i]);
        if (std::string(argv[i]) == "--prefilter-size" && i+1 < argc) prefilterSize = std::atoi(argv[++i]);
    }

#if defined(_WIN32)
    _putenv("OSG_GL_CONTEXT_PROFILE_MASK=1");
    _putenv("OSG_GL_VERSION=4.6");
    _putenv("OSG_GL_CONTEXT_VERSION=4.6");
    _putenv("OSG_THREADING=SingleThreaded");
#else
    setenv("OSG_GL_CONTEXT_PROFILE_MASK", "1",            1);
    setenv("OSG_GL_VERSION",              "4.6",          1);
    setenv("OSG_GL_CONTEXT_VERSION",      "4.6",          1);
    setenv("OSG_THREADING",               "SingleThreaded",1);
#endif

    osg::setNotifyLevel(osg::NOTICE);

    // Load HDR
    osg::ref_ptr<osg::Image> hdrImg = osgDB::readImageFile(hdrPath);
    if (!hdrImg) {
        fprintf(stderr, "ibl-bake: failed to load HDR: %s\n", hdrPath.c_str());
        return 1;
    }
    OSG_NOTICE << "ibl-bake: loaded " << hdrPath
               << "  " << hdrImg->s() << "×" << hdrImg->t() << "\n";

    // Equirect texture
    auto* envTex = new osg::Texture2D();
    envTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
    envTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    envTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    envTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    envTex->setInternalFormat(GL_RGB16F);
    envTex->setResizeNonPowerOfTwoHint(false);
    envTex->setImage(hdrImg.get());

    // Build bake scene — prefilter samples equirect directly (no intermediate cube)
    auto* root = new osg::Group();
    auto prefilterTex = makePrefilterBake(envTex, root, prefilterSize,
                                          hdrImg->s(), hdrImg->t());

    // Compute num mips (same formula as makePrefilterBake)
    int numMips = 0;
    for (int s = prefilterSize; s >= 1; s >>= 1) ++numMips;

    // Readback fires on frame 2+: frame 0 uploads equirect + generates its mips,
    // frame 1 is the first prefilter render with correct mip-sampled equirect.
    auto* readback = new ReadbackCallback(prefilterTex.get(), prefilterSize, numMips, 2);

    // Small window — just enough for a GL4 context
    osgViewer::Viewer viewer;
    viewer.setUpViewInWindow(0, 0, 128, 128);
    viewer.setSceneData(root);
    viewer.getCamera()->setPostDrawCallback(readback);

    // Run until readback is done (≤ 4 frames in practice)
    for (int frame = 0; frame < 8 && !readback->done; ++frame)
        viewer.frame();

    if (!readback->done) {
        fprintf(stderr, "ibl-bake: readback never completed\n");
        return 1;
    }

    // Set filter/wrap on the CPU-side result before writing
    readback->result->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
    readback->result->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    readback->result->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    readback->result->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    readback->result->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);

    if (!osgDB::writeObjectFile(*readback->result, outputPath)) {
        fprintf(stderr, "ibl-bake: failed to write %s\n", outputPath.c_str());
        return 1;
    }

    OSG_NOTICE << "ibl-bake: wrote " << outputPath << "\n";
    return 0;
}
