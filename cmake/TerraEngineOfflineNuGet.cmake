# TerraEngine's managed targets restore exclusively from the audited repository
# feed. Keeping verification and restore setup in one module prevents individual
# CMake targets from accidentally re-enabling a user or network NuGet source.
include_guard(GLOBAL)

set(TERRAENGINE_OFFLINE_NUGET_MODULE "${CMAKE_CURRENT_LIST_FILE}")
set(TERRAENGINE_OFFLINE_NUGET_ROOT "${CMAKE_SOURCE_DIR}/.nuget")
set(TERRAENGINE_OFFLINE_NUGET_FEED "${TERRAENGINE_OFFLINE_NUGET_ROOT}/feed")
set(TERRAENGINE_OFFLINE_NUGET_METADATA_ROOT
	"${TERRAENGINE_OFFLINE_NUGET_ROOT}")
set(TERRAENGINE_OFFLINE_NUGET_LOCK
	"${CMAKE_SOURCE_DIR}/cmake/TerraEngineNuGet.lock.json")
set(TERRAENGINE_OFFLINE_NUGET_MANIFEST
	"${TERRAENGINE_OFFLINE_NUGET_METADATA_ROOT}/manifest.json")
set(TERRAENGINE_OFFLINE_NUGET_CONFIG "${CMAKE_SOURCE_DIR}/NuGet.Config")
set(TERRAENGINE_NATIVEAOT_NUGET_CONFIG
	"${TERRAENGINE_OFFLINE_NUGET_METADATA_ROOT}/NativeAotNuGet.Config")
set(TERRAENGINE_DOTNET_GLOBAL_JSON "${CMAKE_SOURCE_DIR}/global.json")
set(TERRAENGINE_NUGET_CACHE_MODE
	"AUTO"
	CACHE STRING
	"AUTO regenerates missing or damaged pinned NuGet archives; OFF requires a complete existing cache")
set_property(CACHE TERRAENGINE_NUGET_CACHE_MODE
	PROPERTY STRINGS AUTO OFF)

# Populate only absent or invalid package archives. The tracked lock contains an
# HTTPS source and SHA-256 for every archive, so AUTO mode can repair a fresh or
# damaged disposable cache without consulting an ambient NuGet configuration.
function(terraengine_prepare_offline_nuget_feed)
	if(NOT EXISTS "${TERRAENGINE_OFFLINE_NUGET_LOCK}")
		message(FATAL_ERROR
			"TerraEngine NuGet lock is missing: "
			"${TERRAENGINE_OFFLINE_NUGET_LOCK}")
	endif()
	if(NOT TERRAENGINE_NUGET_CACHE_MODE STREQUAL "AUTO"
			AND NOT TERRAENGINE_NUGET_CACHE_MODE STREQUAL "OFF")
		message(FATAL_ERROR
			"TERRAENGINE_NUGET_CACHE_MODE must be AUTO or OFF, not "
			"'${TERRAENGINE_NUGET_CACHE_MODE}'.")
	endif()

	file(READ "${TERRAENGINE_OFFLINE_NUGET_LOCK}" manifest_json)
	string(JSON package_count LENGTH "${manifest_json}" packages)
	file(MAKE_DIRECTORY "${TERRAENGINE_OFFLINE_NUGET_FEED}")
	math(EXPR package_last "${package_count} - 1")
	foreach(package_index RANGE 0 ${package_last})
		string(JSON package_id GET
			"${manifest_json}" packages ${package_index} id)
		string(JSON package_version GET
			"${manifest_json}" packages ${package_index} version)
		string(JSON package_relative_path GET
			"${manifest_json}" packages ${package_index} file)
		string(JSON expected_sha256 GET
			"${manifest_json}" packages ${package_index} sha256)
		string(JSON source_url GET
			"${manifest_json}" packages ${package_index} sourceUrl)
		set(package_path
			"${TERRAENGINE_OFFLINE_NUGET_ROOT}/${package_relative_path}")
		set(package_valid FALSE)
		if(EXISTS "${package_path}")
			file(SHA256 "${package_path}" observed_sha256)
			string(TOLOWER "${observed_sha256}" observed_sha256)
			if(observed_sha256 STREQUAL expected_sha256)
				set(package_valid TRUE)
			endif()
		endif()
		if(package_valid)
			continue()
		endif()
		if(TERRAENGINE_NUGET_CACHE_MODE STREQUAL "OFF")
			message(FATAL_ERROR
				"NuGet cache entry is missing or damaged for "
				"${package_id} ${package_version}. Reconfigure with "
				"-DTERRAENGINE_NUGET_CACHE_MODE=AUTO to regenerate it.")
		endif()
		if(NOT source_url MATCHES "^https://")
			message(FATAL_ERROR
				"NuGet lock source must use HTTPS for "
				"${package_id} ${package_version}.")
		endif()

		set(download_path "${package_path}.download")
		file(REMOVE "${download_path}")
		message(STATUS
			"Regenerating NuGet cache entry "
			"${package_id} ${package_version}")
		file(DOWNLOAD
			"${source_url}"
			"${download_path}"
			EXPECTED_HASH "SHA256=${expected_sha256}"
			TLS_VERIFY ON
			STATUS download_status)
		list(GET download_status 0 download_result)
		list(GET download_status 1 download_message)
		if(NOT download_result EQUAL 0)
			file(REMOVE "${download_path}")
			message(FATAL_ERROR
				"Failed to regenerate ${package_id} ${package_version}: "
				"${download_message}")
		endif()
		# The replacement is already hash-verified. Remove a damaged old entry
		# only after its replacement has downloaded successfully.
		file(REMOVE "${package_path}")
		file(RENAME "${download_path}" "${package_path}")
	endforeach()
