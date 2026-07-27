// AnimationAdvancedOperations.cpp - typed skeleton/retarget/motion/ragdoll edits.

#include "AnimationAdvancedOperations.h"

#include "AnimationAssetOperations.h"
#include "AnimationAutomationSchemas.h"
#include "../Animation/AnimationAssetAuthoringDecode.h"
#include "../Assets/AssetDatabase.h"
#include "Engine/Animation/Assets/MotionDatabaseAsset.h"
#include "Engine/Animation/Assets/RagdollProfileAsset.h"
#include "Engine/Animation/Assets/RetargetProfileAsset.h"
#include "Engine/Animation/Assets/SkeletonAsset.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		using namespace TerraEngine::Animation;
		using TerraEngine::Editor::Animation::DeserializeAnimationAssetForAuthoring;

		AutomationDispatchResult Fail(ErrorCategory category, std::string message)
		{
			AutomationDispatchResult result;
			result.error = category;
			result.message = std::move(message);
			return result;
		}

		template<typename Record, typename IdReader>
		Record* FindRecord(std::vector<Record>& records, std::uint64_t stableId, IdReader readId)
		{
			const auto found = std::find_if(records.begin(), records.end(), [&](const Record& record) {
				return readId(record) == stableId;
			});
			return found == records.end() ? nullptr : &*found;
		}

		AutomationDispatchResult Success(std::string action, std::uint64_t stableId, nlohmann::json data)
		{
			AutomationDispatchResult result;
			result.ok = true;
			result.message = "Prepared and validated the complete advanced animation asset candidate.";
			data["action"] = std::move(action);
			data["stableId"] = stableId;
			result.data = std::move(data);
			return result;
		}

		AutomationDispatchResult EditSkeleton(
			const nlohmann::json& payload,
			std::span<const std::uint8_t> currentBytes,
			std::vector<std::uint8_t>& candidateBytes)
		{
			SkeletonAsset skeleton;
			const AnimationStatus decoded = DeserializeAnimationAssetForAuthoring(
				currentBytes, skeleton, DeserializeSkeletonAsset);
			if(decoded != AnimationStatus::Ok)
			{
				return Fail(ErrorCategory::Validation,
					"The skeleton could not be decoded: " + std::string(ToString(decoded)) + ".");
			}
			const std::string action = payload.value("action", std::string{});
			const std::uint64_t stableId = payload.value("stableId", 0ULL);
			if(action == "skeleton.set_joint")
			{
				SkeletonJoint* joint = FindRecord(skeleton.joints, stableId,
					[](const SkeletonJoint& value) { return value.stableId; });
				if(joint == nullptr) return Fail(ErrorCategory::MissingTarget, "The skeleton joint stableId was not found.");
				if(payload.contains("semantic")) joint->semanticRole = payload["semantic"].get<std::string>();
				if(payload.contains("displayName")) joint->displayName = payload["displayName"].get<std::string>();
				if(payload.contains("deformsMesh")) joint->deformsMesh = payload["deformsMesh"].get<bool>();
			}
			else if(action == "skeleton.set_socket")
			{
				SkeletonSocket* socket = FindRecord(skeleton.sockets, stableId,
					[](const SkeletonSocket& value) { return value.stableId; });
				if(socket == nullptr) return Fail(ErrorCategory::MissingTarget, "The skeleton socket stableId was not found.");
				if(payload.contains("semantic")) socket->semanticRole = payload["semantic"].get<std::string>();
				if(payload.contains("displayName")) socket->displayName = payload["displayName"].get<std::string>();
				if(payload.contains("jointStableId")) socket->jointStableId = payload["jointStableId"].get<std::uint64_t>();
			}
			else
			{
				return Fail(ErrorCategory::UnsupportedCapability, "The skeleton edit action is unsupported.");
			}
			const AnimationStatus encoded = SerializeSkeletonAsset(skeleton, candidateBytes);
			if(encoded != AnimationStatus::Ok)
			{
				return Fail(ErrorCategory::Validation,
					"The complete skeleton candidate was rejected: " + std::string(ToString(encoded)) + ".");
			}
			return Success(action, stableId, {
				{"assetKind", "skeleton"},
				{"jointCount", skeleton.joints.size()},
				{"socketCount", skeleton.sockets.size()}
			});
		}

		AutomationDispatchResult EditRetarget(
			const nlohmann::json& payload,
			std::span<const std::uint8_t> currentBytes,
			std::vector<std::uint8_t>& candidateBytes)
		{
			RetargetProfileAsset profile;
			const AnimationStatus decoded = DeserializeAnimationAssetForAuthoring(
				currentBytes, profile, DeserializeRetargetProfileAsset);
			if(decoded != AnimationStatus::Ok)
			{
				return Fail(ErrorCategory::Validation,
					"The retarget profile could not be decoded: " + std::string(ToString(decoded)) + ".");
			}
			const std::string action = payload.value("action", std::string{});
			const std::uint64_t stableId = payload.value("stableId", 0ULL);
			if(action == "retarget.set_mapping")
			{
				RetargetSemanticMapping* semantic = FindRecord(profile.semanticMappings, stableId,
					[](const RetargetSemanticMapping& value) { return value.mappingId; });
				RetargetExplicitJointMapping* explicitMapping = FindRecord(profile.explicitMappings, stableId,
					[](const RetargetExplicitJointMapping& value) { return value.mappingId; });
				if((semantic == nullptr) == (explicitMapping == nullptr))
				{
					return Fail(ErrorCategory::MissingTarget,
						"stableId must identify exactly one semantic or explicit retarget mapping.");
				}
				if(semantic != nullptr)
				{
					if(payload.contains("sourceJointId")) semantic->sourceJointId = payload["sourceJointId"].get<std::uint64_t>();
					if(payload.contains("targetJointId")) semantic->targetJointId = payload["targetJointId"].get<std::uint64_t>();
					if(payload.contains("sourceSemantic")) semantic->sourceSemantic = payload["sourceSemantic"].get<std::string>();
					if(payload.contains("targetSemantic")) semantic->targetSemantic = payload["targetSemantic"].get<std::string>();
					if(payload.contains("required")) semantic->required = payload["required"].get<bool>();
				}
				else
				{
					if(payload.contains("sourceJointId")) explicitMapping->sourceJointId = payload["sourceJointId"].get<std::uint64_t>();
					if(payload.contains("targetJointId")) explicitMapping->targetJointId = payload["targetJointId"].get<std::uint64_t>();
					if(payload.contains("required")) explicitMapping->required = payload["required"].get<bool>();
				}
			}
			else if(action == "retarget.set_contact")
			{
				RetargetContactRule* contact = FindRecord(profile.contacts, stableId,
					[](const RetargetContactRule& value) { return value.contactId; });
				if(contact == nullptr) return Fail(ErrorCategory::MissingTarget, "The retarget contact stableId was not found.");
				if(payload.contains("sourceJointId")) contact->sourceJointId = payload["sourceJointId"].get<std::uint64_t>();
				if(payload.contains("targetJointId")) contact->targetJointId = payload["targetJointId"].get<std::uint64_t>();
				if(payload.contains("targetChainId")) contact->targetChainId = payload["targetChainId"].get<std::uint64_t>();
				if(payload.contains("toleranceMeters")) contact->toleranceMeters = payload["toleranceMeters"].get<float>();
				if(payload.contains("preservePosition")) contact->preservePosition = payload["preservePosition"].get<bool>();
				if(payload.contains("preserveOrientation")) contact->preserveOrientation = payload["preserveOrientation"].get<bool>();
				if(payload.contains("required")) contact->required = payload["required"].get<bool>();
			}
			else if(action == "retarget.set_quality")
			{
				if(payload.contains("reachableToleranceMeters"))
				{
					profile.quality.reachableEndEffectorToleranceMeters = payload["reachableToleranceMeters"].get<float>();
				}
				if(payload.contains("reviewReason")) profile.quality.reviewReason = payload["reviewReason"].get<std::string>();
				if(payload.contains("authoredCoverage")) profile.quality.authoredCoverage = payload["authoredCoverage"].get<float>();
			}
			else
			{
				return Fail(ErrorCategory::UnsupportedCapability, "The retarget edit action is unsupported.");
			}
			const AnimationStatus encoded = SerializeRetargetProfileAsset(profile, candidateBytes);
			if(encoded != AnimationStatus::Ok)
			{
				return Fail(ErrorCategory::Validation,
					"The complete retarget candidate was rejected: " + std::string(ToString(encoded)) + ".");
			}
			return Success(action, stableId, {
				{"assetKind", "retarget"},
				{"semanticMappings", profile.semanticMappings.size()},
				{"explicitMappings", profile.explicitMappings.size()},
				{"contacts", profile.contacts.size()}
			});
		}

		AutomationDispatchResult EditMotionDatabase(
			const nlohmann::json& payload,
			std::span<const std::uint8_t> currentBytes,
			std::vector<std::uint8_t>& candidateBytes)
		{
			MotionDatabaseAsset database;
			const AnimationStatus decoded = DeserializeAnimationAssetForAuthoring(
				currentBytes, database, DeserializeMotionDatabaseAsset);
			if(decoded != AnimationStatus::Ok)
			{
				return Fail(ErrorCategory::Validation,
					"The motion database could not be decoded: " + std::string(ToString(decoded)) + ".");
			}
			const std::string action = payload.value("action", std::string{});
			const std::uint64_t stableId = payload.value("stableId", 0ULL);
			if(action != "motion.set_sample_metadata")
			{
				return Fail(ErrorCategory::UnsupportedCapability, "The motion database edit action is unsupported.");
			}
			MotionDatabaseSample* sample = FindRecord(database.samples, stableId,
				[](const MotionDatabaseSample& value) { return value.stableSampleId; });
			if(sample == nullptr) return Fail(ErrorCategory::MissingTarget, "The motion sample stableId was not found.");
			if(payload.contains("tagMask")) sample->tagMask = payload["tagMask"].get<std::uint64_t>();
			if(payload.contains("contactMask")) sample->contactMask = payload["contactMask"].get<std::uint32_t>();
			if(payload.contains("phaseQuantized")) sample->phaseQuantized = payload["phaseQuantized"].get<std::uint16_t>();
			if(payload.contains("transitionFlags")) sample->transitionFlags = payload["transitionFlags"].get<std::uint16_t>();
			if(payload.contains("continuityGroup")) sample->continuityGroup = payload["continuityGroup"].get<std::uint32_t>();
			const AnimationStatus encoded = SerializeMotionDatabaseAsset(database, candidateBytes);
			if(encoded != AnimationStatus::Ok)
			{
				return Fail(ErrorCategory::Validation,
					"The complete motion database candidate was rejected: " + std::string(ToString(encoded)) + ".");
			}
			return Success(action, stableId, {
				{"assetKind", "motion_database"},
				{"sampleCount", database.samples.size()}
			});
		}

		AutomationDispatchResult EditRagdoll(
			const nlohmann::json& payload,
			std::span<const std::uint8_t> currentBytes,
			std::vector<std::uint8_t>& candidateBytes)
		{
			RagdollProfileAsset profile;
			const AnimationStatus decoded = DeserializeAnimationAssetForAuthoring(
				currentBytes, profile, DeserializeRagdollProfileAsset);
			if(decoded != AnimationStatus::Ok)
			{
				return Fail(ErrorCategory::Validation,
					"The ragdoll profile could not be decoded: " + std::string(ToString(decoded)) + ".");
			}
			const std::string action = payload.value("action", std::string{});
			const std::uint64_t stableId = payload.value("stableId", 0ULL);
			if(action == "ragdoll.set_body")
			{
				RagdollBodyRecord* body = FindRecord(profile.bodies, stableId,
					[](const RagdollBodyRecord& value) { return value.bodyId; });
				if(body == nullptr) return Fail(ErrorCategory::MissingTarget, "The ragdoll body stableId was not found.");
				if(payload.contains("massKilograms")) body->massKilograms = payload["massKilograms"].get<float>();
				if(payload.contains("linearDamping")) body->linearDamping = payload["linearDamping"].get<float>();
				if(payload.contains("angularDamping")) body->angularDamping = payload["angularDamping"].get<float>();
				if(payload.contains("gravityFactor")) body->gravityFactor = payload["gravityFactor"].get<float>();
				if(payload.contains("allowSleep")) body->allowSleep = payload["allowSleep"].get<bool>();
				if(payload.contains("startAwake")) body->startAwake = payload["startAwake"].get<bool>();
			}
			else if(action == "ragdoll.set_blend")
			{
				RagdollBlendRecord* blend = FindRecord(profile.blends, stableId,
					[](const RagdollBlendRecord& value) { return value.stableJointId; });
				if(blend == nullptr) return Fail(ErrorCategory::MissingTarget, "The ragdoll blend stableId was not found.");
				if(payload.contains("defaultWeight")) blend->defaultWeight = payload["defaultWeight"].get<float>();
				if(payload.contains("activationBlendSeconds")) blend->activationBlendSeconds = payload["activationBlendSeconds"].get<float>();
				if(payload.contains("recoveryBlendSeconds")) blend->recoveryBlendSeconds = payload["recoveryBlendSeconds"].get<float>();
				if(payload.contains("motorStrengthScale")) blend->motorStrengthScale = payload["motorStrengthScale"].get<float>();
				if(payload.contains("includeDescendants")) blend->includeDescendants = payload["includeDescendants"].get<bool>();
			}
			else if(action == "ragdoll.set_constraint_limit")
			{
				RagdollConstraintRecord* constraint = FindRecord(profile.constraints, stableId,
					[](const RagdollConstraintRecord& value) { return value.constraintId; });
				if(constraint == nullptr) return Fail(ErrorCategory::MissingTarget, "The ragdoll constraint stableId was not found.");
				const std::uint32_t axis = payload.value("axis", std::numeric_limits<std::uint32_t>::max());
				if(axis >= static_cast<std::uint32_t>(RagdollConstraintAxis::Count))
				{
					return Fail(ErrorCategory::Validation, "axis is outside the ragdoll constraint axis table.");
				}
				RagdollAxisLimit& limit = constraint->limits[axis];
				if(payload.contains("limitMode"))
				{
					const std::string mode = payload["limitMode"].get<std::string>();
					limit.mode = mode == "locked" ? RagdollLimitMode::Locked
						: mode == "limited" ? RagdollLimitMode::Limited : RagdollLimitMode::Free;
				}
				if(payload.contains("minimum")) limit.minimum = payload["minimum"].get<float>();
				if(payload.contains("maximum")) limit.maximum = payload["maximum"].get<float>();
				if(payload.contains("restitution")) limit.restitution = payload["restitution"].get<float>();
				if(payload.contains("springFrequencyHertz")) limit.springFrequencyHertz = payload["springFrequencyHertz"].get<float>();
				if(payload.contains("springDampingRatio")) limit.springDampingRatio = payload["springDampingRatio"].get<float>();
			}
			else
			{
				return Fail(ErrorCategory::UnsupportedCapability, "The ragdoll edit action is unsupported.");
			}
			const AnimationStatus encoded = SerializeRagdollProfileAsset(profile, candidateBytes);
			if(encoded != AnimationStatus::Ok)
			{
				return Fail(ErrorCategory::Validation,
					"The complete ragdoll candidate was rejected: " + std::string(ToString(encoded)) + ".");
			}
			return Success(action, stableId, {
				{"assetKind", "ragdoll"},
				{"bodyCount", profile.bodies.size()},
				{"constraintCount", profile.constraints.size()},
				{"blendCount", profile.blends.size()}
			});
		}

		AutomationDispatchResult EditAdvancedAsset(
			std::string_view importer,
			const nlohmann::json& payload,
			std::span<const std::uint8_t> currentBytes,
			std::vector<std::uint8_t>& candidateBytes)
		{
			if(importer == "SkeletonAssetImporter") return EditSkeleton(payload, currentBytes, candidateBytes);
			if(importer == "RetargetProfileAssetImporter") return EditRetarget(payload, currentBytes, candidateBytes);
			if(importer == "MotionDatabaseAssetImporter") return EditMotionDatabase(payload, currentBytes, candidateBytes);
			if(importer == "RagdollProfileAssetImporter") return EditRagdoll(payload, currentBytes, candidateBytes);
			return Fail(ErrorCategory::UnsupportedCapability,
				"advanced.edit supports registered skeleton, retarget, motion database, and ragdoll assets.");
		}
	}

	AutomationDispatchResult ExecuteAnimationAdvancedOperation(
		std::string_view operation,
		const nlohmann::json& arguments,
		AnimationAutomationTransactions& transactions,
		std::uint64_t generation,
		AssetDatabase* assetDatabase)
	{
		AutomationDispatchResult validated = ValidateAnimationAutomationRequest(operation, arguments);
		if(!validated.ok)
		{
			return validated;
		}
		if(operation != "animation.advanced.edit" || assetDatabase == nullptr)
		{
			return Fail(ErrorCategory::Unavailable,
				"Advanced authoring requires the open project's AssetDatabase protected boundary.");
		}
		const std::optional<UUID> assetId = TerraEngine::ParseUUID(arguments.value("assetId", std::string{}));
		if(!assetId)
		{
			return Fail(ErrorCategory::Validation, "assetId is not a UUID.");
		}
		const AssetMetadata* metadata = assetDatabase->FindAssetByGuid(*assetId);
		if(metadata == nullptr)
		{
			return Fail(ErrorCategory::MissingTarget, "The advanced animation asset is not registered.");
		}
		const std::string importer = metadata->importerName;
		if(importer != "SkeletonAssetImporter"
			&& importer != "RetargetProfileAssetImporter"
			&& importer != "MotionDatabaseAssetImporter"
			&& importer != "RagdollProfileAssetImporter")
		{
			return Fail(ErrorCategory::UnsupportedCapability,
				"The requested asset kind is not editable through animation.advanced.edit.");
		}

		const bool dryRun = arguments.value("dryRun", false);
		const nlohmann::json payload = arguments.value("payload", nlohmann::json::object());
		AnimationAutomationMutation mutation{
			std::string(operation),
			arguments.value("idempotencyKey", ""),
			arguments.value("correlationId", ""),
			arguments.value("expectedGeneration", 0ULL),
			dryRun,
			arguments};
		return transactions.ExecuteWithCommit(mutation, generation,
			[assetDatabase, assetId = *assetId, importer, payload, dryRun]() {
				const std::size_t maximumBytes = importer == "MotionDatabaseAssetImporter"
					? kMotionDatabaseMaximumPayloadBytes
					: 64U * 1024U * 1024U;
				return CommitCookedAnimationAssetEdit(
					*assetDatabase,
					assetId,
					importer,
					maximumBytes,
					dryRun,
					[importer, payload](std::span<const std::uint8_t> currentBytes,
						std::vector<std::uint8_t>& candidateBytes) {
						return EditAdvancedAsset(importer, payload, currentBytes, candidateBytes);
					});
			});
	}
}
