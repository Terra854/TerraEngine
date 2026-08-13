# TerraEngine keeps every build dependency in External/ so clean checkouts do
# not depend on a system package manager. This helper configures the approved
# Jolt revision as a private, CPU-only engine implementation detail.
function(terraengine_read_checkout_revision checkout_root output_variable)
	# Read Git's documented administrative files directly. Besides avoiding an
	# external process during every CMake regeneration, this works in offline and
	# hermetic builds where the source checkout exists but git.exe is unavailable.
	set(_terraengine_git_marker "${checkout_root}/.git")
	if(IS_DIRECTORY "${_terraengine_git_marker}")
		set(_terraengine_git_dir "${_terraengine_git_marker}")
	else()
		file(READ "${_terraengine_git_marker}" _terraengine_git_marker_text)
		string(STRIP "${_terraengine_git_marker_text}" _terraengine_git_marker_text)
		if(NOT _terraengine_git_marker_text MATCHES "^gitdir:[ \t]*(.+)$")
			message(FATAL_ERROR "${checkout_root}/.git is not a recognized Git checkout marker.")
		endif()
		get_filename_component(
			_terraengine_git_dir
			"${CMAKE_MATCH_1}"
			ABSOLUTE
			BASE_DIR "${checkout_root}")
	endif()

	file(READ "${_terraengine_git_dir}/HEAD" _terraengine_git_head)
	string(STRIP "${_terraengine_git_head}" _terraengine_git_head)
	if(NOT _terraengine_git_head MATCHES "^ref:[ \t]*(.+)$")
		set(_terraengine_revision "${_terraengine_git_head}")
	else()
		set(_terraengine_head_ref "${CMAKE_MATCH_1}")
		set(_terraengine_ref_roots "${_terraengine_git_dir}")
		if(EXISTS "${_terraengine_git_dir}/commondir")
			file(READ "${_terraengine_git_dir}/commondir" _terraengine_common_dir)
			string(STRIP "${_terraengine_common_dir}" _terraengine_common_dir)
			get_filename_component(
				_terraengine_common_dir
				"${_terraengine_common_dir}"
				ABSOLUTE
				BASE_DIR "${_terraengine_git_dir}")
			list(APPEND _terraengine_ref_roots "${_terraengine_common_dir}")
		endif()

		foreach(_terraengine_ref_root IN LISTS _terraengine_ref_roots)
			if(EXISTS "${_terraengine_ref_root}/${_terraengine_head_ref}")
				file(READ "${_terraengine_ref_root}/${_terraengine_head_ref}" _terraengine_revision)
				string(STRIP "${_terraengine_revision}" _terraengine_revision)
				break()
			endif()
			if(EXISTS "${_terraengine_ref_root}/packed-refs")
				file(STRINGS "${_terraengine_ref_root}/packed-refs" _terraengine_packed_refs)
				foreach(_terraengine_packed_ref IN LISTS _terraengine_packed_refs)
					if(_terraengine_packed_ref MATCHES "^([0-9a-fA-F]+)[ \t]+${_terraengine_head_ref}$")
						set(_terraengine_revision "${CMAKE_MATCH_1}")
						break()
					endif()
				endforeach()
			endif()
			if(DEFINED _terraengine_revision)
				break()
			endif()
		endforeach()
	endif()

	if(NOT DEFINED _terraengine_revision OR NOT _terraengine_revision MATCHES "^[0-9a-fA-F]+$")
		message(FATAL_ERROR "Could not resolve the checked-out revision for ${checkout_root} without invoking Git.")
	endif()
	string(LENGTH "${_terraengine_revision}" _terraengine_revision_length)
	if(NOT _terraengine_revision_length EQUAL 40)
		message(FATAL_ERROR "The resolved revision for ${checkout_root} is not a complete 40-character object id.")
	endif()
	string(TOLOWER "${_terraengine_revision}" _terraengine_revision)
	set(${output_variable} "${_terraengine_revision}" PARENT_SCOPE)
endfunction()

