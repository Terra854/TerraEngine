#pragma once

#include "Renderer/RenderTypes.h"
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyEvidenceExport.h"
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyInstallIdentity.h"
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyScenePublication.h"
#include "Scene/TransactionalMutation.h"
#include "Animation/Runtime/AnimationCommandQueue.h"
#include "Animation/Runtime/AnimatorPlayback.h"
#include <future>
#include <functional>
#include <map>
#include <set>
#include <filesystem>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace TerraEngine::Scene { struct Transaction; class SceneMutationQueue; }
namespace TerraEngine::Animation { struct AnimationSnapshot; struct AnimationPresentationFrame; }
class RenderSystem;
class ObjectManager;

namespace TerraEngine::Editor::Automation
{
	// P5 category edits are decoded on workers from immutable authored values.
	bool PrepareRendererEfficiencyStaticShadowTransaction(const nlohmann::json& requests,
		const std::map<std::string, UUID>& bindings, Scene::Transaction& output, std::string& error);
	bool ParseRendererEfficiencyStaticShadowControls(const nlohmann::json& step,
		Renderer::RendererEfficiencyFixtureIntent& output, std::string& error);
	std::int64_t RendererEfficiencyStaticSampleTick(std::uint64_t stepFrame);
	// A completed owner identifies the allocation; only this fresh inventory can
	// prove it still has a recorded/accepted use that has not retired.
	bool RendererEfficiencyStaticOwnerHasOutstandingUse(const Renderer::RendererEfficiencyStaticShadowExecution& current,
		const Renderer::RendererEfficiencyStaticShadowAllocation& completedOwner);
	// P4 authored values become detached requests only. Exact protected live
	// observations and genuine queue/publication receipts are added by the host.
	bool PrepareRendererEfficiencyShadowBatchTransaction(const nlohmann::json& requests,
		const std::map<std::string, UUID>& bindings, Scene::Transaction& output, std::string& error);
	bool ParseRendererEfficiencyShadowBatchControls(const nlohmann::json& step,
		Renderer::RendererEfficiencyFixtureIntent& output, std::string& error, bool dedicated = true);
	bool ParseRendererEfficiencyShadowBatchCameras(const nlohmann::json& script, const nlohmann::json& step,
		std::span<const Renderer::RendererEfficiencyFixtureCamera> primary,
		std::vector<Renderer::RendererEfficiencyFixtureCamera>& output, std::string& error);
	Renderer::RendererEfficiencyFixtureIntent RendererEfficiencyShadowBatchMeasurementIntent(
		const Renderer::RendererEfficiencyFixtureIntent& completedTransition);
	// Recipes become detached typed writes; protected callers add current object
	// observations before queue admission. Failure never modifies caller output.
	bool PrepareRendererEfficiencyLightingTransaction(const nlohmann::json& requests,
		Scene::Transaction& output, std::string& error);
	bool ParseRendererEfficiencyLightingControls(const nlohmann::json& payload, bool baseline,
		Renderer::RendererEfficiencyFixtureLightingControls& output, std::string& error);
	// P3 ingress validates owned sealed bytes before retaining immutable typed
	// inputs. Rejection leaves outputs unchanged; no live scene/depth is written.
	bool ParseRendererEfficiencyDepthControls(const std::filesystem::path& root,
		const nlohmann::json& script, const nlohmann::json& step,
		Renderer::RendererEfficiencyFixtureDepthControls& output, std::string& error);
	bool ParseRendererEfficiencyDepthCameras(const nlohmann::json& script, const nlohmann::json& step,
		std::span<const Renderer::RendererEfficiencyFixtureCamera> primary,
		std::vector<Renderer::RendererEfficiencyFixtureCamera>& output, std::string& error);
	bool RendererEfficiencyDepthObservationUnavailable(const Renderer::RendererEfficiencyEvidence& row,
		const Renderer::RendererEfficiencyFixtureIntent& applied, const Renderer::RendererEfficiencyOptionsSnapshot& options,
		std::uint64_t lastAcceptedFrame);
	bool SelectRendererEfficiencyDepthBundle(const nlohmann::json& requiredBundle,
		const Renderer::RendererEfficiencyFixtureIntent& applied,
		const Renderer::RendererEfficiencyOptionsSnapshot& options,
		std::span<const Renderer::RendererEfficiencyEvidence> rows,
		std::vector<Renderer::RendererEfficiencyEvidence>& output, std::string& error);
	bool SelectRendererEfficiencyLightingBundle(const nlohmann::json& requiredBundle,
		const Renderer::RendererEfficiencyFixtureIntent& applied,
		std::span<const Renderer::RendererEfficiencyEvidence> rows,
		std::vector<Renderer::RendererEfficiencyEvidence>& output, std::string& error);
	bool RendererEfficiencyLightingBaselineReady(std::uint64_t completed,
		const std::array<std::set<std::pair<std::uint32_t, std::uint32_t>>, 2>& observedSlots);
	// Pause is a state transition, not an idempotent freeze operation. A stopped
	// or paused animator can seek directly without submitting an invalid Pause.
	std::vector<Animation::AnimationCommandKind> RendererEfficiencySeekCommandKinds(Animation::AnimatorPlaybackRunState state);
	// A protected importer bridge supplies actual named publication identities,
	// never template child GUIDs or queued HTTP/import responses.
	struct RendererEfficiencyPublishedBinding
	{
		std::string bindingKey;
		UUID assetId{};
		UUID packageGuid{};
		std::string kind;
		std::filesystem::path sourcePath, cookedPath;
		std::uint64_t resourceGeneration = 0;
		std::uint32_t cookedLods = 0, cookedMeshlets = 0;
	};
	struct RendererEfficiencySourcePatch
	{
		std::filesystem::path sourcePath;
		std::string baselineBytes, replacementBytes, priorHash, replacementHash, baselineHash;
	};
	// Preparation is detached file I/O; application is admitted only after the
	// protected host verifies its current operation and unchanged ownership marker.
	bool PrepareRendererEfficiencySourcePatch(const std::filesystem::path& root,
		const std::filesystem::path& source, const std::filesystem::path& replacement,
		std::string_view retainedBaseline, std::string_view expectedPriorHash,
		RendererEfficiencySourcePatch& output, std::string& error);
	bool ApplyRendererEfficiencySourcePatch(const std::filesystem::path& root,
		const RendererEfficiencySourcePatch& patch, std::string& error);
	// A page edit compares against its actually imported dense baseline, while
	// cleanup independently retains the initial direct-mesh source bytes.
	bool PrepareRendererEfficiencyPageVariantPatch(const std::filesystem::path& root,
		const std::filesystem::path& source, const std::filesystem::path& replacement,
		std::string_view originalBytes, std::string_view expectedPriorHash,
		std::string_view denseBaselineBytes, RendererEfficiencySourcePatch& output, std::string& error);
	bool RendererEfficiencyDensePageBaselineMatches(std::span<const Renderer::RendererEfficiencyEvidence> rows,
		const Renderer::RendererEfficiencyFixtureIntent& applied, const UUID& target, const UUID& mesh,
		std::uint64_t publication, std::string& error);
	bool RendererEfficiencyCompletedControlMatches(const Renderer::RendererEfficiencyEvidence& row,
		const Renderer::RendererEfficiencyFixtureIntent& applied);
	// Detached baseline results are admitted only while their exact protected
	// control owner and options epoch remain current after worker completion.
	bool RendererEfficiencyLightingExportOwnerMatches(const Renderer::RendererEfficiencyFixtureIntent& captured,
		std::uint64_t optionsEpoch, const Renderer::RendererEfficiencyOptions& expectedOptions,
		const Renderer::RendererEfficiencyFixtureIntent& current,
		const Renderer::RendererEfficiencyOptionsSnapshot& options);
	// Select the original completed control-change cohort before its correlation
	// is replaced by measurement admission. Output stays unchanged while pending.
	bool SelectRendererEfficiencyControlTransition(const nlohmann::json& requiredBundle,
		const Renderer::RendererEfficiencyFixtureIntent& applied,
		const Renderer::RendererEfficiencyOptionsSnapshot& options,
		std::span<const Renderer::RendererEfficiencyEvidence> rows,
		std::vector<Renderer::RendererEfficiencyEvidence>& output, std::string& error);
	// Mutation and fixture-control counters are independent domains. Identical
	// post-admission tokens must obtain a fresh actual native sample owner.
	bool RendererEfficiencyTransitionNeedsSamplingRearm(const Renderer::RendererEfficiencyFixtureIntent& consumed,
		const Renderer::RendererEfficiencyFixtureCorrelation& measurement);
	bool RendererEfficiencyDensePageBaselineRetryable(std::span<const Renderer::RendererEfficiencyEvidence> rows,
		const Renderer::RendererEfficiencyFixtureIntent& applied, const UUID& target, const UUID& mesh, std::uint64_t publication);
	// Detached IO helper for copied authored shader receipts. It rejects paths
	// outside the owned project and leaves the hash unchanged on failure.
	bool HashRendererEfficiencyOwnedInput(const std::filesystem::path& root,
		const std::filesystem::path& relative, std::string& hash, std::string& error);
	// Pure ingress validation assigns output only after the entire request succeeds.
	bool ParseRendererEfficiencyOptions(const nlohmann::json& request,
		const Renderer::RendererEfficiencyOptions& base, Renderer::RendererEfficiencyOptions& output,
		std::string& error);
	bool ValidateRendererEfficiencyOwnedProject(const std::filesystem::path& currentRoot,
		std::string_view fixtureId, std::string_view fixtureHash, std::string& error);
	nlohmann::json SerializeRendererEfficiencyOptions(const Renderer::RendererEfficiencyOptions& options);
	nlohmann::json SerializeRendererEfficiencyEvidence(const Renderer::RendererEfficiencyEvidence& evidence);
	nlohmann::json SerializeRendererEfficiencyToken(const Renderer::RendererEfficiencyOptionsToken& token);
	bool ParseRendererEfficiencyToken(const nlohmann::json& value, Renderer::RendererEfficiencyOptionsToken& token, std::string& error);

