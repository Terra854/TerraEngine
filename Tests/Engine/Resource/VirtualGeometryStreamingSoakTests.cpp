// VirtualGeometryStreamingSoakTests.cpp - deterministic bounded feedback stress.

#include "Resource/VirtualGeometryStreamingPolicy.h"
#include "TestHarness.h"

#include <algorithm>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryStreamingSoakTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Streaming.SoakRemainsBoundedAndDeterministic",
			[](TestContext &context)
			{
				const auto asset = TerraEngine::GenerateUUID();
				std::vector<Resource::VirtualGeometryStreamingFeedback> feedback;
				for (std::uint32_t index = 0u; index < 1000u; ++index)
					feedback.push_back({ asset, index % 20u, 0u, {}, static_cast<std::int32_t>(index % 7u), index, true });
				Resource::VirtualGeometryStreamingPolicy policy;
				policy.maximumRequests = 12u;
				const auto first = Resource::BuildVirtualGeometryStreamingPlan(feedback, policy);
				const auto second = Resource::BuildVirtualGeometryStreamingPlan(feedback, policy);
				context.ExpectEqual(static_cast<std::size_t>(12u), first.requests.size(), "The request cap bounds sustained feedback pressure.");
				context.ExpectEqual(first.requests.size(), first.decodeIntents.size(), "The same cap must bound value-only decode intents.");
				context.ExpectEqual(first.requests.size(), first.uploadIntents.size(), "The same cap must bound protected upload intents.");
				context.ExpectEqual(
					first.requests.front().pageId, second.requests.front().pageId, "Equivalent feedback must produce deterministic priority order.");
				context.Expect(first.requests.size() == second.requests.size()
					&& std::equal(first.requests.begin(), first.requests.end(), second.requests.begin(),
						[](const auto& left, const auto& right) { return left.pageId == right.pageId && left.priority == right.priority; }),
					"Repeated planning must preserve the complete request ordering.");
				context.Expect(first.duplicateFeedbackCount > 0u, "Repeated page observations must be counted instead of growing the queue.");
				context.Expect(first.requests.size() <= policy.maximumRequests
					&& first.decodeIntents.size() <= policy.maximumRequests
					&& first.uploadIntents.size() <= policy.maximumRequests,
					"Sustained feedback must keep requests, decode, and upload work bounded by one cap.");
				for(const auto& intent : first.uploadIntents)
					context.Expect(intent.requiresVerifiedDecode && intent.requiresProtectedUploadCommit,
						"Upload intents must retain verified-decode and protected-commit requirements.");
			});
	}
} // namespace TerraEngine::Tests