endfunction()

# Generated text files are rewritten only when their bytes change. Preserving
# timestamps keeps CMake's dependency graph warm across ordinary reconfigure
# passes and prevents an unnecessary managed restore.
function(terraengine_write_if_different path content)
	set(write_file TRUE)
	if(EXISTS "${path}")
		file(READ "${path}" existing_content)
		if(existing_content STREQUAL content)
			set(write_file FALSE)
		endif()
	endif()
	if(write_file)
		get_filename_component(parent_directory "${path}" DIRECTORY)
		file(MAKE_DIRECTORY "${parent_directory}")
		file(WRITE "${path}" "${content}")
	endif()
endfunction()

# Extract one legal file from a hash-verified package archive and prove its
# independent legal-file lock before publishing it into the generated cache.
function(terraengine_extract_locked_legal_file
		package_path archive_entry destination expected_bytes expected_sha256)
	set(extraction_root
		"${TERRAENGINE_OFFLINE_NUGET_ROOT}/.legal-extract")
	file(REMOVE_RECURSE "${extraction_root}")
	file(MAKE_DIRECTORY "${extraction_root}")
	file(ARCHIVE_EXTRACT
		INPUT "${package_path}"
		DESTINATION "${extraction_root}"
		PATTERNS "${archive_entry}")
	set(extracted_path "${extraction_root}/${archive_entry}")
	if(NOT EXISTS "${extracted_path}")
		message(FATAL_ERROR
			"NuGet package legal entry is missing: ${archive_entry}")
	endif()
	file(SIZE "${extracted_path}" observed_bytes)
	file(SHA256 "${extracted_path}" observed_sha256)
	string(TOLOWER "${observed_sha256}" observed_sha256)
	if(NOT observed_bytes EQUAL expected_bytes
			OR NOT observed_sha256 STREQUAL expected_sha256)
		message(FATAL_ERROR
			"NuGet package legal entry does not match its lock: "
			"${archive_entry}")
	endif()
	get_filename_component(destination_directory "${destination}" DIRECTORY)
	file(MAKE_DIRECTORY "${destination_directory}")
	file(COPY_FILE "${extracted_path}" "${destination}"
		ONLY_IF_DIFFERENT)
endfunction()

