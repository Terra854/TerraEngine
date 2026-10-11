// Tests exercise actual typed command fields and canonical camera invariance.
#include "TestHarness.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
namespace TerraEngine::Tests
{
	void RegisterRendererEfficiencyFixtureControlTests(TestRegistry& registry)
	{
		using namespace Renderer;
		// These controls operate on copied active candidates, before the real budget
		// selector runs. Rejection must preserve both candidates and frame settings.
		registry.Add("Renderer.Efficiency.FixtureControls.LightingCompletePermutationPreservesPayload", [](TestContext& t) {
			UUID a{}, b{}, c{}; a[0]=1; b[0]=2; c[0]=3;
			std::vector<FrameLightCandidate> candidates(3);
			candidates[0].lightId=a; candidates[1].lightId=b; candidates[2].lightId=c;
			for (std::size_t i=0;i<candidates.size();++i) {
				candidates[i].stabilityPreference=float(i+10); candidates[i].candidateFlags=std::uint32_t(i+20);
				candidates[i].gpuLight.colorAndIntensity.w=float(i+30);
				candidates[i].canonicalPosition.offsetMetres[0]=double(i+40);
			}
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId="owned"; intent.controlsApplied=true;
			intent.lightingControls.emplace(); intent.lightingControls->candidateOrder={c,a,b};
			intent.lightingControls->maxUploadedLights=3; intent.lightingControls->maxDynamicLights=2; intent.lightingControls->maxShadowLights=1;
			RenderSettings settings; std::string reason; const std::array<UUID,3> active{a,b,c};
			t.Expect(ValidateRendererEfficiencyFixtureLightingControls(intent,active,reason), "Protected complete active membership qualifies.");
			t.Expect(ApplyRendererEfficiencyFixtureLightingControls(intent,candidates,settings,reason), "Captured candidate order applies.");
			t.Expect(candidates[0].lightId==c && candidates[1].lightId==a && candidates[2].lightId==b && candidates[0].stabilityPreference==12 && candidates[0].candidateFlags==22
				&& candidates[0].gpuLight.colorAndIntensity.w==32 && candidates[0].canonicalPosition.offsetMetres[0]==42, "Real candidate payload and canonical coordinates move with identity.");
			t.Expect(settings.maxUploadedLights==3 && settings.maxDynamicLights==2 && settings.maxShadowLights==1, "Only copied budget inputs change.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.LightingMembershipRejectionIsAtomic", [](TestContext& t) {
			UUID a{}, b{}, unknown{}; a[0]=1; b[0]=2; unknown[0]=3;
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId="owned"; intent.controlsApplied=true; intent.lightingControls.emplace();
			const std::vector<std::vector<UUID>> invalid{{a},{a,a},{a,unknown},{a,b,unknown},{EMPTY_UUID,b}};
			for(const auto& order:invalid) {
				intent.lightingControls->candidateOrder=order; std::vector<FrameLightCandidate> candidates(2); candidates[0].lightId=a; candidates[1].lightId=b;
				RenderSettings settings; const auto original=settings.maxUploadedLights; std::string reason;
				t.Expect(!ApplyRendererEfficiencyFixtureLightingControls(intent,candidates,settings,reason) && !reason.empty(), "Incomplete, duplicated, unavailable or inactive membership rejects.");
				t.Expect(candidates.size()==2 && candidates[0].lightId==a && candidates[1].lightId==b && settings.maxUploadedLights==original, "Rejected controls leave copied inputs intact.");
			}
		});
		registry.Add("Renderer.Efficiency.FixtureControls.LightingBudgetsAndSnapshotBounds", [](TestContext& t) {
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId="owned"; intent.lightingControls.emplace(); std::string reason;
			const auto valid=*intent.lightingControls;
			for(unsigned lane=0;lane<6;++lane) {
				*intent.lightingControls=valid;
				if(lane==0) intent.lightingControls->maxUploadedLights=1025;
				if(lane==1) { intent.lightingControls->maxUploadedLights=1; intent.lightingControls->maxDynamicLights=2; }
				if(lane==2) { intent.lightingControls->maxDynamicLights=1; intent.lightingControls->maxShadowLights=2; }
				if(lane==3) intent.lightingControls->snapshotSlot=8;
				if(lane==4) { intent.lightingControls->snapshotSlot=0; intent.lightingControls->snapshotViewType=RenderViewType::Shadow; }
				if(lane==5) intent.lightingControls->frameSlot=3;
				t.Expect(!ValidateRendererEfficiencyFixtureLightingControls(intent,reason), "Illegal budget or physical snapshot target rejects.");
			}
			*intent.lightingControls=valid; intent.lightingControls->snapshotSlot=7; intent.lightingControls->snapshotViewType=RenderViewType::Game; intent.lightingControls->snapshotOutputId=0; intent.lightingControls->frameSlot=2;
			t.Expect(ValidateRendererEfficiencyFixtureLightingControls(intent,reason), "Real Game output zero, final snapshot slice and final physical frame slot are valid.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.LightingZeroRemainsZero", [](TestContext& t) {
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId="owned"; intent.controlsApplied=true; intent.lightingControls.emplace();
			intent.lightingControls->maxUploadedLights=0; intent.lightingControls->maxDynamicLights=0; intent.lightingControls->maxShadowLights=0;
			std::vector<FrameLightCandidate> candidates; RenderSettings settings; std::string reason;
			t.Expect(ApplyRendererEfficiencyFixtureLightingControls(intent,candidates,settings,reason) && candidates.empty(), "No active lights requires no fabricated candidate.");
			t.Expect(settings.maxUploadedLights==0 && settings.maxDynamicLights==0 && settings.maxShadowLights==0, "Zero budgets survive capture.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.LightingOverCapOrderRetainsAllInputs", [](TestContext& t) {
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId="owned"; intent.controlsApplied=true; intent.lightingControls.emplace();
			std::vector<FrameLightCandidate> candidates(1025);
			for(std::size_t i=0;i<candidates.size();++i) { candidates[i].lightId[0]=std::uint8_t(i%255+1); candidates[i].lightId[1]=std::uint8_t(i/255); }
			for(auto i=candidates.rbegin();i!=candidates.rend();++i) intent.lightingControls->candidateOrder.push_back(i->lightId);
			const auto last=candidates.back().lightId; RenderSettings settings; std::string reason;
			t.Expect(ApplyRendererEfficiencyFixtureLightingControls(intent,candidates,settings,reason) && candidates.size()==1025 && candidates.front().lightId==last, "Ordering preserves all1025 inputs before budget selection.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.LightingRequiresOwnedAppliedIntent", [](TestContext& t) {
			RendererEfficiencyFixtureIntent intent; std::vector<FrameLightCandidate> candidates; RenderSettings settings; std::string reason;
			t.Expect(ApplyRendererEfficiencyFixtureLightingControls(intent,candidates,settings,reason), "Absent controls preserve ordinary frames.");
			intent.lightingControls.emplace();
			t.Expect(!ValidateRendererEfficiencyFixtureLightingControls(intent,reason), "Unowned lighting input rejects.");
			intent.correlation.operationId="owned";
			t.Expect(!ApplyRendererEfficiencyFixtureLightingControls(intent,candidates,settings,reason), "Queued controls cannot apply before protected capture.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.PerViewCasterListsFilterActualLightCommands", [](TestContext& t) {
			UUID first{}, second{}, offCamera{}; first[0] = 1; second[0] = 2; offCamera[0] = 3;
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId = "owned"; intent.controlsApplied = true;
			intent.casterSelections = {{RenderViewType::Scene, 1, {first, offCamera}}, {RenderViewType::Game, 2, {second}}};
			RenderItemRecord a, b, c; a.objectId = first; b.objectId = second; c.objectId = offCamera;
			a.virtualMeshletCount = 4; c.virtualMeshletCount = 7;
			const std::vector<RenderItemRecord> actualLightCommands{a, b, c};
			auto scene = actualLightCommands, game = actualLightCommands;
			FilterRendererEfficiencyFixtureShadowCasters(scene, ResolveRendererEfficiencyFixtureCasterSelection(intent, RenderViewType::Scene, 1));
			FilterRendererEfficiencyFixtureShadowCasters(game, ResolveRendererEfficiencyFixtureCasterSelection(intent, RenderViewType::Game, 2));
			t.Expect(scene.size() == 2 && scene[0].objectId == first && scene[1].objectId == offCamera, "Scene light list retains its exact allowed actual casters, including off-camera candidates.");
			t.Expect(game.size() == 1 && game[0].objectId == second, "Game receives a different real light membership.");
			t.Expect(scene[0].virtualMeshletCount == 4 && scene[1].virtualMeshletCount == 7, "Filtering preserves actual selected geometry lanes.");
			t.Expect(actualLightCommands.size() == 3, "Immutable original commands remain available to ordinary views.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.CasterAllowlistNeverCreatesMissingDraw", [](TestContext& t) {
			UUID queried{}, missing{}; queried[0] = 1; missing[0] = 2;
			RendererEfficiencyFixtureCasterSelection selection{RenderViewType::Scene, 1, {missing}};
			RenderItemRecord command; command.objectId = queried;
			std::vector<RenderItemRecord> commands{command};
			FilterRendererEfficiencyFixtureShadowCasters(commands, &selection);
			t.Expect(commands.empty(), "An allowlist cannot invent geometry absent from the light spatial query.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.UnappliedAndOtherOutputsRemainUnfiltered", [](TestContext& t) {
			UUID id{}; id[0] = 1;
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId = "owned";
			intent.casterSelections = {{RenderViewType::Scene, 1, {id}}};
			t.Expect(ResolveRendererEfficiencyFixtureCasterSelection(intent, RenderViewType::Scene, 1) == nullptr, "Queued control cannot filter an unacknowledged capture.");
			intent.controlsApplied = true;
			t.Expect(ResolveRendererEfficiencyFixtureCasterSelection(intent, RenderViewType::Game, 2) == nullptr
				&& ResolveRendererEfficiencyFixtureCasterSelection(intent, RenderViewType::Scene, 0) == nullptr, "Other actual output keys remain uncontrolled.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.CasterAllowlistRejectsAmbiguityAndBounds", [](TestContext& t) {
			UUID id{}; id[0] = 1;
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId = "owned";
			intent.casterSelections = {{RenderViewType::Scene, 1, {id}}}; std::string reason;
			t.Expect(ValidateRendererEfficiencyFixtureCasterSelections(intent, reason), "One bounded actual output is valid.");
			const auto valid = intent;
			intent.casterSelections.push_back(intent.casterSelections[0]);
			t.Expect(!ValidateRendererEfficiencyFixtureCasterSelections(intent, reason), "Duplicate output lists reject atomically.");
			intent = valid; intent.casterSelections[0].objectIds.push_back(id);
			t.Expect(!ValidateRendererEfficiencyFixtureCasterSelections(intent, reason), "Duplicate caster coverage rejects.");
			intent = valid; intent.casterSelections[0].objectIds[0] = EMPTY_UUID;
			t.Expect(!ValidateRendererEfficiencyFixtureCasterSelections(intent, reason), "Unknown caster identity rejects.");
			intent = valid; intent.casterSelections[0].objectIds.clear();
			t.Expect(!ValidateRendererEfficiencyFixtureCasterSelections(intent, reason), "Empty requested list rejects.");
			intent = valid; intent.casterSelections[0].objectIds.resize(4097, id);
			t.Expect(!ValidateRendererEfficiencyFixtureCasterSelections(intent, reason), "Oversized membership rejects before processing.");
			intent = valid; intent.casterSelections[0].outputId = 2;
			t.Expect(!ValidateRendererEfficiencyFixtureCasterSelections(intent, reason), "Scene2 is not a supported actual output.");
			intent = valid; intent.correlation.operationId.clear();
			t.Expect(!ValidateRendererEfficiencyFixtureCasterSelections(intent, reason), "Unowned operation cannot request fixture filters.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.CasterAllowlistRequiresAllProtectedTargets", [](TestContext& t) {
			UUID first{}, second{}; first[0] = 1; second[0] = 2;
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId = "owned";
			intent.casterSelections = {{RenderViewType::Scene, 1, {first}}, {RenderViewType::Game, 2, {second}}};
			RendererEfficiencyFixtureTargetProof a, b; a.objectId = first; b.objectId = second; a.lifetimeGeneration = b.lifetimeGeneration = 1;
			std::vector<RendererEfficiencyFixtureTargetProof> targets{a}; std::string reason;
			t.Expect(!ValidateRendererEfficiencyFixtureCasterSelections(intent, targets, reason), "One missing Game identity rejects the entire captured control set.");
			targets.push_back(b); t.Expect(ValidateRendererEfficiencyFixtureCasterSelections(intent, targets, reason), "All protected actual identities authorize the copied control set.");
			targets[1].lifetimeGeneration = 0; t.Expect(!ValidateRendererEfficiencyFixtureCasterSelections(intent, targets, reason), "Unavailable target lifetime cannot establish live membership.");
			const std::array<RendererEfficiencyFixtureViewOrderEntry, 1> onlyScene{{{RenderViewType::Scene, 1}}};
			std::vector<std::size_t> order{9};
			t.Expect(!ResolveRendererEfficiencyFixtureViewOrder(intent, onlyScene, order, reason) && order == std::vector<std::size_t>{9},
				"Unavailable Game output rejects both lists without publishing partial output order.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.ReverseActualViewPermutation", [](TestContext& t) {
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId = "owned-validation";
			intent.viewOrder = {{RenderViewType::Game, 2}, {RenderViewType::Scene, 1}};
			const std::array<RendererEfficiencyFixtureViewOrderEntry, 2> actual{{{RenderViewType::Scene, 1}, {RenderViewType::Game, 2}}};
			std::vector<std::size_t> indices; std::string reason;
			t.Expect(ResolveRendererEfficiencyFixtureViewOrder(intent, actual, indices, reason), "An exact actual output permutation is supported.");
			t.Expect(indices == std::vector<std::size_t>({1, 0}), "Actual execution owners reverse, not diagnostic labels.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.DuplicateMissingAndAmbiguousViewControlsReject", [](TestContext& t) {
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId = "owned-validation";
			const std::array<RendererEfficiencyFixtureViewOrderEntry, 2> actual{{{RenderViewType::Scene, 1}, {RenderViewType::Game, 2}}};
			std::vector<std::size_t> indices{9}; std::string reason;
			intent.viewOrder = {{RenderViewType::Scene, 1}, {RenderViewType::Scene, 1}};
			t.Expect(!ResolveRendererEfficiencyFixtureViewOrder(intent, actual, indices, reason), "Duplicate view cannot invent a third execution.");
			intent.viewOrder = {{RenderViewType::Scene, 1}};
			t.Expect(!ResolveRendererEfficiencyFixtureViewOrder(intent, actual, indices, reason), "Missing actual view rejects whole permutation.");
			intent.viewOrder.clear(); intent.referenceOriginDelta = Scene::WorldDelta{};
			intent.viewOrigins = {{RenderViewType::Scene, 1, Scene::WorldDelta{}}};
			t.Expect(!ResolveRendererEfficiencyFixtureViewOrder(intent, actual, indices, reason), "Shared and per-view origins cannot conflict.");
			t.Expect(indices == std::vector<std::size_t>({9}), "Rejected controls publish no partial owner order.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.EvidenceResetDropsRowsAndAdvancesCursor", [](TestContext& t) {
			RendererEfficiencyEvidenceBatch batch; batch.batchSequence = 40; batch.rows.emplace_back();
			ResetRendererEfficiencyFixtureEvidenceBatch(batch);
			t.Expect(batch.rows.empty(), "Earlier renderer lifetime rows cannot survive reset.");
			t.ExpectEqual(batch.batchSequence, std::uint64_t(41), "Existing client cursor cannot suppress the next lifecycle.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.ControlAckRequiresActualMatchingCommand", [](TestContext& t) {
			RendererEfficiencyFixtureIntent intent; intent.controlsApplied = true; intent.controlGeneration = 8;
			intent.correlation.operationId = "owned"; intent.correlation.fixtureStepId = "origin";
			intent.correlation.settingsGeneration = 4; intent.correlation.rendererSessionGeneration = 2;
			RendererEfficiencyEvidence row; row.fixtureCorrelation = intent.correlation; row.fixtureControlGeneration = 8;
			row.status = RendererEfficiencyEvidenceStatus::Valid;
			t.Expect(!HasRendererEfficiencyFixtureControlAcknowledgment(std::span(&row, 1), intent), "Queued/captured controls are never an actual emitted command.");
			row.commandSerial = 20; row.status = RendererEfficiencyEvidenceStatus::Pending;
			t.Expect(!HasRendererEfficiencyFixtureControlAcknowledgment(std::span(&row, 1), intent), "Pending recording is not terminal evidence.");
			row.status = RendererEfficiencyEvidenceStatus::Unavailable; row.fixtureControlGeneration = 7;
			t.Expect(!HasRendererEfficiencyFixtureControlAcknowledgment(std::span(&row, 1), intent), "Older controls cannot acknowledge the new order.");
			row.fixtureControlGeneration = 8;
			t.Expect(HasRendererEfficiencyFixtureControlAcknowledgment(std::span(&row, 1), intent), "Real recorded-submit abort terminal receipt acknowledges emission without claiming valid pixels.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.StoppedOperationAndReusedBackendSessionRowsReject", [](TestContext& t) {
			RendererEfficiencyOptionsSnapshot options; options.rendererSessionGeneration = 2; options.optionsSessionGeneration = 9;
			RendererEfficiencyFixtureIntent intent; intent.correlation.operationId = "new-operation";
			RendererEfficiencyEvidence row; row.effectiveOptions = options; row.fixtureCorrelation.operationId = "old-operation";
			t.Expect(!IsCurrentRendererEfficiencyFixtureEvidence(row, options, intent), "Stopped fixture rows cannot be returned by the next session.");
			row.fixtureCorrelation.operationId = intent.correlation.operationId; row.effectiveOptions.optionsSessionGeneration = 8;
			t.Expect(!IsCurrentRendererEfficiencyFixtureEvidence(row, options, intent), "Reused numeric backend generation cannot revive an older options lifetime.");
			row.effectiveOptions.optionsSessionGeneration = 9;
			t.Expect(IsCurrentRendererEfficiencyFixtureEvidence(row, options, intent), "Current actual lifetime evidence remains readable.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.CookedLodChangesActualCommand", [](TestContext& t) {
			RendererEfficiencyFixtureTargetProof proof; proof.objectId[0] = 1; proof.meshId[0] = 2;
			proof.lifetimeGeneration = 3; proof.resourceGeneration = 4; proof.meshletCapacity = 12;
			proof.ownerGeometrySupported = true; proof.detailLevels = {{0, 0, 8}, {1, 8, 4}};
			RendererEfficiencyFixtureShadowSelection selection; selection.objectId = proof.objectId; selection.meshId = proof.meshId;
			selection.lifetimeGeneration = 3; selection.resourceGeneration = 4; selection.lodIndex = 1;
			RenderItemRecord command; command.objectId = proof.objectId; command.meshId = proof.meshId; command.objectLifetimeGeneration = 3;
			t.Expect(ApplyRendererEfficiencyFixtureSelection(command, selection, proof), "Current cooked LOD must apply to actual command.");
			t.ExpectEqual(command.selectedVirtualLod, std::uint32_t(1), "Actual LOD selector changes.");
			t.ExpectEqual(command.virtualMeshletOffset, std::uint32_t(8), "Actual cooked offset changes.");
			t.ExpectEqual(command.virtualMeshletCount, std::uint32_t(4), "Actual cooked count changes.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.StaleProofAndOutOfRangeRejectAtomically", [](TestContext& t) {
			RendererEfficiencyFixtureTargetProof proof; proof.objectId[0] = 1; proof.meshId[0] = 2;
			proof.lifetimeGeneration = 3; proof.resourceGeneration = 4; proof.meshletCapacity = 12;
			proof.ownerGeometrySupported = true; proof.detailLevels = {{0, 0, 8}, {1, 8, 4}};
			RendererEfficiencyFixtureShadowSelection selection; selection.objectId = proof.objectId; selection.meshId = proof.meshId;
			selection.lifetimeGeneration = 3; selection.resourceGeneration = 4; selection.kind = RendererEfficiencyFixtureSelectionKind::MeshletRange;
			selection.lodIndex = 1; selection.meshletOffset = 9; selection.meshletCount = 4;
			RenderItemRecord command; command.objectId = proof.objectId; command.meshId = proof.meshId; command.objectLifetimeGeneration = 3;
			command.virtualMeshletOffset = 5; command.virtualMeshletCount = 2;
			t.Expect(!ApplyRendererEfficiencyFixtureSelection(command, selection, proof), "Range cannot exceed actual selected cooked level.");
			t.ExpectEqual(command.virtualMeshletOffset, std::uint32_t(5), "Rejected range changes no live command fields.");
			selection.meshletCount = 2; selection.resourceGeneration = 5;
			t.Expect(!ApplyRendererEfficiencyFixtureSelection(command, selection, proof), "Stale asset publication is rejected.");
			selection.resourceGeneration = 4;
			t.Expect(ApplyRendererEfficiencyFixtureSelection(command, selection, proof), "Real bounded subset changes emitted command.");
			t.ExpectEqual(command.virtualMeshletOffset, std::uint32_t(9), "Selected subset is consumed.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.ReferenceRebasePreservesPhysicalCamera", [](TestContext& t) {
			Scene::WorldPosition camera; camera.offsetMetres = {10, 20, 30};
			Scene::WorldDelta delta; delta.offsetMetres = {128, -64, 32};
			RenderReferenceFrame frame; frame.canonicalOrigin = camera;
			glm::mat4 view{1}; glm::vec3 localCamera{0};
			t.Expect(RebaseRendererEfficiencyFixtureView(camera, delta, frame, view, localCamera), "Finite canonical delta changes only renderer origin.");
			t.Expect(frame.canonicalOrigin != camera, "Reference origin actually changes.");
			t.Expect(glm::length(localCamera - glm::vec3(-128, 64, -32)) < 0.0001f, "Canonical camera is correctly localized.");
			const glm::vec4 localizedObject = glm::vec4(glm::vec3(5, 6, 7) + localCamera, 1);
			t.Expect(glm::length(view * localizedObject - glm::vec4(5, 6, 7, 1)) < 0.0001f, "View-space physical geometry is unchanged.");
		});
		registry.Add("Renderer.Efficiency.FixtureControls.RejectedOriginLeavesBothViewsUnchanged", [](TestContext& t) {
			Scene::WorldPosition camera; camera.cell.x = 4; camera.offsetMetres = {10, 20, 30};
			Scene::WorldDelta invalid; invalid.offsetMetres.x = std::numeric_limits<double>::quiet_NaN();
			for (int viewIndex = 0; viewIndex < 2; ++viewIndex)
			{
				RenderReferenceFrame frame; frame.canonicalOrigin = camera; frame.viewId = viewIndex + 1;
				glm::mat4 view(1); glm::vec3 local(0);
				t.Expect(!RebaseRendererEfficiencyFixtureView(camera, invalid, frame, view, local), "Nonfinite delta rejects each affected view.");
				t.Expect(frame.canonicalOrigin == camera && local == glm::vec3(0), "Rejected controls change neither canonical nor local camera coordinates.");
				t.Expect(view == glm::mat4(1), "Rejected origin leaves the full view matrix unchanged.");
			}
		});
		registry.Add("Renderer.Efficiency.FixtureControls.LifetimeAndMissingCookedLevelCannotFabricateSelection", [](TestContext& t) {
			RendererEfficiencyFixtureTargetProof proof; proof.objectId[0] = 1; proof.meshId[0] = 2;
			proof.lifetimeGeneration = 3; proof.resourceGeneration = 4; proof.meshletCapacity = 12;
			proof.ownerGeometrySupported = true; proof.detailLevels = {{0, 0, 8}};
			RendererEfficiencyFixtureShadowSelection selection; selection.objectId = proof.objectId; selection.meshId = proof.meshId;
			selection.lifetimeGeneration = 3; selection.resourceGeneration = 4; selection.lodIndex = 1;
			RenderItemRecord command; command.objectId = proof.objectId; command.meshId = proof.meshId; command.objectLifetimeGeneration = 3;
			command.virtualMeshletCount = 8;
			t.Expect(!ApplyRendererEfficiencyFixtureSelection(command, selection, proof), "Unavailable cooked LOD cannot be invented from its index.");
			selection.lodIndex = 0; command.objectLifetimeGeneration = 9;
			t.Expect(!ApplyRendererEfficiencyFixtureSelection(command, selection, proof), "Reused object identity cannot consume the previous lifetime's control.");
			t.ExpectEqual(command.virtualMeshletCount, std::uint32_t(8), "Rejected identity preserves the original emitted range.");
		});
	}
}
