// LocomotionRagdollRecoveryTests.cpp - real Physics ownership and recovery handoff.

#include "../TestHarness.h"
#include "LocomotionAnimationFixture.h"

#include "Animation/AnimationSystem.h"
#include "Animation/Ragdoll/RagdollState.h"
#include "Animation/Runtime/AnimationConsumerRegistry.h"
#include "Components/CharacterLocomotionComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Transform.h"
#include "Components/AnimatorComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Locomotion/LocomotionCommands.h"
#include "Locomotion/LocomotionSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::Locomotion;
		using namespace TerraEngine::Physics;

		constexpr float kFixedStep = 1.0F / 60.0F;

		NativeAnimationUuid ToNativeUuid(const UUID& value)
		{
			NativeAnimationUuid result{};
			std::memcpy(&result.first, value.data(), sizeof(result.first));
			std::memcpy(&result.second, value.data() + sizeof(result.first), sizeof(result.second));
			return result;
		}

		bool MatchesNativeUuid(const NativeAnimationUuid& actual, const UUID& expected)
		{
			const NativeAnimationUuid converted = ToNativeUuid(expected);
			return actual.first == converted.first && actual.second == converted.second;
		}

		std::string DescribeReceipt(const CommandReceipt& receipt)
		{
			return "state=" + std::to_string(static_cast<unsigned>(receipt.state))
				+ ",reason=" + std::to_string(static_cast<unsigned>(receipt.reason))
				+ ",epoch=" + std::to_string(receipt.authorityEpoch)
				+ ",slotGeneration=" + std::to_string(receipt.handle.slotGeneration)
				+ ",token=" + std::to_string(receipt.ownerToken);
		}

		bool ExecuteAnimationFrame(AnimationSystem& animation)
		{
			auto workloads = animation.DeclareFrameWorkloads(kFixedStep);
			const auto result = Scheduling::ExecuteFrameWorkloadsCompatibility(
				std::move(workloads), "locomotion-ragdoll-recovery");
			return result.valid && result.completed;
		}

		class RagdollRecoveryRuntime final {
		public:
			explicit RagdollRecoveryRuntime(TestContext& context) : context_(context)
			{
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				ObjectManager::instance().clear();
				Scene::SceneMutationQueue::Instance().Clear();
				locomotion_.Shutdown();
				animation_.Shutdown();
				AnimationConsumerRegistry::Instance().Clear();
				physics_.Shutdown();
				physics_.SetEditorLifecycleControlled(true);
				physics_.Initialize();
				assets_ = LocomotionAnimationFixture::Create(context_);
			}

			~RagdollRecoveryRuntime()
			{
				locomotion_.Shutdown();
				animation_.Shutdown();
				AnimationConsumerRegistry::Instance().Clear();
				(void)physics_.StopWorld();
				physics_.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				assets_.reset();
				ThreadPool::instance().shutdown();
			}

			bool Start(bool attachValidProfile)
			{
				if(!assets_) return false;
				auto& objects = ObjectManager::instance();
				owner_ = objects.createObject("Locomotion ragdoll recovery owner");
				if(!objects.AddComponent<Transform>(owner_)) return false;
				auto* controller = objects.AddComponent<CharacterControllerComponent>(owner_);
				auto* movement = objects.AddComponent<CharacterLocomotionComponent>(owner_);
				auto* animator = assets_->AttachAnimator(objects, owner_);
				if(!controller || !movement || !animator) return false;
				controller->radius = 0.35F;
				controller->height = 1.8F;
				controller->localCenter = {0.0F, 0.9F, 0.0F};
				controller->useGravity = false;
				movement->animatorBinding = CharacterLocomotionComponent::kSelfBinding;
				movement->maximumSpeed = 8.0;
				animator->controllerAssetId = EMPTY_UUID;
				animator->defaultClipAssetId = assets_->RootClip().assetId.value;
				animator->skeletonOverrideAssetId = assets_->Skeleton().assetId.value;
				animator->rootMotionPolicy = AnimatorRootMotionPolicy::ExtractOnly;
				if(attachValidProfile)
					animator->ragdollProfileOverrideAssetId = assets_->RagdollProfileId();

				animation_.SetHeadlessExecutionRequired(true);
				animation_.Initialize();
				if(!physics_.StartWorld()) return false;
				for(std::uint32_t index = 0U; index < 4U; ++index) AdvancePhysics();
				locomotion_.Initialize();
				locomotion_.ObserveProtectedBoundary();
				for(std::uint32_t index = 0U; index < 3U; ++index) {
					if(!AdvanceAnimation()) return false;
					locomotion_.ObserveProtectedBoundary();
				}
				locomotion_.Update(kFixedStep);
				const auto handle = locomotion_.ResolvePublished(objects.getUUID(owner_));
				if(!handle) return false;
				handle_ = *handle;
				animator_ = animation_.ResolveAnimatorForObject(objects.getUUID(owner_));
				const PhysicsCharacterCommandTarget target = Controller();
				return animator_.IsValid() && target.world.IsValid() && target.character.IsValid();
			}

			void AdvancePhysics()
			{
				auto& objects = ObjectManager::instance();
				(void)objects.PublishSceneChangePacket();
				physics_.Update(kFixedStep);
				const auto flushed = Scene::SceneMutationQueue::Instance().Apply(
					objects, "locomotion-ragdoll-recovery-physics-flush");
				physics_.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
				lastFlushTrace_ = DescribeFlush(flushed);
				const auto ragdollResults = physics_.LatestRagdollPhysicsResults();
				if(ragdollResults && ragdollResults->results) {
					for(const RagdollPhysicsResult& candidate : *ragdollResults->results) {
						const bool alreadyRecorded = std::any_of(retainedRagdollResults_.begin(),
							retainedRagdollResults_.end(), [&candidate](const RagdollPhysicsResult& prior) {
								return prior.ragdollId == candidate.ragdollId
									&& prior.kind == candidate.kind
									&& prior.correlationId == candidate.correlationId
									&& prior.generation == candidate.generation
									&& prior.committedStep == candidate.committedStep
									&& prior.status == candidate.status;
							});
						if(!alreadyRecorded) retainedRagdollResults_.push_back(candidate);
					}
				}
			}

			std::string DescribeFlush(const Scene::SceneMutationBatchResult& flushed) const
			{
				std::string result = "flush(applied=" + std::to_string(flushed.applied)
					+ ",accepted=" + std::to_string(flushed.acceptedMutationCount)
					+ ",rejected=" + std::to_string(flushed.rejectedMutationCount)
					+ ",transactions=" + std::to_string(flushed.transactionResults.size())
					+ ",groups=" + std::to_string(flushed.publicationGroupResults.size()) + ")";
				for(const auto& transaction : flushed.transactionResults)
					result += " tx[" + transaction.transactionId + ":status="
						+ std::to_string(static_cast<unsigned>(transaction.status))
						+ ",message=" + transaction.message + "]";
				for(const auto& group : flushed.publicationGroupResults)
					result += " group[" + group.groupId + ":status="
						+ std::to_string(static_cast<unsigned>(group.status))
						+ ",preflight=" + std::to_string(group.preflightAccepted)
						+ ",writes=" + std::to_string(group.appliedWriteCount)
						+ ",rejected=" + std::to_string(group.rejectedWriteCount)
						+ ",message=" + group.message + "]";
				return result;
			}

			std::string DescribePhysicsDiagnostics() const
			{
				const auto diagnostics = physics_.LatestDiagnostics();
				if(!diagnostics) return "physicsDiagnostics=missing";
				const auto& publication = diagnostics->publication;
				return "physics(tick=" + std::to_string(diagnostics->tick)
					+ ",prepared=" + std::to_string(publication.preparedPackets)
					+ ",accepted=" + std::to_string(publication.acceptedPackets)
					+ ",rejected=" + std::to_string(publication.rejectedPackets)
					+ ",acknowledged=" + std::to_string(publication.acknowledgedEntries)
					+ ",currentEntries=" + std::to_string(publication.currentEntries)
					+ ",currentChunks=" + std::to_string(publication.currentChunks)
					+ ",pendingGroups=" + std::to_string(publication.pendingGroups)
					+ ",lastFailure=" + publication.lastFailure + ")";
			}

			std::string DescribeRagdollResults() const
			{
				constexpr std::size_t kMaximumDiagnosticResults = 6U;
				const std::size_t first = retainedRagdollResults_.size() > kMaximumDiagnosticResults
					? retainedRagdollResults_.size() - kMaximumDiagnosticResults : 0U;
				std::string result = "ragdollResults=" + std::to_string(retainedRagdollResults_.size())
					+ (first != 0U ? "(older omitted=" + std::to_string(first) + ")" : "");
				for(std::size_t index = first; index < retainedRagdollResults_.size(); ++index) {
					const auto& item = retainedRagdollResults_[index];
					result += " [id=" + std::to_string(item.ragdollId)
						+ ",kind=" + std::to_string(static_cast<unsigned>(item.kind))
						+ ",status=" + std::to_string(static_cast<unsigned>(item.status))
						+ ",correlation=" + std::to_string(item.correlationId)
						+ ",generation=" + std::to_string(item.generation.worldGeneration) + "/"
						+ std::to_string(item.generation.profileGeneration) + "/"
						+ std::to_string(item.generation.skeletonGeneration) + "/"
						+ std::to_string(item.generation.publicationGeneration)
						+ ",step=" + std::to_string(item.committedStep)
						+ ",diagnostic=" + item.diagnostic + "]";
				}
				return result;
			}

			std::string DescribePoseState() const
			{
				const auto state = State();
				const auto* transform = ObjectManager::instance().GetComponent<Transform>(owner_);
				std::string result = "poseState=";
				if(state) result += "physics(" + std::to_string(state->pose.position.x) + ","
					+ std::to_string(state->pose.position.y) + "," + std::to_string(state->pose.position.z)
					+ ",tick=" + std::to_string(state->tick) + ")";
				else result += "physics(missing)";
				if(transform) {
					const auto& position = transform->worldPose.position;
					result += " transform(cell=" + std::to_string(position.cell.x) + ","
						+ std::to_string(position.cell.y) + "," + std::to_string(position.cell.z)
						+ ";offset=" + std::to_string(position.offsetMetres.x) + ","
						+ std::to_string(position.offsetMetres.y) + ","
						+ std::to_string(position.offsetMetres.z) + ",revision="
						+ std::to_string(ObjectManager::instance().GetObjectTransformRevision(owner_)) + ")";
			} else result += " transform(missing)";
				return result;
			}

			std::string LastFlushTrace() const { return lastFlushTrace_; }
			bool IsAnimatorAlive() const { return animation_.IsAnimatorInstanceAlive(animator_); }

			bool AdvanceAnimation() { return ExecuteAnimationFrame(animation_); }

			void ReconcileLocomotion()
			{
				locomotion_.ObserveProtectedBoundary();
				locomotion_.Update(kFixedStep);
			}

			PhysicsCharacterCommandTarget Controller() const
			{
				const auto packet = physics_.LatestCharacterStatePacket();
				if(!packet || !packet->states) return {};
				const UUID objectId = ObjectManager::instance().getUUID(owner_);
				const auto found = std::find_if(packet->states->begin(), packet->states->end(),
					[objectId](const PhysicsCharacterState& state) { return state.objectId == objectId; });
				if(found == packet->states->end()) return {};
				return {found->world, found->handle, found->objectId,
					found->objectGeneration, found->componentGeneration};
			}

			std::optional<PhysicsCharacterState> State() const
			{
				const auto packet = physics_.LatestCharacterStatePacket();
				if(!packet || !packet->states) return std::nullopt;
				const UUID objectId = ObjectManager::instance().getUUID(owner_);
				const auto found = std::find_if(packet->states->begin(), packet->states->end(),
					[objectId](const PhysicsCharacterState& state) { return state.objectId == objectId; });
				return found == packet->states->end() ? std::nullopt
					: std::optional<PhysicsCharacterState>(*found);
			}

			std::optional<NativeAnimationRagdollSnapshot> RagdollSnapshot() const
			{
				std::array<NativeAnimationRagdollSnapshot, 32U> snapshots{};
				std::uint32_t requiredCount = 0U;
				if(animation_.CopyRagdollSnapshots(animator_, snapshots, requiredCount) != AnimationStatus::Ok
					|| requiredCount == 0U || requiredCount > snapshots.size()) return std::nullopt;
				// The command store copies retained snapshots oldest-to-newest, so the
				// final row is authoritative even when two rows share a generation.
				return snapshots[requiredCount - 1U];
			}

			std::optional<RagdollPhysicsResult> LatestLifecycleResult(
				RagdollPhysicsRequestKind kind = RagdollPhysicsRequestKind::Activate) const
			{
				const auto page = physics_.LatestRagdollPhysicsResults();
				if(!page || !page->results) return std::nullopt;
				const std::uint64_t ragdollId = (static_cast<std::uint64_t>(animator_.generation) << 32U)
					| (static_cast<std::uint64_t>(animator_.slotIndex) + 1U);
				const auto found = std::find_if(page->results->rbegin(), page->results->rend(),
					[ragdollId, kind](const RagdollPhysicsResult& result) {
						return result.ragdollId == ragdollId && result.kind == kind;
					});
				return found == page->results->rend() ? std::nullopt
					: std::optional<RagdollPhysicsResult>(*found);
			}

			[[nodiscard]] const std::vector<RagdollPhysicsResult>& RetainedRagdollResults() const noexcept
			{
				return retainedRagdollResults_;
			}

			NativeAnimationRagdollReceipt RequestRagdoll(
				NativeAnimationRagdollOperation operation, std::uint64_t correlation,
				bool validProfile, std::uint32_t flags = 0U)
			{
				NativeAnimationRagdollRequest request{};
				request.header = {static_cast<std::uint32_t>(sizeof(request)), kNativeAnimationAbiVersion,
					static_cast<std::uint16_t>(NativeAnimationRecordKind::RagdollRequest)};
				request.animator = {animator_.slotIndex, animator_.generation};
				request.operation = operation;
				request.flags = flags;
				request.physicsWorldGeneration = Controller().world.worldGeneration;
				request.blendWeight = 1.0F;
				request.correlationId = correlation;
				if(validProfile) {
					const UUID id = assets_->RagdollProfileId();
					request.profileAssetId = ToNativeUuid(id);
					request.profileGeneration = ResourceManager::Instance()
						.GetAnimationAssetRuntimeGeneration(id, AssetType::RagdollProfile);
				} else {
					request.profileAssetId = {1U, 2U};
					request.profileGeneration = 1U;
				}
				return animation_.SubmitRagdollRequest(request);
			}

			CommandReceipt Acquire(std::uint64_t sequence, MotionMode mode)
			{
				// Admission must use the current published binding and epoch. Component
				// removal or an external Physics handoff can advance either identity, so
				// this explicit Acquire never replays a fixture-cached handle or revision.
				ReconcileLocomotion();
				const auto publishedHandle = locomotion_.ResolvePublished(
					ObjectManager::instance().getUUID(owner_));
				if(!publishedHandle) return {};
				handle_ = *publishedHandle;
				const auto observations = locomotion_.CopyObservations();
				if(!observations) return {};
				const auto row = std::find_if(observations->entries.begin(), observations->entries.end(),
					[this](const LocomotionObservation& value) { return value.handle == handle_; });
				if(row == observations->entries.end()) return {};
				LocomotionCommand command;
				command.kind = CommandKind::Acquire;
				command.handle = handle_;
				command.expectedConfigurationRevision = row->authority.configurationRevision;
				command.expectedAuthorityEpoch = row->authority.authorityEpoch;
				command.producerId = 911U;
				command.sequence = sequence;
				command.correlationId = 91100U + sequence;
				command.mode = mode;
				const auto queued = locomotion_.SubmitBatch(std::span(&command, 1U));
				if(!queued.queued) return {};
				locomotion_.Update(kFixedStep);
				return locomotion_.ReadReceipt(handle_, command.producerId,
					command.sequence, command.correlationId);
			}

			CommandReceipt Submit(CommandKind kind, std::uint64_t sequence,
				std::uint64_t epoch, std::uint64_t token, MotionMode mode)
			{
				LocomotionCommand command;
				command.kind = kind;
				command.handle = handle_;
				command.expectedConfigurationRevision = 1U;
				command.expectedAuthorityEpoch = epoch;
				command.ownerToken = token;
				command.producerId = 911U;
				command.sequence = sequence;
				command.correlationId = 91100U + sequence;
				command.mode = mode;
				const auto queued = locomotion_.SubmitBatch(std::span(&command, 1U));
				if(!queued.queued) return {};
				locomotion_.Update(kFixedStep);
				return locomotion_.ReadReceipt(handle_, command.producerId,
					command.sequence, command.correlationId);
			}

			AnimationStatus RebindToIdle()
			{
				AnimatorPlaybackBinding binding;
				binding.clip = std::make_shared<AnimationClipAsset>(assets_->IdleClip());
				binding.skeleton = std::make_shared<SkeletonAsset>(assets_->Skeleton());
				auto mutableProfile = std::make_shared<RagdollProfileAsset>(assets_->RagdollProfile());
				binding.clipGeneration = ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
					assets_->IdleClip().assetId.value, AssetType::AnimationClip);
				binding.skeletonGeneration = ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
					assets_->Skeleton().assetId.value, AssetType::Skeleton);
				binding.programGeneration = binding.clipGeneration;
				binding.ragdollProfileGeneration = ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
					assets_->RagdollProfileId(), AssetType::RagdollProfile);
				mutableProfile->runtimeGeneration = binding.ragdollProfileGeneration;
				binding.ragdollProfile = std::move(mutableProfile);
				binding.stateId = 1U;
				binding.fallbackCompatibilityFingerprint = assets_->Skeleton().compatibilityFingerprint;
				binding.rootMotionPolicy = AnimatorRootMotionPolicy::ExtractOnly;
				binding.requestedExecutionPolicy = AnimationExecutionPolicy::ForceCpuDeterministicFallback;
				binding.playback.timePolicy = AnimatorTimePolicy::Fixed;
				binding.playback.fixedStepTicks = 1;
				binding.playback.loop = true;
				binding.updatePolicy = AnimatorUpdatePolicy::FixedStep;
				binding.playOnBind = true;
				return animation_.BindAnimatorPlayback(animator_, std::move(binding));
			}

			AnimationSystem& Animation() noexcept { return animation_; }
			LocomotionSystem& Locomotion() noexcept { return locomotion_; }
			PhysicsSystem& Physics() noexcept { return physics_; }
			LocomotionAnimationFixture& Assets() noexcept { return *assets_; }
			Object Owner() const noexcept { return owner_; }
			AnimatorInstanceHandle Animator() const noexcept { return animator_; }

		private:
			TestContext& context_;
			AnimationSystem& animation_ = AnimationSystem::Instance();
			LocomotionSystem& locomotion_ = LocomotionSystem::Instance();
			PhysicsSystem& physics_ = PhysicsSystem::Instance();
			std::unique_ptr<LocomotionAnimationFixture> assets_;
			Object owner_;
			LocomotionHandle handle_;
			AnimatorInstanceHandle animator_;
			std::vector<RagdollPhysicsResult> retainedRagdollResults_;
			std::string lastFlushTrace_ = "flush(not-run)";
		};
	}

	void RegisterLocomotionRagdollRecoveryTests(TestRegistry& registry)
	{
		registry.Add("Engine.Locomotion.RagdollRecovery.ActivePhysicsOwnerRecoversAndReleasesOnce", [](TestContext& context) {
			RagdollRecoveryRuntime runtime(context);
			if(!runtime.Start(true)) {
				context.Expect(false, "The real bound ragdoll fixture must start with its compatible cooked recovery profile.");
				return;
			}
			std::string activationTrace = "afterStart " + runtime.DescribePoseState();
			const auto acquired = runtime.Acquire(1U, MotionMode::RootMotion);
			activationTrace += " afterAcquire " + runtime.DescribePoseState();
			const auto oldLease = runtime.Physics().ReadCharacterMotionLease(runtime.Controller());
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U
				&& oldLease && oldLease->state == CharacterMotionLeaseState::LocomotionOwned,
				"A root-ready controller must begin with a real locomotion lease before ragdoll admission.");
			if(acquired.state != ReceiptState::Applied || !oldLease) return;

			const auto activate = runtime.RequestRagdoll(
				NativeAnimationRagdollOperation::Activate, 9301U, true);
			context.Expect(activate.status == NativeAnimationStatus::Queued,
				"A valid cooked ragdoll Activate must queue through the production ABI request record.");
			runtime.ReconcileLocomotion();
			const auto externalBeforeActivation = runtime.Physics().ReadCharacterMotionLease(runtime.Controller());
			context.Expect(externalBeforeActivation
				&& externalBeforeActivation->state == CharacterMotionLeaseState::ExternalExclusive,
				"The accepted pending Activate must first exclude locomotion at its protected boundary.");
			context.Expect(runtime.RebindToIdle() == AnimationStatus::NotReady,
				"A pending external owner must block rebinding before the ragdoll provider has activated.");
			context.Expect(runtime.AdvanceAnimation(), "The accepted ragdoll request must process in Animation's protected frame.");
			activationTrace += " afterActivateAnimation " + runtime.DescribePoseState();
			runtime.AdvancePhysics();
			activationTrace += " afterActivatePhysics " + runtime.DescribePoseState()
				+ " " + runtime.DescribePhysicsDiagnostics() + " " + runtime.LastFlushTrace();
			const auto physicsResult = runtime.LatestLifecycleResult();
			const std::uint64_t ragdollId = (static_cast<std::uint64_t>(runtime.Animator().generation) << 32U)
				| (static_cast<std::uint64_t>(runtime.Animator().slotIndex) + 1U);
			const RagdollGenerationStamp generation = physicsResult
				? physicsResult->generation : RagdollGenerationStamp{};
			const auto activePose = physicsResult
				? runtime.Physics().LatestRagdollPhysicsPoseAtOrAfter(ragdollId, generation) : nullptr;
			context.Expect(physicsResult && physicsResult->status == AnimationStatus::Ok
				&& activePose && activePose->completeCurrentStep && activePose->bodies.size() == 1U,
				"Physics must create, activate, and publish the fixture's real one-body ragdoll pose.");
			context.Expect(runtime.AdvanceAnimation(), "Animation must acknowledge the Physics activation result.");
			activationTrace += " afterActivationAck " + runtime.DescribePoseState();
			const auto activeSnapshot = runtime.RagdollSnapshot();
			const auto externalActive = runtime.Physics().ReadCharacterMotionLease(runtime.Controller());
			context.Expect(activeSnapshot
				&& activeSnapshot->lifecycle == static_cast<std::uint32_t>(RagdollLifecycleState::Active)
				&& activeSnapshot->bodyCount == 1U
				&& MatchesNativeUuid(activeSnapshot->profileAssetId, runtime.Assets().RagdollProfileId())
				&& externalActive && externalActive->state == CharacterMotionLeaseState::ExternalExclusive
				&& externalActive->externalOwnerId == RagdollExternalOwnerId(runtime.Animator()),
				"The published active snapshot and the Physics character lease must agree on exclusive ragdoll ownership.");
			const AnimationStatus blockedRebind = runtime.RebindToIdle();
			context.Expect(blockedRebind == AnimationStatus::NotReady,
				"Hot rebinding must not replace an Animation runtime while its bound character is externally ragdoll-owned.");
			const auto preserved = runtime.RagdollSnapshot();
			context.Expect(preserved && MatchesNativeUuid(preserved->profileAssetId, runtime.Assets().RagdollProfileId())
				&& preserved->lifecycle == static_cast<std::uint32_t>(RagdollLifecycleState::Active),
				"A rejected hot rebind must preserve the active profile and runtime context.");

			const auto recovering = runtime.RequestRagdoll(
				NativeAnimationRagdollOperation::RequestRecovery, 9302U, true, 0x1U);
			context.Expect(recovering.status == NativeAnimationStatus::Queued,
				"Explicit recovery with the caller's immutable-clearance bit must queue.");
			bool released = false;
			std::string recoveryTrace = activationTrace;
			for(std::uint32_t frame = 0U; frame < 24U; ++frame) {
				context.Expect(runtime.AdvanceAnimation(), "Each recovery Animation frame must complete its protected schedule.");
				const auto lease = runtime.Physics().ReadCharacterMotionLease(runtime.Controller());
				if(lease && lease->state == CharacterMotionLeaseState::Unleased) {
					released = true;
					break;
				}
				runtime.AdvancePhysics();
				if(frame < 4U)
					recoveryTrace += " frame" + std::to_string(frame) + " " + runtime.DescribePoseState()
						+ " " + runtime.DescribePhysicsDiagnostics() + " "
						+ runtime.LastFlushTrace() + " " + runtime.DescribeRagdollResults();
			}
			context.Expect(released,
				"A bounded number of real Animation/Physics frames must complete the recovery blend and external handoff.");
			const auto recoveredSnapshot = runtime.RagdollSnapshot();
			const auto releasedLease = runtime.Physics().ReadCharacterMotionLease(runtime.Controller());
			context.Expect(recoveredSnapshot
				&& recoveredSnapshot->lifecycle == static_cast<std::uint32_t>(RagdollLifecycleState::Inactive)
				&& releasedLease && releasedLease->state == CharacterMotionLeaseState::Unleased,
				"Recovery must publish Inactive only after Physics accepts teardown and releases the external lease.");
			context.Expect(runtime.RebindToIdle() == AnimationStatus::Ok,
				"The same complete immutable runtime binding must be replaceable after external recovery release.");

			const auto staleOwner = runtime.Submit(CommandKind::Stop, 2U,
				acquired.authorityEpoch, acquired.ownerToken, MotionMode::RootMotion);
			context.Expect(staleOwner.state != ReceiptState::Applied,
				"The pre-ragdoll owner token must not regain authority after recovery.");
			runtime.ReconcileLocomotion();
			runtime.AdvancePhysics();
			const auto handoffState = runtime.State();
			const auto* transform = ObjectManager::instance().GetComponent<Transform>(runtime.Owner());
			const Scene::WorldLocalizationResult localizedTransform = transform
				? Scene::LocalizeWorldPosition(transform->worldPose.position, Scene::WorldPosition{},
					Scene::kTransformSpaceMaximumRelativeRadiusMetres)
				: Scene::WorldLocalizationResult{};
			const bool localizedFinite = localizedTransform.Succeeded()
				&& std::isfinite(localizedTransform.offsetMetres.x)
				&& std::isfinite(localizedTransform.offsetMetres.y)
				&& std::isfinite(localizedTransform.offsetMetres.z);
			context.Expect(handoffState.has_value() && transform != nullptr && localizedFinite
				&& std::abs(localizedTransform.offsetMetres.x - handoffState->pose.position.x) < 0.02
				&& std::abs(localizedTransform.offsetMetres.y - handoffState->pose.position.y) < 0.02
				&& std::abs(localizedTransform.offsetMetres.z - handoffState->pose.position.z) < 0.02,
				"The handed-off controller pose must match the next authoritative Physics frame; state=("
				+ (handoffState ? std::to_string(handoffState->pose.position.x) : "missing") + ","
				+ (handoffState ? std::to_string(handoffState->pose.position.y) : "missing") + ","
				+ (handoffState ? std::to_string(handoffState->pose.position.z) : "missing") + "), transform=("
				+ (localizedFinite ? std::to_string(localizedTransform.offsetMetres.x) : "not-localized") + ","
				+ (localizedFinite ? std::to_string(localizedTransform.offsetMetres.y) : "not-localized") + ","
				+ (localizedFinite ? std::to_string(localizedTransform.offsetMetres.z) : "not-localized")
				+ "), cell=(" + (transform ? std::to_string(transform->worldPose.position.cell.x) : "missing") + ","
				+ (transform ? std::to_string(transform->worldPose.position.cell.y) : "missing") + ","
				+ (transform ? std::to_string(transform->worldPose.position.cell.z) : "missing")
				+ "), localization=" + std::string(Scene::ToString(localizedTransform.error))
				+ ", physicalTick=" + (handoffState ? std::to_string(handoffState->tick) : "missing")
				+ "; trace=" + recoveryTrace + " current=" + runtime.DescribePoseState()
				+ " " + runtime.DescribePhysicsDiagnostics() + " " + runtime.LastFlushTrace()
				+ " " + runtime.DescribeRagdollResults());
			const auto quietBefore = handoffState;
			runtime.AdvancePhysics();
			const auto quietAfter = runtime.State();
			context.Expect(quietBefore && quietAfter
				&& std::abs(quietAfter->pose.position.x - quietBefore->pose.position.x) < 0.005F
				&& std::abs(quietAfter->pose.position.z - quietBefore->pose.position.z) < 0.005F
				&& std::abs(quietAfter->realizedYawRadians) < 0.001F,
				"An uncovered post-recovery Physics tick must not inherit ragdoll or root motion.");
			const auto reacquired = runtime.Acquire(3U, MotionMode::Velocity);
			context.Expect(reacquired.state == ReceiptState::Applied && reacquired.ownerToken != 0U
				&& reacquired.ownerToken != acquired.ownerToken,
				"Fresh locomotion ownership must be obtainable after recovery (" + DescribeReceipt(reacquired) + ").");
		});

		registry.Add("Engine.Locomotion.RagdollRecovery.FailedActivateNeedsExplicitAbortBeforeReacquire", [](TestContext& context) {
			RagdollRecoveryRuntime runtime(context);
			if(!runtime.Start(false)) {
				context.Expect(false, "The failed-admission fixture must start with a bound character but no ragdoll profile.");
				return;
			}
			const auto acquired = runtime.Acquire(1U, MotionMode::RootMotion);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U,
				"The bound character must acquire locomotion before invalid Activate is submitted.");
			if(acquired.state != ReceiptState::Applied) return;
			const auto failedActivate = runtime.RequestRagdoll(
				NativeAnimationRagdollOperation::Activate, 9401U, false);
			context.Expect(failedActivate.status == NativeAnimationStatus::Queued,
				"The malformed-but-well-formed profile identity must queue to the production admission path.");
			runtime.ReconcileLocomotion();
			const auto heldExternal = runtime.Physics().ReadCharacterMotionLease(runtime.Controller());
			context.Expect(heldExternal && heldExternal->state == CharacterMotionLeaseState::ExternalExclusive,
				"Pending failed admission must hold the external lease before provider validation completes.");
			context.Expect(runtime.AdvanceAnimation(), "The dummy profile must be rejected at the Animation protected boundary.");
			const auto failureSnapshot = runtime.RagdollSnapshot();
			const auto stillHeld = runtime.Physics().ReadCharacterMotionLease(runtime.Controller());
			context.Expect(failureSnapshot && failureSnapshot->status != NativeAnimationStatus::Ok
				&& stillHeld && stillHeld->state == CharacterMotionLeaseState::ExternalExclusive,
				"A failed Activate must preserve ExternalExclusive until an explicit abort request.");
			context.Expect(runtime.RebindToIdle() == AnimationStatus::NotReady,
				"Failed-admission external ownership must also block hot-rebinding the old runtime context.");

			const auto abort = runtime.RequestRagdoll(
				NativeAnimationRagdollOperation::Deactivate, 9402U, false);
			context.Expect(abort.status == NativeAnimationStatus::Queued,
				"An explicit Deactivate must be accepted as the caller-authorized failed-admission abort.");
			context.Expect(runtime.AdvanceAnimation(), "The explicit abort must run at Animation's protected boundary.");
			const auto released = runtime.Physics().ReadCharacterMotionLease(runtime.Controller());
			context.Expect(released && released->state == CharacterMotionLeaseState::Unleased,
				"Only the explicit abort may release a failed admission's external character lease.");
			context.Expect(runtime.RebindToIdle() == AnimationStatus::Ok,
				"A valid immutable rebind may proceed after the explicit failed-admission abort.");
			runtime.ReconcileLocomotion();
			runtime.AdvancePhysics();
			const auto quiet = runtime.State();
			context.Expect(quiet.has_value()
				&& std::abs(quiet->realizedDisplacement.x) < 0.005F
				&& std::abs(quiet->realizedDisplacement.z) < 0.005F,
				"Aborting a failed Activate must not leave delayed physical movement behind.");
			const auto staleOwner = runtime.Submit(CommandKind::Stop, 2U,
				acquired.authorityEpoch, acquired.ownerToken, MotionMode::RootMotion);
			context.Expect(staleOwner.state != ReceiptState::Applied,
				"The token from before the failed admission cannot write after the explicit abort.");
			const auto reacquired = runtime.Acquire(3U, MotionMode::Velocity);
			context.Expect(reacquired.state == ReceiptState::Applied && reacquired.ownerToken != 0U
				&& reacquired.ownerToken != acquired.ownerToken,
				"Fresh Acquire after explicit abort must apply (" + DescribeReceipt(reacquired) + ").");
		});

		registry.Add("Engine.Locomotion.RagdollRecovery.AnimatorRemovalTearsDownAndReleasesExternalOwner", [](TestContext& context) {
			RagdollRecoveryRuntime runtime(context);
			if(!runtime.Start(true)) {
				context.Expect(false, "The animator-removal fixture must start with a compatible bound ragdoll profile.");
				return;
			}
			const auto acquired = runtime.Acquire(1U, MotionMode::RootMotion);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U,
				"The live animator's character must have an explicit locomotion owner before ragdoll activation.");
			if(acquired.state != ReceiptState::Applied) return;

			const auto activate = runtime.RequestRagdoll(
				NativeAnimationRagdollOperation::Activate, 9501U, true);
			context.Expect(activate.status == NativeAnimationStatus::Queued,
				"The valid ragdoll activation request must enter the protected Animation path.");
			runtime.ReconcileLocomotion();
			context.Expect(runtime.AdvanceAnimation(), "Animation must consume the valid activation request.");
			runtime.AdvancePhysics();
			context.Expect(runtime.LatestLifecycleResult() && runtime.LatestLifecycleResult()->status == AnimationStatus::Ok,
				"Physics must have committed the active ragdoll before animator removal begins.");
			context.Expect(runtime.AdvanceAnimation(), "Animation must observe the committed active ragdoll before removal.");
			const auto controller = runtime.Controller();
			const auto activeLease = runtime.Physics().ReadCharacterMotionLease(controller);
			const auto before = runtime.State();
			context.Expect(activeLease && activeLease->state == CharacterMotionLeaseState::ExternalExclusive
				&& before.has_value(),
				"The active ragdoll must hold external ownership and expose a current provider controller pose.");
			if(!activeLease || activeLease->state != CharacterMotionLeaseState::ExternalExclusive || !before) return;

			Scene::SceneMutationQueue removal;
			Scene::SceneMutationRequest removeAnimator;
			removeAnimator.requestId = "remove-ragdoll-owned-animator";
			removeAnimator.operation = Scene::SceneMutationOperation::RemoveComponent;
			removeAnimator.targetObjectId = ObjectManager::instance().getUUID(runtime.Owner());
			removeAnimator.componentName = std::string(AnimatorComponent::kComponentName);
			removeAnimator.componentPayloadKind = Scene::SceneComponentMutationPayloadKind::Remove;
			removal.Enqueue(std::move(removeAnimator));
			const auto removed = removal.Apply(ObjectManager::instance());
			context.Expect(removed.applied
				&& ObjectManager::instance().GetComponent<AnimatorComponent>(runtime.Owner()) == nullptr,
				"Animator removal must commit transactionally before retirement is observed.");
			if(!removed.applied) return;
			const std::string removalFlushTrace = runtime.DescribeFlush(removed);
			runtime.Physics().OnAfterPhysicsFlush(removed.transactionResults, removed.publicationGroupResults);

			context.Expect(runtime.AdvanceAnimation(), "Animation must observe the removal and queue owned ragdoll teardown.");
			context.Expect(!runtime.Animation().ResolveAnimatorForObject(
				ObjectManager::instance().getUUID(runtime.Owner())).IsValid(),
				"Removing the component must retire the old animator handle.");
			const bool animatorStillAliveAfterRemoval = runtime.IsAnimatorAlive();
			const auto stillExternal = runtime.Physics().ReadCharacterMotionLease(controller);
			context.Expect(stillExternal && stillExternal->state == CharacterMotionLeaseState::ExternalExclusive,
				"The external lease must remain held until Physics accepts the tagged teardown and pose handoff.");
			runtime.AdvancePhysics();
			const auto& retainedResults = runtime.RetainedRagdollResults();
			const std::uint64_t retiredRagdollId = (static_cast<std::uint64_t>(runtime.Animator().generation) << 32U)
				| (static_cast<std::uint64_t>(runtime.Animator().slotIndex) + 1U);
			const auto teardown = std::find_if(retainedResults.rbegin(), retainedResults.rend(),
				[retiredRagdollId](const RagdollPhysicsResult& result) {
					return result.ragdollId == retiredRagdollId
						&& result.kind == RagdollPhysicsRequestKind::Teardown;
				});
			context.Expect(teardown != retainedResults.rend() && teardown->status == AnimationStatus::Ok,
				"Physics teardown result must succeed; " + removalFlushTrace + "; "
				+ runtime.LastFlushTrace() + "; " + runtime.DescribePhysicsDiagnostics()
				+ "; animatorAlive=" + std::to_string(animatorStillAliveAfterRemoval)
				+ "; " + runtime.DescribeRagdollResults() + "; retained count=" + std::to_string(retainedResults.size())
				+ (teardown == retainedResults.rend() ? ", teardown=missing"
					: ", status=" + std::to_string(static_cast<unsigned>(teardown->status))
						+ ", correlation=" + std::to_string(teardown->correlationId)
						+ ", diagnostic=" + teardown->diagnostic));
			const auto replacementController = runtime.Controller();
			context.Expect(replacementController.world.IsValid() && replacementController.character.IsValid()
				&& replacementController != controller,
				"Removing an animator component must refresh the full controller target generation at Physics.");
			context.Expect(!runtime.Physics().ReadCharacterMotionLease(controller),
				"The pre-removal controller target must no longer be live after component-generation replacement.");
			const auto released = runtime.Physics().ReadCharacterMotionLease(replacementController);
			context.Expect(released && released->state == CharacterMotionLeaseState::Unleased,
				"The exact external owner must release only after the provider pose handoff succeeds.");
			const auto after = runtime.State();
			context.Expect(after.has_value()
				&& std::abs(after->pose.position.x - before->pose.position.x) < 0.02F
				&& std::abs(after->pose.position.y - before->pose.position.y) < 0.02F
				&& std::abs(after->pose.position.z - before->pose.position.z) < 0.02F,
				"The controller must retain the current provider pose rather than jump back to its old locomotion pose.");

			const auto oldToken = runtime.Submit(CommandKind::Stop, 2U,
				acquired.authorityEpoch, acquired.ownerToken, MotionMode::RootMotion);
			context.Expect(oldToken.state != ReceiptState::Applied,
				"The pre-removal locomotion token cannot regain authority after external teardown.");
			const auto reacquired = runtime.Acquire(3U, MotionMode::Velocity);
			context.Expect(reacquired.state == ReceiptState::Applied && reacquired.ownerToken != 0U
				&& reacquired.ownerToken != acquired.ownerToken,
				"Fresh Acquire after active animator retirement must apply (" + DescribeReceipt(reacquired) + ").");
		});

		registry.Add("Engine.Locomotion.RagdollRecovery.PendingActivateRemovalDrainsQueuedProviderWork", [](TestContext& context) {
			RagdollRecoveryRuntime runtime(context);
			if(!runtime.Start(true)) {
				context.Expect(false, "The pending-retirement fixture must start with a valid profile and bound animator.");
				return;
			}
			const auto acquired = runtime.Acquire(1U, MotionMode::RootMotion);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U,
				"The controller must have an explicit locomotion owner before pending ragdoll admission.");
			if(acquired.state != ReceiptState::Applied) return;

			const auto activate = runtime.RequestRagdoll(
				NativeAnimationRagdollOperation::Activate, 9601U, true);
			context.Expect(activate.status == NativeAnimationStatus::Queued,
				"A valid Activate must queue before provider work has reached its Physics boundary.");
			runtime.ReconcileLocomotion();
			const auto controller = runtime.Controller();
			const auto pendingLease = runtime.Physics().ReadCharacterMotionLease(controller);
			context.Expect(pendingLease && pendingLease->state == CharacterMotionLeaseState::ExternalExclusive,
				"The pending request must already exclude locomotion while the provider has not activated it.");
			if(!pendingLease || pendingLease->state != CharacterMotionLeaseState::ExternalExclusive) return;
			const auto before = runtime.State();
			const std::uint64_t ragdollId = (static_cast<std::uint64_t>(runtime.Animator().generation) << 32U)
				| (static_cast<std::uint64_t>(runtime.Animator().slotIndex) + 1U);

			// Remove the component before Animation advances the queued Activate. Its
			// retirement path must drain both admitted work and the owner lease safely.
			Scene::SceneMutationQueue removal;
			Scene::SceneMutationRequest removeAnimator;
			removeAnimator.requestId = "remove-pending-ragdoll-animator";
			removeAnimator.operation = Scene::SceneMutationOperation::RemoveComponent;
			removeAnimator.targetObjectId = ObjectManager::instance().getUUID(runtime.Owner());
			removeAnimator.componentName = std::string(AnimatorComponent::kComponentName);
			removeAnimator.componentPayloadKind = Scene::SceneComponentMutationPayloadKind::Remove;
			removal.Enqueue(std::move(removeAnimator));
			const auto removed = removal.Apply(ObjectManager::instance());
			context.Expect(removed.applied
				&& ObjectManager::instance().GetComponent<AnimatorComponent>(runtime.Owner()) == nullptr,
				"The pending animator must be removed through a committed scene transaction.");
			if(!removed.applied) return;
			runtime.Physics().OnAfterPhysicsFlush(removed.transactionResults, removed.publicationGroupResults);
			context.Expect(runtime.AdvanceAnimation(),
				"Animation must retire the removed component and enqueue teardown before provider activation.");
			context.Expect(!runtime.Animation().ResolveAnimatorForObject(
				ObjectManager::instance().getUUID(runtime.Owner())).IsValid(),
				"The animator handle must become stale as part of component retirement.");
			runtime.AdvancePhysics();
			const auto teardown = runtime.LatestLifecycleResult(RagdollPhysicsRequestKind::Teardown);
			context.Expect(teardown && teardown->status == AnimationStatus::Ok,
				"Physics must drain pending activation work and commit the exact retirement teardown.");
			const auto replacementController = runtime.Controller();
			context.Expect(replacementController.world.IsValid() && replacementController.character.IsValid()
				&& replacementController != controller,
				"Pending retirement must refresh the full controller target after component removal.");
			context.Expect(!runtime.Physics().ReadCharacterMotionLease(controller),
				"The pre-removal target must be stale once the component-generation replacement commits.");
			const auto released = runtime.Physics().ReadCharacterMotionLease(replacementController);
			context.Expect(released && released->state == CharacterMotionLeaseState::Unleased,
				"The external lease must be released only after pending provider work is drained safely.");
			if(teardown) {
				context.Expect(runtime.Physics().LatestRagdollPhysicsPose(
					ragdollId, teardown->generation) == nullptr,
					"The drained pending Activate must not leave an active immutable Physics pose after teardown.");
			}
			const auto after = runtime.State();
			context.Expect(before && after
				&& std::abs(after->pose.position.x - before->pose.position.x) < 0.02F
				&& std::abs(after->pose.position.y - before->pose.position.y) < 0.02F
				&& std::abs(after->pose.position.z - before->pose.position.z) < 0.02F,
				"Pending retirement must hand off the current controller pose without a stale teleport.");
			const auto reacquired = runtime.Acquire(2U, MotionMode::Velocity);
			context.Expect(reacquired.state == ReceiptState::Applied && reacquired.ownerToken != 0U
				&& reacquired.ownerToken != acquired.ownerToken,
				"Fresh Acquire after pending teardown must apply (" + DescribeReceipt(reacquired) + ").");
		});

		registry.Add("Engine.Locomotion.RagdollRecovery.RetirementSupersedesQueuedPhysicsActivation", [](TestContext& context) {
			RagdollRecoveryRuntime runtime(context);
			if(!runtime.Start(true)) {
				context.Expect(false, "The queued-activation retirement case needs a compatible bound animator/profile.");
				return;
			}
			const auto acquired = runtime.Acquire(1U, MotionMode::RootMotion);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U,
				"A locomotion lease must be established before queuing the native ragdoll activation.");
			if(acquired.state != ReceiptState::Applied) return;

			constexpr std::uint64_t activateCorrelation = 9701U;
			const auto activate = runtime.RequestRagdoll(
				NativeAnimationRagdollOperation::Activate, activateCorrelation, true);
			context.Expect(activate.status == NativeAnimationStatus::Queued,
				"The valid activation request must be admitted before component retirement.");
			runtime.ReconcileLocomotion();
			const auto controller = runtime.Controller();
			const auto external = runtime.Physics().ReadCharacterMotionLease(controller);
			context.Expect(external && external->state == CharacterMotionLeaseState::ExternalExclusive,
				"The pending activation must reserve the external owner before Animation schedules provider work.");
			if(!external || external->state != CharacterMotionLeaseState::ExternalExclusive) return;
			const auto before = runtime.State();
			const auto animator = runtime.Animator();
			const std::uint64_t ragdollId = (static_cast<std::uint64_t>(animator.generation) << 32U)
				| (static_cast<std::uint64_t>(animator.slotIndex) + 1U);

			// Animation submits Create/Activate to Physics, but Physics is deliberately
			// not advanced yet. Retirement must supersede those exact queued commands.
			context.Expect(runtime.AdvanceAnimation(), "Animation must enqueue typed provider Create/Activate work.");
			const auto animationQueuedLease = runtime.Physics().ReadCharacterMotionLease(controller);
			context.Expect(animationQueuedLease
				&& animationQueuedLease->state == CharacterMotionLeaseState::ExternalExclusive,
				"Queued provider work must not release the external exclusion before Physics commits it.");
			Scene::SceneMutationQueue removal;
			Scene::SceneMutationRequest removeAnimator;
			removeAnimator.requestId = "remove-animator-with-queued-provider-activation";
			removeAnimator.operation = Scene::SceneMutationOperation::RemoveComponent;
			removeAnimator.targetObjectId = ObjectManager::instance().getUUID(runtime.Owner());
			removeAnimator.componentName = std::string(AnimatorComponent::kComponentName);
			removeAnimator.componentPayloadKind = Scene::SceneComponentMutationPayloadKind::Remove;
			removal.Enqueue(std::move(removeAnimator));
			const auto removed = removal.Apply(ObjectManager::instance());
			context.Expect(removed.applied
				&& ObjectManager::instance().GetComponent<AnimatorComponent>(runtime.Owner()) == nullptr,
				"The AnimatorComponent must be transactionally removed while Physics work is still queued.");
			if(!removed.applied) return;
			runtime.Physics().OnAfterPhysicsFlush(removed.transactionResults, removed.publicationGroupResults);
			context.Expect(runtime.AdvanceAnimation(),
				"Animation retirement must cancel or supersede queued provider work and enqueue teardown.");
			context.Expect(!runtime.Animation().ResolveAnimatorForObject(
				ObjectManager::instance().getUUID(runtime.Owner())).IsValid(),
				"The retired animator handle must be stale before Physics drains its queue.");
			runtime.AdvancePhysics();

			const auto& results = runtime.RetainedRagdollResults();
			const auto cancelledCreateCorrelation = activateCorrelation ^ 0xA11A000000000000ULL;
			const auto cancelledCreate = std::find_if(results.begin(), results.end(),
				[&](const RagdollPhysicsResult& result) {
					return result.ragdollId == ragdollId
						&& result.kind == RagdollPhysicsRequestKind::Create
						&& result.correlationId == cancelledCreateCorrelation
						&& result.status == AnimationStatus::Cancelled;
				});
			const auto cancelledActivate = std::find_if(results.begin(), results.end(),
				[&](const RagdollPhysicsResult& result) {
					return result.ragdollId == ragdollId
						&& result.kind == RagdollPhysicsRequestKind::Activate
						&& result.correlationId == activateCorrelation
						&& result.status == AnimationStatus::Cancelled;
				});
			const auto teardown = std::find_if(results.rbegin(), results.rend(),
				[ragdollId](const RagdollPhysicsResult& result) {
					return result.ragdollId == ragdollId
						&& result.kind == RagdollPhysicsRequestKind::Teardown
						&& result.status == AnimationStatus::Ok;
				});
			context.Expect(cancelledCreate != results.end() && cancelledActivate != results.end(),
				"Retirement must publish terminal cancellation for the queued Create and Activate identities.");
			context.Expect(teardown != results.rend(),
				"Physics must commit the tagged teardown after cancelling queued provider work.");
			const auto replacementController = runtime.Controller();
			context.Expect(replacementController.world.IsValid() && replacementController.character.IsValid()
				&& replacementController != controller,
				"Animator removal must refresh the full character target generation.");
			context.Expect(!runtime.Physics().ReadCharacterMotionLease(controller),
				"The old controller target must be stale after replacement.");
			const auto released = runtime.Physics().ReadCharacterMotionLease(replacementController);
			context.Expect(released && released->state == CharacterMotionLeaseState::Unleased,
				"The new controller target must be unleased after teardown handoff.");
			if(teardown != results.rend()) {
				context.Expect(runtime.Physics().LatestRagdollPhysicsPose(ragdollId, teardown->generation) == nullptr,
					"Successful teardown must leave no active Physics pose for the retired ragdoll generation.");
			}
			const auto after = runtime.State();
			context.Expect(before && after
				&& std::abs(after->pose.position.x - before->pose.position.x) < 0.02F
				&& std::abs(after->pose.position.y - before->pose.position.y) < 0.02F
				&& std::abs(after->pose.position.z - before->pose.position.z) < 0.02F,
				"The queued-work cancellation path must preserve the controller's current pose.");
			runtime.AdvancePhysics();
			const auto stillUnleased = runtime.Physics().ReadCharacterMotionLease(runtime.Controller());
			context.Expect(stillUnleased && stillUnleased->state == CharacterMotionLeaseState::Unleased,
				"A quiet Physics tick must not resurrect cancelled provider work or an external lease.");
			const auto reacquired = runtime.Acquire(2U, MotionMode::Velocity);
			context.Expect(reacquired.state == ReceiptState::Applied && reacquired.ownerToken != 0U
				&& reacquired.ownerToken != acquired.ownerToken,
				"Fresh Acquire after draining queued activation must apply (" + DescribeReceipt(reacquired) + ").");
		});
	}
}
