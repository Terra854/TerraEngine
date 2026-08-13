// AudioSchedulerTests.cpp - explicit workload and protected-boundary declarations.

#include "../TestHarness.h"
#include "Audio/AudioSystem.h"

namespace TerraEngine::Tests {
	void RegisterAudioSchedulerTests(TestRegistry& registry) {
		registry.Add("Audio.Scheduler.DeclaresSnapshotPrepareCommit", [](TestContext& context) {
			auto& system = Audio::AudioSystem::Instance();
			const auto declaration = system.SchedulerDeclaration();
			const auto workloads = system.DeclareFrameWorkloads(1.0F / 60.0F);
			context.Expect(declaration.phase == "Audio" && declaration.hardBoundary,
				"AudioSystem must declare its protected phase and boundary.");
			context.Expect(workloads.size() == 3U && workloads[1].workerEligible && !workloads[1].mainThreadRequired,
				"Audio must expose protected snapshot, worker preparation, and protected commit workloads.");
		});
	}
}
