// Test-first scheduler contract tests for provider-neutral ground navigation.
//
// The scheduler integration must provide these headers and declarations. Direct inclusion is
// deliberate: silently excluding this file until NavigationSystem exists would
// allow an incomplete runtime registration to look like a passing test suite.

#include "../TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Navigation/NavigationSystem.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Scheduling;

		bool Contains(const std::vector<std::string>& values, std::string_view expected)
		{
			return std::find(values.begin(), values.end(), expected) != values.end();
		}

		const SchedulerPhaseDefinition* FindPhase(const std::vector<SchedulerPhaseDefinition>& phases,
			std::string_view name)
		{
			const auto iterator = std::find_if(phases.begin(), phases.end(), [name](const SchedulerPhaseDefinition& phase) {
				return phase.name == name;
			});
			return iterator == phases.end() ? nullptr : &*iterator;
		}

		const FrameWorkloadDeclaration* FindWorkload(const std::vector<FrameWorkloadDeclaration>& workloads,
			std::string_view stableDescriptorId)
		{
			const auto iterator = std::find_if(workloads.begin(), workloads.end(),
				[stableDescriptorId](const FrameWorkloadDeclaration& workload) {
					return workload.stableDescriptorId == stableDescriptorId;
				});
			return iterator == workloads.end() ? nullptr : &*iterator;
		}
	}

	void RegisterNavigationSchedulerContractTests(TestRegistry& registry)
	{
		registry.Add("Navigation.Scheduler.PhaseIsOrder35BetweenSceneAndScripting", [](TestContext& context) {
			const std::vector<SchedulerPhaseDefinition> phases = DefaultSchedulerPhases();
			const SchedulerPhaseDefinition* scene = FindPhase(phases, "Scene");
			const SchedulerPhaseDefinition* navigation = FindPhase(phases, "Navigation");
			const SchedulerPhaseDefinition* scripting = FindPhase(phases, "Scripting");
			context.Expect(scene != nullptr && navigation != nullptr && scripting != nullptr,
				"The default scheduler must register Scene, Navigation, and Scripting phases.");
			if(scene == nullptr || navigation == nullptr || scripting == nullptr) return;
			context.ExpectEqual(35, navigation->order,
				"Navigation must run at the fixed order 35 after Scene and before gameplay scripting.");
			context.Expect(scene->order < navigation->order && navigation->order < scripting->order,
				"Navigation phase order must preserve its committed-scene-before-script observation boundary.");
		});

		registry.Add("Navigation.Scheduler.DeclaresImmutableInputsAndProtectedPublication", [](TestContext& context) {
			TerraEngine::Navigation::NavigationSystem& system = TerraEngine::Navigation::NavigationSystem::Instance();
			const SystemDeclaration declaration = system.SchedulerDeclaration();
			context.ExpectEqual(std::string("NavigationSystem"), declaration.name,
				"NavigationSystem must declare a stable scheduler identity.");
			context.ExpectEqual(std::string("Navigation"), declaration.phase,
				"NavigationSystem must execute in the Navigation phase.");
			context.Expect(!declaration.mainThread && declaration.phaseKind == WorkerSystemPhaseKind::WorkerPrepare,
				"Navigation must separate bounded worker preparation from protected root publication.");
			context.Expect(declaration.boundaryClassification == HardBoundaryClassification::PartiallyWorkerSafe
				&& declaration.hardBoundary,
				"Navigation publication must be an explicit protected ownership/revision boundary.");
			context.Expect(!declaration.safeSubwork.empty() && !declaration.protectedCommit.empty()
				&& !declaration.protectedReason.empty() && !declaration.profilingLabel.empty(),
				"Navigation declarations must make worker scope, protected reason, and profiling visible to diagnostics.");
			context.Expect(Contains(declaration.reads, "SceneChangePublication")
				&& Contains(declaration.reads, "PublishedSpatialSnapshot")
				&& Contains(declaration.reads, "TerrainPublishedCollision")
				&& Contains(declaration.reads, "PublishedWorldRegionResidency")
				&& Contains(declaration.reads, "NavigationCaptureInputs")
				&& Contains(declaration.reads, "NavigationGeometryPublications")
				&& Contains(declaration.reads, "NavigationRequestQueue")
				&& Contains(declaration.reads, "NavigationPreparedCandidates"),
				"Navigation workers must declare scene, spatial, terrain, resource, streaming, profile, request, and prepared inputs.");
			context.Expect(Contains(declaration.writes, "NavigationEligibleSourceIndex")
				&& Contains(declaration.writes, "NavigationWorldRoot")
				&& Contains(declaration.writes, "NavigationRouteValidity")
				&& Contains(declaration.writes, "NavigationResults")
				&& Contains(declaration.writes, "NavigationDiagnostics"),
				"Navigation publication must declare every automatic index/world/route-validity/result/diagnostic output.");
			context.Expect(!Contains(declaration.reads, "Renderer") && !Contains(declaration.writes, "Renderer"),
				"Navigation must not depend on renderer state, so the same declaration is valid in rendered and headless lanes.");

			const std::vector<SchedulerResourceDefinition> resources = DefaultSchedulerResources();
			for(const std::string_view required : { "NavigationCaptureInputs", "NavigationGeometryPublications", "NavigationRequestQueue",
				"NavigationPreparedCandidates", "NavigationEligibleSourceIndex", "NavigationWorldRoot", "NavigationRouteValidity",
				"NavigationResults", "NavigationDiagnostics" }) {
				context.Expect(std::any_of(resources.begin(), resources.end(), [required](const SchedulerResourceDefinition& resource) {
					return resource.name == required;
				}), "Every Navigation handoff must be registered in scheduler diagnostics.");
			}
		});

		registry.Add("Navigation.Scheduler.WorkloadsExposeSafePreparationAndProtectedCommit", [](TestContext& context) {
			TerraEngine::Navigation::NavigationSystem& system = TerraEngine::Navigation::NavigationSystem::Instance();
			system.Initialize();
			system.SetContractTestWorkAvailable(true);
			const std::vector<FrameWorkloadDeclaration> workloads = system.DeclareFrameWorkloads(1.0F / 60.0F);
			system.SetContractTestWorkAvailable(false);
			system.Shutdown();

			const FrameWorkloadDeclaration* prepare = FindWorkload(workloads, "NavigationSystem/Navigation/Prepare");
			const FrameWorkloadDeclaration* publish = FindWorkload(workloads, "NavigationSystem/Navigation/Publish");
			context.Expect(prepare != nullptr && publish != nullptr,
				"Active navigation work must declare both worker preparation and protected publication descriptors.");
			if(prepare == nullptr || publish == nullptr) return;
			context.Expect(prepare->workerEligible && !prepare->mainThreadRequired
				&& prepare->phaseKind == WorkerSystemPhaseKind::WorkerPrepare,
				"Geometry/BVH/build/query preparation must be shared-pool worker-safe work.");
			context.Expect(publish->mainThreadRequired && !publish->workerEligible
				&& publish->phaseKind == WorkerSystemPhaseKind::ProtectedCommit
				&& publish->hardBoundary,
				"Ready groups must revalidate and publish only through the protected Navigation boundary.");
			context.Expect(Contains(publish->explicitDependencies, prepare->stableDescriptorId),
				"Protected navigation publication must depend on the matching prepared candidate descriptor.");
			context.Expect(!prepare->safeSubwork.empty() && !publish->protectedCommit.empty()
				&& !publish->protectedReason.empty() && !publish->profilingLabel.empty(),
				"Workload metadata must explain its worker work, protected commit, reason, and profiling scope.");
		});

		registry.Add("Navigation.Scheduler.OmitsIdleWorkWithoutRemovingItsDeclarations", [](TestContext& context) {
			TerraEngine::Navigation::NavigationSystem& system = TerraEngine::Navigation::NavigationSystem::Instance();
			system.Initialize();
			system.SetContractTestWorkAvailable(false);
			const SystemDeclaration declaration = system.SchedulerDeclaration();
			const std::vector<FrameWorkloadDeclaration> workloads = system.DeclareFrameWorkloads(1.0F / 60.0F);
			system.Shutdown();
			context.Expect(declaration.omitWhenNoFrameWorkloads,
				"Navigation must opt into scheduler-recognized omission when no sources, completions, or consumers are active.");
			context.Expect(workloads.empty(),
				"An idle navigation world must submit zero unnecessary worker jobs and no empty protected publication descriptor.");
		});
	}
}
