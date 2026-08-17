// TerrainAutomationSchema.h -- Canonical-only automation request validation.

#pragma once

#include "EditorAutomationTypes.h"
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
	TerrainAutomationValidation ParseTerrainAutomationRequest(const nlohmann::json& json);
	std::string TerrainApprovalFingerprint(const TerrainAutomationRequest& request);
}
