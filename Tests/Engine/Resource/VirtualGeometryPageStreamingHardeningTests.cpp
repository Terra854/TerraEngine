#include "TestHarness.h"

#include "Resource/ResourceResidency.h"
#include "Resource/ResourceStreaming.h"
#include "Renderer/Graph/RenderDiagnostics.h"

#include <string>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryPageStreamingHardeningTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Hardening.PageStreaming.PinnedFallbackStaysResident", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.assetId = TerraEngine::GenerateUUID();
			asset.detailLevels = {
				{ 0u, 0u, 4u, 128u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};

			GeometryPageRuntime fallback{};
			fallback.pageId = 0u;
			fallback.state = GeometryPageState::Pinned;
			fallback.byteSize = 256u;
			fallback.fallbackPage = true;
			fallback.pinnedFallback = true;

			GeometryPageRuntime missingDetail{};
			missingDetail.pageId = 1u;
			missingDetail.state = GeometryPageState::NotResident;
			missingDetail.byteSize = 1024u;
			missingDetail.fallbackPageId = fallback.pageId;
			asset.pages = { fallback, missingDetail };

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Game, 2904u, { asset }, MakeDefaultVirtualGeometryQualityProfile());

			context.ExpectEqual(1u, diagnostics.residentPages, "Pinned fallback pages should remain resident under page pressure.");
			context.ExpectEqual(1u, diagnostics.pinnedFallbackPages, "Pinned fallback pages should have a dedicated hardening counter.");
			context.ExpectEqual(1u, diagnostics.missingPages, "Missing high-detail pages should be counted without hiding fallback geometry.");
			context.Expect(!diagnostics.budgetContributors.empty(), "Streaming diagnostics should publish page contributors for future budget tests.");
		});

		registry.Add("VirtualGeometry.Hardening.PageStreaming.PendingStagesStayDistinct", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.detailLevels = {
				{ 0u, 0u, 2u, 64u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};

			GeometryPageRuntime pendingIo{};
			pendingIo.pageId = 2u;
			pendingIo.state = GeometryPageState::PendingIO;

			GeometryPageRuntime pendingUpload{};
			pendingUpload.pageId = 3u;
			pendingUpload.state = GeometryPageState::PendingUpload;
			asset.pages = { pendingIo, pendingUpload };

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Scene, 2905u, { asset }, MakeDefaultVirtualGeometryQualityProfile());

			context.ExpectEqual(2u, diagnostics.requestedPages, "Pending IO and upload pages should count as requested streaming work.");
			context.ExpectEqual(1u, diagnostics.pageResidency.pendingIoPages, "Pending IO should stay separate from upload work.");
			context.ExpectEqual(1u, diagnostics.pageResidency.pendingUploadPages, "Pending upload should stay separate from disk IO work.");
		});

		registry.Add("VirtualGeometry.Hardening.PageStreaming.FailedDesiredPageStillRequestsFallback", [](TestContext& context) {
			Resource::VirtualGeometryPageNeedInput input;
			input.assetId = TerraEngine::GenerateUUID();
			input.objectId = TerraEngine::GenerateUUID();
			input.viewId = "game";
			input.desiredLodIndex = 3u;
			input.desiredClusterId = 12u;
			input.fallbackRequired = true;
			input.pages = {
				{ 0u, 0u, 1u, 0u, 256u, 1000, false, false, 0u },
				{ 3u, 3u, 1u, 0u, 2048u, 200, false, true, 12u }
			};

			const Resource::VirtualGeometryPageRequestPlan plan =
				Resource::BuildVirtualGeometryPageRequestPlan({ input }, Resource::MakeDefaultStreamingPolicy());

			context.ExpectEqual(static_cast<std::size_t>(1u), plan.failures.size(), "Failed desired pages should publish a failure record instead of silently disappearing.");
			context.ExpectEqual(1u, plan.missingDesiredPages, "Desired-page failure count should identify the missing high-detail request.");
			context.Expect(plan.failures.front().fallbackAvailable, "Fallback availability should be reported with the desired-page failure.");
			context.Expect(!plan.failures.front().fallbackResident, "The fallback in this fixture still needs IO.");
			context.Expect(plan.failures.front().reason.find("failed") != std::string::npos, "Failure reason should explain why the desired page was not requested.");
			context.ExpectEqual(static_cast<std::size_t>(1u), plan.packets.size(), "Planner should still request fallback geometry after a high-detail failure.");
			context.Expect(plan.packets.front().fallbackRequest, "The remaining request should be marked as fallback-critical.");
			context.ExpectEqual(0u, plan.packets.front().pageId, "Fallback request should target the fallback page.");
		});

		registry.Add("VirtualGeometry.Hardening.PageStreaming.FailedDetailPreservesPinnedFallback", [](TestContext& context) {
			Resource::VirtualGeometryResidentPage fallback;
			fallback.assetId = TerraEngine::GenerateUUID();
			fallback.pageId = 0u;
			fallback.fallbackPageId = 0u;
			fallback.byteSize = 256u;
			fallback.fallbackPage = true;
			fallback.pinnedFallback = true;
			fallback.state = Resource::VirtualGeometryPageResidencyState::Pinned;

			Resource::VirtualGeometryResidentPage detail;
			detail.assetId = fallback.assetId;
			detail.pageId = 4u;
			detail.fallbackPageId = fallback.pageId;
			detail.byteSize = 2048u;
			detail.state = Resource::VirtualGeometryPageResidencyState::PendingIO;

			auto failed = Resource::ApplyVirtualGeometryPageTransition(detail, Resource::VirtualGeometryPageResidencyEvent::MarkFailed, 42u, "fixture io error");
			auto evictFallback = Resource::ApplyVirtualGeometryPageTransition(fallback, Resource::VirtualGeometryPageResidencyEvent::BeginEvict, 43u);

			context.Expect(failed.applied, "High-detail page failure should publish atomically.");
			context.Expect(detail.state == Resource::VirtualGeometryPageResidencyState::Failed, "Failed high-detail page should enter Failed state.");
			context.ExpectEqual(std::string("fixture io error"), detail.failureReason, "Failure reason should survive publication for diagnostics.");
			context.Expect(!evictFallback.applied, "Budget pressure must not evict fallback after a detail page fails.");
			context.Expect(fallback.state == Resource::VirtualGeometryPageResidencyState::Pinned, "Pinned fallback should remain renderable after desired-page failure.");
		});
	}
}
