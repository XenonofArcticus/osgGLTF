cmake_minimum_required(VERSION 3.19)

project(osgdb_gltf)

# set(CMAKE_CXX_STANDARD 20)

find_package(OpenSceneGraph REQUIRED osg)

add_library(osgdb_gltf gltf.cpp gltf.h picojson.h tiny_gltf_loader.h)
target_compile_features(osgdb_gltf INTERFACE cxx_std_20 REQUIRED)
target_link_libraries(osgdb_gltf PRIVATE ${OPENSCENEGRAPH_LIBRARIES})
target_include_directories(osgdb_gltf PRIVATE ${OPENSCENEGRAPH_INCLUDE_DIRS})

if(WIN32)
	target_compile_options(osgdb_gltf PRIVATE /W4 /wd4100)
	# /Wall /wd4225 /wd4668) # /wd4244 /wd4146 /wd4267 /wd4996 /wd4334 /wd4311)

else(WIN32)
	target_compile_options(osgdb_gltf PRIVATE -W -Wall -Wno-unused-parameter)
endif()

