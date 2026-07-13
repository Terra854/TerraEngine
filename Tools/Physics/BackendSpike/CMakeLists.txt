# The backend spike deliberately lives outside the root build.  It verifies the
# caller's candidate contract and installs only its own provenance record; the
# companion scripts configure each provider in a hermetic sibling directory.
cmake_minimum_required(VERSION 3.25)

project(TerraEnginePhysicsBackendSpike LANGUAGES NONE)

set(BACKEND_SPIKE_PROVIDER "" CACHE STRING
	"Candidate being evaluated: Jolt55, Jolt56, or PhysX58.")
set(BACKEND_SPIKE_SOURCE_DIR "" CACHE PATH
	"Absolute provider source directory supplied by the caller.")
set(BACKEND_SPIKE_REVISION "" CACHE STRING
	"Exact provider revision used for this candidate.")
option(BACKEND_SPIKE_VALIDATE_ONLY
	"Configure the harness without a provider source tree (safe CI syntax check)." OFF)
option(BACKEND_SPIKE_BUILD_FIXTURE
	"Build the Jolt provider fixture against an already installed spike provider." OFF)
set(BACKEND_SPIKE_PROVIDER_INSTALL_DIR "" CACHE PATH
	"Install tree containing the Jolt package used by the provider fixture.")

# Keep all generated files inside the caller-controlled build tree.  This makes
# it possible to archive a run without mixing temporary probe data into source.
set(BACKEND_SPIKE_EVIDENCE_DIR "${CMAKE_BINARY_DIR}/evidence" CACHE PATH
	"Directory that receives this harness's generated provenance JSON.")

set(_backend_spike_valid_candidates Jolt55 Jolt56 PhysX58)
if(NOT BACKEND_SPIKE_PROVIDER IN_LIST _backend_spike_valid_candidates)
	message(FATAL_ERROR
		"BACKEND_SPIKE_PROVIDER must be one of Jolt55, Jolt56, or PhysX58; received '${BACKEND_SPIKE_PROVIDER}'.")
endif()

if(BACKEND_SPIKE_PROVIDER STREQUAL "Jolt55")
	set(_backend_spike_version "5.5.0")
	set(_backend_spike_expected_revision "23dadd0e603f1b321142d4c74df07fce85064989")
	set(_backend_spike_license "MIT")
elseif(BACKEND_SPIKE_PROVIDER STREQUAL "Jolt56")
	set(_backend_spike_version "5.6.0")
	set(_backend_spike_expected_revision "e77f175595e64cb44218cc9d9d56fc365ad0e36a")
	set(_backend_spike_license "MIT")
else()
	set(_backend_spike_version "5.8.0")
	set(_backend_spike_expected_revision "fc1018a3745664a1db2b95ce03fb5e91eb585f2e")
	set(_backend_spike_license "BSD-3-Clause")
endif()

if(BACKEND_SPIKE_REVISION STREQUAL "")
	message(FATAL_ERROR
		"BACKEND_SPIKE_REVISION is required. Pass the observed source revision.")
endif()
if(NOT _backend_spike_expected_revision STREQUAL "caller-supplied" AND
	NOT BACKEND_SPIKE_REVISION STREQUAL _backend_spike_expected_revision)
	message(FATAL_ERROR
		"${BACKEND_SPIKE_PROVIDER} must use revision ${_backend_spike_expected_revision}; received ${BACKEND_SPIKE_REVISION}.")
endif()

if(BACKEND_SPIKE_VALIDATE_ONLY)
	if(NOT BACKEND_SPIKE_SOURCE_DIR STREQUAL "")
		message(STATUS "Validation-only mode ignores BACKEND_SPIKE_SOURCE_DIR.")
	endif()
	set(_backend_spike_source_status "not-inspected-validation-only")
else()
	if(BACKEND_SPIKE_SOURCE_DIR STREQUAL "" OR NOT IS_DIRECTORY "${BACKEND_SPIKE_SOURCE_DIR}")
		message(FATAL_ERROR
			"BACKEND_SPIKE_SOURCE_DIR must name an existing provider source directory unless BACKEND_SPIKE_VALIDATE_ONLY=ON.")
	endif()
	# The recorded source is the Git checkout root used for revision and size
	# evidence. Jolt and PhysX both keep their actual CMake entry point in a
	# subdirectory, which the platform scripts select explicitly.
	set(_backend_spike_source_status "caller-path-validated")
endif()

file(MAKE_DIRECTORY "${BACKEND_SPIKE_EVIDENCE_DIR}")

