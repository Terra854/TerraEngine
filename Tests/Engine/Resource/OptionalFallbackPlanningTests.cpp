// Value-only planning must never synthesize a sentinel page as real work.
#include "TestHarness.h"
#include "Resource/VirtualGeometryStreamingPolicy.h"
#include <algorithm>

namespace TerraEngine::Tests {
namespace {
	Resource::VirtualGeometryStreamingFeedback Feedback() {
		Resource::VirtualGeometryStreamingFeedback value; value.assetId = TerraEngine::UUID{126U}; value.pageId = 7U;
		value.observedFrame = 10U; value.priority = 4; return value;
	}
	void ExpectOnly(TestContext& context, const Resource::VirtualGeometryStreamingPlan& plan, std::vector<std::uint32_t> expected) {
		std::vector<std::uint32_t> actual; for(const auto& request : plan.requests) actual.push_back(request.pageId);
		std::sort(actual.begin(), actual.end()); std::sort(expected.begin(), expected.end());
		context.Expect(actual == expected && plan.decodeIntents.size() == expected.size() && plan.uploadIntents.size() == expected.size(),
			"Only actual primary/dependency pages may produce request/decode/upload intents.");
		context.Expect(std::none_of(plan.decodeIntents.begin(), plan.decodeIntents.end(), [](const auto& value) { return value.request.pageId == kVirtualGeometryInvalidIndex; })
			&& std::none_of(plan.uploadIntents.begin(), plan.uploadIntents.end(), [](const auto& value) { return value.request.pageId == kVirtualGeometryInvalidIndex; }),
			"Optional absence is not a physical decode/upload page.");
	}
}
void RegisterOptionalFallbackPlanningTests(TestRegistry& registry) {
	registry.Add("Resource.OptionalFallback.AbsentFallbackDoesNotCreateSentinelWork", [](TestContext& context) {
		Resource::VirtualGeometryStreamingPolicy policy; policy.requireFallback = false;
		const auto plan = Resource::BuildVirtualGeometryStreamingPlan({Feedback()}, policy); ExpectOnly(context, plan, {7U});
		context.Expect(plan.expandedDependencyCount == 0U && plan.rejectedFeedbackCount == 0U, "An optional missing fallback has no expansion or rejection charge.");
	});
	registry.Add("Resource.OptionalFallback.RealDependenciesStillExpand", [](TestContext& context) {
		auto value = Feedback(); value.dependencyPageIds = {2U}; Resource::VirtualGeometryStreamingPolicy policy; policy.requireFallback = false;
		const auto plan = Resource::BuildVirtualGeometryStreamingPlan({value}, policy); ExpectOnly(context, plan, {2U,7U});
		context.Expect(plan.expandedDependencyCount == 1U, "Count the one real dependency without adding a sentinel fallback.");
	});
	registry.Add("Resource.OptionalFallback.RequiredAndPresentFallbackControls", [](TestContext& context) {
		context.Expect(Resource::BuildVirtualGeometryStreamingPlan({Feedback()}).requests.empty(), "The unchanged required-fallback policy rejects missing fallback evidence.");
		auto value = Feedback(); value.fallbackPageId = 3U; ExpectOnly(context, Resource::BuildVirtualGeometryStreamingPlan({value}), {3U,7U});
		value.fallbackPageId = value.pageId; ExpectOnly(context, Resource::BuildVirtualGeometryStreamingPlan({value}), {7U});
		Resource::VirtualGeometryStreamingPolicy disabled; disabled.requireFallback = false; disabled.expandDependencies = false;
		ExpectOnly(context, Resource::BuildVirtualGeometryStreamingPlan({Feedback()}, disabled), {7U});
	});
}
}
