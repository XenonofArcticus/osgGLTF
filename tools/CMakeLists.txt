# GPU IBL baker: HDR equirectangular image -> GGX-prefiltered KTX2 cubemap.
add_executable(osggltf-iblbake-gpu
	osggltf-iblbake-gpu.cpp
)

osggltf_compile_options(osggltf-iblbake-gpu)
target_include_directories(osggltf-iblbake-gpu PRIVATE
	${OPENSCENEGRAPH_INCLUDE_DIRS}
)
target_link_libraries(osggltf-iblbake-gpu PRIVATE
	osgx::osgx
)
target_compile_features(osggltf-iblbake-gpu PRIVATE cxx_std_20)

# CPU-only IBL baker. Uses stb_image, libktx, and OpenMP when available.
find_package(OpenMP)

add_executable(osggltf-iblbake-cpu
	osggltf-iblbake-cpu.cpp
)

osggltf_compile_options(osggltf-iblbake-cpu)
target_include_directories(osggltf-iblbake-cpu PRIVATE
	${PROJECT_SOURCE_DIR}/ext/tinygltf
	${PROJECT_SOURCE_DIR}/ext/KTX-Software/lib/include
)
target_link_libraries(osggltf-iblbake-cpu PRIVATE ktx)
target_compile_features(osggltf-iblbake-cpu PRIVATE cxx_std_20)

if(OpenMP_CXX_FOUND)
	target_link_libraries(osggltf-iblbake-cpu PRIVATE OpenMP::OpenMP_CXX)
endif()

if(OSGGLTF_INSTALL)
	install(TARGETS
		osggltf-iblbake-gpu
		osggltf-iblbake-cpu
		DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
	)

	if(OSGGLTF_BUILD_PYTHON)
		install(PROGRAMS osggltf.py
			DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
			RENAME osggltf
		)
	endif()
endif()
