#include "TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Core/SystemBase.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		class SpatialFixtureSystem final : public SystemsBase
		{
		public:
			void Initialize() override {}
			void Update(float) override {}
			void Shutdown() override {}
			std::string SystemName() override { return "WorldSpatialSystem"; }
		};

		bool HasResource(const std::vector<TerraEngine::Scheduling::SchedulerResourceDefinition>& resources, std::string_view expected)
		{
			return std::any_of(resources.begin(), resources.end(), [expected](const auto& resource) {
				return resource.name == expected;
			});
		}
	}

	void RegisterWorldSpatialSchedulerTests(TestRegistry& registry)
	{
		registry.Add("Engine.WorldSpatialScheduler.DefaultResourcesExposeCanonicalHandoffs", [](TestContext& context)
		{
			const auto resources = TerraEngine::Scheduling::DefaultSchedulerResources();
			context.Expect(HasResource(resources, TerraEngine::Scheduling::kResourcePublishedSpatialSnapshot),
				"The immutable published spatial snapshot must be a scheduler resource.");
			context.Expect(HasResource(resources, TerraEngine::Scheduling::kResourceSpatialTransitionQueue),
				"The typed spatial transition queue must be a scheduler resource.");
			context.Expect(HasResource(resources, TerraEngine::Scheduling::kResourcePreparedSpatialTransitions),
				"Worker-prepared spatial transitions must be a scheduler resource.");
		});

		registry.Add("Engine.WorldSpatialScheduler.DeclarationSeparatesWorkerPreparationAndProtectedPublication", [](TestContext& context)
		{
			SpatialFixtureSystem system;
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(system);

			TerraEngine::Scheduling::FrameWorkloadDeclaration prepare;
			prepare.workloadName = "WorldSpatialSystem:SpatialTransitionPrepare";
			prepare.ownerSystem = "WorldSpatialSystem";
			prepare.phase = "Scene";
			prepare.stableDescriptorId = "WorldSpatialSystem/Scene/SpatialTransitionPrepare";
			prepare.reads = { TerraEngine::Scheduling::kResourcePublishedSpatialSnapshot,
				TerraEngine::Scheduling::kResourceSpatialTransitionQueue };
			prepare.writes = { TerraEngine::Scheduling::kResourcePreparedSpatialTransitions };
			prepare.workerEligible = true;
			prepare.mainThreadRequired = false;
			prepare.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare;
			prepare.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::WorkerSafe;
			prepare.safeSubwork = "Validate canonical spatial transitions on immutable snapshots.";
			prepare.work = [](TerraEngine::Scheduling::JobGraphContext&) {};

			TerraEngine::Scheduling::FrameWorkloadDeclaration publish;
			publish.workloadName = "WorldSpatialSystem:SpatialTransitionPublish";
			publish.ownerSystem = "WorldSpatialSystem";
			publish.phase = "Scene";
			publish.stableDescriptorId = "WorldSpatialSystem/Scene/SpatialTransitionPublish";
			publish.explicitDependencies = { prepare.stableDescriptorId };
			publish.reads = { TerraEngine::Scheduling::kResourcePreparedSpatialTransitions,
				TerraEngine::Scheduling::kResourcePublishedSpatialSnapshot };
			publish.writes = { TerraEngine::Scheduling::kResourcePublishedSpatialSnapshot };
			publish.workerEligible = false;
			publish.mainThreadRequired = true;
			publish.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
			publish.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::Protected;
			publish.protectedCommit = "Revalidate and atomically publish the accepted spatial group.";
			publish.protectedReason = "Live scene and snapshot publication are protected.";
			publish.work = [](TerraEngine::Scheduling::JobGraphContext&) {};

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ prepare, publish }, "world-spatial-contract");
			context.Expect(plan.valid, "The spatial worker/protected declarations should validate.");
			context.Expect(plan.migrationSummary.workerSafeCount >= 1U,
				"Spatial preparation must remain eligible for worker execution.");
			context.Expect(plan.migrationSummary.protectedCount >= 1U,
				"Spatial publication must remain protected.");
		});

		registry.Add("Engine.WorldSpatialScheduler.DefaultVisibilityIsFrameEnd", [](TestContext& context)
		{
			const auto flushes = TerraEngine::Scheduling::DefaultMutationFlushPoints();
			const auto frameEnd = std::find_if(flushes.begin(), flushes.end(), [](const auto& point) {
				return point.position == TerraEngine::Scheduling::MutationFlushPosition::FrameEnd;
			});
			context.Expect(frameEnd != flushes.end(), "The default mutation policy must include a Frame End boundary.");
			if (frameEnd != flushes.end())
			{
				context.Expect(frameEnd->diagnosePendingAtFrameEnd,
					"Frame End must diagnose pending spatial mutations.");
			}
			const auto beforePhysics = std::find_if(flushes.begin(), flushes.end(), [](const auto& point) {
				return point.position == TerraEngine::Scheduling::MutationFlushPosition::BeforePhase
					&& point.phaseName == "Physics";
			});
			context.Expect(beforePhysics != flushes.end(),
				"The default graph must expose the protected Before Physics edge for urgent spatial candidates.");
			if (beforePhysics != flushes.end())
			{
				context.Expect(beforePhysics->requiredBeforeSystems.empty(),
					"The spatial-only edge must not flush the generic scene queue for a consumer system.");
			}
		});

		registry.Add("Engine.WorldSpatialScheduler.BeforePhysicsBoundaryIsExplicitAndNarrow", [](TestContext& context)
		{
			SpatialFixtureSystem system;
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.SetMutationFlushPoints({
				{ "Before Physics Spatial Migration", TerraEngine::Scheduling::MutationFlushPosition::BeforePhase,
					"Physics", { "WorldSpatialSystem" }, true, false },
				{ "Frame End", TerraEngine::Scheduling::MutationFlushPosition::FrameEnd, "", {}, true, true }
			});
			scheduler.RegisterSystem(system);
			const auto& summary = scheduler.BuildSchedule();
			context.Expect(summary.valid, "An explicitly declared migration boundary should validate.");
			const auto& flushes = scheduler.MutationFlushPoints();
			const auto found = std::find_if(flushes.begin(), flushes.end(), [](const auto& point) {
				return point.position == TerraEngine::Scheduling::MutationFlushPosition::BeforePhase;
			});
			context.Expect(found != flushes.end(), "The explicit Before Physics migration boundary must be observable.");
			if (found != flushes.end())
			{
				context.ExpectEqual(std::string("Physics"), found->phaseName,
					"The early boundary must be scoped to Physics.");
				context.ExpectEqual(static_cast<std::size_t>(1U), found->requiredBeforeSystems.size(),
					"Only the spatial migration owner may request the early boundary.");
			}
		});
	}
}
