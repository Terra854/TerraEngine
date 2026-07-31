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

	enum class VirtualGeometryComparisonClassification : std::uint8_t
	{
		Superiority = 0u,
		Parity = 1u,
		Regression = 2u,
		Incomparable = 3u,
		Insufficient = 4u
	};

	struct VirtualGeometryRouteEvidence
	{
		std::string selectedRoute;
		std::string selectionSource;
		std::string policyIdentity;
		std::string calibrationIdentity;
		std::optional<double> estimatedCost;
		std::optional<double> measuredCost;
		std::optional<double> confidenceMargin;
		std::uint32_t stableFrameCount = 0u;
		std::uint64_t sharedGeometryBytes = 0u;
		std::uint32_t directBatchCount = 0u;
		std::uint32_t directInstanceCount = 0u;
		std::uint32_t suppressedPageRequests = 0u;
		std::uint32_t suppressedDecodeBlocks = 0u;
		std::uint32_t suppressedResidencyRecords = 0u;
		std::string fallbackReason;
	};

	struct VirtualGeometryCodecEvidence
	{
		std::string profileIdentity;
		std::string quantizationProfileIdentity;
		std::uint32_t pageSizeBytes = 0u;
		std::uint64_t storedBytes = 0u;
		std::uint64_t decodedBytes = 0u;
		std::uint32_t blockCount = 0u;
		std::uint64_t unusedPageBytes = 0u;
		std::optional<double> requestAmplification;
		std::optional<std::uint64_t> buildNanoseconds;
		std::optional<std::uint64_t> processorDecodeNanoseconds;
		std::optional<std::uint64_t> deviceDecodeNanoseconds;
		bool independentDecodePassed = false;
		bool integrityPassed = false;
		std::vector<std::string> rejectionReasons;
	};

	struct VirtualGeometryQuantizationEvidence
	{
		std::string profileIdentity;
		std::string domainSetHash;
		std::array<std::uint8_t, 4u> positionBits{};
		std::array<std::uint8_t, 4u> attributeBits{};
		std::optional<double> maximumObjectError;
		std::optional<double> p95ObjectError;
		std::optional<double> rmsObjectError;
		std::optional<double> maximumScreenError;
		std::uint64_t sharedBoundaryMismatchCount = 0u;
		std::uint64_t conservativeBoundsFailureCount = 0u;
		std::uint64_t detailOrderingFailureCount = 0u;
		std::vector<std::string> rejectionReasons;
	};

	struct VirtualGeometryComparativeMetric
	{
		std::string name;
		std::string unit;
		bool protectedMetric = false;
		std::vector<double> candidateSamples;
		std::vector<double> referenceSamples;
		std::vector<std::string> rejectedSamples;
		std::optional<double> candidateMedian;
		std::optional<double> candidateP95;
		std::optional<double> candidateP99;
		std::optional<double> referenceMedian;
		std::optional<double> referenceP95;
		std::optional<double> referenceP99;
	};

	struct VirtualGeometryComparativeVisualResult
	{
		std::map<std::string, std::string> artifactHashes;
		bool imageAccepted = false;
		bool silhouetteAccepted = false;
		bool coverageAccepted = false;
		bool materialAccepted = false;
		bool shadowAccepted = false;
		bool animationAccepted = false;
		bool crackAccepted = false;
		bool identityAccepted = false;
		std::vector<std::string> rejectionReasons;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return imageAccepted
				&& silhouetteAccepted
				&& coverageAccepted
				&& materialAccepted
				&& shadowAccepted
				&& animationAccepted
				&& crackAccepted
				&& identityAccepted;
		}
	};

	struct VirtualGeometryComparativePair
	{
		std::string pairIdentity;
		std::string workloadIdentity;
		std::string candidateRevision;
		std::string referenceProduct;
		std::string referenceRelease;
		std::string deviceIdentity;
		std::string driverIdentity;
		std::string operatingSystemIdentity;
		std::string graphicsPath;
		std::map<std::string, std::string> matchedInputHashes;
		std::vector<VirtualGeometryComparativeMetric> metrics;
		VirtualGeometryComparativeVisualResult visual{};
		VirtualGeometryComparisonClassification classification =
			VirtualGeometryComparisonClassification::Insufficient;
		std::vector<std::string> classificationReasons;
		std::string integrityHash;
	};

	[[nodiscard]] std::string SerializeVirtualGeometryBenchmarkCapture(const VirtualGeometryBenchmarkCapture &capture);
} // namespace TerraEngine::Renderer
