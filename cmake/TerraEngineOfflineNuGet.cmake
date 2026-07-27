# TerraEngine's managed targets restore exclusively from the audited repository
# feed. Keeping verification and restore setup in one module prevents individual
# CMake targets from accidentally re-enabling a user or network NuGet source.
include_guard(GLOBAL)

set(TERRAENGINE_OFFLINE_NUGET_ROOT "${CMAKE_SOURCE_DIR}/External/NuGet")
set(TERRAENGINE_OFFLINE_NUGET_FEED "${TERRAENGINE_OFFLINE_NUGET_ROOT}/feed")
set(TERRAENGINE_OFFLINE_NUGET_MANIFEST "${TERRAENGINE_OFFLINE_NUGET_ROOT}/manifest.json")
set(TERRAENGINE_OFFLINE_NUGET_CONFIG "${CMAKE_SOURCE_DIR}/NuGet.Config")
set(TERRAENGINE_NATIVEAOT_NUGET_CONFIG
	"${TERRAENGINE_OFFLINE_NUGET_ROOT}/NativeAotNuGet.Config")
set(TERRAENGINE_DOTNET_GLOBAL_JSON "${CMAKE_SOURCE_DIR}/global.json")

# Verify package and legal-file bytes before NuGet sees the feed. This makes a
# partial copy, accidental update, or unreviewed extra archive a configure-time
# error with a concrete filename.
function(terraengine_verify_offline_nuget_feed runtime_id)
	if(NOT EXISTS "${TERRAENGINE_OFFLINE_NUGET_MANIFEST}")
		message(FATAL_ERROR
			"TerraEngine offline NuGet manifest is missing: "
			"${TERRAENGINE_OFFLINE_NUGET_MANIFEST}")
	endif()
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
					"${TERRAENGINE_OFFLINE_NUGET_ROOT}/${primary_license}")
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
					"${TERRAENGINE_OFFLINE_NUGET_ROOT}/${legal_relative_path}")
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

# Add one serialized restore boundary for all managed targets. Downstream build
# and publish commands use --no-restore so no target can independently select a
# user or network package source.
function(terraengine_add_offline_nuget_restore_target target_name runtime_id)
	if(TARGET "${target_name}")
		message(FATAL_ERROR
			"Offline NuGet restore target already exists: ${target_name}")
	endif()

	terraengine_verify_offline_nuget_feed("${runtime_id}")
	# Native-only Linux/headless validation must remain configurable even on a
	# worker that intentionally has no .NET SDK. Keep the managed target present
	# and fail when that target is actually built; Windows clean builds still
	# resolve the exact global.json-pinned SDK and exercise the offline feed.
	find_program(dotnet_executable NAMES dotnet)
	if(NOT dotnet_executable)
		set(dotnet_executable dotnet)
		message(WARNING
			"dotnet was not found while configuring ${runtime_id}. Native-only "
			"targets remain available, but TerraEngine.Managed.Restore and every "
			"managed publish target will fail until the pinned SDK is present.")
	endif()

	set(package_cache "${CMAKE_BINARY_DIR}/NuGetPackages")
	set(dotnet_cli_home "${CMAKE_BINARY_DIR}/DotNetCliHome")
	set(dotnet_environment
		"NUGET_PACKAGES=${package_cache}"
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

	add_custom_target("${target_name}"
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
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		COMMENT
			"Restoring managed projects from the repository NuGet feed"
		VERBATIM
		USES_TERMINAL)

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
		"${TERRAENGINE_OFFLINE_NUGET_ROOT}/README.md"
		"${TERRAENGINE_OFFLINE_NUGET_ROOT}/NOTICE.md"
		"${TERRAENGINE_OFFLINE_NUGET_ROOT}/manifest.json"
		DESTINATION "${destination}/ThirdPartyNotices/NuGet"
	)
	install(DIRECTORY
		"${TERRAENGINE_OFFLINE_NUGET_ROOT}/licenses/"
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
	install(FILES "${TERRAENGINE_OFFLINE_NUGET_ROOT}/NativeAotNuGet.Config"
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
	message(STATUS
		"Verified TerraEngine offline NuGet feed for "
		"${TERRAENGINE_RUNTIME_ID}.")
endif()
