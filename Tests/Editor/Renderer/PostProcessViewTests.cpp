#include "../TestHarness.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewPicking.h"

#include <limits>

namespace TerraEngine::EditorTests
{
	void RegisterPostProcessViewTests(TestRegistry& registry)
	{
		registry.Add("PostProcess.View.PickingUsesPublishedIdentityExtent", [](TestContext& context) {
			using TerraEngine::Editor::SceneViewPickPixel;
			context.Expect(SceneViewPickPixel({20.0F, 30.0F}, {640.0F, 480.0F}, {640U, 480U})
				== glm::uvec2(20U, 30U), "Native picking must retain the displayed top-left pixel orientation.");
			context.Expect(SceneViewPickPixel({320.0F, 240.0F}, {640.0F, 480.0F}, {320U, 240U})
				== glm::uvec2(160U, 120U), "A half-resolution identity image must use normalized viewport coordinates.");
			context.Expect(SceneViewPickPixel({160.0F, 120.0F}, {320.0F, 240.0F}, {640U, 480U})
				== glm::uvec2(320U, 240U), "An enlarged identity image must map into its actual attachment extent.");
			context.Expect(SceneViewPickPixel({0.0F, 0.0F}, {640.0F, 480.0F}, {320U, 240U})
				== glm::uvec2(0U), "The image's top-left pixel must stay at the attachment's top-left.");
			context.Expect(SceneViewPickPixel({700.0F, -2.0F}, {640.0F, 480.0F}, {320U, 240U})
				== glm::uvec2(319U, 0U), "Boundary overflow must clamp to a valid attachment texel.");
			context.Expect(SceneViewPickPixel({640.0F, 480.0F}, {640.0F, 480.0F}, {320U, 240U})
				== glm::uvec2(319U, 239U), "The exact lower-right image boundary must select its last texel.");
		});

		registry.Add("PostProcess.View.PickingHandlesFractionalDisplayAndInvalidInputs", [](TestContext& context) {
			using TerraEngine::Editor::SceneViewPickPixel;
			// The middle of a fractional ImGui rectangle still identifies the middle
			// attachment texel. A near-edge coordinate stays in the final pixel.
			context.Expect(SceneViewPickPixel({320.25F, 240.25F}, {640.5F, 480.5F}, {320U, 240U})
				== glm::uvec2(160U, 120U), "Fractional UI extents must preserve normalized position.");
			context.Expect(SceneViewPickPixel({640.49F, 480.49F}, {640.5F, 480.5F}, {320U, 240U})
				== glm::uvec2(319U, 239U), "A fractional image edge must not round beyond the identity attachment.");
			context.Expect(SceneViewPickPixel({20.0F, 30.0F}, {640.0F, 480.0F}, {1U, 1U})
				== glm::uvec2(0U), "A single-texel attachment has one selectable pixel.");
			const float nan = std::numeric_limits<float>::quiet_NaN();
			const float infinity = std::numeric_limits<float>::infinity();
			context.Expect(!SceneViewPickPixel({nan, 0.0F}, {640.0F, 480.0F}, {320U, 240U}),
				"A non-finite pointer position must not produce a GPU request.");
			context.Expect(!SceneViewPickPixel({0.0F, infinity}, {640.0F, 480.0F}, {320U, 240U}),
				"An infinite pointer coordinate must be rejected.");
			context.Expect(!SceneViewPickPixel({0.0F, 0.0F}, {infinity, 480.0F}, {320U, 240U}),
				"A non-finite display extent must be rejected.");
			context.Expect(!SceneViewPickPixel({0.0F, 0.0F}, {0.0F, 480.0F}, {320U, 240U}),
				"A zero display extent must be rejected.");
			context.Expect(!SceneViewPickPixel({0.0F, 0.0F}, {640.0F, -1.0F}, {320U, 240U}),
				"A negative display extent must be rejected.");
			context.Expect(!SceneViewPickPixel({0.0F, 0.0F}, {640.0F, 480.0F}, {0U, 240U}),
				"A missing identity attachment extent must be rejected.");
		});

		registry.Add("PostProcess.View.PendingPickExpiresOnAttachmentReplacement", [](TestContext& context) {
			TerraEngine::Editor::SceneViewPickRequest pending;
			pending.viewport = {10.0F, 20.0F, 640.0F, 480.0F};
			pending.identityExtent = {320U, 240U};
			pending.identityTargetGeneration = 7U;
			pending.expiresAt = 2.0;
			context.Expect(pending.IsCurrent(pending, 1.0), "An unchanged attachment remains valid while its click is pending.");
			auto current = pending;
			current.identityExtent = {640U, 480U};
			context.Expect(!pending.IsCurrent(current, 1.0), "Changing internal resolution must invalidate the pending pixel request.");
			current = pending;
			++current.identityTargetGeneration;
			context.Expect(!pending.IsCurrent(current, 1.0), "Replacing an attachment at the same extent must invalidate the old click.");
			current = pending;
			current.viewport.x += 1.0F;
			context.Expect(!pending.IsCurrent(current, 1.0), "Moving the displayed image must invalidate the pending click.");
		});
	}
}
