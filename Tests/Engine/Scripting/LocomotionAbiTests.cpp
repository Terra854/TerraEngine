// LocomotionAbiTests.cpp - frozen copied-record bridge and gameplay semantics.

#include "../TestHarness.h"

#include "Scripting/ScriptGlue.h"
#include "Components/CharacterLocomotionComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Locomotion/LocomotionCommands.h"
#include "Locomotion/LocomotionSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Locomotion;
		using namespace TerraEngine::Physics;
		constexpr float kStep = 1.0F / 60.0F;

		template<typename T>
		void SetHeader(T& record, NativeLocomotionRecordKind kind)
		{
			record.header = {kNativeLocomotionAbiVersion, static_cast<std::uint32_t>(sizeof(T)), kind, 0U, 0U};
		}

		NativePhysicsUuid ToNativeUuid(const UUID& value)
		{
			NativePhysicsUuid result{};
			std::memcpy(&result.first, value.data(), sizeof(result.first));
			std::memcpy(&result.second, value.data() + sizeof(result.first), sizeof(result.second));
			return result;
		}

		class LocomotionAbiRuntime final {
		public:
			LocomotionAbiRuntime()
			{
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				ObjectManager::instance().clear();
				Scene::SceneMutationQueue::Instance().Clear();
				locomotion_.Shutdown();
				physics_.Shutdown();
				physics_.SetEditorLifecycleControlled(true);
				physics_.Initialize();
				owner_ = ObjectManager::instance().createObject("Locomotion ABI owner");
				(void)ObjectManager::instance().AddComponent<Transform>(owner_);
				controller_ = ObjectManager::instance().AddComponent<CharacterControllerComponent>(owner_);
				movement_ = ObjectManager::instance().AddComponent<CharacterLocomotionComponent>(owner_);
				if(controller_) {
					controller_->radius = 0.35F;
					controller_->height = 1.8F;
					controller_->localCenter = {0.0F, 0.9F, 0.0F};
					controller_->useGravity = false;
				}
				started_ = controller_ && movement_ && physics_.StartWorld();
				for(unsigned frame = 0U; started_ && frame < 4U; ++frame) AdvancePhysics();
				if(started_) {
					locomotion_.Initialize();
					locomotion_.ObserveProtectedBoundary();
					locomotion_.Update(kStep);
				}
			}

			~LocomotionAbiRuntime()
			{
				locomotion_.Shutdown();
				(void)physics_.StopWorld();
				physics_.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				ThreadPool::instance().shutdown();
			}

			void AdvancePhysics()
			{
				auto& objects = ObjectManager::instance();
				(void)objects.PublishSceneChangePacket();
				physics_.Update(kStep);
				const auto flushed = Scene::SceneMutationQueue::Instance().Apply(objects,
					"locomotion-abi-physics-flush");
				physics_.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			}

			NativeLocomotionResolveResult Resolve() const
			{
				NativeLocomotionResolveRequest request{};
				SetHeader(request, NativeLocomotionRecordKind::ResolveRequest);
				request.objectId = ToNativeUuid(ObjectManager::instance().getUUID(owner_));
				NativeLocomotionResolveResult result{};
				SetHeader(result, NativeLocomotionRecordKind::ResolveResult);
				(void)Native_Locomotion_Resolve(&request, &result);
				return result;
			}

			bool IsStarted() const noexcept { return started_; }
			Object Owner() const noexcept { return owner_; }
			CharacterLocomotionComponent* Movement() const noexcept { return movement_; }
			LocomotionSystem& Locomotion() noexcept { return locomotion_; }

		private:
			PhysicsSystem& physics_ = PhysicsSystem::Instance();
			LocomotionSystem& locomotion_ = LocomotionSystem::Instance();
			Object owner_{};
			CharacterControllerComponent* controller_ = nullptr;
			CharacterLocomotionComponent* movement_ = nullptr;
			bool started_ = false;
		};

		NativeLocomotionCommand MakeAcquire(const NativeLocomotionHandle& handle,
			std::uint64_t configurationRevision, std::uint64_t authorityEpoch,
			std::uint64_t producer, std::uint64_t sequence, std::uint64_t correlation)
		{
			NativeLocomotionCommand command{};
			SetHeader(command, NativeLocomotionRecordKind::Command);
			command.handle = handle;
			command.kind = NativeLocomotionCommandKind::Acquire;
			command.intentKind = NativeLocomotionIntentKind::Script;
			command.directionSpace = NativeLocomotionDirectionSpace::World;
			command.lifetime = NativeLocomotionIntentLifetime::UntilStopped;
			command.mode = NativeLocomotionMotionMode::Velocity;
			command.expectedAuthorityEpoch = authorityEpoch;
			command.expectedConfigurationRevision = configurationRevision;
			command.producerId = producer;
			command.sequence = sequence;
			command.correlationId = correlation;
			return command;
		}

		NativeLocomotionStatus SubmitOne(const NativeLocomotionCommand& command,
			NativeLocomotionReceipt& receipt, NativeLocomotionBufferInfo& info)
		{
			NativeLocomotionCommandBatch batch{};
			SetHeader(batch, NativeLocomotionRecordKind::CommandBatch);
			batch.commandCount = 1U;
			SetHeader(receipt, NativeLocomotionRecordKind::Receipt);
			SetHeader(info, NativeLocomotionRecordKind::BufferInfo);
			return Native_Locomotion_Submit(&batch, &command, &receipt, 1U, &info);
		}
	}

	void RegisterLocomotionAbiTests(TestRegistry& registry)
	{
		registry.Add("Engine.Locomotion.Interop.V1LayoutsAndWireEnumsAreFrozen", [](TestContext& context) {
			context.ExpectEqual(1U, Native_Locomotion_GetAbiVersion(),
				"The copied locomotion surface must advertise its independent ABI v1.");
			context.Expect(sizeof(NativeLocomotionAbiHeader) == 16U
				&& alignof(NativeLocomotionAbiHeader) == 4U
				&& alignof(NativeLocomotionHandle) == 8U
				&& alignof(NativeLocomotionCommand) == 8U
				&& alignof(NativeLocomotionObservation) == 8U
				&& alignof(NativeLocomotionBufferInfo) == 8U
				&& sizeof(NativeLocomotionHandle) == 64U
				&& sizeof(NativeLocomotionResolveRequest) == 32U
				&& sizeof(NativeLocomotionResolveResult) == 104U
				&& sizeof(NativeLocomotionCommandBatch) == 24U
				&& sizeof(NativeLocomotionCommand) == 192U
				&& sizeof(NativeLocomotionReceiptQuery) == 104U
				&& sizeof(NativeLocomotionReceipt) == 136U
				&& sizeof(NativeLocomotionObservationQuery) == 80U
				&& sizeof(NativeLocomotionObservation) == 360U
				&& sizeof(NativeLocomotionBufferInfo) == 40U,
				"Every v1 copied record must retain its approved x64 stride and alignment.");
			context.Expect(offsetof(NativeLocomotionResolveRequest, header) == 0U
				&& offsetof(NativeLocomotionHandle, objectId) == 24U
				&& offsetof(NativeLocomotionCommand, handle) == 16U
				&& offsetof(NativeLocomotionCommand, directionX) == 152U
				&& offsetof(NativeLocomotionReceipt, state) == 104U
				&& offsetof(NativeLocomotionObservation, desiredVelocityX) == 208U
				&& offsetof(NativeLocomotionObservation, realizedYawRadians) == 352U
				&& offsetof(NativeLocomotionBufferInfo, publicationEpoch) == 32U,
				"Critical copied-field boundaries must agree with the managed x64 ABI offsets.");
			context.Expect(static_cast<std::uint16_t>(NativeLocomotionRecordKind::Handle) == 1U
				&& static_cast<std::uint16_t>(NativeLocomotionRecordKind::BufferInfo) == 10U
				&& static_cast<std::uint32_t>(NativeLocomotionCommandKind::Acquire) == 0U
				&& static_cast<std::uint32_t>(NativeLocomotionCommandKind::Release) == 6U
				&& static_cast<std::uint32_t>(NativeLocomotionReceiptState::Queued) == 0U
				&& static_cast<std::uint32_t>(NativeLocomotionReceiptState::Expired) == 4U
				&& static_cast<std::uint32_t>(NativeLocomotionIntentKind::Player) == 0U
				&& static_cast<std::uint32_t>(NativeLocomotionIntentKind::Navigation) == 2U,
				"Record, command, receipt and intent enums are stable fixed-width wire values.");
			context.ExpectEqual(kNativeNavigationAbiVersion, Native_Navigation_GetAbiVersion(),
				"Introducing locomotion ABI v1 must not revise the existing direct Navigation ABI.");
			context.ExpectEqual(1U, Native_NavigationAgent_GetAbiVersion(),
				"Locomotion ABI v1 remains independently versioned from the optional NavigationAgent v1 bridge.");
		});

		registry.Add("Engine.Locomotion.Interop.MalformedRecordsRejectWithoutPartialMutation", [](TestContext& context) {
			NativeLocomotionResolveRequest request{};
			SetHeader(request, NativeLocomotionRecordKind::ResolveRequest);
			NativeLocomotionResolveResult result{};
			SetHeader(result, NativeLocomotionRecordKind::ResolveResult);
			result.configurationRevision = 0xabcU;
			request.header.abiVersion++;
			context.Expect(Native_Locomotion_Resolve(&request, &result) == NativeLocomotionStatus::VersionMismatch
				&& result.configurationRevision == 0U && result.handle.slotGeneration == 0U,
				"An unsupported resolve version must fail before publishing a handle or configuration snapshot.");

			SetHeader(request, NativeLocomotionRecordKind::ResolveRequest);
			request.header.recordKind = NativeLocomotionRecordKind::ObservationQuery;
			result = {};
			SetHeader(result, NativeLocomotionRecordKind::ResolveResult);
			context.Expect(Native_Locomotion_Resolve(&request, &result) == NativeLocomotionStatus::VersionMismatch,
				"A correctly sized record with the wrong kind must be rejected at the ABI boundary.");

			SetHeader(request, NativeLocomotionRecordKind::ResolveRequest);
			request.header.reserved1 = 1U;
			context.Expect(Native_Locomotion_Resolve(&request, &result) == NativeLocomotionStatus::VersionMismatch,
				"Nonzero reserved envelope fields must be rejected rather than treated as future flags.");

			NativeLocomotionCommandBatch batch{};
			SetHeader(batch, NativeLocomotionRecordKind::CommandBatch);
			batch.commandCount = 1U;
			NativeLocomotionCommand command{};
			SetHeader(command, NativeLocomotionRecordKind::Command);
			SetHeader(command.handle, NativeLocomotionRecordKind::Handle);
			command.kind = static_cast<NativeLocomotionCommandKind>(99U);
			NativeLocomotionReceipt receipt{};
			SetHeader(receipt, NativeLocomotionRecordKind::Receipt);
			NativeLocomotionBufferInfo info{};
			SetHeader(info, NativeLocomotionRecordKind::BufferInfo);
			context.Expect(Native_Locomotion_Submit(&batch, &command, &receipt, 1U, &info)
				== NativeLocomotionStatus::InvalidArgument && info.writtenCount == 0U
				&& receipt.state == NativeLocomotionReceiptState::Queued
				&& receipt.correlationId == 0U,
				"Unknown command kinds must reject atomically without a receipt for any batch member.");

			command.kind = NativeLocomotionCommandKind::SetIntent;
			command.directionX = std::numeric_limits<double>::quiet_NaN();
			info = {};
			SetHeader(info, NativeLocomotionRecordKind::BufferInfo);
			context.Expect(Native_Locomotion_Submit(&batch, &command, &receipt, 1U, &info)
				== NativeLocomotionStatus::InvalidArgument && info.writtenCount == 0U,
				"Nonfinite command coordinates must reject before any member enters the queue.");

			command.directionX = 0.0;
			command.handle.header.reserved0 = 1U;
			info = {};
			SetHeader(info, NativeLocomotionRecordKind::BufferInfo);
			context.Expect(Native_Locomotion_Submit(&batch, &command, &receipt, 1U, &info)
				== NativeLocomotionStatus::VersionMismatch && info.writtenCount == 0U,
				"A malformed nested identity envelope must reject the complete command batch.");
		});

		registry.Add("Engine.Locomotion.Interop.UuidBridgeAndResolvePinTheExactAttachment", [](TestContext& context) {
			NativePhysicsUuid untouched{0x1122334455667788ULL, 0x8877665544332211ULL};
			const NativePhysicsUuid original = untouched;
			context.Expect(Native_Object_ReadUuid(0U, nullptr) == NativeRuntimeStatus::InvalidArgument,
				"The copied UUID bridge must reject a null output pointer.");
			context.Expect(Native_Object_ReadUuid(std::numeric_limits<std::uint32_t>::max(), &untouched)
				!= NativeRuntimeStatus::Success && untouched.first == original.first
				&& untouched.second == original.second,
				"An invalid entity lookup must preserve the caller's UUID bytes on failure.");

			LocomotionAbiRuntime runtime;
			context.Expect(runtime.IsStarted(), "The ABI integration fixture needs a live physical locomotion attachment.");
			if(!runtime.IsStarted()) return;
			const NativePhysicsUuid expected = ToNativeUuid(ObjectManager::instance().getUUID(runtime.Owner()));
			NativePhysicsUuid copied{};
			context.Expect(Native_Object_ReadUuid(runtime.Owner().entityValue, &copied) == NativeRuntimeStatus::Success
				&& copied.first == expected.first && copied.second == expected.second,
				"The UUID bridge must return the exact stable UUID for a live entity ID.");
			NativeLocomotionResolveResult resolved = runtime.Resolve();
			context.Expect(resolved.status == NativeLocomotionStatus::Success
				&& resolved.handle.objectId.first == expected.first
				&& resolved.handle.objectId.second == expected.second
				&& resolved.configurationRevision != 0U && resolved.handle.slotGeneration != 0U,
				"Resolve must return the current full attachment identity and the revisions needed for its first command.");
			if(resolved.status != NativeLocomotionStatus::Success) return;

			NativeLocomotionObservationQuery query{};
			SetHeader(query, NativeLocomotionRecordKind::ObservationQuery);
			query.handle = resolved.handle;
			NativeLocomotionObservation sentinel{};
			SetHeader(sentinel, NativeLocomotionRecordKind::Observation);
			sentinel.observedPlanarSpeed = 1234.0;
			NativeLocomotionBufferInfo info{};
			SetHeader(info, NativeLocomotionRecordKind::BufferInfo);
			context.Expect(Native_Locomotion_CopyObservations(&query, nullptr, 0U, &info)
				== NativeLocomotionStatus::Success && info.requiredCount == 1U
				&& info.writtenCount == 0U && info.publicationEpoch != 0U,
				"A null/zero-capacity probe must report one copied owner observation without writing a partial row.");
			NativeLocomotionBufferInfo shortInfo{};
			SetHeader(shortInfo, NativeLocomotionRecordKind::BufferInfo);
			context.Expect(Native_Locomotion_CopyObservations(&query, &sentinel, 0U, &shortInfo)
				== NativeLocomotionStatus::BufferTooSmall && shortInfo.requiredCount == 1U
				&& shortInfo.writtenCount == 0U && sentinel.observedPlanarSpeed == 1234.0,
				"An insufficient observation buffer must report the required count and leave every row untouched.");
			std::array<NativeLocomotionObservation, 1U> observations{};
			SetHeader(observations[0], NativeLocomotionRecordKind::Observation);
			NativeLocomotionBufferInfo copiedInfo{};
			SetHeader(copiedInfo, NativeLocomotionRecordKind::BufferInfo);
			context.Expect(Native_Locomotion_CopyObservations(&query, observations.data(), 1U, &copiedInfo)
				== NativeLocomotionStatus::Success && copiedInfo.writtenCount == 1U
				&& copiedInfo.publicationEpoch == info.publicationEpoch
				&& observations[0].handle.slotGeneration == resolved.handle.slotGeneration
				&& observations[0].physicalTick != 0U,
				"A successful copy must return one full identity-consistent row from the same immutable publication root.");
			NativeLocomotionObservationQuery staleQuery = query;
			staleQuery.handle.slotGeneration++;
			NativeLocomotionObservation staleSentinel{};
			SetHeader(staleSentinel, NativeLocomotionRecordKind::Observation);
			staleSentinel.observedPlanarSpeed = 4321.0;
			NativeLocomotionBufferInfo staleInfo{};
			SetHeader(staleInfo, NativeLocomotionRecordKind::BufferInfo);
			context.Expect(Native_Locomotion_CopyObservations(&staleQuery, &staleSentinel, 1U, &staleInfo)
				== NativeLocomotionStatus::StaleHandle && staleInfo.writtenCount == 0U
				&& staleSentinel.observedPlanarSpeed == 4321.0,
				"A generation-mismatched wrapper must be stale and cannot partially overwrite a copied observation.");
		});

		registry.Add("Engine.Locomotion.Interop.SubmitSeparatesQueuedAndAppliedReceiptsAndRejectsReplayConflict", [](TestContext& context) {
			LocomotionAbiRuntime runtime;
			context.Expect(runtime.IsStarted(), "The ABI command test needs a live physical locomotion attachment.");
			if(!runtime.IsStarted()) return;
			const NativeLocomotionResolveResult resolved = runtime.Resolve();
			context.Expect(resolved.status == NativeLocomotionStatus::Success,
				"The ABI command test must resolve the owner before acquiring motion authority.");
			if(resolved.status != NativeLocomotionStatus::Success) return;

			NativeLocomotionCommand acquire = MakeAcquire(resolved.handle,
				resolved.configurationRevision, resolved.authorityEpoch, 0xA610U, 1U, 0xA61001U);
			NativeLocomotionReceipt queued{};
			NativeLocomotionBufferInfo info{};
			context.Expect(SubmitOne(acquire, queued, info) == NativeLocomotionStatus::Queued
				&& info.requiredCount == 1U && info.writtenCount == 1U
				&& queued.state == NativeLocomotionReceiptState::Queued,
				"Successful command admission must return a queued receipt without pretending gameplay already applied it.");
			if(queued.state != NativeLocomotionReceiptState::Queued) return;

			NativeLocomotionReceiptQuery query{};
			SetHeader(query, NativeLocomotionRecordKind::ReceiptQuery);
			query.handle = resolved.handle;
			query.producerId = acquire.producerId;
			query.sequence = acquire.sequence;
			query.correlationId = acquire.correlationId;
			NativeLocomotionReceipt pending{};
			SetHeader(pending, NativeLocomotionRecordKind::Receipt);
			context.Expect(Native_Locomotion_ReadReceipt(&query, &pending) == NativeLocomotionStatus::Queued
				&& pending.state == NativeLocomotionReceiptState::Queued,
				"The retained receipt query must distinguish admitted work from a later applied update.");

			runtime.Locomotion().Update(kStep);
			NativeLocomotionReceipt applied{};
			SetHeader(applied, NativeLocomotionRecordKind::Receipt);
			context.Expect(Native_Locomotion_ReadReceipt(&query, &applied) == NativeLocomotionStatus::Success
				&& applied.state == NativeLocomotionReceiptState::Applied && applied.ownerToken != 0U
				&& applied.authorityEpoch != 0U,
				"The same full producer/correlation identity must later read back the applied Acquire token and epoch.");
			if(applied.state != NativeLocomotionReceiptState::Applied) return;

			NativeLocomotionReceipt retry{};
			NativeLocomotionBufferInfo retryInfo{};
			const NativeLocomotionStatus retryStatus = SubmitOne(acquire, retry, retryInfo);
			context.Expect((retryStatus == NativeLocomotionStatus::Success
					|| retryStatus == NativeLocomotionStatus::Queued)
				&& retry.state == NativeLocomotionReceiptState::Applied
				&& retry.ownerToken == applied.ownerToken,
				"An exact command retry must return its retained outcome rather than acquire twice.");
			NativeLocomotionCommand conflict = acquire;
			conflict.directionX = 1.0;
			conflict.requestedSpeed = 2.0;
			NativeLocomotionReceipt conflictReceipt{};
			NativeLocomotionBufferInfo conflictInfo{};
			context.Expect(SubmitOne(conflict, conflictReceipt, conflictInfo)
				== NativeLocomotionStatus::SequenceConflict && conflictInfo.writtenCount == 0U,
				"Reusing the acquired producer sequence with a different payload must reject as a conflict.");
		});
	}
}
