#include "TestHarness.h"

#include "Resource/ResourceResidency.h"
#include "Resource/ResourceStreaming.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		TerraEngine::UUID MakePageAssetId(std::uint8_t byte)
		{
			TerraEngine::UUID id = TerraEngine::EMPTY_UUID;
			id[15] = byte;
			return id;
		}

		Resource::VirtualGeometryResidentPage MakeResidentPage(std::uint8_t assetByte, std::uint32_t pageId, std::uint64_t bytes, std::int32_t priority, std::uint64_t lastUsedFrame, bool fallback)
		{
			Resource::VirtualGeometryResidentPage page;
			page.assetId = MakePageAssetId(assetByte);
			page.pageId = pageId;
			page.fallbackPageId = 0u;
			page.state = Resource::VirtualGeometryPageResidencyState::Resident;
			page.byteSize = bytes;
			page.priority = priority;
			page.lastUsedFrame = lastUsedFrame;
			page.fallbackPage = fallback;
			return page;
		}
	}

	void RegisterVirtualGeometryResidencyTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Resource.Residency.PageTransitionsFollowRequestLoadResidentFailureFlow", [](TestContext& context) {
			Resource::VirtualGeometryResidentPage page;
			page.assetId = MakePageAssetId(1u);
			page.pageId = 7u;

			auto result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::Request, 10u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Requested, "A missing page should move to Requested when visibility asks for it.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::BeginLoad, 11u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Loading, "Requested page should enter Loading before publication.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::MarkResident, 12u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Resident, "Loaded page should become Resident only at the publication step.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::BeginEvict, 13u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Evicting, "Resident non-fallback page should enter Evicting under budget pressure.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::MarkEvicted, 14u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Evicted, "Evicting page should become Evicted after the protected release boundary.");

			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::Request, 15u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Requested, "Evicted pages should be requestable again.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::MarkFailed, 16u, "io-error");
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Failed, "Failed loads should publish Failed state atomically.");
			context.ExpectEqual(std::string("io-error"), page.failureReason, "Failure reason should be retained for diagnostics.");
		});

		registry.Add("VirtualGeometry.Resource.Residency.BudgetEvictsHighDetailBeforeFallback", [](TestContext& context) {
			std::vector<Resource::VirtualGeometryResidentPage> pages = {
				MakeResidentPage(1u, 0u, 256u, 1000, 100u, true),
				MakeResidentPage(1u, 1u, 512u, 50, 20u, false),
				MakeResidentPage(1u, 2u, 512u, 200, 10u, false)
			};
			Resource::VirtualGeometryPageBudget budget;
			budget.memoryBudgetBytes = 512u;

			const std::vector<Resource::VirtualGeometryPageEvictionCandidate> candidates =
				Resource::BuildVirtualGeometryPageEvictionCandidates(pages, budget);

			context.ExpectEqual(static_cast<std::size_t>(2), candidates.size(), "Budget pressure should choose enough high-detail pages to reach the target.");
			context.ExpectEqual(1u, candidates.front().pageId, "Lower-priority high-detail page should be evicted first.");
			for (const Resource::VirtualGeometryPageEvictionCandidate& candidate : candidates)
			{
				context.Expect(candidate.pageId != 0u, "Fallback page must be preserved under budget pressure.");
			}
		});

		registry.Add("VirtualGeometry.Resource.Streaming.BuildsDesiredAndFallbackPageNeeds", [](TestContext& context) {
			Resource::VirtualGeometryPageNeedInput input;
			input.assetId = MakePageAssetId(9u);
			input.objectId = MakePageAssetId(8u);
			input.viewId = "game";
			input.desiredLodIndex = 2u;
			input.pages = {
				{ 0u, 0u, 1u, 0u, 256u, 1000, false, false },
				{ 1u, 1u, 1u, 0u, 512u, 500, true, false },
				{ 2u, 2u, 1u, 0u, 512u, 100, false, false }
			};
			Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();

			const std::vector<Resource::StreamingNeed> needs =
				Resource::BuildVirtualGeometryPageStreamingNeeds({ input }, policy, 5.0);

			context.ExpectEqual(static_cast<std::size_t>(2), needs.size(), "Visible virtual geometry should request both desired and fallback pages when either is missing.");
			context.Expect(needs.front().priority >= needs.back().priority, "Merged streaming needs should remain priority ordered.");
			bool sawDesired = false;
			bool sawFallback = false;
			for (const Resource::StreamingNeed& need : needs)
			{
				sawDesired = sawDesired || need.needId == "virtual-geometry-page:2";
				sawFallback = sawFallback || need.needId == "virtual-geometry-page:0";
			}
			context.Expect(sawDesired, "Desired high-detail page should be requested.");
			context.Expect(sawFallback, "Fallback page should be requested when it is not resident.");
		});

		registry.Add("VirtualGeometry.Resource.Residency.BudgetPressureReportsTopContributors", [](TestContext& context) {
			const std::vector<Resource::VirtualGeometryResidentPage> pages = {
				MakeResidentPage(1u, 0u, 128u, 1000, 100u, true),
				MakeResidentPage(1u, 1u, 1024u, 10, 20u, false),
				MakeResidentPage(2u, 0u, 256u, 1000, 100u, true),
				MakeResidentPage(2u, 1u, 256u, 100, 30u, false)
			};
			Resource::VirtualGeometryPageBudget budget;
			budget.memoryBudgetBytes = 512u;

			const Resource::VirtualGeometryPageBudgetPressureSummary summary =
				Resource::BuildVirtualGeometryPageBudgetPressureSummary(pages, budget);

			context.Expect(summary.overBudgetBytes > 0u, "Summary should report geometry page budget pressure.");
			context.ExpectEqual(4u, summary.residentPages, "Summary should count resident pages.");
			context.Expect(!summary.contributors.empty(), "Summary should publish per-asset budget contributors.");
			context.ExpectEqual(TerraEngine::ToHexString(MakePageAssetId(1u)), TerraEngine::ToHexString(summary.contributors.front().assetId), "Largest reclaimable asset should be the top budget contributor.");
			context.Expect(summary.contributors.front().reclaimableBytes > summary.contributors.back().reclaimableBytes, "Contributors should be ordered by reclaimable pressure.");
		});

		registry.Add("VirtualGeometry.Resource.Residency.StreamingPressureChurnPreservesFallbackAndStableRequests", [](TestContext& context) {
			std::vector<Resource::VirtualGeometryResidentPage> residentPages;
			std::vector<Resource::VirtualGeometryPageNeedInput> needInputs;
			for (std::uint8_t asset = 1u; asset <= 12u; ++asset)
			{
				needInputs.push_back({});
				Resource::VirtualGeometryPageNeedInput& input = needInputs.back();
				input.assetId = MakePageAssetId(asset);
				input.objectId = MakePageAssetId(static_cast<std::uint8_t>(asset + 64u));
				input.viewId = "stress-game-view";
				input.desiredLodIndex = 2u;
				input.retentionSeconds = 1.0;

				for (std::uint32_t pageId = 0u; pageId < 3u; ++pageId)
				{
					const bool fallback = pageId == 0u;
					const bool resident = fallback || (asset % 3u == 0u && pageId == 1u);
					input.pages.push_back({ pageId, pageId, 1u, 0u, fallback ? 128u : 512u, fallback ? 1000 : static_cast<std::int32_t>(200 - pageId * 50), resident, false });
					if (resident)
					{
						residentPages.push_back(MakeResidentPage(asset, pageId, fallback ? 128u : 512u, fallback ? 1000 : static_cast<std::int32_t>(200 - pageId * 50), pageId + asset, fallback));
					}
				}
			}

			Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();
			const std::vector<Resource::StreamingNeed> firstNeeds =
				Resource::BuildVirtualGeometryPageStreamingNeeds(needInputs, policy, 20.0);
			const std::vector<Resource::StreamingNeed> repeatedNeeds =
				Resource::BuildVirtualGeometryPageStreamingNeeds(needInputs, policy, 20.25);

			context.ExpectEqual(firstNeeds.size(), repeatedNeeds.size(), "Repeated page-need planning should be deterministic under identical pressure inputs.");
			context.Expect(!firstNeeds.empty(), "Streaming pressure fixture should request missing high-detail pages.");
			for (const Resource::StreamingNeed& need : firstNeeds)
			{
				context.Expect(need.needType == Resource::StreamingNeedType::RuntimeExplicit || need.needType == Resource::StreamingNeedType::FallbackDefault, "Virtual geometry churn should emit only explicit desired or fallback page needs.");
			}

			Resource::VirtualGeometryPageBudget budget;
			budget.memoryBudgetBytes = 1024u;
			const std::vector<Resource::VirtualGeometryPageEvictionCandidate> candidates =
				Resource::BuildVirtualGeometryPageEvictionCandidates(residentPages, budget);
			context.Expect(!candidates.empty(), "Budget churn should produce high-detail eviction candidates.");
			for (const Resource::VirtualGeometryPageEvictionCandidate& candidate : candidates)
			{
				context.Expect(candidate.pageId != 0u, "Budget churn must never evict fallback pages.");
			}

			Resource::VirtualGeometryPageBudgetPressureSummary summary =
				Resource::BuildVirtualGeometryPageBudgetPressureSummary(residentPages, budget);
			context.Expect(summary.overBudgetBytes > 0u, "Streaming pressure diagnostics should report over-budget geometry pages.");
			context.Expect(!summary.contributors.empty(), "Streaming pressure diagnostics should identify budget contributors.");
		});
	}
}
