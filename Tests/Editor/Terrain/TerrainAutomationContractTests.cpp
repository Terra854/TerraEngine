// TerrainAutomationContractTests.cpp -- Canonical/bounded terrain tool safety.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Automation/TerrainAutomationSchema.h"

#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string UuidText(const TerraEngine::UUID& id)
		{
			const std::string compact = TerraEngine::ToHexString(id);
			return compact.substr(0u, 8u) + '-' + compact.substr(8u, 4u) + '-' + compact.substr(12u, 4u) + '-'
				+ compact.substr(16u, 4u) + '-' + compact.substr(20u, 12u);
		}

		nlohmann::json WorldPosition(std::string cellX, std::string cellZ, double localX, double localZ)
		{
			return {{"cellX", std::move(cellX)}, {"cellY", "0"}, {"cellZ", std::move(cellZ)},
				{"localX", localX}, {"localY", 0.0}, {"localZ", localZ}};
		}

		nlohmann::json ValidRequest()
		{
			return {
				{"schemaVersion", 1},
				{"transactionId", UuidText(TerraEngine::GenerateUUID())},
				{"correlationId", "terrain-contract"},
				{"idempotencyKey", "terrain-contract-1"},
				{"target", {{"assetUuid", UuidText(TerraEngine::GenerateUUID())}, {"instanceUuid", UuidText(TerraEngine::GenerateUUID())}}},
				{"operation", {{"kind", "height-remap"}, {"settings", {{"minimumMm", -100}, {"maximumMm", 500}}}, {"seed", 17u}}},
				{"worldBounds", {{"min", WorldPosition("-1", "0", 32767.0, 0.0)}, {"max", WorldPosition("0", "0", -32511.0, 256.0)}}},
				{"expectedRootHash", "00000000000000010000000000000002"},
				{"expectedLayerStackGeneration", "3"},
				{"observations", nlohmann::json::array({{{"areaX", "0"}, {"areaZ", "0"}, {"editGeneration", "7"}, {"authoredContentHash", "00000000000000030000000000000004"}}})},
				{"budgets", {{"maxAreas", 1u}, {"maxSamples", 65'536u}, {"maxPreparedBytes", 2u * 1024u * 1024u}, {"workloadClass", "interactive"}}},
				{"previewOnly", true},
				{"approvalFingerprint", nullptr}
			};
		}
	}

	void RegisterTerrainAutomationContractTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainAutomation.AcceptsCanonicalBoundedRequestAndStableFingerprint", [](TestContext& context) {
			const auto parsed = TerraEngine::Editor::Automation::ParseTerrainAutomationRequest(ValidRequest());
			const std::string fingerprint = parsed.accepted ? TerraEngine::Editor::Automation::TerrainApprovalFingerprint(parsed.request) : std::string{};
			context.Expect(parsed.accepted && parsed.request.bounds == TerraEngine::Terrain::TerrainSampleBounds{-32'769, 0, -32'511, 256}
				&& parsed.request.observations.front().authoredContentHash == TerraEngine::Terrain::ContentHash{4u, 3u}
				&& parsed.request.expectedLayerStackGeneration == 3u && parsed.request.seed == 17u && fingerprint.size() == 32u,
				"A complete request must retain canonical world bounds, typed hashes, generations, budgets, seed, and a stable approval fingerprint.");

			auto changed = parsed.request;
			changed.settings["maximumMm"] = 501;
			context.Expect(TerraEngine::Editor::Automation::TerrainApprovalFingerprint(changed) != fingerprint,
				"Changing operation settings after preflight must invalidate the approval fingerprint.");
		});

		registry.Add("Editor.TerrainAutomation.RejectsAmbiguousUnknownOverflowAndBudgetViolations", [](TestContext& context) {
			auto screen = ValidRequest(); screen["screenPosition"] = nlohmann::json::array({1, 2});
			auto unknown = ValidRequest(); unknown["transaction"] = "substring keys must not pass";
			auto local = ValidRequest(); local["worldBounds"]["min"]["localX"] = TerraEngine::Scene::kWorldCellHalfEdgeMetres;
			auto overflow = ValidRequest(); overflow["worldBounds"]["max"]["cellX"] = "9223372036854775807";
			auto budget = ValidRequest(); budget["budgets"]["maxAreas"] = 0u;
			auto duplicate = ValidRequest(); duplicate["observations"].push_back(duplicate["observations"].front());
			auto hash = ValidRequest(); hash["observations"][0]["authoredContentHash"] = "ABCDEF00000000000000000000000000";
			context.Expect(!TerraEngine::Editor::Automation::ParseTerrainAutomationRequest(screen).accepted
				&& !TerraEngine::Editor::Automation::ParseTerrainAutomationRequest(unknown).accepted
				&& !TerraEngine::Editor::Automation::ParseTerrainAutomationRequest(local).accepted
				&& !TerraEngine::Editor::Automation::ParseTerrainAutomationRequest(overflow).accepted
				&& !TerraEngine::Editor::Automation::ParseTerrainAutomationRequest(budget).accepted
				&& !TerraEngine::Editor::Automation::ParseTerrainAutomationRequest(duplicate).accepted
				&& !TerraEngine::Editor::Automation::ParseTerrainAutomationRequest(hash).accepted,
				"Screen-space, unknown, non-normalized, overflowing, unbounded, duplicate, and non-canonical hash requests must fail before typed mutation preparation.");
		});
	}
}
