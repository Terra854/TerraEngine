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
		registry.Add("VirtualGeometry.Scheduler.RendererStagesDeclareExecutableOwnership",
			[](TestContext &context)
			{
				const auto workloads = RenderSystem::Instance().DeclareFrameWorkloads(0.0F);
				const auto *snapshot = FindWorkload(workloads, "RenderSnapshot");
				const auto *prepare = FindWorkload(workloads, "VirtualGeometryPrepare");
				const auto *commit = FindWorkload(workloads, "BackendCommit");
				const auto *publication = FindWorkload(workloads, "RenderPublication");

				context.Expect(snapshot != nullptr, "RenderSystem must protect exact live-state capture in RenderSnapshot.");
				context.Expect(prepare != nullptr, "RenderSystem must expose worker-safe virtual geometry preparation.");
				context.Expect(commit != nullptr, "RenderSystem must protect device command ownership in BackendCommit.");
				context.Expect(publication != nullptr, "RenderSystem must expose a protected render publication boundary.");
				if (snapshot == nullptr || prepare == nullptr || commit == nullptr || publication == nullptr)
					return;

				context.Expect(snapshot->mainThreadRequired && !snapshot->workerEligible,
					"RenderSnapshot must stay protected because it captures live scene and renderer generations.");
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
				const auto *snapshot = FindWorkload(workloads, "RenderSnapshot");
				context.Expect(snapshot != nullptr && Contains(snapshot->explicitDependencies, "VirtualGeometryStreamingSystem/Render/PagePublish"),
					"RenderSnapshot must wait for the protected page publication descriptor, not a scene-mutation flush.");
			});
	}
} // namespace TerraEngine::Tests
