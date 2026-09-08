# TerraEngine-owned Recast Navigation integration.
#
# This file deliberately lists only the approved Recast and Detour core source
# files. Do not replace these targets with Recast's top-level CMake project:
# that project can also create demos, tests, DebugUtils, DetourCrowd, and
# DetourTileCache targets which are outside the approved dependency scope.
include_guard(GLOBAL)

function(terraengine_configure_recast_dependency)
	if(TARGET TerraEngineRecastCore AND TARGET TerraEngineDetourCore)
		return()
	endif()

	set(_terraengine_recast_root "${CMAKE_SOURCE_DIR}/External/recastnavigation")
	set(TERRAENGINE_RECAST_REVISION "6dc1667f580357e8a2154c28b7867bea7e8ad3a7")

	if(NOT EXISTS "${_terraengine_recast_root}/Recast/Source/Recast.cpp" OR
		NOT EXISTS "${_terraengine_recast_root}/Detour/Source/DetourNavMesh.cpp")
		message(FATAL_ERROR
			"Recast Navigation is missing. Initialize the approved pinned dependency with "
			"'git submodule update --init --recursive External/recastnavigation'.")
	endif()

	# The submodule is the authority for the exact approved source revision. A
	# source archive without Git metadata is intentionally not accepted here: the
	# dependency approval requires a pristine, reviewable checkout pin.
	if(NOT EXISTS "${_terraengine_recast_root}/.git")
		message(FATAL_ERROR
			"External/recastnavigation must be the approved Git submodule at revision "
			"${TERRAENGINE_RECAST_REVISION}.")
	endif()
	terraengine_read_checkout_revision(
		"${_terraengine_recast_root}"
		TERRAENGINE_RECAST_ACTUAL_REVISION)
	if(NOT TERRAENGINE_RECAST_ACTUAL_REVISION STREQUAL TERRAENGINE_RECAST_REVISION)
		message(FATAL_ERROR
			"External/recastnavigation must be revision ${TERRAENGINE_RECAST_REVISION}; "
			"found '${TERRAENGINE_RECAST_ACTUAL_REVISION}'.")
	endif()

	# Recast builds the compact heightfield and polygon data used by the engine
	# adapter. Keeping the source list explicit makes unauthorized provider
	# modules visible during review instead of silently picking them up by glob.
	add_library(TerraEngineRecastCore STATIC
		"${_terraengine_recast_root}/Recast/Source/Recast.cpp"
		"${_terraengine_recast_root}/Recast/Source/RecastAlloc.cpp"
		"${_terraengine_recast_root}/Recast/Source/RecastArea.cpp"
		"${_terraengine_recast_root}/Recast/Source/RecastAssert.cpp"
		"${_terraengine_recast_root}/Recast/Source/RecastContour.cpp"
		"${_terraengine_recast_root}/Recast/Source/RecastFilter.cpp"
		"${_terraengine_recast_root}/Recast/Source/RecastLayers.cpp"
		"${_terraengine_recast_root}/Recast/Source/RecastMesh.cpp"
		"${_terraengine_recast_root}/Recast/Source/RecastMeshDetail.cpp"
		"${_terraengine_recast_root}/Recast/Source/RecastRasterization.cpp"
		"${_terraengine_recast_root}/Recast/Source/RecastRegion.cpp"
	)
	target_include_directories(TerraEngineRecastCore SYSTEM
		PUBLIC "${_terraengine_recast_root}/Recast/Include")
	target_compile_features(TerraEngineRecastCore PRIVATE cxx_std_20)
	set_target_properties(TerraEngineRecastCore PROPERTIES
		POSITION_INDEPENDENT_CODE ON
		CXX_SCAN_FOR_MODULES OFF)

	# Detour executes local ground queries. DT_POLYREF64 must be present on both
	# the provider library and every private native adapter that includes Detour
	# headers, so it is an internal usage requirement of this static target.
	add_library(TerraEngineDetourCore STATIC
		"${_terraengine_recast_root}/Detour/Source/DetourAlloc.cpp"
		"${_terraengine_recast_root}/Detour/Source/DetourAssert.cpp"
		"${_terraengine_recast_root}/Detour/Source/DetourCommon.cpp"
		"${_terraengine_recast_root}/Detour/Source/DetourNavMesh.cpp"
		"${_terraengine_recast_root}/Detour/Source/DetourNavMeshBuilder.cpp"
		"${_terraengine_recast_root}/Detour/Source/DetourNavMeshQuery.cpp"
		"${_terraengine_recast_root}/Detour/Source/DetourNode.cpp"
	)
	target_include_directories(TerraEngineDetourCore SYSTEM
		PUBLIC "${_terraengine_recast_root}/Detour/Include")
	target_compile_definitions(TerraEngineDetourCore PUBLIC DT_POLYREF64=1)
	target_compile_features(TerraEngineDetourCore PRIVATE cxx_std_20)
	set_target_properties(TerraEngineDetourCore PROPERTIES
		POSITION_INDEPENDENT_CODE ON
		CXX_SCAN_FOR_MODULES OFF)

	if(MSVC)
		# Provider warnings are outside TerraEngine's source ownership. The
		# adapter and all engine-owned callers retain the normal project warnings.
		target_compile_options(TerraEngineRecastCore PRIVATE /W0)
		target_compile_options(TerraEngineDetourCore PRIVATE /W0)
	else()
		target_compile_options(TerraEngineRecastCore PRIVATE -w)
		target_compile_options(TerraEngineDetourCore PRIVATE -w)
	endif()

	# Return metadata only for private build/test contract checks. No TerraEngine
	# public target links these libraries publicly, so Recast/Detour headers never
	# become an installed engine API requirement.
	set(TERRAENGINE_RECAST_ROOT "${_terraengine_recast_root}" PARENT_SCOPE)
	set(TERRAENGINE_RECAST_REVISION "${TERRAENGINE_RECAST_REVISION}" PARENT_SCOPE)
endfunction()
