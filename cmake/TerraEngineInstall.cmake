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
endfunction()
