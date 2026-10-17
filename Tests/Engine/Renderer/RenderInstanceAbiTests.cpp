// RenderInstanceAbiTests.cpp - renderer instance-stream contract checks.

#include "TestHarness.h"
#include <string_view>
#include <algorithm>
#include "VulkanSourceTestUtils.h"
#include "RendererThreadingTestFixtures.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace TerraEngine::Tests {

	namespace {
		// Required reads and lexical definition bounds fail before any token or
		// ordering assertion can accidentally accept an empty/missing region.
		std::string RequiredSource(TestContext& context, const char* path)
		{
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / path, std::ios::binary);
			context.Expect(input.good(), std::string("Required source is unreadable: ") + path);
			std::string text((std::istreambuf_iterator<char>(input)), {});
			context.Expect(!text.empty(), std::string("Required source is empty: ") + path);
			text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
			return text;
		}
		std::string RequiredDefinition(TestContext& context, const char* path, std::string_view signature)
		{
			const auto text = RequiredSource(context, path);
			const auto start = text.find(signature);
			context.Expect(start != std::string::npos, std::string("Required definition is absent: ") + std::string(signature));
			const auto open = text.find('{', start + signature.size());
			context.Expect(open != std::string::npos, "Required definition has no opening body.");
			std::size_t depth = 0;
			bool lineComment = false, blockComment = false, quoted = false, character = false, escape = false;
			for(std::size_t index = open; index < text.size(); ++index)
			{
				const char value = text[index], next = index + 1 < text.size() ? text[index + 1] : '\0';
				if(lineComment) { if(value == '\n') lineComment = false; continue; }
				if(blockComment) { if(value == '*' && next == '/') { blockComment = false; ++index; } continue; }
				if(quoted || character) {
					if(escape) { escape = false; continue; }
					if(value == '\\') { escape = true; continue; }
					if((quoted && value == '"') || (character && value == '\'')) quoted = character = false;
					continue;
				}
				if(value == '/' && next == '/') { lineComment = true; ++index; continue; }
				if(value == '/' && next == '*') { blockComment = true; ++index; continue; }
				if(value == '"') { quoted = true; continue; }
				if(value == '\'') { character = true; continue; }
				if(value == '{') ++depth;
				if(value == '}' && --depth == 0) return text.substr(start, index + 1 - start);
			}
			context.Expect(false, "Required definition has no balanced closing body.");
			return {};
		}

		std::string RequiredVulkanContext(TestContext& context)
		{
			for(const char* file : {"VulkanContext.cpp", "VulkanContextSource.h", "VulkanBuiltinPasses.cpp", "VulkanEditorRendering.cpp", "VulkanFrameRendering.cpp", "VulkanFrameSubmission.cpp", "VulkanPostProcessing.cpp", "VulkanReadback.cpp"})
				RequiredSource(context, (std::string("Engine/Renderer/RHI/Vulkan/") + file).c_str());
			return VulkanSourceContracts::ReadContextSources();
		}
		std::string RequiredVulkanResources(TestContext& context)
		{
			for(const char* file : {"VulkanContextResources.cpp", "VulkanResourceSource.h", "VulkanAnimationResources.cpp", "VulkanVirtualGeometryResources.cpp", "VulkanPipelineResources.cpp", "VulkanGlobalResources.cpp", "VulkanLightingResources.cpp", "VulkanGpuDiagnostics.cpp"})
				RequiredSource(context, (std::string("Engine/Renderer/RHI/Vulkan/") + file).c_str());
			return VulkanSourceContracts::ReadResourceSources();
		}
		bool SourceBefore(const std::string& text, std::string_view first, std::string_view second)
		{
			const auto a = text.find(first), b = text.find(second);
			return a != std::string::npos && b != std::string::npos && a < b;
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
			const std::string shader = RequiredSource(context, "Engine/Shaders/Default/RenderInstanceAbi.hlsli");
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
			const std::string shader = RequiredSource(context, "Engine/Shaders/Default/RenderInstanceAbi.hlsli");
			context.Expect(shader.find("for (uint row = 0u; row < 4u; ++row)") != std::string::npos
				&& shader.find("instanceWords[wordOffset + 0u + row]") != std::string::npos
				&& shader.find("instanceWords[wordOffset + 4u + row]") != std::string::npos
				&& shader.find("instanceWords[wordOffset + 8u + row]") != std::string::npos
				&& shader.find("instanceWords[wordOffset + 12u + row]") != std::string::npos,
				"The shader must reconstruct HLSL rows from the four contiguous GLM columns.");
		});

		registry.Add("Renderer.InstanceAbi.PacksExactGroupedCompatibilityPolicy", [](TestContext& context) {
			using RenderInstanceRecord = ::RenderInstanceRecord;
			const std::uint32_t packed = RenderInstanceRecord::PackCompatibilityPolicy(
				true, 7u, true);
			context.Expect((packed & RenderInstanceRecord::kTaskCullingPolicyBit) != 0u,
				"The compact proof must preserve task-culling policy.");
			context.Expect((packed & RenderInstanceRecord::kConservativeVisibilityPolicyBit) != 0u,
				"The compact proof must preserve conservative visibility policy.");
			context.ExpectEqual<std::uint32_t>(7u,
				(packed >> RenderInstanceRecord::kAnimationSourcePolicyShift)
					& RenderInstanceRecord::kAnimationSourcePolicyMask,
				"The compact proof must preserve the exact animation source.");
			const std::string backend = RequiredVulkanContext(context);
			context.Expect(backend.find("groupedRecordMatchesRepresentative")
					!= std::string::npos
				&& backend.find("commandResolver.Resolve(memberCommandIndex, member)")
					== std::string::npos,
				"Grouped validation must inspect sealed compact lanes instead of rebuilding every command.");
		});

		registry.Add("Renderer.SharedSubmission.KeepsIndependentInstanceSelectors", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(40u, 2u);
			MakeGroupable(snapshot);
			// Dynamic rendering localizes this canonical pose into the view frame;
			// the cached float matrix is deliberately not a world-space input.
			snapshot.renderables[0].canonicalWorldPose.position.offsetMetres.x = 3.0;
			snapshot.renderables[1].canonicalWorldPose.position.offsetMetres.x = 9.0;
			snapshot.renderables[0].virtualGeometry.selectedLodIndex = 1u;
			snapshot.renderables[1].virtualGeometry.selectedLodIndex = 4u;
			const auto topology = TerraEngine::Renderer::BuildPersistentRenderTopology(snapshot, 77u);
			TerraEngine::Renderer::RenderFrameViewRequest view{};
			view.outputId = 1u;
			view.viewGeneration = 31u;
			view.referenceFrame = MakeRendererTestReferenceFrame(40u, 1u, view.outputId);
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
			TerraEngine::Renderer::RenderFrameViewRequest view{};
			view.outputId = 1u;
			view.viewGeneration = 41u;
			view.referenceFrame = MakeRendererTestReferenceFrame(41u, 1u, view.outputId);
			const auto dynamic = TerraEngine::Renderer::BuildDynamicRenderFrameState(snapshot, topology, view, 41u);
			context.ExpectEqual<std::size_t>(1u, dynamic.groups.size(),
				"The compatible peers should still receive one group.");
			context.ExpectEqual<std::size_t>(1u, dynamic.singletonOrdinals.size(),
				"Only the incomplete key may take the established fallback path.");
			context.ExpectEqual<std::uint32_t>(1u, dynamic.singletonOrdinals.front(),
				"Fallback work must retain the rejected owner's stable ordinal.");
		});

		registry.Add("Renderer.SharedSubmission.RejectsPartialStreamPublication", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(42u, 2u);
			MakeGroupable(snapshot);
			const auto topology = TerraEngine::Renderer::BuildPersistentRenderTopology(snapshot, 79u);
			TerraEngine::Renderer::RenderFrameViewRequest view{};
			view.outputId = 1u;
			view.viewGeneration = 42u;
			view.referenceFrame = MakeRendererTestReferenceFrame(42u, 1u, view.outputId);
			const auto dynamic = TerraEngine::Renderer::BuildDynamicRenderFrameState(snapshot, topology, view, 42u);
			const auto accepted = TerraEngine::Renderer::Vulkan::BuildGroupedInstanceStreamPlan(dynamic, 2u, 13u);
			context.Expect(accepted.valid, "The complete group range should be publishable.");
			context.ExpectEqual<std::size_t>(1u, accepted.avoidedSubmissionCount,
				"Two compatible instances should replace one comparable submission.");
			const auto rejected = TerraEngine::Renderer::Vulkan::BuildGroupedInstanceStreamPlan(dynamic, 1u, 13u);
			context.Expect(!rejected.valid && rejected.requiresCompleteFallback,
				"Capacity failure must reject the entire optimized stream before publication.");
		});

		registry.Add("Renderer.SharedSubmission.UploadsOnlyDirtyInstanceRanges", [](TestContext& context) {
			using RenderInstanceRecord = ::RenderInstanceRecord;
			std::array<RenderInstanceRecord, 6u> previous{};
			std::array<RenderInstanceRecord, 6u> current = previous;
			const auto unchanged = TerraEngine::Renderer::Vulkan::
				BuildGroupedInstanceDirtyPlan(previous, current, false);
			context.Expect(unchanged.ranges.empty() && unchanged.dirtyRecordCount == 0u,
				"A fence-proven unchanged frame slot should publish no host writes.");

			current[1u].modelMatrix[3u].x = 4.0f;
			current[2u].editorTint.y = 0.5f;
			current[5u].geometrySelectors.z = 9u;
			const auto sparse = TerraEngine::Renderer::Vulkan::
				BuildGroupedInstanceDirtyPlan(previous, current, false);
			context.ExpectEqual<std::size_t>(2u, sparse.ranges.size(),
				"Adjacent dirty records should coalesce into one mapped byte range.");
			context.ExpectEqual<std::size_t>(3u, sparse.dirtyRecordCount,
				"Only records with GPU-visible changes should be uploaded.");
			context.ExpectEqual<std::size_t>(1u, sparse.ranges[0u].firstRecord,
				"The first sparse range should retain its exact stream offset.");
			context.ExpectEqual<std::size_t>(2u, sparse.ranges[0u].recordCount,
				"The first sparse range should cover both adjacent records.");

			const auto replaced = TerraEngine::Renderer::Vulkan::
				BuildGroupedInstanceDirtyPlan(previous, current, true);
			context.Expect(replaced.completeReplacement
				&& replaced.ranges.size() == 1u
				&& replaced.dirtyRecordCount == current.size(),
				"A replaced Vulkan allocation must receive one complete initialized stream.");
		});

		registry.Add("Renderer.SharedSubmission.AnimatedMeshShaderUsesPerInstanceBridge", [](TestContext& context) {
			const std::string meshShader = RequiredSource(context, "Engine/Shaders/Default/Default.mesh.hlsl");
			const std::string taskShader = RequiredSource(context, "Engine/Shaders/Default/Default.task.hlsl");
			context.Expect(meshShader.find("TryLoadGpuAnimationGraphicsRecordAt") != std::string::npos,
				"Animated grouped meshlets must resolve a record-specific graphics bridge.");
			context.Expect(meshShader.find("instanceRecord.animationSelectors.x") != std::string::npos,
				"The mesh stage must consume the stream's per-instance bridge buffer selector.");
			context.Expect(meshShader.find("kRenderInstanceSharedAnimationBufferFlag")
					!= std::string::npos
				&& meshShader.find("? SceneUniforms.meshDataIndex")
					!= std::string::npos
				&& taskShader.find("groupedSharedAnimationBuffer")
					!= std::string::npos,
				"Dense grouped animation must publish one shared buffer selector while retaining each packet index.");
			context.Expect(taskShader.find("instanceIndices[kTaskBatchSize]") != std::string::npos,
				"Task payloads must carry the owning instance selector to mesh workgroups.");
			context.Expect(taskShader.find("!groupedInstances") != std::string::npos,
				"Grouped work must keep task culling disabled while the model lane carries its stream locator.");
		});

		registry.Add("Renderer.SharedSubmission.VirtualGeometryLoadsExactPerInstancePayload", [](TestContext& context) {
			const auto renderer = RequiredDefinition(context, "Engine/Renderer/Frame/RenderViewGrouping.cpp", "RenderSystem::PrepareFrameGroupedSubmissions(");
			const std::string taskShader = RequiredSource(context, "Engine/Shaders/Default/Default.task.hlsl");
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
