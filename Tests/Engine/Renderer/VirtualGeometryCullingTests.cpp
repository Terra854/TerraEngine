#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RenderSystem.h"

namespace TerraEngine::Tests {

	void RegisterVirtualGeometryCullingTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Culling.Foundation.UnknownConeSafetyDisablesConeCull", [](TestContext& context) {
			ConeSafetyMetadata cone;

			const ConeSafetyMetadata normalized = NormalizeVirtualGeometryConeSafetyMetadata(cone);

			context.Expect(!normalized.coneValid, "Cone culling should be disabled until imported material, winding, alpha, and thin-shell metadata are proven safe.");
			context.Expect(!normalized.eligible, "Unknown cone safety metadata should never arm shader cone culling.");
			context.Expect(!normalized.disabledReason.empty(), "Unsafe cone metadata should carry a visible disabled reason.");
		});

		registry.Add("VirtualGeometry.Culling.US6.ProvenOneSidedOpaqueConeSafetyIsEligible", [](TestContext& context) {
			ConeSafetyMetadata cone;
			cone.materialSidedness = VirtualGeometryConeMaterialSidedness::OneSided;
			cone.windingState = VirtualGeometryConeWindingState::Consistent;
			cone.alphaRisk = VirtualGeometryConeAlphaRisk::Opaque;
			cone.thinShellRisk = VirtualGeometryConeThinShellRisk::Low;
			cone.coneValid = true;

			const ConeSafetyMetadata normalized = NormalizeVirtualGeometryConeSafetyMetadata(cone);

			context.Expect(normalized.eligible, "Only proven one-sided opaque low-risk clusters should be eligible for cone culling.");
			context.Expect(normalized.disabledReason.empty(), "Eligible cone metadata should not expose a disabled reason.");
		});

		registry.Add("VirtualGeometry.Culling.US6.RiskyOrMissingClusterMetadataFailsOpen", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset;
			ConeSafetyMetadata risky;
			risky.clusterId = 4u;
			risky.coneValid = true;
			risky.materialSidedness = VirtualGeometryConeMaterialSidedness::DoubleSided;
			risky.windingState = VirtualGeometryConeWindingState::Consistent;
			risky.alphaRisk = VirtualGeometryConeAlphaRisk::Opaque;
			risky.thinShellRisk = VirtualGeometryConeThinShellRisk::Low;
			asset.coneSafety.push_back(NormalizeVirtualGeometryConeSafetyMetadata(risky));

			TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord selected;
			selected.clusterId = 4u;
			std::string disabledReason;
			context.Expect(!RenderSystem::AreVirtualGeometryClustersConeCullingEligible(asset, { selected }, &disabledReason), "Double-sided clusters should fail open instead of arming cone culling.");
			context.Expect(disabledReason.find("one-sided") != std::string::npos, "Rejected cone metadata should explain the missing one-sided proof.");

			selected.clusterId = 9u;
			disabledReason.clear();
			context.Expect(!RenderSystem::AreVirtualGeometryClustersConeCullingEligible(asset, { selected }, &disabledReason), "Missing cone safety metadata should also fail open.");
			context.Expect(disabledReason.find("missing") != std::string::npos, "Missing metadata should be named for diagnostics.");
		});

		registry.Add("VirtualGeometry.Culling.Foundation.HiZRegressionMismatchWarnsWithoutQualityTier", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility;
			visibility.frameId = 3015u;
			visibility.outputId = 8u;
			visibility.viewType = RenderViewType::Game;
			visibility.valid = true;
			visibility.hiZValidationState.state = VirtualGeometryHiZState::Active;
			visibility.hiZValidationState.pyramidReady = true;
			visibility.hiZValidationState.failOpen = false;

			HiZRegressionSnapshot hiZ;
			hiZ.viewId = visibility.outputId;
			hiZ.viewType = visibility.viewType;
			hiZ.frameIndex = visibility.frameId;
			hiZ.state = VirtualGeometryHiZState::Active;
			hiZ.testedMeshlets = 32u;
			hiZ.occludedMeshlets = 8u;
			hiZ.baselineMatched = false;
			visibility.hiZRegressionSnapshots.push_back(hiZ);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(!diagnostics.hiZBaselineMatched, "Diagnostics should expose Hi-Z baseline mismatches.");
			context.ExpectEqual(1u, diagnostics.hiZFalseOcclusionRiskWarnings, "Hi-Z baseline mismatches should increment risk warnings.");
			context.Expect(!diagnostics.warnings.empty(), "Hi-Z baseline mismatches should produce user-visible diagnostics.");
		});
	}
}
