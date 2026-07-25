add_library(osgKTX2 STATIC
	KTX2.cpp
)
add_library(osgKTX2::osgKTX2 ALIAS osgKTX2)

target_compile_features(osgKTX2 PUBLIC cxx_std_20)
osggltf_compile_options(osgKTX2)
target_sources(osgKTX2 PUBLIC
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/KTX2.hpp>"
)
target_include_directories(osgKTX2
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
		"${OPENSCENEGRAPH_INCLUDE_DIRS}"
)
target_link_libraries(osgKTX2 PUBLIC
	ktx
	"${OPENSCENEGRAPH_LIBRARIES}"
)
set_target_properties(osgKTX2 PROPERTIES
	POSITION_INDEPENDENT_CODE ON
)

add_library(osgdb_ktx2 MODULE
	ReaderWriterKTX2.cpp
)

osggltf_compile_options(osgdb_ktx2)
target_link_libraries(osgdb_ktx2 PRIVATE osgKTX2::osgKTX2)

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
