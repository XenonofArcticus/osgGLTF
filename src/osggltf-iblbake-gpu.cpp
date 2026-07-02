// osggltf-iblbake-gpu -- bake an equirectangular HDR into a GGX-prefiltered KTX2 cubemap.

#include <osg/Notify>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgViewer/Viewer>

#include <osgGLTF/IBLBaker.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <string>

int main(int argc, char* argv[]) {
	if(argc < 3) {
		OSG_WARN
			<< "Usage: osggltf-iblbake-gpu <input.hdr> <output.ktx2> [--prefilter-size N]"
			<< std::endl
		;

		return 1;
	}

	std::string inputPath = argv[1];
	std::string outputPath = argv[2];

	osgGLTF::IBLBakeOptions options;

	for(int i = 3; i < argc; ++i) {
		std::string arg = argv[i];

		if(arg == "--prefilter-size" && i + 1 < argc) options.prefilterSize = std::atoi(argv[++i]);
	}

	osg::setNotifyLevel(osg::NOTICE);

	if(options.configureGLContext) osgGLTF::configureIBLGLContext();

	osg::ref_ptr<osg::Image> image = osgDB::readImageFile(inputPath);

	if(!image) {
		OSG_WARN << "osggltf-iblbake-gpu: failed to load HDR image " << inputPath << std::endl;

		return 1;
	}

	osgGLTF::IBLBakeScene scene = osgGLTF::createIBLBakeScene(image.get(), options);

	if(!scene.root) {
		OSG_WARN << "osggltf-iblbake-gpu: failed to build bake scene" << std::endl;

		return 1;
	}

	osgViewer::Viewer viewer;

	viewer.setUpViewInWindow(0, 0, 128, 128);
	viewer.setSceneData(scene.root.get());
	viewer.getCamera()->setPostDrawCallback(scene.readback.get());

	const int maxFrames = std::max(1, options.maxFrames);

	for(int frame = 0; frame < maxFrames && !scene.readback->isDone(); ++frame) viewer.frame();

	if(!scene.readback->isDone()) {
		OSG_WARN << "osggltf-iblbake-gpu: IBL bake readback did not complete" << std::endl;

		return 1;
	}

	osg::ref_ptr<osg::TextureCubeMap> result = osgGLTF::finishIBLBake(scene.readback.get());

	if(!result || !osgDB::writeObjectFile(*result, outputPath)) {
		OSG_WARN << "osggltf-iblbake-gpu: failed to write " << outputPath.c_str() << std::endl;

		return 1;
	}

	OSG_NOTICE << "osggltf-iblbake-gpu: wrote " << outputPath << std::endl;

	return 0;
}
