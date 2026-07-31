#include "Renderer/VirtualGeometry/Selection/VirtualGeometryViewPolicy.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/RHI/Vulkan/VulkanHiZ.h"
#include "TestHarness.h"
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryTwoPassOcclusionTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.Occlusion.InvalidatesHistoryOnTeleport",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				VirtualGeometryViewPolicyState v{ VirtualGeometryViewKind::Camera, 1, 3, 4 };
				v.teleported = true;
				auto d = EvaluateVirtualGeometryViewPolicy(v, v);
				c.Expect(!d.historyValid && d.pass == VirtualGeometryOcclusionPass::Main, "Teleport starts a main pass without stale occlusion.");
			});
		r.Add("VirtualGeometry.Occlusion.DeclaresMainThenPostPassesPerView",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				RenderFrameSnapshot snapshot;
				snapshot.viewRequests.push_back({ RenderOutputKind::GameView,
					RenderViewType::Game, glm::uvec2(640u, 360u), 2u });
				BackendCapabilityProfile profile;
				const std::vector<RenderPassNode> passes = BuildDefaultRenderPasses(snapshot, profile);
				c.Expect(passes.size() == 4u
					&& passes[1].passId.find("main-occlusion") != std::string::npos
					&& passes[2].dependencies.front() == passes[1].passId
					&& passes[3].passId.find("post-occlusion") != std::string::npos
					&& passes[3].dependencies.front() == passes[2].passId,
					"Each view must schedule conservative main occlusion before draw and post occlusion after its depth output.");
			});
		r.Add("VirtualGeometry.Occlusion.HiZScheduleInvalidatesBeforePostReuse",
			[](TestContext &c)
			{
				VulkanHiZHistoryMetadata history{};
				history.valid = true;
				history.viewId = 7u;
				history.viewClass = VulkanHiZViewClass::Game;
				history.targetId = 9u;
				history.width = 640u;
				history.height = 360u;
				history.projectionSignature = 11u;
				history.depthSceneSignature = 13u;
				VulkanHiZValidationInput input{};
				input.pyramidReady = true;
				input.viewId = 7u;
				input.viewClass = VulkanHiZViewClass::Game;
				input.targetId = 9u;
				input.width = 640u;
				input.height = 360u;
				input.projectionSignature = 11u;
				input.depthSceneSignature = 13u;
				const VulkanHiZOcclusionSchedule reused = BuildVulkanHiZOcclusionSchedule(history, input);
				input.cameraCut = true;
				const VulkanHiZOcclusionSchedule invalidated = BuildVulkanHiZOcclusionSchedule(history, input);
				c.Expect(reused.runMainPass && reused.runPostPass && reused.reusesHistory
					&& invalidated.runMainPass && invalidated.runPostPass
					&& !invalidated.reusesHistory
					&& invalidated.invalidation == VulkanHiZInvalidReason::CameraCut,
					"A camera cut must rebuild conservative main occlusion before the ready-pyramid post pass can reuse history.");
			});
}
} // namespace TerraEngine::Tests
