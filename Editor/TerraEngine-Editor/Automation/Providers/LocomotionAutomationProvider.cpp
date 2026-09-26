// LocomotionAutomationProvider.cpp - bounded JSON adaptation, never live movement writes.
#include "LocomotionAutomationProvider.h"
#include "Engine/Locomotion/LocomotionSystem.h"
#include "Engine/Scripting/ScriptingSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

namespace TerraEngine::Editor::Automation {
	namespace {
		namespace Movement = TerraEngine::Locomotion;
		using Json = nlohmann::json;

		bool Only(const Json& value, std::initializer_list<std::string_view> fields) {
			if(!value.is_object()) return false;
			for(auto entry = value.begin(); entry != value.end(); ++entry)
				if(std::find(fields.begin(), fields.end(), entry.key()) == fields.end()) return false;
			return true;
		}

		// JSON numbers must retain their complete integer identity. Fractional,
		// negative, overflowed, and stringified generations are never coerced.
		bool ReadInteger(const Json& value, std::string_view key, std::uint64_t& output, bool required = true) {
			const auto found = value.find(std::string(key));
			if(found == value.end()) return !required;
			if(found->is_number_unsigned()) { output = found->get<std::uint64_t>(); return true; }
			if(found->is_number_integer() && found->get<std::int64_t>() >= 0) {
				output = static_cast<std::uint64_t>(found->get<std::int64_t>());
				return true;
			}
			return false;
		}

		bool ReadScalar(const Json& value, std::string_view key, double& output) {
			const auto found = value.find(std::string(key));
			if(found == value.end()) return true;
			if(!found->is_number()) return false;
			output = found->get<double>();
			return std::isfinite(output);
		}

		template<class Enum, std::size_t Count>
		bool ReadEnum(const Json& value, std::string_view key, Enum& output,
			const std::array<std::string_view, Count>& names, bool required = false) {
			const auto found = value.find(std::string(key));
			if(found == value.end()) return !required;
			if(!found->is_string()) return false;
			const auto text = found->get<std::string>();
			const auto match = std::find(names.begin(), names.end(), text);
			if(match == names.end()) return false;
			output = static_cast<Enum>(std::distance(names.begin(), match));
			return true;
		}

		constexpr std::array<std::string_view, 7> kCommands{
			"Acquire", "SetIntent", "FollowNavigation", "Stop", "SetMode", "Transfer", "Release"};
		constexpr std::array<std::string_view, 2> kModes{"Velocity", "RootMotion"};
		constexpr std::array<std::string_view, 3> kIntents{"Player", "Script", "Navigation"};
		constexpr std::array<std::string_view, 2> kSpaces{"World", "ControllerLocal"};
		constexpr std::array<std::string_view, 2> kLifetimes{"OneUpdate", "UntilStopped"};
		constexpr std::array<std::string_view, 5> kReceiptStates{"Queued", "Applied", "Adjusted", "Rejected", "Expired"};
		constexpr std::array<std::string_view, 4> kAuthorityStates{"Idle", "Controlled", "Stopped", "Suspended"};
		constexpr std::array<std::string_view, 16> kReasons{"None", "InvalidArgument", "StaleHandle", "StaleConfiguration",
			"OwnershipConflict", "UnsupportedController", "UnsupportedFrame", "MissingBinding", "AnimationUnavailable",
			"CapacityExceeded", "NotPlaying", "ExpiredReceipt", "SequenceConflict", "Stopped", "Revoked", "ExternalExclusive"};

		template<class Enum, std::size_t Count>
		std::string Name(Enum value, const std::array<std::string_view, Count>& names) {
			const auto index = static_cast<std::size_t>(value);
			return index < names.size() ? std::string(names[index]) : "Unknown";
		}

		Json HandleJson(const Movement::LocomotionHandle& handle) {
			return {{"sceneLifetime", handle.sceneLifetime}, {"objectId", ToHexString(handle.objectId)},
				{"objectGeneration", handle.objectGeneration}, {"attachmentGeneration", handle.attachmentGeneration},
				{"slotIndex", handle.slotIndex}, {"slotGeneration", handle.slotGeneration}};
		}

