#include "TestHarness.h"

#include "Resource/ResourceResidency.h"

#include <vector>

namespace TerraEngine::Tests {
	void RegisterVirtualGeometryPageResidencyTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometryPageResidency.Foundation.RetirementLifecycleKeepsPageResidentUntilSafeRelease", [](TestContext& context) {
			Resource::VirtualGeometryResidentPage page;
			page.pageId = 5u;
			page.byteSize = 4096u;
			page.state = Resource::VirtualGeometryPageResidencyState::Resident;

			const Resource::VirtualGeometryPageTransitionResult queued = Resource::ApplyVirtualGeometryPageTransition(page,
				Resource::VirtualGeometryPageResidencyEvent::BeginRetire,
				20u,
				"budget pressure");
			context.Expect(queued.applied, "Resident high-detail pages should enter a deferred retirement state.");
			context.Expect(page.state == Resource::VirtualGeometryPageResidencyState::PendingRetirement, "Retirement should first change eligibility, not immediately destroy the page.");
			context.Expect(Resource::IsVirtualGeometryPageResidentForRendering(page), "Pending-retirement pages remain renderable until in-flight frames are safe.");

			const Resource::VirtualGeometryPageTransitionResult retired = Resource::ApplyVirtualGeometryPageTransition(page,
				Resource::VirtualGeometryPageResidencyEvent::MarkRetired,
				23u,
				"safe frame completed");
			context.Expect(retired.applied, "Pending-retirement pages should retire after the protected release boundary.");
			context.Expect(page.state == Resource::VirtualGeometryPageResidencyState::Retired, "Safe release should publish the terminal retired state.");
			context.Expect(!Resource::IsVirtualGeometryPageResidentForRendering(page), "Retired pages must no longer be eligible for rendering.");
			context.Expect(Resource::ToString(page.state) == "retired", "Diagnostics should expose a stable retired-state label.");
		});

		registry.Add("VirtualGeometryPageResidency.Foundation.ProtectedFallbackCannotRetire", [](TestContext& context) {
			Resource::VirtualGeometryResidentPage fallback;
			fallback.pageId = 1u;
			fallback.byteSize = 1024u;
			fallback.state = Resource::VirtualGeometryPageResidencyState::Pinned;
			fallback.protectedFallback = true;

			const Resource::VirtualGeometryPageTransitionResult result = Resource::ApplyVirtualGeometryPageTransition(fallback,
				Resource::VirtualGeometryPageResidencyEvent::BeginRetire,
				30u,
				"budget pressure");

			context.Expect(!result.applied, "Protected fallback pages should not enter the retirement path.");
			context.Expect(fallback.state == Resource::VirtualGeometryPageResidencyState::Pinned, "Fallback protection must keep the renderable pinned state intact.");
			context.Expect(Resource::IsVirtualGeometryPagePinnedFallback(fallback), "Protected fallback flags should participate in pin checks.");
		});

		registry.Add("VirtualGeometryPageResidency.Foundation.BudgetSummaryTracksHighDetailFallbackAndRetiringBytes", [](TestContext& context) {
			Resource::VirtualGeometryResidentPage highDetail;
			highDetail.pageId = 2u;
			highDetail.byteSize = 4096u;
			highDetail.state = Resource::VirtualGeometryPageResidencyState::Resident;

			Resource::VirtualGeometryResidentPage fallback;
			fallback.pageId = 1u;
			fallback.byteSize = 1024u;
			fallback.state = Resource::VirtualGeometryPageResidencyState::Pinned;
			fallback.fallbackPage = true;
			fallback.protectedFallback = true;

			Resource::VirtualGeometryResidentPage retiring;
			retiring.pageId = 3u;
			retiring.byteSize = 2048u;
			retiring.state = Resource::VirtualGeometryPageResidencyState::PendingRetirement;

			const std::vector<Resource::VirtualGeometryResidentPage> pages = {
				highDetail,
				fallback,
				retiring,
			};
			Resource::VirtualGeometryPageBudget budget;
			budget.memoryBudgetBytes = 8192u;
			const Resource::VirtualGeometryPageBudgetPressureSummary summary = Resource::BuildVirtualGeometryPageBudgetPressureSummary(pages, budget);

			context.Expect(summary.residentPages == 3u, "Resident accounting should include pinned and pending-retirement pages.");
			context.Expect(summary.residentBytes == 7168u, "Resident bytes should include all render-eligible page payloads.");
			context.Expect(summary.residentHighDetailPages == 1u, "Only non-fallback resident pages should count as high-detail resident pages.");
			context.Expect(summary.residentHighDetailBytes == 4096u, "High-detail bytes should exclude protected fallback and retiring payloads.");
			context.Expect(summary.pinnedFallbackPages == 1u, "Pinned fallback pages need explicit budget pressure accounting.");
			context.Expect(summary.embeddedFallbackBytes == 1024u, "Fallback byte totals should remain visible even when fallbacks are protected.");
			context.Expect(summary.retiringPages == 1u, "Pending-retirement pages should be visible to budget diagnostics.");
			context.Expect(summary.retiringBytes == 2048u, "Retiring byte totals help verify deferred release drains over time.");
		});

		registry.Add("VirtualGeometryPageResidency.PinnedFallbacksAreExcludedFromEvictionCandidates", [](TestContext& context) {
			Resource::VirtualGeometryResidentPage fallback;
			fallback.assetId = TerraEngine::GenerateUUID();
			fallback.pageId = 0u;
			fallback.fallbackPageId = 0u;
			fallback.byteSize = 1024u;
			fallback.state = Resource::VirtualGeometryPageResidencyState::Resident;
			fallback.fallbackPage = true;

			Resource::VirtualGeometryResidentPage highDetail;
			highDetail.assetId = fallback.assetId;
			highDetail.pageId = 1u;
			highDetail.fallbackPageId = 0u;
			highDetail.byteSize = 8192u;
			highDetail.state = Resource::VirtualGeometryPageResidencyState::Resident;
			highDetail.priority = 1;

			std::vector<Resource::VirtualGeometryResidentPage> pages = { fallback, highDetail };
			const std::uint32_t protectedCount = Resource::ProtectVirtualGeometryFallbackPages(pages, 55u, "visible asset fallback coverage");

			Resource::VirtualGeometryPageBudget budget;
			budget.memoryBudgetBytes = 1024u;
			const std::vector<Resource::VirtualGeometryPageEvictionCandidate> candidates =
				Resource::BuildVirtualGeometryPageEvictionCandidates(pages, budget);

			context.ExpectEqual(1u, protectedCount, "Exactly one fallback page should be protected for the visible asset.");
			context.Expect(pages.front().state == Resource::VirtualGeometryPageResidencyState::Pinned, "Fallback protection should promote resident fallback pages to pinned.");
			context.ExpectEqual(static_cast<std::size_t>(1u), candidates.size(), "Only high-detail pages should become eviction candidates under pressure.");
			context.ExpectEqual(1u, candidates.front().pageId, "The high-detail page should be selected before protected fallback coverage.");
		});

		registry.Add("VirtualGeometryPageResidency.ProtectionRestoresFallbacksTouchedByEviction", [](TestContext& context) {
			Resource::VirtualGeometryResidentPage fallback;
			fallback.pageId = 0u;
			fallback.fallbackPageId = 0u;
			fallback.byteSize = 2048u;
			fallback.state = Resource::VirtualGeometryPageResidencyState::Evicting;
			fallback.protectedFallback = true;

			std::vector<Resource::VirtualGeometryResidentPage> pages = { fallback };
			const std::uint32_t protectedCount = Resource::ProtectVirtualGeometryFallbackPages(pages, 88u, "fallback coverage must remain visible");

			context.ExpectEqual(1u, protectedCount, "Protection should detect a fallback page even after an eviction state touched it.");
			context.Expect(pages.front().state == Resource::VirtualGeometryPageResidencyState::Pinned, "Protection should restore renderable pinned state before the frame publishes residency.");
			context.Expect(Resource::IsVirtualGeometryPageResidentForRendering(pages.front()), "Restored fallback pages should be renderable immediately.");
			context.Expect(pages.front().requestReason.find("fallback") != std::string::npos, "Protection should preserve an editor-readable reason.");
		});

		registry.Add("VirtualGeometryPageResidency.BudgetPressureEvictsOldHighDetailBeforeFallback", [](TestContext& context) {
			const TerraEngine::UUID assetId = TerraEngine::GenerateUUID();

			Resource::VirtualGeometryResidentPage fallback;
			fallback.assetId = assetId;
			fallback.pageId = 0u;
			fallback.fallbackPageId = 0u;
			fallback.byteSize = 1024u;
			fallback.state = Resource::VirtualGeometryPageResidencyState::Pinned;
			fallback.fallbackPage = true;
			fallback.protectedFallback = true;

			Resource::VirtualGeometryResidentPage oldHighDetail;
			oldHighDetail.assetId = assetId;
			oldHighDetail.pageId = 2u;
			oldHighDetail.fallbackPageId = 0u;
			oldHighDetail.byteSize = 4096u;
			oldHighDetail.state = Resource::VirtualGeometryPageResidencyState::Resident;
			oldHighDetail.priority = 1;
			oldHighDetail.lastUsedFrame = 10u;

			Resource::VirtualGeometryResidentPage newerHighDetail = oldHighDetail;
			newerHighDetail.pageId = 3u;
			newerHighDetail.priority = 10;
			newerHighDetail.lastUsedFrame = 20u;

			std::vector<Resource::VirtualGeometryResidentPage> pages = { fallback, newerHighDetail, oldHighDetail };
			Resource::VirtualGeometryPageBudget budget;
			budget.memoryBudgetBytes = 5120u;

			const Resource::VirtualGeometryPageBudgetPressureSummary summary =
				Resource::BuildVirtualGeometryPageBudgetPressureSummary(pages, budget);
			const std::vector<Resource::VirtualGeometryPageEvictionCandidate> candidates =
				Resource::BuildVirtualGeometryPageEvictionCandidates(pages, budget);

			context.ExpectEqual(9216ull, summary.residentBytes, "Budget pressure should include fallback plus both high-detail pages.");
			context.ExpectEqual(4096ull, summary.overBudgetBytes, "The planner only needs to reclaim one high-detail page.");
			context.ExpectEqual(static_cast<std::size_t>(1u), candidates.size(), "Eviction candidates should be trimmed to the required reclaim size.");
			context.ExpectEqual(2u, candidates.front().pageId, "The older, lower-priority high-detail page should be evicted first.");
		});
	}
}
