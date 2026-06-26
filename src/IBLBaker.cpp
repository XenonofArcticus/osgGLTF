#include "osgGLTF/IBLBaker.hpp"

#include <osg/GL>
#include <osg/Camera>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Notify>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Viewport>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgViewer/Viewer>

#include <algorithm>
#include <cstdlib>

#ifndef GL_RGB16F
#  define GL_RGB16F 0x881B
#endif
#ifndef GL_HALF_FLOAT
#  define GL_HALF_FLOAT 0x140B
#endif
#ifndef GL_COLOR_BUFFER_BIT
#  define GL_COLOR_BUFFER_BIT 0x00004000
#endif

namespace osgGLTF
{
namespace
{

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

osg::ref_ptr<osg::Geode> makeFullscreenQuad()
{
    auto* quad = osg::createTexturedQuadGeometry(
        osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0));
    auto* geode = new osg::Geode();
    geode->addDrawable(quad);
    return geode;
}

osg::ref_ptr<osg::Program> makeProgram(const char* vert, const char* frag)
{
    auto* p = new osg::Program();
    p->addShader(new osg::Shader(osg::Shader::VERTEX, vert));
    p->addShader(new osg::Shader(osg::Shader::FRAGMENT, frag));
    return p;
}

int mipCountForSize(int size)
{
    int count = 0;
    for (int s = size; s >= 1; s >>= 1) ++count;
    return std::max(1, count);
}

osg::ref_ptr<osg::TextureCubeMap> makePrefilterBake(
    osg::Texture2D* srcEquirect, osg::Group* root, int prefilterSize, int eqW, int eqH)
{
    const int numMips = mipCountForSize(prefilterSize);

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

    auto prog = makeProgram(FULLSCREEN_VERT, PREFILTER_FRAG);
    auto quad = makeFullscreenQuad();
    auto* group = new osg::Group();

    for (int mip = 0; mip < numMips; ++mip) {
        const float roughness = (numMips > 1) ? float(mip) / float(numMips - 1) : 0.0f;
        const int mipSize = std::max(1, prefilterSize >> mip);

        for (int face = 0; face < 6; ++face) {
            auto* cam = new osg::Camera();
            cam->setName("osgGLTF_Prefilter_m" + std::to_string(mip) + "_f" + std::to_string(face));
            cam->setRenderOrder(osg::Camera::PRE_RENDER, 1);
            cam->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
            cam->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
            cam->setClearMask(GL_COLOR_BUFFER_BIT);
            cam->setViewport(0, 0, mipSize, mipSize);
            cam->setProjectionMatrix(osg::Matrix::identity());
            cam->setViewMatrix(osg::Matrix::identity());
            cam->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
            cam->setCullingMode(osg::Camera::NO_CULLING);
            cam->attach(osg::Camera::COLOR_BUFFER0, prefilterTex, mip, face, false);

            auto* ss = cam->getOrCreateStateSet();
            ss->setAttributeAndModes(prog.get());
            ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            ss->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            ss->addUniform(new osg::Uniform("equirectTex", 0));
            ss->addUniform(new osg::Uniform("faceIndex", face));
            ss->addUniform(new osg::Uniform("roughness", roughness));
            ss->addUniform(new osg::Uniform("equirectWidth", eqW));
            ss->addUniform(new osg::Uniform("equirectHeight", eqH));
            ss->setTextureAttributeAndModes(0, srcEquirect, osg::StateAttribute::ON);

            cam->addChild(quad.get());
            group->addChild(cam);
        }
    }

    root->addChild(group);
    return prefilterTex;
}

class ReadbackCallback : public osg::Camera::DrawCallback
{
public:
    ReadbackCallback(osg::TextureCubeMap* tex, int size, int mips, int trigger)
        : srcTex(tex), prefilterSize(size), numMips(mips), triggerFrame(trigger)
    {
        result = new osg::TextureCubeMap();
    }

