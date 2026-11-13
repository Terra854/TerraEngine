# Validation fixtures use the existing production package compiler closure.
# The shared producer uses
# the actual package compiler closure (public direct glTF route), not a fake
# skeleton writer. Omit only CLI/unused dispatcher+FBX route and manifests.
get_target_property(review_package_sources TerraEngineModelPackageCompiler SOURCES)
set(review_package_closure)
foreach(source IN LISTS review_package_sources)
	if(source MATCHES "TerraEngineModelPackageCompiler[.]cpp$" OR source MATCHES "[/]ModelPackageImporter[.]cpp$" OR source MATCHES "[/]FbxPackageImporter[.]cpp$" OR source MATCHES "[.]manifest$")
		continue()
	endif()
	if(IS_ABSOLUTE "${source}")
		list(APPEND review_package_closure "${source}")
	else()
		list(APPEND review_package_closure "${CMAKE_SOURCE_DIR}/${source}")
	endif()
endforeach()
add_executable(TerraEngineProductionShapeFixtureBaker
	${CMAKE_SOURCE_DIR}/Tests/Tools/CodeQuality/Fixtures/ProductionShape/ProductionShapeFixtureBaker.cpp
	${CMAKE_SOURCE_DIR}/Tests/Support/ReviewRemediation/AnimatedConsumerCompanion.cpp
	${review_package_closure})
get_target_property(review_package_includes TerraEngineModelPackageCompiler INCLUDE_DIRECTORIES)
get_target_property(review_package_links TerraEngineModelPackageCompiler LINK_LIBRARIES)
target_include_directories(TerraEngineProductionShapeFixtureBaker PRIVATE ${review_package_includes})
target_compile_features(TerraEngineProductionShapeFixtureBaker PRIVATE cxx_std_20)
# The production compiler owns staging its transitive runtime DLL closure.
add_dependencies(TerraEngineProductionShapeFixtureBaker TerraEngineModelPackageCompiler)
target_link_libraries(TerraEngineProductionShapeFixtureBaker PRIVATE ${review_package_links})
# Package target links TerraEngine-Engine for actual skeleton/clip codecs. Do not
# compile the mesh-only standalone codec or define engine-export ownership here.
if(MSVC)
	target_compile_options(TerraEngineProductionShapeFixtureBaker PRIVATE /FS /bigobj)
endif()

add_executable(TerraEngineProcessLifecycleChild
	${CMAKE_SOURCE_DIR}/Tests/Tools/CodeQuality/Fixtures/ProcessLifecycle/ProcessLifecycleChild.cpp)
target_compile_features(TerraEngineProcessLifecycleChild PRIVATE cxx_std_20)
target_link_libraries(TerraEngineProcessLifecycleChild PRIVATE Threads::Threads)
set_target_properties(TerraEngineProductionShapeFixtureBaker TerraEngineProcessLifecycleChild
	PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(MSVC AND WIN32)
	target_sources(TerraEngineProductionShapeFixtureBaker PRIVATE ${CMAKE_SOURCE_DIR}/cmake/TerraEngine.Windows.manifest)
	target_sources(TerraEngineProcessLifecycleChild PRIVATE ${CMAKE_SOURCE_DIR}/cmake/TerraEngine.Windows.manifest)
endif()

# Multi-config output should remain beside each configuration's actual runner.
# Parent may substitute its maintained per-config runtime-directory convention.
if(CMAKE_CONFIGURATION_TYPES)
	set(review_shape_dir "${CMAKE_BINARY_DIR}/$<CONFIG>")
else()
	set(review_shape_dir "${CMAKE_BINARY_DIR}")
endif()
set(review_shape_file "${review_shape_dir}/TerraEngineProductionShapeFixture.temf")
set(review_shape_companions
	"${review_shape_dir}/TerraEngineProductionShapeFixture.teskel"
	"${review_shape_dir}/TerraEngineProductionShapeFixture.teskel.sha256"
	"${review_shape_dir}/TerraEngineProductionShapeFixture.teanim"
	"${review_shape_dir}/TerraEngineProductionShapeFixture.teanim.sha256"
	"${review_shape_dir}/TerraEngineProductionShapeFixture.identity"
	"${review_shape_dir}/TerraEngineProductionShapeFixture.identity.sha256"
	"${review_shape_dir}/TerraEngineProductionShapeFixture.manifest")
add_custom_command(OUTPUT "${review_shape_file}" "${review_shape_file}.sha256" ${review_shape_companions}
	COMMAND ${CMAKE_COMMAND}
		"-DBAKER=$<TARGET_FILE:TerraEngineProductionShapeFixtureBaker>"
		"-DDESTINATION=${review_shape_dir}"
		-P ${CMAKE_SOURCE_DIR}/Tests/Tools/CodeQuality/Fixtures/ProductionShape/BakeProductionShapeFixture.cmake
	DEPENDS TerraEngineProductionShapeFixtureBaker
		${CMAKE_SOURCE_DIR}/Tests/Tools/CodeQuality/Fixtures/ProductionShape/BakeProductionShapeFixture.cmake
	VERBATIM)
add_custom_target(TerraEngineProductionShapeFixture DEPENDS "${review_shape_file}" "${review_shape_file}.sha256" ${review_shape_companions})
add_dependencies(TerraEngine-EngineTests TerraEngineProductionShapeFixture TerraEngineProcessLifecycleChild)
target_include_directories(TerraEngine-EngineTests PRIVATE ${CMAKE_SOURCE_DIR}/External/meshoptimizer/src)
install(TARGETS TerraEngineProcessLifecycleChild
	RUNTIME DESTINATION "${CMAKE_BINARY_DIR}/Build/Tests/Engine")
install(FILES "${review_shape_file}" "${review_shape_file}.sha256" ${review_shape_companions}
	DESTINATION "${CMAKE_BINARY_DIR}/Build/Tests/Engine")
