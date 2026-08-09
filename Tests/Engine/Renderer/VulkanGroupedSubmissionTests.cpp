#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanCommandRecording.h"

#include <numeric>

namespace TerraEngine::Tests
{
	void RegisterVulkanGroupedSubmissionTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VulkanGroupedSubmission.ConservesGroupedAndSingletonRanges",
			[](TestContext& context) {
				Renderer::RenderFramePublication publication{};
				publication.views.push_back({});
				publication.views[0].generation = 1u;
				// Two compatible items form one grouped range while the last item
				// remains a singleton. Both forms address the same compact selection.
				for(std::uint32_t ordinal = 0u; ordinal < 3u; ++ordinal)
				{
					publication.views[0].visibleOrdinals.push_back({ ordinal, 5u });
					publication.views[0].selectedLods.push_back(ordinal);
					publication.views[0].routeSelectors.push_back(0u);
					publication.views[0].packetTemplateIdentities.push_back(
						ordinal < 2u ? 7u : 8u);
					publication.views[0].tintSelectors.push_back(0u);
					publication.views[0].editorTints.push_back(glm::vec4(0.0f));
				}
				publication.submissionRanges.push_back({
					Renderer::RenderSubmissionRoute::Direct, 0u, 0u, 2u, 7u, false });
				publication.submissionRanges.push_back({
					Renderer::RenderSubmissionRoute::Direct, 0u, 2u, 1u, 8u, true });
				const auto plan = Renderer::Vulkan::BuildVulkanPublishedRangePlan(
					publication, 0u);
				const std::uint32_t selectedCount = std::accumulate(
					plan.ranges.begin(), plan.ranges.end(), 0u,
					[](std::uint32_t total, const Renderer::RenderSubmissionRange& range) {
						return total + range.selectionCount;
					});
				context.Expect(plan.valid && plan.ranges.size() == 2u
					&& selectedCount == publication.views[0].visibleOrdinals.size(),
					"Grouped and singleton ranges must conserve every selected item exactly once.");
				context.Expect(plan.ranges[0].firstSelection == 0u
					&& plan.ranges[1].firstSelection == 2u
					&& publication.views[0].visibleOrdinals[2].ordinal == 2u
					&& publication.views[0].visibleOrdinals[2].lifetimeGeneration == 5u,
					"Backend planning must preserve stable ordinal and lifetime identities.");
			});

		registry.Add("Renderer.VulkanGroupedSubmission.InvalidTailPublishesNoPartialPlan",
			[](TestContext& context) {
				Renderer::RenderFramePublication publication{};
				publication.views.push_back({});
				publication.views[0].generation = 1u;
				publication.views[0].visibleOrdinals.resize(2u, { 0u, 1u });
				publication.submissionRanges.push_back({
					Renderer::RenderSubmissionRoute::Direct, 0u, 0u, 1u, 1u, true });
				// A malformed later range must discard the earlier valid range. Returning
				// a prefix here would submit a visually incomplete frame.
				publication.submissionRanges.push_back({
					Renderer::RenderSubmissionRoute::Direct, 0u, 1u, 2u, 2u, false });
				const auto plan = Renderer::Vulkan::BuildVulkanPublishedRangePlan(
					publication, 0u);
				context.Expect(!plan.valid && plan.ranges.empty(),
					"One invalid range must reject the complete per-view backend plan.");
			});
	}
}
