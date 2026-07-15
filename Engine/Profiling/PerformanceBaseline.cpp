// PerformanceBaseline.cpp — Implements validation and correctness-first baseline comparison rules.

#include "Profiling/PerformanceBaseline.h"

#include <algorithm>
#include <array>
#include <utility>

namespace TerraEngine::Profiling
{
namespace
{
// AddDiagnostic keeps validation sites concise while ensuring every rejected
// record gives automation and beginners a concrete explanation.
void AddDiagnostic(BaselineValidationResult& result, std::string diagnostic)
{
	result.diagnostics.push_back(std::move(diagnostic));
}

// IsIntentionallyVaried prevents a comparison mismatch only when the caller
// explicitly records that the corresponding experiment changed that dimension.
bool IsIntentionallyVaried(
	const std::vector<BaselineComparisonDimension>& dimensions,
	BaselineComparisonDimension expected)
{
	return std::find(dimensions.begin(), dimensions.end(), expected) != dimensions.end();
}

// WorkloadsMatch compares every input that affects work volume or scheduling.
bool WorkloadsMatch(const PerformanceWorkloadIdentity& left, const PerformanceWorkloadIdentity& right)
{
	return left.scenarioId == right.scenarioId
		&& left.scale == right.scale
		&& left.totalObjectCount == right.totalObjectCount
		&& left.dynamicBodyCount == right.dynamicBodyCount
		&& left.kinematicBodyCount == right.kinematicBodyCount
		&& left.staticBodyCount == right.staticBodyCount
		&& left.measuredStepCount == right.measuredStepCount
		&& left.settingsHash == right.settingsHash;
}

// IsKnownMetricUnit rejects malformed serialized values before a comparison can
// combine quantities with different meanings.
bool IsKnownMetricUnit(PerformanceMetricUnit unit)
{
	return unit == PerformanceMetricUnit::Nanoseconds
		|| unit == PerformanceMetricUnit::Bytes
		|| unit == PerformanceMetricUnit::Count
		|| unit == PerformanceMetricUnit::Percent;
}

// IsKnownMetricAvailability protects missing-evidence semantics from corrupt
// enum payloads that would otherwise be interpreted as measured data.
bool IsKnownMetricAvailability(PerformanceMetricAvailability availability)
{
	return availability == PerformanceMetricAvailability::Measured
		|| availability == PerformanceMetricAvailability::Unavailable;
}

// IsKnownWorkloadScale keeps external artifacts from bypassing the deliberate
// small-scene versus canonical-large-scene separation.
bool IsKnownWorkloadScale(PerformanceWorkloadScale scale)
{
	return scale == PerformanceWorkloadScale::SmallScene || scale == PerformanceWorkloadScale::CanonicalLargeScene;
}
}

PerformanceBaselineIdentity::PerformanceBaselineIdentity(PerformanceBaselineIdentityInput input) noexcept
	: data_(std::move(input))
{
}

std::optional<PerformanceBaselineIdentity> PerformanceBaselineIdentity::TryCreate(
	PerformanceBaselineIdentityInput input,
	BaselineValidationResult& validation)
{
	validation = {};
	if (input.baselineId.empty()) AddDiagnostic(validation, "Baseline identity requires a baseline id.");
	if (input.featureRevision.empty()) AddDiagnostic(validation, "Baseline identity requires a feature revision.");
	if (input.buildRevision.empty()) AddDiagnostic(validation, "Baseline identity requires a build revision.");
	if (input.installedTreeIdentity.empty()) AddDiagnostic(validation, "Baseline identity requires an installed-tree identity.");
	if (input.hardwareFingerprint.empty()) AddDiagnostic(validation, "Baseline identity requires a hardware fingerprint.");
	if (input.operatingSystem.empty()) AddDiagnostic(validation, "Baseline identity requires an operating-system identity.");
	if (input.workerCount == 0u) AddDiagnostic(validation, "Baseline identity requires at least one worker.");
	if (input.projectSourceHash.empty()) AddDiagnostic(validation, "Baseline identity requires a project source hash.");
	if (input.artifactHash.empty()) AddDiagnostic(validation, "Baseline identity requires an artifact hash.");
	if (input.captureDetailLevel.empty()) AddDiagnostic(validation, "Baseline identity requires a capture detail level.");
	if (input.editorLayoutHash.empty()) AddDiagnostic(validation, "Baseline identity requires an editor layout hash.");
	if (input.editorPreferenceHash.empty()) AddDiagnostic(validation, "Baseline identity requires an editor preference hash.");

	const BaselineValidationResult workloadValidation = ValidatePerformanceWorkload(input.workload);
	validation.diagnostics.insert(validation.diagnostics.end(), workloadValidation.diagnostics.begin(), workloadValidation.diagnostics.end());
	validation.valid = validation.diagnostics.empty();
	if (!validation.valid)
	{
		return std::nullopt;
	}

	// Construct the value inside this class method, where the private validating
	// constructor is accessible, before handing it to optional's public move path.
	return std::optional<PerformanceBaselineIdentity>(PerformanceBaselineIdentity(std::move(input)));
}

const PerformanceBaselineIdentityInput& PerformanceBaselineIdentity::Data() const noexcept
{
	return data_;
}

BaselineValidationResult ValidatePerformanceMetric(const PerformanceMetric& metric)
{
	BaselineValidationResult result;
	if (metric.metricId.empty()) AddDiagnostic(result, "Performance metric requires a metric id.");
	if (!IsKnownMetricUnit(metric.unit)) AddDiagnostic(result, "Performance metric has an unrecognized unit.");
	if (!IsKnownMetricAvailability(metric.availability)) AddDiagnostic(result, "Performance metric has an unrecognized availability state.");
	if (metric.availability == PerformanceMetricAvailability::Measured && !metric.value.has_value())
	{
		AddDiagnostic(result, "A measured performance metric requires a value.");
	}
	if (metric.availability == PerformanceMetricAvailability::Unavailable)
	{
		if (metric.value.has_value()) AddDiagnostic(result, "An unavailable performance metric must not contain a numeric value.");
		if (metric.unavailableReason.empty()) AddDiagnostic(result, "An unavailable performance metric requires a reason.");
	}
	result.valid = result.diagnostics.empty();
	return result;
}

BaselineValidationResult ValidatePerformanceWorkload(const PerformanceWorkloadIdentity& workload)
{
	BaselineValidationResult result;
	if (workload.scenarioId.empty()) AddDiagnostic(result, "Performance workload requires a scenario id.");
	if (!IsKnownWorkloadScale(workload.scale)) AddDiagnostic(result, "Performance workload has an unrecognized scale.");
	if (workload.settingsHash.empty()) AddDiagnostic(result, "Performance workload requires a settings hash.");
	if (workload.totalObjectCount == 0u) AddDiagnostic(result, "Performance workload requires at least one object.");
	if (workload.measuredStepCount == 0u) AddDiagnostic(result, "Performance workload requires at least one measured step.");
	const std::uint64_t countedBodies = static_cast<std::uint64_t>(workload.dynamicBodyCount)
		+ workload.kinematicBodyCount + workload.staticBodyCount;
	if (countedBodies != workload.totalObjectCount)
	{
		AddDiagnostic(result, "Performance workload body counts must equal the total object count.");
	}
	if (workload.scale == PerformanceWorkloadScale::CanonicalLargeScene
		&& (workload.totalObjectCount != 10000u || workload.dynamicBodyCount != 1000u
			|| workload.kinematicBodyCount != 1000u || workload.staticBodyCount != 8000u))
	{
		AddDiagnostic(result, "The canonical large-scene workload must retain its 8,000 static, 1,000 dynamic, and 1,000 kinematic body counts.");
	}
	result.valid = result.diagnostics.empty();
	return result;
}

PerformanceComparisonEligibility AssessBaselineComparisonEligibility(
	const PerformanceBaselineIdentity& baseline,
	const PerformanceBaselineIdentity& candidate,
	const std::vector<BaselineComparisonDimension>& intentionallyVariedDimensions)
{
	PerformanceComparisonEligibility result;
	const PerformanceBaselineIdentityInput& expected = baseline.Data();
	const PerformanceBaselineIdentityInput& actual = candidate.Data();
	auto requireEqual = [&result, &intentionallyVariedDimensions](
		BaselineComparisonDimension dimension,
		bool equal,
		const char* description) {
		if (!equal && !IsIntentionallyVaried(intentionallyVariedDimensions, dimension))
		{
			result.mismatches.emplace_back(description);
		}
	};

	requireEqual(BaselineComparisonDimension::Workload, WorkloadsMatch(expected.workload, actual.workload), "Workload identity differs.");
	requireEqual(BaselineComparisonDimension::Hardware, expected.hardwareFingerprint == actual.hardwareFingerprint, "Hardware identity differs.");
	requireEqual(BaselineComparisonDimension::OperatingSystem, expected.operatingSystem == actual.operatingSystem, "Operating-system identity differs.");
	requireEqual(BaselineComparisonDimension::WorkerCount, expected.workerCount == actual.workerCount, "Worker count differs.");
	requireEqual(BaselineComparisonDimension::ProjectSource, expected.projectSourceHash == actual.projectSourceHash, "Project source hash differs.");
	requireEqual(BaselineComparisonDimension::Artifact, expected.artifactHash == actual.artifactHash, "Artifact identity differs.");
	requireEqual(BaselineComparisonDimension::CaptureDetailLevel, expected.captureDetailLevel == actual.captureDetailLevel, "Capture detail level differs.");
	requireEqual(BaselineComparisonDimension::EditorLayout, expected.editorLayoutHash == actual.editorLayoutHash, "Editor layout differs.");
	requireEqual(BaselineComparisonDimension::EditorPreferences, expected.editorPreferenceHash == actual.editorPreferenceHash, "Editor preferences differ.");
	result.eligible = result.mismatches.empty();
	return result;
}

BaselineValidationResult ValidateCorrectnessGates(const std::vector<CorrectnessGateResult>& gates)
{
	BaselineValidationResult result;
	constexpr std::array allGates{
		CorrectnessGate::Determinism,
		CorrectnessGate::Rollback,
		CorrectnessGate::Visibility,
		CorrectnessGate::Lifecycle,
		CorrectnessGate::ExportParity,
		CorrectnessGate::AuthoredStateRestoration,
		CorrectnessGate::NormalShutdown
	};
	for (const CorrectnessGate requiredGate : allGates)
	{
		const auto matches = std::count_if(gates.begin(), gates.end(), [requiredGate](const CorrectnessGateResult& gate) {
			return gate.gate == requiredGate;
		});
		if (matches != 1)
		{
			AddDiagnostic(result, "Every correctness gate must be reported exactly once.");
			break;
		}
	}
	result.valid = result.diagnostics.empty();
	return result;
}

bool PerformanceAcceptanceOutcome::IsAccepted() const
{
	if (!eligibility.eligible || !ValidateCorrectnessGates(correctness).valid || metrics.empty())
	{
		return false;
	}
	if (std::any_of(correctness.begin(), correctness.end(), [](const CorrectnessGateResult& gate) { return !gate.passed; }))
	{
		return false;
	}
	return std::none_of(metrics.begin(), metrics.end(), [](const PerformanceMetricComparison& metric) {
		return metric.required && metric.status != PerformanceMetricComparisonStatus::Met;
	});
}
}
