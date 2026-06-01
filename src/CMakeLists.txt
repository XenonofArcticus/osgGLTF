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

# Optional: install into OSG's plugin directory so osgDB finds it automatically.
# get_filename_component(OSG_LIB_DIR ${OSG_LIBRARY} DIRECTORY)
# install(TARGETS osgdb_gltf DESTINATION ${OSG_LIB_DIR}/osgPlugins-${OPENSCENEGRAPH_VERSION})
