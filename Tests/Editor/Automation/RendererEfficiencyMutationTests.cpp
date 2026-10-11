// Renderer efficiency fixture producers prepare copied transactions; this suite
// checks the real scene commit path and its existing editor visibility boundary.
#include "../TestHarness.h"
#include "../TestSha256.h"
#include "Tests/Support/RendererEfficiency/Editor/EditorRendererEfficiencyValidation.h"
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyFixtureBindings.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"
#include "Core/Object/ObjectManager.h"
#include "Components/Transform.h"
#include "Components/MeshRenderComponent.h"
#include "Components/LightComponent.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"
#include "Utilities/UUID.h"
#include "Animation/Runtime/AnimationAuthorityPublication.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"
#include "Renderer/RHI/Vulkan/VulkanShadowValidationReadback.h"
#include <bit>

#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <utility>

// Keep this task-local predicate out of the widely included host header. The
// regression calls the same typed gate used by protected fixture admission.
namespace TerraEngine::Editor::Automation
{
	bool RendererEfficiencyAtomicConflictReceiptMatches(const Scene::AutomationTransactionReceipt&, std::size_t);
	std::string_view RendererEfficiencyCompletedExportPhase(std::uint64_t warmupCompleted, std::uint64_t warmupFrames);
	bool RendererEfficiencyFixtureControlsMatch(const Renderer::RendererEfficiencyFixtureIntent& expected,
		const Renderer::RendererEfficiencyFixtureIntent& applied);
	std::string RendererEfficiencyPendingObservationReason(const Renderer::RendererEfficiencyFixtureIntent& expected,
		const Renderer::RendererEfficiencyFixtureIntent& applied, std::span<const Renderer::RendererEfficiencyEvidence> rows);
	bool RendererEfficiencyAnimationAuthorityMatches(const Animation::AnimationAuthorityRow& row,
		const Animation::AnimationAuthorityFrame& frame, const Animation::AnimationCommandReceipt& receipt,
		const UUID& objectId, bool baseline, Animation::AnimationFixedTick playbackTick,
		const std::shared_ptr<const Animation::AnimationSnapshot>& prior);
	bool RendererEfficiencyGpuAnimationAuthorityMatches(const Animation::AnimationPresentationFrame& frame,
		const Animation::AnimationPresentationFrame& prior, const Animation::AnimationCommandReceipt& receipt,
		const UUID& id, bool baseline, Animation::AnimationFixedTick requestedTick);
}

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace TerraEngine::Scene;
		using TerraEngine::Editor::Automation::EditorRendererEfficiencyValidation;
		constexpr const char* kBoundary = "post_editor_automation_flush";
		constexpr std::uint64_t kProjectGeneration = 17;
		constexpr const char* kFixtureHash = "fixture-mutation-content-hash";

		// Each case owns one small scene and a UUID-named temporary marker directory.
		// Workers finish before the fixture releases ObjectManager or its queue.
		class MutationFixture
		{
		public:
			MutationFixture()
				: objects(ObjectManager::instance()), root(std::filesystem::temp_directory_path()
					/ ("TerraEngine-RendererMutation-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID())))
			{
				objects.clear();
				Transform::registerType();
				std::filesystem::create_directories(root);
				WriteMarker(kFixtureHash);
				target = objects.createObject("Shadow Caster");
				targetId = objects.getUUID(target);
			}
			~MutationFixture()
			{
				queue.Clear();
				objects.clear();
				// Verify the absolute deletion target is still this fixture's direct
				// child of the temporary directory before recursively removing it.
				std::error_code ignored;
				const auto absolute = std::filesystem::weakly_canonical(root, ignored);
				const auto temporary = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(), ignored);
				if(!ignored && absolute.parent_path() == temporary
					&& absolute.filename().string().starts_with("TerraEngine-RendererMutation-"))
					std::filesystem::remove_all(absolute, ignored);
			}
			void WriteMarker(std::string hash)
			{
				const nlohmann::json marker = {{"schemaVersion", 1}, {"kind", "RendererEfficiencyValidationProject"},
					{"projectRoot", std::filesystem::canonical(root).generic_string()},
					{"fixtureId", "shadow-invalidation"}, {"fixtureHash", std::move(hash)}};
				std::ofstream output(root / "renderer-efficiency-project.json", std::ios::binary | std::ios::trunc);
				output << marker.dump(2);
			}
			bool Start(std::string& error)
			{
				TerraEngine::Renderer::RendererEfficiencyOptionsSnapshot options;
				options.optionsSessionGeneration = 3;
				return host.Start(root, kProjectGeneration,
					{{"fixtureId", "shadow-invalidation"}, {"fixtureHash", kFixtureHash}, {"stage", "P1"}}, options, error);
			}
			Transaction Prepare(std::string id, const WorldPose& pose)
			{
				// Capture observations while the test owns the protected live scene.
				// The worker receives only this detached transaction and canonical pose.
				TransactionalMutationBuilder builder(std::move(id), TransactionOrigin::Automation, "RendererEfficiencyFixture");
				builder.SetWorldTransform(objects, targetId, pose);
				return std::move(builder).Build();
			}
			SceneGraph Capture() const { return BuildSceneGraphFromObjectManager(objects); }
			ObjectManager& objects;
			std::filesystem::path root;
			Object target;
			TerraEngine::UUID targetId{};
			SceneMutationQueue queue;
			EditorRendererEfficiencyValidation host;
		};

		WorldPose ChangedPose()
		{
			WorldPose pose;
			// A large exact cell makes a lossy float-position implementation fail.
			pose.position.cell.x = (std::int64_t{1} << 40) + 7;
			pose.position.offsetMetres = glm::dvec3(13.25, 2.0, -4.0);
			pose.rotation = glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
			pose.scale = glm::vec3(2.0F, 3.0F, 4.0F);
			return pose;
		}
		void ExpectPose(TestContext& context, const WorldPose& actual, const WorldPose& expected, std::string_view message)
		{
			context.Expect(actual.position == expected.position && actual.rotation == expected.rotation && actual.scale == expected.scale, message);
		}
		void ExpectRejectedWithoutWrites(TestContext& context, const SceneMutationBatchResult& result,
			MutationFixture& fixture, const SceneGraph& before)
		{
			context.ExpectEqual(std::size_t{1}, result.transactionResults.size(), "The protected flush must return exactly one whole-batch decision.");
			context.Expect(result.transactionResults.front().status == TransactionStatus::Rejected
				|| result.transactionResults.front().status == TransactionStatus::Conflicted, "A rejected fixture batch must never report committed.");
			const auto after = fixture.Capture();
			ExpectPose(context, after.objects.at(fixture.targetId).worldPose, before.objects.at(fixture.targetId).worldPose,
				"Rejecting a later operation must restore the exact canonical transform, rotation and scale.");
			context.ExpectEqual(before.objects.at(fixture.targetId).name, after.objects.at(fixture.targetId).name,
				"Rejecting a fixture batch must preserve metadata alongside its transform.");
			context.ExpectEqual(before.objects.size(), after.objects.size(), "Rejected fixture work must not leak objects.");
		}
	}

	void RegisterRendererEfficiencyMutationTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.Editor.Mutation.StaticCategoryWaitsForExactPacketAndRollsBack", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			MutationFixture fixture;
			RendererEfficiencyScenePublication proof; proof.priorPacket = std::make_shared<const SceneChangePacket>(fixture.objects.PublishSceneChangePacket());
			TransactionalMutationBuilder builder("static-category", TransactionOrigin::Automation, "RendererEfficiencyFixture");
			builder.ObserveScene(fixture.objects); builder.ObserveObject(fixture.objects,fixture.targetId); builder.SetStatic(fixture.targetId,true);
			proof.transaction = std::move(builder).Build(); fixture.queue.EnqueueTransaction(proof.transaction);
			context.Expect(!fixture.objects.isObjectStatic(fixture.target), "Worker-authored category writes must stay invisible before the protected boundary.");
			fixture.queue.Apply(fixture.objects,kBoundary);
			const auto receipt = fixture.queue.TakeCompletedAutomationTransactionReceipt("static-category"); std::string reason;
			context.Expect(receipt && CaptureRendererEfficiencyCommittedScenePublication(fixture.objects,proof,*receipt,reason), "Actual SetStatic commit must capture exact authored category: " + reason);
			std::uint64_t token = 0;
			context.Expect(!ResolveRendererEfficiencyScenePublication(fixture.objects,proof,token,reason) && token == 0, "Committed category cannot release a token before scheduler publication.");
			fixture.objects.PublishSceneChangePacket();
			context.Expect(ResolveRendererEfficiencyScenePublication(fixture.objects,proof,token,reason) && token == fixture.objects.GetSpatialRevision(), "Exact category metadata packet must authorize the real renderer clock: " + reason);
			const auto metadata = fixture.objects.getObjectMetadataGeneration(fixture.target), spatial = fixture.objects.GetObjectSpatialRevision(fixture.target);
			TransactionalMutationBuilder conflict("static-rollback",TransactionOrigin::Automation,"RendererEfficiencyFixture");
			conflict.ObserveScene(fixture.objects); conflict.ObserveObject(fixture.objects,fixture.targetId);
			conflict.SetStatic(fixture.targetId,false).DeleteObject(fixture.targetId).SetStatic(fixture.targetId,true);
			auto conflictTransaction=std::move(conflict).Build();const auto expectedRejected=conflictTransaction.writeSet.operations.size();
			fixture.queue.EnqueueTransaction(std::move(conflictTransaction)); fixture.queue.Apply(fixture.objects,kBoundary);
			const auto rejected = fixture.queue.TakeCompletedAutomationTransactionReceipt("static-rollback");
			context.Expect(rejected && (rejected->status == TransactionStatus::Conflicted || rejected->status == TransactionStatus::Rejected) && !rejected->appliedWriteCount && rejected->rejectedWriteCount == expectedRejected
				&& fixture.objects.isObjectStatic(fixture.target) && metadata == fixture.objects.getObjectMetadataGeneration(fixture.target)
				&& spatial == fixture.objects.GetObjectSpatialRevision(fixture.target), "Atomic category conflict must preserve actual category and target-local revisions. actualStatus=" + (rejected ? std::string(ToString(rejected->status)) : "Missing") + " applied=" + std::to_string(rejected ? rejected->appliedWriteCount : 0) + " rejected=" + std::to_string(rejected ? rejected->rejectedWriteCount : 0));
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.AtomicConflictPreservesRealDecision", [](TestContext& context) {
			MutationFixture fixture;
			// Establish an actual spatial owner through a real committed baseline,
			// rather than assigning a synthetic test clock to a new empty object.
			fixture.queue.EnqueueTransaction(fixture.Prepare("rollback-baseline", ChangedPose()));
			const auto baseline = fixture.queue.Apply(fixture.objects, kBoundary);
			context.Expect(baseline.transactionResults.front().status == TransactionStatus::Committed, "Actual baseline must commit before testing rollback visibility.");
			const auto before = fixture.Capture();
			using TerraEngine::Editor::Automation::CaptureRendererEfficiencyRollbackPublication;
			using TerraEngine::Editor::Automation::RendererEfficiencyRollbackPublication;
			TransactionalMutationBuilder unpublished("unpublished", TransactionOrigin::Automation, "RendererEfficiencyFixture");
			unpublished.ObserveScene(fixture.objects); unpublished.ObserveObject(fixture.objects, fixture.targetId);
			RendererEfficiencyRollbackPublication absent;
			std::string missingReason;
			context.Expect(!CaptureRendererEfficiencyRollbackPublication(fixture.objects, std::move(unpublished).Build().readSet, absent, missingReason)
				&& !absent.packet && !absent.rendererSpatialRevision, "No unpublished scene can manufacture prior visibility proof.");
			// Emulate the existing scheduler publication boundary, before any
			// rejected writes. The host must reuse this real protected clock.
			fixture.objects.PublishSceneChangePacket();
			const auto published = fixture.objects.LoadPublishedSceneChangePacketShared();
			const auto spatial = fixture.objects.GetSpatialRevision();
			const auto targetSpatial = fixture.objects.GetObjectSpatialRevision(fixture.target);
			TransactionalMutationBuilder builder("owned-delete-write-conflict", TransactionOrigin::Automation, "RendererEfficiencyFixture");
			builder.ObserveScene(fixture.objects); builder.ObserveObject(fixture.objects, fixture.targetId);
			builder.DeleteObject(fixture.targetId);
			auto transaction = std::move(builder).Build();
			// Match the native worker's detached write preparation: observe once,
			// then append writes without touching live storage from the worker.
			TransactionWriteOperation write; write.kind = TransactionWriteKind::SetWorldTransform;
			write.target.objectId = fixture.targetId; write.worldPose = ChangedPose();
			transaction.writeSet.operations.push_back(std::move(write));
			TerraEngine::Editor::Automation::RendererEfficiencyRollbackPublication prior;
			std::string publicationReason;
			context.ExpectEqual(std::uint64_t{0}, transaction.readSet.spatialSnapshotGeneration, "The detached read-set field is truthfully reserved zero.");
			const auto capturedPrior = CaptureRendererEfficiencyRollbackPublication(fixture.objects, transaction.readSet, prior, publicationReason);
			std::cout << "Actual prior capture=" << capturedPrior << " reason=" << publicationReason << " spatial=" << spatial << " target=" << targetSpatial << '\n';
			context.Expect(capturedPrior, "Published protected scene ownership must supply the actual prior renderer clock.");
			context.ExpectEqual(spatial, prior.rendererSpatialRevision, "Rollback must reuse the actual ObjectManager renderer clock.");
			context.Expect(prior.packet == published, "Rollback proof must retain the actual immutable publication owner.");
			for(unsigned failure = 0; failure < 3; ++failure)
			{
				auto invalidRead = transaction.readSet;
				if(failure == 0) ++invalidRead.sceneGeneration;
				if(failure == 1) ++invalidRead.objects.front().lifetimeGeneration;
				if(failure == 2) invalidRead.objects.front().objectId = TerraEngine::GenerateUUID();
				auto unchanged = prior;
				context.Expect(!CaptureRendererEfficiencyRollbackPublication(fixture.objects, invalidRead, unchanged, publicationReason)
					&& unchanged.packet == prior.packet && unchanged.rendererSpatialRevision == prior.rendererSpatialRevision,
					"Wrong scene, target or lifetime must reject atomically without replacing retained proof.");
			}
			fixture.queue.EnqueueTransaction(std::move(transaction));
			context.Expect(!fixture.queue.TakeCompletedAutomationTransactionReceipt("owned-delete-write-conflict"), "No decision exists before the existing protected flush.");
			const auto batch = fixture.queue.Apply(fixture.objects, kBoundary);
			const auto receipt = fixture.queue.TakeCompletedAutomationTransactionReceipt("owned-delete-write-conflict");
			context.Expect(receipt.has_value(), "Actual queued conflict must retain its terminal decision.");
			if(!receipt) return;
			std::cout << "Actual delete/write decision=" << ToString(receipt->status) << " applied=" << receipt->appliedWriteCount << " rejected=" << receipt->rejectedWriteCount << " message=" << receipt->message << '\n';
			ExpectRejectedWithoutWrites(context, batch, fixture, before);
			context.ExpectEqual(std::uint64_t{0}, receipt->spatialPublicationGeneration, "Conflict cannot invent a new publication generation.");
			context.Expect(fixture.objects.LoadPublishedSceneChangePacketShared() == published, "Conflict must not publish another scene packet.");
			context.ExpectEqual(spatial, fixture.objects.GetSpatialRevision(), "Atomic conflict must not advance the real renderer clock.");
			context.ExpectEqual(targetSpatial, fixture.objects.GetObjectSpatialRevision(fixture.target), "Atomic conflict must preserve the exact target revision.");
			context.Expect(receipt->status == TransactionStatus::Conflicted, "Frozen delete/write fixture must yield its real conflict decision.");
			context.Expect(TerraEngine::Editor::Automation::RendererEfficiencyAtomicConflictReceiptMatches(*receipt, 2), "Host must admit the genuine complete atomic conflict receipt without relabeling it.");
			auto invalid = *receipt; invalid.status = TransactionStatus::Committed;
			context.Expect(!TerraEngine::Editor::Automation::RendererEfficiencyAtomicConflictReceiptMatches(invalid, 2), "Committed work cannot prove rollback.");
			invalid = *receipt; invalid.rejectedWriteCount = 1;
			context.Expect(!TerraEngine::Editor::Automation::RendererEfficiencyAtomicConflictReceiptMatches(invalid, 2), "Partial rejected-write accounting cannot prove the complete batch.");
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.ResidencyArmsReuploadBeforeInvalidation", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			using namespace TerraEngine::Renderer;
			RendererEfficiencyFixtureIntent baseline;
			baseline.correlation = {"owned-residency", "shadow-invalidation-12", 360, 31, 2, 3};
			baseline.controlGeneration = 16; baseline.controlsApplied = true;
			baseline.sampleTick = 30; baseline.holdFixedTick = true;
			for(unsigned rejection = 0; rejection < 5; ++rejection) {
				auto active = baseline, pendingNative = baseline;
				std::uint64_t stepFrame = 360; bool queued = true;
				unsigned admissions = 0, invalidations = 0;
				std::string error;
				const auto admit = [&](const RendererEfficiencyFixtureIntent& intent) {
					++admissions;
					RendererEfficiencyFixtureIntentReceipt receipt{true, {}, intent.correlation, 16};
					if(rejection == 1) { receipt.accepted = false; receipt.reason = "Actual native rejection"; }
					if(rejection == 2) receipt.controlGeneration = 0;
					if(rejection == 3) ++receipt.correlation.rendererSessionGeneration;
					if(rejection == 4) ++receipt.controlGeneration;
					if(receipt.accepted) pendingNative = intent;
					return receipt;
				};
				const auto invalidate = [&](std::string&) {
					++invalidations;
					context.Expect(pendingNative.correlation.stepFrame == 361,
						"Actual invalidation dispatch must see already-armed native361, never consumed baseline360.");
					return true;
				};
				const bool accepted = AdmitRendererEfficiencyResidencyReupload(active, 360, stepFrame, queued, admit, invalidate, error);
				if(rejection) {
					context.Expect(!accepted && invalidations == 0 && stepFrame == 360 && active.correlation == baseline.correlation && !error.empty(),
						"Rejected or incoherent native admission cannot evict or advance the Host.");
				} else {
					context.Expect(accepted && queued && stepFrame == 361 && active.correlation == pendingNative.correlation && active.controlGeneration == 16
						&& active.sampleTick == baseline.sampleTick && active.holdFixedTick == baseline.holdFixedTick,
						"Successful admission retains the real queued361 owner in the same protected pump.");
					context.Expect(AdmitRendererEfficiencyResidencyReupload(active, 360, stepFrame, queued, admit, invalidate, error)
						&& admissions == 1 && invalidations == 1, "Duplicate pumps must neither rearm nor evict again.");
				}
			}
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.ResidencyRetainsBothOriginalCompletedCohorts", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			// CPU receipt-policy inputs model the writer's completed frame metadata;
			// they are not a claim that this backend-free test submitted GPU work.
			const auto observation = [](std::uint64_t stepFrame, std::uint64_t nativeFrame) {
				nlohmann::json result{{"controlGeneration", "16"}, {"frameSerial", std::to_string(nativeFrame)},
					{"commandSerial", std::to_string(nativeFrame)}, {"submitSerial", std::to_string(nativeFrame - 1)}, {"completionSerial", std::to_string(nativeFrame - 1)},
					{"originalCorrelation", {{"operationId", "owned-residency"}, {"fixtureStepId", "shadow-invalidation-12"}, {"stepFrame", stepFrame},
						{"mutationGeneration", 31}, {"settingsGeneration", 2}, {"rendererSessionGeneration", 3}}},
					{"frames", nlohmann::json::array()}, {"artifacts", {{{"path", "immutable-cpu-artifact"}, {"sha256", std::string(64, 'a')}}}}};
				for(unsigned light = 0; light < 4; ++light) for(unsigned face = 0; face < (light == 0 ? 6U : 1U); ++face)
					result["frames"].push_back({{"measurementPhase", "Transition"}, {"fixtureStepId", "shadow-invalidation-12"}, {"stepFrame", stepFrame},
						{"viewType", "Scene"}, {"viewId", 1}, {"lightId", {{"status", "Valid"}, {"value", std::to_string(light)}}}, {"faceIndex", {{"status", "Valid"}, {"value", face}}},
						{"frameSerial", {{"status", "Valid"}, {"value", nativeFrame}}}, {"commandSerial", {{"status", "Valid"}, {"value", nativeFrame}}},
						{"submitSerial", {{"status", "Valid"}, {"value", nativeFrame - 1}}}, {"completionSerial", {{"status", "Valid"}, {"value", nativeFrame - 1}}},
						{"settingsGeneration", {{"status", "Valid"}, {"value", 2}}}, {"rendererSessionGeneration", {{"status", "Valid"}, {"value", 3}}}, {"mutationGeneration", {{"status", "Valid"}, {"value", 31}}}});
				return result;
			};
			const auto baseline = observation(360, 100), reupload = observation(361, 110);
			const nlohmann::json scalar{{"kind", "ResidencyPublication"}, {"fixtureStepId", "shadow-invalidation-12"},
				{"previousActualFrameSerial", 100}, {"newActualFrameSerial", 110}, {"commandSerial", 110}, {"submitSerial", 109}, {"completionSerial", 109}};
			nlohmann::json receipt; std::string error;
			context.Expect(CompleteRendererEfficiencyResidencyPublicationReceipt(scalar, baseline, reupload, receipt, error), "Both original complete cohorts must finish the production residency receipt: " + error);
			context.Expect(receipt.contains("baselineObservation") && receipt.contains("reuploadObservation"), "The old scalar-only Host receipt discards the first native reupload cohort.");
			context.Expect(receipt.at("baselineObservation") == baseline && receipt.at("reuploadObservation") == reupload,
				"Native baseline360 and reupload361 tokens and immutable artifact closure must survive unchanged.");
			const auto withGame = [](nlohmann::json original) {
				const auto scene = original["frames"];
				for(auto frame : scene) {
					frame["viewType"] = "Game"; frame["viewId"] = 2;
					frame["commandSerial"]["value"] = frame["commandSerial"]["value"].get<std::uint64_t>() + 100;
					original["frames"].push_back(std::move(frame));
				}
				return original;
			};
			const auto bothBaseline = withGame(baseline), bothReupload = withGame(reupload);
			auto gameScalar = scalar; gameScalar["commandSerial"] = 210;
			context.Expect(CompleteRendererEfficiencyResidencyPublicationReceipt(gameScalar, bothBaseline, bothReupload, receipt, error)
				&& receipt.at("commandSerial") == 210 && receipt.at("reuploadObservation").at("commandSerial") == "110",
				"The truthful target Game command may differ from the nested first Scene representative in a complete eighteen-region cohort.");
			gameScalar["commandSerial"] = 999; nlohmann::json unchangedCommand = {{"sentinel", true}};
			context.Expect(!CompleteRendererEfficiencyResidencyPublicationReceipt(gameScalar, bothBaseline, bothReupload, unchangedCommand, error)
				&& unchangedCommand == nlohmann::json{{"sentinel", true}}, "An invented command absent from every qualified output must reject atomically.");
			const auto reject = [&](nlohmann::json first, nlohmann::json second) {
				nlohmann::json unchanged = {{"sentinel", true}};
				context.Expect(!CompleteRendererEfficiencyResidencyPublicationReceipt(scalar, first, second, unchanged, error) && unchanged == nlohmann::json{{"sentinel", true}},
					"Incomplete, mixed or late-owner observation must reject atomically.");
			};
			auto incomplete = baseline; incomplete["frames"].erase(0); reject(incomplete, reupload);
			auto mixed = reupload; mixed["frames"][0]["submitSerial"]["value"] = 108; reject(baseline, mixed);
			auto late = reupload; late["originalCorrelation"]["rendererSessionGeneration"] = 4; reject(baseline, late);
			auto relabeled = reupload; relabeled["originalCorrelation"]["stepFrame"] = 0; reject(baseline, relabeled);
			auto noncanonical = reupload; noncanonical["controlGeneration"] = "016"; reject(baseline, noncanonical);
			auto noArtifacts = baseline; noArtifacts["artifacts"] = nlohmann::json::array(); reject(noArtifacts, reupload);
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.ImportAdmissionIsBoundedAndGenerationOwned", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			const auto package = *ParseUUID("65100000000000000000000000000004");
			// This is the actual Q failure shape: repeated identical texture uses
			// are valid references, but a failed texture cook is never readiness.
			nlohmann::json receipt{{"packageGuid", ToHexString(package)}, {"package", {{"sourceHash", "b2bc1b581d55fcb"}}},
				{"animationManifest", {{"candidateGeneration", 6}}}, {"status", "SucceededWithWarnings"},
				{"diagnostics", {{{"code", "TextureCompileFailed"}, {"severity", "Warning"}}}},
				{"generatedTextures", {{{"name", "CutoutAlpha"}, {"guid", "2d8ec612dcfa755d61c08d93d0e0d1f4"}},
					{{"name", "CutoutAlpha"}, {"guid", "2d8ec612dcfa755d61c08d93d0e0d1f4"}}}}};
			const auto failed = ClassifyRendererEfficiencyImportReceipt(receipt, package, "b2bc1b581d55fcb");
			context.Expect(failed.disposition == RendererEfficiencyImportDisposition::RetryableCookFailure,
				"A current identity-matched TextureCompileFailed receipt must be typed retryable, never ready.");
			context.Expect(DecideRendererEfficiencyImportAdmission(failed, 1, 0) == RendererEfficiencyImportDecision::Reimport,
				"The old silent polling loop must instead request one actual same-source retry.");
			context.Expect(DecideRendererEfficiencyImportAdmission(failed, 2, 6) == RendererEfficiencyImportDecision::Wait,
				"The same failed receipt generation must not dispatch another import.");
			context.Expect(DecideRendererEfficiencyImportAdmission(failed, 2, 5) == RendererEfficiencyImportDecision::Reimport,
				"A fresh second failed candidate may use the last additional actual import.");
			context.Expect(DecideRendererEfficiencyImportAdmission(failed, 3, 5) == RendererEfficiencyImportDecision::Fail,
				"Three actual admitted imports exhaust the two additional retry limit.");
			receipt["status"] = "Succeeded"; receipt["diagnostics"] = nlohmann::json::array();
			receipt["animationManifest"]["candidateGeneration"] = 7;
			context.Expect(DecideRendererEfficiencyImportAdmission(ClassifyRendererEfficiencyImportReceipt(receipt, package, "b2bc1b581d55fcb"), 2, 6)
				== RendererEfficiencyImportDecision::PrepareBindings, "Only a fresh successful receipt proceeds to unchanged strict binding preparation.");
			context.Expect(DecideRendererEfficiencyImportAdmission(ClassifyRendererEfficiencyImportReceipt(receipt, package, "b2bc1b581d55fcb"), 2, 7)
				== RendererEfficiencyImportDecision::Wait, "A nonfresh success cannot stand in for the newly dispatched import candidate.");
			context.Expect(ClassifyRendererEfficiencyImportReceipt(receipt, package, "other-source").disposition == RendererEfficiencyImportDisposition::Pending,
				"An earlier source receipt remains pending until the current physical import publishes.");
			receipt["animationManifest"]["candidateGeneration"] = -1;
			context.Expect(ClassifyRendererEfficiencyImportReceipt(receipt, package, "b2bc1b581d55fcb").disposition == RendererEfficiencyImportDisposition::Invalid,
				"A malformed signed generation cannot wrap into a fresh successful owner.");
			receipt["animationManifest"]["candidateGeneration"] = 7;
			receipt["packageGuid"] = "65100000000000000000000000000003";
			context.Expect(DecideRendererEfficiencyImportAdmission(ClassifyRendererEfficiencyImportReceipt(receipt, package, "b2bc1b581d55fcb"), 1, 0)
				== RendererEfficiencyImportDecision::Fail, "A conflicting owned package identity is terminal rather than retried.");
			MutationFixture fixture; std::string error;
			context.Expect(fixture.Start(error), "The cleanup protocol must use an actual owned Host operation.");
			const auto source = std::filesystem::path("Assets") / "import-admission.gltf";
			std::filesystem::create_directories((fixture.root / source).parent_path());
			{ std::ofstream file(fixture.root / source, std::ios::binary); file << "{}"; }
			const auto changed = InspectRendererEfficiencyOwnedImport(fixture.root, source, package, std::string(64, '0'));
			context.Expect(changed.disposition == RendererEfficiencyImportDisposition::Invalid
				&& DecideRendererEfficiencyImportAdmission(changed, 1, 0) == RendererEfficiencyImportDecision::Fail,
				"Actual current authored SHA mismatch is terminal before any same-source reimport or binding preparation.");
			const auto id = fixture.host.Status().at("operationId").get<std::string>();
			const auto prior = fixture.host.PriorToken();
			fixture.host.ReportNativeResourcePending("Awaiting physical source settlement.");
			context.Expect(fixture.host.Active(), "Genuine pending physical imports do not end the owned operation.");
			fixture.host.FailNativeResourcePreparation("Texture cook retries exhausted.");
			context.Expect(!fixture.host.Active() && fixture.host.Status().at("state") == "Failed",
				"Exhaustion must end measurement rather than silently poll forever.");
			context.Expect(fixture.host.HasOperation(id) && fixture.host.Stop(id, error),
				"Terminal import failure must still permit the owning runner to Stop for source/options cleanup.");
			context.Expect(fixture.host.PriorToken().valid == prior.valid && fixture.host.PriorToken().options == prior.options,
				"A terminal importer failure must preserve the actual captured restoration token.");
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.ControlTransitionRequiresOriginalCompleteCohort", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation; using namespace Renderer;
			RendererEfficiencyFixtureIntent applied; applied.controlsApplied = true; applied.controlGeneration = 7;
			applied.correlation = {"owned-transition", "selected-lod", 0, 31, 2, 3};
			RendererEfficiencyOptionsSnapshot options; options.settingsGeneration = 2; options.rendererSessionGeneration = 3; options.optionsSessionGeneration = 1;
			std::vector<RendererEfficiencyEvidence> rows, selected;
			nlohmann::json bundle = {{"views", nlohmann::json::array()}};
			for(unsigned view = 1; view <= 2; ++view) for(unsigned light = 0; light < 4; ++light) {
				const auto id = GenerateUUID(); nlohmann::json faces = nlohmann::json::array();
				for(unsigned face = 0; face < (light == 0 ? 6U : 1U); ++face) {
					faces.push_back(face); RendererEfficiencyEvidence row;
					row.status = RendererEfficiencyEvidenceStatus::Valid; row.fixtureCorrelation = applied.correlation; row.fixtureControlGeneration = applied.controlGeneration;
					row.frameSerial = row.commandSerial = 10; row.submitSerial = row.completionSerial = 9;
					row.viewType = view == 1 ? RenderViewType::Scene : RenderViewType::Game; row.viewId = view; row.lightId = id; row.faceIndex = face;
					row.effectiveOptions.settingsGeneration = 2; row.effectiveOptions.rendererSessionGeneration = 3;
					auto artifacts = std::make_shared<RendererEfficiencyArtifacts>(); artifacts->frameColor = std::make_shared<RendererEfficiencyFrameColor>(); row.artifacts = artifacts;
					rows.push_back(std::move(row));
				}
				bundle["views"].push_back({{"viewType", view == 1 ? "Scene" : "Game"}, {"viewId",view}, {"lightId",ToHexString(id)}, {"faceIndices",faces}});
			}
			// CPU rows exercise the actual protected policy, not physical GPU work.
			std::string error;
			context.Expect(!SelectRendererEfficiencyControlTransition(bundle, applied, options, std::span(rows).first(1), selected, error), "One acknowledged command cannot substitute for the original complete transition cohort.");
			context.Expect(SelectRendererEfficiencyControlTransition(bundle, applied, options, rows, selected, error) && selected.size() == 18, "Both actual required views must retain their complete eighteen-region cohort.");
			context.Expect(selected.front().fixtureCorrelation == applied.correlation && selected.front().fixtureControlGeneration == 7, "Transition export must retain original mutation correlation and actual control generation.");
			auto scene = bundle; scene["views"].erase(scene["views"].begin() + 4, scene["views"].end());
			context.Expect(SelectRendererEfficiencyControlTransition(scene, applied, options, std::span(rows).first(9), selected, error) && selected.size() == 9, "A Scene-only recipe requires nine regions, not an invented second view.");
			context.Expect(!SelectRendererEfficiencyControlTransition(bundle, applied, options, std::span(rows).first(17), selected, error), "Partial native view/cube cohort must remain pending.");
			rows[0].fixtureControlGeneration = 6; context.Expect(!SelectRendererEfficiencyControlTransition(bundle, applied, options, rows, selected, error), "Prior sampled control generation cannot admit the transition."); rows[0].fixtureControlGeneration = 7;
			++rows[0].fixtureCorrelation.mutationGeneration; context.Expect(!SelectRendererEfficiencyControlTransition(bundle, applied, options, rows, selected, error), "Admission must not relabel the original mutation correlation."); --rows[0].fixtureCorrelation.mutationGeneration;
			rows[0].completionSerial = 0; context.Expect(!SelectRendererEfficiencyControlTransition(bundle, applied, options, rows, selected, error), "Recorded-only control is not completed first-visible evidence."); rows[0].completionSerial = 9;
			++rows[0].commandSerial; context.Expect(!SelectRendererEfficiencyControlTransition(bundle, applied, options, rows, selected, error), "Different commands within one native view cannot form its first cohort."); --rows[0].commandSerial;
			for(auto& row : rows) if(row.viewType == RenderViewType::Game) row.commandSerial = 11;
			context.Expect(SelectRendererEfficiencyControlTransition(bundle, applied, options, rows, selected, error), "Real additional-camera command buffers may differ while each output remains coherent.");
			rows[0].effectiveOptions.optionsSessionGeneration = 0; context.Expect(!SelectRendererEfficiencyControlTransition(bundle, applied, options, rows, selected, error), "A missing actual options epoch cannot authorize transition admission."); rows[0].effectiveOptions.optionsSessionGeneration = 1;
			++options.optionsSessionGeneration; context.Expect(!SelectRendererEfficiencyControlTransition(bundle, applied, options, rows, selected, error), "A retired options epoch must reject retained transition rows."); --options.optionsSessionGeneration;
			rows[0].status = RendererEfficiencyEvidenceStatus::Unavailable; context.Expect(!SelectRendererEfficiencyControlTransition(bundle, applied, options, rows, selected, error), "Unavailable first observation cannot be admitted as a transition.");
			// Unit-only counter inputs exercise the actual native sampling rule;
			// they do not claim that a backend-less RenderSystem accepted a queue.
			auto collision = applied; collision.correlation.mutationGeneration = collision.controlGeneration;
			ShadowValidationSampleIntent sampled; auto requested = rows.front();
			requested.fixtureCorrelation = collision.correlation; requested.fixtureControlGeneration = collision.controlGeneration;
			context.Expect(AdmitShadowValidationSample(sampled, requested), "First native sample retains the original transition token.");
			++requested.frameSerial;
			context.Expect(!AdmitShadowValidationSample(sampled, requested), "Identical consumed correlation/control owner suppresses later frame sampling.");
			context.Expect(RendererEfficiencyTransitionNeedsSamplingRearm(collision, requested.fixtureCorrelation), "Independent mutation/control clocks may collide and require actual capture rearming before warmup zero.");
			auto distinct = requested.fixtureCorrelation; ++distinct.mutationGeneration;
			context.Expect(!RendererEfficiencyTransitionNeedsSamplingRearm(collision, distinct), "A genuinely distinct measurement token needs no extra fixture clear.");
			requested.fixtureControlGeneration = 8;
			context.Expect(AdmitShadowValidationSample(sampled, requested), "Typed native sampler permits a distinct control owner with identical correlation; production obtains it from the real queue.");
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.PageVariantRetainsDenseBaselineAndOriginalCleanup", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			MutationFixture fixture; std::string error;
			const std::string original = R"({"asset":{"version":"2.0"},"extras":{"grid":32,"displacement":0}})", dense = R"({"asset":{"version":"2.0"},"extras":{"grid":64,"displacement":0}})", changed = R"({"asset":{"version":"2.0"},"extras":{"grid":64,"displacement":0.5}})";
			std::filesystem::create_directories(fixture.root / "Assets/Models"); std::filesystem::create_directories(fixture.root / "renderer-efficiency-variants");
			const auto source = std::filesystem::path("Assets/Models/grid.gltf"), baseline = std::filesystem::path("renderer-efficiency-variants/dense.json"), variant = std::filesystem::path("renderer-efficiency-variants/changed.json");
			nlohmann::json files = nlohmann::json::array();
			for(const auto& [path, bytes] : std::vector<std::pair<std::filesystem::path, std::string>>{{source, original}, {baseline, dense}, {variant, changed}}) {
				{ std::ofstream stream(fixture.root / path, std::ios::binary); stream << bytes; }
				std::string hash; context.Expect(HashRendererEfficiencyOwnedInput(fixture.root, path, hash, error), "Actual authored input must hash."); files.push_back({{"projectPath",path.generic_string()}, {"sha256",hash}});
			}
			{ std::ifstream stream(fixture.root / "renderer-efficiency-project.json"); nlohmann::json marker; stream >> marker; marker["generatedFiles"] = files; std::ofstream output(fixture.root / "renderer-efficiency-project.json"); output << marker; }
			RendererEfficiencySourcePatch setup; context.Expect(PrepareRendererEfficiencySourcePatch(fixture.root, source, baseline, {}, {}, setup, error) && ApplyRendererEfficiencySourcePatch(fixture.root, setup, error), "Dense unmeasured source baseline must genuinely replace owned initial bytes.");
			RendererEfficiencySourcePatch edit; context.Expect(PrepareRendererEfficiencyPageVariantPatch(fixture.root, source, variant, original, setup.replacementHash, dense, edit, error), "Page variant must prepare against its real current dense baseline.");
			context.Expect(edit.baselineBytes == dense && edit.baselineHash == setup.replacementHash && edit.replacementBytes == changed && edit.replacementHash != edit.baselineHash, "Page publication receipt must compare displaced dense bytes against dense baseline, not initial direct topology.");
			RendererEfficiencySourcePatch unchanged = edit;
			context.Expect(!PrepareRendererEfficiencyPageVariantPatch(fixture.root, source, variant, original, setup.replacementHash, original, unchanged, error) && unchanged.baselineBytes == edit.baselineBytes, "Wrong semantic baseline must fail without replacing the caller's prepared receipt.");
			context.Expect(ApplyRendererEfficiencySourcePatch(fixture.root, edit, error), "Actual displaced source must apply once.");
			RendererEfficiencySourcePatch restore; context.Expect(PrepareRendererEfficiencySourcePatch(fixture.root, source, {}, original, edit.replacementHash, restore, error) && ApplyRendererEfficiencySourcePatch(fixture.root, restore, error) && restore.replacementBytes == original, "Cleanup must retain immutable original direct source independently.");
			using namespace Renderer;
			RendererEfficiencyFixtureIntent applied; applied.controlsApplied = true; applied.controlGeneration = 7;
			applied.correlation = {"owned-page-operation", "shadow-invalidation-11", 0, 31, 2, 3};
			const auto mesh = GenerateUUID(); std::vector<RendererEfficiencyEvidence> rows;
			for(std::uint32_t index = 0; index < 9; ++index) {
				RendererEfficiencyEvidence row; row.status = RendererEfficiencyEvidenceStatus::Valid; row.fixtureCorrelation = applied.correlation; row.fixtureControlGeneration = applied.controlGeneration;
				row.frameSerial = row.commandSerial = 10; row.submitSerial = row.completionSerial = 9; row.effectiveOptions.settingsGeneration = 2; row.effectiveOptions.rendererSessionGeneration = 3;
				row.viewType = RenderViewType::Scene; row.viewId = 1; row.lightId = index < 6 ? ParseUUID("65000000000000000000000000000002").value() : GenerateUUID(); row.faceIndex = index < 6 ? index : 0;
				auto artifacts = std::make_shared<RendererEfficiencyArtifacts>(); ShadowCasterStamp stamp;
				stamp.objectId = fixture.targetId; stamp.meshId = mesh; stamp.meshRevision = stamp.pageContentRevision = stamp.payloadContentRevision = 12;
				stamp.payloadSource = 256; stamp.payloadByteSize = 4096; stamp.residencyGeneration = 4; stamp.lifetimeGeneration = 2;
				artifacts->eligibleMembership.rows.push_back(stamp); row.artifacts = artifacts; rows.push_back(std::move(row));
			}
			// These typed CPU rows test the production admission policy only; they
			// do not assert that any physical GPU submitted or completed this data.
			context.Expect(RendererEfficiencyDensePageBaselineMatches(rows, applied, fixture.targetId, mesh, 12, error), "Every current completed native-page region must satisfy the admission policy.");
			context.Expect(!RendererEfficiencyDensePageBaselineMatches(std::span(rows).first(8), applied, fixture.targetId, mesh, 12, error), "Partial cube/region baseline cannot authorize the page edit.");
			context.Expect(!RendererEfficiencyDensePageBaselineRetryable(std::span(rows).first(8), applied, fixture.targetId, mesh, 12), "Partial baseline cannot request a new control owner.");
			context.Expect(!RendererEfficiencyDensePageBaselineMatches(rows, applied, fixture.targetId, mesh, 13, error), "A later unrelated publication cannot substitute for the retained baseline owner.");
			context.Expect(RendererEfficiencyCompletedControlMatches(rows[0], applied), "Normal completed bundle admission must use its actual applied control owner.");
			rows[0].fixtureControlGeneration = applied.controlGeneration - 1; context.Expect(!RendererEfficiencyCompletedControlMatches(rows[0], applied) && !RendererEfficiencyDensePageBaselineMatches(rows, applied, fixture.targetId, mesh, 12, error), "An older sampled control cannot enter the production completed bundle or authorize a new dense baseline."); rows[0].fixtureControlGeneration = applied.controlGeneration;
			auto queuedOnly = applied; queuedOnly.controlsApplied = false; context.Expect(!RendererEfficiencyCompletedControlMatches(rows[0], queuedOnly), "Queued-only controls cannot accept even an otherwise matching completed row.");
			rows[0].completionSerial = 0; context.Expect(!RendererEfficiencyDensePageBaselineMatches(rows, applied, fixture.targetId, mesh, 12, error), "A queued/late native completion cannot authorize source change."); rows[0].completionSerial = 9;
			auto ownerFallback = std::make_shared<RendererEfficiencyArtifacts>(*rows[0].artifacts); ownerFallback->eligibleMembership.rows[0].payloadSource = 0; rows[0].artifacts = ownerFallback;
			context.Expect(!RendererEfficiencyDensePageBaselineMatches(rows, applied, fixture.targetId, mesh, 12, error), "Owner-buffer fallback cannot prove an actual dense page baseline.");
			context.Expect(RendererEfficiencyDensePageBaselineRetryable(rows, applied, fixture.targetId, mesh, 12), "A full current completed dense-owner fallback may request another actual capture, without accepting the page baseline.");
			context.Expect(!RendererEfficiencyDensePageBaselineRetryable(rows, applied, fixture.targetId, mesh, 13), "A wrong dense owner publication must not use preparation retries.");
			rows[0].fixtureControlGeneration = applied.controlGeneration - 1; context.Expect(!RendererEfficiencyDensePageBaselineRetryable(rows, applied, fixture.targetId, mesh, 12), "Stale generation fallback must not request a new capture."); rows[0].fixtureControlGeneration = applied.controlGeneration;
			context.Expect(!RendererEfficiencyDensePageBaselineRetryable(rows, queuedOnly, fixture.targetId, mesh, 12), "Queued-only controls cannot consume a fallback baseline.");
			ownerFallback->eligibleMembership.rows[0].payloadSource = 256; ownerFallback->eligibleMembership.rows[0].payloadByteSize = 0;
			context.Expect(RendererEfficiencyDensePageBaselineRetryable(rows, applied, fixture.targetId, mesh, 12), "Exact current page owner with unavailable payload bytes may retry physical preparation.");
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.CommittedStepWaitsForActualScenePublication", [](TestContext& context) {
			MutationFixture fixture; std::string error;
			context.Expect(fixture.Start(error), "Owned fixture must start before admitting a typed mutation.");
			// Emulate only the existing scheduler publication boundary. A committed
			// transaction is not itself an immutable renderer scene-change packet.
			const auto before = fixture.objects.PublishSceneChangePacket();
			const auto operation = fixture.host.Status().at("operationId").get<std::string>();
			context.Expect(fixture.host.QueueFixtureTransaction(fixture.root, kProjectGeneration, operation,
				fixture.Prepare("publication-transform", ChangedPose()), fixture.queue, error), "Actual host must enqueue the typed transform.");
			fixture.queue.Apply(fixture.objects, kBoundary);
			fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			context.Expect(fixture.host.Status().at("state") == "AwaitingPublication" && fixture.host.Status().at("mutationGeneration") == 0,
				"Committed zero-publication receipt must not admit a renderer token before the scheduler publishes its exact target change.");
			context.Expect(fixture.objects.LoadPublishedSceneChangePacketShared()->PublicationRevision() == before.PublicationRevision(), "Host receipt processing must never publish an extra scene packet.");
			fixture.objects.PublishSceneChangePacket();
			fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			context.Expect(fixture.host.Status().at("state") == "AwaitingFirstObservation"
				&& fixture.host.Status().at("mutationGeneration") == fixture.objects.GetSpatialRevision()
				&& fixture.objects.GetSpatialRevision() != 0, "Actual fresh publication must authorize exactly the native renderer spatial revision, without a fallback generation.");
			// Idempotent active-baseline writes still need a new scheduler boundary,
			// but must not demand a nonexistent target delta from that packet.
			TransactionalMutationBuilder noop("publication-noop", TransactionOrigin::Automation, "RendererEfficiencyFixture"); noop.ObserveScene(fixture.objects); noop.ObserveObject(fixture.objects, fixture.targetId); noop.SetActive(fixture.targetId, fixture.objects.isObjectActive(fixture.target));
			context.Expect(fixture.host.QueueFixtureTransaction(fixture.root, kProjectGeneration, operation, std::move(noop).Build(), fixture.queue, error), "Owned idempotent baseline must be admitted.");
			fixture.queue.Apply(fixture.objects, kBoundary); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			context.Expect(fixture.host.Status().at("state") == "AwaitingPublication", "No-op receipt must retain its publication barrier.");
			fixture.objects.PublishSceneChangePacket(); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			context.Expect(fixture.host.Status().at("state") == "AwaitingFirstObservation", "Actual new packet must release a truthful idempotent baseline.");
			TransactionalMutationBuilder active("publication-active", TransactionOrigin::Automation, "RendererEfficiencyFixture"); active.ObserveScene(fixture.objects); active.ObserveObject(fixture.objects, fixture.targetId); active.SetActive(fixture.targetId, false);
			context.Expect(fixture.host.QueueFixtureTransaction(fixture.root, kProjectGeneration, operation, std::move(active).Build(), fixture.queue, error), "Owned active mutation must queue.");
			fixture.queue.Apply(fixture.objects, kBoundary); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			context.Expect(fixture.host.Status().at("state") == "AwaitingPublication", "Changed active metadata must wait for exact publication.");
			fixture.objects.PublishSceneChangePacket(); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			context.Expect(fixture.host.Status().at("state") == "AwaitingFirstObservation" && !fixture.objects.isObjectActive(fixture.target), "Actual active metadata publication must admit the authored value.");
			MeshRenderComponent::registerType(); LightComponent::registerType(); fixture.objects.AddComponent<MeshRenderComponent>(fixture.target); fixture.objects.PublishSceneChangePacket();
			// Engine getters and editor producers may use different module reflection
			// contexts. Their authored UUID values must compare by all sixteen bytes.
			entt::meta_ctx producerContext;
			const auto mesh = GenerateUUID(), material = GenerateUUID(); TransactionalMutationBuilder binding("publication-binding", TransactionOrigin::Automation, "RendererEfficiencyFixture"); binding.ObserveScene(fixture.objects); binding.ObserveObject(fixture.objects, fixture.targetId); binding.UpdateComponentProperty(fixture.targetId, "MeshRenderComponent", "meshId", Reflection::Variant(producerContext, mesh));
			binding.UpdateComponentProperty(fixture.targetId, "MeshRenderComponent", "materialId", Reflection::Variant(producerContext, material));
			binding.UpdateComponentProperty(fixture.targetId, "MeshRenderComponent", "taskShaderCullingEnabled", Reflection::Variant(producerContext, false));
			context.Expect(fixture.host.QueueFixtureTransaction(fixture.root, kProjectGeneration, operation, std::move(binding).Build(), fixture.queue, error), "Actual typed resource binding must queue.");
			fixture.queue.Apply(fixture.objects, kBoundary); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			context.Expect(fixture.host.Status().at("state") == "AwaitingPublication", "Resource binding must retain committed proof until publication.");
			// Unrelated component membership advances a broad diagnostic generation;
			// it must not invalidate an unchanged exact authored mesh UUID.
			fixture.objects.AddComponent<LightComponent>(fixture.target); fixture.objects.PublishSceneChangePacket(); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			context.Expect(fixture.host.Status().at("state") == "AwaitingFirstObservation" && fixture.objects.GetComponent<MeshRenderComponent>(fixture.target)->meshId == mesh && fixture.objects.GetComponent<MeshRenderComponent>(fixture.target)->materialId == material && !fixture.objects.GetComponent<MeshRenderComponent>(fixture.target)->taskShaderCullingEnabled, "Exact authored mesh, material and boolean values from a distinct producer context must admit despite unrelated component generation advances.");
			TransactionalMutationBuilder conflict("publication-rejected", TransactionOrigin::Automation, "RendererEfficiencyFixture"); conflict.ObserveScene(fixture.objects); conflict.ObserveObject(fixture.objects, fixture.targetId); conflict.DeleteObject(fixture.targetId); conflict.SetWorldTransform(fixture.objects, fixture.targetId, ChangedPose());
			const auto retained = fixture.Capture().objects.at(fixture.targetId).worldPose;
			const auto retainedSpatialRevision = fixture.objects.GetObjectSpatialRevision(fixture.target);
			context.Expect(fixture.host.QueueFixtureTransaction(fixture.root, kProjectGeneration, operation, std::move(conflict).Build(), fixture.queue, error), "Real conflicting typed batch must reach the normal atomic decision.");
			fixture.queue.Apply(fixture.objects, kBoundary); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue); fixture.objects.PublishSceneChangePacket();
			context.Expect(fixture.host.Status().at("state") == "Failed" && !fixture.host.Active(), "Rejected transaction must never acquire eligible scene publication proof.");
			ExpectPose(context, fixture.Capture().objects.at(fixture.targetId).worldPose, retained, "Actual rejected write-after-delete must leave authored pose intact.");
			context.Expect(retainedSpatialRevision != 0 && fixture.objects.GetObjectSpatialRevision(fixture.target) == retainedSpatialRevision, "Atomic rejection must retain the exact target spatial revision without comparing unrelated global actors.");
			context.Expect(fixture.Start(error), "A fresh owned operation must reset the failed proof state.");
			fixture.objects.PublishSceneChangePacket(); const auto changedOperation = fixture.host.Status().at("operationId").get<std::string>();
			context.Expect(fixture.host.QueueFixtureTransaction(fixture.root, kProjectGeneration, changedOperation, fixture.Prepare("publication-intervened", ChangedPose()), fixture.queue, error), "Fresh operation must admit its own typed target.");
			fixture.queue.Apply(fixture.objects, kBoundary); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			auto intervened = ChangedPose(); intervened.position.offsetMetres.x += 2;
			context.Expect(fixture.objects.SetCanonicalWorldPose(fixture.target, intervened) == WorldCoordinateError::None, "Intervening protected authored pose must actually publish before testing rejection."); fixture.objects.PublishSceneChangePacket(); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			context.Expect(fixture.host.Status().at("state") == "Failed" && fixture.host.Status().at("mutationGeneration") == 0, "A fresh packet with changed authored pose cannot authorize stale committed expectations.");
			context.Expect(fixture.Start(error), "Another fresh operation must not reuse accepted or invalid proof.");
			// Exercise the exact fixture replacement batch through native queue ingress.
			// The old read-set lifetime protects deletion; all new writes use local id
			// one, so only the real committed replacement may authorize publication.
			fixture.objects.PublishSceneChangePacket();
			const auto replacementOperation = fixture.host.Status().at("operationId").get<std::string>();
			const auto originalLifetime = fixture.objects.getObjectLifetimeGeneration(fixture.target);
			TransactionalMutationBuilder replacementCapture("publication-ordered-replacement", TransactionOrigin::Automation, "RendererEfficiencyFixture");
			replacementCapture.ObserveScene(fixture.objects); replacementCapture.ObserveObject(fixture.objects, fixture.targetId);
			auto replacement = std::move(replacementCapture).Build();
			TransactionWriteOperation remove; remove.kind = TransactionWriteKind::DeleteObject; remove.target.objectId = fixture.targetId;
			replacement.writeSet.operations.push_back(std::move(remove));
			TransactionWriteOperation create; create.kind = TransactionWriteKind::CreateObject; create.createdLocalObjectId = 1;
			create.createdStableObjectId = fixture.targetId; create.displayName = "Replacement Caster";
			replacement.writeSet.operations.push_back(std::move(create));
			TransactionWriteOperation component; component.kind = TransactionWriteKind::AddComponent; component.target.localObjectId = 1; component.componentName = "MeshRenderComponent";
			replacement.writeSet.operations.push_back(std::move(component));
			for(const auto& property : {std::string("meshId"), std::string("materialId")})
			{
				TransactionWriteOperation value; value.kind = TransactionWriteKind::UpdateComponentProperty; value.target.localObjectId = 1;
				value.componentName = "MeshRenderComponent"; value.propertyName = property;
				value.propertyValue = Reflection::Variant(producerContext, property == "meshId" ? mesh : material);
				replacement.writeSet.operations.push_back(std::move(value));
			}
			TransactionWriteOperation pose; pose.kind = TransactionWriteKind::SetWorldTransform; pose.target.localObjectId = 1; pose.worldPose = ChangedPose();
			replacement.writeSet.operations.push_back(std::move(pose));
			context.Expect(replacement.writeSet.operations.size() == 6, "Replacement must retain the exact six fixture operations.");
			context.Expect(fixture.host.QueueFixtureTransaction(fixture.root, kProjectGeneration, replacementOperation, std::move(replacement), fixture.queue, error), "Actual host must admit the observed same-UUID replacement batch.");
			fixture.queue.Apply(fixture.objects, kBoundary); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			context.Expect(fixture.host.Status().at("state") == "AwaitingPublication" && fixture.host.Status().at("mutationGeneration") == 0, "Real replacement commit must wait for scheduler publication without admitting a renderer token.");
			const auto replaced = fixture.objects.getObject(fixture.targetId);
			context.Expect(replaced.has_value() && fixture.objects.getObjectLifetimeGeneration(*replaced) != originalLifetime, "Same logical UUID must resolve to its actual new native lifetime.");
			if(replaced)
			{
				fixture.target = *replaced;
				const auto* currentMesh = fixture.objects.GetComponent<MeshRenderComponent>(fixture.target);
				context.Expect(currentMesh && currentMesh->meshId == mesh && currentMesh->materialId == material, "New lifetime must expose both exact authored resource bindings.");
				ExpectPose(context, fixture.Capture().objects.at(fixture.targetId).worldPose, ChangedPose(), "New lifetime must expose the authored canonical pose.");
			}
			fixture.objects.PublishSceneChangePacket(); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			context.Expect(fixture.host.Status().at("state") == "AwaitingFirstObservation" && fixture.host.Status().at("mutationGeneration") == fixture.objects.GetSpatialRevision() && fixture.objects.GetSpatialRevision() != 0,
				"Exact new-lifetime Added packet must release the committed replacement for actual renderer observation.");
			// The following stale-lifetime regression now starts from this accepted
			// replacement, and must still reject a later uncorrelated recreation.
			fixture.objects.PublishSceneChangePacket(); const auto lifetimeOperation = fixture.host.Status().at("operationId").get<std::string>();
			context.Expect(fixture.host.QueueFixtureTransaction(fixture.root, kProjectGeneration, lifetimeOperation, fixture.Prepare("publication-lifetime", ChangedPose()), fixture.queue, error), "Lifetime regression must queue a real typed transform.");
			fixture.queue.Apply(fixture.objects, kBoundary); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			fixture.objects.removeObject(fixture.target); fixture.target = fixture.objects.createObject("Replacement"); fixture.objects.UpdateObjectUUID(fixture.target, fixture.targetId);
			context.Expect(fixture.objects.SetCanonicalWorldPose(fixture.target, ChangedPose()) == WorldCoordinateError::None, "Replacement lifetime must have the expected actual pose before testing stale ownership."); fixture.objects.PublishSceneChangePacket(); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			context.Expect(fixture.host.Status().at("state") == "Failed" && fixture.host.Status().at("mutationGeneration") == 0, "Recycled UUID with a new actual lifetime must reject an old committed publication proof.");
			context.Expect(fixture.Start(error), "A new operation must discard the failed lifetime proof.");
			fixture.objects.AddComponent<MeshRenderComponent>(fixture.target); fixture.objects.PublishSceneChangePacket();
			const auto semanticOperation = fixture.host.Status().at("operationId").get<std::string>();
			TransactionalMutationBuilder authored("publication-material", TransactionOrigin::Automation, "RendererEfficiencyFixture"); authored.ObserveScene(fixture.objects); authored.ObserveObject(fixture.objects, fixture.targetId);
			authored.UpdateComponentProperty(fixture.targetId, "MeshRenderComponent", "materialId", Reflection::Variant(producerContext, material));
			context.Expect(fixture.host.QueueFixtureTransaction(fixture.root, kProjectGeneration, semanticOperation, std::move(authored).Build(), fixture.queue, error), "Distinct-context authored material must queue its exact value.");
			fixture.queue.Apply(fixture.objects, kBoundary); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			TransactionalMutationBuilder interveningMaterial("publication-other-material", TransactionOrigin::Automation, "RendererEfficiencyFixture"); interveningMaterial.ObserveScene(fixture.objects); interveningMaterial.ObserveObject(fixture.objects, fixture.targetId);
			interveningMaterial.UpdateComponentProperty(fixture.targetId, "MeshRenderComponent", "materialId", Reflection::Variant(GenerateUUID()));
			fixture.queue.EnqueueTransaction(std::move(interveningMaterial).Build()); fixture.queue.Apply(fixture.objects, kBoundary); fixture.objects.PublishSceneChangePacket(); fixture.host.PumpFixtureReceiptsAtProtectedFrame(fixture.root, kProjectGeneration, fixture.objects, fixture.queue);
			const auto semanticStatus = fixture.host.Status(); const auto semanticReason = semanticStatus.at("unavailableReason").get<std::string>();
			context.Expect(semanticStatus.at("state") == "Failed" && semanticStatus.at("mutationGeneration") == 0 && semanticReason.find("property=materialId") != std::string::npos && semanticReason.find(ToHexString(fixture.targetId)) != std::string::npos && semanticReason.find("actualType=") != std::string::npos && semanticReason.find("expectedType=") != std::string::npos, "A genuinely different native material UUID must reject with target, property and type diagnostics.");
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.WarmupExportsActualCompletedArtifacts", [](TestContext& context) {
			using namespace Renderer;
			using namespace TerraEngine::Editor::Automation;
			// Serializer-only native-shaped CPU bytes exercise the real writer. This
			// regression never labels its synthetic inputs as physical GPU acceptance.
			RendererEfficiencyEvidenceExportContext capture; capture.projectRoot = std::filesystem::temp_directory_path() / ("TerraRendererWarmup-" + ToHexString(GenerateUUID()));
			capture.fixtureId = "shadow-invalidation"; capture.fixtureHash = std::string(64,'a'); capture.operationId = ToHexString(GenerateUUID()); capture.fixtureStepId = "shadow-invalidation-00";
			capture.outputDirectory = capture.projectRoot / "renderer-efficiency-evidence" / capture.operationId / "step0";
			capture.runId = "warmup-serializer-unit"; capture.seed = 65001; capture.mode = "Candidate"; capture.settingsGeneration = 2; capture.mutationGeneration = 3; capture.rendererSessionGeneration = 4;
			std::filesystem::create_directories(capture.projectRoot);
			std::ofstream(capture.projectRoot / "renderer-efficiency-project.json") << nlohmann::json{{"schemaVersion",1},{"kind","RendererEfficiencyValidationProject"},{"projectRoot",capture.projectRoot.generic_string()},{"fixtureId",capture.fixtureId},{"fixtureHash",capture.fixtureHash}};
			RendererEfficiencyEvidence row; row.status = RendererEfficiencyEvidenceStatus::Valid; row.resolution = {1,1}; row.frameSerial = 5; row.commandSerial = 6; row.submitSerial = row.completionSerial = 7;
			row.viewType = RenderViewType::Scene; row.viewId = 1; row.lightId = GenerateUUID(); row.contentOwner = "native-shaped-unit-owner";
			row.effectiveOptions.settingsGeneration = 2; row.effectiveOptions.rendererSessionGeneration = 4; row.fixtureCorrelation = {capture.operationId,capture.fixtureStepId,0,3,2,4};
			for(auto* counter : {&row.eligibleCasterSegments,&row.submittedCasterSegments,&row.actualDrawSubmissions}) { counter->status = RendererEfficiencyEvidenceStatus::Valid; counter->value = 0; counter->reason.clear(); }
			auto bytes = std::make_shared<RendererEfficiencyArtifacts>(); bytes->shadowKey.rendererSessionGeneration = 4; bytes->shadowKey.viewType = row.viewType; bytes->shadowKey.viewId = 1; bytes->shadowKey.lightId = row.lightId;
			bytes->shadowKey.resourceKind = ShadowResourceKind::SpotAtlas; bytes->shadowKey.allocationGeneration = 1; bytes->shadowKey.extent = {1,1}; bytes->shadowKey.depthFormat = bytes->depthSourceFormat = 126; bytes->contentToken = 1;
			const auto bits = std::bit_cast<std::uint32_t>(0.5F); for(unsigned byte = 0; byte < 4; ++byte) bytes->depthF32LE.push_back(static_cast<std::uint8_t>(bits >> (byte*8))); bytes->coverageU8 = {1};
			auto color = std::make_shared<RendererEfficiencyFrameColor>(); color->width = color->height = 1; color->sourceFormat = 44; color->bytes = {10,20,30,255}; bytes->frameColor = color; row.artifacts = bytes;
			std::string previousCounters;
			for(const std::uint64_t completed : {0ULL,59ULL,60ULL})
			{
				capture.measurementPhase = RendererEfficiencyCompletedExportPhase(completed,60); row.fixtureCorrelation.stepFrame = completed;
				RendererEfficiencyEvidenceExportResult result; std::string error;
				const bool exported = ExportRendererEfficiencyEvidence(capture,std::span(&row,1),result,error);
				context.Expect(exported, "Both phases must use the strict native artifact writer: " + error);
				context.Expect(result.frames.size() == 1 && result.frames[0].at("measurementPhase") == (completed < 60 ? "Warmup" : "Measured")
					&& result.frames[0].at("stepFrame") == completed, "Captured phases retain exact absolute step frames without underflow or measured renumbering.");
				const auto counter = result.artifacts.back().at("path").get<std::string>(); context.Expect(counter != previousCounters, "Phase/frame-qualified content addressed counter files must not overwrite prior observations."); previousCounters = counter;
				std::ifstream input(capture.projectRoot / counter); const auto native = nlohmann::json::parse(input);
				context.Expect(native.at("measurementPhase") == capture.measurementPhase && native.at("stepFrame") == std::to_string(completed), "The actual native counter artifact must retain the frozen export phase and frame identity.");
			}
			bytes->coverageU8.clear(); capture.measurementPhase = RendererEfficiencyCompletedExportPhase(0,60);
			RendererEfficiencyEvidenceExportResult rejected; std::string error;
			context.Expect(!ExportRendererEfficiencyEvidence(capture,std::span(&row,1),rejected,error) && rejected.frames.empty(), "Warmup must reject incomplete bytes through the same strict measured artifact gate.");
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.PendingDiagnosticsRequireCurrentNativeIdentity", [](TestContext& context) {
			using namespace Renderer;
			using TerraEngine::Editor::Automation::RendererEfficiencyFixtureControlsMatch;
			using TerraEngine::Editor::Automation::RendererEfficiencyPendingObservationReason;
			RendererEfficiencyFixtureIntent expected; expected.correlation = {"owned-operation","stable-00",3,4,5,6};
			auto applied = expected; applied.controlsApplied = true; applied.controlGeneration = 7;
			RendererEfficiencyEvidence row; row.fixtureCorrelation = expected.correlation; row.effectiveOptions.settingsGeneration = 5; row.effectiveOptions.rendererSessionGeneration = 6;
			row.status = RendererEfficiencyEvidenceStatus::Unavailable; row.fallbackReason = "Caster preparation was incomplete."; row.unavailableReason = "Complete native six-face cube payload is unavailable.";
			const auto reason = [&](const RendererEfficiencyEvidence& value) { return RendererEfficiencyPendingObservationReason(expected,applied,std::span(&value,1)); };
			context.Expect(reason(row) == row.fallbackReason + "; " + row.unavailableReason, "Current actual unavailable rows must diagnose Pending with both native reasons verbatim.");
			for(unsigned lane = 0; lane < 8; ++lane)
			{
				auto stale = row;
				switch(lane) { case 0: stale.fixtureCorrelation.operationId = "prior"; break; case 1: stale.fixtureCorrelation.fixtureStepId = "prior"; break;
				case 2: ++stale.fixtureCorrelation.stepFrame; break; case 3: ++stale.fixtureCorrelation.mutationGeneration; break;
				case 4: ++stale.fixtureCorrelation.settingsGeneration; break; case 5: ++stale.fixtureCorrelation.rendererSessionGeneration; break;
				case 6: ++stale.effectiveOptions.settingsGeneration; break; case 7: ++stale.effectiveOptions.rendererSessionGeneration; break; }
				context.Expect(reason(stale).empty(), "Prior operation/step/frame/mutation/options/session observations must not replace current Pending diagnostics.");
			}
			context.Expect(RendererEfficiencyFixtureControlsMatch(expected,applied), "Actual matching protected controls permit only the separate completed-bundle check.");
			applied.controlsApplied = false; applied.controlReason = "Actual protected camera rejection.";
			context.Expect(!RendererEfficiencyFixtureControlsMatch(expected,applied)
				&& RendererEfficiencyPendingObservationReason(expected,applied,{}) == applied.controlReason, "Native control rejection remains Pending and cannot manufacture readiness.");
			for(unsigned lane = 0; lane < 6; ++lane)
			{
				auto stale = applied;
				switch(lane) { case 0: stale.correlation.operationId = "prior"; break; case 1: stale.correlation.fixtureStepId = "prior"; break;
				case 2: ++stale.correlation.stepFrame; break; case 3: ++stale.correlation.mutationGeneration; break;
				case 4: ++stale.correlation.settingsGeneration; break; case 5: ++stale.correlation.rendererSessionGeneration; break; }
				context.Expect(!RendererEfficiencyFixtureControlsMatch(expected,stale)
					&& RendererEfficiencyPendingObservationReason(expected,stale,{}) != stale.controlReason, "A prior protected control reason must not diagnose the current pre-admission step.");
			}
			applied = {}; context.Expect(!RendererEfficiencyFixtureControlsMatch(expected,applied)
				&& !RendererEfficiencyPendingObservationReason(expected,applied,{}).empty(), "Queued ingress without exact protected acknowledgement is Pending.");
			applied = expected; applied.controlsApplied = true; applied.controlGeneration = 0;
			context.Expect(!RendererEfficiencyFixtureControlsMatch(expected,applied), "An applied boolean without actual control publication generation is insufficient.");
			applied.controlGeneration = 7; row.status = RendererEfficiencyEvidenceStatus::Valid; row.completionSerial = 0;
			context.Expect(!reason(row).empty(), "Incomplete native completion must remain diagnostic and cannot pass the completed-bundle gate.");
			row.completionSerial = 9; context.Expect(reason(row).empty(), "Qualified valid observations must not retain an old unavailable diagnostic.");
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.GpuOnlySeekUsesSameRootDynamicInput", [](TestContext& context) {
			using namespace Animation;
			using TerraEngine::Editor::Automation::RendererEfficiencyGpuAnimationAuthorityMatches;
			const UUID owner = GenerateUUID(), mesh = GenerateUUID();
			// Build native typed publications, deliberately leaving compact CPU-only
			// status/object-generation/snapshot lanes at their production defaults.
			const auto makeFrame = [&](std::uint32_t tick, std::uint32_t work, std::uint64_t authority) {
				auto page = std::make_shared<AnimationAuthorityPage>(); page->changedMasks.resize(2);
				AnimationAuthorityRow row; row.key = {7,3,11}; row.ownerObjectId = owner; row.componentGeneration = 2;
				row.sourceGeneration = 1; row.poseGeneration = work; row.active = true; row.safeFallback = false; page->rows.push_back(row);
				auto root = std::make_shared<AnimationAuthorityFrame>(); root->frameId = authority; root->authorityGeneration = authority;
				root->sessionGeneration = 11; root->lifecycleGeneration = 1; root->receipt.outcome = AnimationAuthorityPublicationOutcome::Published; root->receipt.authorityGeneration = authority;
				root->pages = std::make_shared<const std::vector<std::shared_ptr<const AnimationAuthorityPage>>>(std::vector<std::shared_ptr<const AnimationAuthorityPage>>{page});
				auto sidecars = std::make_shared<AnimationFrameSidecars>(); AnimationSpecialistRecord candidate; candidate.key = {7,3,11}; candidate.authorityGeneration = authority; candidate.kind = 1; candidate.status = AnimationStatus::Ok; sidecars->gpuCandidates.push_back(candidate); root->sidecars = sidecars;
				auto gpu = std::make_shared<PublishedGpuAnimationPresentationPlan>(); gpu->presentationGeneration = authority; gpu->sourceGeneration = authority;
				gpu->safeFallback = false; gpu->loweringStatus = GpuAnimationLoweringStatus::Ok; gpu->planStatus = GpuAnimationPlanStatus::Ok;
				GpuAnimationOwnerObjectMapping mapping; mapping.ownerObjectId = owner; mapping.instanceSlot = 7; mapping.instanceGeneration = 3; mapping.componentGeneration = 2; mapping.stableInstanceId = (7ULL<<32)|3; mapping.workPacketIndex = 0; gpu->ownerObjectMappings.push_back(mapping);
				GpuAnimationWorkPacket packet; packet.stableInstanceId = mapping.stableInstanceId; packet.stableProgramId = 123; packet.sampleTick = 0; gpu->plan.workPackets.push_back(packet);
				auto lanes = std::make_shared<GpuAnimationPlanDynamicLanes>(); GpuAnimationGenerationRecord generation;
				generation.instanceSlot = 7; generation.instanceGeneration = 3; generation.componentGeneration = 2; generation.workGeneration = work;
				generation.programGeneration = generation.controllerGeneration = generation.clipGeneration = generation.skeletonGeneration = generation.meshGeneration = generation.resourceGeneration = generation.rendererGeneration = generation.sceneGeneration = 1;
				lanes->generationRecords.push_back(generation); lanes->sampleTicks.push_back(tick); GpuAnimationRenderIdentity identity; identity.meshId = mesh; identity.meshHeaderVersion = 1; identity.deformablePayloadGeneration = identity.rendererGeneration = identity.sceneGeneration = identity.skeletonCompatibilityFingerprint = 1; lanes->renderIdentities.push_back(identity); gpu->plan.retainedDynamicLanes = lanes;
				auto frame = std::make_shared<AnimationPresentationFrame>(); frame->authority = root; frame->gpuPlan = gpu; frame->presentationGeneration = authority; frame->postPhysicsBoundary.schedulerGeneration = 1;
				frame->ownerBindings = std::make_shared<const std::vector<AnimationPresentationFrame::OwnerBinding>>(std::vector<AnimationPresentationFrame::OwnerBinding>{{owner,row.key}}); return frame;
			};
			const auto prior = makeFrame(0,10,500); auto current = makeFrame(0,10,510);
			AnimationCommandReceipt receipt; receipt.status = AnimationStatus::Ok; receipt.target = {7,3}; receipt.publicationGeneration = 510;
			context.Expect(RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,true,0), "A real zero GPU input baseline needs no fabricated CPU pose or CPU row status.");
			current = makeFrame(30,11,520); receipt.publicationGeneration = 520;
			context.Expect(current->gpuPlan->plan.WorkPackets()[0].sampleTick == 0 && RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,false,30), "Changed seek must resolve actual dynamic tick 30, not retained static tick zero.");
			const auto reset = makeFrame(0,12,530); auto resetReceipt = receipt; resetReceipt.publicationGeneration = 530;
			context.Expect(RendererEfficiencyGpuAnimationAuthorityMatches(*reset,*current,resetReceipt,owner,true,0), "A baseline reset after tick30 must select actual new known-zero GPU input with unchanged source bindings.");
			context.Expect(!RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,true,0), "Nonzero GPU input cannot prove a zero baseline.");
			context.Expect(!RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,GenerateUUID(),false,30), "An unrelated full owner UUID must reject GPU proof.");
			receipt.target.generation++; context.Expect(!RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,false,30), "Recycled animator handle rejects the old GPU candidate."); receipt.target.generation--;
			receipt.publicationGeneration++; context.Expect(!RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,false,30), "A stale authority root cannot prove command completion."); receipt.publicationGeneration--;
			current = makeFrame(30,10,520); context.Expect(!RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,false,30), "Tick alone cannot prove a newly selected work-content generation.");
			current = makeFrame(0,11,520); context.Expect(!RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,false,30), "Accepted command cannot substitute for the actual requested sample tick.");
			current = makeFrame(30,11,520); auto gpu = std::const_pointer_cast<PublishedGpuAnimationPresentationPlan>(current->gpuPlan);
			gpu->ownerObjectMappings[0].workPacketIndex = 99; context.Expect(!RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,false,30), "Out-of-range packet indices reject before native lane access."); gpu->ownerObjectMappings[0].workPacketIndex = 0;
			gpu->ownerObjectMappings.push_back(gpu->ownerObjectMappings.front()); context.Expect(!RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,false,30), "Ambiguous owner mappings cannot choose a GPU packet."); gpu->ownerObjectMappings.pop_back();
			auto lanes = std::const_pointer_cast<GpuAnimationPlanDynamicLanes>(gpu->plan.retainedDynamicLanes); lanes->renderIdentities[0].meshId = GenerateUUID(); context.Expect(!RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,false,30), "Seek must preserve the exact full native mesh/source identity."); lanes->renderIdentities[0].meshId = mesh;
			lanes->sampleTicks.clear(); context.Expect(!RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,false,30), "Incomplete dynamic lanes reject before accessor reads."); lanes->sampleTicks.push_back(30);
			gpu->plan.workPackets[0].flags = static_cast<std::uint32_t>(GpuAnimationWorkFlag::UseHoldLastFallback); context.Expect(!RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,false,30), "Retained safe output is not actual selected GPU visual work."); gpu->plan.workPackets[0].flags = 0;
			auto root = std::const_pointer_cast<AnimationAuthorityFrame>(current->authority); auto sidecars = std::const_pointer_cast<AnimationFrameSidecars>(root->sidecars); sidecars->gpuCandidates[0].authorityGeneration--;
			context.Expect(!RendererEfficiencyGpuAnimationAuthorityMatches(*current,*prior,receipt,owner,false,30), "Candidate from another authority root cannot prove this command.");
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.SeekUsesNativePlaybackTransitions", [](TestContext& context) {
			using namespace TerraEngine::Animation;
			using TerraEngine::Editor::Automation::RendererEfficiencySeekCommandKinds;
			for(const auto initial : {AnimatorPlaybackRunState::Stopped, AnimatorPlaybackRunState::Playing, AnimatorPlaybackRunState::Paused}) {
				AnimatorPlayback playback; AnimatorPlaybackConfiguration configuration; configuration.durationTicks = 100;
				context.Expect(playback.Configure(configuration) == AnimationStatus::Ok, "Native playback must accept the bounded fixture configuration.");
				if(initial != AnimatorPlaybackRunState::Stopped) context.Expect(playback.Play() == AnimationStatus::Ok, "Playing fixture state must be established by its real native transition.");
				if(initial == AnimatorPlaybackRunState::Paused) context.Expect(playback.Pause() == AnimationStatus::Ok, "Paused fixture state must be established by its real native transition.");
				const auto kinds = RendererEfficiencySeekCommandKinds(playback.Snapshot().runState);
				context.Expect(kinds.size() == (initial == AnimatorPlaybackRunState::Playing ? 2U : 1U) && kinds.back() == AnimationCommandKind::Seek,
					"Stopped and paused playback must seek directly; only playing playback needs a Pause transition.");
				for(const auto kind : kinds) context.Expect((kind == AnimationCommandKind::Pause ? playback.Pause() : playback.Seek(30)) == AnimationStatus::Ok,
					"Every selected command must be accepted by the actual native playback state machine.");
				context.Expect(playback.Snapshot().absoluteTick == 30 && playback.Snapshot().runState != AnimatorPlaybackRunState::Playing,
					"Native seek must expose its exact tick while preserving a frozen stopped or paused state.");
			}
			using TerraEngine::Editor::Automation::RendererEfficiencyAnimationAuthorityMatches;
			const auto objectId = GenerateUUID();
			AnimationCommandReceipt receipt; receipt.target = {7, 3}; receipt.status = AnimationStatus::Ok; receipt.publicationGeneration = 500;
			AnimationAuthorityRow row; row.key = {7, 3, 11}; row.ownerObjectId = objectId; row.authorityPublicationGeneration = 500; row.status = AnimationStatus::Ok; row.safeFallback = false;
			row.objectGeneration = 1; row.componentGeneration = 2;
			AnimationAuthorityFrame frame; frame.sessionGeneration = 11; frame.lifecycleGeneration = 1; frame.authorityGeneration = 500;
			frame.receipt.outcome = AnimationAuthorityPublicationOutcome::Published; frame.receipt.authorityGeneration = 500;
			auto snapshot = std::make_shared<AnimationSnapshot>(); snapshot->animator = receipt.target; snapshot->publicationGeneration = 2; row.snapshot = snapshot;
			context.Expect(RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, objectId, false, 30, snapshot),
				"A small retained pose counter must not be compared with the independently advancing command authority counter.");
			frame.sessionGeneration = 12;
			context.Expect(!RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, objectId, false, 30, snapshot)
				&& !RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, GenerateUUID(), false, 30, snapshot),
				"A different session or object must not satisfy the accepted command publication.");
			frame.sessionGeneration = 11; row.key.lifetimeGeneration = 4;
			context.Expect(!RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, objectId, false, 30, snapshot), "A recycled animator handle must reject the old receipt.");
			row.key.lifetimeGeneration = 3; row.authorityPublicationGeneration = 499;
			context.Expect(!RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, objectId, false, 30, snapshot), "A changed seek must wait for the actual command row publication.");
			context.Expect(RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, objectId, true, 0, snapshot), "An unchanged baseline pose may be retained after its exact seek completes in the newer published authority root.");
			context.Expect(!RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, GenerateUUID(), true, 0, snapshot)
				&& !RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, objectId, true, 30, snapshot), "Baseline reuse must retain exact ownership and the actual zero playback tick.");
			frame.authorityGeneration = 499; frame.receipt.authorityGeneration = 499;
			context.Expect(!RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, objectId, true, 0, snapshot), "A stale completed authority frame cannot prove baseline command completion.");
			frame.authorityGeneration = 500; frame.receipt.authorityGeneration = 499;
			context.Expect(!RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, objectId, true, 0, snapshot), "An incoherent publication receipt cannot prove baseline command completion.");
			frame.receipt.authorityGeneration = 500; row.safeFallback = true;
			context.Expect(!RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, objectId, true, 0, snapshot), "A fallback output cannot prove the expected baseline pose.");
			row.safeFallback = false; snapshot->playback.normalizedTime = 0.5F;
			context.Expect(!RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, objectId, true, 0, snapshot), "A retained nonzero pose cannot prove the requested zero baseline.");
			snapshot->playback.normalizedTime = 0; frame.lifecycleGeneration = 0;
			context.Expect(!RendererEfficiencyAnimationAuthorityMatches(row, frame, receipt, objectId, true, 0, snapshot), "A missing authority lifecycle must reject the retained baseline.");
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.InstalledBindingAdmissionKeepsAssetBindingsAlive", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			using Json = nlohmann::json;
			MutationFixture fixture;
			// Sealed-recipe Start admits an identity job. Own a small live pool for
			// this case and drain it before the fixture removes its recipe files.
			struct PoolScope
			{
				PoolScope() { ThreadPool::instance().shutdown(); ThreadPool::instance().initialize(2); }
				~PoolScope() { ThreadPool::instance().shutdown(); }
			} poolScope;
			// Establish reflection explicitly before constructing the binding batch.
			MeshRenderComponent::registerType();
			// Use a real immutable recipe identity rather than the legacy shorthand fixture hash.
			const auto bindingFixtureHash = ComputeTestSha256("InstalledBindingAdmissionKeepsAssetBindingsAlive-v1");
			fixture.objects.AddComponent<MeshRenderComponent>(fixture.target);
			fixture.objects.PublishSceneChangePacket();
			const auto meshId = GenerateUUID(), materialId = GenerateUUID(), packageId = GenerateUUID();
			const Json cameras = Json::array({{{"viewType", "Scene"}, {"viewId", 1}, {"position", {0, 0, 5}},
				{"target", {0, 0, 0}}, {"referenceOrigin", {0, 0, 0}}, {"up", {0, 1, 0}},
				{"verticalFovDegrees", 60}, {"nearPlane", 0.1}, {"farPlane", 100}}});
			const Json extent{{"width", 1920}, {"height", 1080}};
			const Json cameraReceipt{{"schemaVersion", 1}, {"cameras", cameras}, {"nativeExtent", extent}};
			// Seal a genuine installed-style recipe. Nonempty two-property bindings
			// exercise the real host JSON iteration, not a duplicate test loop.
			const Json steps{{"schemaVersion", 1}, {"fixtureId", "shadow-invalidation"}, {"fixtureHash", bindingFixtureHash},
				{"seed", 0}, {"stage", "P1"}, {"cameras", cameras}, {"nativeExtent", extent},
				{"assets", Json::array({{{"sourcePath", "Models/Caster.gltf"}, {"packageGuid", ToHexString(packageId)},
					{"requiredSubAssets", Json::array({{{"bindingKey", "caster.mesh"}, {"kind", "Mesh"}}, {{"bindingKey", "caster.material"}, {"kind", "Material"}}})},
					{"requiredCook", {{"minimumLods", 1}, {"minimumMeshlets", 1}}}}})},
				{"targets", {{"caster", {{"objectId", ToHexString(fixture.targetId)}, {"componentName", "MeshRenderComponent"},
					{"assetBindings", {{"meshId", "caster.mesh"}, {"materialId", "caster.material"}}}}}}},
				{"steps", Json::array({{{"id", "stable"}, {"ordinal", 0}, {"warmupFrames", 60}, {"measuredFrames", 300},
					{"requiredBundle", {{"views", Json::array()}}}}})}};
			const auto write = [](const std::filesystem::path& path, const std::string& bytes) { std::ofstream(path, std::ios::binary) << bytes; };
			write(fixture.root / "renderer-efficiency-camera.json", cameraReceipt.dump());
			write(fixture.root / "renderer-efficiency-steps.json", steps.dump());
			Json marker{{"schemaVersion", 1}, {"kind", "RendererEfficiencyValidationProject"},
				{"projectRoot", std::filesystem::canonical(fixture.root).generic_string()}, {"fixtureId", "shadow-invalidation"},
				{"fixtureHash", bindingFixtureHash}, {"stepsPath", "renderer-efficiency-steps.json"}, {"stepsHash", ComputeTestSha256(steps.dump())},
				{"cameraHash", ComputeTestSha256(cameraReceipt.dump())}};
			write(fixture.root / "renderer-efficiency-project.json", marker.dump());
			Renderer::RendererEfficiencyOptionsSnapshot options; options.optionsSessionGeneration = 3;
			std::string error;
			const bool started = fixture.host.Start(fixture.root, kProjectGeneration,
				{{"fixtureId", "shadow-invalidation"}, {"fixtureHash", bindingFixtureHash}, {"stage", "P1"}, {"runId", "binding-regression"},
				{"seed", 0}, {"runIndex", 0}, {"runs", 3}, {"warmupFrames", 60}, {"measuredFrames", 300}}, options, error);
			context.Expect(started, "Sealed installed-style recipe must start: " + error);
			if(!started) return;
			// Admission consumes already-qualified publication records; this case
			// tests their transactional binding lifetime, not native resource loading.
			RendererEfficiencyPublishedBinding mesh, material;
			mesh.bindingKey = "caster.mesh"; mesh.assetId = meshId; mesh.packageGuid = packageId; mesh.kind = "Mesh";
			mesh.resourceGeneration = 1; mesh.cookedLods = mesh.cookedMeshlets = 1;
			material.bindingKey = "caster.material"; material.assetId = materialId; material.packageGuid = packageId; material.kind = "Material"; material.resourceGeneration = 1;
			const auto operation = fixture.host.Status().at("operationId").get<std::string>();
			const bool admitted = fixture.host.AdmitPublishedFixtureBindings(fixture.root, kProjectGeneration, operation, {mesh, material}, fixture.objects, fixture.queue, error);
			context.Expect(admitted, "Actual host must retain nonempty assetBindings through admission: " + error);
			if(!admitted) return;
			const auto* before = fixture.objects.GetComponent<MeshRenderComponent>(fixture.target);
			context.Expect(before && before->meshId == EMPTY_UUID && before->materialId == EMPTY_UUID, "Admission must queue both bindings without changing live storage.");
			const auto committed = fixture.queue.Apply(fixture.objects, kBoundary);
			context.Expect(committed.transactionResults.size() == 1 && committed.transactionResults.front().status == TransactionStatus::Committed,
				"The existing protected boundary must commit the real binding batch.");
			const auto* after = fixture.objects.GetComponent<MeshRenderComponent>(fixture.target);
			context.Expect(after && after->meshId == meshId && after->materialId == materialId, "Both exact UUID properties must become visible together after flush.");
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.BindingIdenticalTextureReferencesAreOnePublication", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			using Json = nlohmann::json;
			const auto root = std::filesystem::temp_directory_path() / ("TerraBindingMultiplicity-" + ToHexString(GenerateUUID()));
			struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); } } cleanup{root};
			std::filesystem::create_directories(root / "Assets/Models"); std::filesystem::create_directories(root / "Build/Assets/Models");
			const std::string source = "authored";
			std::uint64_t sourceHash = 1469598103934665603ULL;
			for(const auto byte : source) { sourceHash ^= static_cast<unsigned char>(byte); sourceHash *= 1099511628211ULL; }
			std::ostringstream hashText; hashText << std::hex << sourceHash;
			const std::string package = "65100000000000000000000000000004", texture = "65200000000000000000000000000004";
			const auto write = [](const std::filesystem::path& path, const std::string& bytes) { std::ofstream(path, std::ios::binary) << bytes; };
			write(root / "Assets/Models/Cutout.gltf", source); write(root / "Build/Assets/Models/Cutout.tetf", "cooked");
			write(root / "Assets/Models/Cutout.gltf.meta", Json{{"guid", package}, {"importerName", "GltfPackageImporter"}, {"lastModified", 0}, {"sourcePath", "Models/Cutout.gltf"}, {"compiledPath", ""}, {"settings", Json::object()}}.dump());
			const Json child{{"guid", texture}, {"name", "CutoutAlpha"}, {"compiledPath", "Models/Cutout.tetf"}, {"sourcePath", "Models/Cutout.png"}, {"usage", "BaseColor"}};
			Json receipt{{"packageGuid", package}, {"status", "Succeeded"}, {"package", {{"sourceHash", hashText.str()}}}, {"generatedTextures", Json::array({child, child})}};
			const Json steps{{"schemaVersion", 1}, {"assets", Json::array({{{"sourcePath", "Models/Cutout.gltf"}, {"packageGuid", package},
				{"requiredSubAssets", Json::array({{{"bindingKey", "cutout.texture"}, {"kind", "Texture"}, {"name", "CutoutAlpha"}}})}, {"requiredCook", {{"minimumLods", 0}, {"minimumMeshlets", 0}}}}})}};
			write(root / "Assets/Models/Cutout.gltf.import.json", receipt.dump());
			RendererEfficiencyFixtureBindingPreparation prepared; std::string error;
			context.Expect(PrepareRendererEfficiencyFixtureBindings(root, steps, prepared, error), "Repeated identical native texture dependency rows must resolve one actual child publication.");
			context.Expect(prepared.candidates.size() == 1 && prepared.candidates.front().binding.assetId == ParseUUID(texture).value(), "Identical dependencies must preserve the exact imported child GUID.");
			// Detached importer metadata alone cannot invent a live publication.
			// Query a fresh real typed cache identity, without loading or mutating it.
			auto missingPublication = prepared;
			missingPublication.candidates.front().binding.assetId = GenerateUUID();
			std::vector<RendererEfficiencyPublishedBinding> published(1);
			published.front().bindingKey = "retained";
			context.Expect(!ResolveRendererEfficiencyFixtureBindings(missingPublication, ResourceManager::Instance(), published, error),
				"An imported child without a current cache payload must not resolve.");
			context.Expect(error.find("bindingKey=cutout.texture") != std::string::npos
				&& error.find("assetId=" + ToHexString(missingPublication.candidates.front().binding.assetId)) != std::string::npos
				&& error.find("kind=Texture") != std::string::npos && error.find("state=Unpublished") != std::string::npos
				&& error.find("present=false generation=0") != std::string::npos
				&& error.find("registeredPath=") != std::string::npos && error.find("deformablePayloadGeneration=0") != std::string::npos,
				"Missing-publication diagnostics must name the actual child and copied state, rather than blame a busy writer.");
			context.Expect(published.size() == 1 && published.front().bindingKey == "retained", "Resolution rejection preserves prior output.");
			// Same-name dependencies with any changed metadata or identity remain
			// ambiguous. Rejection leaves the prior detached preparation unchanged.
			for(const auto& field : {std::string("usage"), std::string("guid"), std::string("compiledPath")}) {
				auto conflicting = receipt;
				conflicting["generatedTextures"][1][field] = field == "guid" ? "65200000000000000000000000000005" : "different";
				write(root / "Assets/Models/Cutout.gltf.import.json", conflicting.dump());
				context.Expect(!PrepareRendererEfficiencyFixtureBindings(root, steps, prepared, error) && error.find("ambiguous") != std::string::npos,
					"Conflicting native same-name dependency records must remain ambiguous.");
				context.Expect(prepared.candidates.size() == 1 && prepared.candidates.front().binding.assetId == ParseUUID(texture).value(), "Failed binding preparation must leave the prior exact publication unchanged.");
			}
		});
		registry.Add("Renderer.Efficiency.Editor.Mutation.ReceiptCommittedOnlyAfterFlushAndConsumedOnce", [](TestContext& context) {
			MutationFixture fixture;
			fixture.queue.EnqueueTransaction(fixture.Prepare("receipt-first", ChangedPose()));
			fixture.queue.EnqueueTransaction(fixture.Prepare("receipt-second", ChangedPose()));
			context.Expect(!fixture.queue.TakeCompletedAutomationTransactionReceipt("receipt-first"), "Pending ingress has no completed receipt.");
			const auto batch = fixture.queue.Apply(fixture.objects, kBoundary);
			context.Expect(!fixture.queue.TakeCompletedAutomationTransactionReceipt("wrong-id"), "An unmatched lookup must consume nothing.");
			const auto first = fixture.queue.TakeCompletedAutomationTransactionReceipt("receipt-first");
			context.Expect(first.has_value(), "The real protected flush must retain the automation decision by ID.");
			context.ExpectEqual(std::string("receipt-first"), first->transactionId, "Receipt identity must match the requested transaction.");
			context.Expect(first->status == TransactionStatus::Committed && first->appliedWriteCount > 0 && first->rejectedWriteCount == 0,
				"A committed receipt must copy actual write counts, not pending estimates.");
			context.ExpectEqual(batch.transactionResults.front().spatialPublicationGeneration, first->spatialPublicationGeneration,
				"The automation receipt must preserve the actual protected spatial publication generation.");
			context.Expect(!fixture.queue.TakeCompletedAutomationTransactionReceipt("receipt-first"), "A decision is consumable once.");
			context.Expect(fixture.queue.TakeCompletedAutomationTransactionReceipt("receipt-second").has_value(), "Taking one ID must leave another decision intact.");
		});

		registry.Add("Renderer.Efficiency.Editor.Mutation.ReceiptRejectedDecisionPreservesLivePose", [](TestContext& context) {
			MutationFixture fixture;
			const auto before = fixture.Capture();
			TransactionalMutationBuilder builder("receipt-rejected", TransactionOrigin::Automation, "RendererEfficiencyFixture");
			builder.SetWorldTransform(fixture.objects, fixture.targetId, ChangedPose()).DeleteObject(fixture.targetId).SetName(fixture.targetId, "After deletion");
			fixture.queue.EnqueueTransaction(std::move(builder).Build());
			context.Expect(!fixture.queue.TakeCompletedAutomationTransactionReceipt("receipt-rejected"), "Preflight has not completed while queued.");
			const auto batch = fixture.queue.Apply(fixture.objects, kBoundary);
			ExpectRejectedWithoutWrites(context, batch, fixture, before);
			const auto receipt = fixture.queue.TakeCompletedAutomationTransactionReceipt("receipt-rejected");
			context.Expect(receipt.has_value(), "Rejected batches must retain a terminal automation receipt.");
			context.Expect(receipt->status == batch.transactionResults.front().status && receipt->appliedWriteCount == 0
				&& receipt->rejectedWriteCount > 0 && !receipt->message.empty(), "Rejection receipts must preserve the real decision and diagnostic.");
		});

		registry.Add("Renderer.Efficiency.Editor.Mutation.ReceiptCanceledOnlyAfterFlush", [](TestContext& context) {
			MutationFixture fixture;
			const auto before = fixture.Capture();
			fixture.queue.EnqueueTransaction(fixture.Prepare("receipt-canceled", ChangedPose()));
			context.Expect(fixture.queue.CancelTransaction("receipt-canceled"), "Cancel must mark the pending real transaction.");
			context.Expect(!fixture.queue.TakeCompletedAutomationTransactionReceipt("receipt-canceled"), "Cancellation ingress is not a protected completion.");
			fixture.queue.Apply(fixture.objects, kBoundary);
			const auto receipt = fixture.queue.TakeCompletedAutomationTransactionReceipt("receipt-canceled");
			context.Expect(receipt.has_value() && receipt->status == TransactionStatus::Canceled && receipt->appliedWriteCount == 0,
				"The existing flush must publish a canceled terminal decision without live writes.");
			ExpectPose(context, fixture.Capture().objects.at(fixture.targetId).worldPose, before.objects.at(fixture.targetId).worldPose, "Canceled work must leave canonical pose unchanged.");
		});

		registry.Add("Renderer.Efficiency.Editor.Mutation.ReceiptClearAndUndoChannelRemainIndependent", [](TestContext& context) {
			MutationFixture fixture;
			fixture.queue.EnqueueTransaction(fixture.Prepare("receipt-clear", ChangedPose()));
			TransactionalMutationBuilder editor("receipt-editor", TransactionOrigin::Editor, "UndoFixture");
			editor.SetUndoGroup("Owned editor history").SetName(fixture.targetId, "Editor name");
			fixture.queue.EnqueueTransaction(std::move(editor).Build());
			fixture.queue.Apply(fixture.objects, kBoundary);
			const auto automation = fixture.queue.TakeCompletedAutomationTransactionReceipt("receipt-clear");
			context.Expect(automation.has_value(), "Automation must retain its own receipt.");
			const auto undo = fixture.queue.DrainCompletedEditorTransactions();
			context.Expect(undo.size() == 1 && undo.front().transaction.id == "receipt-editor"
				&& !undo.front().result.inverseDeltas.empty(), "Automation consumption must leave full editor undo history intact.");
			fixture.queue.EnqueueTransaction(fixture.Prepare("receipt-reset", ChangedPose()));
			fixture.queue.Apply(fixture.objects, kBoundary);
			fixture.queue.Clear();
			context.Expect(!fixture.queue.TakeCompletedAutomationTransactionReceipt("receipt-reset"), "Clear must discard retained old-session receipts.");
		});

		registry.Add("Renderer.Efficiency.Editor.Mutation.WorkerPreparationWaitsForExistingBoundary", [](TestContext& context) {
			MutationFixture fixture;
			const auto before = fixture.Capture();
			const auto desired = ChangedPose();
			auto detached = fixture.Prepare("renderer-fixture-worker", before.objects.at(fixture.targetId).worldPose);
			auto worker = std::async(std::launch::async, [transaction = std::move(detached), desired]() mutable {
				// No ObjectManager, component pointer, host, or queue is captured.
				transaction.writeSet.operations.front().worldPose = desired;
				return transaction;
			});
			fixture.queue.EnqueueTransaction(worker.get());
			const auto whileQueued = fixture.Capture();
			ExpectPose(context, whileQueued.objects.at(fixture.targetId).worldPose, before.objects.at(fixture.targetId).worldPose,
				"Prepared and queued worker data must remain invisible to the current scene capture.");
			context.ExpectEqual(std::size_t{1}, fixture.queue.PendingTransactions().size(), "Preparation must leave a pending typed transaction.");
			const auto committed = fixture.queue.Apply(fixture.objects, kBoundary);
			context.ExpectEqual(std::string(kBoundary), committed.commitName, "Visibility must use the existing editor automation flush.");
			context.ExpectEqual(std::size_t{1}, committed.transactionResults.size(), "One prepared transaction must produce one protected receipt.");
			context.Expect(committed.transactionResults.front().status == TransactionStatus::Committed, "The valid canonical worker edit must commit atomically.");
			ExpectPose(context, fixture.Capture().objects.at(fixture.targetId).worldPose, desired,
				"The first capture after the declared flush must observe the edit.");
			ExpectPose(context, before.objects.at(fixture.targetId).worldPose, whileQueued.objects.at(fixture.targetId).worldPose,
				"Previously copied immutable scene captures must retain their original values after commit.");
		});

		registry.Add("Renderer.Efficiency.Editor.Mutation.ConflictingBatchPreservesTransformAndMetadata", [](TestContext& context) {
			MutationFixture fixture;
			const auto before = fixture.Capture();
			TransactionalMutationBuilder builder("renderer-fixture-conflict", TransactionOrigin::Automation, "RendererEfficiencyFixture");
			// Ordered name updates are legal. A write after deletion is a real
			// structural conflict and must reject even the earlier valid pose/name.
			builder.SetWorldTransform(fixture.objects, fixture.targetId, ChangedPose())
				.SetName(fixture.targetId, "Would Apply").DeleteObject(fixture.targetId)
				.SetName(fixture.targetId, "Conflicting Write");
			fixture.queue.EnqueueTransaction(std::move(builder).Build());
			ExpectRejectedWithoutWrites(context, fixture.queue.Apply(fixture.objects, kBoundary), fixture, before);
		});

		registry.Add("Renderer.Efficiency.Editor.Mutation.CommitFailureRollsBackAppliedTransform", [](TestContext& context) {
			MutationFixture fixture;
			const auto before = fixture.Capture();
			TransactionalMutationBuilder builder("renderer-fixture-rollback", TransactionOrigin::Automation, "RendererEfficiencyFixture");
			builder.SetWorldTransform(fixture.objects, fixture.targetId, ChangedPose())
				.SetName(fixture.targetId, "Would Apply").InjectCommitFailureForTesting(1);
			fixture.queue.EnqueueTransaction(std::move(builder).Build());
			ExpectRejectedWithoutWrites(context, fixture.queue.Apply(fixture.objects, kBoundary), fixture, before);
		});

		registry.Add("Renderer.Efficiency.Editor.Mutation.OwnedSessionAdmitsTypedFixtureStepWithoutFlushing", [](TestContext& context) {
			MutationFixture fixture;
			std::string error;
			context.Expect(fixture.Start(error), "The matching owned validation project must start.");
			const auto before = fixture.Capture();
			const auto operationId = fixture.host.Status().at("operationId").get<std::string>();
			const auto operationUuid = ParseUUID(operationId);
			context.Expect(operationId.size() == 32 && operationUuid && *operationUuid != EMPTY_UUID
				&& ToHexString(*operationUuid) == operationId, "Actual owned Start/status must issue the canonical nonzero full UUID required by artifact export and independent comparison.");
			context.Expect(fixture.host.QueueFixtureTransaction(fixture.root, kProjectGeneration, operationId,
				fixture.Prepare("renderer-owned-fixture", ChangedPose()), fixture.queue, error),
				"An active owned renderer fixture must admit its native typed transaction; the foundation unavailable stub is deliberately red.");
			ExpectPose(context, fixture.Capture().objects.at(fixture.targetId).worldPose, before.objects.at(fixture.targetId).worldPose,
				"Owned session admission must queue work without mutating live storage or adding a flush.");
			context.ExpectEqual(std::size_t{1}, fixture.queue.PendingTransactions().size(), "Owned admission must retain the complete typed batch.");
			const auto committed = fixture.queue.Apply(fixture.objects, kBoundary);
			context.Expect(committed.transactionResults.size() == 1 && committed.transactionResults.front().status == TransactionStatus::Committed,
				"Only the existing protected boundary may commit the owned fixture edit.");
			ExpectPose(context, fixture.Capture().objects.at(fixture.targetId).worldPose, ChangedPose(), "The first protected capture after admission and flush must see the fixture step.");
		});

		registry.Add("Renderer.Efficiency.Editor.Mutation.InvalidOwnershipQueuesNoPartialWork", [](TestContext& context) {
			MutationFixture fixture;
			std::string error;
			context.Expect(fixture.Start(error), "The ownership rejection fixture must start.");
			const auto operationId = fixture.host.Status().at("operationId").get<std::string>();
			const auto before = fixture.Capture();
			const auto reject = [&](std::uint64_t generation, std::string_view operation, const char* id) {
				error.clear();
				context.Expect(!fixture.host.QueueFixtureTransaction(fixture.root, generation, operation,
					fixture.Prepare(id, ChangedPose()), fixture.queue, error), "Invalid owned ingress must be rejected before queue admission.");
				context.Expect(!error.empty() && fixture.queue.Empty(), "Rejected ownership must have a diagnostic and enqueue no operations.");
				ExpectPose(context, fixture.Capture().objects.at(fixture.targetId).worldPose, before.objects.at(fixture.targetId).worldPose,
					"Ownership rejection must preserve the exact live transform.");
			};
			reject(kProjectGeneration, "other-operation", "renderer-wrong-operation");
			reject(kProjectGeneration + 1, operationId, "renderer-stale-project");
			fixture.WriteMarker("changed-fixture-hash");
			reject(kProjectGeneration, operationId, "renderer-modified-marker");
			fixture.WriteMarker(kFixtureHash);
			context.Expect(fixture.host.Stop(operationId, error), "The owning caller must stop the session.");
			reject(kProjectGeneration, operationId, "renderer-stopped-session");
		});
	}
}
