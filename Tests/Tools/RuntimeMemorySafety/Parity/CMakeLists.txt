cmake_minimum_required(VERSION 3.24)
project(RuntimeMemorySafetyParity LANGUAGES CXX)
# Standalone narrow build: callers supply repository, immutable compatibility
# archive and matching candidate import library explicitly. No runtime is run.
set(TERRA_REPOSITORY "" CACHE PATH "Checkout containing bundled dependencies/current headers")
set(TERRA_REFERENCE "" CACHE PATH "Immutable archive containing Compatibility headers/import library")
set(TERRA_CANDIDATE_LIBRARY "" CACHE FILEPATH "Matching candidate import library")
foreach(required IN ITEMS TERRA_REPOSITORY TERRA_REFERENCE TERRA_CANDIDATE_LIBRARY)
	if(NOT IS_ABSOLUTE "${${required}}" OR NOT EXISTS "${${required}}")
		message(FATAL_ERROR "${required} must name an existing absolute path")
	endif()
endforeach()
foreach(variant IN ITEMS Baseline Candidate)
	add_executable(RuntimeMemorySafetyParity${variant} RuntimeMemorySafetyParityHost.cpp)
	if(variant STREQUAL "Baseline")
		set(engine_headers "${TERRA_REFERENCE}/Compatibility/Engine")
		set(engine_library "${TERRA_REFERENCE}/Compatibility/Libraries/TerraEngine.lib")
	else()
		set(engine_headers "${TERRA_REPOSITORY}/Engine")
		set(engine_library "${TERRA_CANDIDATE_LIBRARY}")
		# This branch alone can name the new typed scratch validation API. The
		# archived baseline translation unit never includes or references it.
		target_compile_definitions(RuntimeMemorySafetyParity${variant} PRIVATE RMS_CANDIDATE_SCRATCH=1)
	endif()
	target_compile_features(RuntimeMemorySafetyParity${variant} PRIVATE cxx_std_20)
	target_compile_definitions(RuntimeMemorySafetyParity${variant} PRIVATE NOMINMAX TERRAENGINE_SHARED
		RMS_HOST_VARIANT="${variant}")
	target_include_directories(RuntimeMemorySafetyParity${variant} PRIVATE "${engine_headers}" "${engine_headers}/Core/Object"
		"${CMAKE_CURRENT_SOURCE_DIR}/support" "${TERRA_REPOSITORY}/External/entt/src"
		"${TERRA_REPOSITORY}/External/glm" "${TERRA_REPOSITORY}/External/json/single_include"
		"${TERRA_REPOSITORY}/External/volk" "${TERRA_REPOSITORY}/External/VMA/include"
		"${TERRA_REPOSITORY}/External/Vulkan-Headers/include" "${TERRA_REPOSITORY}/External/glfw/include")
	target_link_libraries(RuntimeMemorySafetyParity${variant} PRIVATE "${engine_library}" bcrypt)
	# A separate directory prevents accidentally mixing the two runtime closures.
	set_target_properties(RuntimeMemorySafetyParity${variant} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${variant}")
endforeach()
