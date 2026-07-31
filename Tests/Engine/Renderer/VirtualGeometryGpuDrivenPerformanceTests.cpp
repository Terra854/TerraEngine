#include "Renderer/VirtualGeometry/Selection/VirtualGeometryGpuPlan.h"
#include "Renderer/VirtualGeometry/Diagnostics/VirtualGeometryReplay.h"
#include "Renderer/Graph/RenderSubmissionPlan.h"
#include "TestHarness.h"

#include <memory>
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryGpuDrivenPerformanceTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.GpuDriven.DeterministicMillionReplay",
			[](TestContext &c)
			{
				TerraEngine::Renderer::VirtualGeometrySelectionResult s{};
				s.selected.reserve(1000000);
				for (std::uint64_t i = 0; i < 1000000; ++i)
					s.selected.push_back(i);
				TerraEngine::Renderer::VirtualGeometryGpuPlanOptions options{};
				options.packetCapacity = 1000000u;
				options.frameId = 41u;
				options.generation = 7u;
				auto a = TerraEngine::Renderer::BuildVirtualGeometryGpuPlan(s, options);
				auto b = TerraEngine::Renderer::BuildVirtualGeometryGpuPlan(s, options);
				c.ExpectEqual(a.deterministicHash, b.deterministicHash, "One million bounded decisions replay deterministically.");
				c.Expect(a.IsComplete() && a.generatedWork.commandCount == 1000000u,
					"One immutable plan carries traversal, compaction, generated-work, and counter expectations without per-packet submission objects.");

				TerraEngine::Renderer::VirtualGeometryReplayReadbackQueue readbacks;
				readbacks.Enqueue(a, 42u, 1000000u);
				c.Expect(readbacks.PublishCompleted(41u).empty() && readbacks.PendingCount() == 1u,
					"A device counter remains pending until its declared delayed completion boundary.");
				const auto published = readbacks.PublishCompleted(42u);
				c.Expect(published.size() == 1u
					&& published[0].counterAvailability
						== TerraEngine::Renderer::VirtualGeometryReplayCounterAvailability::Authoritative
					&& published[0].authoritativeCounter == 1000000u,
					"Only the delayed completed readback becomes an authoritative replay artifact.");

				TerraEngine::Renderer::RenderSubmissionPlan submission;
				submission.frameId = 41u;
				submission.virtualGeometryWork = std::make_shared<const TerraEngine::Renderer::VirtualGeometryGpuPlan>(a);
				c.Expect(TerraEngine::Renderer::ValidateRenderSubmissionPlan(submission, 41u),
					"Submission accepts a same-frame immutable virtual-geometry envelope after its bounded stages validate.");

				TerraEngine::Renderer::VirtualGeometryReplayReadbackQueue backendReadbacks;
				backendReadbacks.EnqueuePending(a);
				c.Expect(backendReadbacks.PublishCompleted(99u).empty(),
					"An unresolved replay counter cannot become authoritative merely because later frames completed.");
				const std::vector<TerraEngine::Renderer::VirtualGeometryAuthoritativeCounterReadback> receipt{
					{ 41u, 7u, 43u, 1000000u, "vulkan.counter.41.7" } };
				const auto backendPublished = backendReadbacks.PublishAuthoritative(receipt);
				c.Expect(backendPublished.size() == 1u
					&& backendPublished.front().counterAvailability
						== TerraEngine::Renderer::VirtualGeometryReplayCounterAvailability::Authoritative,
					"Only a matching backend completion receipt may publish a queued replay counter.");
			});
	}
} // namespace TerraEngine::Tests
