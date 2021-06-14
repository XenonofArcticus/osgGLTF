add_library(osgdb_gltf SHARED
	"gltf.cpp"
	"gltf.h"
	"picojson.h"
	"tiny_gltf_loader.h"
)

target_link_libraries(osgdb_gltf PRIVATE ${OPENSCENEGRAPH_LIBRARIES})
target_include_directories(osgdb_gltf PRIVATE ${OPENSCENEGRAPH_INCLUDE_DIRS})

if(WIN32)
	target_compile_options(osgdb_gltf PRIVATE
		/W4
		/wd4100
		/wd4267
		/wd4996
		/wd4701
	)

else(WIN32)
	target_compile_options(osgdb_gltf PRIVATE -W -Wall -Wno-unused-parameter)
endif()
