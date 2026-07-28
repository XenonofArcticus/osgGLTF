// osggltf-iblbake-gpu -- bake an equirectangular HDR into a GGX-prefiltered KTX2 cubemap.

#include <osgx/Warnings.hpp>

OSGX_DISABLE_WARNINGS

#include <osg/Notify>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgViewer/Viewer>

#include <osgx/GGXPrefilter.hpp>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <string>

void configureIBLGLContext() {
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

int main(int argc, char* argv[]) {
	if(argc < 3) {
		OSG_WARN
			<< "Usage: osggltf-iblbake-gpu <input.hdr> <output.ktx2> "
			<< "[--prefilter-size N] [--samples N]"
			<< std::endl
		;

		return 1;
	}

	std::string inputPath = argv[1];
	std::string outputPath = argv[2];

	osgx::ibl::GGXPrefilterOptions options;

	for(int i = 3; i < argc; i++) {
		std::string arg = argv[i];

		if(arg == "--prefilter-size" && i + 1 < argc) {
			i++;
			options.prefilterSize = std::atoi(argv[i]);
		}

		else if(arg == "--samples" && i + 1 < argc) {
			i++;
			options.sampleCount = std::atoi(argv[i]);
		}
	}

	osg::setNotifyLevel(osg::NOTICE);

	osg::ref_ptr<osg::Image> image = osgDB::readImageFile(inputPath);

	if(!image) {
		OSG_WARN << "osggltf-iblbake-gpu: failed to load HDR image " << inputPath << std::endl;

		return 1;
	}

	osgx::ibl::GGXPrefilterScene scene = osgx::ibl::createGGXPrefilterScene(
		image,
		options
	);

	if(!scene.root) {
		OSG_WARN << "osggltf-iblbake-gpu: failed to build bake scene" << std::endl;

		return 1;
	}

	osgViewer::Viewer viewer;

	viewer.setUpViewInWindow(0, 0, 128, 128);
	viewer.setSceneData(scene.root);
	viewer.getCamera()->setPostDrawCallback(scene.readback);

	const int maxFrames = std::max(1, options.maxFrames);

	for(int frame = 0; frame < maxFrames && !scene.readback->isDone(); frame++) viewer.frame();

	if(!scene.readback->isDone()) {
		OSG_WARN << "osggltf-iblbake-gpu: IBL bake readback did not complete" << std::endl;

		return 1;
	}

	osg::ref_ptr<osg::TextureCubeMap> result = osgx::ibl::finishGGXPrefilter(
		scene.readback
	);

	if(!result || !osgDB::writeObjectFile(*result, outputPath)) {
		OSG_WARN << "osggltf-iblbake-gpu: failed to write " << outputPath << std::endl;

		return 1;
	}

	OSG_NOTICE << "osggltf-iblbake-gpu: wrote " << outputPath << std::endl;

	return 0;
}
