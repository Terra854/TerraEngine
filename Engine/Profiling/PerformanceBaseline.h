// PerformanceBaseline.h — Defines immutable, correctness-gated performance comparison evidence.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Profiling
{
// PerformanceWorkloadScale keeps small-scene controls separate from canonical
// large-scene evidence. A result from one scale must never silently become the
// baseline for the other scale.
enum class PerformanceWorkloadScale
{
	SmallScene,
	CanonicalLargeScene
};

// PerformanceMetricUnit makes every comparison explicit about the quantity it
// measures so, for example, a byte count cannot be compared with a duration.
enum class PerformanceMetricUnit
{
	Nanoseconds,
	Bytes,
	Count,
	Percent
};

// PerformanceMetricAvailability distinguishes a missing measurement from a
// measured zero. Callers must not treat unavailable evidence as a passing value.
enum class PerformanceMetricAvailability
{
	Measured,
	Unavailable
};

// PerformanceMetric records one accepted or candidate measurement in a unit
// that remains meaningful after the profiler artifact is written to disk.
struct PerformanceMetric
{
	std::string metricId;
	PerformanceMetricUnit unit = PerformanceMetricUnit::Nanoseconds;
	PerformanceMetricAvailability availability = PerformanceMetricAvailability::Unavailable;
	std::optional<std::uint64_t> value;
	std::string unavailableReason;
};

// PerformanceWorkloadIdentity describes the scene and settings that cause
// performance work. The exact body counts prevent a reduced workload from being
// presented as a canonical comparison.
struct PerformanceWorkloadIdentity
{
	std::string scenarioId;
	PerformanceWorkloadScale scale = PerformanceWorkloadScale::SmallScene;
	std::uint32_t totalObjectCount = 0u;
	std::uint32_t dynamicBodyCount = 0u;
	std::uint32_t kinematicBodyCount = 0u;
	std::uint32_t staticBodyCount = 0u;
	std::uint32_t measuredStepCount = 0u;
	std::string settingsHash;
};

// PerformanceBaselineIdentityInput holds the data that is validated before it
// becomes an immutable PerformanceBaselineIdentity.
struct PerformanceBaselineIdentityInput
{
	std::string baselineId;
	std::string featureRevision;
	std::string buildRevision;
	std::string installedTreeIdentity;
	PerformanceWorkloadIdentity workload;
	std::string hardwareFingerprint;
	std::string operatingSystem;
	std::uint32_t workerCount = 0u;
	std::string projectSourceHash;
	std::string artifactHash;
	std::string captureDetailLevel;
	std::string editorLayoutHash;
	std::string editorPreferenceHash;
};

// BaselineValidationResult keeps validation failures as readable diagnostics so
// automation can explain exactly why evidence was rejected instead of guessing.
struct BaselineValidationResult
{
	bool valid = false;
	std::vector<std::string> diagnostics;
};

// PerformanceBaselineIdentity exposes only const access to a validated input.
// This protects baseline comparability fields after an accepted baseline exists.
class PerformanceBaselineIdentity final
{
public:
	PerformanceBaselineIdentity(const PerformanceBaselineIdentity&) = default;
	PerformanceBaselineIdentity(PerformanceBaselineIdentity&&) noexcept = default;
	PerformanceBaselineIdentity& operator=(const PerformanceBaselineIdentity&) = delete;
	PerformanceBaselineIdentity& operator=(PerformanceBaselineIdentity&&) = delete;

	[[nodiscard]] static std::optional<PerformanceBaselineIdentity> TryCreate(
		PerformanceBaselineIdentityInput input,
		BaselineValidationResult& validation);
	[[nodiscard]] const PerformanceBaselineIdentityInput& Data() const noexcept;

private:
	explicit PerformanceBaselineIdentity(PerformanceBaselineIdentityInput input) noexcept;

	const PerformanceBaselineIdentityInput data_;
};

// BaselineComparisonDimension identifies the field intentionally varied by an
// experiment. Every other comparable field must remain identical.
enum class BaselineComparisonDimension
{
	Workload,
	Hardware,
	OperatingSystem,
	WorkerCount,
	ProjectSource,
	Artifact,
	CaptureDetailLevel,
	EditorLayout,
	EditorPreferences
};

struct PerformanceComparisonEligibility
{
	bool eligible = false;
	std::vector<std::string> mismatches;
};

[[nodiscard]] BaselineValidationResult ValidatePerformanceMetric(const PerformanceMetric& metric);
[[nodiscard]] BaselineValidationResult ValidatePerformanceWorkload(const PerformanceWorkloadIdentity& workload);
[[nodiscard]] PerformanceComparisonEligibility AssessBaselineComparisonEligibility(
	const PerformanceBaselineIdentity& baseline,
	const PerformanceBaselineIdentity& candidate,
	const std::vector<BaselineComparisonDimension>& intentionallyVariedDimensions);

// CorrectnessGate names the checks that must pass before a faster measurement
// is accepted. These gates reflect the feature's deterministic and transactional
// runtime contracts rather than benchmark-only conditions.
enum class CorrectnessGate
{
	Determinism,
	Rollback,
	Visibility,
	Lifecycle,
	ExportParity,
	AuthoredStateRestoration,
	NormalShutdown
};

struct CorrectnessGateResult
{
	CorrectnessGate gate = CorrectnessGate::Determinism;
	bool passed = false;
	std::string detail;
};

enum class PerformanceMetricComparisonStatus
{
	Met,
	Regressed,
	Unavailable
};

// PerformanceMetricComparison stores a decision already made by the relevant
// threshold policy. It still retains availability so a required absent stream
// cannot become a false performance success.
struct PerformanceMetricComparison
{
	std::string metricId;
	bool required = true;
	PerformanceMetricComparisonStatus status = PerformanceMetricComparisonStatus::Unavailable;
	std::string detail;
};

// PerformanceAcceptanceOutcome combines comparability, correctness, and metric
// evidence. IsAccepted is deliberately derived so callers cannot mark a result
// successful while a required correctness or metric stream is missing.
struct PerformanceAcceptanceOutcome
{
	PerformanceComparisonEligibility eligibility;
	std::vector<CorrectnessGateResult> correctness;
	std::vector<PerformanceMetricComparison> metrics;

	[[nodiscard]] bool IsAccepted() const;
};

[[nodiscard]] BaselineValidationResult ValidateCorrectnessGates(const std::vector<CorrectnessGateResult>& gates);
}
