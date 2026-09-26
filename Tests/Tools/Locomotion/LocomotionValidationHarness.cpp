// LocomotionValidationHarness.cpp - real-frame, fail-closed locomotion evidence.
//
// Validation owns copied commands and observations only. It never mutates live
// objects or calls a provider directly; Editor Play and exported Engine frames
// run the ordinary scheduler, publication and transactional flush boundaries.

#include "LocomotionValidationHarness.h"

#include "Core/MainEngine.h"
#include "Locomotion/LocomotionSystem.h"
#include "Physics/PhysicsSystem.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace TerraEngine::Locomotion::Validation {
	namespace {
		using Clock = std::chrono::steady_clock;
		constexpr std::string_view kSmokeCase = "intent.manual-world";
		constexpr std::uint32_t kSmokeMotionTicks = 120U;
		constexpr std::uint32_t kSmokeMaximumFrames = 480U;

		struct Options {
			std::string runId;
			std::string executionMode;
			std::string scenario = "smoke";
			std::string caseId = std::string(kSmokeCase);
			UUID smokeObjectId = EMPTY_UUID;
			std::uint32_t requiredWorkerCount = 0U;
			bool qualificationRequested = false;
		};

		enum class Stage : std::uint8_t {
			AwaitBinding, AwaitAcquire, AwaitIntent, ObserveMotion, AwaitStop,
			ObserveStopped, AwaitResume, ObserveResumed, AwaitRelease,
			ObserveReleased, AwaitConflict, AwaitTransfer, AwaitTransferredIntent,
			ObserveTransferred, AwaitFinalStop, Complete
		};

		struct Session {
			Engine* engine = nullptr;
			Options options{};
			Stage stage = Stage::AwaitBinding;
			LocomotionHandle handle{};
			std::uint64_t configurationRevision = 0U;
			std::uint64_t authorityEpoch = 0U;
			std::uint64_t ownerToken = 0U;
			std::uint64_t producerId = 60060U;
			std::uint64_t startPhysicalTick = 0U;
			std::uint64_t phasePhysicalTick = 0U;
			std::uint64_t lastPhysicalTick = 0U;
			std::uint32_t completedFrames = 0U;
			std::uint64_t ingressSequence = 0U;
			std::uint64_t parameterSequence = 0U;
			double initialX = 0.0;
			double initialZ = 0.0;
			double finalX = 0.0;
			double finalZ = 0.0;
			double finalObservedSpeed = 0.0;
			double phaseX = 0.0;
			double settledX = 0.0;
			bool conflictRejected = false;
			bool conflictAttempted = false;
			bool stopSettled = false;
			bool resumed = false;
			bool releaseSettled = false;
			bool transferMoved = false;
			Clock::time_point nextExportFrame{};
			nlohmann::json trace = nlohmann::json::array();
		};

		std::mutex sessionMutex;
		std::optional<Session> activeSession;

		nlohmann::json VectorJson(Physics::PhysicsVector3 vector) {
			return nlohmann::json::array({vector.x, vector.y, vector.z});
		}

		nlohmann::json ControllerJson(const Physics::PhysicsCharacterCommandTarget& target) {
			return {{"worldId", target.world.worldId.value},
				{"worldGeneration", target.world.worldGeneration},
				{"slotIndex", target.character.slotIndex},
				{"slotGeneration", target.character.slotGeneration},
				{"objectId", ToHexString(target.objectId)},
				{"objectGeneration", target.objectGeneration},
				{"componentGeneration", target.componentGeneration}};
		}

		nlohmann::json OwnerJson(const Physics::CharacterMotionOwner& owner) {
			return {{"sceneLifetime", owner.sceneLifetime},
				{"objectId", ToHexString(owner.objectId)},
				{"objectGeneration", owner.objectGeneration},
				{"attachmentGeneration", owner.attachmentGeneration},
				{"slotIndex", owner.slotIndex}, {"slotGeneration", owner.slotGeneration}};
		}

		nlohmann::json LeaseJson(const Physics::CharacterMotionLease& lease) {
			return {{"controller", ControllerJson(lease.controller)},
				{"owner", OwnerJson(lease.owner)},
				{"authorityEpoch", lease.authorityEpoch}, {"motionEpoch", lease.motionEpoch},
				{"publicationEpoch", lease.publicationEpoch}, {"externalOwnerId", lease.externalOwnerId},
				{"state", static_cast<unsigned>(lease.state)}, {"mode", static_cast<unsigned>(lease.mode)}};
		}

		nlohmann::json IngressJson(const Physics::CharacterMotionTraceRecord& record) {
		nlohmann::json result = {{"event", "physical-ingress"}, {"sequence", record.sequence},
			{"precedingTick", record.precedingTick}, {"batch", record.batch},
			{"batchIndex", record.batchIndex}, {"batchCount", record.batchCount},
			{"hadValidityGuard", record.hadValidityGuard}};
		if(const auto* change = std::get_if<Physics::CharacterMotionLeaseChange>(&record.payload)) {
			result["kind"] = "lease";
			result["expected"] = LeaseJson(change->expected);
			result["next"] = LeaseJson(change->next);
		} else if(const auto* target = std::get_if<Physics::CharacterMotionTarget>(&record.payload)) {
			result["kind"] = "target";
			result["controller"] = ControllerJson(target->controller);
			result["owner"] = OwnerJson(target->owner);
			result["authorityEpoch"] = target->authorityEpoch;
			result["motionEpoch"] = target->motionEpoch;
			result["locomotionUpdateEpoch"] = target->locomotionUpdateEpoch;
			result["mode"] = static_cast<unsigned>(target->mode);
			result["frameId"] = target->frame.frameId.value;
			result["frameGeneration"] = target->frame.frameGeneration;
			result["planarVelocity"] = VectorJson(target->planarVelocity);
			result["acceleration"] = target->acceleration;
			result["braking"] = target->braking;
			result["desiredFacing"] = VectorJson(target->desiredFacing);
			result["turnRate"] = target->turnRate;
			result["hasFacing"] = target->hasFacing;
			result["clearImmediately"] = target->clearImmediately;
			result["approachDistanceMetres"] = target->approachDistanceMetres;
		} else if(const auto* interval = std::get_if<Physics::RootMotionInterval>(&record.payload)) {
			result["kind"] = "root-interval";
			result["lease"] = LeaseJson(interval->lease);
			result["frameId"] = interval->frame.frameId.value;
			result["frameGeneration"] = interval->frame.frameGeneration;
			result["animator"] = {{"slotIndex", interval->animator.slotIndex},
				{"animatorGeneration", interval->animator.animatorGeneration},
				{"poseGeneration", interval->animator.poseGeneration},
				{"sourceGeneration", interval->animator.sourceGeneration},
				{"sourceStateId", interval->animator.sourceStateId},
				{"layerIndex", interval->animator.layerIndex},
				{"evaluationSequence", interval->animator.evaluationSequence},
				{"bindingGeneration", interval->animator.bindingGeneration},
					{"publicationGeneration", interval->animator.publicationGeneration},
					{"sourceBeginTick", interval->animator.sourceBeginTick},
					{"sourceEndTick", interval->animator.sourceEndTick},
					{"accumulatedCycles", interval->animator.accumulatedCycles}};
			result["ledger"] = {{"producerId", interval->ledger.producerId},
				{"producerSequence", interval->ledger.producerSequence},
				{"idempotencyKey", interval->ledger.idempotencyKey},
				{"correlationId", interval->ledger.correlationId}};
			result["localTranslation"] = VectorJson(interval->localTranslation);
			result["localYawRadians"] = interval->localYawRadians;
			result["frameTranslation"] = VectorJson(interval->frameTranslation);
			result["frameYawRadians"] = interval->frameYawRadians;
			result["durationSeconds"] = interval->durationSeconds;
			result["maximumSpeed"] = interval->maximumSpeed;
			result["maximumTurnRate"] = interval->maximumTurnRate;
		}
		return result;
		}

		bool CaptureIngress(Session& session, std::string& error) {
			const auto copied = Physics::PhysicsSystem::Instance().CopyCharacterMotionTrace(session.ingressSequence);
			if(!copied.enabled || copied.lostRecords) {
				error = "The bounded physical ingress trace was disabled or lost accepted records.";
				return false;
			}
			for(const auto& record : copied.records) session.trace.push_back(IngressJson(record));
			session.ingressSequence = copied.latestSequence;
			return true;
		}

		nlohmann::json AnimationValueJson(const Animation::AnimationParameterValue& value) {
			return std::visit([](const auto& item) -> nlohmann::json {
				using Item = std::decay_t<decltype(item)>;
				if constexpr(std::is_same_v<Item, std::monostate>) return nullptr;
				else if constexpr(std::is_same_v<Item, bool> || std::is_same_v<Item, std::int32_t>
					|| std::is_same_v<Item, float>) return item;
				else if constexpr(std::is_same_v<Item, Animation::AnimationFloat3>)
					return nlohmann::json::array({item.x, item.y, item.z});
				else if constexpr(std::is_same_v<Item, Animation::AnimationQuaternion>)
					return nlohmann::json::array({item.x, item.y, item.z, item.w});
				else return {{"translation", nlohmann::json::array({item.translation.x,
					item.translation.y, item.translation.z})},
					{"rotation", nlohmann::json::array({item.rotation.x, item.rotation.y,
						item.rotation.z, item.rotation.w})},
					{"scale", nlohmann::json::array({item.scale.x, item.scale.y, item.scale.z})}};
			}, value);
		}

		bool CaptureParameters(Session& session, std::string& error) {
			const auto copied = LocomotionSystem::Instance().CopyParameterTrace(session.parameterSequence);
			if(!copied.enabled || copied.lostRecords) {
				error = "The bounded animation parameter ingress trace was disabled or lost accepted records.";
				return false;
			}
			for(const auto& record : copied.records) {
				nlohmann::json commands = nlohmann::json::array();
				for(std::uint32_t index = 0U; index < record.count; ++index) {
					const auto& command = record.commands[index];
					commands.push_back({{"targetSlot", command.target.slotIndex},
						{"targetGeneration", command.target.generation},
						{"controllerAssetId", ToHexString(command.expectedController.assetId.value)},
						{"controllerGeneration", command.expectedController.generation},
						{"kind", static_cast<unsigned>(command.kind)},
						{"parameterType", static_cast<unsigned>(command.parameterType)},
						{"parameterId", command.parameterId},
						{"valueType", command.value.index()},
						{"value", AnimationValueJson(command.value)},
						{"playbackTick", command.playbackTick},
						{"playbackSpeed", command.playbackSpeed},
						{"playbackFlag", command.playbackFlag},
						{"stateId", command.stateId},
						{"transitionTicks", command.transitionTicks},
						{"layerIndex", command.layerIndex},
						{"layerWeight", command.layerWeight},
						{"targetTick", command.targetTick},
						{"producerId", command.producerId},
						{"producerSequence", command.producerSequence},
						{"correlationId", command.correlationId},
						{"idempotencyKey", command.idempotencyKey},
						{"cancellationToken", command.cancellationToken}});
				}
				session.trace.push_back({{"event", "animation-parameter-ingress"},
					{"sequence", record.sequence}, {"updateEpoch", record.updateEpoch},
					{"precedingTick", record.precedingTick},
					{"animatorObjectId", ToHexString(record.animatorObjectId)},
					{"count", record.count}, {"commands", std::move(commands)}});
			}
			session.parameterSequence = copied.latestSequence;
			return true;
		}

		nlohmann::json Failure(std::string reason, std::uint32_t frames = 0U) {
			return {{"schemaVersion", 1U}, {"pending", false}, {"succeeded", false},
				{"executedCaseCount", 0U}, {"completedFrames", frames}, {"failure", std::move(reason)}};
		}

		std::optional<Options> ParseOptions(const nlohmann::json& source, std::string& error) {
			if(!source.is_object() || !source.contains("runId") || !source["runId"].is_string()
				|| !source.contains("executionMode") || !source["executionMode"].is_string()
				|| !source.contains("smokeObjectId") || !source["smokeObjectId"].is_string()
				|| !source.contains("requiredWorkerCount") || !source["requiredWorkerCount"].is_number_unsigned()) {
				error = "Locomotion validation requires typed runId, executionMode, smokeObjectId and requiredWorkerCount.";
				return std::nullopt;
			}
			Options result{};
			result.runId = source["runId"].get<std::string>();
			result.executionMode = source["executionMode"].get<std::string>();
			if(const auto found = source.find("scenario"); found != source.end()) {
				if(!found->is_string()) {
					error = "scenario must be a named validation lane.";
					return std::nullopt;
				}
				result.scenario = found->get<std::string>();
			}
			if(const auto found = source.find("caseId"); found != source.end()) {
				if(!found->is_string()) {
					error = "caseId must be a frozen semantic case name.";
					return std::nullopt;
				}
				result.caseId = found->get<std::string>();
			}
			const auto parsedId = ParseUUID(source["smokeObjectId"].get_ref<const std::string&>());
			const std::uint64_t workers = source["requiredWorkerCount"].get<std::uint64_t>();
			if(result.runId.empty() || !parsedId || *parsedId == EMPTY_UUID || workers == 0U || workers > 64U
				|| (result.executionMode != "editor-play" && result.executionMode != "export")) {
				error = "Locomotion validation options contain an invalid run identity, mode, UUID or worker count.";
				return std::nullopt;
			}
			constexpr std::array supportedCases{
				"intent.manual-world", "intent.manual-local", "intent.diagonal-normalized",
				"intent.zero-and-tiny", "intent.continuous-expiry", "intent.persistent-script",
				"authority.stop-resume", "authority.release",
				"authority.competing-producer", "authority.explicit-transfer"};
			const std::string_view selectedCase = result.caseId;
			if((result.scenario != "smoke" && result.scenario != "semantic")
				|| (result.scenario == "smoke" && selectedCase != kSmokeCase)
				|| (result.scenario == "semantic" && std::none_of(supportedCases.begin(),
					supportedCases.end(), [selectedCase](const char* candidate) {
						return std::string_view(candidate) == selectedCase;
					}))) {
				error = "The requested named semantic validation case has no implemented physical verifier.";
				return std::nullopt;
			}
			result.smokeObjectId = *parsedId;
			result.requiredWorkerCount = static_cast<std::uint32_t>(workers);
			if(const auto found = source.find("qualificationRequested"); found != source.end()) {
				if(!found->is_boolean()) {
					error = "qualificationRequested must be a JSON boolean.";
					return std::nullopt;
				}
				result.qualificationRequested = found->get<bool>();
			}
			return result;
		}

		const Physics::PhysicsCharacterState* FindCharacter(
			const Physics::PhysicsCharacterStatePacket& packet, UUID objectId) {
			if(!packet.states) return nullptr;
			const auto found = std::find_if(packet.states->begin(), packet.states->end(),
				[objectId](const auto& state) { return state.objectId == objectId; });
			return found == packet.states->end() ? nullptr : &*found;
		}

		bool Submit(Session& session, CommandKind kind, std::uint64_t sequence,
			double x, double speed, std::string& error) {
			LocomotionCommand command{};
			command.kind = kind;
			command.handle = session.handle;
			command.expectedConfigurationRevision = session.configurationRevision;
			command.expectedAuthorityEpoch = session.authorityEpoch;
			command.ownerToken = kind == CommandKind::Acquire ? 0U : session.ownerToken;
			command.producerId = session.producerId;
			command.sequence = sequence;
			command.correlationId = session.producerId * 1000U + sequence;
			command.intentKind = IntentKind::Script;
			command.lifetime = IntentLifetime::UntilStopped;
			command.direction = {x, 0.0, 0.0};
			command.requestedSpeed = speed;
			if(kind == CommandKind::Transfer) command.transferProducerId = 60061U;
			if(kind == CommandKind::SetIntent) {
				if(session.options.caseId == "intent.manual-local")
					command.directionSpace = DirectionSpace::ControllerLocal;
				if(session.options.caseId == "intent.diagonal-normalized")
					command.direction.z = 1.0;
				if(session.options.caseId == "intent.zero-and-tiny")
					command.direction.x = 1.0e-10;
				if(session.options.caseId == "intent.continuous-expiry") {
					command.intentKind = IntentKind::Player;
					command.lifetime = IntentLifetime::OneUpdate;
				}
			}
			const auto admission = LocomotionSystem::Instance().SubmitBatch(std::span(&command, 1U));
			if(!admission.queued || admission.receipts.size() != 1U
				|| admission.receipts.front().state != ReceiptState::Queued) {
				error = "The live locomotion command was not admitted at the public copied queue.";
				return false;
			}
			session.trace.push_back({{"event", "admitted"}, {"kind", static_cast<unsigned>(kind)},
				{"producerId", session.producerId}, {"sequence", sequence},
				{"acceptedUpdateEpoch", admission.receipts.front().acceptedUpdateEpoch}});
			return true;
		}

		std::optional<CommandReceipt> ReadApplied(Session& session, std::uint64_t sequence, std::string& error) {
			const auto receipt = LocomotionSystem::Instance().ReadCurrentReceipt(session.handle,
				session.producerId, sequence, session.producerId * 1000U + sequence);
			if(!receipt) {
				error = "The smoke character's full attachment identity became stale.";
				return std::nullopt;
			}
			if(receipt->state == ReceiptState::Rejected || receipt->state == ReceiptState::Expired) {
				error = "The protected locomotion boundary rejected or expired an admitted command.";
				return std::nullopt;
			}
			if(receipt->state == ReceiptState::Queued) return receipt;
			session.trace.push_back({{"event", "applied"}, {"producerId", session.producerId},
				{"sequence", sequence},
				{"authorityEpoch", receipt->authorityEpoch}, {"ownerToken", receipt->ownerToken}});
			return receipt;
		}

		nlohmann::json CaseResult(const Session& session) {
			const double deltaX = session.finalX - session.initialX;
			const double deltaZ = session.finalZ - session.initialZ;
			const double planarDistance = std::hypot(deltaX, deltaZ);
			bool physicalPass = deltaX > 0.5;
			if(session.options.caseId == "intent.manual-local") physicalPass = planarDistance > 0.5;
			if(session.options.caseId == "intent.diagonal-normalized")
				physicalPass = deltaX > 0.5 && deltaZ > 0.5 && planarDistance <= 10.05;
			if(session.options.caseId == "intent.zero-and-tiny") physicalPass = planarDistance <= 0.05;
			if(session.options.caseId == "intent.continuous-expiry")
				physicalPass = planarDistance < 1.0 && session.finalObservedSpeed <= 0.05;
			if(session.options.caseId == "intent.persistent-script") physicalPass = planarDistance > 5.0;
			if(session.options.caseId == "authority.stop-resume")
				physicalPass = session.stopSettled && session.resumed;
			if(session.options.caseId == "authority.release")
				physicalPass = session.releaseSettled;
			if(session.options.caseId == "authority.competing-producer")
				physicalPass = session.conflictRejected && deltaX > 1.0;
			if(session.options.caseId == "authority.explicit-transfer")
				physicalPass = session.transferMoved;
			const bool observedLease = std::any_of(session.trace.begin(), session.trace.end(),
				[](const auto& entry) { return entry.value("kind", "") == "lease"; });
			const bool observedTarget = std::any_of(session.trace.begin(), session.trace.end(),
				[](const auto& entry) { return entry.value("kind", "") == "target"; });
			const auto storage = Physics::PhysicsSystem::Instance().ReadCharacterMotionStorage();
			return {{"schemaVersion", 1U}, {"runId", session.options.runId},
				{"executionMode", session.options.executionMode}, {"scenario", session.options.scenario},
				{"pending", false}, {"succeeded", physicalPass && observedLease && observedTarget},
				{"executedCaseCount", 1U},
				{"actualWorkerCount", ThreadPool::instance().getThreadCount()},
				{"completedFrames", session.completedFrames},
				{"cases", nlohmann::json::array({{{"id", session.options.caseId},
					{"passed", physicalPass && observedLease && observedTarget},
					{"initialX", session.initialX}, {"finalX", session.finalX},
					{"initialZ", session.initialZ}, {"finalZ", session.finalZ},
					{"planarDistanceMetres", planarDistance},
					{"stopSettled", session.stopSettled}, {"resumed", session.resumed},
					{"releaseSettled", session.releaseSettled},
					{"conflictRejected", session.conflictRejected},
					{"transferMoved", session.transferMoved},
					{"finalObservedPlanarSpeed", session.finalObservedSpeed},
					{"startPhysicalTick", session.startPhysicalTick},
					{"lastPhysicalTick", session.lastPhysicalTick}}})},
				{"storage", {{"characters", storage.characters}, {"ownedLeases", storage.ownedLeases},
					{"externalLeases", storage.externalLeases}, {"guardedTargets", storage.guardedTargets},
					{"retainedIntervals", storage.retainedIntervals},
					{"committedIntervals", storage.committedIntervals},
					{"livePayloadBytes", storage.livePayloadBytes},
					{"traceReservedBytes", storage.traceReservedBytes}}},
				{"rawTrace", session.trace},
				{"qualificationComplete", false}};
		}
	}

	nlohmann::json RunLocomotionApplicationValidation(Engine& engine, const nlohmann::json& options) {
		std::string error;
		const auto parsed = ParseOptions(options, error);
		if(!parsed) return Failure(std::move(error));
		if(ThreadPool::instance().getThreadCount() != parsed->requiredWorkerCount)
			return Failure("The actual scheduler worker count differs from requiredWorkerCount.");
		if(parsed->qualificationRequested && parsed->scenario == "smoke")
			return Failure("A smoke diagnostic cannot claim the 65-case semantic and sustained qualification.");
		std::lock_guard lock(sessionMutex);
		if(!activeSession) {
			if(!Physics::PhysicsSystem::Instance().ConfigureCharacterMotionTrace(true, 16384U))
				return Failure("The bounded physical ingress trace could not reserve its ring.");
			if(!LocomotionSystem::Instance().ConfigureParameterTrace(true, 4096U)) {
				(void)Physics::PhysicsSystem::Instance().ConfigureCharacterMotionTrace(false);
				return Failure("The bounded animation parameter trace could not reserve its ring.");
			}
			Session next{};
			next.engine = &engine;
			next.options = *parsed;
			// A reconfigured trace keeps its global monotonic sequence so a stale
			// observer cannot alias another run. Take this run's cursor at admission.
			next.ingressSequence = Physics::PhysicsSystem::Instance()
				.CopyCharacterMotionTrace(0U).latestSequence;
			next.parameterSequence = LocomotionSystem::Instance().CopyParameterTrace(0U).latestSequence;
			activeSession = std::move(next);
		}
		Session& session = *activeSession;
		if(session.engine != &engine || session.options.runId != parsed->runId
			|| session.options.executionMode != parsed->executionMode) {
			return Failure("An existing locomotion validation session has a different host or identity.");
		}
		if(++session.completedFrames > kSmokeMaximumFrames) {
			const auto failed = Failure("The real smoke command did not complete within 480 application frames.", session.completedFrames);
			activeSession.reset();
			(void)Physics::PhysicsSystem::Instance().ConfigureCharacterMotionTrace(false);
			(void)LocomotionSystem::Instance().ConfigureParameterTrace(false);
			return failed;
		}
		if(parsed->executionMode == "export") {
			const auto frameDuration = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / 60.0));
			if(session.nextExportFrame == Clock::time_point{}) session.nextExportFrame = Clock::now();
			if(Clock::now() < session.nextExportFrame) std::this_thread::sleep_until(session.nextExportFrame);
			session.nextExportFrame += frameDuration;
			if(!engine.Update(1.0F / 60.0F)) {
				const auto failed = Failure("The exported Engine stopped before the smoke case completed.", session.completedFrames);
				activeSession.reset();
				(void)Physics::PhysicsSystem::Instance().ConfigureCharacterMotionTrace(false);
				(void)LocomotionSystem::Instance().ConfigureParameterTrace(false);
				return failed;
			}
		}
		if(!CaptureIngress(session, error) || !CaptureParameters(session, error)) {
			const auto failed = Failure(error, session.completedFrames);
			activeSession.reset();
			(void)Physics::PhysicsSystem::Instance().ConfigureCharacterMotionTrace(false);
			(void)LocomotionSystem::Instance().ConfigureParameterTrace(false);
			return failed;
		}
		const auto packet = Physics::PhysicsSystem::Instance().LatestCharacterStatePacket();
		const auto* physical = packet ? FindCharacter(*packet, parsed->smokeObjectId) : nullptr;
		if(!physical) return {{"pending", true}, {"succeeded", false}, {"executedCaseCount", 0U}};
		session.lastPhysicalTick = physical->tick;
		session.trace.push_back({{"event", "physics"}, {"tick", physical->tick},
			{"x", physical->pose.position.x}, {"y", physical->pose.position.y},
			{"z", physical->pose.position.z}});
		const auto finishFailure = [&session](std::string reason) {
			const auto failed = Failure(std::move(reason), session.completedFrames);
			activeSession.reset();
			(void)Physics::PhysicsSystem::Instance().ConfigureCharacterMotionTrace(false);
			(void)LocomotionSystem::Instance().ConfigureParameterTrace(false);
			return failed;
		};
		if(session.stage == Stage::AwaitBinding) {
			const auto binding = LocomotionSystem::Instance().ResolvePublishedSnapshot(parsed->smokeObjectId);
			if(!binding) return {{"pending", true}, {"succeeded", false}, {"executedCaseCount", 0U}};
			if(binding->bindingReason != LocomotionReason::None || binding->configurationRevision == 0U)
				return finishFailure("The authored smoke controller has an incompatible locomotion binding.");
			session.handle = binding->handle;
			session.configurationRevision = binding->configurationRevision;
			session.authorityEpoch = binding->authorityEpoch;
			session.initialX = physical->pose.position.x;
			session.initialZ = physical->pose.position.z;
			session.startPhysicalTick = physical->tick;
			if(!Submit(session, CommandKind::Acquire, 1U, 0.0, 0.0, error)) return finishFailure(error);
			session.stage = Stage::AwaitAcquire;
		}
		else if(session.stage == Stage::AwaitAcquire) {
			const auto receipt = ReadApplied(session, 1U, error);
			if(!receipt) return finishFailure(error);
			if(receipt->state != ReceiptState::Queued) {
				session.ownerToken = receipt->ownerToken;
				session.authorityEpoch = receipt->authorityEpoch;
				if(session.ownerToken == 0U || !Submit(session, CommandKind::SetIntent, 2U, 1.0, 5.0, error))
					return finishFailure(error.empty() ? "Acquire did not grant a valid owner token." : error);
				session.stage = Stage::AwaitIntent;
			}
		}
		else if(session.stage == Stage::AwaitIntent) {
			const auto receipt = ReadApplied(session, 2U, error);
			if(!receipt) return finishFailure(error);
			if(receipt->state != ReceiptState::Queued) session.stage = Stage::ObserveMotion;
		}
		else if(session.stage == Stage::ObserveMotion) {
			const bool authorityCase = session.options.caseId.starts_with("authority.");
			if(physical->tick >= session.startPhysicalTick + (authorityCase ? 60U : kSmokeMotionTicks)) {
				session.finalX = physical->pose.position.x;
				session.finalZ = physical->pose.position.z;
				if(physical->completedStepDuration > 0.0F)
					session.finalObservedSpeed = std::hypot(physical->realizedDisplacement.x,
						physical->realizedDisplacement.z) / physical->completedStepDuration;
				if(session.options.caseId == "authority.release") {
					if(!Submit(session, CommandKind::Release, 3U, 0.0, 0.0, error)) return finishFailure(error);
					session.stage = Stage::AwaitRelease;
				} else if(session.options.caseId == "authority.explicit-transfer") {
					if(!Submit(session, CommandKind::Transfer, 3U, 0.0, 0.0, error)) return finishFailure(error);
					session.stage = Stage::AwaitTransfer;
				} else if(session.options.caseId == "authority.competing-producer") {
					session.stage = Stage::AwaitConflict;
				} else {
					if(!Submit(session, CommandKind::Stop, 3U, 0.0, 0.0, error)) return finishFailure(error);
					session.stage = Stage::AwaitStop;
				}
			}
		}
		else if(session.stage == Stage::AwaitStop) {
			const auto receipt = ReadApplied(session, 3U, error);
			if(!receipt) return finishFailure(error);
			if(receipt->state != ReceiptState::Queued) {
				session.authorityEpoch = receipt->authorityEpoch;
				session.phasePhysicalTick = physical->tick;
				session.stage = session.options.caseId == "authority.stop-resume"
					? Stage::ObserveStopped : Stage::Complete;
			}
		}
		else if(session.stage == Stage::ObserveStopped) {
			// Allow configured braking to finish, then measure a separate settled window.
			if(physical->tick >= session.phasePhysicalTick + 30U && session.settledX == 0.0)
				session.settledX = physical->pose.position.x;
			if(physical->tick >= session.phasePhysicalTick + 50U) {
				const double speed = physical->completedStepDuration > 0.0F
					? std::hypot(physical->realizedDisplacement.x, physical->realizedDisplacement.z)
						/ physical->completedStepDuration : 0.0;
				session.stopSettled = speed <= 0.1 && std::abs(physical->pose.position.x - session.settledX) <= 0.1;
				if(!session.stopSettled) return finishFailure("Stop did not settle the physical controller.");
				session.phaseX = physical->pose.position.x;
				if(!Submit(session, CommandKind::SetIntent, 4U, 1.0, 5.0, error)) return finishFailure(error);
				session.stage = Stage::AwaitResume;
			}
		}
		else if(session.stage == Stage::AwaitResume) {
			const auto receipt = ReadApplied(session, 4U, error);
			if(!receipt) return finishFailure(error);
			if(receipt->state != ReceiptState::Queued) {
				session.phasePhysicalTick = physical->tick;
				session.stage = Stage::ObserveResumed;
			}
		}
		else if(session.stage == Stage::ObserveResumed) {
			if(physical->tick >= session.phasePhysicalTick + 60U) {
				session.resumed = physical->pose.position.x - session.phaseX > 0.5;
				if(!session.resumed) return finishFailure("A fresh intent did not resume physical movement after Stop.");
				if(!Submit(session, CommandKind::Stop, 5U, 0.0, 0.0, error)) return finishFailure(error);
				session.stage = Stage::AwaitFinalStop;
			}
		}
		else if(session.stage == Stage::AwaitRelease) {
			const auto receipt = ReadApplied(session, 3U, error);
			if(!receipt) return finishFailure(error);
			if(receipt->state != ReceiptState::Queued) {
				session.phasePhysicalTick = physical->tick;
				session.phaseX = physical->pose.position.x;
				session.stage = Stage::ObserveReleased;
			}
		}
		else if(session.stage == Stage::ObserveReleased) {
			if(physical->tick >= session.phasePhysicalTick + 30U) {
				const double speed = physical->completedStepDuration > 0.0F
					? std::hypot(physical->realizedDisplacement.x, physical->realizedDisplacement.z)
						/ physical->completedStepDuration : 0.0;
				session.releaseSettled = speed <= 0.1 && std::abs(physical->pose.position.x - session.phaseX) <= 0.1;
				session.stage = Stage::Complete;
			}
		}
		else if(session.stage == Stage::AwaitConflict) {
			if(!session.conflictAttempted) {
				LocomotionCommand competing{};
				competing.kind = CommandKind::SetIntent;
				competing.handle = session.handle;
				competing.expectedConfigurationRevision = session.configurationRevision;
				competing.expectedAuthorityEpoch = session.authorityEpoch;
				competing.ownerToken = session.ownerToken;
				competing.producerId = 60061U;
				competing.sequence = 1U;
				competing.correlationId = 60061001U;
				competing.intentKind = IntentKind::Script;
				competing.lifetime = IntentLifetime::UntilStopped;
				competing.direction = {-1.0, 0.0, 0.0};
				competing.requestedSpeed = 5.0;
				const auto admission = LocomotionSystem::Instance().SubmitBatch(std::span(&competing, 1U));
				session.conflictAttempted = true;
				if(!admission.queued) {
					session.conflictRejected = admission.receipts.size() == 1U
						&& admission.receipts.front().reason == LocomotionReason::OwnershipConflict;
				}
			}
			if(!session.conflictRejected) {
				const auto receipt = LocomotionSystem::Instance().ReadCurrentReceipt(session.handle,
					60061U, 1U, 60061001U);
				if(receipt && receipt->state == ReceiptState::Rejected)
					session.conflictRejected = receipt->reason == LocomotionReason::OwnershipConflict;
			}
			if(session.conflictRejected) {
				session.trace.push_back({{"event", "competing-producer-rejected"},
					{"reason", static_cast<unsigned>(LocomotionReason::OwnershipConflict)}});
				session.phasePhysicalTick = physical->tick;
				session.stage = Stage::ObserveTransferred;
			}
		}
		else if(session.stage == Stage::AwaitTransfer) {
			const auto receipt = ReadApplied(session, 3U, error);
			if(!receipt) return finishFailure(error);
			if(receipt->state != ReceiptState::Queued) {
				session.ownerToken = receipt->ownerToken;
				session.authorityEpoch = receipt->authorityEpoch;
				session.producerId = 60061U;
				session.phaseX = physical->pose.position.x;
				if(session.ownerToken == 0U || !Submit(session, CommandKind::SetIntent, 1U, 1.0, 5.0, error))
					return finishFailure(error.empty() ? "Transfer did not grant the recipient a token." : error);
				session.stage = Stage::AwaitTransferredIntent;
			}
		}
		else if(session.stage == Stage::AwaitTransferredIntent) {
			const auto receipt = ReadApplied(session, 1U, error);
			if(!receipt) return finishFailure(error);
			if(receipt->state != ReceiptState::Queued) {
				session.phasePhysicalTick = physical->tick;
				session.stage = Stage::ObserveTransferred;
			}
		}
		else if(session.stage == Stage::ObserveTransferred) {
			if(physical->tick >= session.phasePhysicalTick + 60U) {
				if(session.options.caseId == "authority.competing-producer") {
					session.finalX = physical->pose.position.x;
					if(!Submit(session, CommandKind::Stop, 3U, 0.0, 0.0, error)) return finishFailure(error);
				} else {
					session.transferMoved = physical->pose.position.x - session.phaseX > 0.5;
					if(!session.transferMoved) return finishFailure("The transfer recipient did not move the controller.");
					if(!Submit(session, CommandKind::Stop, 2U, 0.0, 0.0, error)) return finishFailure(error);
				}
				session.stage = Stage::AwaitFinalStop;
			}
		}
		else if(session.stage == Stage::AwaitFinalStop) {
			const auto sequence = session.options.caseId == "authority.competing-producer" ? 3U
				: session.options.caseId == "authority.stop-resume" ? 5U : 2U;
			const auto receipt = ReadApplied(session, sequence, error);
			if(!receipt) return finishFailure(error);
			if(receipt->state != ReceiptState::Queued) session.stage = Stage::Complete;
		}
		if(session.stage == Stage::Complete) {
			const auto report = CaseResult(session);
			activeSession.reset();
			(void)Physics::PhysicsSystem::Instance().ConfigureCharacterMotionTrace(false);
			(void)LocomotionSystem::Instance().ConfigureParameterTrace(false);
			return report;
		}
		return {{"pending", true}, {"succeeded", false}, {"executedCaseCount", 0U}};
	}
}
