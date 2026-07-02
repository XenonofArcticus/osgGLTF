// osggltf-iblbake-gpu -- bake an equirectangular HDR into a GGX-prefiltered KTX2 cubemap.

#include <osg/Notify>

#include <osgGLTF/IBLBaker.hpp>

#include <cstdlib>
#include <cstdio>
#include <string>

int main(int argc, char* argv[])
{
    if (argc < 3) {
        fprintf(stderr,
            "Usage: osggltf-iblbake-gpu <input.hdr> <output.ktx2>"
            " [--prefilter-size N]\n");
        return 1;
    }

    std::string inputPath = argv[1];
    std::string outputPath = argv[2];

    osgGLTF::IBLBakeOptions options;
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--prefilter-size" && i + 1 < argc)
            options.prefilterSize = std::atoi(argv[++i]);
    }

    osg::setNotifyLevel(osg::NOTICE);
    if (!osgGLTF::bakeSpecularIBLToKTX2(inputPath, outputPath, options)) {
        fprintf(stderr, "osggltf-iblbake-gpu: failed to write %s\n", outputPath.c_str());
        return 1;
    }

    OSG_NOTICE << "osggltf-iblbake-gpu: wrote " << outputPath << "\n";
    return 0;
}
