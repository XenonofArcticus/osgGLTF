#pragma once

#include <osg/Camera>
#include <osg/Group>
#include <osg/TextureCubeMap>

namespace osg { class Image; }

namespace osgGLTF {

// TODO: If these can't be NEGATIVE, they need to be OTHER TYPES than `int`!
struct IBLBakeOptions {
	int prefilterSize = 128;
	int maxFrames = 8;
	int readbackFrame = 2;
	// bool configureGLContext = true;

	// If true, IBLReadback calls glFinish() immediately before reading the
	// baked cubemap back from the GPU (deterministic, but stalls the
	// pipeline). If false, it trusts that `readbackFrame` frames having
	// already elapsed is enough for the GPU to have caught up, and skips
	// the stall. TODO: no caller flips this yet; it exists so a future
	// best-effort/async bake mode can do so without touching IBLReadback.
	bool syncReadback = true;
};

// Sets the OSG_GL_* / OSG_THREADING environment variables so that a graphics
// context created afterward (e.g. by an osgViewer::Viewer) is compatible with
// the GLSL 4.60 prefilter shaders. Must be called before that context exists.
// void configureIBLGLContext();

// Post-draw callback that, once attached to a rendering camera, waits until
// `triggerFrame` frames have been rendered and then reads the prefiltered
// cubemap back from the GPU into `getResult()`.
class IBLReadback: public osg::Camera::DrawCallback {
public:
	IBLReadback(osg::TextureCubeMap* srcTex, int triggerFrame, bool sync);

	void operator()(osg::RenderInfo& ri) const override;

	bool isDone() const { return done; }
	osg::TextureCubeMap* getResult() const { return result.get(); }

private:
	osg::TextureCubeMap* srcTex = nullptr;
	int triggerFrame = 0;
	bool sync = true;
	mutable int frameCount = 0;
	osg::ref_ptr<osg::TextureCubeMap> result;
	bool done = false;
};

struct IBLBakeScene {
	osg::ref_ptr<osg::Group> root;
	osg::ref_ptr<IBLReadback> readback;
};

// Builds the offscreen scene graph (PRE_RENDER cameras, one per cubemap
// face/mip) that GGX-prefilters `equirectImage`. Does not render anything:
// the caller owns the graphics context/viewer, is responsible for setting
// `root` as scene data, attaching `readback` as a post-draw callback on the
// camera that will actually render frames, and running frames until
// `readback->isDone()`.
IBLBakeScene createIBLBakeScene(osg::Image* equirectImage, const IBLBakeOptions& options={});

// Applies the standard cubemap filter/wrap settings to a completed bake.
// Only valid to call once `readback->isDone()`.
osg::ref_ptr<osg::TextureCubeMap> finishIBLBake(IBLReadback* readback);

}
