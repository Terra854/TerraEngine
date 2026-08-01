// PerformanceTelemetryTests.cpp — Verifies typed profiler performance records reject ambiguous evidence.

#include "TestHarness.h"

#include "Profiling/PerformanceTelemetry.h"

namespace TerraEngine::Tests
{
namespace
{
using namespace TerraEngine::Profiling;

TelemetryDuration AvailableDuration(std::uint64_t nanoseconds)
{
	return {TelemetryAvailability::Available, nanoseconds, {}};
}

TelemetryDuration UnavailableDuration(const char* reason)
{
	return {TelemetryAvailability::Unavailable, 0u, reason};
}
}

void RegisterPerformanceTelemetryTests(TestRegistry& registry)
{
	registry.Add("Profiling.PerformanceTelemetryValidatesPhysicsAndSceneViewStages", [](TestContext& context) {
		PerformanceStageRecord physics{PerformanceStageDomain::Physics, "Physics.Extract", AvailableDuration(45000u), "physics extraction", 1000u, false};
		PerformanceStageRecord sceneView{PerformanceStageDomain::SceneView, "SceneView.Density", AvailableDuration(12000u), "marker density", 800u, true};
		PerformanceStageRecord editor{PerformanceStageDomain::Editor, "Editor.MainDraw", AvailableDuration(18000u), "EditorSystem", 1u, false};
		context.Expect(ValidatePerformanceStageRecord(physics).valid, "Typed physics stages with measured durations should validate.");
		context.Expect(ValidatePerformanceStageRecord(sceneView).valid, "Typed Scene View stages with measured durations should validate.");
		context.Expect(ValidatePerformanceStageRecord(editor).valid, "Typed editor stages should remain separate valid evidence outside Engine CPU.");
		physics.stageName.clear();
		context.Expect(!ValidatePerformanceStageRecord(physics).valid, "Stages without names should be rejected from profiler evidence.");
	});

	registry.Add("Profiling.PerformanceTelemetryValidatesProtectedBoundaryAndPresentationWaits", [](TestContext& context) {
		ProtectedBoundaryRecord boundary{"After Physics", AvailableDuration(1u), AvailableDuration(2u), AvailableDuration(3u), AvailableDuration(4u), AvailableDuration(5u)};
		context.Expect(ValidateProtectedBoundaryRecord(boundary).valid, "A complete protected-boundary breakdown should validate.");
		boundary.flushDuration = {TelemetryAvailability::Unavailable, 2u, "No flush clock"};
		context.Expect(!ValidateProtectedBoundaryRecord(boundary).valid, "Unavailable boundary timing must not contain a numeric duration.");

		PresentationWaitRecord wait{PresentationWaitKind::GpuCompletion, UnavailableDuration("GPU timestamps unavailable."), "No timestamp query"};
		context.Expect(ValidatePresentationWaitRecord(wait).valid, "Presentation waits may explicitly report unavailable timing.");
	});

	registry.Add("Profiling.PerformanceTelemetryValidatesWorkerAndResponsivenessRecords", [](TestContext& context) {
		WorkerTelemetryRecord worker{2u, 4u, 3u, 64u, AvailableDuration(100u), AvailableDuration(20u), AvailableDuration(10u)};
		context.Expect(ValidateWorkerTelemetryRecord(worker).valid, "Worker utilization history should validate measured run, wait, and idle times.");
		FrameResponsivenessRecord responsiveness{12u, AvailableDuration(500u), AvailableDuration(250u), 17u};
		context.Expect(ValidateFrameResponsivenessRecord(responsiveness).valid, "Frame responsiveness should retain input-to-presentation and frame durations.");
		responsiveness.inputToPresentationDuration = {TelemetryAvailability::Unavailable, 0u, {}};
		context.Expect(!ValidateFrameResponsivenessRecord(responsiveness).valid, "Unavailable responsiveness timing requires a reason rather than a hidden zero.");
	});

	registry.Add("Profiling.PerformanceTelemetryValidatesArtifactAndWorkVolumes", [](TestContext& context) {
		ProfilerArtifactRecord artifact{"canonical-detailed", 1024u, 120u, false, true};
		context.Expect(ValidateProfilerArtifactRecord(artifact).valid, "Incomplete captures may report successful tail recovery.");
		artifact.complete = true;
		context.Expect(!ValidateProfilerArtifactRecord(artifact).valid, "Complete captures must not claim incomplete-tail recovery.");
		artifact.complete = false;
		artifact.storedByteCount = 0u;
		context.Expect(!ValidateProfilerArtifactRecord(artifact).valid, "Capture artifacts need a nonzero stored byte count.");

		PhysicsWorkVolumeRecord volume{7u, 10000u, 1000u, 1000u, 800u, 700u, 600u, 500u};
		context.Expect(ValidatePhysicsWorkVolumeRecord(volume).valid, "Processed-versus-published physics volumes should validate when counts are ordered.");
		volume.publishedBodyCount = 601u;
		volume.changedBodyCount = 600u;
		context.Expect(!ValidatePhysicsWorkVolumeRecord(volume).valid, "Published physics bodies cannot exceed changed bodies.");
	});
}
}
