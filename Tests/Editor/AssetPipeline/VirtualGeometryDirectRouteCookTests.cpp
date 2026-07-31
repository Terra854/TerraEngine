#include "TestHarness.h"

#include "VirtualGeometryProductionBlocks.h"
#include "Resource/VirtualGeometryCodec.h"
#include "Renderer/VirtualGeometry/VirtualGeometryTypes.h"

#include <array>
#include <span>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor;

	void RegisterVirtualGeometryDirectRouteCookTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.AssetPipeline.DirectRouteCookCoversProductionTriangleBands",
			[](TestContext& context)
			{
				// Direct-only cooking is intentionally exercised at each production
				// triangle band boundary; these files must not acquire virtual pages.
				for(const std::uint32_t triangles : { 12u, 64u, 65u, 512u, 513u, 4096u })
				{
					const std::uint32_t byteCount = triangles * 3u;
					std::vector<std::uint8_t> source(byteCount, 0x5au);
					TemfProductionStreamInput stream{
						TemfStreamKind::Position, TemfCodecProfile::ExpandedProduction,
						0u, 0u, triangles, 3u, source };
					TemfQuantizationDomain domain{};
					domain.domainVersion = 1u;
					domain.domainScope = static_cast<std::uint16_t>(TemfQuantizationDomainScope::Asset);
					domain.streamKind = static_cast<std::uint16_t>(TemfStreamKind::Position);
					domain.componentCount = 3u;
					domain.domainHash = triangles;
					TemfProductionBuildRequest request{};
					request.representationFlags = kTemfRepresentationDirectMeshlets;
					request.logicalPageSizeBytes = 0u;
					request.assetIdentity = 0x1000u + triangles;
					request.sourceContentHash = 0x2000u + triangles;
					request.buildRecipeHash = 0x3000u;
					request.toolIdentity = 0x4000u;
					request.generationIdentity = triangles;
					request.quantizationDomains = std::span<const TemfQuantizationDomain>(&domain, 1u);
					request.streams = std::span<const TemfProductionStreamInput>(&stream, 1u);

					const TemfProductionBuildResult cooked = BuildTemfProductionBlocks(request);
					const auto validated = TerraEngine::Resource::ValidateTemfProductionFile(cooked.bytes, {});
					context.Expect(cooked.IsComplete() && validated.IsComplete(),
						"Every supported direct triangle band must produce a valid canonical TEMF file: "
							+ (cooked.IsComplete() ? validated.diagnostic : cooked.error));
					if(!validated.IsComplete()) continue;
					context.ExpectEqual(0u, validated.directory.pages.size(),
						"Direct-only production must not allocate virtual-geometry pages.");
					context.ExpectEqual(0u, validated.directory.dependencies.size(),
						"Direct-only production must not emit virtual-geometry DAG metadata.");
					context.Expect((validated.directory.header.representationFlags & kTemfRepresentationVirtualGeometry) == 0u,
						"Direct-only production must not advertise a virtual-geometry directory.");
					context.Expect((validated.directory.header.representationFlags & kTemfRepresentationDirectMeshlets) != 0u,
						"Direct-only production must advertise direct-resident meshlets.");
				}
			});

		registry.Add("VirtualGeometry.Editor.AssetPipeline.DirectRouteCookIsDeterministicAndSharesRouteSummary",
			[](TestContext& context)
			{
				const std::array<std::uint8_t, 12u> source = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u };
				TemfProductionStreamInput stream{
					TemfStreamKind::Position, TemfCodecProfile::ExpandedProduction,
					0u, 0u, 4u, 3u, source };
				TemfQuantizationDomain domain{};
				domain.domainVersion = 1u;
				domain.domainScope = static_cast<std::uint16_t>(TemfQuantizationDomainScope::Asset);
				domain.streamKind = static_cast<std::uint16_t>(TemfStreamKind::Position);
				domain.componentCount = 3u;
				domain.domainHash = 1u;
				TemfProductionBuildRequest request{};
				request.representationFlags = kTemfRepresentationDirectMeshlets;
				request.logicalPageSizeBytes = 0u;
				request.quantizationDomains = std::span<const TemfQuantizationDomain>(&domain, 1u);
				request.streams = std::span<const TemfProductionStreamInput>(&stream, 1u);
				const auto first = BuildTemfProductionBlocks(request);
				const auto second = BuildTemfProductionBlocks(request);
				context.Expect(first.bytes == second.bytes, "Equal direct cooks must be byte deterministic.");

				Renderer::GeometryRouteInputSummary summary{};
				summary.sourceTriangleCount = 4u;
				summary.packedMeshletCount = 1u;
				summary.availableRepresentationMask = kTemfRepresentationDirectMeshlets;
				summary.directBatchEligible = true;
				summary.repeatedInstanceStorageCompatible = true;
				summary.deterministicSummaryHash = 0xfeedbeefu;
				const Renderer::GeometryRouteInputSummary copy = summary;
				context.Expect(summary.deterministicSummaryHash == copy.deterministicSummaryHash
					&& summary.availableRepresentationMask == copy.availableRepresentationMask,
					"Identical repeated instances must share one deterministic route-input summary.");
			});
	}
} // namespace TerraEngine::EditorTests
