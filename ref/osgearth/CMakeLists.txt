set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/ref/osgearth")

find_package(OsgEarth REQUIRED)

if(${OSGEARTH_FOUND})
	add_library(osgdb_gltf SHARED
		"ReaderWriterGLTF.cpp"
		"GLTFReader.h"
		"GLTFWriter.h"
		"B3DMReader.h"
		"B3DMWriter.h"
	)

	target_link_libraries(osgdb_gltf PRIVATE
		${OPENSCENEGRAPH_LIBRARIES}
		${OSGEARTH_LIBRARY}
		# ${OSGEARTHUTIL_LIBRARY}
	)

	target_include_directories(osgdb_gltf PRIVATE
		${OPENSCENEGRAPH_INCLUDE_DIRS}
		# "${PROJECT_SOURCE_DIR}/ext/tinygltf"
		"${PROJECT_SOURCE_DIR}/ext/rapidjson/include/rapidjson"
	)

	if(WIN32)
		target_compile_options(osgdb_gltf PRIVATE
			/W4
			# /wd4100
			# /wd4267
			# /wd4996
			# /wd4701
		)

	else(WIN32)
		target_compile_options(osgdb_gltf PRIVATE -W -Wall -Wno-unused-parameter)
	endif()
endif()
