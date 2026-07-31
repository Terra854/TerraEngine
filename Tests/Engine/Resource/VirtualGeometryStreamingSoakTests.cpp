// VirtualGeometryStreamingSoakTests.cpp - deterministic bounded feedback stress.

#include "Resource/VirtualGeometryStreamingPolicy.h"
#include "TestHarness.h"

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
			});
	}
} // namespace TerraEngine::Tests