# Everything below .nuget is generated from one tracked lock. Package archives
# are downloaded above; configs and notices are deterministic text, while legal
# files are extracted from their owning hash-verified archives.
function(terraengine_generate_offline_nuget_metadata)
	file(READ "${TERRAENGINE_OFFLINE_NUGET_LOCK}" lock_json)
	file(MAKE_DIRECTORY "${TERRAENGINE_OFFLINE_NUGET_ROOT}")
	configure_file(
		"${TERRAENGINE_OFFLINE_NUGET_LOCK}"
		"${TERRAENGINE_OFFLINE_NUGET_MANIFEST}"
		COPYONLY)

	set(nativeaot_config [=[<?xml version="1.0" encoding="utf-8"?>
<configuration>
	<packageSources>
		<clear />
		<add key="TerraEngineNativeAotOffline" value="feed" />
	</packageSources>
	<config>
		<add key="signatureValidationMode" value="accept" />
	</config>
</configuration>
]=])
	terraengine_write_if_different(
		"${TERRAENGINE_NATIVEAOT_NUGET_CONFIG}"
		"${nativeaot_config}")

	set(cache_readme [=[# TerraEngine Generated NuGet Cache

This directory is regenerated by `cmake/TerraEngineOfflineNuGet.cmake`.
Do not edit or commit its contents. The authoritative package IDs, versions,
HTTPS sources, hashes, and legal-file inventory live in
`cmake/TerraEngineNuGet.lock.json`.

Use `TERRAENGINE_NUGET_CACHE_MODE=AUTO` to repair missing or damaged archives,
or `TERRAENGINE_NUGET_CACHE_MODE=OFF` for a strictly offline fail-closed build.
]=])
	terraengine_write_if_different(
		"${TERRAENGINE_OFFLINE_NUGET_ROOT}/README.md"
		"${cache_readme}")

	# Shared legal text may not be embedded in every package that references it.
	# The lock therefore names one audited archive entry as its canonical source.
	string(JSON generated_legal_count LENGTH
		"${lock_json}" generatedLegalFiles)
	if(generated_legal_count GREATER 0)
		math(EXPR generated_legal_last "${generated_legal_count} - 1")
		foreach(generated_index RANGE 0 ${generated_legal_last})
			string(JSON legal_relative_path GET
				"${lock_json}" generatedLegalFiles ${generated_index} path)
			string(JSON package_relative_path GET
				"${lock_json}" generatedLegalFiles ${generated_index}
				packageFile)
			string(JSON archive_entry GET
				"${lock_json}" generatedLegalFiles ${generated_index}
				archiveEntry)
			string(JSON expected_bytes GET
				"${lock_json}" generatedLegalFiles ${generated_index} bytes)
			string(JSON expected_sha256 GET
				"${lock_json}" generatedLegalFiles ${generated_index} sha256)
			set(legal_path
				"${TERRAENGINE_OFFLINE_NUGET_ROOT}/${legal_relative_path}")
			set(legal_valid FALSE)
			if(EXISTS "${legal_path}")
				file(SIZE "${legal_path}" observed_bytes)
				file(SHA256 "${legal_path}" observed_sha256)
				string(TOLOWER "${observed_sha256}" observed_sha256)
				if(observed_bytes EQUAL expected_bytes
						AND observed_sha256 STREQUAL expected_sha256)
					set(legal_valid TRUE)
				endif()
			endif()
			if(NOT legal_valid)
				terraengine_extract_locked_legal_file(
					"${TERRAENGINE_OFFLINE_NUGET_ROOT}/${package_relative_path}"
					"${archive_entry}"
					"${legal_path}"
					"${expected_bytes}"
					"${expected_sha256}")
			endif()
		endforeach()
	endif()

	string(CONCAT notice_text
		"# TerraEngine Offline NuGet Notices\n\n"
		"The following signed NuGet archives are cached for managed builds "
		"and installed NativeAOT export.\n")
	string(JSON package_count LENGTH "${lock_json}" packages)
	math(EXPR package_last "${package_count} - 1")
	foreach(package_index RANGE 0 ${package_last})
		string(JSON package_id GET
			"${lock_json}" packages ${package_index} id)
		string(JSON package_version GET
			"${lock_json}" packages ${package_index} version)
		string(JSON package_relative_path GET
			"${lock_json}" packages ${package_index} file)
		string(JSON source_url GET
			"${lock_json}" packages ${package_index} sourceUrl)
		string(JSON license_value GET
			"${lock_json}" packages ${package_index} license value)
		string(JSON primary_license GET
			"${lock_json}" packages ${package_index} license primaryFile)
		string(APPEND notice_text
			"\n## ${package_id} ${package_version}\n\n"
			"- Source: ${source_url}\n"
			"- License: ${license_value}\n"
			"- Primary license: ${primary_license}\n")

		string(JSON legal_file_count LENGTH
			"${lock_json}" packages ${package_index} legalFiles)
		if(legal_file_count GREATER 0)
			math(EXPR legal_file_last "${legal_file_count} - 1")
			foreach(legal_index RANGE 0 ${legal_file_last})
				string(JSON legal_relative_path GET
					"${lock_json}" packages ${package_index}
					legalFiles ${legal_index} path)
				string(JSON expected_bytes GET
					"${lock_json}" packages ${package_index}
					legalFiles ${legal_index} bytes)
				string(JSON expected_sha256 GET
					"${lock_json}" packages ${package_index}
					legalFiles ${legal_index} sha256)
				set(legal_path
					"${TERRAENGINE_OFFLINE_NUGET_ROOT}/${legal_relative_path}")
				set(legal_valid FALSE)
				if(EXISTS "${legal_path}")
					file(SIZE "${legal_path}" observed_bytes)
					file(SHA256 "${legal_path}" observed_sha256)
					string(TOLOWER "${observed_sha256}" observed_sha256)
					if(observed_bytes EQUAL expected_bytes
							AND observed_sha256 STREQUAL expected_sha256)
						set(legal_valid TRUE)
					endif()
				endif()
				if(NOT legal_valid)
					get_filename_component(archive_entry
						"${legal_relative_path}" NAME)
					terraengine_extract_locked_legal_file(
						"${TERRAENGINE_OFFLINE_NUGET_ROOT}/${package_relative_path}"
						"${archive_entry}"
						"${legal_path}"
						"${expected_bytes}"
						"${expected_sha256}")
				endif()
				if(NOT legal_relative_path STREQUAL primary_license)
					string(APPEND notice_text
						"- Additional notice: ${legal_relative_path}\n")
				endif()
			endforeach()
		endif()
	endforeach()
	terraengine_write_if_different(
		"${TERRAENGINE_OFFLINE_NUGET_ROOT}/NOTICE.md"
		"${notice_text}")
	file(REMOVE_RECURSE
		"${TERRAENGINE_OFFLINE_NUGET_ROOT}/.legal-extract")
