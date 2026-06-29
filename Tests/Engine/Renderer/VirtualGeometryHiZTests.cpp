#include "TestHarness.h"

#include "Renderer/RHI/Vulkan/VulkanHiZ.h"

#include <glm/glm.hpp>

namespace TerraEngine::Tests
{
	namespace
	{
		VulkanHiZHistoryMetadata MakeValidHistory(std::uint32_t viewId, std::uint32_t width, std::uint32_t height, const glm::mat4& viewProjection)
		{
			VulkanHiZHistoryMetadata history{};
			history.viewId = viewId;
			history.width = width;
			history.height = height;
			history.projectionSignature = BuildVulkanHiZProjectionSignature(viewProjection);
			history.frameId = 11u;
			history.valid = true;
			history.invalidReason = VulkanHiZInvalidReason::None;
			return history;
		}

		VulkanHiZValidationInput MakeValidationInput(std::uint32_t viewId, std::uint32_t width, std::uint32_t height, const glm::mat4& viewProjection)
		{
			VulkanHiZValidationInput input{};
			input.viewId = viewId;
			input.width = width;
			input.height = height;
			input.projectionSignature = BuildVulkanHiZProjectionSignature(viewProjection);
			input.pyramidReady = true;
			input.qualityAllowsOcclusion = true;
			input.viewSupported = true;
			input.selfOcclusionGuardPassed = true;
			return input;
		}
	}

	void RegisterVirtualGeometryHiZTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.HiZ.Validation.RejectsFirstFrameResizeProjectionAndViewMismatch", [](TestContext& context) {
			glm::mat4 viewProjection(1.0f);
			VulkanHiZValidationInput input = MakeValidationInput(1u, 1920u, 1080u, viewProjection);

			const VulkanHiZValidationResult firstFrame = ValidateVulkanHiZHistory({}, input);
			context.Expect(!firstFrame.occlusionAllowed, "The first frame should fail open because no previous Hi-Z pyramid exists.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::FirstFrame), static_cast<std::uint32_t>(firstFrame.invalidReason), "First-frame validation should report the first-frame reason.");

			const VulkanHiZHistoryMetadata history = MakeValidHistory(1u, 1920u, 1080u, viewProjection);
			VulkanHiZValidationInput resizeInput = input;
			resizeInput.width = 1280u;
			const VulkanHiZValidationResult resize = ValidateVulkanHiZHistory(history, resizeInput);
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::Resize), static_cast<std::uint32_t>(resize.invalidReason), "Target-size changes should invalidate Hi-Z history.");

			glm::mat4 changedProjection = viewProjection;
			changedProjection[3][2] = 0.25f;
			VulkanHiZValidationInput projectionInput = MakeValidationInput(1u, 1920u, 1080u, changedProjection);
			const VulkanHiZValidationResult projectionChanged = ValidateVulkanHiZHistory(history, projectionInput);
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::ProjectionChanged), static_cast<std::uint32_t>(projectionChanged.invalidReason), "Projection changes should invalidate Hi-Z history.");

			VulkanHiZValidationInput viewInput = input;
			viewInput.viewId = 2u;
			const VulkanHiZValidationResult viewMismatch = ValidateVulkanHiZHistory(history, viewInput);
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::ViewMismatch), static_cast<std::uint32_t>(viewMismatch.invalidReason), "Another view must not reuse this view's Hi-Z pyramid.");

			const VulkanHiZValidationResult valid = ValidateVulkanHiZHistory(history, input);
			context.Expect(valid.historyValid, "Matching view-owned Hi-Z metadata should validate.");
			context.Expect(valid.occlusionAllowed, "Matching view-owned Hi-Z metadata should allow occlusion when quality and guard gates pass.");
		});

		registry.Add("VirtualGeometry.HiZ.Validation.FailsOpenForSelfOcclusionRisk", [](TestContext& context) {
			const glm::mat4 viewProjection(1.0f);
			const VulkanHiZHistoryMetadata history = MakeValidHistory(3u, 1024u, 768u, viewProjection);
			VulkanHiZValidationInput input = MakeValidationInput(3u, 1024u, 768u, viewProjection);
			input.selfOcclusionGuardPassed = false;

			const VulkanHiZValidationResult result = ValidateVulkanHiZHistory(history, input);

			context.Expect(result.historyValid, "Self-occlusion guard failures should keep the history classified separately from stale data.");
			context.Expect(!result.occlusionAllowed, "Self-occlusion risk should fail open instead of hiding thin or self-overlapping geometry.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::SelfOcclusionRisk), static_cast<std::uint32_t>(result.invalidReason), "Self-occlusion guard failure should report its own reason.");
		});

		registry.Add("VirtualGeometry.HiZ.Validation.CameraCutAndMultiViewReuseStayFailOpen", [](TestContext& context) {
			glm::mat4 previousViewProjection(1.0f);
			const VulkanHiZHistoryMetadata history = MakeValidHistory(9u, 1600u, 900u, previousViewProjection);

			glm::mat4 cameraCutProjection(1.0f);
			cameraCutProjection[3][0] = 4.0f;
			cameraCutProjection[3][2] = -2.0f;
			const VulkanHiZValidationInput cameraCutInput = MakeValidationInput(9u, 1600u, 900u, cameraCutProjection);
			const VulkanHiZValidationResult cameraCut = ValidateVulkanHiZHistory(history, cameraCutInput);

			VulkanHiZValidationInput sceneViewInput = MakeValidationInput(10u, 1600u, 900u, previousViewProjection);
			const VulkanHiZValidationResult multiViewReuse = ValidateVulkanHiZHistory(history, sceneViewInput);

			context.Expect(!cameraCut.occlusionAllowed, "Camera cuts should fail open instead of trusting stale Hi-Z depth.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::ProjectionChanged), static_cast<std::uint32_t>(cameraCut.invalidReason), "Camera-cut validation should report projection/view-projection change.");
			context.Expect(!multiViewReuse.occlusionAllowed, "A second view must not reuse another view's Hi-Z history.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::ViewMismatch), static_cast<std::uint32_t>(multiViewReuse.invalidReason), "Multi-view reuse should stay isolated by view id.");
		});
	}
}
