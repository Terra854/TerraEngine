// VirtualGeometryEvidence.h - Versioned benchmark capture records.
//
// Evidence is tooling data, so JSON is appropriate here. The serializer emits
// fixed top-level sections and sorted map keys to produce reviewable, byte-
// stable captures suitable for integrity hashes and source-control diffs.

#pragma once

#include "Renderer/VirtualGeometry/Capability/VirtualGeometryCapability.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Renderer
{
	inline constexpr std::array<std::string_view, 16u> kVirtualGeometryBenchmarkRequiredSections = { "identity",
		"environment",
		"workload",
		"warmupAndSampling",
		"capabilities",
		"rawSamples",
		"statistics",
		"geometry",
		"residencyAndStreaming",
		"visibilityAndShading",
		"deformation",
		"auxiliaryViews",
		"fallbacksAndFailures",
		"baselineAndComparability",
		"artifacts",
		"integrity" };

	struct VirtualGeometryBenchmarkIdentity
	{
		std::string schemaVersion = "1";
		std::string workloadVersion;
		std::string captureId;
		std::string runId;
		std::string engineHash;
		std::string sourceHash;
		std::string diffHash;
		std::string executableHash;
		std::string shaderHash;
		std::string assetHash;
		std::string buildRecipeHash;
	};

	struct VirtualGeometryBenchmarkEnvironment
	{
		std::string adapter;
		std::string driver;
		std::string backend;
		std::string operatingSystem;
		std::string route;
		std::string resolution;
		std::string quality;
		std::uint32_t viewCount = 0u;
	};

	struct VirtualGeometryBenchmarkWorkload
	{
		std::string name;
		std::string sceneHash;
		std::string workloadHash;
	};

	struct VirtualGeometryBenchmarkSampling
	{
		std::uint32_t warmupSamples = 0u;
		std::uint32_t requestedSamples = 0u;
		bool randomizedOrder = false;
	};

	struct VirtualGeometryBenchmarkStatistics
	{
		std::optional<std::uint64_t> medianNanoseconds;
		std::optional<std::uint64_t> p95Nanoseconds;
		std::optional<std::uint64_t> p99Nanoseconds;
		std::optional<std::uint64_t> varianceNanosecondsSquared;
		std::optional<std::uint64_t> worstNanoseconds;
		std::uint32_t sampleCount = 0u;
		std::uint32_t rejectedCount = 0u;
		std::vector<std::string> rejectionReasons;
	};

	struct VirtualGeometryBenchmarkEvidenceSections
	{
		std::map<std::string, std::string> geometry;
		std::map<std::string, std::string> residencyAndStreaming;
		std::map<std::string, std::string> visibilityAndShading;
		std::map<std::string, std::string> deformation;
		std::map<std::string, std::string> auxiliaryViews;
		std::map<std::string, std::string> fallbacksAndFailures;
		std::map<std::string, std::string> baselineAndComparability;
		std::map<std::string, std::string> artifacts;
	};

	struct VirtualGeometryBenchmarkCapture
	{
		VirtualGeometryBenchmarkIdentity identity{};
		VirtualGeometryBenchmarkEnvironment environment{};
		VirtualGeometryBenchmarkWorkload workload{};
		VirtualGeometryBenchmarkSampling warmupAndSampling{};
		std::vector<VirtualGeometryCapabilityEvidence> capabilities;
		std::vector<VirtualGeometryMeasurementRecord> rawSamples;
		VirtualGeometryBenchmarkStatistics statistics{};
		VirtualGeometryBenchmarkEvidenceSections sections{};
		std::map<std::string, std::string> diagnostics;
		std::string integrityHash;
	};

	[[nodiscard]] std::string SerializeVirtualGeometryBenchmarkCapture(const VirtualGeometryBenchmarkCapture &capture);
} // namespace TerraEngine::Renderer
