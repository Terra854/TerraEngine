// VulkanVirtualGeometryProductionDecodeTests.cpp - pure production-decode contracts.

#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/RHI/Vulkan/VulkanVirtualGeometryExecutor.h"

#include <array>

namespace TerraEngine::Tests
{
	void RegisterVulkanVirtualGeometryProductionDecodeTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Vulkan.DirectMeshletBatchesSuppressExecutorWork",
			[](TestContext& context)
			{
				Renderer::DirectMeshletBatch batch{};
				batch.geometryIdentity = 41u;
				batch.geometryGeneration = 7u;
				batch.materialIdentity = 19u;
				batch.meshletCount = 65u;
				batch.instanceIdentities = { 100u, 101u, 102u };
				const auto plan = Renderer::Vulkan::BuildVulkanDirectMeshletBatchPlan({ batch }, true, 32u);
				context.Expect(plan.valid && plan.draws.size() == 1u,
					"A stable direct batch should lower to the existing Vulkan mesh-task indirect path.");
				context.Expect(plan.draws.front().taskGroupCount == 3u && plan.draws.front().instanceCount == 3u,
					"Direct batching should share geometry while retaining every instance identity.");
				context.Expect(plan.suppressedVirtualGeometryExecutors == 1u && plan.suppressedPageRequests == 1u,
					"Direct lowering must explicitly suppress VG executor allocation and feedback.");
			});

		registry.Add("VirtualGeometry.Vulkan.ProductionDecodeRejectsUnsupportedProfilesAndValidatesParity",
			[](TestContext& context)
			{
				Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeRequest unsupported{};
				unsupported.profileId = 2u; // Meshoptimizer remains CPU-only until a real kernel passes parity.
				unsupported.codecVersion = 1u;
				unsupported.requestIdentity = 9u;
				const auto unavailable = Renderer::Vulkan::BuildVulkanVirtualGeometryProductionDecodePlan(
					unsupported, 3u, 11u, {}, {});
				context.Expect(!unavailable.valid && !unavailable.supportedProfile,
					"Unsupported meshoptimizer profiles must remain truthfully unavailable to the Vulkan decode path.");

				const std::array<std::uint8_t, 4u> referenceBytes{ 3u, 5u, 7u, 11u };
				Renderer::Vulkan::VulkanVirtualGeometryProductionDecodePlan complete{};
				complete.valid = true;
				complete.generation = 12u;
				complete.expectedDecodedHash = CalculateVirtualGeometryCompactHash(referenceBytes);
				complete.request.requestIdentity = 77u;
				complete.request.decodedByteCount = static_cast<std::uint32_t>(referenceBytes.size());
				Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeResult result{ 77u, 1u,
					static_cast<std::uint32_t>(referenceBytes.size()), 0u };
				const auto parity = Renderer::Vulkan::VerifyVulkanVirtualGeometryProductionDecodeParity(
					complete, result, referenceBytes);
				context.Expect(parity.deviceDecodeAvailable && parity.acceptedForPublication,
					"Only a completed byte-count and hash match may expose device decode capability.");

				result.requestIdentity = 78u;
				const auto rejected = Renderer::Vulkan::VerifyVulkanVirtualGeometryProductionDecodeParity(
					complete, result, referenceBytes);
				context.Expect(!rejected.deviceDecodeAvailable && !rejected.acceptedForPublication,
					"A mismatched request identity must retain CPU fallback rather than publish a partial device result.");
			});
	}
}
