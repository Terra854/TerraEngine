#include "Renderer/VirtualGeometry/Capability/VirtualGeometryCapability.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace TerraEngine::Renderer
{
	namespace
	{
		constexpr std::uint32_t kMinimumComparableMeasuredRuns = 5u;

		void HashText(std::uint64_t &hash, std::string_view text)
		{
			// FNV-1a is used only as a deterministic capture fingerprint. It is not
			// a security primitive and the benchmark writer remains responsible for
			// publishing its stronger artifact hashes alongside this convenience ID.
			for (const unsigned char character : text)
			{
				hash ^= character;
				hash *= 1099511628211ull;
			}
			hash ^= 0xffu;
			hash *= 1099511628211ull;
		}

		void HashNumber(std::uint64_t &hash, std::uint64_t value) { HashText(hash, std::to_string(value)); }

		std::uint64_t PercentileUpperNearest(const std::vector<std::uint64_t> &values, std::uint32_t percentile)
		{
			if (values.empty())
			{
				return 0u;
			}
			const std::size_t numerator = static_cast<std::size_t>(percentile) * values.size();
			const std::size_t rank = (numerator + 99u) / 100u;
			return values[std::max<std::size_t>(1u, rank) - 1u];
		}
	} // namespace

	VirtualGeometryCapabilityEvidence MakePlannedVirtualGeometryCapability(VirtualGeometryCapabilityKey key)
	{
		VirtualGeometryCapabilityEvidence evidence{};
		evidence.key = std::move(key);
		evidence.planned = true;
		return evidence;
	}

	void EvaluateVirtualGeometryCapabilitySupport(
		VirtualGeometryCapabilityEvidence &evidence, bool supported, std::string supportReason, std::string queriedLimits)
	{
		// A new device/support decision invalidates all command and validation facts
		// from the prior evaluation. The reset happens together before publication.
		evidence.supported = supported;
		evidence.supportReason = std::move(supportReason);
		evidence.queriedLimits = std::move(queriedLimits);
		evidence.executed = false;
		evidence.validated = false;
		evidence.execution = {};
		evidence.validationArtifactIds.clear();
	}

	bool AttachVirtualGeometryExecutionEvidence(VirtualGeometryCapabilityEvidence &evidence, VirtualGeometryCapabilityExecution execution)
	{
		if (!evidence.planned || !evidence.supported || !execution.IsComplete())
		{
			return false;
		}

		// Copy the complete observation before exposing the executed fact so readers
		// cannot observe an executed capability without its frame/view evidence ID.
		evidence.execution = std::move(execution);
		evidence.executed = true;
		evidence.validated = false;
		evidence.validationArtifactIds.clear();
		return true;
	}

	bool AttachVirtualGeometryValidationEvidence(VirtualGeometryCapabilityEvidence &evidence, std::vector<std::string> validationArtifactIds)
	{
		validationArtifactIds.erase(std::remove(validationArtifactIds.begin(), validationArtifactIds.end(), std::string()), validationArtifactIds.end());
		std::sort(validationArtifactIds.begin(), validationArtifactIds.end());
		validationArtifactIds.erase(std::unique(validationArtifactIds.begin(), validationArtifactIds.end()), validationArtifactIds.end());
		if (!evidence.CanReportExecuted() || validationArtifactIds.empty())
		{
			return false;
		}

		evidence.validationArtifactIds = std::move(validationArtifactIds);
		evidence.validated = true;
		return true;
	}

	VirtualGeometryBenchmarkSampleStatistics BuildVirtualGeometryBenchmarkStatistics(const std::vector<VirtualGeometryMeasurementRecord> &samples)
	{
		VirtualGeometryBenchmarkSampleStatistics statistics{};
		std::vector<std::uint64_t> measuredDurations;
		measuredDurations.reserve(samples.size());
		for (const VirtualGeometryMeasurementRecord &sample : samples)
		{
			if (sample.HasAuthoritativeDuration())
			{
				measuredDurations.push_back(*sample.durationNanoseconds);
				continue;
			}
			++statistics.rejectedCount;
			statistics.rejectionReasons.push_back(sample.rejectionReason.empty() ? "sample was not authoritatively measured" : sample.rejectionReason);
		}
		std::sort(measuredDurations.begin(), measuredDurations.end());
		statistics.measuredCount = static_cast<std::uint32_t>(measuredDurations.size());
		if (measuredDurations.empty())
		{
			return statistics;
		}

		statistics.medianNanoseconds = PercentileUpperNearest(measuredDurations, 50u);
		statistics.p95Nanoseconds = PercentileUpperNearest(measuredDurations, 95u);
		statistics.p99Nanoseconds = PercentileUpperNearest(measuredDurations, 99u);
		statistics.worstNanoseconds = measuredDurations.back();

		std::uint64_t sum = 0u;
		for (const std::uint64_t duration : measuredDurations)
		{
			sum += duration;
		}
		const std::uint64_t mean = sum / measuredDurations.size();
		std::uint64_t squaredDifferenceSum = 0u;
		for (const std::uint64_t duration : measuredDurations)
		{
			const std::uint64_t difference = duration >= mean ? duration - mean : mean - duration;
			squaredDifferenceSum += difference * difference;
		}
		statistics.varianceNanosecondsSquared = squaredDifferenceSum / measuredDurations.size();
		return statistics;
	}

	std::string BuildVirtualGeometryBenchmarkIntegrityHash(const VirtualGeometryBenchmarkRun &run)
	{
		std::uint64_t hash = 1469598103934665603ull;
		HashText(hash, run.captureId);
		HashText(hash, run.scope.backend);
		HashText(hash, run.scope.adapter);
		HashText(hash, run.scope.driver);
		HashText(hash, run.scope.route);
		HashText(hash, run.scope.resolution);
		HashText(hash, run.scope.quality);
		HashNumber(hash, run.scope.viewCount);
		HashText(hash, run.scope.workload);
		HashText(hash, run.sourceHash);
		HashText(hash, run.executableHash);
		HashText(hash, run.shaderHash);
		HashText(hash, run.assetHash);
		HashText(hash, run.buildRecipeHash);
		HashNumber(hash, run.randomizedOrder ? 1u : 0u);
		for (const VirtualGeometryMeasurementRecord &sample : run.samples)
		{
			HashNumber(hash, static_cast<std::uint64_t>(sample.availability));
			HashNumber(hash, sample.durationNanoseconds.value_or(0u));
			HashText(hash, sample.queryIdentity);
			HashText(hash, sample.executionEvidenceId);
			HashText(hash, sample.rejectionReason);
		}
		std::ostringstream output;
		output << std::hex << std::setfill('0') << std::setw(16) << hash;
		return output.str();
	}

	VirtualGeometryBenchmarkComparability EvaluateVirtualGeometryBenchmarkComparability(
		const VirtualGeometryBenchmarkRun &baseline, const VirtualGeometryBenchmarkRun &candidate)
	{
		VirtualGeometryBenchmarkComparability result{};
		if (baseline.scope != candidate.scope)
		{
			result.reason = "baseline and candidate execution scope differ";
			return result;
		}
		if (!baseline.randomizedOrder || !candidate.randomizedOrder)
		{
			result.reason = "paired comparison requires randomized ordering";
			return result;
		}
		const VirtualGeometryBenchmarkSampleStatistics baselineStatistics = BuildVirtualGeometryBenchmarkStatistics(baseline.samples);
		const VirtualGeometryBenchmarkSampleStatistics candidateStatistics = BuildVirtualGeometryBenchmarkStatistics(candidate.samples);
		if (baselineStatistics.measuredCount < kMinimumComparableMeasuredRuns || candidateStatistics.measuredCount < kMinimumComparableMeasuredRuns)
		{
			result.reason = "paired comparison requires at least five measured runs per side";
			return result;
		}
		result.comparable = true;
		return result;
	}
} // namespace TerraEngine::Renderer