endfunction()

# Verify package and legal-file bytes before NuGet sees the feed. This makes a
# partial copy, accidental update, or unreviewed extra archive a configure-time
# error with a concrete filename.
function(terraengine_verify_offline_nuget_feed runtime_id)
	terraengine_prepare_offline_nuget_feed()
	terraengine_generate_offline_nuget_metadata()
	if(NOT EXISTS "${TERRAENGINE_OFFLINE_NUGET_CONFIG}")
		message(FATAL_ERROR
			"TerraEngine's fail-closed NuGet.Config is missing: "
			"${TERRAENGINE_OFFLINE_NUGET_CONFIG}")
	endif()
	if(NOT EXISTS "${TERRAENGINE_NATIVEAOT_NUGET_CONFIG}")
		message(FATAL_ERROR
			"TerraEngine's installed NativeAOT NuGet.Config is missing: "
			"${TERRAENGINE_NATIVEAOT_NUGET_CONFIG}")
	endif()
	if(NOT EXISTS "${TERRAENGINE_DOTNET_GLOBAL_JSON}")
		message(FATAL_ERROR
			"TerraEngine's pinned global.json is missing: "
			"${TERRAENGINE_DOTNET_GLOBAL_JSON}")
	endif()

	file(READ "${TERRAENGINE_OFFLINE_NUGET_MANIFEST}" manifest_json)
	string(JSON manifest_schema GET "${manifest_json}" schemaVersion)
	if(NOT manifest_schema EQUAL 1)
		message(FATAL_ERROR
			"Unsupported TerraEngine offline NuGet manifest schema: "
			"${manifest_schema}")
	endif()

	file(READ "${TERRAENGINE_DOTNET_GLOBAL_JSON}" global_json)
	string(JSON manifest_sdk GET "${manifest_json}" dotnetSdkVersion)
	string(JSON configured_sdk GET "${global_json}" sdk version)
	string(JSON configured_roll_forward GET "${global_json}" sdk rollForward)
	if(NOT configured_sdk STREQUAL manifest_sdk
			OR NOT configured_roll_forward STREQUAL "disable")
		message(FATAL_ERROR
			"global.json must pin offline-feed SDK ${manifest_sdk} with "
			"rollForward=disable; observed ${configured_sdk} and "
			"${configured_roll_forward}.")
	endif()

	# Export pins RestoreSources to the verified installed feed after all
	# profile arguments. NuGet gives that absolute override a different source
	# identity than the relative configured source, so source mapping would
	# reject every package even with a wildcard. Keep the runtime config
	# single-source and mapping-free; repository restores use the root config.
	file(READ "${TERRAENGINE_NATIVEAOT_NUGET_CONFIG}" nativeaot_config)
	if(NOT nativeaot_config MATCHES "<clear[ \t]*/>"
			OR NOT nativeaot_config MATCHES
				"key=\"TerraEngineNativeAotOffline\"[ \t]+value=\"feed\""
			OR nativeaot_config MATCHES "<packageSourceMapping>")
		message(FATAL_ERROR
			"NativeAotNuGet.Config must clear ambient sources, name only the "
			"relative installed feed, and omit packageSourceMapping because "
			"export pins the same feed through RestoreSources.")
	endif()

	string(JSON supported_rid_count LENGTH
		"${manifest_json}" supportedRuntimeIdentifiers)
	set(runtime_supported FALSE)
	if(supported_rid_count GREATER 0)
		math(EXPR supported_rid_last "${supported_rid_count} - 1")
		foreach(rid_index RANGE 0 ${supported_rid_last})
			string(JSON supported_rid GET
				"${manifest_json}" supportedRuntimeIdentifiers ${rid_index})
			if(supported_rid STREQUAL runtime_id)
				set(runtime_supported TRUE)
			endif()
		endforeach()
	endif()
	if(NOT runtime_supported)
		message(WARNING
			"The repository NuGet feed does not yet contain runtime packs for "
			"${runtime_id}. Managed restore will remain local-only and fail "
			"closed until that RID is audited and vendored.")
	endif()

	string(JSON declared_package_count GET "${manifest_json}" packageCount)
	string(JSON package_count LENGTH "${manifest_json}" packages)
	if(NOT declared_package_count EQUAL package_count OR package_count LESS 1)
		message(FATAL_ERROR
			"Offline NuGet packageCount does not match the package inventory.")
	endif()

	set(expected_package_names)
	set(observed_total_bytes 0)
	math(EXPR package_last "${package_count} - 1")
	foreach(package_index RANGE 0 ${package_last})
		string(JSON package_id GET
			"${manifest_json}" packages ${package_index} id)
		string(JSON package_version GET
			"${manifest_json}" packages ${package_index} version)
		string(JSON package_relative_path GET
			"${manifest_json}" packages ${package_index} file)
		string(JSON expected_bytes GET
			"${manifest_json}" packages ${package_index} bytes)
		string(JSON expected_sha256 GET
			"${manifest_json}" packages ${package_index} sha256)
		string(JSON expected_sha512 GET
			"${manifest_json}" packages ${package_index} sha512)
		string(JSON signature_present GET
			"${manifest_json}" packages ${package_index} signatureEntryPresent)

		if(NOT package_relative_path MATCHES "^feed/[^/]+[.]nupkg$"
				OR NOT signature_present)
			message(FATAL_ERROR
				"Invalid manifest entry for ${package_id} ${package_version}.")
		endif()

		set(package_path
			"${TERRAENGINE_OFFLINE_NUGET_ROOT}/${package_relative_path}")
		if(NOT EXISTS "${package_path}")
			message(FATAL_ERROR
				"Offline NuGet package is missing: ${package_relative_path}")
		endif()

		file(SIZE "${package_path}" observed_bytes)
		file(SHA256 "${package_path}" observed_sha256)
		file(SHA512 "${package_path}" observed_sha512)
		string(TOLOWER "${observed_sha256}" observed_sha256)
		string(TOLOWER "${observed_sha512}" observed_sha512)
		if(NOT observed_bytes EQUAL expected_bytes
				OR NOT observed_sha256 STREQUAL expected_sha256
				OR NOT observed_sha512 STREQUAL expected_sha512)
			message(FATAL_ERROR
				"Offline NuGet package hash/size mismatch: "
				"${package_relative_path}")
		endif()

		math(EXPR observed_total_bytes
			"${observed_total_bytes} + ${observed_bytes}")
		get_filename_component(package_name "${package_path}" NAME)
		list(APPEND expected_package_names "${package_name}")

		string(JSON primary_license GET
			"${manifest_json}" packages ${package_index} license primaryFile)
		if(primary_license STREQUAL ""
				OR NOT EXISTS
					"${TERRAENGINE_OFFLINE_NUGET_METADATA_ROOT}/${primary_license}")
			message(FATAL_ERROR
				"Primary license is missing for "
				"${package_id} ${package_version}.")
		endif()

		string(JSON legal_file_count LENGTH
			"${manifest_json}" packages ${package_index} legalFiles)
		if(legal_file_count GREATER 0)
			math(EXPR legal_file_last "${legal_file_count} - 1")
			foreach(legal_index RANGE 0 ${legal_file_last})
				string(JSON legal_relative_path GET
					"${manifest_json}" packages ${package_index}
					legalFiles ${legal_index} path)
				string(JSON legal_expected_bytes GET
					"${manifest_json}" packages ${package_index}
					legalFiles ${legal_index} bytes)
				string(JSON legal_expected_sha256 GET
					"${manifest_json}" packages ${package_index}
					legalFiles ${legal_index} sha256)
				set(legal_path
					"${TERRAENGINE_OFFLINE_NUGET_METADATA_ROOT}/${legal_relative_path}")
				if(NOT EXISTS "${legal_path}")
					message(FATAL_ERROR
						"Offline NuGet legal file is missing: "
						"${legal_relative_path}")
				endif()
				file(SIZE "${legal_path}" legal_observed_bytes)
				file(SHA256 "${legal_path}" legal_observed_sha256)
				string(TOLOWER
					"${legal_observed_sha256}" legal_observed_sha256)
				if(NOT legal_observed_bytes EQUAL legal_expected_bytes
						OR NOT legal_observed_sha256
							STREQUAL legal_expected_sha256)
					message(FATAL_ERROR
						"Offline NuGet legal-file hash/size mismatch: "
						"${legal_relative_path}")
				endif()
			endforeach()
		endif()
	endforeach()

	string(JSON declared_total_bytes GET "${manifest_json}" totalBytes)
	if(NOT observed_total_bytes EQUAL declared_total_bytes)
		message(FATAL_ERROR
			"Offline NuGet totalBytes does not match the verified archives.")
	endif()

	# The feed is intentionally flat and archive-only. Reject any unexpected
	# file or directory, including one whose extension differs only by case.
	file(GLOB actual_package_names
		RELATIVE "${TERRAENGINE_OFFLINE_NUGET_FEED}"
		"${TERRAENGINE_OFFLINE_NUGET_FEED}/*")
	list(SORT expected_package_names)
	list(SORT actual_package_names)
	if(NOT "${actual_package_names}" STREQUAL "${expected_package_names}")
		list(JOIN expected_package_names ", " expected_package_text)
		list(JOIN actual_package_names ", " actual_package_text)
		message(FATAL_ERROR
			"Offline NuGet feed does not exactly match its manifest. "
			"Expected [${expected_package_text}], observed "
			"[${actual_package_text}].")
	endif()
