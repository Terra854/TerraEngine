// PhysicsDiagnosticsWindowTests.cpp - unregistered immutable diagnostics contracts.

#include "TestHarness.h"

#include "Physics/PhysicsDiagnostics.h"
#include "Physics/PhysicsSystem.h"
#include "Profiling/ProfilerCapture.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewOverlayTypes.h"

#include <filesystem>
#include <memory>
#include <string>

namespace TerraEngine::EditorTests
{
	void RegisterPhysicsDiagnosticsWindowTests(TestRegistry& registry)
	{
		registry.Add("Physics.Diagnostics.PublishedSnapshotCarriesSchedulerAndCounters", [](TestContext& context) {
			auto snapshot = std::make_shared<Physics::PhysicsDiagnosticsSnapshot>();
			snapshot->world = {{1U}, 1U};
			snapshot->tick = 4U;
			snapshot->counters.bodyCount = 2U;
			snapshot->counters.contactCount = 3U;
			snapshot->cache.hitCount = 4U;
			snapshot->memory.highWaterProviderAllocations = 5U;
			snapshot->scheduler = std::make_shared<const std::vector<Physics::PhysicsSchedulerDeclaration>>(std::vector<Physics::PhysicsSchedulerDeclaration>{{"PhysicsStep", "FixedUpdate", "PhysicsCommands", "PhysicsSnapshots", "AfterPhysicsFlush", true}});
			Physics::PublishPhysicsDiagnosticsSnapshot(snapshot);
			const auto read = Physics::AcquirePhysicsDiagnosticsSnapshot();
			context.Expect(read && read->counters.bodyCount == 2U, "Diagnostics consumers must receive the published immutable counter snapshot.");
			context.Expect(read && read->scheduler && read->scheduler->front().workerSafe, "Scheduler declarations must remain observable without a live physics system call.");
			context.Expect(read && read->counters.contactCount == 3U && read->cache.hitCount == 4U
				&& read->memory.highWaterProviderAllocations == 5U, "Contact, cache, memory, and allocator high-water diagnostics must cross the immutable publication boundary.");
		});

		registry.Add("Physics.Diagnostics.SystemDeclarationNamesProfilerScopeAndBoundaries", [](TestContext& context) {
			const auto declaration = Physics::PhysicsSystem::Instance().SchedulerDeclaration();
			context.ExpectEqual(std::string("Physics"), declaration.profilingLabel, "The physics system needs a named profiler scope even when its diagnostics window is closed.");
			context.Expect(!declaration.safeSubwork.empty() && !declaration.protectedCommit.empty(), "Profiler-visible physics work must declare worker-safe and protected boundary responsibilities.");
		});

		registry.Add("Physics.Diagnostics.SceneViewDeclarationExplainsProtectedMutationBoundary", [](TestContext& context) {
			const Editor::SceneViewOverlay::SceneViewSchedulerDeclaration declaration = Editor::SceneViewOverlay::BuildSceneViewOverlaySchedulerDeclaration();
			context.Expect(!declaration.workerSafePreparation.empty(), "Scene View must declare worker-safe immutable preparation.");
			context.Expect(declaration.protectedBoundary.find("transaction") != std::string::npos, "Shape commits must declare their one protected authoring transaction.");
		});

		registry.Add("Physics.Diagnostics.SnapshotReadsDoNotCreateDebugWork", [](TestContext& context) {
			Physics::PublishedPhysicsDebugPackets().Clear();
			(void)Physics::AcquirePhysicsDiagnosticsSnapshot();
			context.ExpectEqual(static_cast<std::size_t>(0U), Physics::PublishedPhysicsDebugPackets().Size(), "Reading diagnostics must not produce packet history work when no producer published a packet.");
		});

		registry.Add("Physics.Diagnostics.ProfilerFieldsSerializeWithoutDiagnosticsWindow", [](TestContext& context) {
			Profiling::ProfilerCaptureDocument document;
			Profiling::ProfilerFrameRecord frame;
			frame.frameIndex = 1U;
			frame.physics = Profiling::ProfilerPhysicsFrameCounters{};
			frame.physics->tick = 6U;
			frame.physics->bodyCount = 12U;
			frame.physics->shapeCacheBytes = 1024U;
			frame.physics->shapeCacheHits = 8U;
			frame.physics->snapshotHighWaterBytes = 2048U;
			frame.physics->providerAllocationHighWater = 32U;
			document.frames.push_back(frame);
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEnginePhysicsProfilerFields.tprof";
			std::string error;
			context.Expect(Profiling::SaveCaptureDocument(document, path, error), "Physics capture fields must serialize with normal profiler capture output.");
			Profiling::ProfilerCaptureDocument loaded;
			context.Expect(Profiling::LoadCaptureDocument(path, loaded, error), "Physics capture fields must load without a diagnostics window.");
			context.Expect(!loaded.frames.empty() && loaded.frames.front().physics && loaded.frames.front().physics->bodyCount == 12U, "Captured physics counters must survive profiler playback.");
			context.Expect(loaded.frames.front().physics && loaded.frames.front().physics->shapeCacheBytes == 1024U
				&& loaded.frames.front().physics->shapeCacheHits == 8U
				&& loaded.frames.front().physics->snapshotHighWaterBytes == 2048U
				&& loaded.frames.front().physics->providerAllocationHighWater == 32U,
				"Profiler playback must retain cache, snapshot-memory, and allocator high-water counters.");
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		});
	}
}
