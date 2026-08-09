// AnimationAuthorityPageTests.cpp - Fixed-capacity authority-page behavior.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationAuthorityPublication.h"

#include <memory>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;

	void RegisterAnimationAuthorityPageTests(TestRegistry& registry)
	{
		registry.Add("Animation.AuthorityPages.RejectUnsupportedCapacity", [](TestContext& context) {
			Animation::AnimationAuthorityPage page;
			page.capacity = 64U;
			page.changedMasks.assign(1U, 0U);
			context.Expect(!page.IsValid(),
				"Authority pages accept only the benchmarked 128- or 256-row capacities.");
		});

		registry.Add("Animation.AuthorityPages.EmptyPageIsImmutableAndValid", [](TestContext& context) {
			auto page = std::make_shared<Animation::AnimationAuthorityPage>();
			page->capacity = static_cast<std::uint32_t>(Animation::kAnimationAuthorityPageCapacity128);
			page->changedMasks.assign(2U, 0U);
			const std::shared_ptr<const Animation::AnimationAuthorityPage> immutable = page;
			context.Expect(immutable->IsValid() && immutable->Find({1U, 1U, 1U}) == nullptr,
				"Unchanged empty pages remain shareable immutable values without a live-slot lookup.");
		});

		registry.Add("Animation.AuthorityPages.AcceptsBothBoundedPageCapacities", [](TestContext& context) {
			AnimationAuthorityPage compact;
			compact.capacity = static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128);
			compact.changedMasks.assign(2U, 0U);
			AnimationAuthorityPage dense;
			dense.capacity = static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity256);
			dense.changedMasks.assign(4U, 0U);
			context.Expect(compact.IsValid() && dense.IsValid(),
				"Authority pages must support the two explicitly bounded 128- and 256-row layouts.");
		});
	}
}