endfunction()

# Verify the isolated build-tree package cache after restore. NuGet's global
# package layout keeps the original signed archive beside each extracted
# package, so the feed manifest can prove that regeneration produced the exact
# audited bytes rather than merely creating a directory with the expected name.
function(terraengine_verify_nuget_package_cache package_cache)
	if(NOT IS_DIRECTORY "${package_cache}")
		message(FATAL_ERROR
			"TerraEngine NuGet package cache is missing: ${package_cache}")
	endif()

	file(READ "${TERRAENGINE_OFFLINE_NUGET_MANIFEST}" manifest_json)
	string(JSON package_count LENGTH "${manifest_json}" packages)
	set(expected_metadata_paths)
	math(EXPR package_last "${package_count} - 1")
	foreach(package_index RANGE 0 ${package_last})
		string(JSON package_id GET
			"${manifest_json}" packages ${package_index} id)
		string(JSON package_version GET
			"${manifest_json}" packages ${package_index} version)
		string(JSON expected_sha512 GET
			"${manifest_json}" packages ${package_index} sha512)
		# The feed is a cross-platform superset, but NuGet restores only the
		# runtime packs selected by the active RID. Verify that exact host subset
		# while terraengine_verify_offline_nuget_feed continues to hash every
		# archive in the shared audited feed.
		string(TOLOWER "${package_id}" normalized_package_id)
		if(TERRAENGINE_RUNTIME_ID STREQUAL "linux-x64"
				AND (normalized_package_id MATCHES "[.]win-x64$"
					OR normalized_package_id MATCHES "^runtime[.]win-x64[.]"))
			continue()
		endif()
		if(TERRAENGINE_RUNTIME_ID STREQUAL "win-x64"
				AND (normalized_package_id MATCHES "[.]linux-x64$"
					OR normalized_package_id MATCHES "^runtime[.]linux-x64[.]"))
			continue()
		endif()
		string(TOLOWER "${package_id}" package_directory)
		set(package_version_directory
			"${package_cache}/${package_directory}/${package_version}")
		set(package_metadata
			"${package_version_directory}/.nupkg.metadata")
		set(package_archive
			"${package_version_directory}/${package_directory}.${package_version}.nupkg")
		if(NOT EXISTS "${package_metadata}" OR NOT EXISTS "${package_archive}")
			message(FATAL_ERROR
				"Regenerated NuGet cache is incomplete for "
				"${package_id} ${package_version}.")
		endif()
		file(SHA512 "${package_archive}" observed_sha512)
		string(TOLOWER "${observed_sha512}" observed_sha512)
		if(NOT observed_sha512 STREQUAL expected_sha512)
			message(FATAL_ERROR
				"Regenerated NuGet cache hash mismatch for "
				"${package_id} ${package_version}.")
		endif()
		list(APPEND expected_metadata_paths "${package_metadata}")
	endforeach()

	file(GLOB_RECURSE actual_metadata_paths
		LIST_DIRECTORIES FALSE
		"${package_cache}/.nupkg.metadata"
		"${package_cache}/*/.nupkg.metadata"
		"${package_cache}/*/*/.nupkg.metadata")
	list(SORT expected_metadata_paths)
	list(SORT actual_metadata_paths)
	if(NOT "${actual_metadata_paths}" STREQUAL "${expected_metadata_paths}")
		message(FATAL_ERROR
			"Regenerated NuGet cache contains an unexpected package version.")
	endif()
