# Feature 045 schema validation is a small reusable library shared by tests and
# the installed benchmark. It uses only TerraEngine's vendored JSON header.
add_library(TerraEngineVirtualGeometrySchemas STATIC
	VirtualGeometrySchemas.cpp
	VirtualGeometrySchemas.h
)

add_executable(TerraEngineVirtualGeometryAssetTool
	VirtualGeometryAssetTool.cpp
)
target_compile_features(TerraEngineVirtualGeometryAssetTool PRIVATE cxx_std_20)
target_include_directories(TerraEngineVirtualGeometryAssetTool PRIVATE
	"${CMAKE_SOURCE_DIR}/External/json/single_include"
)
if(MSVC)
	target_compile_options(TerraEngineVirtualGeometryAssetTool PRIVATE /FS)
endif()
# TerraEngine's runnable install tree is <build>/Build. Use the same explicit
# destination as the benchmark and codec experiment so copied install trees do
# not lose the asset utility in the generic CMake prefix.
install(TARGETS TerraEngineVirtualGeometryAssetTool
	RUNTIME DESTINATION "${CMAKE_BINARY_DIR}/Build/Tools")

target_compile_features(TerraEngineVirtualGeometrySchemas PUBLIC cxx_std_20)
target_include_directories(TerraEngineVirtualGeometrySchemas
	PUBLIC
		"${CMAKE_CURRENT_SOURCE_DIR}"
	PRIVATE
		"${CMAKE_SOURCE_DIR}/External/json/single_include"
)

set_target_properties(TerraEngineVirtualGeometrySchemas PROPERTIES
	CXX_SCAN_FOR_MODULES OFF
)

if(MSVC)
	target_compile_options(TerraEngineVirtualGeometrySchemas PRIVATE /FS)
endif()

add_executable(TerraEngineVirtualGeometryBenchmark
	TerraEngineVirtualGeometryBenchmark.cpp
)

target_compile_features(TerraEngineVirtualGeometryBenchmark PRIVATE cxx_std_20)
target_include_directories(TerraEngineVirtualGeometryBenchmark
	PRIVATE
		"${CMAKE_SOURCE_DIR}/External/json/single_include"
)
target_link_libraries(TerraEngineVirtualGeometryBenchmark
	PRIVATE
		TerraEngineVirtualGeometrySchemas
)

set_target_properties(TerraEngineVirtualGeometryBenchmark PROPERTIES
	CXX_SCAN_FOR_MODULES OFF
)

if(MSVC)
	target_compile_options(TerraEngineVirtualGeometryBenchmark PRIVATE /FS)
endif()

add_executable(TerraEngineVirtualGeometryCodecExperiment
	TerraEngineVirtualGeometryCodecExperiment.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/VirtualGeometryCodecs.cpp
	${CMAKE_SOURCE_DIR}/Editor/AssetCompilers/VirtualGeometryMeshoptCodec.cpp
	${CMAKE_SOURCE_DIR}/Engine/Resource/VirtualGeometryCodec.cpp
)

target_compile_features(
	TerraEngineVirtualGeometryCodecExperiment PRIVATE cxx_std_20)
target_include_directories(TerraEngineVirtualGeometryCodecExperiment
	PRIVATE
		"${CMAKE_SOURCE_DIR}"
		"${CMAKE_SOURCE_DIR}/Engine"
		"${CMAKE_SOURCE_DIR}/Editor/AssetCompilers"
		"${CMAKE_SOURCE_DIR}/External/json/single_include"
)
target_link_libraries(TerraEngineVirtualGeometryCodecExperiment
	PRIVATE
		meshoptimizer
)
target_compile_definitions(TerraEngineVirtualGeometryCodecExperiment
	PRIVATE
		# The standalone tool compiles the runtime decoder implementation
		# directly, so its public API must not be treated as a DLL import.
		TERRAENGINE_ENGINE_BUILD
)
set_target_properties(TerraEngineVirtualGeometryCodecExperiment PROPERTIES
	CXX_SCAN_FOR_MODULES OFF
)
if(MSVC)
	target_compile_options(
		TerraEngineVirtualGeometryCodecExperiment PRIVATE /FS)
endif()

# The benchmark executable belongs beside installed evidence workflows, while
# its source/scripts/manifests are copied by the repository-level install rule.
install(TARGETS
	TerraEngineVirtualGeometryBenchmark
	TerraEngineVirtualGeometryCodecExperiment
	# This subdirectory is configured before the repository-level
	# ENGINE_INSTALL_DESTINATION variable is assigned. Use the same concrete
	# build-tree contract here so an empty variable cannot install to C:/Tools.
	RUNTIME DESTINATION "${CMAKE_BINARY_DIR}/Build/Tools"
)
