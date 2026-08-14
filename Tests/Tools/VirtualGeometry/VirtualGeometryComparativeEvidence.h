#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace TerraEngine::Tools::VirtualGeometry
{
	enum class SampleAvailability { Measured, Unavailable };
	enum class ComparisonClassification { TerraEngineWin, Parity, Regression, Incomparable, Insufficient };

	// A comparison is meaningful only when both captures pin the same content,
	// camera, quality, adapter/driver, Vulkan path, runtime revision, and input
	// command stream. Keep the fields explicit rather than accepting one opaque
	// hash so a rejected pair explains exactly what drifted.
	struct ComparisonIdentity
	{
		std::string contentHash;
		std::string cameraPathHash;
		std::string qualityProfileHash;
		std::string hardwareIdentity;
		std::string graphicsPath;
		std::string runtimeRevision;
		std::string commandHash;

		[[nodiscard]] bool IsComplete() const noexcept;
		[[nodiscard]] bool Matches(const ComparisonIdentity& other) const noexcept;
	};

	struct ComparisonSample
	{
		SampleAvailability availability = SampleAvailability::Measured;
		bool visualCorrect = false;
		std::string rejectionReason;
		std::string sampleIdentity;
		double value = 0.0;
	};

	struct ComparisonInput
	{
		std::vector<ComparisonSample> terraSamples;
		std::vector<ComparisonSample> referenceSamples;
		ComparisonIdentity terraIdentity;
		ComparisonIdentity referenceIdentity;
		bool referenceSupported = true;
	};

	struct ComparisonValidation
	{
		bool valid = false;
		std::size_t unavailableCount = 0;
		std::size_t rejectedCount = 0;
		std::string rejectionReason;
	};

	struct ComparisonReport
	{
		ComparisonClassification classification = ComparisonClassification::Insufficient;
		std::size_t unavailableCount = 0;
		std::size_t rejectedCount = 0;
		bool visualGatePassed = false;
		bool protectedMetricsRegressed = false;
		bool referenceSupported = true;
		double performanceImprovement = 0.0;
		// A workload-specific superiority claim requires at least ten percent
		// lower median total-frame cost. Parity permits at most ten percent
		// regression; protected tail/residency checks are represented separately.
		double superiorityMargin = 0.10;
		double parityRegressionLimit = 0.10;
		double terraMedian = 0.0;
		double terraP95 = 0.0;
		double terraP99 = 0.0;
		double referenceMedian = 0.0;
		double referenceP95 = 0.0;
		double referenceP99 = 0.0;
	};

	[[nodiscard]] ComparisonValidation ValidateComparisonInput(const ComparisonInput& input);
	[[nodiscard]] ComparisonReport BuildComparisonReport(const ComparisonInput& input);
	[[nodiscard]] ComparisonClassification ClassifyComparison(const ComparisonReport& report);
	[[nodiscard]] ComparisonClassification ClassifyComparisonAggregate(
		const std::vector<ComparisonReport>& reports);
}
