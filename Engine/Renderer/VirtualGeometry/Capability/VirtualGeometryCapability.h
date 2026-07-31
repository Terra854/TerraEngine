// VirtualGeometryCapability.h - Truthful capability and measurement records.
//
// The renderer can prepare work on workers, but only a completed, generation-
// safe backend command can prove execution. These records encode that boundary
// so editor and automation surfaces cannot promote planned work into a claim.

#pragma once

#include "Core/Export.h"
#include "Renderer/VirtualGeometry/VirtualGeometryTypes.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Renderer
{
	enum class VirtualGeometryCapabilityStage : std::uint8_t
	{
		Traversal = 0u,
		Compaction = 1u,
		Indirect = 2u,
		Decode = 3u,
		HardwareRaster = 4u,
		ComputeRaster = 5u,
		Resolve = 6u,
		Shading = 7u,
		Streaming = 8u,
		Deformation = 9u,
		Shadow = 10u,
		Capture = 11u,
		RayTracing = 12u
	};

	enum class VirtualGeometryCapabilityBackend : std::uint8_t
	{
		Unknown = 0u,
		Vulkan = 1u
	};

	enum class VirtualGeometryCapabilityRoute : std::uint8_t
	{
		Unknown = 0u,
		MeshShader = 1u,
		ComputeRaster = 2u,
		IndexedFallback = 3u
	};

	struct VirtualGeometryCapabilityKey
	{
		VirtualGeometryCapabilityStage stage = VirtualGeometryCapabilityStage::Traversal;
		VirtualGeometryCapabilityBackend backend = VirtualGeometryCapabilityBackend::Unknown;
		VirtualGeometryCapabilityRoute route = VirtualGeometryCapabilityRoute::Unknown;
		std::string deviceIdentity;
		std::vector<std::string> requiredFeatures;
	};

	struct VirtualGeometryCapabilityExecution
	{
		VirtualGeometryFrameViewIdentity identity{};
		std::string evidenceId;
		bool completedLiveCommand = false;
		bool preparedOnly = false;

		[[nodiscard]] bool IsComplete() const noexcept { return completedLiveCommand && !preparedOnly && identity.IsComplete() && !evidenceId.empty(); }
	};

	// The four booleans are deliberately independent facts, not a single enum:
	// a route may be planned and supported without having executed this session.
	struct VirtualGeometryCapabilityEvidence
	{
		VirtualGeometryCapabilityKey key{};
		bool planned = false;
		bool supported = false;
		bool executed = false;
		bool validated = false;
		std::string supportReason;
		std::string queriedLimits;
		VirtualGeometryCapabilityExecution execution{};
		std::vector<std::string> validationArtifactIds;
		VirtualGeometryCapabilityRoute fallbackRoute = VirtualGeometryCapabilityRoute::Unknown;
		std::string fallbackReason;
		std::string timestamp;
		std::string source;

		[[nodiscard]] bool CanReportExecuted() const noexcept { return planned && supported && executed && execution.IsComplete(); }

		[[nodiscard]] bool CanReportValidated() const noexcept { return validated && CanReportExecuted() && !validationArtifactIds.empty(); }
	};

	enum class VirtualGeometryMeasurementAvailability : std::uint8_t
	{
		Pending = 0u,
		Unavailable = 1u,
		Invalid = 2u,
		Measured = 3u
	};

	// A duration is optional so pending, unavailable, and invalid samples cannot
	// silently masquerade as a measured zero in diagnostics or benchmark output.
	struct VirtualGeometryMeasurementRecord
	{
		VirtualGeometryCapabilityStage stage = VirtualGeometryCapabilityStage::Traversal;
		VirtualGeometryFrameViewIdentity identity{};
		VirtualGeometryMeasurementAvailability availability = VirtualGeometryMeasurementAvailability::Unavailable;
		std::optional<std::uint64_t> rawTicks;
		std::optional<std::uint64_t> durationNanoseconds;
		std::string queryIdentity;
		std::string executionEvidenceId;
		std::string rejectionReason;
		std::uint32_t warmupOrdinal = 0u;
		std::uint32_t sampleOrdinal = 0u;

		[[nodiscard]] bool HasAuthoritativeDuration() const noexcept
		{
			return availability == VirtualGeometryMeasurementAvailability::Measured && durationNanoseconds.has_value() && !queryIdentity.empty() &&
				   !executionEvidenceId.empty();
		}
	};

	// Benchmark scope contains every environment value that must match before a
	// performance comparison can make a portable, reviewable claim.
	struct VirtualGeometryBenchmarkScope
	{
		std::string backend;
		std::string adapter;
		std::string driver;
		std::string route;
		std::string resolution;
		std::string quality;
		std::uint32_t viewCount = 0u;
		std::string workload;

		auto operator<=>(const VirtualGeometryBenchmarkScope &) const = default;
	};

	struct VirtualGeometryBenchmarkSampleStatistics
	{
		std::uint64_t medianNanoseconds = 0u;
		std::uint64_t p95Nanoseconds = 0u;
		std::uint64_t p99Nanoseconds = 0u;
		std::uint64_t varianceNanosecondsSquared = 0u;
		std::uint64_t worstNanoseconds = 0u;
		std::uint32_t measuredCount = 0u;
		std::uint32_t rejectedCount = 0u;
		std::vector<std::string> rejectionReasons;
	};

	struct VirtualGeometryBenchmarkRun
	{
		std::string captureId;
		VirtualGeometryBenchmarkScope scope{};
		std::string sourceHash;
		std::string executableHash;
		std::string shaderHash;
		std::string assetHash;
		std::string buildRecipeHash;
		bool randomizedOrder = true;
		std::vector<VirtualGeometryMeasurementRecord> samples;
	};

	struct VirtualGeometryBenchmarkComparability
	{
		bool comparable = false;
		std::string reason;
	};

	TERRAENGINE_API VirtualGeometryCapabilityEvidence MakePlannedVirtualGeometryCapability(VirtualGeometryCapabilityKey key);
	TERRAENGINE_API void EvaluateVirtualGeometryCapabilitySupport(
		VirtualGeometryCapabilityEvidence &evidence, bool supported, std::string supportReason, std::string queriedLimits);
	TERRAENGINE_API bool AttachVirtualGeometryExecutionEvidence(VirtualGeometryCapabilityEvidence &evidence, VirtualGeometryCapabilityExecution execution);
	TERRAENGINE_API bool AttachVirtualGeometryValidationEvidence(VirtualGeometryCapabilityEvidence &evidence, std::vector<std::string> validationArtifactIds);
	TERRAENGINE_API VirtualGeometryBenchmarkSampleStatistics BuildVirtualGeometryBenchmarkStatistics(
		const std::vector<VirtualGeometryMeasurementRecord> &samples);
	TERRAENGINE_API std::string BuildVirtualGeometryBenchmarkIntegrityHash(const VirtualGeometryBenchmarkRun &run);
	TERRAENGINE_API VirtualGeometryBenchmarkComparability EvaluateVirtualGeometryBenchmarkComparability(
		const VirtualGeometryBenchmarkRun &baseline, const VirtualGeometryBenchmarkRun &candidate);
} // namespace TerraEngine::Renderer
