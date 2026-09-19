// AudioSchedulerTests.cpp - explicit workload and protected-boundary declarations.

#include "../TestHarness.h"
#include "Audio/AudioSystem.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Core/Scheduling/SystemScheduler.h"

#include <algorithm>

namespace TerraEngine::Tests {
	void RegisterAudioSchedulerTests(TestRegistry& registry) {
		registry.Add("Audio.Scheduler.DeclaresSnapshotPrepareCommit", [](TestContext& context) {
			auto& system = Audio::AudioSystem::Instance();
			system.Shutdown();
			// An unavailable provider keeps this scheduler contract deterministic and
			// proves the logical publication path without opening an audio device.
			system.SetConfiguredProvider("test.missing");
			system.Initialize();
			const auto declaration = system.SchedulerDeclaration();
			auto workloads = system.DeclareFrameWorkloads(1.0F / 60.0F);
			context.Expect(declaration.phase == "Audio" && declaration.hardBoundary,
				"AudioSystem must declare its protected phase and boundary.");
			context.Expect(workloads.size() == 3U && workloads[1].workerEligible && !workloads[1].mainThreadRequired,
				"Audio must expose protected snapshot, worker preparation, and protected commit workloads.");
			context.Expect(workloads.size() == 3U
				&& workloads[0].phase == "Audio"
				&& workloads[1].phase == "Audio"
				&& workloads[2].phase == "Audio",
				"Every audio sub-phase must remain inside the scheduler's declared Audio phase.");
			context.Expect(workloads.size() == 3U
				&& workloads[1].explicitDependencies == std::vector<std::string>{workloads[0].stableDescriptorId}
				&& workloads[2].explicitDependencies == std::vector<std::string>{workloads[1].stableDescriptorId},
				"Audio snapshot, preparation, and commit must publish in strict dependency order.");

			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads(
				std::move(workloads), "audio-scheduler-contract");
			context.Expect(plan.valid && plan.completed,
				"The complete audio workload graph must validate and execute without unordered resource conflicts.");
			const auto publication = system.LatestPlaybackSnapshot();
			context.Expect(publication && publication->frame == 1U,
				"Executing the declared graph must invoke snapshot, preparation, and commit rather than completing empty jobs.");

			system.Shutdown();
			system.SetConfiguredProvider("terra.miniaudio");
		});

		registry.Add("Audio.Scheduler.HeadlessDeclarationOmitsOptionalPresentationTargets", [](TestContext& context) {
			auto& system = Audio::AudioSystem::Instance();
			TerraEngine::Scheduling::SystemScheduler scheduler;
			auto declaration = system.SchedulerDeclaration();
			std::erase(declaration.before, "UISystem");
			std::erase(declaration.before, "RenderSystem");
			scheduler.RegisterSystem(system, std::move(declaration));
			const auto& summary = scheduler.BuildSchedule();
			context.Expect(summary.valid,
				"The headless audio declaration must remain valid when optional UI and Render systems are omitted.");
		});
	}
}
