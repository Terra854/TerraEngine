// Pure recipe and actual scene-boundary tests. Synthetic completed rows test
// admission guards only; they are not GPU acceptance or physical-slot evidence.
#include "../TestHarness.h"
#include "Tests/Support/RendererEfficiency/Editor/EditorRendererEfficiencyValidation.h"
#include "Core/Object/ObjectManager.h"
#include "Components/LightComponent.h"
#include "Components/Transform.h"
#include "Scene/SceneMutationQueue.h"
#include "Utilities/UUID.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace TerraEngine;
		using namespace Editor::Automation;
		using Json = nlohmann::json;
		Json Payload()
		{
			return {{"candidateOrder", {"65200000000000000000000000000001"}},
				{"baselineCandidateOrder", Json::array()}, {"lightingBudgets", {{"maxUploadedLights", 1024}, {"maxDynamicLights", 999}, {"maxShadowLights", 0}}}};
		}
		Json Bundle()
		{
			return {{"views", Json::array({{{"viewType", "Scene"}, {"viewId", 1}, {"lightingPurposes", {"FinalLights", "Analysis"}}}})}};
		}
		Renderer::RendererEfficiencyFixtureIntent Applied()
		{
			Renderer::RendererEfficiencyFixtureIntent value;
			value.controlsApplied = true; value.controlGeneration = 3; value.correlation = {"operation", "lighting-thresholds-00", 0, 2, 4, 5};
			value.lightingControls.emplace(); value.lightingControls->snapshotSlot = 7;
			return value;
		}
		std::vector<Renderer::RendererEfficiencyEvidence> ZeroRows(const Renderer::RendererEfficiencyFixtureIntent& intent)
		{
			std::vector<Renderer::RendererEfficiencyEvidence> rows(2);
			for(std::size_t index = 0; index < rows.size(); ++index)
			{
				auto& row = rows[index]; row.kind = Renderer::RendererEfficiencyEvidenceKind::Lighting; row.analysisPurpose = index == 1;
				row.status = Renderer::RendererEfficiencyEvidenceStatus::Valid; row.fixtureCorrelation = intent.correlation; row.fixtureControlGeneration = intent.controlGeneration;
				row.viewType = RenderViewType::Scene; row.viewId = 1; row.frameSerial = 10; row.commandSerial = 11; row.submitSerial = 12; row.completionSerial = 13;
				row.effectiveOptions.optionsSessionGeneration = 6; row.effectiveOptions.settingsGeneration = 4; row.effectiveOptions.rendererSessionGeneration = 5;
				auto& upload = row.lightingUploads[index]; upload.available = upload.succeeded = true; upload.analysisPurpose = index == 1;
				upload.allocationId = 20 + index; upload.allocationGeneration = 30; upload.frameSlot = 2; upload.snapshotSlot = 7;
			}
			return rows;
		}
	}
	void RegisterRendererEfficiencyLightingFixtureTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.LightingFixture.SharedPurposesCannotReplaceShadowRegions", [](TestContext& context) {
			// Copied synthetic owners exercise actual admission, not GPU completion.
			// Three lights repeat Scene1, but its two purposes must appear only once.
			const auto intent = Applied(); auto rows = ZeroRows(intent); auto bundle = Bundle(); bundle["views"] = Json::array();
			for(unsigned light = 1; light <= 3; ++light)
			{
				const auto lightId = ParseUUID("6500000000000000000000000000000" + std::to_string(light)).value();
				const unsigned faces = light == 3 ? 6 : (light == 2 ? 2 : 1); Json indices = Json::array();
				for(unsigned face = 0; face < faces; ++face) { auto row = rows.front(); row.kind = Renderer::RendererEfficiencyEvidenceKind::Shadow; row.lightId = lightId; row.faceIndex = face; rows.push_back(row); indices.push_back(face); }
				bundle["views"].push_back({{"viewType", "Scene"}, {"viewId", 1}, {"lightId", ToHexString(lightId)}, {"faceIndices", indices}, {"lightingPurposes", {"FinalLights", "Analysis"}}});
			}
			for(auto& row : rows) { auto artifacts = std::make_shared<Renderer::RendererEfficiencyArtifacts>(); artifacts->frameColor = std::make_shared<Renderer::RendererEfficiencyFrameColor>(); row.artifacts = artifacts; }
			const auto options = rows.front().effectiveOptions; std::vector<Renderer::RendererEfficiencyEvidence> output; std::string error;
			context.Expect(SelectRendererEfficiencyControlTransition(bundle, intent, options, rows, output, error) && output.size() == 11, "Shared cohort retains nine actual shadow regions and exactly two purposes.");
			const auto sceneRows = rows; auto multiview = bundle;
			for(const auto& view : bundle["views"]) { auto game = view; game["viewType"] = "Game"; game["viewId"] = 2; multiview["views"].push_back(std::move(game)); }
			for(auto row : sceneRows) { row.viewType = RenderViewType::Game; row.viewId = 2; row.commandSerial = 14; for(auto& upload : row.lightingUploads) upload.snapshotSlot = 6; rows.push_back(std::move(row)); }
			context.Expect(SelectRendererEfficiencyControlTransition(multiview, intent, options, rows, output, error) && output.size() == 22, "Two actual views keep distinct commands and reserved lighting slices within one completed cohort.");
			rows = sceneRows; output.clear(); context.Expect(SelectRendererEfficiencyControlTransition(bundle, intent, options, rows, output, error), "Restore the single-view complete cohort for atomic negative checks.");
			const auto previousSize = output.size(); rows.pop_back(); context.Expect(!SelectRendererEfficiencyControlTransition(bundle, intent, options, rows, output, error) && output.size() == previousSize, "A missing cube face cannot be replaced by complete lighting owners.");
			rows.push_back(output.back()); rows.back().submitSerial++; context.Expect(!SelectRendererEfficiencyControlTransition(bundle, intent, options, rows, output, error), "Different lighting/shadow submissions cannot produce a shared cohort.");
		});
		registry.Add("Renderer.Efficiency.LightingFixture.BaselineRequiresRealCompleteOwnerCoverage", [](TestContext& context) {
			std::array<std::set<std::pair<std::uint32_t, std::uint32_t>>, 2> slots;
			for(auto& purpose : slots) for(std::uint32_t frame = 0; frame < 3; ++frame) for(std::uint32_t snapshot = 0; snapshot < 8; ++snapshot) purpose.insert({frame, snapshot});
			context.Expect(!RendererEfficiencyLightingBaselineReady(59, slots), "Complete owner coverage cannot replace the 60 actual baseline completions.");
			context.Expect(RendererEfficiencyLightingBaselineReady(60, slots), "Exactly60 plus all24 pairs each purpose admits the authored edit.");
			slots[1].erase({2, 7}); slots[1].insert({3, 7}); context.Expect(!RendererEfficiencyLightingBaselineReady(60, slots), "Cardinality24 with a missing actual owner cannot authorize an edit.");
			const auto captured = Applied(); const auto options = ZeroRows(captured).front().effectiveOptions;
			context.Expect(RendererEfficiencyLightingExportOwnerMatches(captured, 6, options.options, captured, options), "Unchanged actual protected owner permits a detached baseline result.");
			for(unsigned kind = 0; kind < 6; ++kind)
			{
				auto current = captured; auto fresh = options;
				if(kind == 0) ++current.controlGeneration;
				if(kind == 1) ++fresh.settingsGeneration;
				if(kind == 2) ++fresh.rendererSessionGeneration;
				if(kind == 3) ++fresh.optionsSessionGeneration;
				if(kind == 4) current.controlsApplied = false;
				if(kind == 5) fresh.options.lightingUploads = Renderer::RendererEfficiencyLightingUploads::Full;
				context.Expect(!RendererEfficiencyLightingExportOwnerMatches(captured, 6, options.options, current, fresh), "Changed control/settings/session/epoch or queued-only state cannot append or count the detached baseline.");
			}
		});
		registry.Add("Renderer.Efficiency.LightingFixture.PublicStatsRetainActualPurposeReceipts", [](TestContext& context) {
			auto rows = ZeroRows(Applied()); auto& row = rows[1]; auto& upload = row.lightingUploads[1]; upload.payloadLightIds.push_back(ParseUUID("65200000000000000000000000000001").value()); upload.activeRecordCount = 1; upload.logicalWriteBytes = 176; upload.logicalWriteRecords = 1; upload.logicalWriteRanges = {{0, 176}};
			const auto stats = SerializeRendererEfficiencyEvidence(row);
			context.Expect(stats.at("evidenceKind") == "Lighting" && stats.at("analysisPurpose") == true && stats.at("lightingUploads")[1].at("logicalWriteRecords") == 1 && stats.at("lightingUploads")[1].at("payloadLightIds")[0] == "65200000000000000000000000000001", "Copied diagnostics retain fullUUID, actual purpose and actual write counts.");
		});
		registry.Add("Renderer.Efficiency.LightingFixture.LegalEmptyAndOverCapacityControls", [](TestContext& context) {
			std::string error; Renderer::RendererEfficiencyFixtureLightingControls result;
			context.Expect(ParseRendererEfficiencyLightingControls(Payload(), true, result, error) && result.candidateOrder.empty(), "Zero is a legal actual candidate order.");
			auto payload = Payload(); payload["candidateOrder"] = Json::array();
			for(unsigned i = 1; i <= 1025; ++i) { auto id = GenerateUUID(); payload["candidateOrder"].push_back(ToHexString(id)); }
			context.Expect(ParseRendererEfficiencyLightingControls(payload, false, result, error) && result.candidateOrder.size() == 1025, "Source candidates must not be silently clamped to upload capacity.");
		});
		registry.Add("Renderer.Efficiency.LightingFixture.RejectControlsAtomically", [](TestContext& context) {
			Renderer::RendererEfficiencyFixtureLightingControls result; result.maxUploadedLights = 17; const auto original = result; std::string error;
			for(unsigned kind = 0; kind < 3; ++kind) { auto payload = Payload(); if(kind == 0) payload["candidateOrder"].push_back(payload["candidateOrder"][0]); if(kind == 1) payload["lightingBudgets"]["maxDynamicLights"] = 1025; if(kind == 2) payload["lightingBudgets"]["maxShadowLights"] = 1000; context.Expect(!ParseRendererEfficiencyLightingControls(payload, false, result, error) && result == original, "Malformed controls leave output unchanged."); }
		});
		registry.Add("Renderer.Efficiency.LightingFixture.TypedCommitWaitsForPublication", [](TestContext& context) {
			auto& objects = ObjectManager::instance(); objects.clear(); Transform::registerType(); LightComponent::registerType();
			const auto object = objects.createObject("P2 Light"); objects.AddComponent<LightComponent>(object); const auto id = objects.getUUID(object);
			objects.PublishSceneChangePacket(); Scene::Transaction values; std::string error;
			context.Expect(PrepareRendererEfficiencyLightingTransaction(Json::array({{{"operation", "UpdateComponentProperty"}, {"objectId", ToHexString(id)}, {"componentName", "LightComponent"}, {"propertyName", "intensity"}, {"value", 0.5}}}), values, error), "Prepare actual typed light property write.");
			Scene::TransactionalMutationBuilder captured("p2-light-edit", Scene::TransactionOrigin::Automation, "P2Test"); captured.ObserveScene(objects); captured.ObserveObject(objects, id);
			auto transaction = std::move(captured).Build(); transaction.writeSet = values.writeSet;
			RendererEfficiencyScenePublication proof; proof.transaction = transaction; proof.sceneGeneration = objects.GetSceneGeneration(); proof.priorPacket = objects.LoadPublishedSceneChangePacketShared();
			Scene::SceneMutationQueue queue; queue.EnqueueTransaction(transaction);
			context.Expect(objects.GetComponent<LightComponent>(object)->intensity == 1.0f, "Worker preparation and queue ingress cannot change live illumination.");
			queue.Apply(objects, "post_editor_automation_flush"); const auto receipt = queue.TakeCompletedAutomationTransactionReceipt("p2-light-edit");
			context.Expect(receipt && receipt->status == Scene::TransactionStatus::Committed && objects.GetComponent<LightComponent>(object)->intensity == 0.5f, "Existing protected flush commits the genuine write.");
			context.Expect(CaptureRendererEfficiencyCommittedScenePublication(objects, proof, *receipt, error), "Capture actual committed authored values.");
			std::uint64_t revision = 0; context.Expect(!ResolveRendererEfficiencyScenePublication(objects, proof, revision, error), "Committed write cannot advance before scheduler publication.");
			objects.PublishSceneChangePacket(); context.Expect(ResolveRendererEfficiencyScenePublication(objects, proof, revision, error) && revision > 0, "Actual scheduler packet releases the light edit."); queue.Clear(); objects.clear();
		});
		registry.Add("Renderer.Efficiency.LightingFixture.ConflictLeavesMembershipUnchanged", [](TestContext& context) {
			auto& objects = ObjectManager::instance(); objects.clear(); const auto object = objects.createObject("P2 Membership"); const auto id = objects.getUUID(object);
			Scene::TransactionalMutationBuilder builder("p2-conflict", Scene::TransactionOrigin::Automation, "P2Test"); builder.ObserveScene(objects); builder.ObserveObject(objects, id); builder.DeleteObject(id); builder.SetActive(id, false);
			Scene::SceneMutationQueue queue; queue.EnqueueTransaction(std::move(builder).Build()); queue.Apply(objects, "post_editor_automation_flush"); const auto receipt = queue.TakeCompletedAutomationTransactionReceipt("p2-conflict");
			context.Expect(receipt && receipt->status == Scene::TransactionStatus::Conflicted && receipt->appliedWriteCount == 0 && receipt->rejectedWriteCount == 2 && objects.getObject(id).has_value() && objects.isObjectActive(object), "Actual atomic conflict preserves original candidate membership."); queue.Clear(); objects.clear();
		});
		registry.Add("Renderer.Efficiency.LightingFixture.ZeroPurposeBundleNeedsBothOwners", [](TestContext& context) {
			const auto intent = Applied(); auto rows = ZeroRows(intent); std::vector<Renderer::RendererEfficiencyEvidence> output; std::string error;
			context.Expect(SelectRendererEfficiencyLightingBundle(Bundle(), intent, rows, output, error) && output.size() == 2, "Zero-light rows identify actual purposes without shadow UUID or cube faces.");
			const auto original = output; rows.pop_back(); context.Expect(!SelectRendererEfficiencyLightingBundle(Bundle(), intent, rows, output, error) && output.size() == original.size(), "Missing Analysis owner cannot produce a complete bundle.");
		});
		registry.Add("Renderer.Efficiency.LightingFixture.StaleSlotAndMixedCompletionReject", [](TestContext& context) {
			const auto intent = Applied(); std::vector<Renderer::RendererEfficiencyEvidence> output; std::string error;
			for(unsigned kind = 0; kind < 4; ++kind) { auto rows = ZeroRows(intent); if(kind == 0) --rows[1].fixtureControlGeneration; if(kind == 1) rows[1].lightingUploads[1].snapshotSlot = 6; if(kind == 2) ++rows[1].submitSerial; if(kind == 3) rows[1].lightingUploads[1].activeRecordCount = 1; context.Expect(!SelectRendererEfficiencyLightingBundle(Bundle(), intent, rows, output, error) && output.empty(), "Native stale generation, wrong slice, mixed completion and wrong actual count fail closed."); }
		});
	}
}
