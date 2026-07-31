// VirtualGeometryFeedbackStreamingTests.cpp - worker-safe request planning contracts.

#include "Resource/VirtualGeometryStreamingPolicy.h"
#include "TestHarness.h"

#include <algorithm>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryFeedbackStreamingTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Streaming.FeedbackCoalescesAndExpandsPriorFrameRequests",
			[](TestContext& context)
			{
				const auto asset = TerraEngine::GenerateUUID();
				Resource::VirtualGeometryStreamingFeedback low{ asset, 4u, 0u, { 0u, 1u }, 2, 10u, true };
				auto high = low;
				high.priority = 8;
				high.dependencyPageIds = { 1u, 2u };
				const auto plan = Resource::BuildVirtualGeometryStreamingPlan({ low, high });

				context.ExpectEqual(static_cast<std::size_t>(4u), plan.requests.size(),
					"The requested page, pinned fallback, and two dependencies should each receive one typed request.");
				const auto root = std::find_if(plan.requests.begin(), plan.requests.end(),
					[](const auto& request) { return request.pageId == 4u; });
				context.Expect(root != plan.requests.end(),
					"The requested high-detail page must remain in the dependency-expanded plan.");
				if (root != plan.requests.end())
				{
					context.ExpectEqual(8, root->priority,
						"Coalescing should retain the strongest priority.");
					context.ExpectEqual(static_cast<std::size_t>(3u), root->dependencyPageIds.size(),
						"Dependency proof should be deduplicated.");
				}
				context.ExpectEqual(plan.requests.size(), plan.decodeIntents.size(),
					"Every request must expose one value-only decode intent.");
				context.ExpectEqual(plan.requests.size(), plan.uploadIntents.size(),
					"Every request must expose one protected-upload intent.");
				context.ExpectEqual(3u, plan.expandedDependencyCount,
					"Fallback and dependency expansion must be observable without claiming residency.");
			});

		registry.Add("VirtualGeometry.Streaming.FeedbackRejectsSelfDependentRequest",
			[](TestContext& context)
			{
				Resource::VirtualGeometryStreamingFeedback invalid;
				invalid.assetId = TerraEngine::GenerateUUID();
				invalid.pageId = 7u;
				invalid.fallbackPageId = 0u;
				invalid.dependencyPageIds = { 7u };
				const auto plan = Resource::BuildVirtualGeometryStreamingPlan({ invalid });
				context.Expect(plan.requests.empty(),
					"A self dependency cannot enter a worker plan because no publication can prove it complete.");
				context.ExpectEqual(1u, plan.rejectedFeedbackCount,
					"Invalid dependency feedback must be counted for diagnostics.");
			});
	}
} // namespace TerraEngine::Tests
