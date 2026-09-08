# Real-engine navigation evidence runner used only by installed acceptance.
add_executable(TerraEngineNavigationValidation TerraEngineNavigationValidation.cpp)
target_compile_features(TerraEngineNavigationValidation PRIVATE cxx_std_20)
target_link_libraries(TerraEngineNavigationValidation PRIVATE TerraEngine-Engine)
set_target_properties(TerraEngineNavigationValidation PROPERTIES
	OUTPUT_NAME TerraEngineNavigationAcceptance
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Build/Tests/Tools/Navigation"
)

# The installed acceptance wrapper invokes the real engine runner by this
# stable name; keep it out of the normal SDK and package it beside the tool.
install(TARGETS TerraEngineNavigationValidation
	RUNTIME DESTINATION Tests/Tools/Navigation
	LIBRARY DESTINATION Tests/Tools/Navigation
	ARCHIVE DESTINATION Tests/Tools/Navigation
)

# Keep the installed wrapper and its evidence contract beside the executable so
# acceptance can run from a clean directory without source-tree fallbacks.
install(FILES
	RunInstalledNavigationAcceptance.ps1
	CompareNavigationEvidence.ps1
	VerifyNavigationArchitecture.ps1
	navigation-evidence.schema.json
	DESTINATION Tests/Tools/Navigation
)

if(WIN32)
	# The acceptance executable is launched directly from the installed tool
	# directory, so keep its engine DLL dependencies beside it.
	install(FILES $<TARGET_FILE:TerraEngine-Engine>
		DESTINATION Tests/Tools/Navigation)
	install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-Engine>
		DESTINATION Tests/Tools/Navigation OPTIONAL)
endif()