# CMake's JSON command handles platform quoting, including backslashes in
# Windows paths, so the raw provenance document remains machine-readable.
set(_backend_spike_json "{}")
string(JSON _backend_spike_json SET "${_backend_spike_json}" schemaVersion 1)
string(JSON _backend_spike_json SET "${_backend_spike_json}" candidate "\"${BACKEND_SPIKE_PROVIDER}\"")
string(JSON _backend_spike_json SET "${_backend_spike_json}" version "\"${_backend_spike_version}\"")
string(JSON _backend_spike_json SET "${_backend_spike_json}" revision "\"${BACKEND_SPIKE_REVISION}\"")
string(JSON _backend_spike_json SET "${_backend_spike_json}" expectedRevision "\"${_backend_spike_expected_revision}\"")
string(JSON _backend_spike_json SET "${_backend_spike_json}" license "\"${_backend_spike_license}\"")
string(JSON _backend_spike_json SET "${_backend_spike_json}" sourceStatus "\"${_backend_spike_source_status}\"")
string(JSON _backend_spike_json SET "${_backend_spike_json}" networkPolicy "\"scripts-set-fetchcontent-fully-disconnected-and-loopback-proxies\"")
string(JSON _backend_spike_json SET "${_backend_spike_json}" providerPrivateThreadPoolAllowed false)
file(WRITE "${BACKEND_SPIKE_EVIDENCE_DIR}/harness-config.raw.json" "${_backend_spike_json}\n")

# A real target gives the scripts an identical configure/build/install timing
# sequence even during validation-only runs with no third-party source present.
add_custom_target(BackendSpikeManifest ALL
	COMMENT "Preparing disposable backend-spike provenance")
install(FILES "${BACKEND_SPIKE_EVIDENCE_DIR}/harness-config.raw.json"
	DESTINATION "backend-spike/evidence")

if(BACKEND_SPIKE_BUILD_FIXTURE)
	if(NOT BACKEND_SPIKE_PROVIDER STREQUAL "Jolt55" AND
		NOT BACKEND_SPIKE_PROVIDER STREQUAL "Jolt56")
		message(FATAL_ERROR "ProviderFixture currently supports Jolt candidates only.")
	endif()
	if(BACKEND_SPIKE_PROVIDER_INSTALL_DIR STREQUAL "")
		message(FATAL_ERROR "BACKEND_SPIKE_PROVIDER_INSTALL_DIR is required when building the fixture.")
	endif()

	# The provider is built and installed first by the companion script. Loading
	# only that exact package keeps the fixture from finding a global Jolt SDK.
	enable_language(CXX)
	find_package(Jolt CONFIG REQUIRED
		PATHS "${BACKEND_SPIKE_PROVIDER_INSTALL_DIR}/lib/cmake/Jolt"
		NO_DEFAULT_PATH)
	add_executable(BackendSpikeProviderFixture
		"${CMAKE_CURRENT_LIST_DIR}/ProviderFixture.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/ProviderProfilerStub.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/../../../Engine/ThreadPool/ThreadPool.cpp"
		"${CMAKE_CURRENT_LIST_DIR}/../../../Engine/Core/Logger/Logger.cpp")
	target_compile_features(BackendSpikeProviderFixture PRIVATE cxx_std_20)
	target_include_directories(BackendSpikeProviderFixture PRIVATE
		"${CMAKE_CURRENT_LIST_DIR}/../../.."
		"${CMAKE_CURRENT_LIST_DIR}/../../../Engine")
	target_link_libraries(BackendSpikeProviderFixture PRIVATE Jolt::Jolt)
	# Jolt is built without compiler RTTI. Match that ABI in the derived job
	# adapter so GCC/Clang do not request typeinfo symbols absent from the library.
	if(MSVC)
		target_compile_options(BackendSpikeProviderFixture PRIVATE /GR-)
	else()
		target_compile_options(BackendSpikeProviderFixture PRIVATE -fno-rtti)
	endif()
	# ReleaseASAN is a Jolt-provided configuration name rather than a built-in
	# CMake build type. The imported static library is instrumented, so the host
	# fixture must link the sanitizer runtime explicitly as well. Keeping this on
	# the disposable target avoids changing TerraEngine's normal build policy.
	if(CMAKE_BUILD_TYPE STREQUAL "ReleaseASAN")
		if(MSVC)
			target_compile_options(BackendSpikeProviderFixture PRIVATE /fsanitize=address)
		else()
			target_compile_options(BackendSpikeProviderFixture PRIVATE -fsanitize=address)
			target_link_options(BackendSpikeProviderFixture PRIVATE -fsanitize=address)
		endif()
	endif()
	set_target_properties(BackendSpikeProviderFixture PROPERTIES
		OUTPUT_NAME "ProviderFixture"
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	install(TARGETS BackendSpikeProviderFixture
		RUNTIME DESTINATION "backend-spike/bin")
endif()

message(STATUS "Backend spike candidate: ${BACKEND_SPIKE_PROVIDER} ${_backend_spike_version}")
message(STATUS "Backend spike source status: ${_backend_spike_source_status}")
