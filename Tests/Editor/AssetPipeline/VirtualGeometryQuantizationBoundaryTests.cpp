#include "TestHarness.h"

#include "VirtualGeometryErrorModel.h"
#include "VirtualGeometryIdentity.h"
#include "VirtualGeometryQuantization.h"

#include <array>
#include <cmath>

namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryQuantizationBoundaryTests(TestRegistry &registry)
	{
		using namespace TerraEngine::Editor;

		registry.Add("VirtualGeometry.QuantizationBoundaries.StableBeforePageScheduling", [](TestContext &context) {
			const VirtualGeometryBoundaryGroup first = AssignVirtualGeometryBoundaryGroup(42u, 3u, 7u);
			const VirtualGeometryBoundaryGroup repeated = AssignVirtualGeometryBoundaryGroup(42u, 3u, 7u);
			const VirtualGeometryBoundaryGroup changedVertex = AssignVirtualGeometryBoundaryGroup(42u, 3u, 8u);

			context.ExpectEqual(first.groupId, repeated.groupId, "Boundary groups must be deterministic before page jobs divide the source primitive.");
			context.Expect(first.groupId != changedVertex.groupId, "Different source vertices must not accidentally share a protected boundary group.");
			context.Expect(first.sourcePrimitiveId == repeated.sourcePrimitiveId, "Boundary groups should retain their source primitive identity for diagnostics.");
		});

		registry.Add("VirtualGeometry.QuantizationBoundaries.UsesSharedDomainAndHalfStepError", [](TestContext &context) {
			VirtualGeometryQuantizationDomain domain{};
			domain.kind = VirtualGeometryQuantizationDomainKind::SharedBoundary;
			domain.stableId = AssignVirtualGeometryBoundaryGroup(42u, 3u, 7u).groupId;
			domain.bounds = { { -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f } };
			domain.protectsSharedBoundary = true;
			const VirtualGeometryQuantizationProfile &profile =
				GetVirtualGeometryQuantizationProfile(VirtualGeometryQuantizationProfileName::Cinematic);
			const VirtualGeometryQuantizationAnalysis analysis = AnalyzeVirtualGeometryQuantization(domain, profile);

			const VirtualGeometryQuantizedPosition first = QuantizeVirtualGeometryPosition({ 0.25f, -0.5f, 0.75f }, domain.bounds);
			const VirtualGeometryQuantizedPosition second = QuantizeVirtualGeometryPosition({ 0.25f, -0.5f, 0.75f }, domain.bounds);
			const std::array<VirtualGeometryQuantizedPosition, 1u> firstBoundary{ first };
			const std::array<VirtualGeometryQuantizedPosition, 1u> secondBoundary{ second };
			context.Expect(analysis.Suitable(), "A finite protected shared domain should be quantizable with the Cinematic profile.");
			context.Expect(ValidateVirtualGeometryWatertightBoundary(firstBoundary, secondBoundary), "Shared-domain vertices must quantize to one matching lattice value.");
			context.Expect(analysis.maximumEuclideanError > 0.0f && analysis.maximumEuclideanError <= profile.maximumPositionError,
				"Reported half-step error must be positive and bounded by the selected profile contract.");
		});

		registry.Add("VirtualGeometry.QuantizationBoundaries.DeclaresPageAssetAndDeformableDomains", [](TestContext &context) {
			VirtualGeometryQuantizationDomain domain{};
			domain.stableId = 17u;
			domain.bounds = { { -2.0f, -2.0f, -2.0f }, { 2.0f, 2.0f, 2.0f } };
			domain.kind = VirtualGeometryQuantizationDomainKind::Page;
			context.Expect(domain.Valid(), "A page domain should validate when it has stable identity and finite bounds.");
			domain.kind = VirtualGeometryQuantizationDomainKind::Asset;
			context.Expect(domain.Valid(), "An asset domain should preserve one stable lattice for independently decoded pages.");
			domain.kind = VirtualGeometryQuantizationDomainKind::DeformableEnvelope;
			context.Expect(!domain.Valid(), "Deformable domains must reject missing deformation-envelope coverage.");
			domain.includesDeformationEnvelope = true;
			context.Expect(domain.Valid(), "Deformable domains should accept a declared conservative deformation envelope.");
		});

		registry.Add("VirtualGeometry.QuantizationBoundaries.RejectsDegenerateAndAccumulatesMonotonically", [](TestContext &context) {
			VirtualGeometryQuantizationDomain degenerate{};
			degenerate.kind = VirtualGeometryQuantizationDomainKind::Page;
			degenerate.stableId = 9u;
			degenerate.bounds = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 1.0f } };
			const VirtualGeometryQuantizationAnalysis analysis = AnalyzeVirtualGeometryQuantization(
				degenerate, GetVirtualGeometryQuantizationProfile(VirtualGeometryQuantizationProfileName::Balanced));
			const VirtualGeometryAccumulatedDetailError accumulated = BuildVirtualGeometryAccumulatedDetailError(0.25f, 0.125f, { 0.5f, 0.375f });

			context.Expect(analysis.suitability == VirtualGeometryQuantizationSuitability::DegenerateExtent,
				"Degenerate page bounds must fail closed with an explicit diagnostic.");
			context.Expect(accumulated.Valid() && std::abs(accumulated.accumulatedError - 0.5f) < 0.0001f,
				"Accumulated detail error must retain the largest local, half-step, or child error.");
		});

		registry.Add("VirtualGeometry.QuantizationBoundaries.ProductionSharedRangesStayBitIdentical", [](TestContext& context) {
			const VirtualGeometryProductionQuantizationStreamRange shared{
				VirtualGeometryQuantizationAttribute::Position,
				TemfQuantizationDomainScope::SharedBoundary,
				TemfStreamKind::Position,
				AssignVirtualGeometryBoundaryGroup(7u, 11u, 13u).groupId,
				3u,
				0u,
				0u,
				{ -4.0, -4.0, -4.0, 0.0 },
				{ 8.0, 8.0, 8.0, 0.0 },
				true,
				false };
			const std::array<VirtualGeometryProductionQuantizationStreamRange, 1u> firstRanges{ shared };
			const std::array<VirtualGeometryProductionQuantizationStreamRange, 1u> neighboringRanges{ shared };
			const auto first = BuildVirtualGeometryProductionQuantizationDomains(
				{ VirtualGeometryQuantizationProfileName::Cinematic, firstRanges });
			const auto neighboring = BuildVirtualGeometryProductionQuantizationDomains(
				{ VirtualGeometryQuantizationProfileName::Cinematic, neighboringRanges });

			context.Expect(first.IsComplete() && neighboring.IsComplete(),
				"Protected shared-boundary ranges must produce complete domains before page jobs start.");
			context.Expect(first.domains[0].domainHash == neighboring.domains[0].domainHash
				&& first.domainSetHash == neighboring.domainSetHash,
				"Two pages using the same protected source identity and range must reference one bit-identical grid.");
		});

		registry.Add("VirtualGeometry.QuantizationBoundaries.RequiresExplicitCoverageFlags", [](TestContext& context) {
			VirtualGeometryProductionQuantizationStreamRange shared{};
			shared.attribute = VirtualGeometryQuantizationAttribute::Position;
			shared.scope = TemfQuantizationDomainScope::SharedBoundary;
			shared.streamKind = TemfStreamKind::Position;
			shared.stableIdentity = 0x4001u;
			shared.componentCount = 3u;
			shared.extent = { 1.0, 1.0, 1.0, 0.0 };
			const std::array<VirtualGeometryProductionQuantizationStreamRange, 1u> ranges{ shared };
			const auto missingBoundary = BuildVirtualGeometryProductionQuantizationDomains(
				{ VirtualGeometryQuantizationProfileName::Balanced, ranges });
			shared.scope = TemfQuantizationDomainScope::DeformableEnvelope;
			const std::array<VirtualGeometryProductionQuantizationStreamRange, 1u> deformableRanges{ shared };
			const auto missingEnvelope = BuildVirtualGeometryProductionQuantizationDomains(
				{ VirtualGeometryQuantizationProfileName::Balanced, deformableRanges });

			context.Expect(!missingBoundary.IsComplete() && !missingEnvelope.IsComplete(),
				"Shared and deformable ranges must fail closed until their conservative coverage is explicit.");
		});
	}
} // namespace TerraEngine::EditorTests
