#include "TestHarness.h"

#include "VirtualGeometryQuantization.h"

#include <array>
#include <cmath>
#include <limits>

namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryQuantizationProfileTests(TestRegistry &registry)
	{
		using namespace TerraEngine::Editor;

		registry.Add("VirtualGeometry.QuantizationProfiles.VersionedAttributeBudgets", [](TestContext &context) {
			const VirtualGeometryQuantizationProfile &cinematic =
				GetVirtualGeometryQuantizationProfile(VirtualGeometryQuantizationProfileName::Cinematic);
			const VirtualGeometryQuantizationProfile &balanced =
				GetVirtualGeometryQuantizationProfile(VirtualGeometryQuantizationProfileName::Balanced);
			const VirtualGeometryQuantizationProfile &memory =
				GetVirtualGeometryQuantizationProfile(VirtualGeometryQuantizationProfileName::Memory);

			context.Expect(cinematic.Valid() && balanced.Valid() && memory.Valid(), "Named production profiles need versioned, internally valid declarations.");
			context.Expect(
				GetVirtualGeometryQuantizationBits(cinematic, VirtualGeometryQuantizationAttribute::Position) >
					GetVirtualGeometryQuantizationBits(balanced, VirtualGeometryQuantizationAttribute::Position) &&
					GetVirtualGeometryQuantizationBits(balanced, VirtualGeometryQuantizationAttribute::Position) >
						GetVirtualGeometryQuantizationBits(memory, VirtualGeometryQuantizationAttribute::Position),
				"Cinematic, Balanced, and Memory profiles must retain their ordered position precision trade-off.");
			context.Expect(
				GetVirtualGeometryQuantizationBits(cinematic, VirtualGeometryQuantizationAttribute::MorphDelta) >
					GetVirtualGeometryQuantizationBits(memory, VirtualGeometryQuantizationAttribute::MorphDelta),
				"Morph deltas require a profile-specific budget instead of borrowing position precision.");
		});

		registry.Add("VirtualGeometry.QuantizationProfiles.SelectsBitsPerAttribute", [](TestContext &context) {
			const VirtualGeometryQuantizationProfile &profile =
				GetVirtualGeometryQuantizationProfile(VirtualGeometryQuantizationProfileName::Balanced);
			const std::uint32_t normal = QuantizeVirtualGeometryAttribute(0.5f, -1.0f, 1.0f,
				GetVirtualGeometryQuantizationBits(profile, VirtualGeometryQuantizationAttribute::Normal));
			const std::uint32_t texture = QuantizeVirtualGeometryAttribute(0.5f, 0.0f, 1.0f,
				GetVirtualGeometryQuantizationBits(profile, VirtualGeometryQuantizationAttribute::TextureCoordinate));

			context.Expect(normal > 0u && texture > normal, "Attributes with different declared bit budgets must use distinct integer lattices.");
			context.Expect(
				std::abs(DequantizeVirtualGeometryAttribute(texture, 0.0f, 1.0f,
					GetVirtualGeometryQuantizationBits(profile, VirtualGeometryQuantizationAttribute::TextureCoordinate)) - 0.5f) < 0.001f,
				"Per-attribute dequantization should reconstruct values from the matching declared budget.");
		});

		registry.Add("VirtualGeometry.QuantizationProfiles.RejectsUnsuitableLargeDomain", [](TestContext &context) {
			VirtualGeometryQuantizationDomain domain{};
			domain.kind = VirtualGeometryQuantizationDomainKind::Asset;
			domain.stableId = 11u;
			domain.bounds = { { 0.0f, 0.0f, 0.0f }, { 100000001.0f, 1.0f, 1.0f } };
			const VirtualGeometryQuantizationAnalysis analysis = AnalyzeVirtualGeometryQuantization(
				domain, GetVirtualGeometryQuantizationProfile(VirtualGeometryQuantizationProfileName::Balanced));

			context.Expect(
				analysis.suitability == VirtualGeometryQuantizationSuitability::ExtentTooLarge,
				"Large-coordinate domains must diagnose an unsuitable profile rather than silently overflowing precision.");
		});

		registry.Add("VirtualGeometry.QuantizationProfiles.BuildsAttributeSpecificProductionDomains", [](TestContext& context) {
			const std::array<VirtualGeometryProductionQuantizationStreamRange, 3u> ranges = {
				VirtualGeometryProductionQuantizationStreamRange{
					VirtualGeometryQuantizationAttribute::Position,
					TemfQuantizationDomainScope::Asset,
					TemfStreamKind::Position,
					0x1001u,
					3u,
					0u,
					0u,
					{ -2.0, -1.0, -4.0, 0.0 },
					{ 4.0, 2.0, 8.0, 0.0 } },
				VirtualGeometryProductionQuantizationStreamRange{
					VirtualGeometryQuantizationAttribute::Normal,
					TemfQuantizationDomainScope::PageInterior,
					TemfStreamKind::NormalTangent,
					0x1002u,
					2u,
					1u,
					0u,
					{ -1.0, -1.0, 0.0, 0.0 },
					{ 2.0, 2.0, 0.0, 0.0 } },
				VirtualGeometryProductionQuantizationStreamRange{
					VirtualGeometryQuantizationAttribute::TextureCoordinate,
					TemfQuantizationDomainScope::Asset,
					TemfStreamKind::TextureColor,
					0x1003u,
					2u,
					0u,
					0u,
					{ 0.0, 0.0, 0.0, 0.0 },
					{ 1.0, 1.0, 0.0, 0.0 } }
			};
			const VirtualGeometryProductionQuantizationRequest request{
				VirtualGeometryQuantizationProfileName::Balanced, ranges };
			const VirtualGeometryProductionQuantizationResult first =
				BuildVirtualGeometryProductionQuantizationDomains(request);
			const VirtualGeometryProductionQuantizationResult repeated =
				BuildVirtualGeometryProductionQuantizationDomains(request);

			context.Expect(first.IsComplete(), "A valid named profile should build one canonical domain per attribute stream.");
			context.ExpectEqual(static_cast<std::size_t>(3u), first.domains.size(),
				"Position, normal, and texture streams need independent descriptors.");
			context.Expect(first.domains[0].domainId == 0u && first.domains[1].domainId == 1u
				&& first.domains[2].domainId == 2u,
				"Production quantization domain IDs must be contiguous in descriptor order.");
			context.Expect(
				first.domains[0].componentBitWidths[0] != first.domains[1].componentBitWidths[0]
				&& first.domains[1].componentBitWidths[0] != first.domains[2].componentBitWidths[0],
				"Each descriptor must use the named profile's budget for its own attribute class.");
			context.Expect(first.settingsHash == repeated.settingsHash
				&& first.domainSetHash == repeated.domainSetHash,
				"Identical profile and range inputs must produce deterministic generation identities.");
		});

		registry.Add("VirtualGeometry.QuantizationProfiles.InvalidatesChangedProfileAndRange", [](TestContext& context) {
			VirtualGeometryProductionQuantizationStreamRange range{
				VirtualGeometryQuantizationAttribute::Position,
				TemfQuantizationDomainScope::Asset,
				TemfStreamKind::Position,
				0x2001u,
				3u,
				0u,
				0u,
				{ -1.0, -1.0, -1.0, 0.0 },
				{ 2.0, 2.0, 2.0, 0.0 } };
			const std::array<VirtualGeometryProductionQuantizationStreamRange, 1u> originalRanges{ range };
			const auto balanced = BuildVirtualGeometryProductionQuantizationDomains(
				{ VirtualGeometryQuantizationProfileName::Balanced, originalRanges });
			const auto cinematic = BuildVirtualGeometryProductionQuantizationDomains(
				{ VirtualGeometryQuantizationProfileName::Cinematic, originalRanges });
			range.stableIdentity = 0x2002u;
			const std::array<VirtualGeometryProductionQuantizationStreamRange, 1u> changedRanges{ range };
			const auto changed = BuildVirtualGeometryProductionQuantizationDomains(
				{ VirtualGeometryQuantizationProfileName::Balanced, changedRanges });

			context.Expect(balanced.IsComplete() && cinematic.IsComplete() && changed.IsComplete(),
				"Valid comparison inputs should all build production domain sets.");
			context.Expect(balanced.settingsHash != cinematic.settingsHash
				&& balanced.domainSetHash != cinematic.domainSetHash,
				"Changing the named profile must invalidate both settings and domain-set identities.");
			context.Expect(balanced.domainSetHash != changed.domainSetHash,
				"Changing the stable source range identity must invalidate the domain set.");
		});

		registry.Add("VirtualGeometry.QuantizationProfiles.RejectsMalformedProductionRange", [](TestContext& context) {
			VirtualGeometryProductionQuantizationStreamRange range{};
			range.stableIdentity = 0x3001u;
			range.componentCount = 3u;
			range.extent = { 1.0, 1.0, 1.0, 0.0 };
			range.origin[1] = std::numeric_limits<double>::infinity();
			const std::array<VirtualGeometryProductionQuantizationStreamRange, 1u> ranges{ range };
			const auto result = BuildVirtualGeometryProductionQuantizationDomains(
				{ VirtualGeometryQuantizationProfileName::Balanced, ranges });

			context.Expect(!result.IsComplete() && result.domains.empty() && !result.diagnostic.empty(),
				"Non-finite production ranges must fail closed without publishing partial domains.");
		});
	}
} // namespace TerraEngine::EditorTests