endfunction()

# Add one serialized restore boundary for all managed targets. Downstream build
# and publish commands use --no-restore so no target can independently select a
# user or network package source.
function(terraengine_add_offline_nuget_restore_target target_name runtime_id)
	if(TARGET "${target_name}")
		message(FATAL_ERROR
			"Offline NuGet restore target already exists: ${target_name}")
	endif()

	terraengine_verify_offline_nuget_feed("${runtime_id}")
	# The root configure resolves an absolute, hash-verified SDK executable.
	# Refuse an unset or missing path instead of silently consulting PATH.
	if(NOT DEFINED TERRAENGINE_DOTNET_EXECUTABLE
			OR NOT EXISTS "${TERRAENGINE_DOTNET_EXECUTABLE}")
		message(FATAL_ERROR
			"The repository-local .NET SDK was not resolved before configuring "
			"the managed restore boundary.")
	endif()
	set(dotnet_executable "${TERRAENGINE_DOTNET_EXECUTABLE}")

	set(package_cache "${CMAKE_BINARY_DIR}/NuGetPackages")
	set(dotnet_cli_home "${CMAKE_BINARY_DIR}/DotNetCliHome")
	set(dotnet_environment
		"NUGET_PACKAGES=${package_cache}"
		"DOTNET_ROOT=${TERRAENGINE_DOTNET_SDK_ROOT}"
		"DOTNET_MULTILEVEL_LOOKUP=0"
		"DOTNET_CLI_HOME=${dotnet_cli_home}"
		"DOTNET_CLI_TELEMETRY_OPTOUT=1"
		"DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1"
		"DOTNET_NOLOGO=1"
		"NUGET_XMLDOC_MODE=skip"
		"NUGET_CERT_REVOCATION_MODE=offline")
	set(dotnet_command
		"${CMAKE_COMMAND}" -E env
		${dotnet_environment}
		"${dotnet_executable}")
	set(restore_arguments
		--configfile "${TERRAENGINE_OFFLINE_NUGET_CONFIG}"
		--packages "${package_cache}"
		--no-http-cache
		--disable-parallel
		--verbosity minimal
		"-p:RestoreSources=${TERRAENGINE_OFFLINE_NUGET_FEED}"
		"-p:RestoreFallbackFolders="
		"-p:RestoreAdditionalProjectSources="
		"-p:RestoreIgnoreFailedSources=false"
		"-p:NuGetAudit=false")
	set(hexa_compatibility_arguments
		"-p:IsAotCompatible=false"
		"-p:EnableTrimAnalyzer=false"
		"-p:EnableSingleFileAnalyzer=false"
		"-p:EnableAotAnalyzer=false")

	# Model restore as a generated cache instead of an always-dirty target.
	# Removing the cache removes this stamp, while changing any audited archive,
	# restore policy, SDK pin, or participating project makes the command stale.
	# Package metadata files are outputs too, so a partially deleted cache also
	# triggers one clean regeneration at the next managed build boundary.
	set(restore_stamp
		"${package_cache}/.terraengine-managed-restore.stamp")
	set(restore_inputs
		"${TERRAENGINE_OFFLINE_NUGET_MODULE}"
		"${TERRAENGINE_OFFLINE_NUGET_LOCK}"
		"${TERRAENGINE_OFFLINE_NUGET_MANIFEST}"
		"${TERRAENGINE_OFFLINE_NUGET_CONFIG}"
		"${TERRAENGINE_NATIVEAOT_NUGET_CONFIG}"
		"${TERRAENGINE_DOTNET_GLOBAL_JSON}"
		"${CMAKE_SOURCE_DIR}/Engine/Managed/TerraEngine.csproj"
		"${CMAKE_SOURCE_DIR}/Editor/Managed/TerraEngine.Export.Generator/TerraEngine.Export.Generator.csproj"
		"${CMAKE_SOURCE_DIR}/External/Hexa.NET.ImGui/Hexa.NET.ImGui/Hexa.NET.ImGui.csproj"
		"${CMAKE_SOURCE_DIR}/Editor/Managed/TerraEngine.Editor/TerraEngine.Editor.csproj")
	set(package_cache_metadata)
	file(READ "${TERRAENGINE_OFFLINE_NUGET_MANIFEST}" restore_manifest_json)
	string(JSON restore_package_count LENGTH
		"${restore_manifest_json}" packages)
	math(EXPR restore_package_last "${restore_package_count} - 1")
	foreach(package_index RANGE 0 ${restore_package_last})
		string(JSON package_id GET
			"${restore_manifest_json}" packages ${package_index} id)
		string(JSON package_version GET
			"${restore_manifest_json}" packages ${package_index} version)
		string(JSON package_relative_path GET
			"${restore_manifest_json}" packages ${package_index} file)
		string(TOLOWER "${package_id}" package_directory)
		list(APPEND package_cache_metadata
			"${package_cache}/${package_directory}/${package_version}/.nupkg.metadata")
		list(APPEND restore_inputs
			"${TERRAENGINE_OFFLINE_NUGET_ROOT}/${package_relative_path}")
	endforeach()
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		${restore_inputs})

	add_custom_command(
		OUTPUT "${restore_stamp}" ${package_cache_metadata}
		# Verify before deleting the last usable cache. A damaged source feed must
		# fail closed without destroying the previously restored package graph.
		COMMAND "${CMAKE_COMMAND}"
			"-DTERRAENGINE_RUNTIME_ID=${runtime_id}"
			-P "${TERRAENGINE_OFFLINE_NUGET_MODULE}"
		COMMAND "${CMAKE_COMMAND}" -E rm -rf "${package_cache}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"${package_cache}" "${dotnet_cli_home}"
		COMMAND ${dotnet_command} restore
			"${CMAKE_SOURCE_DIR}/Engine/Managed/TerraEngine.csproj"
			--runtime "${runtime_id}"
			# NativeAOT is an SDK-provided implicit package graph. Request it during
			# the normal restore boundary so a missing compiler or linker archive
			# fails here, rather than later from an installed editor export.
			"-p:PublishAot=true"
			${restore_arguments}
		COMMAND ${dotnet_command} restore
			"${CMAKE_SOURCE_DIR}/Editor/Managed/TerraEngine.Export.Generator/TerraEngine.Export.Generator.csproj"
			${restore_arguments}
		# Restore only the Hexa target framework consumed by the editor. Walking
		# every target framework would request old runtime packs and ILLink
		# analyzers that cannot participate in TerraEngine's .NET 10 publish.
		COMMAND ${dotnet_command} restore
			"${CMAKE_SOURCE_DIR}/External/Hexa.NET.ImGui/Hexa.NET.ImGui/Hexa.NET.ImGui.csproj"
			"-p:TargetFramework=netstandard2.0"
			--runtime "${runtime_id}"
			${hexa_compatibility_arguments}
			${restore_arguments}
		# The project reference now has a verified assets file, so restoring the
		# editor root without dependencies avoids a second all-framework walk.
		COMMAND ${dotnet_command} restore
			"${CMAKE_SOURCE_DIR}/Editor/Managed/TerraEngine.Editor/TerraEngine.Editor.csproj"
			--runtime "${runtime_id}"
			--no-dependencies
			${hexa_compatibility_arguments}
			${restore_arguments}
		COMMAND "${CMAKE_COMMAND}"
			"-DTERRAENGINE_RUNTIME_ID=${runtime_id}"
			"-DTERRAENGINE_NUGET_PACKAGE_CACHE=${package_cache}"
			-P "${TERRAENGINE_OFFLINE_NUGET_MODULE}"
		COMMAND "${CMAKE_COMMAND}" -E touch "${restore_stamp}"
		DEPENDS ${restore_inputs}
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		COMMENT
			"Regenerating the isolated managed NuGet package cache"
		VERBATIM
		USES_TERMINAL)
	add_custom_target("${target_name}"
		DEPENDS "${restore_stamp}" ${package_cache_metadata})

	# Return the exact environment-prefixed command to the caller so build and
	# publish targets read the same isolated package cache as restore.
	set(TERRAENGINE_DOTNET_COMMAND
		"${dotnet_command}"
		PARENT_SCOPE)
	set(TERRAENGINE_OFFLINE_NUGET_PACKAGE_CACHE
		"${package_cache}"
		PARENT_SCOPE)
	set(TERRAENGINE_DOTNET_HEXA_COMPATIBILITY_ARGUMENTS
		"${hexa_compatibility_arguments}"
		PARENT_SCOPE)
