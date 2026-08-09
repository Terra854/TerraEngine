// AnimationAutomationRuntimeTests.cpp - typed runtime automation regressions.

#include "TestHarness.h"

#include "../../Engine/Animation/AnimationAuthorityTestHelpers.h"

#include "Editor/TerraEngine-Editor/Automation/AnimationAutomationJobs.h"
#include "Editor/TerraEngine-Editor/Automation/AnimationRuntimeOperations.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Animation/Diagnostics/AnimationDiagnostics.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace Editor::Automation;
		using namespace Animation;
		using TerraEngine::Tests::PinnedAnimationAuthorityRow;

		AnimatorInstanceHandle CreateAutomationAnimator(std::uint8_t seed)
		{
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId[0U] = seed;
			descriptor.componentGeneration = 1U;
			return system.CreateAnimatorInstance(descriptor);
		}

		std::shared_ptr<const SkeletonAsset> CreateAutomationSkeleton()
		{
			auto skeleton = std::make_shared<SkeletonAsset>();
			skeleton->assetId.value[0U] = 91U;
			skeleton->compatibilityFingerprint = 0xA11CEULL;
			skeleton->rootJointIndex = 0U;
			SkeletonJoint root;
			root.stableId = 101U;
			root.parentIndex = -1;
			root.normalizedSourcePath = "/root";
			skeleton->joints.push_back(root);
			return skeleton;
		}

		bool ExecuteAutomationAnimationFrame(AnimationSystem& system, const char* frameId)
		{
			// Tests use the production scheduler boundary so the retained pose has
			// exactly the same ownership and publication rules as an editor frame.
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> workloads =
				system.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan plan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(workloads), frameId);
			return plan.valid && plan.completed;
		}

		nlohmann::json RuntimeArguments(
			AnimatorInstanceHandle handle,
			std::string idempotencyKey,
			nlohmann::json commands)
		{
			return {
				{"target", {{"animator", {
					{"slotIndex", handle.slotIndex},
					{"generation", handle.generation}}}}},
				{"commands", std::move(commands)},
				{"idempotencyKey", std::move(idempotencyKey)},
				{"correlationId", "runtime-automation-test"},
				{"expectedGeneration", 1U}
			};
		}

		nlohmann::json RuntimeTarget(AnimatorInstanceHandle handle)
		{
			return {{"animator", {{"slotIndex", handle.slotIndex}, {"generation", handle.generation}}}};
		}

		nlohmann::json ProceduralClearArguments(AnimatorInstanceHandle handle, std::string key)
		{
			return {{"target", RuntimeTarget(handle)}, {"action", "clear"}, {"goalId", 700U},
				{"idempotencyKey", std::move(key)}, {"correlationId", "procedural-runtime"},
				{"expectedGeneration", 1U}};
		}

		nlohmann::json ProceduralSetArguments(AnimatorInstanceHandle handle, std::string key)
		{
			return {{"target", RuntimeTarget(handle)}, {"action", "set"}, {"goalId", 701U},
				{"goalKind", "position"}, {"coordinateSpace", "world"}, {"targetJointStableId", 101U},
				{"goalTransform", {{"translation", nlohmann::json::array({0.0, 0.0, 0.0})},
					{"rotation", nlohmann::json::array({0.0, 0.0, 0.0, 1.0})},
					{"scale", nlohmann::json::array({1.0, 1.0, 1.0})}}},
				{"poleVector", nlohmann::json::array({0.0, 1.0, 0.0})}, {"priority", 0},
				{"positionWeight", 1.0}, {"rotationWeight", 0.0}, {"poleWeight", 0.0},
				{"maximumStretchScale", 1.0}, {"expectedPoseGeneration", 1U},
				{"expectedPublicationGeneration", 1U}, {"idempotencyKey", std::move(key)},
				{"correlationId", "procedural-runtime"}, {"expectedGeneration", 1U}};
		}

		nlohmann::json RetargetArguments(AnimatorInstanceHandle handle, std::string key)
		{
			return {{"target", RuntimeTarget(handle)}, {"action", "activate_profile"},
				{"profileAssetId", ToHexString(GenerateUUID())}, {"profileGeneration", 1U},
				{"idempotencyKey", std::move(key)}, {"correlationId", "retarget-runtime"},
				{"expectedGeneration", 1U}};
		}

		class RecordingEndpointDirectory final
		{
		public:
			RecordingEndpointDirectory()
				: path_(std::filesystem::temp_directory_path()
					/ ("TerraEngine-RecordingEndpoint-" + ToHexString(GenerateUUID())))
			{
				std::filesystem::create_directories(path_);
			}

			~RecordingEndpointDirectory()
			{
				// This fixture creates one unique child below the system temp directory,
				// so cleanup cannot remove any project asset or shared test artifact.
				std::error_code ignored;
				std::filesystem::remove_all(path_, ignored);
			}

			const std::filesystem::path& Path() const noexcept { return path_; }

		private:
			std::filesystem::path path_;
		};

		nlohmann::json RecordingStartArguments(AnimatorInstanceHandle handle)
		{
			const std::string assetId = ToHexString(GenerateUUID());
			return {
				{"target", {{"animator", {{"slotIndex", handle.slotIndex}, {"generation", handle.generation}}}}},
				{"assetId", assetId}, {"skeletonAssetId", ToHexString(GenerateUUID())},
				{"sourceAnimatorId", ToHexString(GenerateUUID())}, {"compatibilityFingerprint", 0xA11CEULL},
				{"publicationGeneration", 1U}, {"jointStableIds", nlohmann::json::array({101U})},
				{"destinationPath", "Captured/Endpoint.teanim"}, {"firstTick", 0U}, {"lastTick", 2U},
				{"sampleRate", 60U}, {"clock", "fixed_simulation"},
				{"channels", nlohmann::json::array({{{"stableChannelId", 1U}, {"stableTargetId", 101U},
					{"name", "Hip Translation"}, {"kind", "bone_translation"}, {"interpolation", "linear"}}})},
				{"sourceSceneIdentity", "Tests/RecordingEndpoint.scene"},
				{"sourceSessionIdentity", "recording-endpoint-session"}, {"segmentDurationTicks", 2U},
				{"idempotencyKey", "recording-endpoint"}, {"correlationId", "recording-endpoint"},
				{"expectedGeneration", 1U}
			};
		}
	}

	void RegisterAnimationAutomationRuntimeTests(TestRegistry& registry)
	{
		registry.Add("Editor.Animation.Automation.Runtime.RendererUnavailableCapture", [](TestContext& context) {
			AnimationAutomationTransactions transactions;
			const AutomationDispatchResult unavailable = ExecuteAnimationRuntimeOperation(
				"animation.camera.capture",
				{{"source", "scene"}, {"rendererAvailable", false}},
				transactions,
				1U);
			context.Expect(!unavailable.ok && unavailable.error == ErrorCategory::Unavailable,
				"Capture must fail explicitly when no renderer exists.");

			const AutomationDispatchResult diagnostics = ExecuteAnimationRuntimeOperation(
				"animation.runtime.diagnostics",
				{{"assetId", "a"}, {"rendererAvailable", false}},
				transactions,
				1U);
			context.Expect(diagnostics.ok && !diagnostics.data.value("rendererAvailable", true),
				"Diagnostics remain available as a renderer-independent snapshot.");
		});

		registry.Add("Editor.Animation.Automation.Runtime.CollapsedProfilerAndResidencyStayReadOnly", [](TestContext& context) {
			AnimationAutomationTransactions transactions;
			const AutomationDispatchResult profiler = ExecuteAnimationRuntimeOperation(
				"animation.runtime.profiler", { {"includeDetails", false} }, transactions, 1U);
			const AutomationDispatchResult residency = ExecuteAnimationRuntimeOperation(
				"animation.runtime.residency", { {"maximumItems", 1U} }, transactions, 1U);
			context.Expect(
				profiler.ok && residency.ok
					&& !profiler.data.value("collectionPromotedByRequest", true),
				"Collapsed profiler and residency reads must use copied snapshots and must not enable detailed collection.");
		});

		registry.Add("Editor.Animation.Automation.Runtime.DetailedRequestRetainsUntilProtectedPublication", [](TestContext& context) {
			ClearPublishedAnimationDetailedDiagnostics();
			AnimationAutomationTransactions transactions;
			AnimationAutomationDetailedDiagnosticsLease automationLease;
			const AutomationDispatchResult pending = ExecuteAnimationRuntimeOperation(
				"animation.runtime.diagnostics", {{"includeDetails", true}}, transactions, 1U,
				{}, nullptr, {}, &automationLease);
			context.Expect(pending.ok && automationLease.IsRequested()
				&& pending.data.value("detailsPendingPublication", false),
				"A detailed MCP request must retain its opt-in lease until a later protected frame can publish copied rows.");

			AnimationDetailedDiagnosticsRequest retainedRequest =
				AcquireAnimationDetailedDiagnosticsRequest();
			AnimationDetailedDiagnosticsSnapshot detailed;
			detailed.frameIndex = 88U;
			detailed.collapsed = false;
			AnimationAnimatorDiagnosticRecord animator;
			animator.stableInstanceId = 0x0000000100000001ULL;
			animator.instanceGeneration = 1U;
			animator.selectionReason = AnimationExecutionModeReason::ForceCpuRequested;
			animator.fallbackReason = AnimationExecutionModeReason::ForceCpuRequested;
			animator.poseResidency = AnimationPoseResidencyState::CpuOnly;
			detailed.animators.push_back(animator);
			context.Expect(retainedRequest.IsActive()
				&& PublishAnimationDetailedDiagnosticsSnapshot(detailed),
				"The MCP contract fixture must retain a copied detail publication while a consumer owns it.");
			const AutomationDispatchResult available = ExecuteAnimationRuntimeOperation(
				"animation.runtime.diagnostics", {{"includeDetails", true}}, transactions, 1U,
				{}, nullptr, {}, &automationLease);
			context.Expect(available.ok && !available.data.value("detailsPendingPublication", true)
				&& available.data.value("detailedFrameIndex", 0U) == 88U,
				"A scoped MCP request must return the copied detailed frame when one has reached the protected boundary.");
			retainedRequest.Reset();
			automationLease.Reset();
			context.Expect(!AcquireAnimationDetailedDiagnosticsSnapshot().has_value(),
				"The test's final consumer release must clear the copied detail frame.");
		});

		registry.Add("Editor.Animation.Automation.Runtime.ProductionFrameBuildsBoundedDetailedRows", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			const AnimatorInstanceHandle handle = CreateAutomationAnimator(80U);
			AnimationDetailedDiagnosticsRequest request = AcquireAnimationDetailedDiagnosticsRequest();
			context.Expect(request.IsActive()
				&& ExecuteAutomationAnimationFrame(system, "animation-detailed-production-frame"),
				"An active detailed consumer must observe the normal protected production animation frame.");
			const std::optional<AnimationDetailedDiagnosticsSnapshot> detailed =
				AcquireAnimationDetailedDiagnosticsSnapshot();
			context.Expect(detailed.has_value() && !detailed->collapsed
				&& !detailed->animators.empty()
				&& detailed->animators.size() <= kMaximumAnimationExecutionModeBatchSize,
				"The production boundary must publish a bounded non-empty detailed record set only while requested.");
			request.Reset();
			context.Expect(!AcquireAnimationDetailedDiagnosticsSnapshot().has_value()
				&& system.InvalidateAnimatorInstance(handle),
				"Releasing detailed inspection must clear rows without preventing normal animator cleanup.");
		});

		registry.Add("Editor.Animation.Automation.Runtime.TypedBatchQueuesOnceAndReplaysReceipt", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			const AnimatorInstanceHandle handle = CreateAutomationAnimator(81U);
			AnimationAutomationTransactions transactions;
			const std::size_t pendingBefore = system.PendingCommandCount();
			const nlohmann::json arguments = RuntimeArguments(handle, "typed-batch", nlohmann::json::array({
				{{"kind", "set_speed"}, {"speed", 1.25}},
				{{"kind", "set_looping"}, {"flag", true}}
			}));

			const AutomationDispatchResult accepted = ExecuteAnimationRuntimeOperation(
				"animation.runtime.parameters", arguments, transactions, 1U);
			const AutomationDispatchResult replayed = ExecuteAnimationRuntimeOperation(
				"animation.runtime.parameters", arguments, transactions, 1U);
			context.Expect(accepted.ok
				&& accepted.data.value("status", std::string{}) == "queued"
				&& accepted.data.value("queuedCount", 0U) == 2U,
				"A valid runtime request must enter the native typed deferred command queue. Result: "
					+ accepted.message + " Data: " + accepted.data.dump());
			context.Expect(replayed.ok
				&& replayed.data.value("idempotencyDisposition", std::string{}) == "replay"
				&& system.PendingCommandCount() == pendingBefore + 2U,
				"An exact retry must replay the retained receipt without queueing a second batch.");

			for(std::size_t index = 0U; index < accepted.data["commands"].size(); ++index)
			{
				const nlohmann::json& receipt = accepted.data["commands"][index];
				const AnimationCommandReceipt cancelled = system.CancelCommand(
					receipt["cancellationToken"].get<std::uint64_t>(),
					9000U + static_cast<std::uint64_t>(index));
				context.Expect(cancelled.status == AnimationStatus::Cancelled,
					"The test must remove every deferred command through its copied cancellation identity.");
			}
			context.Expect(system.InvalidateAnimatorInstance(handle),
				"The automation test must retire its generation-safe animator fixture.");
		});

		registry.Add("Editor.Animation.Automation.Runtime.InvalidDryRunAndStaleBatchesStayAtomic", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			const AnimatorInstanceHandle handle = CreateAutomationAnimator(82U);
			const std::size_t pendingBefore = system.PendingCommandCount();

			AnimationAutomationTransactions invalidTransactions;
			nlohmann::json invalid = RuntimeArguments(handle, "invalid-batch", nlohmann::json::array({
				{{"kind", "set_speed"}, {"speed", 2.0}},
				{{"kind", "set_parameter"}, {"parameterId", 7U}, {"parameterType", "integer"}, {"integerValue", 1.5}}
			}));
			const AutomationDispatchResult rejected = ExecuteAnimationRuntimeOperation(
				"animation.runtime.parameters", invalid, invalidTransactions, 1U);
			context.Expect(!rejected.ok && rejected.error == ErrorCategory::Validation
				&& system.PendingCommandCount() == pendingBefore,
				"A malformed later command must reject before a valid prefix reaches native storage.");

			AnimationAutomationTransactions dryRunTransactions;
			nlohmann::json dryRun = RuntimeArguments(handle, "dry-run-batch", nlohmann::json::array({
				{{"kind", "set_parameter"}, {"parameterId", 7U}, {"parameterType", "integer"}, {"integerValue", -2}}
			}));
			dryRun["dryRun"] = true;
			const AutomationDispatchResult validated = ExecuteAnimationRuntimeOperation(
				"animation.runtime.parameters", dryRun, dryRunTransactions, 1U);
			context.Expect(validated.ok
				&& validated.data.value("status", std::string{}) == "validated"
				&& system.PendingCommandCount() == pendingBefore,
				"Dry-run must validate the live target and typed payload without publishing work.");

			AnimationAutomationTransactions staleTransactions;
			nlohmann::json stale = RuntimeArguments(handle, "stale-batch", nlohmann::json::array({
				{{"kind", "play"}}
			}));
			stale["expectedGeneration"] = 2U;
			const AutomationDispatchResult staleResult = ExecuteAnimationRuntimeOperation(
				"animation.runtime.parameters", stale, staleTransactions, 1U);
			context.Expect(!staleResult.ok && staleResult.error == ErrorCategory::StaleTarget
				&& system.PendingCommandCount() == pendingBefore,
				"A stale automation generation must reject before the typed runtime boundary.");
			context.Expect(system.InvalidateAnimatorInstance(handle),
				"The atomicity test must retire its generation-safe animator fixture.");
		});

		registry.Add("Editor.Animation.Automation.Runtime.ProceduralAndRetargetMcpRemainDeferredAndCopied", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			const AnimatorInstanceHandle handle = CreateAutomationAnimator(84U);
			AnimationAutomationTransactions proceduralTransactions;
			const nlohmann::json clear = ProceduralClearArguments(handle, "procedural-clear");
			const AutomationDispatchResult queuedClear = ExecuteAnimationRuntimeOperation(
				"animation.runtime.procedural.goal", clear, proceduralTransactions, 1U);
			const AutomationDispatchResult replayedClear = ExecuteAnimationRuntimeOperation(
				"animation.runtime.procedural.goal", clear, proceduralTransactions, 1U);
			const AutomationDispatchResult beforeBoundary = ExecuteAnimationRuntimeOperation(
				"animation.runtime.procedural.read", {{"target", RuntimeTarget(handle)}, {"maximumItems", 1U}}, proceduralTransactions, 1U);
			context.Expect(queuedClear.ok
				&& queuedClear.data.value("acknowledgement", std::string{}) == "queued_not_yet_published"
				&& replayedClear.ok
				&& replayedClear.data.value("idempotencyDisposition", std::string{}) == "replay"
				&& beforeBoundary.ok,
				"A procedural MCP write must acknowledge only queue admission and an exact retry must not enqueue duplicate work.");
			context.Expect(ExecuteAutomationAnimationFrame(system, "animation-procedural-mcp-boundary"),
				"The production protected animation boundary must consume the copied procedural command.");
			const AutomationDispatchResult afterBoundary = ExecuteAnimationRuntimeOperation(
				"animation.runtime.procedural.read", {{"target", RuntimeTarget(handle)}, {"maximumItems", 1U}}, proceduralTransactions, 1U);
			context.Expect(afterBoundary.ok
				&& afterBoundary.data.value("activeGoalCount", 1U) == 0U
				&& afterBoundary.data.value("visibilityBoundary", std::string{}) == "immutable_procedural_store_copies",
				"A clear must become visible only through the copied protected-boundary procedural readback.");

			AnimationAutomationTransactions staleTransactions;
			nlohmann::json stale = ProceduralClearArguments(handle, "procedural-stale");
			stale["expectedGeneration"] = 2U;
			const AutomationDispatchResult staleResult = ExecuteAnimationRuntimeOperation(
				"animation.runtime.procedural.goal", stale, staleTransactions, 1U);
			context.Expect(!staleResult.ok && staleResult.error == ErrorCategory::StaleTarget,
				"A stale automation generation must reject before a procedural request reaches runtime storage.");

			AnimationAutomationTransactions stalePoseTransactions;
			const AutomationDispatchResult queuedStalePose = ExecuteAnimationRuntimeOperation(
				"animation.runtime.procedural.goal", ProceduralSetArguments(handle, "procedural-stale-pose"), stalePoseTransactions, 1U);
			context.Expect(queuedStalePose.ok && ExecuteAutomationAnimationFrame(system, "animation-procedural-stale-pose"),
				"A syntactically valid procedural set must defer publication-time pose generation validation.");
			const AutomationDispatchResult stalePoseRead = ExecuteAnimationRuntimeOperation(
				"animation.runtime.procedural.read", {{"target", RuntimeTarget(handle)}, {"maximumItems", 1U}}, stalePoseTransactions, 1U);
			context.Expect(stalePoseRead.ok && stalePoseRead.data.value("activeGoalCount", 1U) == 0U,
				"A stale pose/publication precondition must leave procedural active-goal state unchanged.");

			AnimationAutomationTransactions retargetTransactions;
			const AutomationDispatchResult queuedMismatch = ExecuteAnimationRuntimeOperation(
				"animation.runtime.retarget.request", RetargetArguments(handle, "retarget-mismatch"), retargetTransactions, 1U);
			context.Expect(queuedMismatch.ok && ExecuteAutomationAnimationFrame(system, "animation-retarget-mismatch"),
				"A retarget request must be queued before the protected runtime checks its prebound profile.");
			const AutomationDispatchResult retargetRead = ExecuteAnimationRuntimeOperation(
				"animation.runtime.retarget.read", {{"target", RuntimeTarget(handle)}, {"maximumItems", 1U}}, retargetTransactions, 1U);
			context.Expect(retargetRead.ok
				&& retargetRead.data.value("visibilityBoundary", std::string{}) == "immutable_retarget_store_copies"
				&& retargetRead.data.value("snapshotCount", 0U) == 1U
				&& retargetRead.data["snapshots"][0].value("profileGeneration", 0U) == 1U
				&& retargetRead.data["latestSnapshot"] == retargetRead.data["snapshots"][0],
				"Retarget reads must expose only bounded copied publications, including a failed prebound-profile request.");
			context.Expect(system.InvalidateAnimatorInstance(handle),
				"The procedural and retarget automation test must retire its generation-safe fixture.");
		});

		registry.Add("Editor.Animation.Automation.Runtime.MotionAndRagdollMcpRemainDeferredAndCopied", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			const AnimatorInstanceHandle handle = CreateAutomationAnimator(85U);
			const nlohmann::json target = RuntimeTarget(handle);
			const nlohmann::json motion = {{"target", target}, {"action", "set_database"},
				{"assetId", ToHexString(GenerateUUID())}, {"assetGeneration", 1U}, {"featureCount", 48U},
				{"idempotencyKey", "motion-deferred"}, {"correlationId", "motion-deferred"}, {"expectedGeneration", 1U}};
			AnimationAutomationTransactions motionTransactions;
			const AutomationDispatchResult queuedMotion = ExecuteAnimationRuntimeOperation(
				"animation.runtime.motion_match.request", motion, motionTransactions, 1U);
			const AutomationDispatchResult replayedMotion = ExecuteAnimationRuntimeOperation(
				"animation.runtime.motion_match.request", motion, motionTransactions, 1U);
			const nlohmann::json ragdoll = {{"target", target}, {"action", "activate"},
				{"assetId", ToHexString(GenerateUUID())}, {"assetGeneration", 1U}, {"physicsWorldGeneration", 1U}, {"blendWeight", 0.5},
				{"idempotencyKey", "ragdoll-deferred"}, {"correlationId", "ragdoll-deferred"}, {"expectedGeneration", 1U}};
			AnimationAutomationTransactions ragdollTransactions;
			const AutomationDispatchResult queuedRagdoll = ExecuteAnimationRuntimeOperation(
				"animation.runtime.ragdoll.request", ragdoll, ragdollTransactions, 1U);
			context.Expect(queuedMotion.ok && replayedMotion.ok
				&& replayedMotion.data.value("idempotencyDisposition", std::string{}) == "replay"
				&& queuedRagdoll.ok && ExecuteAutomationAnimationFrame(system, "animation-motion-ragdoll-mcp-boundary"),
				"Motion and ragdoll MCP writes must acknowledge copied queue admission, replay safely, and defer owner validation to a protected frame.");
			const AutomationDispatchResult motionRead = ExecuteAnimationRuntimeOperation(
				"animation.runtime.motion_match.read", {{"target", target}, {"maximumItems", 1U}}, motionTransactions, 1U);
			const AutomationDispatchResult ragdollRead = ExecuteAnimationRuntimeOperation(
				"animation.runtime.ragdoll.read", {{"target", target}, {"maximumItems", 1U}}, ragdollTransactions, 1U);
			context.Expect(motionRead.ok && ragdollRead.ok
				&& motionRead.data.value("snapshotCount", 0U) == 1U
				&& ragdollRead.data.value("snapshotCount", 0U) == 1U
				&& motionRead.data["latestSnapshot"] == motionRead.data["snapshots"][0]
				&& ragdollRead.data["latestSnapshot"] == ragdollRead.data["snapshots"][0],
				"Missing bound assets must be exposed only as copied post-boundary motion and ragdoll publications.");
			context.Expect(system.InvalidateAnimatorInstance(handle),
				"The motion/ragdoll automation fixture must retire its generation-safe animator.");
		});

		registry.Add("Editor.Animation.Automation.Runtime.RecordingEndpointRequiresCopiedStableBinding", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			const AnimatorInstanceHandle handle = CreateAutomationAnimator(83U);
			AnimationAutomationTransactions transactions;
			EditorAutomationJobService jobStore;
			AnimationAutomationJobs jobs(jobStore);
			RecordingEndpointDirectory assetRoot;

			nlohmann::json missingBinding = RecordingStartArguments(handle);
			missingBinding.erase("jointStableIds");
			const AutomationDispatchResult rejected = ExecuteAnimationRuntimeOperation(
				"animation.recording.start", missingBinding, transactions, 1U, {}, &jobs, assetRoot.Path());
			context.Expect(!rejected.ok && rejected.error == ErrorCategory::Validation,
				"Recording must fail closed when the caller does not supply copied stable joint IDs.");

			const AutomationDispatchResult unavailable = ExecuteAnimationRuntimeOperation(
				"animation.recording.start", RecordingStartArguments(handle), transactions, 1U, {}, &jobs, assetRoot.Path());
			context.Expect(!unavailable.ok && unavailable.error == ErrorCategory::StaleTarget,
				"Recording must reject an unapproved target rather than creating a one-frame placeholder session.");
			context.Expect(system.InvalidateAnimatorInstance(handle),
				"The recording endpoint test must retire its generation-safe animator fixture.");
		});

		registry.Add("Editor.Animation.Automation.Runtime.RecordingApprovalBindsExactPublishedPose", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			const AnimatorInstanceHandle handle = CreateAutomationAnimator(84U);
			AnimatorPlaybackBinding playback;
			playback.skeleton = CreateAutomationSkeleton();
			playback.jointStableIds = {101U};
			// The race fixture deliberately requests a fresh immutable bind pose on
			// each failed-dependency frame; hold-last would correctly reuse generation one.
			playback.fallbackPolicy = AnimationFallbackPolicy::BindPose;
			context.Expect(system.BindAnimatorPlayback(handle, playback) == AnimationStatus::MissingAsset
					&& ExecuteAutomationAnimationFrame(system, "recording-approval-initial"),
				"The recording fixture must publish a complete immutable safe pose through the production frame boundary.");
			const PinnedAnimationAuthorityRow approvedPose =
				PinnedAnimationAuthorityRow::ForAnimator(system, handle);
			context.Expect(approvedPose && approvedPose->snapshot
					&& approvedPose->snapshot->tick > 0
					&& approvedPose->jointStableIds == std::vector<std::uint64_t>{101U},
				"The approval fixture needs one non-zero-tick pose with a copied stable-joint binding.");
			const AnimationGeneration approvedGeneration =
				approvedPose && approvedPose->snapshot
					? approvedPose->snapshot->publicationGeneration : 0U;
			const AnimationFixedTick approvedTick =
				approvedPose && approvedPose->snapshot ? approvedPose->snapshot->tick : -1;
			const std::vector<std::uint64_t> approvedJoints =
				approvedPose ? approvedPose->jointStableIds : std::vector<std::uint64_t>{};

			AnimationAutomationTransactions transactions;
			EditorAutomationJobService jobStore;
			AnimationAutomationJobs jobs(jobStore);
			RecordingEndpointDirectory assetRoot;
			nlohmann::json arguments = RecordingStartArguments(handle);
			arguments["bindCurrentPublication"] = true;
			arguments["publicationGeneration"] = 0U;
			arguments["jointStableIds"] = nlohmann::json::array({999U});
			arguments["destinationPath"] = "Captured/ApprovalBound.teanim";
			// These values intentionally describe a four-tick relative range from a
			// stale client snapshot. Approval-time binding must rebase that duration
			// onto the current immutable pose instead of rejecting a real UI delay.
			arguments["firstTick"] = 0U;
			arguments["lastTick"] = 4U;
			arguments["sampleRate"] = 24U;
			arguments["idempotencyKey"] = "recording-approval-bound";
			const AutomationDispatchResult accepted = ExecuteAnimationRuntimeOperation(
				"animation.recording.start", arguments, transactions, 1U, {}, &jobs, assetRoot.Path());
			const std::string jobId = accepted.data.value("jobId", std::string{});
			context.Expect(accepted.ok && !jobId.empty()
					&& accepted.data.value("boundPublicationGeneration", 0U)
						== approvedGeneration
					&& accepted.data.value("boundTick", std::int64_t{-1})
						== approvedTick
					&& accepted.data.value("boundJointStableIds",
						std::vector<std::uint64_t>{}) == approvedJoints,
				"Approval-safe start must rebase a relative range, ignore stale caller bindings, and retain the exact current publication. Result: "
					+ accepted.message + " Data: " + accepted.data.dump());
			// Advance production frames without a job.get call. At 24 Hz the rational
			// cadence captures ticks 0 and 3 while the unaligned end tick 4 only closes
			// the range; it must not manufacture a third numeric sample.
			context.Expect(ExecuteAutomationAnimationFrame(system, "recording-approval-newer"),
				"The live animator should continue publishing after approval.");
			const PinnedAnimationAuthorityRow newerPose =
				PinnedAnimationAuthorityRow::ForAnimator(system, handle);
			context.Expect(newerPose && newerPose->snapshot
					&& newerPose->snapshot->publicationGeneration
						> approvedGeneration,
				"The race regression requires a genuinely newer live publication before the job is pumped.");

			jobs.PumpActiveRecordingsFromPublishedSnapshots();
			context.Expect(ExecuteAutomationAnimationFrame(system, "recording-approval-complete"),
				"The recording fixture must publish a third real production frame.");
			jobs.PumpActiveRecordingsFromPublishedSnapshots();
			context.Expect(ExecuteAutomationAnimationFrame(system, "recording-approval-fourth"),
				"The 24 Hz cadence fixture must publish its deterministic tick-three sample.");
			jobs.PumpActiveRecordingsFromPublishedSnapshots();
			context.Expect(ExecuteAutomationAnimationFrame(system, "recording-approval-unaligned-end"),
				"The 24 Hz cadence fixture must reach its unaligned declared end through a fresh publication.");
			jobs.PumpActiveRecordingsFromPublishedSnapshots();
			const AutomationDispatchResult terminal = jobs.Get(jobId);
			const nlohmann::json retainedResult = terminal.data.value(
				"result", nlohmann::json::object());
			context.Expect(terminal.ok
					&& terminal.data.value("state", std::string{}) == "succeeded"
					&& retainedResult.value("published", false)
					&& retainedResult.value("candidateValidated", false)
					&& retainedResult.value("capturedSamples", 0U) == 2U
					&& retainedResult.value("boundPublicationGeneration", 0U)
						== approvedGeneration
					&& retainedResult.value("boundTick", std::int64_t{-1})
						== approvedTick
					&& retainedResult.value("boundJointStableIds",
						std::vector<std::uint64_t>{}) == approvedJoints,
				"The durable terminal result must prove the exact approval-time generation, tick, and joints were captured.");
			const PinnedAnimationAuthorityRow stopPose =
				PinnedAnimationAuthorityRow::ForAnimator(system, handle);
			nlohmann::json stopArguments = RecordingStartArguments(handle);
			stopArguments["bindCurrentPublication"] = true;
			stopArguments["publicationGeneration"] = 0U;
			stopArguments["jointStableIds"] = nlohmann::json::array({999U});
			stopArguments["destinationPath"] = "Captured/ExplicitStop.teanim";
			stopArguments["firstTick"] = static_cast<std::uint64_t>(stopPose->snapshot->tick);
			stopArguments["lastTick"] = static_cast<std::uint64_t>(stopPose->snapshot->tick + 4);
			stopArguments["idempotencyKey"] = "recording-explicit-stop";
			const AutomationDispatchResult stopStarted = ExecuteAnimationRuntimeOperation(
				"animation.recording.start", stopArguments, transactions, 1U, {}, &jobs, assetRoot.Path());
			const std::string stopJobId = stopStarted.data.value("jobId", std::string{});
			const AutomationDispatchResult stopped = jobs.StopRecording(stopJobId);
			const AutomationDispatchResult stoppedRetained = jobs.Get(stopJobId);
			const nlohmann::json stoppedResult = stoppedRetained.data.value("result", nlohmann::json::object());
			context.Expect(stopStarted.ok && stopped.ok && stoppedRetained.ok
					&& stoppedResult.value("boundPublicationGeneration", 0U) == stopPose->snapshot->publicationGeneration
					&& stoppedResult.value("boundTick", std::int64_t{-1}) == stopPose->snapshot->tick
					&& stoppedResult.value("boundJointStableIds", std::vector<std::uint64_t>{}) == stopPose->jointStableIds,
				"An explicit typed stop must retain the same approval binding evidence as range completion.");
			nlohmann::json cancelArguments = RecordingStartArguments(handle);
			cancelArguments["bindCurrentPublication"] = true;
			cancelArguments["publicationGeneration"] = 0U;
			cancelArguments["jointStableIds"] = nlohmann::json::array({999U});
			cancelArguments["destinationPath"] = "Captured/CanceledAtPausedTick.teanim";
			cancelArguments["firstTick"] = static_cast<std::uint64_t>(stopPose->snapshot->tick);
			cancelArguments["lastTick"] = static_cast<std::uint64_t>(stopPose->snapshot->tick + 4);
			cancelArguments["idempotencyKey"] = "recording-cancel-paused-tick";
			const AutomationDispatchResult cancelStarted = ExecuteAnimationRuntimeOperation(
				"animation.recording.start", cancelArguments, transactions, 1U, {}, &jobs, assetRoot.Path());
			const std::string cancelJobId = cancelStarted.data.value("jobId", std::string{});
			const AutomationDispatchResult cancelRequested = jobs.Cancel(cancelJobId);
			// Do not advance the animation frame: a paused editor can only republish
			// the same tick, but cancellation must still reach a durable terminal state.
			jobs.PumpActiveRecordingsFromPublishedSnapshots();
			const AutomationDispatchResult canceledRetained = jobs.Get(cancelJobId);
			const nlohmann::json canceledResult = canceledRetained.data.value(
				"result", nlohmann::json::object());
			context.Expect(cancelStarted.ok && cancelRequested.ok && canceledRetained.ok
					&& canceledRetained.data.value("state", std::string{}) == "canceled"
					&& !canceledResult.value("published", true)
					&& canceledResult.value("boundPublicationGeneration", 0U)
						== stopPose->snapshot->publicationGeneration
					&& canceledResult.value("boundTick", std::int64_t{-1})
						== stopPose->snapshot->tick
					&& canceledResult.value("boundJointStableIds",
						std::vector<std::uint64_t>{}) == stopPose->jointStableIds,
				"Cancellation must complete on a paused duplicate tick without publishing a partial recording.");
			context.Expect(system.InvalidateAnimatorInstance(handle),
				"The approval-bound recording test must retire its generation-safe animator fixture.");
		});
	}
}
