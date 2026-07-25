add_library(osgGLTF
	Accessor.cpp
	Animation.cpp
	Reader.cpp
	Skin.cpp
	Texture.cpp
	TinyGLTF.cpp
)
add_library(osgGLTF::osgGLTF ALIAS osgGLTF)

target_compile_features(osgGLTF PUBLIC cxx_std_20)
target_include_directories(osgGLTF
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
		"$<BUILD_INTERFACE:${OPENSCENEGRAPH_INCLUDE_DIRS}>"
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
	PRIVATE
		${PROJECT_SOURCE_DIR}/ext/tinygltf
)
target_sources(osgGLTF PUBLIC
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/osgGLTF/Reader.hpp>"
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/osgGLTF/Shader.hpp>"
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/osgGLTF/SimplePlayer.hpp>"
)
target_sources(osgGLTF PRIVATE
	Accessor.hpp
	Animation.hpp
	GLTFReader.hpp
	Log.hpp
	Skin.hpp
	Texture.hpp
)
target_link_libraries(osgGLTF PUBLIC
	"$<BUILD_INTERFACE:${OPENSCENEGRAPH_LIBRARIES}>"
)
set_target_properties(osgGLTF PROPERTIES
	EXPORT_NAME osgGLTF
	POSITION_INDEPENDENT_CODE ON
	WINDOWS_EXPORT_ALL_SYMBOLS ON
)

# TinyGLTF.cpp owns the single tinygltf/STB implementation. tinygltf's bundled JSON header uses
# the deprecated std::is_pod trait; isolate that third-party warning to these translation units.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	set_source_files_properties(
		Accessor.cpp Animation.cpp Reader.cpp Skin.cpp Texture.cpp TinyGLTF.cpp
		PROPERTIES
		COMPILE_OPTIONS "-Wno-deprecated-declarations"
	)
endif()

if(OSGGLTF_INSTALL)
	install(TARGETS osgGLTF EXPORT osgGLTFTargets
		ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
		LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
		RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
	)
	install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/osgGLTF/"
		DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/osgGLTF"
		FILES_MATCHING PATTERN "*.hpp"
	)
	install(EXPORT osgGLTFTargets
		FILE osgGLTFTargets.cmake
		NAMESPACE osgGLTF::
		DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/osgGLTF"
	)

	configure_package_config_file(
		"${PROJECT_SOURCE_DIR}/cmake/osgGLTFConfig.cmake.in"
		"${PROJECT_BINARY_DIR}/osgGLTFConfig.cmake"
		INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/osgGLTF"
	)
	install(FILES "${PROJECT_BINARY_DIR}/osgGLTFConfig.cmake"
		DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/osgGLTF"
	)
endif()

add_library(osgdb_gltf MODULE
	ReaderWriterGLTF.cpp
)

target_link_libraries(osgdb_gltf PRIVATE osgGLTF::osgGLTF)

set_target_properties(osgdb_gltf PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	PREFIX "" # OSG plugin loader expects no "lib" prefix
)

if(OSGGLTF_INSTALL)
	install(TARGETS osgdb_gltf
		DESTINATION ${CMAKE_INSTALL_PREFIX}/lib64/osgPlugins-${OPENSCENEGRAPH_VERSION}
	)
endif()

# ---------------------------------------------------------------------------
# KTX2 plugin - requires ext/KTX-Software submodule
# ---------------------------------------------------------------------------

if(OSGGLTF_BUILD_KTX2)
	add_library(osgdb_ktx2 MODULE
		ReaderWriterKTX2.cpp
	)

	target_include_directories(osgdb_ktx2 PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${OPENSCENEGRAPH_INCLUDE_DIRS}
	)

	target_link_libraries(osgdb_ktx2 PRIVATE
		ktx
		${OPENSCENEGRAPH_LIBRARIES}
	)

	set_target_properties(osgdb_ktx2 PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED ON
		PREFIX ""
	)

	if(OSGGLTF_INSTALL)
		install(TARGETS osgdb_ktx2
			DESTINATION ${CMAKE_INSTALL_PREFIX}/lib64/osgPlugins-${OPENSCENEGRAPH_VERSION}
		)
	endif()
endif()

# ---------------------------------------------------------------------------
# osggltf-iblbake-gpu - GPU IBL baker: HDR equirect -> GGX-prefiltered KTX2 cubemap
# ---------------------------------------------------------------------------

if(OSGGLTF_BUILD_TOOLS)
	add_executable(osggltf-iblbake-gpu osggltf-iblbake-gpu.cpp)

	target_include_directories(osggltf-iblbake-gpu PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${OPENSCENEGRAPH_INCLUDE_DIRS}
	)

	target_link_libraries(osggltf-iblbake-gpu PRIVATE
		osgx::osgx
	)

	set_target_properties(osggltf-iblbake-gpu PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED ON
	)

	if(OSGGLTF_INSTALL)
		install(TARGETS osggltf-iblbake-gpu
			DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
		)
	endif()

# ---------------------------------------------------------------------------
# osggltf-iblbake-cpu - CPU-only IBL baker (no OpenGL, no OSG). Uses stb_image + libktx.
# ---------------------------------------------------------------------------

	find_package(OpenMP)

	add_executable(osggltf-iblbake-cpu osggltf-iblbake-cpu.cpp)

	target_include_directories(osggltf-iblbake-cpu PRIVATE
		${PROJECT_SOURCE_DIR}/ext/tinygltf # stb_image.h
		${PROJECT_SOURCE_DIR}/ext/KTX-Software/lib/include
	)

	target_link_libraries(osggltf-iblbake-cpu PRIVATE ktx)

	if(OpenMP_CXX_FOUND)
		target_link_libraries(osggltf-iblbake-cpu PRIVATE OpenMP::OpenMP_CXX)
	endif()

	set_target_properties(osggltf-iblbake-cpu PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED ON
	)

	if(OSGGLTF_INSTALL)
		install(TARGETS osggltf-iblbake-cpu DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
	endif()

	if(OSGGLTF_INSTALL AND OSGGLTF_BUILD_PYTHON)
		install(PROGRAMS osggltf.py
			DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
			RENAME osggltf
		)
	endif()
endif()
