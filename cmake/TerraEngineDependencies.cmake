# TerraEngine keeps every build dependency in External/ so clean checkouts do
# not depend on a system package manager. This helper configures the approved
# Jolt revision as a private, CPU-only engine implementation detail.
function(terraengine_configure_jolt_dependency)
	set(_terraengine_jolt_root "${CMAKE_SOURCE_DIR}/External/JoltPhysics")
	set(TERRAENGINE_JOLT_REVISION "e77f175595e64cb44218cc9d9d56fc365ad0e36a")
	set(TERRAENGINE_JOLT_VERSION "5.6.0")

	if(NOT EXISTS "${_terraengine_jolt_root}/Build/CMakeLists.txt")
		message(FATAL_ERROR
			"Jolt Physics is missing. Initialize the pinned dependency with "
			"'git submodule update --init --recursive External/JoltPhysics'.")
	endif()

	# A Git checkout carries an exact object identity. Source archives may omit
	# Git metadata, so the version check below remains the portable fallback.
	if(EXISTS "${_terraengine_jolt_root}/.git")
		find_package(Git QUIET)
		if(NOT Git_FOUND)
			message(FATAL_ERROR "Git is required to validate the Jolt submodule revision.")
		endif()
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" -C "${_terraengine_jolt_root}" rev-parse HEAD
			OUTPUT_VARIABLE TERRAENGINE_JOLT_ACTUAL_REVISION
			OUTPUT_STRIP_TRAILING_WHITESPACE
			RESULT_VARIABLE TERRAENGINE_JOLT_REVISION_RESULT
		)
		if(NOT TERRAENGINE_JOLT_REVISION_RESULT EQUAL 0 OR
			NOT TERRAENGINE_JOLT_ACTUAL_REVISION STREQUAL TERRAENGINE_JOLT_REVISION)
			message(FATAL_ERROR
				"External/JoltPhysics must be revision ${TERRAENGINE_JOLT_REVISION}; "
				"found '${TERRAENGINE_JOLT_ACTUAL_REVISION}'.")
		endif()
	endif()

	file(READ "${_terraengine_jolt_root}/Jolt/Core/Core.h" TERRAENGINE_JOLT_CORE_HEADER)
	foreach(TERRAENGINE_VERSION_PAIR IN ITEMS
		"JPH_VERSION_MAJOR;5"
		"JPH_VERSION_MINOR;6"
		"JPH_VERSION_PATCH;0")
		list(GET TERRAENGINE_VERSION_PAIR 0 TERRAENGINE_VERSION_NAME)
		list(GET TERRAENGINE_VERSION_PAIR 1 TERRAENGINE_VERSION_VALUE)
		if(NOT TERRAENGINE_JOLT_CORE_HEADER MATCHES
			"#define[ \t]+${TERRAENGINE_VERSION_NAME}[ \t]+${TERRAENGINE_VERSION_VALUE}")
			message(FATAL_ERROR "External/JoltPhysics does not contain approved Jolt 5.6.0 headers.")
		endif()
	endforeach()

	# Only the core static library is part of TerraEngine. Provider applications,
	# tests, serialization, debug rendering, profiling, GPU paths, and install rules
	# stay disabled. A named list keeps the audited provider surface reviewable and
	# lets us remove every temporary cache override after Jolt creates its target.
	set(_terraengine_jolt_disabled_options
		JPH_BUILD_SHARED_LIBS
		ENABLE_INSTALL
		TARGET_UNIT_TESTS
		TARGET_HELLO_WORLD
		TARGET_PERFORMANCE_TEST
		TARGET_SAMPLES
		TARGET_VIEWER
		ENABLE_OBJECT_STREAM
		JPH_USE_DX12
		JPH_USE_VK
		JPH_USE_MTL
		JPH_USE_CPU_COMPUTE
		DEBUG_RENDERER_IN_DEBUG_AND_RELEASE
		DEBUG_RENDERER_IN_DISTRIBUTION
		PROFILER_IN_DEBUG_AND_RELEASE
		PROFILER_IN_DISTRIBUTION
		JPH_USE_EXTERNAL_PROFILE
		OVERRIDE_CXX_FLAGS
		ENABLE_ALL_WARNINGS
		USE_STATIC_MSVC_RUNTIME_LIBRARY
		INTERPROCEDURAL_OPTIMIZATION
	)
	foreach(_terraengine_jolt_option IN LISTS _terraengine_jolt_disabled_options)
		set(${_terraengine_jolt_option} OFF CACHE BOOL "TerraEngine core-only Jolt profile" FORCE)
	endforeach()
	# OFF preserves Jolt's custom allocator hooks; TerraEngine supplies the bounded
	# allocator adapter used by the provider runtime.
	set(DISABLE_CUSTOM_ALLOCATOR OFF CACHE BOOL "Use TerraEngine's Jolt allocator adapter" FORCE)
	set(DOUBLE_PRECISION OFF CACHE BOOL "Match TerraEngine float transforms" FORCE)
	set(USE_ASSERTS ON CACHE BOOL "Route provider assertions through TerraEngine" FORCE)

	add_subdirectory(
		"${_terraengine_jolt_root}/Build"
		"${CMAKE_BINARY_DIR}/External/JoltPhysics"
		EXCLUDE_FROM_ALL
	)
	set_target_properties(Jolt PROPERTIES
		POSITION_INDEPENDENT_CODE ON
		CXX_SCAN_FOR_MODULES OFF
	)

	# Jolt has already consumed these options. Clear them so the parent cache does
	# not expose provider-private switches as persistent TerraEngine configuration.
	foreach(_terraengine_jolt_option IN LISTS _terraengine_jolt_disabled_options)
		unset(${_terraengine_jolt_option} CACHE)
	endforeach()
	unset(DISABLE_CUSTOM_ALLOCATOR CACHE)
	unset(DOUBLE_PRECISION CACHE)
	unset(USE_ASSERTS CACHE)

	# Return stable metadata to the parent directory for engine/test contracts.
	set(TERRAENGINE_JOLT_ROOT "${_terraengine_jolt_root}" PARENT_SCOPE)
	set(TERRAENGINE_JOLT_REVISION "${TERRAENGINE_JOLT_REVISION}" PARENT_SCOPE)
	set(TERRAENGINE_JOLT_VERSION "${TERRAENGINE_JOLT_VERSION}" PARENT_SCOPE)
endfunction()

# Keep the root build declarative: every production dependency is configured
# through this single entry point before engine targets are declared.
function(terraengine_configure_dependencies)
	terraengine_configure_jolt_dependency()
endfunction()