		bool ParseHandle(const Json& value, Movement::LocomotionHandle& handle) {
			if(!Only(value, {"sceneLifetime", "objectId", "objectGeneration", "attachmentGeneration", "slotIndex", "slotGeneration"})
				|| !value.contains("objectId") || !value["objectId"].is_string()) return false;
			const auto id = ParseUUID(value["objectId"].get<std::string>());
			std::uint64_t slot = 0U, generation = 0U;
			if(!id || !ReadInteger(value, "sceneLifetime", handle.sceneLifetime)
				|| !ReadInteger(value, "objectGeneration", handle.objectGeneration)
				|| !ReadInteger(value, "attachmentGeneration", handle.attachmentGeneration)
				|| !ReadInteger(value, "slotIndex", slot) || slot >= Movement::kInvalidLocomotionSlot
				|| !ReadInteger(value, "slotGeneration", generation)
				|| generation > std::numeric_limits<std::uint32_t>::max()) return false;
			handle.objectId = *id;
			handle.slotIndex = static_cast<std::uint32_t>(slot);
			handle.slotGeneration = static_cast<std::uint32_t>(generation);
			return handle.IsValid();
		}

		bool ParseCommand(const Json& value, Movement::LocomotionCommand& command) {
			if(!Only(value, {"kind", "handle", "expectedAuthorityEpoch", "expectedConfigurationRevision", "ownerToken",
				"producerId", "sequence", "correlationId", "intentKind", "directionSpace", "direction", "requestedSpeed",
				"lifetime", "mode", "transferProducerId"}) || !value.contains("handle")) return false;
			if(!ParseHandle(value["handle"], command.handle)
				|| !ReadEnum(value, "kind", command.kind, kCommands, true)
				|| !ReadInteger(value, "expectedAuthorityEpoch", command.expectedAuthorityEpoch)
				|| !ReadInteger(value, "expectedConfigurationRevision", command.expectedConfigurationRevision)
				|| !ReadInteger(value, "ownerToken", command.ownerToken)
				|| !ReadInteger(value, "producerId", command.producerId)
				|| !ReadInteger(value, "sequence", command.sequence)
				|| !ReadInteger(value, "correlationId", command.correlationId)
				|| !ReadInteger(value, "transferProducerId", command.transferProducerId, false)
				|| !ReadEnum(value, "intentKind", command.intentKind, kIntents)
				|| !ReadEnum(value, "directionSpace", command.directionSpace, kSpaces)
				|| !ReadEnum(value, "lifetime", command.lifetime, kLifetimes)
				|| !ReadEnum(value, "mode", command.mode, kModes)
				|| !ReadScalar(value, "requestedSpeed", command.requestedSpeed)) return false;
			if(value.contains("direction")) {
				const auto& direction = value["direction"];
				if(!direction.is_array() || direction.size() != 3U) return false;
				for(std::size_t index = 0U; index < 3U; ++index) {
					if(!direction[index].is_number()) return false;
					command.direction[static_cast<glm::length_t>(index)] = direction[index].get<double>();
					if(!std::isfinite(command.direction[static_cast<glm::length_t>(index)])) return false;
				}
			}
			return true;
		}

		Json ReceiptJson(const Movement::CommandReceipt& receipt) {
			return {{"handle", HandleJson(receipt.handle)}, {"producerId", receipt.producerId},
				{"sequence", receipt.sequence}, {"correlationId", receipt.correlationId},
				{"state", Name(receipt.state, kReceiptStates)}, {"reason", Name(receipt.reason, kReasons)},
				{"acceptedUpdateEpoch", receipt.acceptedUpdateEpoch}, {"authorityEpoch", receipt.authorityEpoch},
				{"ownerToken", receipt.ownerToken}};
		}

		Json VectorJson(const glm::dvec3& vector) { return Json::array({vector.x, vector.y, vector.z}); }

