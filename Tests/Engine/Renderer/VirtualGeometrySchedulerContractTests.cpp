// VirtualGeometrySchedulerContractTests.cpp - executable scheduler ownership contracts.

#include "TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Renderer/RenderSystem.h"
#include "Resource/VirtualGeometryStreamingSystem.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		const TerraEngine::Scheduling::FrameWorkloadDeclaration *FindWorkload(
			const std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> &workloads, std::string_view workloadName)
		{
			const auto found =
				std::find_if(workloads.begin(), workloads.end(), [workloadName](const auto &workload) { return workload.workloadName == workloadName; });
			return found == workloads.end() ? nullptr : &*found;
		}

		bool Contains(const std::vector<std::string> &values, std::string_view expected)
		{
			return std::find(values.begin(), values.end(), expected) != values.end();
		}
	} // namespace

	void RegisterVirtualGeometrySchedulerContractTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Scheduler.RendererScratchHasOneProtectedWriter", [](TestContext& context) {
			using namespace TerraEngine::Scheduling;
			const auto workloads = RenderSystem::Instance().DeclareFrameWorkloads(0.0F);
			const std::vector<std::string> names{"RenderPublicationCapture", "GeometryRoutePrepare", "VirtualGeometryPrepare", "BackendCommit", "RenderPublication"};
			context.Expect(workloads.size() == names.size(), "Allocator ownership must preserve the existing five-workload render chain.");
			for (std::size_t index = 0; index < names.size(); ++index) {
				const auto* workload = FindWorkload(workloads, names[index]);
				context.Expect(workload != nullptr, "Every existing renderer workload must remain declared.");
				context.Expect(workload->phase == "Render", "Scratch storage must not introduce another scheduler phase.");
				if (index == 0) {
					context.Expect(Contains(workload->writes, kResourceRendererFrameScratch)
						&& workload->mainThreadRequired && !workload->workerEligible,
						"Only protected capture may open/reset and populate scratch after previous borrowers retire.");
				} else {
					context.Expect(Contains(workload->reads, kResourceRendererFrameScratch)
						&& !Contains(workload->writes, kResourceRendererFrameScratch),
						"Later stages borrow completed collections or copy terminal facts without allocator writes.");
					const auto* previous = FindWorkload(workloads, names[index - 1]);
					context.Expect(previous != nullptr && Contains(workload->explicitDependencies, previous->stableDescriptorId),
						"Existing explicit dependencies must keep every borrower inside its captured episode.");
				}
			}
			const auto* route = FindWorkload(workloads, names[1]);
			const auto* prepare = FindWorkload(workloads, names[2]);
			const auto* publication = FindWorkload(workloads, names[4]);
			context.Expect(route->workerEligible && prepare->workerEligible,
				"Read-only scratch borrowing must preserve both real worker phases.");
			context.Expect(publication->mainThreadRequired && Contains(publication->writes, "RendererDiagnostics"),
				"Copied scratch facts are published through the existing protected diagnostics owner.");
			const auto phases = DefaultSchedulerPhases();
			const auto renderPhase = std::find_if(phases.begin(), phases.end(), [](const auto& phase) { return phase.name == "Render"; });
			context.Expect(renderPhase != phases.end() && !renderPhase->flushBefore && !renderPhase->flushAfter,
				"Changing camera/light storage must not add a scene mutation flush around Render.");
			const auto resources = DefaultSchedulerResources();
			context.Expect(std::any_of(resources.begin(), resources.end(), [](const auto& resource) {
				return resource.name == kResourceRendererFrameScratch && !resource.description.empty() && !resource.futureComponentScope.empty();
			}), "Scratch must be a known scheduler resource with visible protected/worker ownership intent.");
		});

		registry.Add("VirtualGeometry.Scheduler.RendererStagesDeclareExecutableOwnership",
			[](TestContext &context)
			{
				const auto workloads = RenderSystem::Instance().DeclareFrameWorkloads(0.0F);
				const auto *snapshot = FindWorkload(workloads, "RenderPublicationCapture");
				const auto *prepare = FindWorkload(workloads, "VirtualGeometryPrepare");
				const auto *commit = FindWorkload(workloads, "BackendCommit");
				const auto *publication = FindWorkload(workloads, "RenderPublication");

				context.Expect(snapshot != nullptr, "RenderSystem must protect immutable publication capture.");
				context.Expect(prepare != nullptr, "RenderSystem must expose worker-safe virtual geometry preparation.");
				context.Expect(commit != nullptr, "RenderSystem must protect device command ownership in BackendCommit.");
				context.Expect(publication != nullptr, "RenderSystem must expose a protected render publication boundary.");
				if (snapshot == nullptr || prepare == nullptr || commit == nullptr || publication == nullptr)
					return;

				context.Expect(snapshot->mainThreadRequired && !snapshot->workerEligible,
					"Render publication capture must stay protected because it seals scene and renderer generations.");
				context.Expect(Contains(snapshot->writes,
					TerraEngine::Scheduling::kResourceRenderCaptureInputs)
					&& !Contains(snapshot->writes,
						TerraEngine::Scheduling::kResourcePersistentRenderSceneState),
					"Protected capture must publish immutable inputs without performing persistent topology lowering.");
				context.Expect(prepare->workerEligible && !prepare->mainThreadRequired,
					"VirtualGeometryPrepare must be an actual worker-safe workload rather than metadata-only "
					"worker intent.");
				context.Expect(static_cast<bool>(prepare->work), "VirtualGeometryPrepare must supply executable work for the scheduler.");
				context.Expect(!Contains(prepare->reads, "ObjectManager") && !Contains(prepare->reads, "Renderer"),
					"Worker preparation must not read live ObjectManager or renderer ownership.");
				context.Expect(Contains(prepare->writes, TerraEngine::Scheduling::kResourceVirtualGeometryPageNeedPackets),
					"Worker preparation must publish typed page-need packets for streaming.");
				context.Expect(commit->mainThreadRequired && !commit->workerEligible, "BackendCommit must remain protected for RHI and queue ownership.");
				context.Expect(
					Contains(commit->explicitDependencies, prepare->stableDescriptorId), "BackendCommit must wait for the immutable virtual-geometry plan.");
				context.Expect(
					Contains(publication->explicitDependencies, commit->stableDescriptorId), "Render publication must wait for protected backend submission.");
				context.Expect(publication->sameFrameVisibilityRequired, "Render publication must explicitly expose the frame visibility boundary.");
			});

		registry.Add("VirtualGeometry.Scheduler.StreamingPublishesBeforeNextRenderSnapshot",
			[](TestContext &context)
			{
				const auto declaration = TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance().SchedulerDeclaration();
				const auto workloads = TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance().DeclareFrameWorkloads(0.0F);
				const auto *prepare = FindWorkload(workloads, "StreamingPrepare");
				const auto *publish = FindWorkload(workloads, "PagePublish");

				context.Expect(declaration.phase == "Render", "Streaming must share the render phase so publication can precede render snapshot capture.");
				context.Expect(Contains(declaration.before, "RenderSystem"), "StreamingSystem must register before RenderSystem's snapshot work.");
				context.Expect(
					prepare != nullptr && publish != nullptr, "Streaming must expose both worker preparation and protected atomic publication workloads.");
				if (prepare == nullptr || publish == nullptr)
					return;

				context.Expect(prepare->workerEligible && !prepare->mainThreadRequired && static_cast<bool>(prepare->work),
					"StreamingPrepare must execute only private candidate planning on a worker.");
				context.Expect(Contains(prepare->reads, TerraEngine::Scheduling::kResourceVirtualGeometryPageRequests),
					"StreamingPrepare must consume render page requests.");
				context.Expect(Contains(prepare->writes, TerraEngine::Scheduling::kResourceVirtualGeometryPageNeedPackets),
					"StreamingPrepare must publish typed private page candidates.");
				context.Expect(
					publish->mainThreadRequired && !publish->workerEligible, "PagePublish must remain protected to preserve atomic residency visibility.");
				context.Expect(Contains(publish->explicitDependencies, prepare->stableDescriptorId),
					"PagePublish must wait for the complete private streaming candidate set.");
				context.Expect(Contains(publish->writes, TerraEngine::Scheduling::kResourceVirtualGeometryPagePublications),
					"PagePublish must own the visible page-publication resource.");
				context.Expect(
					publish->sameFrameVisibilityRequired, "PagePublish must state that accepted pages become visible before the next render snapshot.");
			});

		registry.Add("VirtualGeometry.Scheduler.StreamingPublicationOrdersBeforeRenderSnapshot",
			[](TestContext &context)
			{
				auto &streaming = TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance();
				auto &renderer = RenderSystem::Instance();
				TerraEngine::Scheduling::SystemScheduler scheduler;
				scheduler.RegisterSystem(streaming, streaming.SchedulerDeclaration());
				scheduler.RegisterSystem(renderer, renderer.SchedulerDeclaration());
				const auto &summary = scheduler.BuildSchedule();

				context.Expect(summary.valid, "Streaming and renderer declarations must have known typed resources and an unambiguous order.");
				context.Expect(summary.systems.size() == 2U && summary.systems.front() == "VirtualGeometryStreamingSystem",
					"The streaming publisher must be ordered before RenderSystem in the render phase.");
				const auto workloads = scheduler.BuildFrameWorkloadDeclarations(0.0F);
				const auto *snapshot = FindWorkload(workloads, "RenderPublicationCapture");
				context.Expect(snapshot != nullptr && Contains(snapshot->explicitDependencies, "VirtualGeometryStreamingSystem/Render/PagePublish"),
					"Render publication capture must wait for the protected page publication descriptor.");
			});
		registry.Add("VirtualGeometry.Scheduler.RoutePreparationAndSuppressionExecute",
			[](TestContext &context)
			{
				const auto workloads = RenderSystem::Instance().DeclareFrameWorkloads(0.0F);
				const auto *routePrepare = FindWorkload(workloads, "GeometryRoutePrepare");
				const auto *vgPrepare = FindWorkload(workloads, "VirtualGeometryPrepare");
				context.Expect(routePrepare != nullptr, "Route selection must declare a worker-safe GeometryRoutePrepare workload.");
				context.Expect(vgPrepare != nullptr, "VirtualGeometry traversal must remain a separately declared workload.");
				if (routePrepare == nullptr || vgPrepare == nullptr)
					return;
				context.Expect(routePrepare->workerEligible && !routePrepare->mainThreadRequired && static_cast<bool>(routePrepare->work),
					"GeometryRoutePrepare must execute immutable route scoring on workers.");
				context.Expect(Contains(routePrepare->writes, "GeometryRouteDecisions"),
					"Route preparation must publish typed route decisions.");
				context.Expect(Contains(routePrepare->reads,
					TerraEngine::Scheduling::kResourceRenderCaptureInputs)
					&& Contains(routePrepare->writes,
						TerraEngine::Scheduling::kResourcePersistentRenderSceneState),
					"The route worker must lower protected capture inputs into persistent renderer state.");
				context.Expect(Contains(vgPrepare->reads, "GeometryRouteDecisions"),
					"VG traversal must consume route decisions rather than re-evaluate live policy.");
				context.Expect(Contains(routePrepare->writes, "DirectMeshletBatches"),
					"Direct routes must publish direct batches without allocating VG executor work.");
			});

		registry.Add("VirtualGeometry.Scheduler.ProtectedCommitPublishesExecutionEvidence",
			[](TestContext &context)
			{
				const auto workloads = RenderSystem::Instance().DeclareFrameWorkloads(0.0F);
				const auto *commit = FindWorkload(workloads, "BackendCommit");
				const auto *publication = FindWorkload(workloads, "RenderPublication");
				context.Expect(commit != nullptr && publication != nullptr,
					"Protected backend commit and publication workloads must remain explicit.");
				if (commit == nullptr || publication == nullptr)
					return;
				context.Expect(commit->mainThreadRequired && !commit->workerEligible && static_cast<bool>(commit->work),
					"BackendCommit must be the executable protected device ownership boundary.");
				context.Expect(publication->mainThreadRequired && publication->sameFrameVisibilityRequired && static_cast<bool>(publication->work),
					"RenderPublication must expose an executable protected visibility boundary.");
			});
	}
} // namespace TerraEngine::Tests
