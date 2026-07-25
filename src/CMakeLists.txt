add_library(osgGLTF
	Accessor.cpp
	Animation.cpp
	Material.cpp
	Mesh.cpp
	Reader.cpp
	ReaderImpl.cpp
	Scene.cpp
	Skin.cpp
	Texture.cpp
	TinyGLTF.cpp
)
add_library(osgGLTF::osgGLTF ALIAS osgGLTF)

target_compile_features(osgGLTF PUBLIC cxx_std_20)
target_include_directories(osgGLTF
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
		"$<BUILD_INTERFACE:${OPENSCENEGRAPH_INCLUDE_DIRS}>"
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
	PRIVATE
		${PROJECT_SOURCE_DIR}/ext/tinygltf
)
target_sources(osgGLTF PUBLIC
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/osgGLTF/Reader.hpp>"
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/osgGLTF/Shader.hpp>"
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/osgGLTF/SimplePlayer.hpp>"
)
target_sources(osgGLTF PRIVATE
	Accessor.hpp
	Animation.hpp
	Log.hpp
	Material.hpp
	Mesh.hpp
	ReaderImpl.hpp
	Scene.hpp
	Skin.hpp
	Texture.hpp
)
target_link_libraries(osgGLTF PUBLIC
	"$<BUILD_INTERFACE:${OPENSCENEGRAPH_LIBRARIES}>"
)
set_target_properties(osgGLTF PROPERTIES
	EXPORT_NAME osgGLTF
	POSITION_INDEPENDENT_CODE ON
	WINDOWS_EXPORT_ALL_SYMBOLS ON
)

# TinyGLTF.cpp owns the single tinygltf/STB implementation. tinygltf's bundled JSON header uses
# the deprecated std::is_pod trait; isolate that third-party warning to these translation units.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	set_source_files_properties(
		Accessor.cpp Animation.cpp Material.cpp Mesh.cpp Reader.cpp ReaderImpl.cpp Scene.cpp Skin.cpp Texture.cpp TinyGLTF.cpp
		PROPERTIES
		COMPILE_OPTIONS "-Wno-deprecated-declarations"
	)
endif()

if(OSGGLTF_INSTALL)
	install(TARGETS osgGLTF EXPORT osgGLTFTargets
		ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
		LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
		RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
	)
	install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/osgGLTF/"
		DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/osgGLTF"
		FILES_MATCHING PATTERN "*.hpp"
	)
	install(EXPORT osgGLTFTargets
		FILE osgGLTFTargets.cmake
		NAMESPACE osgGLTF::
		DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/osgGLTF"
	)

	configure_package_config_file(
		"${PROJECT_SOURCE_DIR}/cmake/osgGLTFConfig.cmake.in"
		"${PROJECT_BINARY_DIR}/osgGLTFConfig.cmake"
		INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/osgGLTF"
	)
	install(FILES "${PROJECT_BINARY_DIR}/osgGLTFConfig.cmake"
		DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/osgGLTF"
	)
endif()
