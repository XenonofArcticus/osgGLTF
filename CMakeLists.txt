cmake_minimum_required(VERSION 3.19)

project(osgGLTF)

find_package(OpenSceneGraph REQUIRED
	osg
	osgDB
	osgViewer
	osgGA
	osgUtil
	osgAnimation
)

add_subdirectory("ref/jesesun")
# add_subdirectory("ref/osgearth")
