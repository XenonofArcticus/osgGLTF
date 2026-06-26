// ktx2-skybox — load a KTX2 cubemap and display it as a skybox.
//
// Usage: ktx2-skybox <file.ktx2>
//   +/-  or  ,/.  : step mip level up/down (verify mip chain)
//   scroll wheel   : also adjusts mip level

#include <osg/Geode>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Shape>
#include <osg/ShapeDrawable>
#include <osg/StateSet>
#include <osg/TextureCubeMap>
#include <osg/Uniform>
#include <osgDB/ReadFile>
#include <osgGA/GUIEventHandler>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

#include <cstdio>

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

static const char* SKYBOX_VERT = R"glsl(
#version 460 core
in vec4 osg_Vertex;
uniform mat4 osg_ModelViewProjectionMatrix;
out vec3 vDir;
void main() {
    vDir = osg_Vertex.xyz;
    gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;
}
)glsl";

static const char* SKYBOX_FRAG = R"glsl(
#version 460 core
uniform samplerCube envMap;
uniform float mipLevel;
in  vec3 vDir;
out vec4 fragColor;
void main() {
    vec3 color = textureLod(envMap, normalize(vDir), mipLevel).rgb;
    // Reinhard tone map + gamma — cubemap is linear HDR
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    fragColor = vec4(color, 1.0);
}
)glsl";

// ---------------------------------------------------------------------------
// Key handler: +/-  and scroll wheel change the displayed mip level
// ---------------------------------------------------------------------------

class MipHandler : public osgGA::GUIEventHandler
{
    osg::ref_ptr<osg::Uniform> _mipUniform;
    float _mipLevel  = 0.0f;
    int   _maxMip    = 0;

    void _clamp() { _mipLevel = std::max(0.0f, std::min(float(_maxMip), _mipLevel)); }
    void _apply()
    {
        _mipUniform->set(_mipLevel);
        printf("  mip level: %.1f / %d\n", _mipLevel, _maxMip);
    }

public:
    MipHandler(osg::Uniform* u, int maxMip) : _mipUniform(u), _maxMip(maxMip) {}

    bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&) override
    {
        if (ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN) {
            if (ea.getKey() == '+' || ea.getKey() == '=') { ++_mipLevel; _clamp(); _apply(); return true; }
            if (ea.getKey() == '-' || ea.getKey() == '_') { --_mipLevel; _clamp(); _apply(); return true; }
            if (ea.getKey() == '.' || ea.getKey() == '>') { _mipLevel += 0.25f; _clamp(); _apply(); return true; }
            if (ea.getKey() == ',' || ea.getKey() == '<') { _mipLevel -= 0.25f; _clamp(); _apply(); return true; }
        }
        if (ea.getEventType() == osgGA::GUIEventAdapter::SCROLL) {
            if (ea.getScrollingMotion() == osgGA::GUIEventAdapter::SCROLL_UP)
                { _mipLevel += 0.5f; _clamp(); _apply(); return true; }
            if (ea.getScrollingMotion() == osgGA::GUIEventAdapter::SCROLL_DOWN)
                { _mipLevel -= 0.5f; _clamp(); _apply(); return true; }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: ktx2-skybox <file.ktx2>\n"
                        "  +/-  or scroll: step through mip levels\n");
        return 1;
    }

    // Load the cubemap (osgdb_ktx2 plugin must be in OSG_LIBRARY_PATH)
    auto* obj = osgDB::readObjectFile(argv[1]);
    if (!obj) {
        fprintf(stderr, "ktx2-skybox: failed to load '%s'\n", argv[1]);
        return 1;
    }

    auto* texcm = dynamic_cast<osg::TextureCubeMap*>(obj);
    if (!texcm) {
        fprintf(stderr, "ktx2-skybox: '%s' is not a TextureCubeMap (got %s)\n",
            argv[1], obj->className());
        return 1;
    }

    // Count mip levels from face 0 image
    int maxMip = 0;
    if (auto* img = texcm->getImage(0)) {
        int numMips = (int)img->getNumMipmapLevels();
        maxMip = std::max(0, numMips - 1);
        printf("ktx2-skybox: loaded '%s'  %dx%d  %d mip levels\n",
            argv[1], img->s(), img->t(), numMips);
    } else {
        printf("ktx2-skybox: loaded '%s'  (no image data on face 0)\n", argv[1]);
    }

    texcm->setUnRefImageDataAfterApply(false);  // keep images for mip queries

    // Large box — camera starts inside; GL_CULL_FACE OFF to see inner faces
    auto* shape  = new osg::ShapeDrawable(new osg::Box(osg::Vec3(), 200.0f));
    auto* geode  = new osg::Geode();
    geode->addDrawable(shape);

    auto* prog = new osg::Program();
    prog->addShader(new osg::Shader(osg::Shader::VERTEX,   SKYBOX_VERT));
    prog->addShader(new osg::Shader(osg::Shader::FRAGMENT, SKYBOX_FRAG));

    auto* mipUniform = new osg::Uniform("mipLevel", 0.0f);

    auto* ss = geode->getOrCreateStateSet();
    ss->setMode(GL_CULL_FACE,                  osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    ss->setMode(GL_DEPTH_TEST,                 osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    ss->setMode(GL_TEXTURE_CUBE_MAP_SEAMLESS,  osg::StateAttribute::ON);
    ss->setAttributeAndModes(prog, osg::StateAttribute::ON);
    ss->setTextureAttributeAndModes(0, texcm, osg::StateAttribute::ON);
    ss->addUniform(new osg::Uniform("envMap", 0));
    ss->addUniform(mipUniform);

    osgViewer::Viewer viewer;
    viewer.setSceneData(geode);
    viewer.addEventHandler(new osgViewer::StatsHandler());
    viewer.addEventHandler(new MipHandler(mipUniform, maxMip));

    printf("Controls: +/- or scroll to step mip levels (0 = sharpest, %d = roughest)\n", maxMip);

    viewer.run();
    return 0;
}
