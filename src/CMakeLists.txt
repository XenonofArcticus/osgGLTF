add_library(osgdb_gltf MODULE
    ReaderWriterGLTF.cpp
)

target_include_directories(osgdb_gltf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/ext/tinygltf
    ${OPENSCENEGRAPH_INCLUDE_DIRS}
)

target_link_libraries(osgdb_gltf PRIVATE
    ${OPENSCENEGRAPH_LIBRARIES}
)

set_target_properties(osgdb_gltf PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
    PREFIX                ""           # OSG plugin loader expects no "lib" prefix
)

install(TARGETS osgdb_gltf
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib64/osgPlugins-${OPENSCENEGRAPH_VERSION}
)

# ---------------------------------------------------------------------------
# KTX2 plugin — requires ext/KTX-Software submodule
# ---------------------------------------------------------------------------

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
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
    PREFIX                ""
)

install(TARGETS osgdb_ktx2
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib64/osgPlugins-${OPENSCENEGRAPH_VERSION}
)

# ---------------------------------------------------------------------------
# osggltf-iblbake-gpu — GPU IBL baker: HDR equirect → GGX-prefiltered KTX2 cubemap
# ---------------------------------------------------------------------------

add_executable(osggltf-iblbake-gpu osggltf-iblbake-gpu.cpp)

target_include_directories(osggltf-iblbake-gpu PRIVATE
    ${OPENSCENEGRAPH_INCLUDE_DIRS}
)

target_link_libraries(osggltf-iblbake-gpu PRIVATE
    ${OPENSCENEGRAPH_LIBRARIES}
    GL
)

set_target_properties(osggltf-iblbake-gpu PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS osggltf-iblbake-gpu
    DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
)

# ---------------------------------------------------------------------------
# osggltf-iblbake-cpu — CPU-only IBL baker (no OpenGL, no OSG).  Uses stb_image + libktx.
# ---------------------------------------------------------------------------

find_package(OpenMP)

add_executable(osggltf-iblbake-cpu osggltf-iblbake-cpu.cpp)

target_include_directories(osggltf-iblbake-cpu PRIVATE
    ${CMAKE_SOURCE_DIR}/ext/tinygltf    # stb_image.h
    ${CMAKE_SOURCE_DIR}/ext/KTX-Software/lib/include
)

target_link_libraries(osggltf-iblbake-cpu PRIVATE ktx)

if(OpenMP_CXX_FOUND)
    target_link_libraries(osggltf-iblbake-cpu PRIVATE OpenMP::OpenMP_CXX)
endif()

set_target_properties(osggltf-iblbake-cpu PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS osggltf-iblbake-cpu DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
