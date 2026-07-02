#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RHI/Vulkan/VulkanHiZ.h"
#include "Renderer/RenderTypes.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>

namespace TerraEngine::Tests
{
	namespace
	{
		std::string ReadDefaultFragmentShader()
		{
			// Shader debug modes are a source-level contract: the C++ enum and
			// HLSL constants must move together because the push constant only
			// carries a small numeric value into the fragment shader.
			const std::filesystem::path shaderPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/Default/Default.frag.hlsl";
			std::ifstream input(shaderPath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		std::string ReadDefaultTaskShader()
		{
			// Hi-Z culling behavior lives in shader source, so this helper lets
			// regression tests verify the task-stage contract without depending
			// on a specific graphics device being available on the test runner.
			const std::filesystem::path shaderPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/Default/Default.task.hlsl";
			std::ifstream input(shaderPath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		std::string ReadDefaultMeshShader()
		{
			// Mesh shader debug propagation is also a source-level contract; the
			// packet/batch counters are packed into interpolators before the
			// fragment visualizer can color selected virtual geometry.
			const std::filesystem::path shaderPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/Default/Default.mesh.hlsl";
			std::ifstream input(shaderPath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		bool ShaderContains(const std::string& shader, const std::string& token)
		{
			return shader.find(token) != std::string::npos;
		}
	}

	void RegisterVirtualGeometryShaderLayoutTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.ShaderLayout.PushConstantsCarrySelectedLodRange", [](TestContext& context) {
			context.Expect(offsetof(PushConstants, virtualMeshletOffset) > offsetof(PushConstants, indirectDrawOffset), "Virtual geometry range should follow indirect draw metadata.");
			context.ExpectEqual(offsetof(PushConstants, virtualMeshletOffset) + sizeof(std::uint32_t), offsetof(PushConstants, virtualMeshletCount), "Virtual meshlet offset/count should be tightly packed.");
			context.ExpectEqual(offsetof(PushConstants, selectedVirtualLod) + sizeof(std::uint32_t), offsetof(PushConstants, virtualGeometryFlags), "Selected LOD and flags should mirror shader uint fields.");
			context.ExpectEqual(offsetof(PushConstants, virtualGeometryFlags) + sizeof(std::uint32_t), offsetof(PushConstants, selectedVirtualClusterOffset), "Virtual cluster payload should begin immediately after virtual geometry flags.");
			context.ExpectEqual(offsetof(PushConstants, selectedVirtualClusterOffset) + sizeof(std::uint32_t), offsetof(PushConstants, selectedVirtualClusterCount), "Selected cluster offset/count should be tightly packed for mesh shader decoding.");
			context.ExpectEqual(offsetof(PushConstants, selectedVirtualPageNeedOffset) + sizeof(std::uint32_t), offsetof(PushConstants, selectedVirtualPageNeedCount), "Page need offset/count should be tightly packed for streaming diagnostics.");
			context.Expect((offsetof(PushConstants, editorTint) % 16u) == 0u, "Editor tint should remain 16-byte aligned after virtual geometry push constants.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.ClusterSelectionRecordsStayCompact", [](TestContext& context) {
			context.Expect(sizeof(TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord) <= 64u, "Selected cluster records should stay compact enough for per-view payload uploads.");
			context.Expect(sizeof(TerraEngine::Renderer::VirtualGeometryPageNeedRecord) <= 80u, "Page need records should stay compact enough for streaming handoff snapshots.");
			context.ExpectEqual(static_cast<std::size_t>(48u), sizeof(MeshletCullingCountersGPU), "Meshlet GPU counters should include the Hi-Z diagnostic breakdown while remaining 16-byte aligned.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.SelectedClusterRecordsCarrySparseIdentity", [](TestContext& context) {
			using TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord;
			context.Expect(std::is_standard_layout_v<VirtualGeometrySelectedClusterRecord>, "Selected cluster records should stay standard-layout for future GPU upload buffers.");
			context.Expect(offsetof(VirtualGeometrySelectedClusterRecord, clusterId) < offsetof(VirtualGeometrySelectedClusterRecord, lodIndex), "Cluster id should be stored before LOD metadata for compact shader decoding.");
			context.Expect(offsetof(VirtualGeometrySelectedClusterRecord, pageId) < offsetof(VirtualGeometrySelectedClusterRecord, fallbackClusterId), "Page id and fallback cluster id should both be present, not replaced by a selected-cluster counter only.");
			context.Expect(offsetof(VirtualGeometrySelectedClusterRecord, usingFallback) > offsetof(VirtualGeometrySelectedClusterRecord, fallbackClusterId), "Fallback state should travel with each sparse cluster record.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.US1.PacketBatchMetadataStaysVisible", [](TestContext& context) {
			const std::string taskShader = ReadDefaultTaskShader();
			const std::string meshShader = ReadDefaultMeshShader();

			context.Expect(ShaderContains(taskShader, "selectedVirtualClusterCount; // Number of selected clusters in this packet batch draw."), "Task shader push constants should document that cluster count can describe a packet batch.");
			context.Expect(ShaderContains(taskShader, "this draw references one record or a packet run"), "Task shader should preserve the packet-run contract without changing meshlet output.");
			context.Expect(ShaderContains(meshShader, "packet-run ownership"), "Mesh shader debug propagation should name packet-run ownership.");
			context.Expect(ShaderContains(meshShader, "SceneUniforms.selectedVirtualClusterCount"), "Mesh shader should continue packing the selected packet count into debug data.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.FrameCullingDataCarriesHiZAvailabilityGate", [](TestContext& context) {
			context.ExpectEqual(offsetof(FrameCullingDataGPU, renderSettings1) + sizeof(glm::uvec4), offsetof(FrameCullingDataGPU, renderSettings2), "renderSettings2 should remain directly after renderSettings1 for the task shader payload.");
			context.ExpectEqual(sizeof(static_cast<FrameCullingDataGPU*>(nullptr)->renderSettings2), sizeof(glm::vec4), "renderSettings2 should remain a float4 so .w can gate Hi-Z without changing the shader layout.");
			context.Expect(offsetof(FrameCullingDataGPU, renderSettings2) < offsetof(FrameCullingDataGPU, globalTaskCullingEnabled), "The Hi-Z availability gate must remain in frame data before task-culling feature flags.");
			context.ExpectEqual(static_cast<std::size_t>(336u), sizeof(FrameCullingDataGPU), "Adding Hi-Z validation state should not change the task-shader frame-culling upload size.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.HiZUsesFullRectSampleBudget", [](TestContext& context) {
			const std::string shader = ReadDefaultTaskShader();
			context.Expect(!shader.empty(), "The default task shader should be readable for Hi-Z sampler contract validation.");
			context.Expect(ShaderContains(shader, "static const uint kMaxHiZRectAxisSamples = 64u;"), "The Hi-Z rectangle sampler should keep its bounded per-axis loop limit explicit.");
			context.Expect(ShaderContains(shader, "static const uint kMaxHiZRectTexelSamples = kMaxHiZRectAxisSamples * kMaxHiZRectAxisSamples;"), "The Hi-Z rectangle sampler should use the full axis budget so it can choose finer mips for normal wall occlusion cases.");
			context.Expect(!ShaderContains(shader, "static const uint kMaxHiZRectTexelSamples = 1024u;"), "The previous 1024-sample cap forced overly coarse Hi-Z mips for wider projected meshlet bounds.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.HiZProjectsActualMeshletVertices", [](TestContext& context) {
			const std::string shader = ReadDefaultTaskShader();
			context.Expect(!shader.empty(), "The default task shader should be readable for vertex-projected Hi-Z contract validation.");
			context.Expect(ShaderContains(shader, "[[vk::binding(0, 1)]] ByteAddressBuffer meshletVertexBytes;"), "The task shader should read packed meshlet vertices when building Hi-Z rectangles.");
			context.Expect(ShaderContains(shader, "[[vk::binding(1, 1)]] ByteAddressBuffer primitiveIndices;"), "The task shader should read primitive indices for triangle coverage votes.");
			context.Expect(ShaderContains(shader, "[[vk::binding(2, 1)]] StructuredBuffer<MeshletRecord> meshletRecords;"), "The task shader should read meshlet records for per-meshlet vertex ranges.");
			context.Expect(ShaderContains(shader, "[[vk::binding(3, 1)]] ByteAddressBuffer meshletVertexRemap;"), "The task shader should read the remap table so Hi-Z tests use the same decoded vertices as mesh emission.");
			context.Expect(ShaderContains(shader, "TryProjectMeshletVertex"), "Hi-Z culling should share one decoded vertex projection helper between rectangle and point-coverage tests.");
			context.Expect(ShaderContains(shader, "TryBuildProjectedMeshletRect"), "Hi-Z culling should build a projected rectangle from actual meshlet vertices instead of only from the bounds sphere.");
			context.Expect(ShaderContains(shader, "IsMeshletVertexCoverageOccluded"), "Small projected meshlets should have an all-vertex Hi-Z fallback when their rectangle contains unrelated background texels.");
			context.Expect(ShaderContains(shader, "static const uint kMaxCoverageTriangleSamples = 64u;"), "Triangle coverage votes should keep a bounded loop count for task-shader culling.");
			context.Expect(ShaderContains(shader, "static const float kMaxTriangleCoverageRectExtentPixels = 512.0f;"), "Triangle coverage votes should stay bounded to medium projected meshlets.");
			context.Expect(ShaderContains(shader, "static const float kMaxRectCoverageRectExtentPixels = 320.0f;"), "Coarse rectangle coverage votes should stay bounded to modest projected meshlets.");
			context.Expect(ShaderContains(shader, "static const float kMaxCenterCoverageRectExtentPixels = 128.0f;"), "Center-tap fallback should stay limited to small projected meshlets.");
			context.Expect(ShaderContains(shader, "const float2 uvPadding = 8.0f / viewportSize;"), "Hi-Z should pad projected meshlet rectangles enough that wall-edge silhouettes fail open.");
			context.Expect(ShaderContains(shader, "static const uint kRequiredRectCoverageSamples = kRectCoverageSampleCount;"), "Coarse rectangle coverage should require every coarse sample to be hidden before culling.");
			context.Expect(ShaderContains(shader, "Coverage-vote fallbacks are intentionally diagnostic-only for now."), "Hi-Z coverage-vote fallbacks should fail open until they have a stronger silhouette-safe proof.");
			context.Expect(!ShaderContains(shader, "if (IsMeshletRectCoverageMostlyOccluded"), "Coarse rectangle votes should not actively cull wall-edge silhouettes.");
			context.Expect(!ShaderContains(shader, "if (IsMeshletTriangleCoverageOccluded"), "Triangle centroid votes should not actively cull wall-edge silhouettes.");
			context.Expect(!ShaderContains(shader, "if (IsMeshletVertexCoverageOccluded"), "Vertex votes should not actively cull wall-edge silhouettes.");
			context.Expect(!ShaderContains(shader, "IsSmallMeshletCenterOccluded(hiZViewIndex"), "A single center tap should not cull visible wall-edge silhouettes.");
			context.Expect(ShaderContains(shader, "const uint requiredOccludedVertices = vertexCount;"), "Vertex fallback should require every projected vertex to be hidden before culling.");
			context.Expect(ShaderContains(shader, "const uint requiredOccludedTriangles = sampledTriangleCount;"), "Triangle centroid sampling should require every sampled triangle to be hidden before culling.");
			context.Expect(ShaderContains(shader, "static const float kMaxVertexCoverageRectExtentPixels = 160.0f;"), "The all-vertex fallback should stay limited to modest projected meshlets to avoid silhouette popping.");
			context.Expect(!ShaderContains(shader, "TryGetHiZProjectionScale"), "The old sphere-projection scale path should stay removed from task-shader Hi-Z culling.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.HiZProjectedBoundsContractIsConservative", [](TestContext& context) {
			VulkanHiZProjectedBoundsInput valid{};
			valid.pyramidReady = true;
			valid.clipW = 4.0f;
			valid.uvCenter = glm::vec2(0.5f, 0.5f);
			valid.uvRadius = glm::vec2(0.04f, 0.04f);
			valid.viewportSize = glm::uvec2(1280u, 720u);
			const VulkanHiZProjectedBoundsResult validResult = ValidateVulkanHiZProjectedBounds(valid);
			context.Expect(validResult.sampleAllowed, "The CPU contract should agree with the task shader that fully contained projected bounds may sample Hi-Z.");

			VulkanHiZProjectedBoundsInput edge = valid;
			edge.uvCenter = glm::vec2(0.01f, 0.5f);
			edge.uvRadius = glm::vec2(0.04f, 0.04f);
			const VulkanHiZProjectedBoundsResult edgeResult = ValidateVulkanHiZProjectedBounds(edge);
			context.Expect(!edgeResult.sampleAllowed, "Projected bounds that cross the view edge should fail open before task-shader Hi-Z sampling.");
			context.Expect(edgeResult.failOpen, "Rejected projected bounds should be visible fallback, not hidden geometry.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.UserFacingVisualizerModesMatchFragmentShaderConstants", [](TestContext& context) {
			const std::string shader = ReadDefaultFragmentShader();
			context.Expect(!shader.empty(), "The default fragment shader should be readable for visualizer contract validation.");

			struct ModeConstant
			{
				VirtualGeometryDebugVisualizationMode mode = VirtualGeometryDebugVisualizationMode::None;
				const char* constantName = "";
				const char* expectedValue = "";
			};

			const std::array<ModeConstant, 8u> constants = {
				ModeConstant{ VirtualGeometryDebugVisualizationMode::LOD, "kVirtualGeometryDebugModeLOD", "1u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::MeshletId, "kVirtualGeometryDebugModeMeshletId", "2u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::Bounds, "kVirtualGeometryDebugModeBounds", "3u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::Culling, "kVirtualGeometryDebugModeCulling", "4u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::HiZ, "kVirtualGeometryDebugModeHiZ", "5u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::Residency, "kVirtualGeometryDebugModeResidency", "6u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::Error, "kVirtualGeometryDebugModeError", "7u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::Cluster, "kVirtualGeometryDebugModeCluster", "8u" }
			};

			for (const ModeConstant& constant : constants)
			{
				const std::string declaration =
					std::string("static const uint ") + constant.constantName + " = " + constant.expectedValue + ";";
				const std::string branch =
					std::string("if (debugMode == ") + constant.constantName + ")";
				context.Expect(ShaderContains(shader, declaration), "Each exposed virtual geometry visualizer mode should keep its numeric HLSL constant aligned with the C++ enum.");
				context.Expect(ShaderContains(shader, branch), "Each exposed virtual geometry visualizer mode should have a fragment shader branch.");
				context.Expect(IsVirtualGeometryDebugVisualizationModeExposed(constant.mode), "The shader contract table should only list public editor-facing modes.");
			}

			context.Expect(!ShaderContains(shader, "kVirtualGeometryDebugModeBoundaries"), "Removed internal visualizer modes should not leave public shader constants behind.");
			context.Expect(!ShaderContains(shader, "kVirtualGeometryDebugModeScale"), "Removed internal visualizer modes should not leave public shader constants behind.");
			context.Expect(!ShaderContains(shader, "kVirtualGeometryDebugModeFallback"), "Removed internal visualizer modes should not leave public shader constants behind.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.FalseColorBranchesMatchPublishedLegend", [](TestContext& context) {
			const std::string shader = ReadDefaultFragmentShader();
			context.Expect(ShaderContains(shader, "BuildVirtualGeometryHashColor(selectedLod + 17u)"), "LOD mode should use a stable false color from the selected detail level.");
			context.Expect(ShaderContains(shader, "BuildVirtualGeometryHashColor(input.meshletIndex + 31u)"), "Meshlet Id mode should use a stable false color from the rendered meshlet id.");
			context.Expect(ShaderContains(shader, "lerp(float3(0.05f, 0.25f, 1.0f), float3(1.0f, 0.9f, 0.05f), radiusBand)"), "Bounds mode should keep the blue-to-yellow size ramp documented in the legend.");
			context.Expect(ShaderContains(shader, "return hiZTrusted ? float3(0.05f, 0.8f, 1.0f) : float3(0.1f, 0.85f, 0.25f);"), "Culling mode should distinguish drawn meshlets with and without trusted Hi-Z.");
			context.Expect(ShaderContains(shader, "return hiZTrusted ? float3(0.05f, 0.8f, 1.0f) : float3(0.35f, 0.35f, 0.35f);"), "Hi-Z mode should keep cyan active and gray missing-pyramid states.");
			context.Expect(ShaderContains(shader, "missingPageCount > 0u ? float3(1.0f, 0.55f, 0.05f) : float3(0.0f, 0.75f, 0.45f)"), "Residency mode should keep orange requested/missing and green resident states.");
			context.Expect(ShaderContains(shader, "missingPageCount > 0u ? float3(1.0f, 0.55f, 0.05f) : float3(0.2f, 0.9f, 0.9f)"), "Error mode should keep orange missing-page pressure and cyan clean state.");
			context.Expect(ShaderContains(shader, "BuildVirtualGeometryHashColor(clusterSeed + 73u)"), "Cluster mode should use a stable false color from selected cluster identity.");
		});
	}
}
