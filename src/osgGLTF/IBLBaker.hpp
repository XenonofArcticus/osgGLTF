#pragma once

#include <osg/TextureCubeMap>

namespace osg { class Image; }

namespace osgGLTF {

// TODO: If these can't be NEGATIVE, they need to be OTHER TYPES than `int`!
struct IBLBakeOptions {
	int prefilterSize = 128;
	int maxFrames = 8;
	int readbackFrame = 2;
	bool configureGLContext = true;
};

osg::ref_ptr<osg::TextureCubeMap> bakeSpecularIBL(
	osg::Image* equirectImage,
	const IBLBakeOptions& options={}
);

osg::ref_ptr<osg::TextureCubeMap> bakeSpecularIBL(
	const std::string& inputPath,
	const IBLBakeOptions& options={}
);

bool bakeSpecularIBLToKTX2(
	const std::string& inputPath,
	const std::string& outputPath,
	const IBLBakeOptions& options={}
);

}