		Json ObservationJson(const Movement::LocomotionObservation& value) {
			return {{"handle", HandleJson(value.handle)}, {"publicationEpoch", value.publicationEpoch},
				{"authority", {{"state", Name(value.authority.state, kAuthorityStates)},
					{"mode", Name(value.authority.mode, kModes)}, {"intentKind", Name(value.authority.kind, kIntents)},
					{"ownerToken", value.authority.ownerToken}, {"ownerProducerId", value.authority.ownerProducerId},
					{"authorityEpoch", value.authority.authorityEpoch}, {"configurationRevision", value.authority.configurationRevision},
					{"acceptedSequence", value.authority.acceptedSequence}, {"suspendedReason", Name(value.authority.suspendedReason, kReasons)}}},
				{"bindingReason", Name(value.bindingReason, kReasons)}, {"navigationReason", Name(value.navigationReason, kReasons)},
				{"navigationState", static_cast<unsigned>(value.navigationState)}, {"navigationOutcome", static_cast<unsigned>(value.navigationOutcome)},
				{"navigationAgentReason", static_cast<unsigned>(value.navigationAgentReason)},
				{"navigationCompatibilityField", static_cast<unsigned>(value.navigationCompatibilityField)},
				{"navigationControllerValue", value.navigationControllerValue}, {"navigationProfileValue", value.navigationProfileValue},
				{"animationReason", Name(value.animationReason, kReasons)}, {"animationParameterField", static_cast<unsigned>(value.animationParameterField)},
				{"animationParameterError", static_cast<unsigned>(value.animationParameterError)},
				{"desiredVelocity", VectorJson(value.desiredVelocity)}, {"observedPlanarSpeed", value.observedPlanarSpeed},
				{"observedLocalDirection", VectorJson(value.observedLocalDirection)}, {"grounded", value.grounded},
				{"physicalTick", value.physicalTick}, {"observationDurationSeconds", value.observationDurationSeconds},
				{"motionHistoryAvailable", value.motionHistoryAvailable}, {"remainingRouteDistanceMetres", value.remainingRouteDistanceMetres},
				{"requestedTranslation", VectorJson(value.requestedTranslation)}, {"realizedTranslation", VectorJson(value.realizedTranslation)},
				{"requestedYawRadians", value.requestedYawRadians}, {"realizedYawRadians", value.realizedYawRadians}};
		}

		AutomationDispatchResult Reject(std::string message, Movement::LocomotionReason reason = Movement::LocomotionReason::InvalidArgument) {
			return {false, ErrorCategory::Validation, std::move(message), {{"schemaVersion", 1U}, {"reason", Name(reason, kReasons)}}};
		}
		AutomationDispatchResult Success(Json data) {
			data["schemaVersion"] = 1U;
			return {true, ErrorCategory::Internal, "Copied locomotion result.", std::move(data)};
		}

