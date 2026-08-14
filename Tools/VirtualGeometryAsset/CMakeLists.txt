# The virtual-geometry stage-cache utility is a production-facing maintenance
# tool. It can inspect records without mutation; guarded repair and invalidation
# require both an explicit repository-local target and --confirm.
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

# TerraEngine's runnable install tree is <build>/Build. Install this genuine
# asset utility under Tools while test-only executables use Tests/Tools.
install(TARGETS TerraEngineVirtualGeometryAssetTool
	RUNTIME DESTINATION "${CMAKE_BINARY_DIR}/Build/Tools")
