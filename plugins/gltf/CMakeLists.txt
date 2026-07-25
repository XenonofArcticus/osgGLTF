add_library(osgdb_gltf MODULE
	ReaderWriterGLTF.cpp
)

osggltf_compile_options(osgdb_gltf)
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
