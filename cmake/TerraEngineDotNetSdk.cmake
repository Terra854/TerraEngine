# TerraEngineDotNetSdk.cmake - hash-pinned, repository-local .NET SDK bootstrap.
#
# Configure with the default AUTO mode to download the exact SDK selected by
# global.json. For an intentionally disconnected build, populate the SDK cache
# once and configure with -DTERRAENGINE_DOTNET_SDK_MODE=OFF.

include_guard(GLOBAL)

set(TERRAENGINE_DOTNET_SDK_LOCK
	"${CMAKE_SOURCE_DIR}/cmake/TerraEngineDotNetSdk.lock.json")
set(TERRAENGINE_DOTNET_GLOBAL_JSON "${CMAKE_SOURCE_DIR}/global.json")
set(TERRAENGINE_DOTNET_SDK_CACHE_ROOT
	"${CMAKE_SOURCE_DIR}/External/dotnet/sdk")
set(TERRAENGINE_DOTNET_SDK_MODE
	"AUTO"
	CACHE STRING
	"AUTO downloads a missing hash-pinned .NET SDK; OFF requires the repository-local SDK cache")
set_property(CACHE TERRAENGINE_DOTNET_SDK_MODE PROPERTY STRINGS AUTO OFF)

# Resolve one host archive into External/dotnet/sdk and return only absolute
# paths. Callers must never fall back to PATH because doing so would reintroduce
# the machine-wide SDK version mismatch this boundary is designed to prevent.
function(terraengine_resolve_bundled_dotnet runtime_id out_executable out_root)
	if(NOT TERRAENGINE_DOTNET_SDK_MODE STREQUAL "AUTO"
			AND NOT TERRAENGINE_DOTNET_SDK_MODE STREQUAL "OFF")
		message(FATAL_ERROR
			"TERRAENGINE_DOTNET_SDK_MODE must be AUTO or OFF, not "
			"'${TERRAENGINE_DOTNET_SDK_MODE}'.")
	endif()
	if(NOT EXISTS "${TERRAENGINE_DOTNET_SDK_LOCK}")
		message(FATAL_ERROR
			"TerraEngine's .NET SDK lock is missing: ${TERRAENGINE_DOTNET_SDK_LOCK}")
	endif()
	if(NOT EXISTS "${TERRAENGINE_DOTNET_GLOBAL_JSON}")
		message(FATAL_ERROR
			"TerraEngine's global.json is missing: ${TERRAENGINE_DOTNET_GLOBAL_JSON}")
	endif()

	file(READ "${TERRAENGINE_DOTNET_SDK_LOCK}" sdk_lock_json)
	file(READ "${TERRAENGINE_DOTNET_GLOBAL_JSON}" global_json)
	string(JSON sdk_version GET "${sdk_lock_json}" sdkVersion)
	string(JSON global_sdk_version GET "${global_json}" sdk version)
	if(NOT global_sdk_version STREQUAL sdk_version)
		message(FATAL_ERROR
			"global.json selects .NET SDK ${global_sdk_version}, but the audited "
			"SDK lock selects ${sdk_version}. Update both files together.")
	endif()

	string(JSON archive_json ERROR_VARIABLE archive_error
		GET "${sdk_lock_json}" archives "${runtime_id}")
	if(archive_error)
		message(FATAL_ERROR
			"The .NET SDK lock has no archive for host runtime ${runtime_id}.")
	endif()
	string(JSON archive_name GET
		"${sdk_lock_json}" archives "${runtime_id}" file)
	string(JSON archive_url GET
		"${sdk_lock_json}" archives "${runtime_id}" url)
	string(JSON archive_sha512 GET
		"${sdk_lock_json}" archives "${runtime_id}" sha512)
	if(NOT archive_url MATCHES "^https://")
		message(FATAL_ERROR
			"The .NET SDK archive for ${runtime_id} must use HTTPS.")
	endif()

	set(sdk_root
		"${TERRAENGINE_DOTNET_SDK_CACHE_ROOT}/${runtime_id}/${sdk_version}")
	if(WIN32)
		set(dotnet_executable "${sdk_root}/dotnet.exe")
	else()
		set(dotnet_executable "${sdk_root}/dotnet")
	endif()

	# Multiple build trees may configure concurrently. The source-local lock makes
	# download, extraction, and publication one atomic dependency operation.
	file(MAKE_DIRECTORY "${TERRAENGINE_DOTNET_SDK_CACHE_ROOT}")
	file(LOCK "${TERRAENGINE_DOTNET_SDK_CACHE_ROOT}/.bootstrap.lock"
		GUARD FUNCTION TIMEOUT 900)

	set(sdk_is_valid FALSE)
	if(EXISTS "${dotnet_executable}"
			AND IS_DIRECTORY "${sdk_root}/sdk/${sdk_version}")
		execute_process(
			COMMAND "${CMAKE_COMMAND}" -E env
				"DOTNET_ROOT=${sdk_root}"
				"DOTNET_MULTILEVEL_LOOKUP=0"
				"DOTNET_NOLOGO=1"
				"${dotnet_executable}" --version
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
			RESULT_VARIABLE version_result
			OUTPUT_VARIABLE observed_version
			ERROR_QUIET
			OUTPUT_STRIP_TRAILING_WHITESPACE)
		if(version_result EQUAL 0 AND observed_version STREQUAL sdk_version)
			set(sdk_is_valid TRUE)
		endif()
	endif()

	if(NOT sdk_is_valid)
		if(TERRAENGINE_DOTNET_SDK_MODE STREQUAL "OFF")
			message(FATAL_ERROR
				"The repository-local .NET SDK ${sdk_version} for ${runtime_id} is "
				"missing or damaged at ${sdk_root}. Reconfigure with "
				"-DTERRAENGINE_DOTNET_SDK_MODE=AUTO to bootstrap it.")
		endif()

		set(download_directory
			"${TERRAENGINE_DOTNET_SDK_CACHE_ROOT}/downloads")
		set(archive_path "${download_directory}/${archive_name}")
		set(staging_root
			"${TERRAENGINE_DOTNET_SDK_CACHE_ROOT}/.staging-${runtime_id}-${sdk_version}")
		file(MAKE_DIRECTORY "${download_directory}")
		message(STATUS
			"Bootstrapping repository-local .NET SDK ${sdk_version} for ${runtime_id}")
		file(DOWNLOAD
			"${archive_url}"
			"${archive_path}"
			EXPECTED_HASH "SHA512=${archive_sha512}"
			TLS_VERIFY ON
			SHOW_PROGRESS
			STATUS download_status)
		list(GET download_status 0 download_result)
		list(GET download_status 1 download_message)
		if(NOT download_result EQUAL 0)
			message(FATAL_ERROR
				"Could not download .NET SDK ${sdk_version}: ${download_message}")
		endif()

		# Extraction happens away from the published root. A failed configure can
		# therefore never leave a partial SDK that a later build mistakes as valid.
		file(REMOVE_RECURSE "${staging_root}")
		file(MAKE_DIRECTORY "${staging_root}")
		file(ARCHIVE_EXTRACT INPUT "${archive_path}" DESTINATION "${staging_root}")
		if(WIN32)
			set(staging_executable "${staging_root}/dotnet.exe")
		else()
			set(staging_executable "${staging_root}/dotnet")
		endif()
		if(NOT EXISTS "${staging_executable}"
				OR NOT IS_DIRECTORY "${staging_root}/sdk/${sdk_version}")
			message(FATAL_ERROR
				"The verified .NET SDK archive did not contain SDK ${sdk_version}.")
		endif()

		file(REMOVE_RECURSE "${sdk_root}")
		get_filename_component(sdk_parent "${sdk_root}" DIRECTORY)
		file(MAKE_DIRECTORY "${sdk_parent}")
		file(RENAME "${staging_root}" "${sdk_root}")
		file(REMOVE "${archive_path}")
	endif()

	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
			"DOTNET_ROOT=${sdk_root}"
			"DOTNET_MULTILEVEL_LOOKUP=0"
			"DOTNET_NOLOGO=1"
			"${dotnet_executable}" --version
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		RESULT_VARIABLE final_version_result
		OUTPUT_VARIABLE final_version
		ERROR_VARIABLE final_version_error
		OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(NOT final_version_result EQUAL 0 OR NOT final_version STREQUAL sdk_version)
		message(FATAL_ERROR
			"The repository-local dotnet executable failed validation. Expected "
			"${sdk_version}, observed '${final_version}'. ${final_version_error}")
	endif()

	message(STATUS
		"Using repository-local .NET SDK ${sdk_version}: ${dotnet_executable}")
	set(${out_executable} "${dotnet_executable}" PARENT_SCOPE)
	set(${out_root} "${sdk_root}" PARENT_SCOPE)
endfunction()
