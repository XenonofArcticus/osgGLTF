cmake_minimum_required(VERSION 3.19)

project(osgGLTF)

find_package(OpenSceneGraph REQUIRED
	osg
	osgDB
	osgViewer
	osgGA
	osgUtil
)

# Emit old-style DT_RPATH instead of the linker's default DT_RUNPATH. RUNPATH
# is searched *after* LD_LIBRARY_PATH, so a shell that exports LD_LIBRARY_PATH
# (e.g. for a different OSG checkout) silently shadows a freshly built,
# not-yet-installed binary's own build-tree libraries. RPATH is searched
# first, so build-tree binaries reliably pick up what CMake just built.
add_link_options(-Wl,--disable-new-dtags)

# ---------------------------------------------------------------------------
# KTX-Software (ext/KTX-Software submodule)
# Disable everything we don't need to keep the build fast.
# ---------------------------------------------------------------------------
set(KTX_FEATURE_STATIC_LIBRARY  ON  CACHE BOOL "" FORCE)
set(KTX_FEATURE_TOOLS           OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TESTS           OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_DOC             OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_LOADTEST_APPS   OFF CACHE BOOL "" FORCE)
add_subdirectory("ext/KTX-Software")

add_subdirectory("src")
add_subdirectory("examples")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY
)

add_custom_target(uninstall
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
)
