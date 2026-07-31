// VirtualGeometryResidencyLifecycleTests.cpp - bounded retry and retirement planning.

#include "Resource/ResourceResidency.h"
#include "TestHarness.h"

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryResidencyLifecycleTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Streaming.LifecycleBoundsRetriesAndRetirement",
			[](TestContext &context)
			{
				Resource::VirtualGeometryResidentPage failed;
				failed.pageId = 1u;
				failed.state = Resource::VirtualGeometryPageResidencyState::Failed;
				failed.requestCount = 2u;
				Resource::VirtualGeometryResidentPage retiring;
				retiring.pageId = 2u;
				retiring.state = Resource::VirtualGeometryPageResidencyState::PendingRetirement;
				retiring.retirementFrame = 10u;
				Resource::VirtualGeometryPageLifecycleLimits limits;
				limits.maximumRetries = 2u;
				limits.retirementFrameLag = 3u;
				const auto plan = Resource::BuildVirtualGeometryPageLifecyclePlan({ failed, retiring }, {}, limits, 13u);
				context.Expect(plan.valid && plan.retryablePageIds.size() == 1u, "Failed pages within the retry cap remain eligible for a later request.");
				context.Expect(plan.retiredPageIds.size() == 1u, "Retirement is delayed until the declared safe-frame lag.");
			});
	}
} // namespace TerraEngine::Tests