		AutomationDispatchResult Execute(std::string_view name, const AutomationRequestContext& request) {
			const Json& args = request.arguments;
			auto& system = Movement::LocomotionSystem::Instance();
			if(name == "locomotion.resolve") {
				if(!Only(args, {"objectId"}) || !args.contains("objectId") || !args["objectId"].is_string())
					return Reject("Resolve requires only a canonical object UUID.");
				const auto id = ParseUUID(args["objectId"].get<std::string>());
				if(!id) return Reject("The object UUID is malformed.");
				const auto binding = system.ResolvePublishedSnapshot(*id);
				if(!binding) return Reject("No current locomotion binding is published.", Movement::LocomotionReason::MissingBinding);
				return Success({{"handle", HandleJson(binding->handle)}, {"bindingReason", Name(binding->bindingReason, kReasons)},
					{"configurationRevision", binding->configurationRevision}, {"authorityEpoch", binding->authorityEpoch}});
			}
			if(name == "locomotion.command") {
				if(!Only(args, {"commands"}) || !args.contains("commands") || !args["commands"].is_array()
					|| args["commands"].empty() || args["commands"].size() > Movement::kMaximumCommandBatch)
					return Reject("Commands must contain 1 through 256 complete records.");
				std::vector<Movement::LocomotionCommand> commands(args["commands"].size());
				for(std::size_t index = 0U; index < commands.size(); ++index)
					if(!ParseCommand(args["commands"][index], commands[index]))
						return Reject("One malformed command rejects the complete batch before admission.");
				if(!ScriptingSystem::Instance().IsRunning())
					return Reject("Locomotion commands require an active Play session.", Movement::LocomotionReason::NotPlaying);
				// Submit once after every JSON record has been validated. This call only
				// queues copied intent; it never runs a physical step or grants authority.
				const auto admitted = system.SubmitBatch(commands);
				Json receipts = Json::array();
				for(const auto& receipt : admitted.receipts) receipts.push_back(ReceiptJson(receipt));
				if(!admitted.queued) {
					auto rejected = Reject("The typed movement batch was rejected.", admitted.reason);
					rejected.data["receipts"] = std::move(receipts);
					return rejected;
				}
				return Success({{"receipts", std::move(receipts)}, {"visibilityBoundary", "next_locomotion_publication"}});
			}
			if(name == "locomotion.receipt") {
				Movement::LocomotionHandle handle{};
				std::uint64_t producer = 0U, sequence = 0U, correlation = 0U;
				if(!Only(args, {"handle", "producerId", "sequence", "correlationId"}) || !args.contains("handle")
					|| !ParseHandle(args["handle"], handle) || !ReadInteger(args, "producerId", producer) || producer == 0U
					|| !ReadInteger(args, "sequence", sequence) || sequence == 0U
					|| !ReadInteger(args, "correlationId", correlation) || correlation == 0U)
					return Reject("Receipt reads require the exact full handle and producer, sequence, and correlation identity.");
				const auto receipt = system.ReadCurrentReceipt(handle, producer, sequence, correlation);
				if(!receipt) return Reject("The receipt handle no longer names a current attachment.", Movement::LocomotionReason::StaleHandle);
				return Success({{"receipt", ReceiptJson(*receipt)}});
			}
			if(name == "locomotion.inspect") {
				std::uint64_t offset = 0U, maximum = Movement::kMaximumObservationCopy, expectedEpoch = 0U;
				if(!Only(args, {"offset", "maxItems", "expectedPublicationEpoch"})
					|| !ReadInteger(args, "offset", offset, false)
					|| !ReadInteger(args, "maxItems", maximum, false) || maximum == 0U || maximum > Movement::kMaximumObservationCopy
					|| !ReadInteger(args, "expectedPublicationEpoch", expectedEpoch, false))
					return Reject("Inspection allows at most 256 entries and exact optional publication qualification.");
				// Retain one immutable root for the entire response. A multi-page caller
				// supplies its prior epoch; a changed root rejects instead of mixing frames.
				const auto root = system.CopyObservations();
				if(!root) return Reject("No observation publication is available.", Movement::LocomotionReason::MissingBinding);
				if(expectedEpoch != 0U && expectedEpoch != root->publicationEpoch)
					return Reject("The observation publication changed; restart the bounded copy.", Movement::LocomotionReason::StaleHandle);
				if(offset > root->entries.size()) return Reject("Observation offset exceeds the retained publication.");
				const std::size_t end = std::min(root->entries.size(), static_cast<std::size_t>(offset) + static_cast<std::size_t>(maximum));
				Json observations = Json::array();
				for(std::size_t index = static_cast<std::size_t>(offset); index < end; ++index)
					observations.push_back(ObservationJson(root->entries[index]));
				return Success({{"publicationEpoch", root->publicationEpoch}, {"totalCount", root->entries.size()},
					{"offset", offset}, {"writtenCount", observations.size()}, {"nextOffset", end}, {"observations", std::move(observations)}});
			}
			return Reject("Unknown locomotion operation.");
		}

