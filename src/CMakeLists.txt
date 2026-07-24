add_library(osgGLTF
	IBLBaker.cpp
)

target_include_directories(osgGLTF PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
	$<INSTALL_INTERFACE:include>
	${OPENSCENEGRAPH_INCLUDE_DIRS}
)

target_link_libraries(osgGLTF PUBLIC
	${OPENSCENEGRAPH_LIBRARIES}
	GL
)

set_target_properties(osgGLTF PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
)

if(OSGGLTF_INSTALL)
	install(TARGETS osgGLTF
		ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/lib64
		LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib64
		RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
	)

	install(FILES
		osgGLTF/IBLBaker.hpp
		osgGLTF/Shader.hpp
		osgGLTF/SimplePlayer.hpp
		DESTINATION ${CMAKE_INSTALL_PREFIX}/include/osgGLTF
	)
endif()

add_library(osgdb_gltf MODULE
	ReaderWriterGLTF.cpp
)

target_include_directories(osgdb_gltf PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${PROJECT_SOURCE_DIR}/ext/tinygltf
	${OPENSCENEGRAPH_INCLUDE_DIRS}
)

# ReaderWriterGLTF.cpp pulls in ext/tinygltf/json.hpp, which uses the now-deprecated
# std::is_pod -- not our code to fix, silence just this warning on just this file
# rather than disabling deprecation warnings project-wide.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	set_source_files_properties(ReaderWriterGLTF.cpp PROPERTIES
		COMPILE_OPTIONS "-Wno-deprecated-declarations"
	)
endif()

target_link_libraries(osgdb_gltf PRIVATE
	${OPENSCENEGRAPH_LIBRARIES}
)

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
		osgGLTF
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
