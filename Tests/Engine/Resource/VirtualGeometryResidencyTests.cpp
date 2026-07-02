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
			context.ExpectEqual(10ull, page.lastRequestedFrame, "Request transitions should record the request frame.");
			context.ExpectEqual(1u, page.requestCount, "Request transitions should increment retry/accounting count.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::BeginIO, 11u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::PendingIO, "Requested page should enter PendingIO when async disk work starts.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::BeginUpload, 12u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::PendingUpload, "Validated IO output should enter PendingUpload before GPU publication.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::MarkResident, 13u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Resident, "Uploaded page should become Resident only at the protected publication step.");
			context.ExpectEqual(13ull, page.lastResidentFrame, "Resident publication should record the protected publish frame.");
			context.ExpectEqual(13ull, page.lastStateChangeFrame, "Transition helper should record the latest state change frame.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::BeginEvict, 14u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Evicting, "Resident non-fallback page should enter Evicting under budget pressure.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::MarkEvicted, 15u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Evicted, "Evicting page should become Evicted after the protected release boundary.");

			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::Request, 16u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Requested, "Evicted pages should be requestable again.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::MarkFailed, 17u, "io-error");
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Failed, "Failed loads should publish Failed state atomically.");
			context.ExpectEqual(std::string("io-error"), page.failureReason, "Failure reason should be retained for diagnostics.");
		});

		registry.Add("VirtualGeometry.Resource.Residency.PinnedFallbackPublishesWithoutVisibleHoles", [](TestContext& context) {
			Resource::VirtualGeometryResidentPage page;
			page.assetId = MakePageAssetId(3u);
			page.pageId = 0u;
			page.fallbackPageId = 0u;
			page.byteSize = 256u;

			auto result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::PinFallback, 1u, "visible fallback");
			context.Expect(!result.applied && page.pinnedFallback, "Fallback pin should be recorded even before the page is resident.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::Request, 2u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Requested, "Pinned fallback pages should still follow the normal request flow.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::BeginIO, 3u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::PendingIO, "Pinned fallback pages should enter PendingIO.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::BeginUpload, 4u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::PendingUpload, "Pinned fallback pages should enter PendingUpload.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::MarkResident, 5u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Pinned, "Resident fallback pages should publish as Pinned while visible.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::BeginEvict, 6u);
			context.Expect(!result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Pinned, "Pinned fallback pages must not enter eviction under budget pressure.");
			result = Resource::ApplyVirtualGeometryPageTransition(page, Resource::VirtualGeometryPageResidencyEvent::UnpinFallback, 7u);
			context.Expect(result.applied && page.state == Resource::VirtualGeometryPageResidencyState::Resident, "Unpinning should return the fallback page to ordinary resident state.");
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

		registry.Add("VirtualGeometry.Resource.Streaming.PrioritizesVisibilityPredictionImportanceAndPressure", [](TestContext& context) {
			Resource::VirtualGeometryPageNeedInput important;
			important.assetId = MakePageAssetId(20u);
			important.objectId = MakePageAssetId(21u);
			important.viewId = "game";
			important.desiredLodIndex = 2u;
			important.screenImportance = 0.9f;
			important.predictedNeed = 0.8f;
			important.assetPriority = 50;
			important.budgetPressureBytes = 64u * 1024u * 1024u;
			important.pages = {
				{ 0u, 0u, 1u, 0u, 128u, 1000, true, false },
				{ 2u, 2u, 1u, 0u, 512u, 10, false, false }
			};

			Resource::VirtualGeometryPageNeedInput background = important;
			background.assetId = MakePageAssetId(22u);
			background.objectId = MakePageAssetId(23u);
			background.screenImportance = 0.1f;
			background.predictedNeed = 0.0f;
			background.assetPriority = 0;
			background.budgetPressureBytes = 0u;

			const std::vector<Resource::StreamingNeed> needs =
				Resource::BuildVirtualGeometryPageStreamingNeeds({ background, important }, Resource::MakeDefaultStreamingPolicy(), 50.0);

			context.ExpectEqual(static_cast<std::size_t>(2u), needs.size(), "Two missing desired pages should produce two streaming needs.");
			context.ExpectEqual(TerraEngine::ToHexString(important.assetId), TerraEngine::ToHexString(needs.front().assetId), "High-importance predicted page under pressure should sort first.");
			context.Expect(needs.front().priority > needs.back().priority, "Priority should combine page priority, screen importance, prediction, asset bias, and pressure.");
		});

		registry.Add("VirtualGeometry.Resource.Streaming.BuildsSnapshotPageRequestPackets", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewSelectionState state;
			state.viewType = RenderViewType::Game;
			state.outputId = 2u;
			TerraEngine::Renderer::VirtualGeometryPageNeedRecord need;
			need.meshId = MakePageAssetId(30u);
			need.objectId = MakePageAssetId(31u);
			need.pageId = 3u;
			need.clusterId = 9u;
			need.desiredLodIndex = 3u;
			need.screenImportance = 0.75f;
			need.fallbackRequired = true;
			need.packetId = 44u;
			need.batchId = 5u;
			need.packetOffset = 40u;
			need.packetCount = 8u;
			state.pageNeeds.push_back(need);

			Resource::VirtualGeometryPageCatalogEntry catalog;
			catalog.assetId = MakePageAssetId(30u);
			catalog.assetPriority = 25;
			catalog.budgetPressureBytes = 8u * 1024u * 1024u;
			catalog.pages = {
				{ 0u, 0u, 1u, 0u, 128u, 1000, false, false },
				{ 3u, 3u, 1u, 0u, 512u, 100, false, false }
			};

			const std::vector<Resource::VirtualGeometryPageNeedInput> inputs =
				Resource::BuildVirtualGeometryPageNeedInputsFromSelectionStates({ state }, { catalog }, 2.0f);
			const std::vector<Resource::VirtualGeometryPageRequestPacket> packets =
				Resource::BuildVirtualGeometryPageRequestPackets(inputs, Resource::MakeDefaultStreamingPolicy());

			context.ExpectEqual(static_cast<std::size_t>(1u), inputs.size(), "Render selection snapshots should become page need inputs.");
			context.ExpectEqual(std::string("Game:2"), inputs.front().viewId, "Snapshot adapter should preserve view ownership in a stable string.");
			context.ExpectEqual(static_cast<std::size_t>(2u), packets.size(), "Async handoff should request desired and fallback pages when neither is resident.");
			context.Expect(packets.front().fallbackRequest, "Fallback request should sort first because it protects visibility.");
			context.ExpectEqual(0u, packets.front().pageId, "Fallback packet should target the fallback page.");
			context.ExpectEqual(3u, packets.back().pageId, "Desired packet should target the requested high-detail page.");
			context.ExpectEqual(44u, packets.back().packetId, "Desired page packet should preserve the selected cluster packet id.");
			context.ExpectEqual(5u, packets.back().batchId, "Desired page packet should preserve the selected batch id.");
			context.ExpectEqual(40u, packets.back().packetOffset, "Desired page packet should preserve the batch packet offset.");
			context.ExpectEqual(8u, packets.back().packetCount, "Desired page packet should preserve the batch packet count.");
		});

		registry.Add("VirtualGeometry.Resource.Streaming.PageRequestQueueRetriesAndDropsExpiredPackets", [](TestContext& context) {
			Resource::VirtualGeometryPageRequestPlan plan;
			Resource::VirtualGeometryPageRequestPacket fallback;
			fallback.assetId = MakePageAssetId(40u);
			fallback.objectId = MakePageAssetId(41u);
			fallback.viewId = "Game:2";
			fallback.pageId = 0u;
			fallback.priority = 1000;
			fallback.fallbackRequest = true;
			plan.packets.push_back(fallback);

			Resource::VirtualGeometryQueuedPageRequest retry;
			retry.packet = fallback;
			retry.packet.pageId = 5u;
			retry.packet.fallbackRequest = false;
			retry.retryCount = 1u;
			retry.nextRetryFrame = 12u;
			Resource::VirtualGeometryQueuedPageRequest dropped = retry;
			dropped.packet.pageId = 6u;
			dropped.retryCount = 3u;

			Resource::VirtualGeometryPageRetryPolicy retryPolicy;
			retryPolicy.maxRetryCount = 3u;
			retryPolicy.retryDelayFrames = 4u;
			const Resource::VirtualGeometryPageRequestQueuePlan queuePlan =
				Resource::BuildVirtualGeometryPageRequestQueuePlan(plan, { retry, dropped }, retryPolicy, 12u);

			context.ExpectEqual(static_cast<std::size_t>(2u), queuePlan.readyPackets.size(), "New packets and due retries should both be ready for protected IO handoff.");
			context.ExpectEqual(static_cast<std::size_t>(2u), queuePlan.queuedRequests.size(), "Ready packets should stay queued until IO/upload publishes a terminal state.");
			context.ExpectEqual(static_cast<std::size_t>(1u), queuePlan.droppedRequests.size(), "Packets at the retry limit should be dropped for diagnostics.");
			context.ExpectEqual(1u, queuePlan.retriedPackets, "Due previous packets should increment retry counters.");
			context.ExpectEqual(1u, queuePlan.fallbackPackets, "Fallback-critical requests should stay counted separately.");
			context.ExpectEqual(16ull, queuePlan.queuedRequests.front().nextRetryFrame, "Queued retries should be delayed by the retry policy.");
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

		registry.Add("VirtualGeometry.Resource.Residency.BudgetPressureReportsPageStateBytes", [](TestContext& context) {
			std::vector<Resource::VirtualGeometryResidentPage> pages = {
				MakeResidentPage(4u, 0u, 128u, 1000, 100u, true),
				MakeResidentPage(4u, 1u, 4096u, 10, 30u, false)
			};
			pages[0].pinnedFallback = true;

			Resource::VirtualGeometryResidentPage requested;
			requested.assetId = MakePageAssetId(4u);
			requested.pageId = 2u;
			requested.state = Resource::VirtualGeometryPageResidencyState::Requested;
			requested.byteSize = 512u;
			pages.push_back(requested);

			Resource::VirtualGeometryResidentPage pendingIo = requested;
			pendingIo.pageId = 3u;
			pendingIo.state = Resource::VirtualGeometryPageResidencyState::PendingIO;
			pendingIo.byteSize = 256u;
			pages.push_back(pendingIo);

			Resource::VirtualGeometryResidentPage pendingUpload = requested;
			pendingUpload.pageId = 4u;
			pendingUpload.state = Resource::VirtualGeometryPageResidencyState::PendingUpload;
			pendingUpload.byteSize = 1024u;
			pages.push_back(pendingUpload);

			Resource::VirtualGeometryResidentPage failed = requested;
			failed.pageId = 5u;
			failed.state = Resource::VirtualGeometryPageResidencyState::Failed;
			failed.byteSize = 2048u;
			pages.push_back(failed);

			Resource::VirtualGeometryResidentPage evicting = requested;
			evicting.pageId = 6u;
			evicting.state = Resource::VirtualGeometryPageResidencyState::Evicting;
			evicting.byteSize = 8192u;
			pages.push_back(evicting);

			Resource::VirtualGeometryResidentPage evicted = requested;
			evicted.pageId = 7u;
			evicted.state = Resource::VirtualGeometryPageResidencyState::Evicted;
			evicted.byteSize = 64u;
			pages.push_back(evicted);

			Resource::VirtualGeometryPageBudget budget;
			budget.memoryBudgetBytes = 1024u;
			const Resource::VirtualGeometryPageBudgetPressureSummary summary =
				Resource::BuildVirtualGeometryPageBudgetPressureSummary(pages, budget);

			context.ExpectEqual(4224ull, summary.residentBytes, "Resident bytes should include fallback and high-detail resident pages.");
			context.ExpectEqual(4096ull, summary.reclaimableBytes, "Only nonfallback resident pages should be reclaimable.");
			context.ExpectEqual(128ull, summary.pinnedFallbackBytes, "Pinned fallback bytes should be protected from eviction.");
			context.ExpectEqual(1792ull, summary.requestedBytes, "Requested bytes should include Requested, PendingIO, and PendingUpload pages.");
			context.ExpectEqual(256ull, summary.pendingIoBytes, "Pending IO bytes should be reported separately from upload work.");
			context.ExpectEqual(1024ull, summary.pendingUploadBytes, "Pending upload bytes should be reported separately from disk IO.");
			context.ExpectEqual(2048ull, summary.failedBytes, "Failed page bytes should be visible for streaming pressure diagnostics.");
			context.ExpectEqual(8192ull, summary.evictingBytes, "Evicting bytes should remain visible until protected retirement completes.");
			context.ExpectEqual(64ull, summary.evictedBytes, "Evicted bytes should identify recently retired page pressure.");
			context.ExpectEqual(summary.overBudgetBytes, summary.pressureBytes, "Pressure bytes should mirror the older over-budget field.");
			context.Expect(!summary.contributors.empty(), "Byte accounting should still publish per-asset contributors.");
			context.ExpectEqual(summary.pinnedFallbackBytes, summary.contributors.front().pinnedFallbackBytes, "Contributor should carry pinned fallback bytes.");
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
