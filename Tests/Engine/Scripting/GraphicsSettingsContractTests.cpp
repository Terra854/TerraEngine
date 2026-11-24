// GraphicsSettingsContractTests.cpp - native requests behind the managed graphics API.
// These cases exercise the actual typed controller with a copied capability and
// preset catalog. Installed managed/export checks qualify the complete bridge.
#include "TestHarness.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsController.h"
#include "Renderer/RenderTypes.h"
#include "Scripting/ScriptGlue.h"
#include "Renderer/GraphicsSettings/GraphicsStartupStore.h"
#include "Renderer/GraphicsSettings/PostProcessColorSettings.h"
#include "Scripting/GraphicsPreferenceStore.h"

#include <algorithm>
#include <array>
#include <optional>
#include <atomic>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer::GraphicsSettings;

		struct GraphicsContractFixture
		{
			std::array<GraphicsSettingDescriptor, 4> descriptors;
			std::array<GraphicsTarget, 1> targets{};
			GraphicsPreparationContext preparation;
			GraphicsSettingsController controller;

			GraphicsContractFixture()
			{
				auto& filter = descriptors[0];
				filter.id = GraphicsSettingId::ShadowFilteringSize;
				filter.valueKind = GraphicsValueKind::Unsigned32;
				filter.defaultValue = std::uint32_t{3};
				filter.minimum = std::uint32_t{1};
				filter.maximum = std::uint32_t{16};
				auto& lights = descriptors[1];
				lights.id = GraphicsSettingId::MaxUploadedLights;
				lights.valueKind = GraphicsValueKind::Unsigned32;
				lights.defaultValue = std::uint32_t{512};
				lights.minimum = std::uint32_t{1};
				lights.maximum = std::uint32_t{4096};
				auto& validation = descriptors[2];
				validation.id = GraphicsSettingId::ValidationEnabled;
				validation.valueKind = GraphicsValueKind::Boolean;
				validation.defaultValue = false;
				validation.activation = GraphicsActivation::NextLaunch;
				auto& exposure = descriptors[3];
				exposure.id = GraphicsSettingId::FixedExposureCompensation;
				exposure.valueKind = GraphicsValueKind::Scalar;
				exposure.defaultValue = 0.0f;
				exposure.minimum = -8.0f;
				exposure.maximum = 8.0f;
				exposure.cameraScope = true;
				for(auto& descriptor : descriptors)
				{
					descriptor.available = true;
					descriptor.gameScope = true;
				}
				preparation.lifecycle = {11u, 12u, 13u, 14u};
				preparation.descriptors = descriptors;
				preparation.validTargets = targets;
				preparation.frameId = 41u;
				controller.Invalidate(preparation);
			}

			GraphicsSettingsRequest Patch(std::initializer_list<GraphicsSettingValue> values) const
			{
				GraphicsSettingsRequest request;
				request.lifecycle = preparation.lifecycle;
				request.baseRevision = controller.Snapshot()->revision;
				request.patches = values;
				return request;
			}
		};

		const GraphicsValue* Value(const GraphicsSettingsSnapshot& snapshot, GraphicsSettingId id)
		{
			const auto found = std::find_if(snapshot.values.begin(), snapshot.values.end(), [id](const auto& field) { return field.id == id; });
			return found == snapshot.values.end() ? nullptr : &found->value;
		}

		GraphicsRequestReceipt Commit(TestContext& context, GraphicsContractFixture& fixture, GraphicsSettingsRequest request)
		{
			fixture.controller.Enqueue(std::move(request));
			auto candidate = fixture.controller.Prepare(fixture.preparation);
			context.Expect(candidate.has_value(), "A valid native contract request must prepare a complete candidate.");
			return fixture.controller.Publish(std::move(*candidate), fixture.preparation);
		}

		GraphicsTarget CameraScope()
		{
			GraphicsCameraTarget camera;
			camera.cameraId[0] = 21u;
			camera.sceneGeneration = 22u;
			camera.objectLifetimeGeneration = 23u;
			camera.componentGeneration = 24u;
			camera.bindingGeneration = 25u;
			GraphicsTarget target;
			target.scope = GraphicsScope::Camera;
			target.camera = camera;
			target.view = GraphicsViewIdentity{RenderViewType::Game, 2u, 26u, RenderOutputKind::GameView};
			return target;
		}

		// Each case owns a unique real directory. Cleanup cannot touch a caller's
		// game data, and reopening the factory exercises the durable JSON boundary.
		struct GraphicsStoreDirectory
		{
			std::filesystem::path root;
			GraphicsStoreDirectory()
			{
				static std::atomic<std::uint64_t> sequence{0};
				root = std::filesystem::temp_directory_path() / ("TerraEngine-GraphicsContract-"
					+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
					+ "-" + std::to_string(++sequence));
				std::filesystem::create_directory(root);
			}
			~GraphicsStoreDirectory()
			{
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}
		};

		GraphicsStartupTransaction StartupTransaction()
		{
			GraphicsStartupTransaction transaction;
			transaction.requestId = std::numeric_limits<std::uint64_t>::max() - 41u;
			transaction.request.operation = GraphicsRequestOperation::StageNextLaunch;
			transaction.request.baseRevision = std::numeric_limits<std::uint64_t>::max() - 13u;
			transaction.request.patches = {{GraphicsSettingId::ValidationEnabled, true},
				{GraphicsSettingId::TextureBudgetMegabytes, std::uint64_t{9007199254740993ull}},
				{GraphicsSettingId::FixedExposureCompensation, std::bit_cast<float>(0x3f000001u)}};
			transaction.fieldPreconditions = {{GraphicsSettingId::ValidationEnabled, false}};
			return transaction;
		}
	}

	void RegisterGraphicsSettingsStoreTests(TestRegistry& registry)
	{
		registry.Add("Scripting.GraphicsSettings.Store.ReopenPreservesOriginalIdentityAndExactVariants", [](TestContext& context) {
			GraphicsStoreDirectory directory;
			const auto transaction = StartupTransaction();
			auto store = OpenGraphicsStartupStore(directory.root, "contract-game");
			context.Expect(store->Stage(transaction, 0u).succeeded, "A complete startup transaction must stage durably.");
			store = OpenGraphicsStartupStore(directory.root, "contract-game");
			GraphicsStartupStoreSnapshot snapshot;
			context.Expect(store->Read(snapshot).succeeded && snapshot.pending.has_value(), "A new factory must recover the staged transaction.");
			context.Expect(snapshot.pending->transaction.requestId == transaction.requestId
				&& snapshot.pending->transaction.request.baseRevision == transaction.request.baseRevision
				&& snapshot.pending->transaction.request.patches == transaction.request.patches
				&& snapshot.pending->transaction.fieldPreconditions == transaction.fieldPreconditions,
				"The persisted original request, checked preconditions, float bits, and full-width integers must round-trip exactly.");
			context.Expect(store->Claim(transaction.requestId, 77u, snapshot.storeRevision).succeeded, "The original request must be claimable once.");
			store = OpenGraphicsStartupStore(directory.root, "contract-game");
			context.Expect(store->Read(snapshot).succeeded && snapshot.pending->claimState == GraphicsStartupClaimState::Claimed
				&& snapshot.pending->claimGeneration == 77u, "Claim ownership must survive another process opening the store.");
			GraphicsRequestReceipt receipt;
			receipt.requestId = transaction.requestId; receipt.state = GraphicsRequestState::Applied;
			context.Expect(store->Promote(transaction.requestId, 77u, snapshot.storeRevision, transaction.request.patches, receipt).succeeded,
				"Only the matching claimed startup candidate may promote its whole profile.");
			context.Expect(store->Read(snapshot).succeeded && !snapshot.pending && snapshot.committedProfile == transaction.request.patches
				&& snapshot.terminalReceipts.size() == 1u && snapshot.terminalReceipts[0].requestId == transaction.requestId
				&& snapshot.terminalReceipts[0].state == GraphicsRequestState::Applied,
				"Committed profile and original Applied receipt must become durable together.");
		});

		registry.Add("Scripting.GraphicsSettings.Store.StaleRevisionAndWrongClaimPreserveCommittedDocument", [](TestContext& context) {
			GraphicsStoreDirectory directory;
			auto store = OpenGraphicsStartupStore(directory.root, "contract-game");
			const auto transaction = StartupTransaction();
			const auto staged = store->Stage(transaction, 0u);
			context.Expect(staged.succeeded, "The baseline startup transaction must stage.");
			context.Expect(store->Claim(transaction.requestId, 19u, 0u).reason == GraphicsRequestReason::StaleRevision,
				"An old durable revision must not claim a newer pending record.");
			context.Expect(store->Claim(transaction.requestId, 19u, staged.storeRevision).succeeded, "The current owner must claim the pending record.");
			GraphicsStartupStoreSnapshot snapshot;
			context.Expect(store->Read(snapshot).succeeded, "The claimed baseline must be readable.");
			const auto revision = snapshot.storeRevision;
			GraphicsRequestReceipt receipt; receipt.requestId = transaction.requestId; receipt.state = GraphicsRequestState::Applied;
			context.Expect(store->Promote(transaction.requestId, 20u, revision, transaction.request.patches, receipt).reason == GraphicsRequestReason::StaleLifetime,
				"A competing startup generation must not promote another candidate.");
			context.Expect(store->Read(snapshot).succeeded && snapshot.storeRevision == revision && snapshot.committedProfile.empty()
				&& snapshot.pending && snapshot.pending->claimGeneration == 19u && snapshot.terminalReceipts.empty(),
				"Rejected ownership must preserve every durable publication field.");
		});

		registry.Add("Scripting.GraphicsSettings.Store.InterruptedClaimTerminatesOriginalReceiptWithoutReplay", [](TestContext& context) {
			GraphicsStoreDirectory directory;
			const auto transaction = StartupTransaction();
			auto store = OpenGraphicsStartupStore(directory.root, "contract-game");
			const auto staged = store->Stage(transaction, 0u);
			context.Expect(staged.succeeded && store->Claim(transaction.requestId, 55u, staged.storeRevision).succeeded, "The interrupted fixture must first own a real durable claim.");
			store = OpenGraphicsStartupStore(directory.root, "contract-game");
			GraphicsStartupStoreSnapshot snapshot;
			context.Expect(store->Read(snapshot).succeeded && snapshot.pending.has_value(), "The abandoned claim must remain observable after reopen.");
			GraphicsRequestReceipt receipt; receipt.requestId = transaction.requestId;
			receipt.state = GraphicsRequestState::Rejected; receipt.reason = GraphicsRequestReason::InterruptedStartup;
			context.Expect(store->Reject(transaction.requestId, snapshot.storeRevision, receipt).succeeded, "Recovery must consume the original abandoned claim with a terminal reason.");
			store = OpenGraphicsStartupStore(directory.root, "contract-game");
			context.Expect(store->Read(snapshot).succeeded && !snapshot.pending && snapshot.committedProfile.empty()
				&& snapshot.terminalReceipts.size() == 1u && snapshot.terminalReceipts[0].reason == GraphicsRequestReason::InterruptedStartup,
				"The next launch must see the terminal original receipt and no transaction to replay.");
		});

		registry.Add("Scripting.GraphicsSettings.Store.CancelAndMalformedSchemaCannotReplacePriorProfile", [](TestContext& context) {
			GraphicsStoreDirectory directory;
			auto store = OpenGraphicsStartupStore(directory.root, "contract-game");
			const auto transaction = StartupTransaction();
			const auto staged = store->Stage(transaction, 0u);
			GraphicsRequestReceipt receipt; receipt.requestId = transaction.requestId;
			receipt.state = GraphicsRequestState::Cancelled; receipt.reason = GraphicsRequestReason::ExplicitCancellation;
			context.Expect(staged.succeeded && store->Cancel(transaction.requestId, staged.storeRevision, receipt).succeeded, "A still-pending record must support explicit cancellation.");
			GraphicsStartupStoreSnapshot snapshot;
			context.Expect(store->Read(snapshot).succeeded && !snapshot.pending && snapshot.committedProfile.empty(), "Cancellation must not promote the proposed values.");
			// Corrupt only the exact file this case created. Failed decode must not
			// rewrite disk or replace the caller's last complete copied snapshot.
			std::filesystem::path document;
			for(const auto& entry : std::filesystem::recursive_directory_iterator(directory.root))
				if(entry.path().filename() == "startup.json") document = entry.path();
			context.Expect(!document.empty(), "The real store must have created its durable startup document.");
			{ std::ofstream output(document, std::ios::binary | std::ios::trunc); output << "{\"schemaVersion\":999}"; }
			const auto revision = snapshot.storeRevision;
			context.Expect(store->Read(snapshot).reason == GraphicsRequestReason::StorageFailed && snapshot.storeRevision == revision
				&& snapshot.terminalReceipts.size() == 1u && snapshot.terminalReceipts[0].state == GraphicsRequestState::Cancelled,
				"Unknown schema must preserve the caller's prior complete snapshot.");
			std::ifstream input(document, std::ios::binary); const std::string bytes{std::istreambuf_iterator<char>{input}, {}};
			context.Expect(bytes == "{\"schemaVersion\":999}", "Decode failure must leave the rejected durable input intact for diagnosis.");
		});

		registry.Add("Scripting.GraphicsSettings.Store.PreferencesAreNamedIdentityScopedAndRevisionChecked", [](TestContext& context) {
			GraphicsStoreDirectory directory;
			const auto values = StartupTransaction().request.patches;
			auto preferences = TerraEngine::Scripting::OpenGraphicsPreferenceStore(directory.root, "game-a");
			const auto saved = preferences->Save("player-choice", values, 0u);
			context.Expect(saved.succeeded, "Explicit preferences must save without changing an effective controller.");
			preferences = TerraEngine::Scripting::OpenGraphicsPreferenceStore(directory.root, "game-a");
			TerraEngine::Scripting::GraphicsPreferenceSnapshot snapshot;
			context.Expect(preferences->Read("player-choice", snapshot).succeeded && snapshot.values == values && snapshot.revision == saved.storeRevision,
				"A new preference factory must recover exact typed values under the same name and identity.");
			context.Expect(preferences->Save("player-choice", {}, 0u).reason == GraphicsRequestReason::StaleRevision,
				"Stale preference saves must not erase a newer player choice.");
			context.Expect(preferences->Read("other-choice", snapshot).succeeded && snapshot.values.empty() && snapshot.revision == 0u,
				"A different named preference must remain independent.");
			auto otherGame = TerraEngine::Scripting::OpenGraphicsPreferenceStore(directory.root, "game-b");
			context.Expect(otherGame->Read("player-choice", snapshot).succeeded && snapshot.values.empty(), "A different product identity cannot inherit another game's preference.");
			GraphicsStartupStoreSnapshot startup;
			context.Expect(OpenGraphicsStartupStore(directory.root, "game-a")->Read(startup).succeeded && !startup.pending && startup.committedProfile.empty(),
				"Saving named choices must not create startup transactions or authored/effective profiles.");
		});
	}

	void RegisterGraphicsSettingsContractTests(TestRegistry& registry)
	{
		RegisterGraphicsSettingsStoreTests(registry);
		registry.Add("Scripting.GraphicsSettings.Contract.GT7IsAnIndependentToneOption", [](TestContext& context) {
			// Managed discovery and requests use these same stable unsigned values.
			std::vector<GraphicsSettingDescriptor> catalog;
			AppendPostProcessColorDescriptors(catalog, true);
			const auto tone = std::find_if(catalog.begin(), catalog.end(), [](const auto& field) {
				return field.id == GraphicsSettingId::ToneMappingMode;
			});
			context.Expect(tone != catalog.end() && tone->available && tone->defaultValue == GraphicsValue{0u}
				&& tone->options == std::vector<GraphicsValue>{0u, 1u, 2u, 3u},
				"Discover GT7 alongside the three stable existing modes with the same Legacy default.");
			const std::array values{GraphicsSettingValue{GraphicsSettingId::ToneMappingMode, 3u},
				GraphicsSettingValue{GraphicsSettingId::ExposureMode, 3u}};
			const auto resolved = ResolvePostProcessColorSettings(values);
			context.Expect(resolved.tone == ToneMappingMode::GT7 && resolved.exposure == ExposureMode::Legacy,
				"GT7's wire value enables tone mapping without extending the exposure enum.");
		});
		registry.Add("Scripting.GraphicsSettings.Contract.MixedStartupBatchLeavesEveryLiveFieldUntouched", [](TestContext& context) {
			GraphicsContractFixture fixture;
			context.Expect(Commit(context, fixture, fixture.Patch({{GraphicsSettingId::ShadowFilteringSize, std::uint32_t{5}}})).state
				== GraphicsRequestState::Applied, "The baseline filter must apply before testing a mixed startup batch.");
			const auto original = fixture.controller.Snapshot();
			const auto admitted = fixture.controller.Enqueue(fixture.Patch({
				{GraphicsSettingId::ShadowFilteringSize, std::uint32_t{7}}, {GraphicsSettingId::ValidationEnabled, true}}));
			context.Expect(!fixture.controller.Prepare(fixture.preparation),
				"A startup-only field must prevent live preparation of the entire mixed batch.");
			const auto receipt = fixture.controller.GetReceipt(admitted.requestId);
			context.Expect(receipt && receipt->state == GraphicsRequestState::RestartRequired
				&& receipt->activation == GraphicsActivation::NextLaunch && receipt->reason == GraphicsRequestReason::RestartRequired,
				"The caller must observe that explicit next-launch staging is required.");
			context.Expect(fixture.controller.Snapshot() == original && !fixture.controller.HasPendingWork(),
				"A regular runtime apply must leave both fields intact and must not silently schedule startup work.");
		});

		registry.Add("Scripting.GraphicsSettings.Contract.PendingRebuildSeparatesAcceptedAndEffectiveValues", [](TestContext& context) {
			GraphicsContractFixture fixture;
			fixture.descriptors[0].activation = GraphicsActivation::Rebuild;
			context.Expect(Commit(context, fixture, fixture.Patch({{GraphicsSettingId::ShadowFilteringSize, std::uint32_t{3}}})).state
				== GraphicsRequestState::Applied, "A baseline must publish before a second target rebuild.");
			const auto original = fixture.controller.Snapshot();
			const auto admitted = fixture.controller.Enqueue(fixture.Patch({{GraphicsSettingId::ShadowFilteringSize, std::uint32_t{7}}}));
			auto candidate = fixture.controller.Prepare(fixture.preparation);
			const auto pending = fixture.controller.GetReceipt(admitted.requestId);
			context.Expect(candidate && pending && pending->state == GraphicsRequestState::PendingRebuild
				&& candidate->snapshot.revision > original->revision && fixture.controller.Snapshot() == original,
				"Accepted rebuilding values must remain separate from the currently published image settings.");
			++fixture.preparation.frameId;
			const auto applied = fixture.controller.Publish(std::move(*candidate), fixture.preparation);
			context.Expect(applied.state == GraphicsRequestState::Applied && applied.requestId == admitted.requestId
				&& applied.effectiveRevision == fixture.controller.Snapshot()->revision
				&& applied.appliedFrameId == fixture.preparation.frameId,
				"Only protected publication may expose a matching effective revision and application frame.");
		});

		registry.Add("Scripting.GraphicsSettings.Contract.InvalidBatchPreservesValidSiblingAndRevision", [](TestContext& context) {
			GraphicsContractFixture fixture;
			const auto original = fixture.controller.Snapshot();
			const auto admitted = fixture.controller.Enqueue(fixture.Patch({
				{GraphicsSettingId::ShadowFilteringSize, std::uint32_t{7}}, {GraphicsSettingId::MaxUploadedLights, std::uint32_t{5000}}}));
			context.Expect(!fixture.controller.Prepare(fixture.preparation),
				"One over-budget light value must reject the entire grouped request.");
			const auto receipt = fixture.controller.GetReceipt(admitted.requestId);
			context.Expect(receipt && receipt->state == GraphicsRequestState::Rejected
				&& receipt->reason == GraphicsRequestReason::InvalidValue && fixture.controller.Snapshot() == original,
				"The valid filter sibling must not apply when the batch's lighting value is invalid.");
		});

		registry.Add("Scripting.GraphicsSettings.Contract.CameraResetResumesCurrentGameInheritance", [](TestContext& context) {
			GraphicsContractFixture fixture;
			context.Expect(Commit(context, fixture, fixture.Patch({{GraphicsSettingId::FixedExposureCompensation, 1.0f}})).state
				== GraphicsRequestState::Applied, "The game exposure must publish before adding a camera override.");
			const auto target = CameraScope();
			std::array targets{target};
			fixture.preparation.validTargets = targets;
			auto cameraPatch = fixture.Patch({{GraphicsSettingId::FixedExposureCompensation, 2.0f}});
			cameraPatch.target = target;
			cameraPatch.baseRevision = 0u;
			context.Expect(Commit(context, fixture, std::move(cameraPatch)).state == GraphicsRequestState::Applied,
				"A scoped camera patch must publish without changing the game exposure.");
			context.Expect(Commit(context, fixture, fixture.Patch({{GraphicsSettingId::FixedExposureCompensation, 3.0f}})).state
				== GraphicsRequestState::Applied, "A newer game exposure must publish while the camera override remains active.");
			const auto beforeReset = fixture.controller.Snapshot(target);
			context.Expect(beforeReset && Value(*beforeReset, GraphicsSettingId::FixedExposureCompensation)
				&& std::get<float>(*Value(*beforeReset, GraphicsSettingId::FixedExposureCompensation)) == 2.0f,
				"A camera override must survive a later game-wide value change.");
			GraphicsSettingsRequest reset;
			reset.operation = GraphicsRequestOperation::Reset;
			reset.target = target;
			reset.lifecycle = fixture.preparation.lifecycle;
			reset.baseRevision = beforeReset->revision;
			reset.resetSettings = {GraphicsSettingId::FixedExposureCompensation};
			context.Expect(Commit(context, fixture, std::move(reset)).state == GraphicsRequestState::Applied,
				"Reset must remove the selected runtime camera override at the publication boundary.");
			const auto inherited = fixture.controller.Snapshot(target);
			context.Expect(inherited && std::get<float>(*Value(*inherited, GraphicsSettingId::FixedExposureCompensation)) == 3.0f,
				"A camera reset must inherit the current game value rather than the old value or catalog default.");
			context.Expect(Commit(context, fixture, fixture.Patch({{GraphicsSettingId::FixedExposureCompensation, 4.0f}})).state
				== GraphicsRequestState::Applied, "A later game change must remain usable after resetting the camera.");
			const auto updated = fixture.controller.Snapshot(target);
			context.Expect(updated && std::get<float>(*Value(*updated, GraphicsSettingId::FixedExposureCompensation)) == 4.0f,
				"Future game changes must flow to a camera whose explicit override was removed.");
		});

		registry.Add("Scripting.GraphicsSettings.Contract.LiveCameraOverridesSurviveViewGapAndRebind", [](TestContext& context) {
			GraphicsContractFixture fixture;
			context.Expect(Commit(context,fixture,fixture.Patch({{GraphicsSettingId::FixedExposureCompensation,0.0f}})).state
				== GraphicsRequestState::Applied,"Seed the inherited game exposure before camera registration.");
			const auto original=CameraScope();
			std::array targets{GraphicsTarget{},original};
			std::array liveOwners{*original.camera};
			fixture.preparation.validTargets=targets;
			fixture.controller.Invalidate(fixture.preparation,liveOwners);
			auto patch=fixture.Patch({{GraphicsSettingId::FixedExposureCompensation,1.0f}});
			patch.target=original; patch.baseRevision=0;
			context.Expect(Commit(context,fixture,patch).state == GraphicsRequestState::Applied,"The camera must own its independent exposure override.");
			const auto pinned=fixture.controller.Snapshot(original);
			context.Expect(pinned && std::get<float>(*Value(*pinned,GraphicsSettingId::FixedExposureCompensation)) == 1.0f,
				"The original admitted view must publish the camera exposure.");
			patch.patches[0].value=2.0f;
			const auto preparedReceipt=fixture.controller.Enqueue(patch);
			context.Expect(fixture.controller.Prepare(fixture.preparation).has_value(),"Prepare a request tied to the old exact view.");
			const auto queuedReceipt=fixture.controller.Enqueue(patch);
			std::array gameOnly{GraphicsTarget{}};
			fixture.preparation.validTargets=gameOnly;
			++fixture.preparation.frameId;
			fixture.controller.Invalidate(fixture.preparation,liveOwners);
			const auto hidden=fixture.controller.CameraOwnerSnapshot(*original.camera);
			GraphicsTarget cameraOnly=original; cameraOnly.view.reset();
			context.Expect(hidden && std::get<float>(*Value(*hidden,GraphicsSettingId::FixedExposureCompensation)) == 1.0f
				&& !fixture.controller.Snapshot(original) && !fixture.controller.Snapshot(cameraOnly),
				"A live camera with no admitted view retains internal values without inventing any managed effective snapshot.");
			context.Expect(fixture.controller.GetReceipt(preparedReceipt.requestId)->state == GraphicsRequestState::Cancelled
				&& fixture.controller.GetReceipt(queuedReceipt.requestId)->state == GraphicsRequestState::Rejected,
				"Missing old views must still cancel prepared and reject queued exact-view work.");
			context.Expect(Commit(context,fixture,fixture.Patch({{GraphicsSettingId::FixedExposureCompensation,0.25f}})).state
				== GraphicsRequestState::Applied,"Game updates remain legal while this camera has no view.");
			auto rebound=original; ++rebound.view->lifetimeGeneration;
			targets[1]=rebound; fixture.preparation.validTargets=targets;
			fixture.controller.Invalidate(fixture.preparation,liveOwners);
			fixture.controller.UpdateViewExtent(rebound,640,360);
			const auto restored=fixture.controller.Snapshot(rebound);
			context.Expect(restored && restored->target.view == rebound.view && restored->outputWidth == 640 && restored->outputHeight == 360
				&& std::get<float>(*Value(*restored,GraphicsSettingId::FixedExposureCompensation)) == 1.0f && !fixture.controller.Snapshot(original),
				"A newly admitted view/extent must project the same retained override while the retired exact view stays unavailable.");
			const auto staleReceipt=fixture.controller.Enqueue(patch);
			context.Expect(!fixture.controller.Prepare(fixture.preparation)
				&& fixture.controller.GetReceipt(staleReceipt.requestId)->reason == GraphicsRequestReason::TargetUnavailable,
				"Retaining an owner never authorizes an old view lifetime to mutate its replacement.");
			auto replacement=rebound; ++replacement.camera->componentGeneration; ++replacement.camera->bindingGeneration;
			++replacement.view->lifetimeGeneration; targets[1]=replacement; liveOwners[0]=*replacement.camera;
			fixture.controller.Invalidate(fixture.preparation,liveOwners);
			const auto fresh=fixture.controller.Snapshot(replacement);
			context.Expect(!fixture.controller.CameraOwnerSnapshot(*original.camera) && fresh
				&& std::get<float>(*Value(*fresh,GraphicsSettingId::FixedExposureCompensation)) == 0.25f,
				"A genuine component/binding replacement retires the old override even when its UUID and physical output are reused.");
			context.Expect(pinned->target == original && std::get<float>(*Value(*pinned,GraphicsSettingId::FixedExposureCompensation)) == 1.0f,
				"Already captured old-view snapshots must remain immutable through gap, rebind and genuine retirement.");
			fixture.preparation.validTargets=gameOnly;
			fixture.controller.Invalidate(fixture.preparation);
			context.Expect(!fixture.controller.CameraOwnerSnapshot(*replacement.camera),"Context-only explicit decommission retains its original retirement behavior.");
		});

		registry.Add("Scripting.GraphicsSettings.Contract.StaleCameraCannotChangeReplacement", [](TestContext& context) {
			GraphicsContractFixture fixture;
			const auto target = CameraScope();
			std::array targets{target};
			fixture.preparation.validTargets = targets;
			auto request = fixture.Patch({{GraphicsSettingId::FixedExposureCompensation, 2.0f}});
			request.target = target;
			++request.target.camera->componentGeneration;
			const auto admitted = fixture.controller.Enqueue(std::move(request));
			context.Expect(!fixture.controller.Prepare(fixture.preparation),
				"A recycled camera component generation must not prepare an override for the replacement.");
			const auto receipt = fixture.controller.GetReceipt(admitted.requestId);
			context.Expect(receipt && receipt->state == GraphicsRequestState::Rejected
				&& receipt->reason == GraphicsRequestReason::TargetUnavailable && !fixture.controller.Snapshot(target),
				"Stale camera scope must receive an explicit unavailable result without creating a replacement publication.");
		});

		registry.Add("Scripting.GraphicsSettings.Contract.OverlappingRevisionRequestsCannotOverwriteNewerState", [](TestContext& context) {
			GraphicsContractFixture fixture;
			context.Expect(Commit(context, fixture, fixture.Patch({{GraphicsSettingId::ShadowFilteringSize, std::uint32_t{3}}})).state
				== GraphicsRequestState::Applied, "A nonzero baseline revision must exist before competing requests.");
			const auto first = fixture.controller.Enqueue(fixture.Patch({{GraphicsSettingId::ShadowFilteringSize, std::uint32_t{5}}}));
			const auto second = fixture.controller.Enqueue(fixture.Patch({{GraphicsSettingId::ShadowFilteringSize, std::uint32_t{7}}}));
			auto candidate = fixture.controller.Prepare(fixture.preparation);
			context.Expect(candidate && candidate->requestId == first.requestId,
				"Competing writes must respect request admission order.");
			context.Expect(fixture.controller.Publish(std::move(*candidate), fixture.preparation).state == GraphicsRequestState::Applied,
				"The first valid revision-matched request must publish.");
			context.Expect(!fixture.controller.Prepare(fixture.preparation),
				"The second write must fail its obsolete revision instead of overwriting the new state.");
			const auto receipt = fixture.controller.GetReceipt(second.requestId);
			context.Expect(receipt && receipt->state == GraphicsRequestState::Rejected
				&& receipt->reason == GraphicsRequestReason::StaleRevision
				&& std::get<std::uint32_t>(*Value(*fixture.controller.Snapshot(), GraphicsSettingId::ShadowFilteringSize)) == 5u,
				"The losing caller must receive a revision conflict with the first value still effective.");
		});

		registry.Add("Scripting.GraphicsSettings.Contract.NamedPresetPublishesItsCoveredValuesAtomically", [](TestContext& context) {
			GraphicsContractFixture fixture;
			std::array<GraphicsPresetDefinition, 1> presets{{{QualityPreset::High, {
				{GraphicsSettingId::ShadowFilteringSize, std::uint32_t{7}},
				{GraphicsSettingId::MaxUploadedLights, std::uint32_t{2048}}}}}};
			fixture.preparation.presets = presets;
			const auto original = fixture.controller.Snapshot();
			GraphicsSettingsRequest preset;
			preset.operation = GraphicsRequestOperation::Preset;
			preset.lifecycle = fixture.preparation.lifecycle;
			preset.preset = QualityPreset::High;
			const auto admitted = fixture.controller.Enqueue(std::move(preset));
			auto candidate = fixture.controller.Prepare(fixture.preparation);
			context.Expect(candidate && candidate->requestId == admitted.requestId
				&& candidate->snapshot.preset == QualityPreset::High && fixture.controller.Snapshot() == original,
				"A supported named preset must resolve against the copied catalog as one unpublished candidate.");
			const auto applied = fixture.controller.Publish(std::move(*candidate), fixture.preparation);
			const auto snapshot = fixture.controller.Snapshot();
			context.Expect(applied.state == GraphicsRequestState::Applied && snapshot->preset == QualityPreset::High
				&& std::get<std::uint32_t>(*Value(*snapshot, GraphicsSettingId::ShadowFilteringSize)) == 7u
				&& std::get<std::uint32_t>(*Value(*snapshot, GraphicsSettingId::MaxUploadedLights)) == 2048u,
				"Publishing High must expose all covered settings and its named identity together.");
		});

		registry.Add("Scripting.GraphicsSettings.Contract.InvalidPresetMemberRejectsAllCoveredValues", [](TestContext& context) {
			GraphicsContractFixture fixture;
			std::array<GraphicsPresetDefinition, 1> presets{{{QualityPreset::High, {
				{GraphicsSettingId::ShadowFilteringSize, std::uint32_t{7}},
				{GraphicsSettingId::MaxUploadedLights, std::uint32_t{5000}}}}}};
			fixture.preparation.presets = presets;
			const auto original = fixture.controller.Snapshot();
			GraphicsSettingsRequest preset;
			preset.operation = GraphicsRequestOperation::Preset;
			preset.lifecycle = fixture.preparation.lifecycle;
			preset.preset = QualityPreset::High;
			const auto admitted = fixture.controller.Enqueue(std::move(preset));
			context.Expect(!fixture.controller.Prepare(fixture.preparation),
				"A preset with an invalid member must not prepare its remaining valid fields.");
			const auto receipt = fixture.controller.GetReceipt(admitted.requestId);
			context.Expect(receipt && receipt->state == GraphicsRequestState::Rejected
				&& receipt->reason == GraphicsRequestReason::InvalidValue && fixture.controller.Snapshot() == original,
				"Invalid preset validation must retain every previous setting and its preset identity.");
		});

		registry.Add("Scripting.GraphicsSettings.Abi.ExpandedPacketsRemainIndependentFromLegacyV1", [](TestContext& context) {
			context.ExpectEqual(2u, Native_GraphicsSettings_GetAbiVersion(),
				"The expanded graphics API must advertise its independent v2 contract.");
			context.ExpectEqual(1u, Native_Graphics_GetAbiVersion(),
				"Adding the expanded API must preserve the old graphics ABI version for existing assemblies.");
			context.Expect(sizeof(NativeGraphicsSettingsRequestPacket) == 112u
				&& sizeof(NativeGraphicsSettingsTargetPacket) == 80u
				&& sizeof(NativeGraphicsSettingsValuePacket) == 40u
				&& sizeof(NativeGraphicsSettingsReceiptPacket) == 72u
				&& sizeof(NativeGraphicsRenderSettings) == 120u,
				"Managed array packets and the old flattened packet must retain separately pinned sizes.");
			context.Expect(offsetof(NativeGraphicsSettingsRequestPacket, target) == 32u
				&& offsetof(NativeGraphicsSettingsReceiptPacket, requestId) == 32u
				&& offsetof(NativeGraphicsRenderSettings, virtualGeometryBudgetBytes) == 96u,
				"Expanded buffer fields and legacy 64-bit budgets must retain their declared offsets.");
		});

		registry.Add("Scripting.GraphicsSettings.Abi.RejectsRequestHeaderBeforeAdmission", [](TestContext& context) {
			const auto generation = Native_Graphics_GetRuntimeSettingsGeneration();
			for(const auto header : {NativeGraphicsAbiHeader{1u, sizeof(NativeGraphicsSettingsRequestPacket)},
				NativeGraphicsAbiHeader{2u, 111u}, NativeGraphicsAbiHeader{2u, 113u}})
			{
				NativeGraphicsSettingsRequestPacket request{};
				request.header = header;
				request.valueCount = 1u;
				NativeGraphicsSettingsReceiptPacket receipt{};
				receipt.header = {kNativeGraphicsSettingsAbiVersion, sizeof(receipt)};
				context.Expect(Native_GraphicsSettings_Submit(&request, nullptr, nullptr, &receipt)
					== NativeGraphicsSettingsStatus::VersionMismatch,
					"An unknown version or wrong exact request size must reject before reading its variable arrays.");
				context.Expect(receipt.requestId == 0u,
					"A malformed header must never receive an accepted native request identity.");
			}
			context.ExpectEqual(generation, Native_Graphics_GetRuntimeSettingsGeneration(),
				"Invalid expanded headers must preserve the current published settings generation.");
		});

		registry.Add("Scripting.GraphicsSettings.Abi.RejectsNullPointersAndMissingArrays", [](TestContext& context) {
			NativeGraphicsSettingsRequestPacket request{};
			request.header = {kNativeGraphicsSettingsAbiVersion, sizeof(request)};
			request.valueCount = 1u;
			NativeGraphicsSettingsReceiptPacket receipt{};
			receipt.header = {kNativeGraphicsSettingsAbiVersion, sizeof(receipt)};
			context.Expect(Native_GraphicsSettings_Submit(nullptr, nullptr, nullptr, &receipt)
				== NativeGraphicsSettingsStatus::InvalidArgument,
				"A missing request must fail before dereferencing any packet fields.");
			context.Expect(Native_GraphicsSettings_Submit(&request, nullptr, nullptr, &receipt)
				== NativeGraphicsSettingsStatus::InvalidArgument,
				"A positive value count requires an actual value array before admission.");
			NativeGraphicsSettingsValuePacket value{};
			value.settingId = static_cast<std::uint32_t>(GraphicsSettingId::ShadowFilteringSize);
			value.valueKind = static_cast<std::uint32_t>(GraphicsValueKind::Unsigned32);
			value.scalarBits = 3u;
			context.Expect(Native_GraphicsSettings_Submit(&request, &value, nullptr, nullptr)
				== NativeGraphicsSettingsStatus::InvalidArgument,
				"The bridge must require an output receipt for a submitted request.");
			request.textBytes = 1u;
			context.Expect(Native_GraphicsSettings_Submit(&request, &value, nullptr, &receipt)
				== NativeGraphicsSettingsStatus::InvalidArgument && receipt.requestId == 0u,
				"A declared UTF-8 byte range requires a valid source before any settings are queued.");
		});

		registry.Add("Scripting.GraphicsSettings.Abi.RejectsUnsupportedOperationScopeAndValueKind", [](TestContext& context) {
			NativeGraphicsSettingsRequestPacket valid{};
			valid.header = {kNativeGraphicsSettingsAbiVersion, sizeof(valid)};
			valid.valueCount = 1u;
			NativeGraphicsSettingsValuePacket validValue{};
			validValue.settingId = static_cast<std::uint32_t>(GraphicsSettingId::ShadowFilteringSize);
			validValue.valueKind = static_cast<std::uint32_t>(GraphicsValueKind::Unsigned32);
			validValue.scalarBits = 3u;
			for(std::uint32_t variant = 0u; variant != 3u; ++variant)
			{
				auto request = valid;
				auto value = validValue;
				if(variant == 0u) request.operation = 255u;
				if(variant == 1u) request.target.scope = 255u;
				if(variant == 2u) value.valueKind = 255u;
				NativeGraphicsSettingsReceiptPacket receipt{};
				receipt.header = {kNativeGraphicsSettingsAbiVersion, sizeof(receipt)};
				context.Expect(Native_GraphicsSettings_Submit(&request, &value, nullptr, &receipt)
					== NativeGraphicsSettingsStatus::InvalidArgument && receipt.requestId == 0u,
					"Unknown operation, scope, or typed-value tags must reject before native queue admission.");
			}
		});

		registry.Add("Scripting.GraphicsSettings.Abi.RejectsMismatchedReceiptCapacity", [](TestContext& context) {
			NativeGraphicsSettingsRequestPacket request{};
			request.header = {kNativeGraphicsSettingsAbiVersion, sizeof(request)};
			request.valueCount = 1u;
			NativeGraphicsSettingsValuePacket value{};
			value.settingId = static_cast<std::uint32_t>(GraphicsSettingId::ShadowFilteringSize);
			value.valueKind = static_cast<std::uint32_t>(GraphicsValueKind::Unsigned32);
			value.scalarBits = 3u;
			for(const auto header : {NativeGraphicsAbiHeader{1u, sizeof(NativeGraphicsSettingsReceiptPacket)},
				NativeGraphicsAbiHeader{2u, 71u}, NativeGraphicsAbiHeader{2u, 73u}})
			{
				NativeGraphicsSettingsReceiptPacket receipt{};
				receipt.header = header;
				context.Expect(Native_GraphicsSettings_Submit(&request, &value, nullptr, &receipt)
					== NativeGraphicsSettingsStatus::VersionMismatch && receipt.requestId == 0u,
					"A receipt buffer with an unknown layout must reject before a request is admitted.");
			}
		});
	}
}
