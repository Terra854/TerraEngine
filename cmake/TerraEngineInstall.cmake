# Physics-specific install rules live here so provider source and samples can
# never accidentally enter the TerraEngine runtime tree.
function(terraengine_install_physics_dependency_metadata destination)
	install(FILES
		"${CMAKE_SOURCE_DIR}/Docs/Dependencies/JoltPhysics.md"
		DESTINATION "${destination}/Docs/Dependencies"
	)
	install(FILES
		"${CMAKE_SOURCE_DIR}/External/JoltPhysics/LICENSE"
		DESTINATION "${destination}/Docs/Licenses"
		RENAME "JoltPhysics-LICENSE.txt"
	)
endfunction()

# Audio installs only the provider-neutral extension contract. The concrete
# miniaudio adapter and all upstream SDK headers remain private build inputs.
function(terraengine_install_audio_foundation destination)
	install(FILES
		"${CMAKE_SOURCE_DIR}/Docs/Audio.md"
		DESTINATION "${destination}/Docs")
	install(FILES
		"${CMAKE_SOURCE_DIR}/Docs/Dependencies/miniaudio.md"
		DESTINATION "${destination}/Docs/Dependencies")
	install(FILES
		"${CMAKE_SOURCE_DIR}/External/miniaudio/LICENSE"
		DESTINATION "${destination}/Docs/Licenses"
		RENAME "miniaudio-LICENSE.txt")
	# The installed extension SDK is an explicit allowlist. Engine-internal
	# authoring/system headers depend on live ECS state and are not provider ABI.
	install(FILES
		"${CMAKE_SOURCE_DIR}/Engine/Audio/AudioTypes.h"
		DESTINATION "${destination}/include/TerraEngine/Audio")
	install(FILES
		"${CMAKE_SOURCE_DIR}/Engine/Audio/Backend/IAudioBackend.h"
		"${CMAKE_SOURCE_DIR}/Engine/Audio/Backend/AudioBackendRegistry.h"
		DESTINATION "${destination}/include/TerraEngine/Audio/Backend")
	install(FILES
		"${CMAKE_SOURCE_DIR}/Engine/Core/Export.h"
		DESTINATION "${destination}/include/TerraEngine/Core")
	install(FILES
		"${CMAKE_SOURCE_DIR}/Engine/Utilities/UUID.h"
		DESTINATION "${destination}/include/TerraEngine/Utilities")
	if(BUILD_TESTING)
		# Fixtures and stress wrappers are QA assets, so production-only installs
		# must not publish them as part of the audio extension SDK.
		install(DIRECTORY "${CMAKE_SOURCE_DIR}/Tests/Fixtures/Audio/"
			DESTINATION "${destination}/Tests/Fixtures/Audio")
		install(DIRECTORY "${CMAKE_SOURCE_DIR}/Tests/Tools/Audio/"
			DESTINATION "${destination}/Tests/Tools/Audio"
			FILES_MATCHING PATTERN "*.ps1" PATTERN "*.json")
	endif()
	install(FILES
		"${CMAKE_SOURCE_DIR}/Tests/Fixtures/Audio/ThirdParty/Kenney/Music-LICENSE.txt"
		"${CMAKE_SOURCE_DIR}/Tests/Fixtures/Audio/ThirdParty/Kenney/Interface-Sounds-LICENSE.txt"
		DESTINATION "${destination}/Docs/Licenses")
	# Install trees are incremental, so remove any internal headers left by an
	# older configuration before publishing the provider allowlist above.
	install(CODE "file(GLOB _terraengine_audio_headers
		\"${destination}/include/TerraEngine/Audio/*.h\"
		\"${destination}/include/TerraEngine/Audio/Assets/*.h\")
	foreach(_header IN LISTS _terraengine_audio_headers)
		get_filename_component(_name \"\${_header}\" NAME)
		if(NOT _name STREQUAL \"AudioTypes.h\")
			file(REMOVE \"\${_header}\")
		endif()
	endforeach()
	file(REMOVE_RECURSE \"${destination}/include/TerraEngine/Audio/Assets\")
	file(GLOB _terraengine_backend_headers \"${destination}/include/TerraEngine/Audio/Backend/*.h\")
	foreach(_header IN LISTS _terraengine_backend_headers)
		get_filename_component(_name \"\${_header}\" NAME)
		if(NOT _name STREQUAL \"IAudioBackend.h\" AND NOT _name STREQUAL \"AudioBackendRegistry.h\")
			file(REMOVE \"\${_header}\")
		endif()
	endforeach()")
endfunction()

function(terraengine_install_physics_headers destination)
	install(DIRECTORY "${CMAKE_SOURCE_DIR}/Engine/Physics/"
		DESTINATION "${destination}/include/TerraEngine/Physics"
		FILES_MATCHING PATTERN "*.h"
		# Provider adapters are private implementation headers and depend on Jolt
		# headers that intentionally are not redistributed in the runtime tree.
		PATTERN "Backend/JoltPhysicsBackend.h" EXCLUDE
		PATTERN "Backend/PhysicsAllocatorAdapter.h" EXCLUDE
		PATTERN "Backend/PhysicsJobAdapter.h" EXCLUDE
	)
	# Remove stale copies left by an older install tree after these headers became
	# private. CMake install is incremental and does not delete excluded files.
	install(CODE "file(REMOVE
		\"${destination}/include/TerraEngine/Physics/Backend/JoltPhysicsBackend.h\"
		\"${destination}/include/TerraEngine/Physics/Backend/PhysicsAllocatorAdapter.h\"
		\"${destination}/include/TerraEngine/Physics/Backend/PhysicsJobAdapter.h\"
	)")
endfunction()

# The root build calls one stable function so the metadata and public-header
# allowlist cannot drift into separate install gates.
function(terraengine_install_physics_foundation destination)
	terraengine_install_physics_dependency_metadata("${destination}")
	terraengine_install_physics_headers("${destination}")
	if(BUILD_TESTING)
		# Keep performance inputs beside installed test tooling for clean-tree QA.
		install(DIRECTORY "${CMAKE_SOURCE_DIR}/Tests/Fixtures/Performance/Physics/"
			DESTINATION "${destination}/Tests/Fixtures/Performance/Physics"
			FILES_MATCHING PATTERN "*.json")
		install(DIRECTORY "${CMAKE_SOURCE_DIR}/Tests/Tools/Physics/"
			DESTINATION "${destination}/Tests/Tools/Physics"
			FILES_MATCHING PATTERN "*.ps1" PATTERN "*.json" PATTERN "README.md")
	endif()
endfunction()
