// PerformanceTelemetry.cpp — Implements defensive validation for typed profiler evidence.

#include "Profiling/PerformanceTelemetry.h"

#include <utility>

namespace TerraEngine::Profiling
{
namespace
{
// AddDiagnostic ensures each validator returns actionable capture diagnostics
// without throwing while loading an incomplete or malformed profiler artifact.
void AddDiagnostic(TelemetryValidationResult& result, std::string diagnostic)
{
	result.diagnostics.push_back(std::move(diagnostic));
}

// Merge appends nested duration failures to their parent record validator.
void Merge(TelemetryValidationResult& target, const TelemetryValidationResult& source, const char* fieldName)
{
	for (const std::string& diagnostic : source.diagnostics)
	{
		target.diagnostics.push_back(std::string(fieldName) + ": " + diagnostic);
	}
}

// Finish marks a validation result valid only after all checks have run, so a
// caller gets every relevant error from a single malformed record.
TelemetryValidationResult Finish(TelemetryValidationResult result)
{
	result.valid = result.diagnostics.empty();
	return result;
}

// IsKnownAvailability makes deserialized enum values safe before they are used
// as profiler evidence. Future readers can reject corrupted numeric payloads.
bool IsKnownAvailability(TelemetryAvailability availability)
{
	return availability == TelemetryAvailability::Available || availability == TelemetryAvailability::Unavailable;
}

// IsKnownStageDomain rejects malformed values while preserving the intentional
// separation between physics and Scene View timing streams.
bool IsKnownStageDomain(PerformanceStageDomain domain)
{
	return domain == PerformanceStageDomain::Physics
		|| domain == PerformanceStageDomain::SceneView
		|| domain == PerformanceStageDomain::Editor;
}

// IsKnownPresentationWaitKind prevents unknown waits from being attributed to a
// misleading known category during later summary aggregation.
bool IsKnownPresentationWaitKind(PresentationWaitKind kind)
{
	return kind == PresentationWaitKind::SwapchainAcquire
		|| kind == PresentationWaitKind::RenderQueueBackpressure
		|| kind == PresentationWaitKind::GpuCompletion
		|| kind == PresentationWaitKind::UiComposition;
}

// These explicit checks make corrupted capture enum values reject cleanly
// instead of being attributed to a plausible but incorrect scheduler path.
bool IsKnownWorkloadExecutionPath(WorkloadExecutionPath path)
{
	return path == WorkloadExecutionPath::None
		|| path == WorkloadExecutionPath::Inline
		|| path == WorkloadExecutionPath::Worker
		|| path == WorkloadExecutionPath::Protected
		|| path == WorkloadExecutionPath::Rejected;
}

bool IsKnownWorkloadExecutionStatus(WorkloadExecutionStatus status)
{
	return status == WorkloadExecutionStatus::Inactive
		|| status == WorkloadExecutionStatus::NotReady
		|| status == WorkloadExecutionStatus::Inline
		|| status == WorkloadExecutionStatus::Queued
		|| status == WorkloadExecutionStatus::Running
		|| status == WorkloadExecutionStatus::Completed
		|| status == WorkloadExecutionStatus::Rejected
		|| status == WorkloadExecutionStatus::Cancelled;
}
}

TelemetryValidationResult ValidateTelemetryDuration(const TelemetryDuration& duration)
{
	TelemetryValidationResult result;
	if (!IsKnownAvailability(duration.availability))
	{
		AddDiagnostic(result, "Duration availability is not a recognized value.");
		return Finish(std::move(result));
	}
	if (duration.availability == TelemetryAvailability::Available && !duration.unavailableReason.empty())
	{
		AddDiagnostic(result, "An available duration must not include an unavailable reason.");
	}
	if (duration.availability == TelemetryAvailability::Unavailable)
	{
		if (duration.nanoseconds != 0u) AddDiagnostic(result, "An unavailable duration must not include a numeric duration.");
		if (duration.unavailableReason.empty()) AddDiagnostic(result, "An unavailable duration requires a reason.");
	}
	return Finish(std::move(result));
}

TelemetryValidationResult ValidatePerformanceStageRecord(const PerformanceStageRecord& record)
{
	TelemetryValidationResult result;
	if (!IsKnownStageDomain(record.domain)) AddDiagnostic(result, "A stage record has an unrecognized domain.");
	if (record.stageName.empty()) AddDiagnostic(result, "A stage record requires a stage name.");
	Merge(result, ValidateTelemetryDuration(record.duration), "duration");
	return Finish(std::move(result));
}

TelemetryValidationResult ValidateProtectedBoundaryRecord(const ProtectedBoundaryRecord& record)
{
	TelemetryValidationResult result;
	if (record.boundaryName.empty()) AddDiagnostic(result, "A protected-boundary record requires a boundary name.");
	Merge(result, ValidateTelemetryDuration(record.queueDuration), "queue duration");
	Merge(result, ValidateTelemetryDuration(record.ownershipWaitDuration), "ownership wait duration");
	Merge(result, ValidateTelemetryDuration(record.executionDuration), "execution duration");
	Merge(result, ValidateTelemetryDuration(record.flushDuration), "flush duration");
	Merge(result, ValidateTelemetryDuration(record.publicationDuration), "publication duration");
	return Finish(std::move(result));
}

TelemetryValidationResult ValidateWorkerTelemetryRecord(const WorkerTelemetryRecord& record)
{
	TelemetryValidationResult result;
	Merge(result, ValidateTelemetryDuration(record.runDuration), "run duration");
	Merge(result, ValidateTelemetryDuration(record.waitDuration), "wait duration");
	Merge(result, ValidateTelemetryDuration(record.idleDuration), "idle duration");
	return Finish(std::move(result));
}

TelemetryValidationResult ValidateWorkloadExecutionRecord(const WorkloadExecutionRecord& record)
{
	TelemetryValidationResult result;
	if (record.frameId.empty()) AddDiagnostic(result, "A workload execution record requires a frame id.");
	if (record.descriptorId.empty()) AddDiagnostic(result, "A workload execution record requires a stable descriptor id.");
	if (record.descriptorGeneration == 0u) AddDiagnostic(result, "A workload execution record requires a nonzero descriptor generation.");
	if (record.ownerSystem.empty()) AddDiagnostic(result, "A workload execution record requires an owning system.");
	if (record.workloadName.empty()) AddDiagnostic(result, "A workload execution record requires a workload name.");
	if (!IsKnownWorkloadExecutionPath(record.selectedPath)) AddDiagnostic(result, "A workload execution record has an unrecognized execution path.");
	if (!IsKnownWorkloadExecutionStatus(record.status)) AddDiagnostic(result, "A workload execution record has an unrecognized status.");
	if (record.itemEnd < record.itemBegin) AddDiagnostic(result, "A workload partition end must not precede its beginning.");
	if (record.partitionCount == 0u && (record.partitionIndex != 0u || record.itemBegin != 0u || record.itemEnd != 0u))
	{
		AddDiagnostic(result, "An unpartitioned workload cannot report partition offsets.");
	}
	if (record.partitionCount > 0u && record.partitionIndex >= record.partitionCount)
	{
		AddDiagnostic(result, "A workload partition index must be within the partition count.");
	}
	if ((record.status == WorkloadExecutionStatus::Inactive || record.status == WorkloadExecutionStatus::NotReady)
		&& (record.dispatchCount != 0u || record.selectedPath != WorkloadExecutionPath::None))
	{
		AddDiagnostic(result, "Inactive or not-ready workload decisions must not report a dispatch path.");
	}
	Merge(result, ValidateTelemetryDuration(record.queueDuration), "queue duration");
	Merge(result, ValidateTelemetryDuration(record.runDuration), "run duration");
	Merge(result, ValidateTelemetryDuration(record.coordinatorWaitDuration), "coordinator wait duration");
	Merge(result, ValidateTelemetryDuration(record.ownershipWaitDuration), "ownership wait duration");
	Merge(result, ValidateTelemetryDuration(record.flushDuration), "flush duration");
	Merge(result, ValidateTelemetryDuration(record.publicationDuration), "publication duration");
	return Finish(std::move(result));
}

TelemetryValidationResult ValidatePresentationWaitRecord(const PresentationWaitRecord& record)
{
	TelemetryValidationResult result;
	if (!IsKnownPresentationWaitKind(record.kind)) AddDiagnostic(result, "A presentation wait record has an unrecognized wait kind.");
	Merge(result, ValidateTelemetryDuration(record.duration), "presentation wait duration");
	return Finish(std::move(result));
}

TelemetryValidationResult ValidateFrameResponsivenessRecord(const FrameResponsivenessRecord& record)
{
	TelemetryValidationResult result;
	Merge(result, ValidateTelemetryDuration(record.inputToPresentationDuration), "input-to-presentation duration");
	Merge(result, ValidateTelemetryDuration(record.frameDuration), "frame duration");
	return Finish(std::move(result));
}

TelemetryValidationResult ValidateProfilerArtifactRecord(const ProfilerArtifactRecord& record)
{
	TelemetryValidationResult result;
	if (record.artifactId.empty()) AddDiagnostic(result, "A profiler artifact record requires an artifact id.");
	if (record.storedByteCount == 0u) AddDiagnostic(result, "A profiler artifact record requires a nonzero stored byte count.");
	if (record.recoveredIncompleteTail && record.complete)
	{
		AddDiagnostic(result, "A complete profiler artifact cannot report incomplete-tail recovery.");
	}
	return Finish(std::move(result));
}

TelemetryValidationResult ValidatePhysicsWorkVolumeRecord(const PhysicsWorkVolumeRecord& record)
{
	TelemetryValidationResult result;
	if (record.activeBodyCount > record.totalBodyCount)
	{
		AddDiagnostic(result, "Active physics bodies cannot exceed total bodies.");
	}
	if (record.examinedBodyCount > record.totalBodyCount)
	{
		AddDiagnostic(result, "Examined physics bodies cannot exceed total bodies.");
	}
	if (record.extractedBodyCount > record.examinedBodyCount
		|| record.convertedBodyCount > record.extractedBodyCount
		|| record.changedBodyCount > record.convertedBodyCount
		|| record.publishedBodyCount > record.changedBodyCount)
	{
		AddDiagnostic(result, "Physics work-volume counts must descend from examined through published bodies.");
	}
	return Finish(std::move(result));
}
}
