// RenderInstanceAbiTests.cpp - renderer instance-stream contract checks.

#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace TerraEngine::Tests {

	namespace {

		std::string ReadShader(const char* relativePath)
		{
			const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path()
				.parent_path().parent_path().parent_path() / relativePath;
			std::ifstream stream(path, std::ios::binary);
			return stream ? std::string(std::istreambuf_iterator<char>(stream), {}) : std::string{};
		}

		void MakeGroupable(TerraEngine::Renderer::RenderFrameSnapshot& snapshot)
		{
			const TerraEngine::UUID meshId = snapshot.renderables.front().meshId;
			const TerraEngine::UUID materialId = snapshot.renderables.front().materialId;
			for (auto& renderable : snapshot.renderables)
			{
				renderable.meshId = meshId;
				renderable.materialId = materialId;
				renderable.invalidation.asset = 11u;
				renderable.invalidation.material = 12u;
				renderable.invalidation.renderer = 13u;
				renderable.invalidation.residency = 14u;
				renderable.geometryRouteDecision.selected = TerraEngine::Renderer::GeometryRoute::DirectMeshlet;
			}
		}

		// Mirrors the word addressing in RenderInstanceAbi.hlsli. GLM exposes
		// column-major elements as matrix[column][row], whereas the HLSL index
		// used by mul(matrix, vector) selects a logical row.
		std::array<glm::vec4, 4u> ReconstructHlslRows(const glm::mat4& matrix)
		{
			std::array<glm::vec4, 4u> rows{};
			for (std::uint32_t row = 0u; row < rows.size(); ++row)
			{
				rows[row] = glm::vec4(matrix[0u][row], matrix[1u][row],
					matrix[2u][row], matrix[3u][row]);
			}
			return rows;
		}

		// Applies the same row-vector dot products as HLSL mul(float4x4, float4)
		// so the ABI regression checks the resulting affine transform, not merely
		// a set of source-code tokens.
		glm::vec4 MultiplyHlslRows(const std::array<glm::vec4, 4u>& rows, const glm::vec4& vector)
		{
			return glm::vec4(glm::dot(rows[0u], vector), glm::dot(rows[1u], vector),
				glm::dot(rows[2u], vector), glm::dot(rows[3u], vector));
		}

	}

	void RegisterRenderInstanceAbiTests(TestRegistry& registry)
	{
		registry.Add("Renderer.InstanceAbi.CpuLayoutMatchesShaderContract", [](TestContext& context) {
			using RenderInstanceRecord = ::RenderInstanceRecord;
			context.ExpectEqual<std::size_t>(176u, sizeof(RenderInstanceRecord),
				"The instance stream must retain the documented byte stride.");
			context.ExpectEqual<std::size_t>(64u, offsetof(RenderInstanceRecord, editorTint),
				"Editor tint must follow the transform at the shader ABI offset.");
			const std::string shader = ReadShader("Engine/Shaders/Default/RenderInstanceAbi.hlsli");
			context.Expect(shader.find("kRenderInstanceRecordBytes = 176u") != std::string::npos,
				"The shader must declare the C++ instance stride.");
			context.Expect(shader.find("instanceIndex >= instanceCount") != std::string::npos,
				"The shader must reject an out-of-range instance selector.");
		});

		registry.Add("Renderer.InstanceAbi.ReconstructsColumnMajorMatrixForHlslRows", [](TestContext& context) {
			// Deliberately use distinct scale and translation values. A transpose
			// moves translation into the homogeneous row and makes this exact affine
			// result fail with the radial/projective distortion seen at runtime.
			glm::mat4 cpuMatrix(1.0f);
			cpuMatrix[0u] = glm::vec4(2.0f, 0.0f, 0.0f, 0.0f);
			cpuMatrix[1u] = glm::vec4(0.0f, 3.0f, 0.0f, 0.0f);
			cpuMatrix[2u] = glm::vec4(0.0f, 0.0f, 4.0f, 0.0f);
			cpuMatrix[3u] = glm::vec4(13.0f, -7.0f, 5.0f, 1.0f);
			const glm::vec4 localPoint(1.0f, 2.0f, -3.0f, 1.0f);
			const glm::vec4 expected = cpuMatrix * localPoint;
			const glm::vec4 decoded = MultiplyHlslRows(ReconstructHlslRows(cpuMatrix), localPoint);
			context.ExpectEqual(expected.x, decoded.x,
				"The first HLSL row must combine the CPU X basis and translation lane.");
			context.ExpectEqual(expected.y, decoded.y,
				"The second HLSL row must combine the CPU Y basis and translation lane.");
			context.ExpectEqual(expected.z, decoded.z,
				"The third HLSL row must combine the CPU Z basis and translation lane.");
			context.ExpectEqual(expected.w, decoded.w,
				"The reconstructed homogeneous row must keep an affine w of one.");
			const std::string shader = ReadShader("Engine/Shaders/Default/RenderInstanceAbi.hlsli");
			context.Expect(shader.find("for (uint row = 0u; row < 4u; ++row)") != std::string::npos
				&& shader.find("instanceWords[wordOffset + 0u + row]") != std::string::npos
				&& shader.find("instanceWords[wordOffset + 4u + row]") != std::string::npos
				&& shader.find("instanceWords[wordOffset + 8u + row]") != std::string::npos
				&& shader.find("instanceWords[wordOffset + 12u + row]") != std::string::npos,
				"The shader must reconstruct HLSL rows from the four contiguous GLM columns.");
		});

		registry.Add("Renderer.SharedSubmission.KeepsIndependentInstanceSelectors", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(40u, 2u);
			MakeGroupable(snapshot);
			snapshot.renderables[0].modelMatrix[3].x = 3.0f;
			snapshot.renderables[1].modelMatrix[3].x = 9.0f;
			snapshot.renderables[0].virtualGeometry.selectedLodIndex = 1u;
			snapshot.renderables[1].virtualGeometry.selectedLodIndex = 4u;
			const auto topology = TerraEngine::Renderer::BuildPersistentRenderTopology(snapshot, 77u);
			TerraEngine::Renderer::RenderFrameViewRequest view{};
			view.viewGeneration = 31u;
			const auto dynamic = TerraEngine::Renderer::BuildDynamicRenderFrameState(snapshot, topology, view, 40u);
			context.Expect(dynamic.valid, "A complete immutable snapshot should build a dynamic sidecar.");
			context.ExpectEqual<std::size_t>(1u, dynamic.groups.size(),
				"Compatible models should form one executable range.");
			context.ExpectEqual<std::size_t>(2u, dynamic.instances.size(),
				"Each grouped owner needs an independent instance record.");
			context.Expect(dynamic.instances[0].modelMatrix[3].x != dynamic.instances[1].modelMatrix[3].x,
				"Grouping must preserve independent transforms.");
			context.Expect(dynamic.instances[0].geometrySelectors.x != dynamic.instances[1].geometrySelectors.x,
				"Grouping must preserve independently selected LOD values.");
			context.Expect(dynamic.instances[0].objectIdentity.w != dynamic.instances[1].objectIdentity.w,
				"Grouping must preserve unique picking and diagnostic identities.");
		});

		registry.Add("Renderer.SharedSubmission.IsolatesIncompleteMember", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(41u, 3u);
			MakeGroupable(snapshot);
			snapshot.renderables[1].invalidation.residency = 0u;
			const auto topology = TerraEngine::Renderer::BuildPersistentRenderTopology(snapshot, 78u);
			const auto dynamic = TerraEngine::Renderer::BuildDynamicRenderFrameState(snapshot, topology, {}, 41u);
			context.ExpectEqual<std::size_t>(1u, dynamic.groups.size(),
				"The compatible peers should still receive one group.");
			context.ExpectEqual<std::size_t>(1u, dynamic.fallbackCommands.size(),
				"Only the incomplete key may take the established fallback path.");
			context.ExpectEqual(TerraEngine::ToHexString(snapshot.renderables[1].objectId),
				TerraEngine::ToHexString(dynamic.fallbackCommands.front().objectId),
				"Fallback work must retain the rejected owner's identity.");
		});

		registry.Add("Renderer.SharedSubmission.RejectsPartialStreamPublication", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(42u, 2u);
			MakeGroupable(snapshot);
			const auto topology = TerraEngine::Renderer::BuildPersistentRenderTopology(snapshot, 79u);
			const auto dynamic = TerraEngine::Renderer::BuildDynamicRenderFrameState(snapshot, topology, {}, 42u);
			const auto accepted = TerraEngine::Renderer::Vulkan::BuildGroupedInstanceStreamPlan(dynamic, 2u, 13u);
			context.Expect(accepted.valid, "The complete group range should be publishable.");
			context.ExpectEqual<std::size_t>(1u, accepted.avoidedSubmissionCount,
				"Two compatible instances should replace one comparable submission.");
			const auto rejected = TerraEngine::Renderer::Vulkan::BuildGroupedInstanceStreamPlan(dynamic, 1u, 13u);
			context.Expect(!rejected.valid && rejected.requiresCompleteFallback,
				"Capacity failure must reject the entire optimized stream before publication.");
		});

		registry.Add("Renderer.SharedSubmission.AnimatedMeshShaderUsesPerInstanceBridge", [](TestContext& context) {
			const std::string meshShader = ReadShader("Engine/Shaders/Default/Default.mesh.hlsl");
			const std::string taskShader = ReadShader("Engine/Shaders/Default/Default.task.hlsl");
			context.Expect(meshShader.find("TryLoadGpuAnimationGraphicsRecordAt") != std::string::npos,
				"Animated grouped meshlets must resolve a record-specific graphics bridge.");
			context.Expect(meshShader.find("instanceRecord.animationSelectors.x") != std::string::npos,
				"The mesh stage must consume the stream's per-instance bridge buffer selector.");
			context.Expect(taskShader.find("instanceIndices[kTaskBatchSize]") != std::string::npos,
				"Task payloads must carry the owning instance selector to mesh workgroups.");
			context.Expect(taskShader.find("!groupedInstances") != std::string::npos,
				"Grouped work must keep task culling disabled while the model lane carries its stream locator.");
		});

		registry.Add("Renderer.SharedSubmission.VirtualGeometryLoadsExactPerInstancePayload", [](TestContext& context) {
			const std::string renderer = ReadShader("Engine/Renderer/RenderSystem.cpp");
			const std::string taskShader = ReadShader("Engine/Shaders/Default/Default.task.hlsl");
			context.Expect(renderer.find("virtualGeometryPacketDescriptorIndex == b.virtualGeometryPacketDescriptorIndex") == std::string::npos,
				"Dense VG grouping should not split compatible instances merely because their packet descriptors differ.");
			context.Expect(renderer.find("virtualGeometryPagePayloadByteOffset == b.virtualGeometryPagePayloadByteOffset") == std::string::npos,
				"Page-local byte ranges belong to each instance record rather than the shared group key.");
			context.Expect(renderer.find("virtualGeometryPayloadSource == b.virtualGeometryPayloadSource") != std::string::npos,
				"Fallback/resident payload-source transitions must remain isolated before grouping.");
			context.Expect(taskShader.find("groupedInstanceRecord.geometrySelectors.y") != std::string::npos
				&& taskShader.find("groupedInstanceRecord.virtualGeometryPayloadSelectors.x") != std::string::npos,
				"The task stage must resolve packet and payload selectors from the exact grouped member record.");
		});
	}

}
