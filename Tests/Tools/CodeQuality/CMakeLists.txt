# This lexer has no engine dependency: parsing source must not start a renderer
# or depend on globally installed parser/image packages.
set(source_review_tools "${ENGINE_INSTALL_DESTINATION}/Tests/Tools/CodeQuality")
# The closure checker accepts explicit registers at runtime. Its portable CTest
# uses only labeled synthetic scope, so a clean checkout needs no ignored specs.
# Honor an explicit toolchain override, otherwise use CMake's interpreter
# discovery to avoid accepting a python-named executable without a version.
if(NOT TERRAENGINE_REVIEW_PYTHON)
	find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
	set(TERRAENGINE_REVIEW_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH
		"Python3.9+ interpreter for validation-only standard-library tools")
endif()
# Registered validator commands require a stable absolute interpreter identity.
# Reject relative overrides before version probing can resolve them through PATH.
if(NOT IS_ABSOLUTE "${TERRAENGINE_REVIEW_PYTHON}")
	message(FATAL_ERROR "TERRAENGINE_REVIEW_PYTHON must be an absolute Python3.9+ executable path.")
endif()
# Validate overrides too. Windows Store aliases and obsolete interpreters must
# fail at configure time, before any installed validation command is registered.
execute_process(COMMAND "${TERRAENGINE_REVIEW_PYTHON}" -c
	"import sys; print(sys.version.split()[0]); sys.exit(0 if sys.version_info >= (3, 9) else 1)"
	RESULT_VARIABLE review_python_status OUTPUT_VARIABLE review_python_version
	ERROR_VARIABLE review_python_error OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 15)
if(NOT review_python_status STREQUAL "0" OR NOT review_python_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
	message(FATAL_ERROR "Source review validation requires a working Python3.9+ interpreter. Set TERRAENGINE_REVIEW_PYTHON to its full path. ${review_python_error}")
endif()
install(FILES ValidateFindingClosure.py
	DESTINATION "${source_review_tools}" COMPONENT SourceReview)
add_test(NAME Repository.FindingClosureContracts
	COMMAND "${TERRAENGINE_REVIEW_PYTHON}"
	"${source_review_tools}/ValidateFindingClosure.py"
	--fixture-register --self-test "${source_review_tools}/Fixtures/FindingClosureCases.json")
set_tests_properties(Repository.FindingClosureContracts PROPERTIES
	LABELS "Repository;ReviewRemediation;Closure;ParserContract"
	ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" RUN_SERIAL TRUE TIMEOUT 90)
if(WIN32)
	# Standalone argv child: OS/process protocol only, no engine or managed ABI.
	# Keep the console subsystem because the source supplies wmain; the owned
	# hidden launch is observed by the receipt rather than a GUI entry point.
	add_executable(TerraEngine-NativeArgvReceipt Fixtures/NativeArgvReceipt.cpp)
	target_compile_features(TerraEngine-NativeArgvReceipt PRIVATE cxx_std_20)
	target_include_directories(TerraEngine-NativeArgvReceipt PRIVATE
		"${PROJECT_SOURCE_DIR}/External/json/single_include")
	target_link_libraries(TerraEngine-NativeArgvReceipt PRIVATE user32)
	if(MSVC)
		set_property(TARGET TerraEngine-NativeArgvReceipt PROPERTY
			MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
	install(TARGETS TerraEngine-NativeArgvReceipt
		RUNTIME DESTINATION "${source_review_tools}" COMPONENT SourceReview)
	include("${CMAKE_CURRENT_SOURCE_DIR}/ReviewEvidenceScripts.cmake")
endif()
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
	VerifySourceReviewContinuation.py TestSourceReviewContinuation.py
	source-review-continuation.schema.json
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

# Artificial roots qualify missing/stale review rejection with the real lexer.
# The script owns a fresh retained report and enforces its nonempty case floor;
# a successful parser contract never establishes current runtime acceptance.
add_test(NAME Repository.SourceReviewContinuationContracts
	COMMAND "${TERRAENGINE_REVIEW_PYTHON}"
	"${source_review_tools}/TestSourceReviewContinuation.py"
	--validator "${source_review_tools}/VerifySourceReviewContinuation.py"
	--audit-executable "${source_review_tools}/TerraEngine-SourceReviewAudit${CMAKE_EXECUTABLE_SUFFIX}"
	--minimum-cases 55)
set_tests_properties(Repository.SourceReviewContinuationContracts PROPERTIES
	LABELS "Repository;SourceReview;Continuation;Contract"
	ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" RUN_SERIAL TRUE TIMEOUT 120)

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

	# Current checks join installed reviews and the checkout with exact retained
	# historical Git objects. Independent token proof still needs its sealed archive.
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
		-ContinuationManifest "${source_review_tools}/Baseline/Continuation/continuation.json"
		-HistoricalManifest "${source_review_tools}/Baseline/Continuation/historical-source-manifest.json"
		-ContinuationValidator "${source_review_tools}/VerifySourceReviewContinuation.py"
		-PythonExecutable "${TERRAENGINE_REVIEW_PYTHON}"
		-Mode Portable)
	set_tests_properties(Repository.SourceReviewQuality PROPERTIES
		# The joined corpus audit launches hundreds of Git provenance checks and
		# verifies historical accounting too. Bound slow developer/CI hosts without
		# treating this source-identity contract as a throughput benchmark.
		LABELS "Repository;SourceReview;FrameStages;Installed;Contract" RUN_SERIAL TRUE TIMEOUT 600)

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