    void operator()(osg::RenderInfo& ri) const override
    {
        auto* self = const_cast<ReadbackCallback*>(this);
        if (self->done) return;
        if (++self->frameCount < triggerFrame) return;

        auto* texObj = srcTex->getTextureObject(ri.getContextID());
        if (!texObj) {
            OSG_WARN << "osgGLTF: prefilter texture not on GPU yet; retrying next frame\n";
            return;
        }

        glFinish();
        glBindTexture(GL_TEXTURE_CUBE_MAP, texObj->id());

        GLint prevAlign = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlign);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        for (int face = 0; face < 6; ++face) {
            size_t totalBytes = 0;
            for (int mip = 0; mip < numMips; ++mip) {
                int s = std::max(1, prefilterSize >> mip);
                totalBytes += size_t(s) * size_t(s) * 3u * 2u;
            }

            auto* buf = new unsigned char[totalBytes];
            osg::Image::MipmapDataType offsets;
            size_t off = 0;

            for (int mip = 0; mip < numMips; ++mip) {
                int s = std::max(1, prefilterSize >> mip);
                size_t mipBytes = size_t(s) * size_t(s) * 3u * 2u;
                if (mip > 0) offsets.push_back(static_cast<unsigned int>(off));

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
    }

    osg::TextureCubeMap* srcTex = nullptr;
    int prefilterSize = 0;
    int numMips = 0;
    int triggerFrame = 0;
    mutable int frameCount = 0;
    osg::ref_ptr<osg::TextureCubeMap> result;
    bool done = false;
};

void configureGLContext()
{
#if defined(_WIN32)
    _putenv("OSG_GL_CONTEXT_PROFILE_MASK=1");
    _putenv("OSG_GL_VERSION=4.6");
    _putenv("OSG_GL_CONTEXT_VERSION=4.6");
    _putenv("OSG_THREADING=SingleThreaded");
#else
    setenv("OSG_GL_CONTEXT_PROFILE_MASK", "1", 1);
    setenv("OSG_GL_VERSION", "4.6", 1);
    setenv("OSG_GL_CONTEXT_VERSION", "4.6", 1);
    setenv("OSG_THREADING", "SingleThreaded", 1);
#endif
}

} // namespace

osg::ref_ptr<osg::TextureCubeMap> bakeSpecularIBL(osg::Image* equirectImage,
                                                  const IBLBakeOptions& options)
{
    if (!equirectImage) return nullptr;

    const int prefilterSize = std::max(1, options.prefilterSize);
    const int numMips = mipCountForSize(prefilterSize);

    if (options.configureGLContext)
        configureGLContext();

    auto* envTex = new osg::Texture2D();
    envTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
    envTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    envTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    envTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    envTex->setInternalFormat(GL_RGB16F);
    envTex->setResizeNonPowerOfTwoHint(false);
    envTex->setImage(equirectImage);

    auto* root = new osg::Group();
    auto prefilterTex = makePrefilterBake(envTex, root, prefilterSize,
                                          equirectImage->s(), equirectImage->t());

    auto* readback = new ReadbackCallback(prefilterTex.get(), prefilterSize, numMips,
                                          std::max(1, options.readbackFrame));

    osgViewer::Viewer viewer;
    viewer.setUpViewInWindow(0, 0, 128, 128);
    viewer.setSceneData(root);
    viewer.getCamera()->setPostDrawCallback(readback);

    const int maxFrames = std::max(1, options.maxFrames);
    for (int frame = 0; frame < maxFrames && !readback->done; ++frame)
        viewer.frame();

    if (!readback->done) {
        OSG_WARN << "osgGLTF: IBL bake readback did not complete\n";
        return nullptr;
    }

    readback->result->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
    readback->result->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    readback->result->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    readback->result->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    readback->result->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
    return readback->result;
}

osg::ref_ptr<osg::TextureCubeMap> bakeSpecularIBL(const std::string& inputPath,
                                                  const IBLBakeOptions& options)
{
    osg::ref_ptr<osg::Image> image = osgDB::readImageFile(inputPath);
    if (!image) {
        OSG_WARN << "osgGLTF: failed to load HDR image " << inputPath << "\n";
        return nullptr;
    }
    return bakeSpecularIBL(image.get(), options);
}

bool bakeSpecularIBLToKTX2(const std::string& inputPath,
                           const std::string& outputPath,
                           const IBLBakeOptions& options)
{
    osg::ref_ptr<osg::TextureCubeMap> result = bakeSpecularIBL(inputPath, options);
    if (!result) return false;
    return osgDB::writeObjectFile(*result, outputPath);
}

} // namespace osgGLTF