# miniaudio is compiled privately; its SDK headers never become an engine usage
# requirement or installed public surface.
function(terraengine_configure_miniaudio_dependency)
	set(_terraengine_miniaudio_root "${CMAKE_SOURCE_DIR}/External/miniaudio")
	set(TERRAENGINE_MINIAUDIO_REVISION "9634bedb5b5a2ca38c1ee7108a9358a4e233f14d")
	if(NOT EXISTS "${_terraengine_miniaudio_root}/miniaudio.c")
		message(FATAL_ERROR "miniaudio is missing. Run 'git submodule update --init --recursive External/miniaudio'.")
	endif()
	if(EXISTS "${_terraengine_miniaudio_root}/.git")
		terraengine_read_checkout_revision("${_terraengine_miniaudio_root}" TERRAENGINE_MINIAUDIO_ACTUAL_REVISION)
		if(NOT TERRAENGINE_MINIAUDIO_ACTUAL_REVISION STREQUAL TERRAENGINE_MINIAUDIO_REVISION)
			message(FATAL_ERROR "External/miniaudio must be revision ${TERRAENGINE_MINIAUDIO_REVISION}; found '${TERRAENGINE_MINIAUDIO_ACTUAL_REVISION}'.")
		endif()
	endif()
	add_library(TerraEngineAudioMiniaudioRuntime STATIC
		# The TerraEngine wrapper makes stb_vorbis declarations visible before
		# compiling miniaudio's implementation so the Ogg backend is registered.
		"${CMAKE_SOURCE_DIR}/Engine/Audio/Backend/MiniaudioImplementation.c"
		"${_terraengine_miniaudio_root}/extras/stb_vorbis.c"
	)
	target_include_directories(TerraEngineAudioMiniaudioRuntime PRIVATE "${_terraengine_miniaudio_root}")
	set_target_properties(TerraEngineAudioMiniaudioRuntime PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_SCAN_FOR_MODULES OFF)
	if(MSVC)
		target_compile_options(TerraEngineAudioMiniaudioRuntime PRIVATE /W0)
	else()
		target_compile_options(TerraEngineAudioMiniaudioRuntime PRIVATE -w)
	endif()
	set(TERRAENGINE_MINIAUDIO_ROOT "${_terraengine_miniaudio_root}" PARENT_SCOPE)
endfunction()

function(terraengine_configure_jolt_dependency)
	set(_terraengine_jolt_root "${CMAKE_SOURCE_DIR}/External/JoltPhysics")
	set(TERRAENGINE_JOLT_REVISION "2e28006e4a367624bb512bf2c37ca99a93d9d1ea")
	set(TERRAENGINE_JOLT_VERSION "5.6.1")

	if(NOT EXISTS "${_terraengine_jolt_root}/Build/CMakeLists.txt")
		message(FATAL_ERROR
			"Jolt Physics is missing. Initialize the pinned dependency with "
			"'git submodule update --init --recursive External/JoltPhysics'.")
	endif()

	# A Git checkout carries an exact object identity. Source archives may omit
	# Git metadata, so the version check below remains the portable fallback.
	if(EXISTS "${_terraengine_jolt_root}/.git")
		terraengine_read_checkout_revision(
			"${_terraengine_jolt_root}"
			TERRAENGINE_JOLT_ACTUAL_REVISION)
		if(NOT TERRAENGINE_JOLT_ACTUAL_REVISION STREQUAL TERRAENGINE_JOLT_REVISION)
			message(FATAL_ERROR
				"External/JoltPhysics must be revision ${TERRAENGINE_JOLT_REVISION}; "
				"found '${TERRAENGINE_JOLT_ACTUAL_REVISION}'.")
		endif()
	endif()

	file(READ "${_terraengine_jolt_root}/Jolt/Core/Core.h" TERRAENGINE_JOLT_CORE_HEADER)
	foreach(TERRAENGINE_VERSION_PAIR IN ITEMS
		"JPH_VERSION_MAJOR;5"
		"JPH_VERSION_MINOR;6"
		"JPH_VERSION_PATCH;1")
		list(GET TERRAENGINE_VERSION_PAIR 0 TERRAENGINE_VERSION_NAME)
		list(GET TERRAENGINE_VERSION_PAIR 1 TERRAENGINE_VERSION_VALUE)
		if(NOT TERRAENGINE_JOLT_CORE_HEADER MATCHES
			"#define[ \t]+${TERRAENGINE_VERSION_NAME}[ \t]+${TERRAENGINE_VERSION_VALUE}")
			message(FATAL_ERROR "External/JoltPhysics does not contain approved Jolt 5.6.1 headers.")
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
	if(NOT MSVC)
		# TerraEngine's job adapter derives from Jolt's JobSystemWithBarrier. GCC's
		# Itanium ABI records the base type information in the derived vtable, so
		# Jolt must emit that type information even though neither implementation
		# performs dynamic casts on the job-system boundary. MSVC does not require
		# the base symbol and keeps Jolt's lower-overhead default.
		set(CPP_RTTI_ENABLED ON CACHE BOOL "Emit Jolt base RTTI required by TerraEngine adapters" FORCE)
	endif()

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
	if(NOT MSVC)
		unset(CPP_RTTI_ENABLED CACHE)
	endif()

	# Return stable metadata to the parent directory for engine/test contracts.
	set(TERRAENGINE_JOLT_ROOT "${_terraengine_jolt_root}" PARENT_SCOPE)
	set(TERRAENGINE_JOLT_REVISION "${TERRAENGINE_JOLT_REVISION}" PARENT_SCOPE)
	set(TERRAENGINE_JOLT_VERSION "${TERRAENGINE_JOLT_VERSION}" PARENT_SCOPE)
endfunction()

# Keep the root build declarative: every production dependency is configured
# through this single entry point before engine targets are declared.
function(terraengine_configure_dependencies)
	terraengine_configure_jolt_dependency()
	terraengine_configure_miniaudio_dependency()
	set(TERRAENGINE_MINIAUDIO_ROOT "${TERRAENGINE_MINIAUDIO_ROOT}" PARENT_SCOPE)
endfunction()
