find_package(OpenSceneGraph REQUIRED osg osgDB osgViewer osgGA)

add_executable(osggltf-ktx2-skybox osggltf-ktx2-skybox.cpp)

osggltf_compile_options(osggltf-ktx2-skybox)
target_include_directories(osggltf-ktx2-skybox PRIVATE
    ${OPENSCENEGRAPH_INCLUDE_DIRS}
)

target_link_libraries(osggltf-ktx2-skybox PRIVATE
    ${OPENSCENEGRAPH_LIBRARIES}
)

set_target_properties(osggltf-ktx2-skybox PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
)

if(OSGGLTF_INSTALL)
	install(TARGETS osggltf-ktx2-skybox DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
endif()