		Json InputSchema(std::string_view name) {
			Json properties = Json::object();
			Json required = Json::array();
			const Json nonnegative = {{"type", "integer"}, {"minimum", 0U}, {"maximum", std::numeric_limits<std::uint64_t>::max()}};
			Json positive = nonnegative;
			positive["minimum"] = 1U;
			const Json uuid = {{"type", "string"}, {"minLength", 32U}, {"maxLength", 32U}};
			const Json handle = {{"type", "object"}, {"additionalProperties", false},
				{"required", {"sceneLifetime", "objectId", "objectGeneration", "attachmentGeneration", "slotIndex", "slotGeneration"}},
				{"properties", {{"sceneLifetime", positive}, {"objectId", uuid}, {"objectGeneration", positive},
					{"attachmentGeneration", positive}, {"slotIndex", {{"type", "integer"}, {"minimum", 0U}, {"maximum", Movement::kInvalidLocomotionSlot - 1U}}},
					{"slotGeneration", {{"type", "integer"}, {"minimum", 1U}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}}}}};
			if(name == "locomotion.resolve") {
				properties["objectId"] = uuid;
				required = {"objectId"};
			} else if(name == "locomotion.command") {
				const Json command = {{"type", "object"}, {"additionalProperties", false},
					{"required", {"kind", "handle", "expectedAuthorityEpoch", "expectedConfigurationRevision", "ownerToken", "producerId", "sequence", "correlationId"}},
					{"properties", {{"kind", {{"type", "string"}, {"enum", kCommands}}}, {"handle", handle},
						{"expectedAuthorityEpoch", nonnegative}, {"expectedConfigurationRevision", positive}, {"ownerToken", nonnegative},
						{"producerId", positive}, {"sequence", positive}, {"correlationId", positive}, {"transferProducerId", nonnegative},
						{"intentKind", {{"type", "string"}, {"enum", kIntents}}}, {"mode", {{"type", "string"}, {"enum", kModes}}},
						{"directionSpace", {{"type", "string"}, {"enum", kSpaces}}}, {"lifetime", {{"type", "string"}, {"enum", kLifetimes}}},
						{"direction", {{"type", "array"}, {"minItems", 3U}, {"maxItems", 3U}, {"items", {{"type", "number"}}}}},
						{"requestedSpeed", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 100.0}}}}}};
				properties["commands"] = {{"type", "array"}, {"minItems", 1U}, {"maxItems", 256U}, {"items", command}};
				required = {"commands"};
			} else if(name == "locomotion.receipt") {
				properties["handle"] = handle;
				for(const char* key : {"producerId", "sequence", "correlationId"}) properties[key] = {{"type", "integer"}, {"minimum", 1U}};
				required = {"handle", "producerId", "sequence", "correlationId"};
			} else {
				properties["offset"] = {{"type", "integer"}, {"minimum", 0U}};
				properties["maxItems"] = {{"type", "integer"}, {"minimum", 1U}, {"maximum", 256U}};
				properties["expectedPublicationEpoch"] = {{"type", "integer"}, {"minimum", 0U}};
			}
			return {{"type", "object"}, {"schemaVersion", 1U}, {"properties", std::move(properties)},
				{"required", std::move(required)}, {"additionalProperties", false}};
		}
	}

	std::string_view LocomotionAutomationProvider::GetProviderId() const { return "locomotion"; }
	std::vector<AutomationCapabilityRegistration> LocomotionAutomationProvider::GetCapabilities() {
		std::vector<AutomationCapabilityRegistration> result;
		for(const std::string_view name : {"locomotion.resolve", "locomotion.inspect", "locomotion.command", "locomotion.receipt"}) {
			const bool write = name == "locomotion.command";
			AutomationCapabilityDescriptor descriptor;
			descriptor.name = std::string(name);
			descriptor.category = "locomotion";
			descriptor.description = "Bounded generation-qualified character movement; admission is not physical application.";
			descriptor.inputSchema = InputSchema(name);
			descriptor.resultSchema = {{"type", "object"}, {"properties", {{"schemaVersion", {{"type", "integer"}, {"const", 1U}}}}}, {"additionalProperties", true}};
			descriptor.permission = write ? AutomationPermission::ProjectWrite : AutomationPermission::Read;
			descriptor.mutationMode = write ? MutationMode::SceneMutation : MutationMode::ReadOnly;
			descriptor.availability.requiresProject = true;
			descriptor.execution.workerPreparation = write;
			descriptor.execution.protectedCommit = write;
			descriptor.execution.visibilityBoundary = write ? "next_locomotion_publication" : "immutable_locomotion_publication";
			descriptor.execution.preparationReads = {"LocomotionAuthority", "LocomotionObservations", "LocomotionReceipts"};
			if(write) descriptor.execution.commitWrites = {"LocomotionCommands"};
			result.push_back({std::move(descriptor), [name](const AutomationRequestContext& request) { return Execute(name, request); }, {}});
		}
		return result;
	}
}