	// The service owns this host on its protected editor boundary. Status polling
	// only copies state; it cannot advance fixtures or manufacture GPU completion.
	// Complete the low-frequency native residency receipt without discarding
	// either original completed cohort. Rejection leaves the caller unchanged.
	bool CompleteRendererEfficiencyResidencyPublicationReceipt(const nlohmann::json& scalarReceipt,
		const nlohmann::json& baselineObservation, const nlohmann::json& reuploadObservation,
		nlohmann::json& output, std::string& error);
	// Admit the next real residency token and one invalidation on the same
	// protected boundary. Callbacks use the existing native queues; no flush is added.
	bool AdmitRendererEfficiencyResidencyReupload(Renderer::RendererEfficiencyFixtureIntent& activeIntent,
		std::uint64_t baselineStepFrame, std::uint64_t& stepFrame, bool& intentQueued,
		const std::function<Renderer::RendererEfficiencyFixtureIntentReceipt(const Renderer::RendererEfficiencyFixtureIntent&)>& queueIntent,
		const std::function<bool(std::string&)>& invalidate, std::string& error);
	class EditorRendererEfficiencyValidation
	{
	public:
		bool Start(const std::filesystem::path& root, std::uint64_t projectGeneration,
			const nlohmann::json& request, const Renderer::RendererEfficiencyOptionsSnapshot& options,
			std::string& error);
		bool Stop(std::string_view operationId, std::string& error);
		void CancelPendingFixtureWorkAtProtectedFrame(Scene::SceneMutationQueue& queue);
		// Validation workers prepare a typed transaction; this ingress admits it
		// only to the existing protected queue, never to live component storage.
		bool QueueFixtureTransaction(const std::filesystem::path& currentRoot,
			std::uint64_t currentProjectGeneration, std::string_view operationId,
			Scene::Transaction transaction, Scene::SceneMutationQueue& queue, std::string& error);
		// Pending ingress is never a protected completion; take only our exact ID.
		void PumpFixtureReceiptsAtProtectedFrame(const std::filesystem::path& currentRoot,
			std::uint64_t currentProjectGeneration, ObjectManager& objects, Scene::SceneMutationQueue& queue);
		bool AdmitPublishedFixtureBindings(const std::filesystem::path& root, std::uint64_t generation,
			std::string_view operation, const std::vector<RendererEfficiencyPublishedBinding>& bindings,
			ObjectManager& objects, Scene::SceneMutationQueue& queue, std::string& error);
		void PumpFixtureAtProtectedFrame(const std::filesystem::path& root, std::uint64_t generation,
			ObjectManager& objects, Scene::SceneMutationQueue& queue, RenderSystem& renderer);
		void ObserveAtProtectedFrame(const std::filesystem::path& currentRoot, std::uint64_t projectGeneration,
			const Renderer::RendererEfficiencyEvidence& evidence);
		nlohmann::json Status() const;
		const Renderer::RendererEfficiencyOptionsToken& PriorToken() const { return priorToken; }
		bool Active() const { return active; }
		const nlohmann::json& InstalledSteps() const { return installedSteps; }
		const std::vector<Renderer::RendererEfficiencyFixtureCamera>& FixtureCameras() const { return fixtureCameras; }
		void ReportBootstrapUnavailable(std::string reason) { if(state == "AwaitingAssets") unavailableReason = std::move(reason); }
		void ReportNativeUnavailable(std::string reason) { if(active) { unavailableReason = std::move(reason); nativeProviderFailure = true; } }
		// Resource ingress runs at the existing protected boundary. A terminal
		// importer failure ends measurement, but retains the owned operation and
		// prior options token so Stop can still restore disposable source state.
		void ReportNativeResourcePending(std::string reason) { if(active) unavailableReason = std::move(reason); }
		void FailNativeResourcePreparation(std::string reason) { if(active) { unavailableReason = std::move(reason); nativeProviderFailure = true; state = "Failed"; active = false; } }
		// Native bridges inspect this copied sealed step, then report an actual
		// publication generation. A queued control/import is never that receipt.
		nlohmann::json PendingNativeStep() const;
		bool AdmitNativeStepPublication(const std::filesystem::path& root, std::uint64_t generation,
			std::string_view operation, std::string_view stepId, std::uint64_t actualPublication, std::string& error);
		// Low-frequency automation receipts are detached JSON export metadata;
		// native execution, transaction ingress and frame correlation remain typed.
		bool RecordFixtureReceipt(const std::filesystem::path& root, std::uint64_t generation,
			std::string_view operation, nlohmann::json receipt, std::string& error);
		bool AdmitCompletedDeviceRecreation(const std::filesystem::path& root, std::uint64_t generation,
			std::string_view operation, std::string_view step, std::uint64_t priorOptionsEpoch,
			const Renderer::RendererEfficiencyOptionsSnapshot& freshOptions, std::uint64_t completedBackendGeneration, std::string& error);
		nlohmann::json PendingDenseResourceBaseline() const;
		bool BeginResourceBaselineReset(const std::filesystem::path& root, std::uint64_t generation, std::string& error, bool densePage = false);
		bool RetainDenseResourceBaselineOwner(const std::filesystem::path& root, std::uint64_t generation,
			const UUID& meshId, std::uint64_t publication, std::string& error);
		bool AdmitResourceBaselinePublication(const std::filesystem::path& root, std::uint64_t generation, std::uint64_t publication, std::string& error);
		bool ValidateOwnedAdmission(const std::filesystem::path& root, std::uint64_t generation,
			std::string_view operation, std::string& error) const { return HasOperation(operation) && ValidateSession(root, generation, error); }
		bool ValidateOwnedRestoration(const std::filesystem::path& root, std::uint64_t generation,
			std::string_view operation, std::string& error) const { return HasOperation(operation) && ValidateSession(root, generation, error, false); }
		bool HasOperation(std::string_view id) const { return !operationId.empty() && id == operationId; }
	private:
		bool ValidateSession(const std::filesystem::path& root, std::uint64_t generation, std::string& error, bool requireActive = true) const;
		std::filesystem::path projectRoot;
		std::uint64_t projectGeneration = 0;
		std::string operationId, fixtureId, fixtureHash, stage, runId, ownerId;
		std::string runMode = "Candidate";
		std::string markerIdentity, pendingTransactionId;
		nlohmann::json installedSteps;
		nlohmann::json fixtureReceipts = nlohmann::json::array(), rejectedReceipt;
		std::vector<Renderer::RendererEfficiencyFixtureCamera> fixtureCameras;
		// Primary identity stays fixed while step-specific immutable cameras carry
		// odd/small native outputs. Prepared recipes never access worker live state.
		std::vector<std::vector<Renderer::RendererEfficiencyFixtureCamera>> depthStepCameras;
		std::vector<Renderer::RendererEfficiencyFixtureDepthControls> depthStepControls;
		nlohmann::json fixtureRoleTargets;
		std::uint64_t mutationGeneration = 0;
		std::map<std::string, UUID> publishedBindings;
		std::size_t stepIndex = 0;
		std::uint64_t stepFrame = 0, warmupCompleted = 0, measuredCompleted = 0, batchCursor = 0, lastAcceptedFrame = 0;
		bool bindingsAwaitingReceipt = false, fixtureIntentQueued = false;
		bool baselineAwaitingReceipt = false, stepMutationReady = false;
		bool lightingBaselineObservation = false;
		std::uint64_t lightingBaselineCompleted = 0;
		bool lightingTransitionPending = false;
		// Shared workloads first establish lighting owners, then keep their original
		// shadow/fault action before admitting the optional lighting membership edit.
		bool sharedLightingBaselineDone = false, sharedLightingEditsDone = false;
		nlohmann::json lightingBaselineReceipt = nullptr;
		std::array<std::set<std::pair<std::uint32_t, std::uint32_t>>, 2> lightingObservedSlots;
		// Fault samples establish an owner, observe one real failure, then resume
		// ordinary samples. Neither preliminary token counts as a measured frame.
		std::uint8_t faultPhase = 0;
		TerraEngine::UUID residencyMeshId = TerraEngine::EMPTY_UUID;
		std::uint64_t residencyPriorAllocation = 0, residencyPriorPublication = 0, residencyPriorFrame = 0, residencyLifetime = 0;
		bool resourceBindingAwaitingReceipt = false, resourceBaselineBinding = false;
		// Dense cooking and its completed baseline are unmeasured preparation.
		// They retain actual owner identity; no queued import can release this gate.
		bool denseBaselineBinding = false, denseResourcePrepared = false, denseBaselineObserved = false, denseAwaitingObservation = false;
		UUID denseBaselineMeshId{};
		std::uint64_t denseBaselineMeshPublication = 0;
		std::uint32_t denseBaselineAttempts = 0;
		Scene::Transaction baselineInputs;
		RendererEfficiencyScenePublication pendingScenePublication;
		std::future<Scene::Transaction> preparedTransaction;
		std::string preparedAction;
		std::vector<Animation::AnimationCommand> pendingAnimationCommands;
		std::map<std::uint64_t, UUID> pendingAnimationObjects;
		std::map<UUID, std::shared_ptr<const Animation::AnimationSnapshot>> animationBefore;
		// Pin one immutable authority/plan envelope before commands. GPU-only rows
		// deliberately have no CPU snapshot; their baseline lives in this owner.
		std::shared_ptr<const Animation::AnimationPresentationFrame> animationGpuBefore;
		std::uint64_t nextAnimationSequence = 1;
		bool animationBaseline = false;
		// Only an accepted completed export advances stepFrame. A native seek
		// publication is retained for that exact sample while GPU/export work waits.
		bool animationPerSample = false, staticResourceApplied = false;
		std::optional<std::uint64_t> animationReadyStepFrame;
		std::uint64_t staticBudgetRequestedGeneration = 0, staticLowerBudgetRequestedGeneration = 0;
		std::optional<Renderer::RendererEfficiencyStaticShadowAllocation> pendingStaticLifetimeOwner, completedStaticLifetimeOwner;
		nlohmann::json pendingStaticBudgetReceipt = nullptr;
		bool expectingRejectedTransaction = false, rejectedTargetActive = false, rejectedTargetStatic = false;
		Scene::TransactionObjectObservation rejectedTargetObservation{};
		Scene::WorldPose rejectedTargetPose{};
		std::uint64_t rejectedPriorSpatialGeneration = 0;
		RendererEfficiencyRollbackPublication rejectedPriorPublication;
		std::uint64_t rejectedTargetSpatialRevision = 0;
		// One immutable completion bundle is exported off the protected hot path.
		// Its token stays frozen until the owned result has been accepted.
		std::future<std::pair<RendererEfficiencyEvidenceExportResult, std::string>> exportJob;
		struct PreparedEvidenceIdentity
		{
			RendererEfficiencyInstallIdentity install;
			std::string cameraHash, geometryMaterialRevisionHash;
		};
		std::future<std::pair<PreparedEvidenceIdentity, std::string>> identityJob;
		std::optional<PreparedEvidenceIdentity> preparedIdentity;
		std::string identityError, adapterName, driverVersion;
		std::uint32_t colorWidth = 0, colorHeight = 0;
		std::uint64_t requestedRuns = 3;
		std::future<std::pair<std::filesystem::path, std::string>> documentJob;
		std::string completedEvidencePath;
		std::uint64_t exportedFrameCount = 0, exportedArtifactCount = 0;
		std::uint64_t pendingExportFrame = 0;
		enum class EvidenceExportPurpose { Sample, ControlTransition, ResidencyBaseline, ResidencyReupload, LightingBaseline };
		EvidenceExportPurpose pendingExportPurpose = EvidenceExportPurpose::Sample;
		nlohmann::json pendingTransitionReceipt = nullptr;
		std::uint64_t pendingTransitionOptionsEpoch = 0;
		// Both cohorts retain their original correlation and immutable CPU owners
		// until detached export settles. Neither enters the measurement windows.
		nlohmann::json residencyBaselineObservation = nullptr, pendingResidencyReceipt = nullptr;
		// GPU batches may arrive one view at a time; keep copied rows by their
		// actual native frame/submission until the exact required bundle exists.
		std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<Renderer::RendererEfficiencyEvidence>> pendingEvidenceBundles;
		nlohmann::json exportedFrames = nlohmann::json::array(), exportedArtifacts = nlohmann::json::array();
		Renderer::RendererEfficiencyFixtureIntent activeFixtureIntent{};
		std::string state = "Stopped", unavailableReason;
		bool nativeProviderFailure = false;
		std::uint64_t seed = 0, runIndex = 0, warmupFrames = 60, measuredFrames = 300;
		std::uint64_t submittedFrames = 0, completedFrames = 0, lastSubmit = 0, lastComplete = 0;
		bool active = false;
		Renderer::RendererEfficiencyOptionsToken priorToken{};
		Renderer::RendererEfficiencyOptionsSnapshot observedOptions{};
	};
}
