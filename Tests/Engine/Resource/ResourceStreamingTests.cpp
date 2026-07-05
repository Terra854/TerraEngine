#include "TestHarness.h"

#include "Resource/ResourceStreaming.h"

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::UUID MakeStreamingId(std::uint8_t value)
		{
			TerraEngine::UUID id{};
			id.fill(0);
			id[15] = value;
			return id;
		}

		Resource::VirtualGeometryStreamingPage MakePage(std::uint32_t pageId, std::uint32_t firstLod, bool resident)
		{
			Resource::VirtualGeometryStreamingPage page;
			page.pageId = pageId;
			page.firstLodIndex = firstLod;
			page.lodCount = 1u;
			page.fallbackPageId = 0u;
			page.byteSize = pageId == 0u ? 1024u : 4096u;
			page.priority = pageId == 0u ? 100 : 5;
			page.resident = resident;
			page.clusterId = pageId;
			page.payloadByteOffset = static_cast<std::uint64_t>(pageId) * 4096u;
			page.payloadByteSize = page.byteSize;
			page.payloadSource = resident
				? (pageId == 0u ? VirtualGeometryPagePayloadSource::ResidentFallbackPayload : VirtualGeometryPagePayloadSource::ResidentPagePayload)
				: VirtualGeometryPagePayloadSource::Missing;
			page.pinnedFallback = pageId == 0u;
			return page;
		}

		Resource::VirtualGeometryPageNeedInput MakeNeed(TerraEngine::UUID assetId, TerraEngine::UUID objectId, float importance)
		{
			Resource::VirtualGeometryPageNeedInput input;
			input.assetId = assetId;
			input.objectId = objectId;
			input.viewId = "Game:1";
			input.desiredLodIndex = 2u;
			input.desiredClusterId = 7u;
			input.packetId = 11u;
			input.batchId = 3u;
			input.packetOffset = 4u;
			input.packetCount = 1u;
			input.screenImportance = importance;
			input.predictedNeed = importance;
			input.fallbackRequired = true;
			input.desiredPayloadSource = VirtualGeometryPagePayloadSource::Missing;
			input.fallbackPayloadSource = VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
			input.pages = {
				MakePage(0u, 0u, true),
				MakePage(5u, 2u, false),
			};
			return input;
		}
	}

	void RegisterResourceStreamingTests(TestRegistry& registry)
	{
		registry.Add("Resource.Streaming.US3.SelectionSnapshotsBecomePageNeedInputs", [](TestContext& context) {
			const TerraEngine::UUID assetId = MakeStreamingId(31u);
			const TerraEngine::UUID objectId = MakeStreamingId(32u);

			TerraEngine::Renderer::VirtualGeometryViewSelectionState state;
			state.viewType = RenderViewType::Game;
			state.outputId = 4u;
			TerraEngine::Renderer::VirtualGeometryPageNeedRecord need;
			need.meshId = assetId;
			need.objectId = objectId;
			need.pageId = 5u;
			need.fallbackPageId = 0u;
			need.clusterId = 7u;
			need.desiredLodIndex = 2u;
			need.screenImportance = 0.5f;
			need.fallbackRequired = true;
			need.desiredPayloadSource = VirtualGeometryPagePayloadSource::Missing;
			need.fallbackPayloadSource = VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
			state.pageNeeds.push_back(need);

			Resource::VirtualGeometryPageCatalogEntry catalog;
			catalog.assetId = assetId;
			catalog.assetPriority = 12;
			catalog.budgetPressureBytes = 4096u;
			catalog.pages = {
				MakePage(0u, 0u, true),
				MakePage(5u, 2u, false),
			};

			const std::vector<Resource::VirtualGeometryPageNeedInput> inputs =
				Resource::BuildVirtualGeometryPageNeedInputsFromSelectionStates({ state }, { catalog }, 2.0f);

			context.ExpectEqual(static_cast<std::size_t>(1u), inputs.size(), "Visible page needs should become worker-safe streaming inputs.");
			context.Expect(inputs.front().assetId == assetId, "The streaming input should keep the virtual-geometry mesh id.");
			context.Expect(inputs.front().objectId == objectId, "The streaming input should keep the requesting object id.");
			context.ExpectEqual(2u, inputs.front().desiredLodIndex, "The desired LOD should survive snapshot-to-streaming conversion.");
			context.Expect(inputs.front().predictedNeed == 1.0f, "Preload bias should clamp predicted need to the normalized range.");
			context.ExpectEqual(static_cast<std::size_t>(2u), inputs.front().pages.size(), "Catalog pages should be copied into the immutable input.");
		});

		registry.Add("Resource.Streaming.US3.PageRequestsCoalesceAndPrioritize", [](TestContext& context) {
			const TerraEngine::UUID assetId = MakeStreamingId(33u);
			const TerraEngine::UUID objectId = MakeStreamingId(34u);

			Resource::VirtualGeometryPageNeedInput first = MakeNeed(assetId, objectId, 0.25f);
			Resource::VirtualGeometryPageNeedInput second = MakeNeed(assetId, objectId, 0.9f);
			second.packetCount = 5u;

			Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();
			const Resource::VirtualGeometryPageRequestPlan plan =
				Resource::BuildVirtualGeometryPageRequestPlan({ first, second }, policy);

			context.ExpectEqual(static_cast<std::size_t>(1u), plan.packets.size(), "Duplicate visible clusters should create one page IO request.");
			context.ExpectEqual(5u, plan.packets.front().pageId, "The missing high-detail page should be requested.");
			context.Expect(!plan.packets.front().fallbackRequest, "Resident fallback pages should not enqueue duplicate fallback IO.");
			context.Expect(plan.packets.front().priority > 0, "The retained request should preserve the strongest visible priority.");
			context.ExpectEqual(5u, plan.packets.front().packetCount, "Merged diagnostics should keep the wider packet range.");
		});

		registry.Add("Resource.Streaming.US3.RetryQueueDeduplicatesPreviousAndReadyRequests", [](TestContext& context) {
			const TerraEngine::UUID assetId = MakeStreamingId(35u);
			const TerraEngine::UUID objectId = MakeStreamingId(36u);
			Resource::VirtualGeometryPageNeedInput input = MakeNeed(assetId, objectId, 0.8f);

			Resource::VirtualGeometryPageRequestPlan plan =
				Resource::BuildVirtualGeometryPageRequestPlan({ input }, Resource::MakeDefaultStreamingPolicy());
			Resource::VirtualGeometryQueuedPageRequest previous;
			previous.packet = plan.packets.front();
			previous.retryCount = 1u;
			previous.nextRetryFrame = 8u;
			previous.lastFailureReason = "temporary IO pressure";

			Resource::VirtualGeometryPageRetryPolicy retryPolicy;
			retryPolicy.maxRetryCount = 3u;
			retryPolicy.retryDelayFrames = 2u;
			const Resource::VirtualGeometryPageRequestQueuePlan queuePlan =
				Resource::BuildVirtualGeometryPageRequestQueuePlan(plan, { previous }, retryPolicy, 9u);

			context.ExpectEqual(static_cast<std::size_t>(1u), queuePlan.readyPackets.size(), "A retried page should not be submitted again by this frame's duplicate visible request.");
			context.ExpectEqual(static_cast<std::size_t>(1u), queuePlan.queuedRequests.size(), "The queue should retain one tracked retry entry for the page.");
			context.ExpectEqual(1u, queuePlan.retriedPackets, "Due retry entries should be counted once.");
			context.ExpectEqual(0u, queuePlan.fallbackPackets, "No fallback IO should be queued when fallback coverage is already resident.");
		});
	}
}
