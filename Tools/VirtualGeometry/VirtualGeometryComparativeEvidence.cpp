#include "VirtualGeometryComparativeEvidence.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Tools::VirtualGeometry
{
	namespace
	{
		std::vector<double> ValidValues(const std::vector<ComparisonSample>& samples, ComparisonReport& report)
		{
			std::vector<double> values;
			for (const auto& sample : samples)
			{
				if (sample.availability == SampleAvailability::Unavailable)
				{
					++report.unavailableCount;
					continue;
				}
				if (!sample.rejectionReason.empty() || !sample.visualCorrect)
				{
					++report.rejectedCount;
					continue;
				}
				values.push_back(sample.value);
			}
			std::sort(values.begin(), values.end());
			return values;
		}

		bool SamplesMatchByIdentity(const ComparisonInput& input)
		{
			if(input.terraSamples.size() != input.referenceSamples.size()) return false;
			for(std::size_t index = 0u; index < input.terraSamples.size(); ++index)
			{
				const std::string& candidate = input.terraSamples[index].sampleIdentity;
				const std::string& reference = input.referenceSamples[index].sampleIdentity;
				if(candidate.empty() || candidate != reference) return false;
			}
			return true;
		}

		double Quantile(const std::vector<double>& values, double probability)
		{
			if (values.empty()) return 0.0;
			const auto index = static_cast<std::size_t>(std::min<double>(values.size() - 1u, std::ceil(values.size() * probability) - 1.0));
			return values[index];
		}
	}

	bool ComparisonIdentity::IsComplete() const noexcept
	{
		return !contentHash.empty() && !cameraPathHash.empty()
			&& !qualityProfileHash.empty() && !hardwareIdentity.empty()
			&& !graphicsPath.empty() && !runtimeRevision.empty()
			&& !commandHash.empty();
	}

	bool ComparisonIdentity::Matches(const ComparisonIdentity& other) const noexcept
	{
		return IsComplete() && other.IsComplete()
			&& contentHash == other.contentHash
			&& cameraPathHash == other.cameraPathHash
			&& qualityProfileHash == other.qualityProfileHash
			&& hardwareIdentity == other.hardwareIdentity
			&& graphicsPath == other.graphicsPath
			&& runtimeRevision == other.runtimeRevision
			&& commandHash == other.commandHash;
	}

	ComparisonValidation ValidateComparisonInput(const ComparisonInput& input)
	{
		ComparisonReport report;
		const auto terra = ValidValues(input.terraSamples, report);
		const auto reference = ValidValues(input.referenceSamples, report);
		ComparisonValidation validation{};
		validation.unavailableCount = report.unavailableCount;
		validation.rejectedCount = report.rejectedCount;
		if(!input.referenceSupported) validation.rejectionReason = "The reference Vulkan path is unavailable.";
		else if(!input.terraIdentity.Matches(input.referenceIdentity)) validation.rejectionReason = "Capture provenance identities do not match exactly.";
		else if(input.terraIdentity.graphicsPath != "Vulkan") validation.rejectionReason = "Only matched Vulkan captures are eligible.";
		else if(!SamplesMatchByIdentity(input)) validation.rejectionReason = "Raw candidate and reference samples are not exactly paired.";
		else if(report.unavailableCount != 0u || report.rejectedCount != 0u) validation.rejectionReason = "Unavailable or rejected raw samples prevent a claim.";
		else if(terra.size() < 6u || reference.size() < 6u) validation.rejectionReason = "At least six valid paired samples are required.";
		else validation.valid = true;
		return validation;
	}

	ComparisonReport BuildComparisonReport(const ComparisonInput& input)
	{
		ComparisonReport report;
		report.referenceSupported = input.referenceSupported;
		const auto terra = ValidValues(input.terraSamples, report);
		const auto reference = ValidValues(input.referenceSamples, report);
		const ComparisonValidation validation = ValidateComparisonInput(input);
		report.visualGatePassed = validation.valid;
		if (!terra.empty()) { report.terraMedian = Quantile(terra, 0.50); report.terraP95 = Quantile(terra, 0.95); report.terraP99 = Quantile(terra, 0.99); }
		if (!reference.empty()) { report.referenceMedian = Quantile(reference, 0.50); report.referenceP95 = Quantile(reference, 0.95); report.referenceP99 = Quantile(reference, 0.99); }
		report.performanceImprovement = report.referenceMedian > 0.0 ? (report.referenceMedian - report.terraMedian) / report.referenceMedian : 0.0;
		report.classification = ClassifyComparison(report);
		return report;
	}

	ComparisonClassification ClassifyComparison(const ComparisonReport& report)
	{
		if (!report.referenceSupported) return ComparisonClassification::Incomparable;
		if (!report.visualGatePassed) return ComparisonClassification::Insufficient;
		if (report.protectedMetricsRegressed) return ComparisonClassification::Regression;
		if (report.performanceImprovement >= report.superiorityMargin)
		{
			return ComparisonClassification::TerraEngineWin;
		}
		if (report.performanceImprovement < -report.parityRegressionLimit)
		{
			return ComparisonClassification::Regression;
		}
		return ComparisonClassification::Parity;
	}

	ComparisonClassification ClassifyComparisonAggregate(
		const std::vector<ComparisonReport>& reports)
	{
		if(reports.empty())
		{
			return ComparisonClassification::Insufficient;
		}
		bool hasInsufficient = false;
		bool hasIncomparable = false;
		for(const ComparisonReport& report : reports)
		{
			const ComparisonClassification classification =
				ClassifyComparison(report);
			if(classification == ComparisonClassification::Regression)
			{
				// One regression is globally visible and cannot be averaged away.
				return ComparisonClassification::Regression;
			}
			hasInsufficient = hasInsufficient
				|| classification == ComparisonClassification::Insufficient;
			hasIncomparable = hasIncomparable
				|| classification == ComparisonClassification::Incomparable;
		}
		if(hasInsufficient)
		{
			return ComparisonClassification::Insufficient;
		}
		if(hasIncomparable)
		{
			return ComparisonClassification::Incomparable;
		}
		// Superiority is workload-scoped. Even if every workload wins, the
		// aggregate stays a non-promotional summary classification.
		return ComparisonClassification::Parity;
	}
}
