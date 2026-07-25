cmake_minimum_required(VERSION 3.19)

project(osgGLTF)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
	set(OSGGLTF_IS_TOP_LEVEL ON)
else()
	set(OSGGLTF_IS_TOP_LEVEL OFF)
endif()

set(OSG_PYTHON_DIR "" CACHE PATH "Path to OpenSceneGraph.py checkout")

set(OSGGLTF_BUILD_PYTHON_DEFAULT OFF)
if(OSG_PYTHON_DIR)
	set(OSGGLTF_BUILD_PYTHON_DEFAULT ON)
endif()

option(OSGGLTF_BUILD_KTX2 "Build the KTX2 osgDB plugin" ${OSGGLTF_IS_TOP_LEVEL})
option(OSGGLTF_BUILD_TOOLS "Build the osgGLTF command-line tools" ${OSGGLTF_IS_TOP_LEVEL})
option(OSGGLTF_BUILD_EXAMPLES "Build the osgGLTF examples" ${OSGGLTF_IS_TOP_LEVEL})
option(OSGGLTF_BUILD_PYTHON "Build the osgGLTF Python module" ${OSGGLTF_BUILD_PYTHON_DEFAULT})
option(OSGGLTF_INSTALL "Generate osgGLTF installation rules" ${OSGGLTF_IS_TOP_LEVEL})

find_package(OpenSceneGraph REQUIRED
	osg
	osgDB
	osgViewer
	osgGA
	osgUtil
)

if(OSGGLTF_BUILD_PYTHON)
	if(NOT OSG_PYTHON_DIR)
		message(FATAL_ERROR
			"OSGGLTF_BUILD_PYTHON requires OSG_PYTHON_DIR to identify an OpenSceneGraph.py checkout"
		)
	endif()

	if(NOT EXISTS "${OSG_PYTHON_DIR}/etc/pybind11/CMakeLists.txt")
		message(FATAL_ERROR
			"OSG_PYTHON_DIR does not contain etc/pybind11: ${OSG_PYTHON_DIR}"
		)
	endif()

	find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
	add_subdirectory("${OSG_PYTHON_DIR}/etc/pybind11" "ext/pybind11")
endif()

# Emit old-style DT_RPATH instead of the linker's default DT_RUNPATH. RUNPATH
# is searched *after* LD_LIBRARY_PATH, so a shell that exports LD_LIBRARY_PATH
# (e.g. for a different OSG checkout) silently shadows a freshly built,
# not-yet-installed binary's own build-tree libraries. RPATH is searched
# first, so build-tree binaries reliably pick up what CMake just built.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	add_link_options(-Wl,--disable-new-dtags)
endif()

# ---------------------------------------------------------------------------
# KTX-Software (ext/KTX-Software submodule)
# Disable everything we don't need to keep the build fast.
# ---------------------------------------------------------------------------
if(OSGGLTF_BUILD_KTX2 OR OSGGLTF_BUILD_TOOLS)
	set(KTX_FEATURE_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
	set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
	set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
	set(KTX_FEATURE_DOC OFF CACHE BOOL "" FORCE)
	set(KTX_FEATURE_LOADTEST_APPS OFF CACHE BOOL "" FORCE)
	add_subdirectory("ext/KTX-Software")
endif()

if(OSGGLTF_BUILD_TOOLS)
	find_package(osgx CONFIG REQUIRED)
endif()

add_subdirectory("src")

if(OSGGLTF_BUILD_EXAMPLES)
	add_subdirectory("examples")
endif()

if(OSGGLTF_INSTALL)
	configure_file(
		"${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
		"${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
		IMMEDIATE @ONLY
	)

	add_custom_target(uninstall
		COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
	)
endif()

if(OSGGLTF_BUILD_PYTHON)
	pybind11_add_module(osgGLTF_python "ext/osgGLTF-python.cpp")
	set_target_properties(osgGLTF_python PROPERTIES OUTPUT_NAME "osgGLTF")
	target_include_directories(osgGLTF_python PRIVATE
		"${OSG_PYTHON_DIR}"
		"${OSG_PYTHON_DIR}/etc"
		"${PROJECT_SOURCE_DIR}/src"
		"${PROJECT_SOURCE_DIR}/ext/tinygltf"
		${OPENSCENEGRAPH_INCLUDE_DIRS}
	)
	target_link_libraries(osgGLTF_python PRIVATE
		pybind11::module
		${OPENSCENEGRAPH_LIBRARIES}
	)
	target_compile_features(osgGLTF_python PUBLIC cxx_std_20)
	# target_compile_options(osgGLTF_python PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
	set_target_properties(osgGLTF_python PROPERTIES POSITION_INDEPENDENT_CODE ON)

	# ext/osgGLTF-python.cpp pulls in ext/tinygltf/json.hpp, which uses the now-deprecated
	# std::is_pod -- not our code to fix, silence just this warning on just this file
	# rather than disabling deprecation warnings project-wide.
	if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
		set_source_files_properties("ext/osgGLTF-python.cpp" PROPERTIES
			COMPILE_OPTIONS "-Wno-deprecated-declarations"
		)
	endif()
endif()
