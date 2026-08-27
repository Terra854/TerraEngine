// TerrainAutomationSchema.h -- Canonical-only automation request validation.

#pragma once

#include "EditorAutomationTypes.h"
#include "Renderer/Terrain/TerrainLodSelection.h"
#include "Terrain/TerrainDiagnostics.h"
#include "Terrain/TerrainEditTransaction.h"

namespace TerraEngine::Editor::Automation
{
	struct TerrainAutomationRequest
	{
		TerraEngine::UUID transactionId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID instanceId = TerraEngine::EMPTY_UUID;
		std::string correlationId, idempotencyKey, operation, expectedRootHash, approvalFingerprint;
		std::string workloadClass;
		nlohmann::json settings = nlohmann::json::object();
		TerraEngine::Scene::WorldPosition minimumWorld{};
		TerraEngine::Scene::WorldPosition maximumWorld{};
		TerraEngine::Terrain::TerrainSampleBounds bounds{};
		TerraEngine::Terrain::TerrainEditBudget budget{};
		std::vector<TerraEngine::Terrain::TerrainAreaObservation> observations;
		TerraEngine::Terrain::Generation expectedLayerStackGeneration = 0u;
		std::optional<std::uint64_t> seed;
		bool previewOnly = true;
	};
	struct TerrainAutomationValidation { bool accepted = false; std::string category, message; TerrainAutomationRequest request{}; };

	// These records are detached copies of the renderer and authoring facts that
	// automation is allowed to inspect. Keeping them value-only prevents a client
	// from retaining pointers into a terrain root while a newer root is published.
	struct TerrainAutomationEnabledLayerSnapshot
	{
		std::uint32_t runtimeSlot = 0u;
		std::uint16_t stableLayerId = 0u;
		std::string name;
		TerraEngine::UUID materialAssetUuid = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainLayerMapping mapping{};
		SamplerFilter requestedSamplerQuality = SamplerFilter::Trilinear;
		SamplerFilter effectiveSamplerQuality = SamplerFilter::Trilinear;
		float requestedAnisotropy = 1.0F;
		float effectiveAnisotropy = 1.0F;
		TerraEngine::Terrain::TerrainSamplerFallbackReason samplerFallbackReason =
			TerraEngine::Terrain::TerrainSamplerFallbackReason::None;
		std::vector<std::string> diagnosticCodes;
	};

	struct TerrainAutomationAreaSnapshot
	{
		TerraEngine::Terrain::TerrainAreaCoordinate coordinate{};
		std::uint64_t stableAreaIdentity = 0u;
		std::uint16_t desiredLod = 0u;
		std::uint16_t actualLod = 0u;
		std::uint16_t balancedLod = 0u;
		std::uint32_t desiredPageId = TerraEngine::Renderer::Terrain::kInvalidTerrainPageId;
		std::uint32_t actualPageId = TerraEngine::Renderer::Terrain::kInvalidTerrainPageId;
		TerraEngine::Resource::VirtualGeometryPagePublicationState pageState =
			TerraEngine::Resource::VirtualGeometryPagePublicationState::Missing;
		std::uint16_t fallbackDepth = 0u;
		bool desiredPageResident = false;
		bool pinnedCoarseCoverage = false;
		bool completeCoverage = false;
		bool balanceRepaired = false;
		std::vector<std::string> diagnosticCodes;
	};

	struct TerrainAutomationSnapshot
	{
		std::uint32_t schemaVersion = 1u;
		TerraEngine::UUID assetUuid = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::ContentHash rootHash{};
		TerraEngine::Terrain::Generation rootGeneration = 0u;
		double sampleSpacingMetres = 1.0;
		std::vector<TerrainAutomationEnabledLayerSnapshot> enabledLayers;
		std::vector<TerrainAutomationAreaSnapshot> areas;
		std::vector<std::string> diagnosticCodes;
	};

	TerrainAutomationValidation ParseTerrainAutomationRequest(const nlohmann::json& json);
	std::string TerrainApprovalFingerprint(const TerrainAutomationRequest& request);
	// Codes are serialized as names instead of enum ordinals so saved automation
	// evidence remains understandable when an enum gains new values later.
	[[nodiscard]] std::string_view TerrainAutomationMappingModeCode(
		TerraEngine::Terrain::TerrainLayerMappingMode mode) noexcept;
	[[nodiscard]] std::string_view TerrainAutomationSamplerQualityCode(SamplerFilter quality) noexcept;
	[[nodiscard]] std::string_view TerrainAutomationPageStateCode(
		TerraEngine::Resource::VirtualGeometryPagePublicationState state) noexcept;
	[[nodiscard]] nlohmann::json SerializeTerrainAutomationSnapshot(
		const TerrainAutomationSnapshot& snapshot);
	[[nodiscard]] nlohmann::json TerrainAutomationSnapshotResultSchema();
}
