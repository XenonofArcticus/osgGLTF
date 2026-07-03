//vimrun! ./test.py

#include "osgGLTF/IBLBaker.hpp"

#include <osg/Image>

#include "pyosg/pyosg.hpp"

#include "pybind11x.hpp"

namespace py = pybind11;
namespace pyx = pybind11x;

PYBIND11_MODULE(osgGLTF, m) {
	auto py_osg = py::module_::import("OpenSceneGraph");

	py::class_<osgGLTF::IBLBakeOptions>(m, "IBLBakeOptions")
		.def(py::init<>())
		.def_readwrite("prefilterSize", &osgGLTF::IBLBakeOptions::prefilterSize)
		.def_readwrite("maxFrames", &osgGLTF::IBLBakeOptions::maxFrames)
		.def_readwrite("readbackFrame", &osgGLTF::IBLBakeOptions::readbackFrame)
		.def_readwrite("syncReadback", &osgGLTF::IBLBakeOptions::syncReadback)
	;

	py::class_<
		osgGLTF::IBLReadback,
		osg::Camera::DrawCallback,
		osg::ref_ptr<osgGLTF::IBLReadback>
	>(m, "IBLReadback")
		.def("isDone", &osgGLTF::IBLReadback::isDone)
		.def("getResult", &osgGLTF::IBLReadback::getResult)
		.def("reset", &osgGLTF::IBLReadback::reset)
	;

	py::class_<osgGLTF::IBLBakeScene>(m, "IBLBakeScene")
		.def_readonly("root", &osgGLTF::IBLBakeScene::root)
		.def_readonly("readback", &osgGLTF::IBLBakeScene::readback)
	;

	m
		.def(
			"createIBLBakeScene",
			&osgGLTF::createIBLBakeScene,
			"equirectImage"_a,
			"options"_a=osgGLTF::IBLBakeOptions()
		)
		.def("rebakeIBLBakeScene", &osgGLTF::rebakeIBLBakeScene, "scene"_a, "equirectImage"_a)
		.def("finishIBLBake", &osgGLTF::finishIBLBake, "readback"_a)
	;

	m.doc() = "osgGLTF - OpenSceneGraph + GLTF/KTX2 (https://github.com/XenonOfArcticus/osgGLTF)";

	py::dict info;

	info["version"] = py::make_tuple(
		0, // OSGGLTF_VERSION_MAJOR,
		0, // OSGGLTF_VERSION_MINOR,
		1 // OSGGLTF_VERSION_PATCH
	);

	pyx::build_info(m, info);
}
