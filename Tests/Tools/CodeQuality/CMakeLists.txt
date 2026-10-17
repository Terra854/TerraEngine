# This lexer has no engine dependency: parsing source must not start a renderer
# or depend on globally installed parser/image packages.
set(source_review_tools "${ENGINE_INSTALL_DESTINATION}/Tests/Tools/CodeQuality")
add_executable(TerraEngine-SourceReviewAudit
	SourceReviewAudit.cpp
	SourceReviewAuditTests.cpp
)
target_compile_features(TerraEngine-SourceReviewAudit PRIVATE cxx_std_20)
if(MSVC)
	# The standalone utility owns its allocations and exports no runtime ABI, so
	# a static compiler runtime makes its install self-contained.
	set_property(TARGET TerraEngine-SourceReviewAudit PROPERTY
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
install(TARGETS TerraEngine-SourceReviewAudit
	RUNTIME DESTINATION "${source_review_tools}" COMPONENT SourceReview)
install(FILES acceptance-contract.json README.md
	VerifyStageOwnership.ps1 TestStageOwnership.ps1
	VerifySourceReviewQuality.ps1 comment-dispositions.schema.json
	DESTINATION "${source_review_tools}" COMPONENT SourceReview)
install(DIRECTORY Baseline/ DESTINATION "${source_review_tools}/Baseline" COMPONENT SourceReview)
install(DIRECTORY Fixtures/ DESTINATION "${source_review_tools}/Fixtures" COMPONENT SourceReview)

# Exercise the installed parser corpus; portable final-source checks below
# additionally verify current source, stage budgets and occurrence accounting.
add_test(NAME Repository.SourceReviewAuditParser
	COMMAND "${source_review_tools}/TerraEngine-SourceReviewAudit${CMAKE_EXECUTABLE_SUFFIX}" --self-test)
set_tests_properties(Repository.SourceReviewAuditParser PROPERTIES
	PASS_REGULAR_EXPRESSION "SourceReviewAudit self-test failures=0"
	LABELS "Repository;SourceReview;Contract" TIMEOUT 30)

if(WIN32)
	# Characterize the repaired requested-size helper separately from frame timing.
	# This calls the installed production implementation and records raw samples;
	# it has no timing pass threshold or comparison to the former fake image.
	add_executable(TerraEngine-SyntheticCaptureCost SyntheticCaptureCostFixture.cpp
		${PROJECT_SOURCE_DIR}/cmake/TerraEngine.Windows.manifest)
	target_compile_features(TerraEngine-SyntheticCaptureCost PRIVATE cxx_std_20)
	target_compile_definitions(TerraEngine-SyntheticCaptureCost PRIVATE NOMINMAX)
	target_link_libraries(TerraEngine-SyntheticCaptureCost PRIVATE TerraEngine-Engine bcrypt)
	install(TARGETS TerraEngine-SyntheticCaptureCost
		RUNTIME DESTINATION "${source_review_tools}" COMPONENT SourceReview)
	install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-SyntheticCaptureCost>
		DESTINATION "${source_review_tools}" COMPONENT SourceReview)

	# These synthetic closures qualify the structural validator's rejection paths;
	# they neither load the engine nor establish publication/lifetime acceptance.
	add_test(NAME Repository.StageOwnershipContracts
		COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
		-File "${source_review_tools}/TestStageOwnership.ps1"
		-AuditExecutable "${source_review_tools}/TerraEngine-SourceReviewAudit${CMAKE_EXECUTABLE_SUFFIX}"
		-ValidatorScript "${source_review_tools}/VerifyStageOwnership.ps1")
	set_tests_properties(Repository.StageOwnershipContracts PROPERTIES
		LABELS "Repository;FrameStages;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 120)

	# Portable checks use only installed manifests plus the current checkout.
	# Independent token proof is a separate invocation with the sealed archive.
	# The wrapper chooses a fresh temporary output for each CTest invocation.
	add_test(NAME Repository.SourceReviewQuality
		COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
		-File "${source_review_tools}/VerifySourceReviewQuality.ps1"
		-RepoRoot "${PROJECT_SOURCE_DIR}"
		-AuditExecutable "${source_review_tools}/TerraEngine-SourceReviewAudit${CMAKE_EXECUTABLE_SUFFIX}"
		-StageValidator "${source_review_tools}/VerifyStageOwnership.ps1"
		-StageManifest "${source_review_tools}/Baseline/stage-ownership.json"
		-Dispositions "${source_review_tools}/Baseline/comment-dispositions.json"
		-ProductionComments "${source_review_tools}/Baseline/production-comments.json"
		-CommentBatches "${source_review_tools}/Baseline/comment-batches.json"
		-Mode Portable)
	set_tests_properties(Repository.SourceReviewQuality PROPERTIES
		LABELS "Repository;SourceReview;FrameStages;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 120)

	# This collector calls the real installed runtime. Keep its dependent DLLs
	# beside it; the CLI checks the parent-pinned DLL path/hash before engine use.
	add_executable(TerraEngine-FrameStageReplayFixture FrameReplayFixture.cpp
		${PROJECT_SOURCE_DIR}/cmake/TerraEngine.Windows.manifest)
	target_compile_features(TerraEngine-FrameStageReplayFixture PRIVATE cxx_std_20)
	target_compile_definitions(TerraEngine-FrameStageReplayFixture PRIVATE
		NOMINMAX TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION=1)
	target_include_directories(TerraEngine-FrameStageReplayFixture PRIVATE
		${PROJECT_SOURCE_DIR}/External/glfw/include)
	target_link_libraries(TerraEngine-FrameStageReplayFixture PRIVATE TerraEngine-Engine bcrypt)
	install(TARGETS TerraEngine-FrameStageReplayFixture
		RUNTIME DESTINATION "${source_review_tools}" COMPONENT SourceReview)
	install(FILES $<TARGET_RUNTIME_DLLS:TerraEngine-FrameStageReplayFixture>
		DESTINATION "${source_review_tools}" COMPONENT SourceReview)

	# An old DLL needs its complete matching Engine headers, including inline
	# member offsets and virtual declarations. Linking the import-library FILE
	# avoids inheriting current Engine include directories from its CMake target.
	# The parent audits every resulting PE import against the old DLL exports.
	set(TERRAENGINE_FRAME_REPLAY_REFERENCE_SOURCE_ROOT "" CACHE PATH
		"Exact reference revision source archive for the frame replay host")
	if(TERRAENGINE_FRAME_REPLAY_REFERENCE_SOURCE_ROOT)
		if(NOT EXISTS "${TERRAENGINE_FRAME_REPLAY_REFERENCE_SOURCE_ROOT}/Engine/Core/MainEngine.h")
			message(FATAL_ERROR "Frame replay reference source lacks complete Engine headers")
		endif()
		add_executable(TerraEngine-FrameStageReplayReference FrameReplayFixture.cpp
			${PROJECT_SOURCE_DIR}/cmake/TerraEngine.Windows.manifest)
		target_compile_features(TerraEngine-FrameStageReplayReference PRIVATE cxx_std_20)
		target_compile_definitions(TerraEngine-FrameStageReplayReference PRIVATE
			NOMINMAX TERRAENGINE_SHARED TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION=1)
		target_include_directories(TerraEngine-FrameStageReplayReference PRIVATE
			${TERRAENGINE_FRAME_REPLAY_REFERENCE_SOURCE_ROOT}/Engine
			${TERRAENGINE_FRAME_REPLAY_REFERENCE_SOURCE_ROOT}/Engine/Core/Object
			${TERRAENGINE_FRAME_REPLAY_REFERENCE_SOURCE_ROOT}
			${PROJECT_SOURCE_DIR}/External/entt/src
			${PROJECT_SOURCE_DIR}/External/glm
			${PROJECT_SOURCE_DIR}/External/json/single_include
			${PROJECT_SOURCE_DIR}/External/stb
			${PROJECT_SOURCE_DIR}/External/tinyobjloader
			${PROJECT_SOURCE_DIR}/External/basisu/transcoder
			${PROJECT_SOURCE_DIR}/External/basisu/zstd
			${PROJECT_SOURCE_DIR}/External/yoga
			${PROJECT_SOURCE_DIR}/External/glfw/include)
		target_link_libraries(TerraEngine-FrameStageReplayReference PRIVATE
			$<TARGET_LINKER_FILE:TerraEngine-Engine> bcrypt)
		add_dependencies(TerraEngine-FrameStageReplayReference TerraEngine-Engine)
		# No current DLL may accompany the reference host. Its launch working
		# directory stays private; PATH selects the original Editor runtime closure.
		install(TARGETS TerraEngine-FrameStageReplayReference
			RUNTIME DESTINATION "${source_review_tools}/Reference" COMPONENT SourceReview)
	endif()
	install(FILES FrameReplayAccess.h FrameReplayProjection.h FrameReplayFixture.cpp
		CompareFrameStageReplay.ps1 TestFrameStageReplayComparison.ps1
		PrepareFrameReplayProjects.ps1 RunFrameStageReplay.ps1
		DESTINATION "${source_review_tools}" COMPONENT SourceReview)
	add_test(NAME Repository.FrameStageReplayComparison
		COMMAND ${TERRAENGINE_CORRECTNESS_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
		-File "${source_review_tools}/TestFrameStageReplayComparison.ps1" -MinimumTests 263)
	# The installed script enforces the established case floor before reporting
	# success. Keep CTest's normal exit-code check as more regressions are added.
	set_tests_properties(Repository.FrameStageReplayComparison PROPERTIES
		LABELS "Repository;FrameStages;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 300)
endif()
