#include "../TestHarness.h"

#include "Core/Scheduling/FrameWorkload.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/BulkScenePublication.h"
#include "Scene/Serializer.h"

#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests {
	void RegisterArchitectureHardeningPlayModeTests(TestRegistry& registry)
	{
		registry.Add("Editor.ArchitectureHardening.ProtectedFallbackReasonIsVisible", [](TestContext& context) {
			TerraEngine::Scheduling::FrameWorkloadDeclaration workload;
			workload.workloadName = "EditorPlayMode";
			workload.ownerSystem = "Editor";
			workload.phase = "Editor";
			workload.reads = { "Editor" };
			workload.protectedReason = "editor play-mode compatibility";

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ workload }, "editor-play-mode");

			context.Expect(plan.valid, "Editor protected fallback should validate.");
			context.Expect(plan.formatted.find("editor play-mode compatibility") != std::string::npos, "Protected fallback reason should be visible.");
		});

		registry.Add("Editor.ArchitectureHardening.PlayModeBulkRestoreRejectsStaleGeneration", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object authored = objects.createObject("Generation Safe Play Snapshot");
			const nlohmann::json authoredScene = {
				{"format", TerraEngine::Scene::kTerraSceneUnboundedFormat},
				{"name", "Generation Safe Play"},
				{"objects", nlohmann::json::array({Serializer::SerializeObject(authored)})}
			};
			const TerraEngine::Scene::PreparedSceneDecode prepared = Serializer::PrepareSceneDecode(authoredScene);
			const TerraEngine::Scene::BulkScenePublicationPacket packet =
				TerraEngine::Scene::BuildBulkScenePublicationPacket(prepared, objects.GetSceneGeneration(), objects.GetSceneGeneration());

			// Clearing models an intervening scene lifecycle boundary. The old packet
			// must not recreate an object in a later scene generation.
			objects.clear();
			const TerraEngine::Scene::BulkScenePublicationPreflight preflight =
				TerraEngine::Scene::PreflightBulkScenePublication(packet, objects);
			context.Expect(!preflight.accepted, "A Play Mode restoration packet from an earlier scene generation must be rejected.");
			context.Expect(preflight.reason.find("stale scene generation") != std::string::npos,
				"The stale restore rejection should provide a stable lifecycle diagnostic.");
			objects.clear();
		});

		registry.Add("Editor.ArchitectureHardening.PlayModeStopRestoresAcrossRuntimeSceneGeneration", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object authored = objects.createObject("Authored Before Play");
			const nlohmann::json authoredScene = {
				{"format", TerraEngine::Scene::kTerraSceneUnboundedFormat},
				{"name", "Play Mode Restore"},
				{"objects", nlohmann::json::array({Serializer::SerializeObject(authored)})}
			};
			const TerraEngine::Scene::PreparedSceneDecode prepared = Serializer::PrepareSceneDecode(authoredScene);
			TerraEngine::Scene::BulkScenePublicationPacket packet =
				TerraEngine::Scene::BuildBulkScenePublicationPacket(prepared, objects.GetSceneGeneration(), objects.GetSceneGeneration());

			// A runtime scene replacement is transient inside Play Mode. The editor's
			// session-owned Stop packet must target the current runtime generation so
			// it can replace that graph with the captured authored state atomically.
			objects.clear();
			(void)objects.createObject("Transient Runtime Scene");
			packet.expectedSceneGeneration = objects.GetSceneGeneration();
			const TerraEngine::Scene::BulkScenePublicationResult restored =
				TerraEngine::Scene::PublishBulkScenePacket(packet, objects);
			const std::vector<Object> roots = objects.getRootObjects();
			context.Expect(restored.published && roots.size() == 1U,
				"Stop must publish the captured authored graph even after runtime scene replacement advances the generation.");
			context.Expect(!roots.empty() && objects.getObjectName(roots.front()) == "Authored Before Play",
				"The transient runtime graph must be discarded rather than surviving Stop.");
			objects.clear();
		});
	}
}