endfunction()

# Redistributed managed binaries retain their package licenses and notices in
# the installed tree. The NativeAOT compiler feed is installed separately below
# Editor because gameplay export must restore without a user or network source.
function(terraengine_install_offline_nuget_notices destination)
	install(FILES
		"${TERRAENGINE_OFFLINE_NUGET_METADATA_ROOT}/README.md"
		"${TERRAENGINE_OFFLINE_NUGET_METADATA_ROOT}/NOTICE.md"
		"${TERRAENGINE_OFFLINE_NUGET_MANIFEST}"
		DESTINATION "${destination}/ThirdPartyNotices/NuGet"
	)
	install(DIRECTORY
		"${TERRAENGINE_OFFLINE_NUGET_METADATA_ROOT}/licenses/"
		DESTINATION "${destination}/ThirdPartyNotices/NuGet/licenses"
	)
endfunction()

# NativeAOT export happens after the editor is installed, so this is a runtime
# build toolchain rather than an ordinary build-only package cache. Keep its
# archive bytes identical to the feed verified during CMake configuration and
# place global.json beside the config so the executable-relative working
# directory selects the same SDK version as a source-tree build.
function(terraengine_install_nativeaot_nuget_toolchain destination)
	set(nativeaot_toolchain_destination
		"${destination}/Editor/NativeAotNuGet")
	install(FILES "${TERRAENGINE_NATIVEAOT_NUGET_CONFIG}"
		DESTINATION "${nativeaot_toolchain_destination}"
		RENAME "NuGet.Config")
	install(FILES
		"${TERRAENGINE_DOTNET_GLOBAL_JSON}"
		"${TERRAENGINE_OFFLINE_NUGET_MANIFEST}"
		DESTINATION "${nativeaot_toolchain_destination}")
	install(DIRECTORY "${TERRAENGINE_OFFLINE_NUGET_FEED}/"
		DESTINATION "${nativeaot_toolchain_destination}/feed"
		FILES_MATCHING PATTERN "*.nupkg"
	)
endfunction()

# Support a fast contract check without configuring or compiling TerraEngine:
#   cmake -DTERRAENGINE_RUNTIME_ID=win-x64
#     -P cmake/TerraEngineOfflineNuGet.cmake
if(DEFINED CMAKE_SCRIPT_MODE_FILE
		AND "${CMAKE_SCRIPT_MODE_FILE}" STREQUAL "${CMAKE_CURRENT_LIST_FILE}")
	if(NOT DEFINED TERRAENGINE_RUNTIME_ID)
		set(TERRAENGINE_RUNTIME_ID "win-x64")
	endif()
	terraengine_verify_offline_nuget_feed("${TERRAENGINE_RUNTIME_ID}")
	if(DEFINED TERRAENGINE_NUGET_PACKAGE_CACHE)
		terraengine_verify_nuget_package_cache(
			"${TERRAENGINE_NUGET_PACKAGE_CACHE}")
		message(STATUS
			"Verified regenerated TerraEngine NuGet package cache at "
			"${TERRAENGINE_NUGET_PACKAGE_CACHE}.")
	endif()
	message(STATUS
		"Verified TerraEngine offline NuGet feed for "
		"${TERRAENGINE_RUNTIME_ID}.")
endif()
