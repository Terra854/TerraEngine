// AnimationControllerOperations.cpp - checked controller document edits.

#include "AnimationControllerOperations.h"

#include "AnimationAssetOperations.h"
#include "AnimationAutomationSchemas.h"
#include "../Animation/AnimationAssetAuthoringDecode.h"
#include "Engine/Animation/Assets/AnimationControllerAsset.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

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

		template<typename Record>
		Record* FindByStableId(std::vector<Record>& records, std::uint64_t stableId)
		{
			const auto found = std::find_if(records.begin(), records.end(), [stableId](const Record& record) {
				return record.stableId == stableId;
			});
			return found == records.end() ? nullptr : &*found;
		}

		template<typename Record>
		bool EraseByStableId(std::vector<Record>& records, std::uint64_t stableId)
		{
			const auto found = std::find_if(records.begin(), records.end(), [stableId](const Record& record) {
				return record.stableId == stableId;
			});
			if(found == records.end())
			{
				return false;
			}
			records.erase(found);
			return true;
		}

		std::optional<AnimationControllerParameterType> ParseParameterType(std::string_view value)
		{
			if(value == "float") return AnimationControllerParameterType::Float;
			if(value == "integer") return AnimationControllerParameterType::Integer;
			if(value == "boolean") return AnimationControllerParameterType::Boolean;
			if(value == "trigger") return AnimationControllerParameterType::Trigger;
			if(value == "vector") return AnimationControllerParameterType::Vector;
			if(value == "transform") return AnimationControllerParameterType::Transform;
			if(value == "tag") return AnimationControllerParameterType::Tag;
			return std::nullopt;
		}

		std::optional<AnimationControllerNodeKind> ParseNodeKind(std::string_view value)
		{
			if(value == "clip") return AnimationControllerNodeKind::Clip;
			if(value == "blend_tree_1d") return AnimationControllerNodeKind::BlendTree1D;
			if(value == "blend_tree_2d") return AnimationControllerNodeKind::BlendTree2D;
			if(value == "subgraph") return AnimationControllerNodeKind::Subgraph;
			if(value == "motion_match") return AnimationControllerNodeKind::MotionMatch;
			if(value == "procedural_pose") return AnimationControllerNodeKind::ProceduralPose;
			return std::nullopt;
		}

		std::optional<AnimationFloat3> ReadFloat3(const nlohmann::json& value)
		{
			if(!value.is_array() || value.size() != 3U)
			{
				return std::nullopt;
			}
			AnimationFloat3 result;
			float* components[] = {&result.x, &result.y, &result.z};
			for(std::size_t index = 0U; index < 3U; ++index)
			{
				const double number = value[index].get<double>();
				if(!std::isfinite(number)
					|| number < -std::numeric_limits<float>::max()
					|| number > std::numeric_limits<float>::max())
				{
					return std::nullopt;
				}
				*components[index] = static_cast<float>(number);
			}
			return result;
		}

		std::optional<AnimationQuaternion> ReadQuaternion(const nlohmann::json& value)
		{
			if(!value.is_array() || value.size() != 4U)
			{
				return std::nullopt;
			}
			AnimationQuaternion result;
			float* components[] = {&result.x, &result.y, &result.z, &result.w};
			for(std::size_t index = 0U; index < 4U; ++index)
			{
				const double number = value[index].get<double>();
				if(!std::isfinite(number)
					|| number < -std::numeric_limits<float>::max()
					|| number > std::numeric_limits<float>::max())
				{
					return std::nullopt;
				}
				*components[index] = static_cast<float>(number);
			}
			return result;
		}

		std::optional<AnimationTransform> ReadTransform(const nlohmann::json& value)
		{
			if(!value.is_object())
			{
				return std::nullopt;
			}
			const std::optional<AnimationFloat3> translation = ReadFloat3(value.value("translation", nlohmann::json{}));
			const std::optional<AnimationQuaternion> rotation = ReadQuaternion(value.value("rotation", nlohmann::json{}));
			const std::optional<AnimationFloat3> scale = ReadFloat3(value.value("scale", nlohmann::json{}));
			if(!translation || !rotation || !scale)
			{
				return std::nullopt;
			}
			return AnimationTransform{*translation, *rotation, *scale};
		}

		std::optional<AnimationControllerParameterValue> ReadParameterValue(
			AnimationControllerParameterType type,
			const nlohmann::json& payload)
		{
			switch(type)
			{
			case AnimationControllerParameterType::Float:
				if(payload.contains("floatValue")) return static_cast<float>(payload["floatValue"].get<double>());
				break;
			case AnimationControllerParameterType::Integer:
				if(payload.contains("integerValue")
					&& (payload["integerValue"].is_number_integer()
						|| payload["integerValue"].is_number_unsigned()))
				{
					const std::int64_t value = payload["integerValue"].get<std::int64_t>();
					if(value >= std::numeric_limits<std::int32_t>::min()
						&& value <= std::numeric_limits<std::int32_t>::max())
					{
						return static_cast<std::int32_t>(value);
					}
				}
				break;
			case AnimationControllerParameterType::Boolean:
				if(payload.contains("booleanValue")) return payload["booleanValue"].get<bool>();
				break;
			case AnimationControllerParameterType::Trigger:
				return std::monostate{};
			case AnimationControllerParameterType::Vector:
				if(payload.contains("vectorValue"))
				{
					if(const std::optional<AnimationFloat3> value = ReadFloat3(payload["vectorValue"]))
					{
						return AnimationControllerParameterValue{*value};
					}
				}
				break;
			case AnimationControllerParameterType::Transform:
				if(payload.contains("transformValue"))
				{
					if(const std::optional<AnimationTransform> value = ReadTransform(payload["transformValue"]))
					{
						return AnimationControllerParameterValue{*value};
					}
				}
				break;
			case AnimationControllerParameterType::Tag:
				if(payload.contains("stringValue")) return payload["stringValue"].get<std::string>();
				break;
			}
			return std::nullopt;
		}

		AutomationDispatchResult ApplyControllerEdit(
			const nlohmann::json& payload,
			AnimationControllerAsset& controller)
		{
			const std::string action = payload.value("action", std::string{});
			const std::uint64_t stableId = payload.value("stableId", 0ULL);
			if(action.empty() || stableId == 0U)
			{
				return Fail(ErrorCategory::Validation, "Controller edits require an action and nonzero stableId.");
			}

			if(action == "add_parameter" || action == "set_parameter_default")
			{
				AnimationControllerParameter* parameter = FindByStableId(controller.parameters, stableId);
				if(action == "add_parameter")
				{
					if(parameter != nullptr)
					{
						return Fail(ErrorCategory::Conflict, "A controller parameter already uses stableId.");
					}
					const std::optional<AnimationControllerParameterType> type = ParseParameterType(
						payload.value("parameterType", std::string{}));
					if(!type || !payload.contains("name"))
					{
						return Fail(ErrorCategory::Validation, "add_parameter requires name and parameterType.");
					}
					controller.parameters.push_back({stableId, payload["name"].get<std::string>(), *type, {}});
					parameter = &controller.parameters.back();
				}
				if(parameter == nullptr)
				{
					return Fail(ErrorCategory::MissingTarget, "The controller parameter stableId was not found.");
				}
				if(payload.contains("name")) parameter->name = payload["name"].get<std::string>();
				if(payload.contains("parameterType"))
				{
					const std::optional<AnimationControllerParameterType> requested = ParseParameterType(
						payload["parameterType"].get<std::string>());
					if(!requested || (action != "add_parameter" && *requested != parameter->type))
					{
						return Fail(ErrorCategory::Validation,
							"Existing controller parameter types cannot be changed implicitly.");
					}
					parameter->type = *requested;
				}
				const std::optional<AnimationControllerParameterValue> value = ReadParameterValue(parameter->type, payload);
				if(!value)
				{
					return Fail(ErrorCategory::Validation, "The payload does not contain a value matching the parameter type.");
				}
				parameter->defaultValue = *value;
			}
			else if(action == "remove_parameter")
			{
				if(!EraseByStableId(controller.parameters, stableId))
				{
					return Fail(ErrorCategory::MissingTarget, "The controller parameter stableId was not found.");
				}
				std::erase_if(controller.transitions, [stableId](const auto& transition) {
					return transition.conditionParameterId == stableId;
				});
				std::erase_if(controller.reusableBindings, [stableId](const auto& binding) {
					return binding.sourceParameterId == stableId;
				});
			}
			else if(action == "add_node" || action == "set_node")
			{
				AnimationControllerNode* node = FindByStableId(controller.nodes, stableId);
				if(action == "add_node")
				{
					if(node != nullptr) return Fail(ErrorCategory::Conflict, "A controller node already uses stableId.");
					const std::optional<AnimationControllerNodeKind> kind = ParseNodeKind(payload.value("nodeKind", std::string{}));
					if(!kind) return Fail(ErrorCategory::Validation, "add_node requires a supported nodeKind.");
					controller.nodes.push_back({});
					node = &controller.nodes.back();
					node->stableId = stableId;
					node->kind = *kind;
				}
				if(node == nullptr) return Fail(ErrorCategory::MissingTarget, "The controller node stableId was not found.");
				if(payload.contains("flags")) node->flags = payload["flags"].get<std::uint32_t>();
				if(payload.contains("outOfDomainPolicy"))
				{
					node->outOfDomainPolicy = payload["outOfDomainPolicy"] == "extrapolate"
						? AnimationControllerBlendOutOfDomainPolicy::Extrapolate
						: AnimationControllerBlendOutOfDomainPolicy::Clamp;
				}
				if(payload.contains("dependencyAssetId"))
				{
					const std::optional<UUID> dependency = TerraEngine::ParseUUID(payload["dependencyAssetId"].get<std::string>());
					if(!dependency) return Fail(ErrorCategory::Validation, "dependencyAssetId is not a UUID.");
					node->dependencyAssetId = AnimationGuid{*dependency};
				}
				if(payload.contains("dependencyGeneration")) node->dependencyGeneration = payload["dependencyGeneration"].get<std::uint64_t>();
				if(payload.contains("dependencyFingerprint")) node->dependencyFingerprint = payload["dependencyFingerprint"].get<std::uint64_t>();
			}
			else if(action == "remove_node")
			{
				if(!EraseByStableId(controller.nodes, stableId)) return Fail(ErrorCategory::MissingTarget, "The controller node stableId was not found.");
				std::vector<std::uint64_t> removedStates;
				for(const auto& state : controller.states)
				{
					if(state.nodeId == stableId) removedStates.push_back(state.stableId);
				}
				std::erase_if(controller.states, [stableId](const auto& state) { return state.nodeId == stableId; });
				for(auto& node : controller.nodes)
				{
					std::erase_if(node.blendSamples, [stableId](const auto& sample) { return sample.childNodeId == stableId; });
				}
				for(const std::uint64_t stateId : removedStates)
				{
					std::erase_if(controller.transitions, [stateId](const auto& transition) {
						return transition.sourceStateId == stateId || transition.destinationStateId == stateId;
					});
					std::erase_if(controller.layers, [stateId](const auto& layer) { return layer.rootStateId == stateId; });
					for(auto& state : controller.states) if(state.parentStateId == stateId) state.parentStateId = 0U;
					for(auto& group : controller.syncGroups)
					{
						std::erase_if(group.markers, [stateId](const auto& marker) { return marker.stateId == stateId; });
					}
				}
			}
			else if(action == "add_state" || action == "set_state")
			{
				AnimationControllerState* state = FindByStableId(controller.states, stableId);
				if(action == "add_state")
				{
					if(state != nullptr) return Fail(ErrorCategory::Conflict, "A controller state already uses stableId.");
					if(!payload.contains("nodeId")) return Fail(ErrorCategory::Validation, "add_state requires nodeId.");
					controller.states.push_back({});
					state = &controller.states.back();
					state->stableId = stableId;
				}
				if(state == nullptr) return Fail(ErrorCategory::MissingTarget, "The controller state stableId was not found.");
				if(payload.contains("nodeId")) state->nodeId = payload["nodeId"].get<std::uint64_t>();
				if(payload.contains("parentStateId")) state->parentStateId = payload["parentStateId"].get<std::uint64_t>();
				if(payload.contains("entry")) state->entry = payload["entry"].get<bool>();
				if(payload.contains("exit")) state->exit = payload["exit"].get<bool>();
				if(payload.contains("anyState")) state->anyState = payload["anyState"].get<bool>();
			}
			else if(action == "remove_state")
			{
				if(!EraseByStableId(controller.states, stableId)) return Fail(ErrorCategory::MissingTarget, "The controller state stableId was not found.");
				std::erase_if(controller.transitions, [stableId](const auto& transition) {
					return transition.sourceStateId == stableId || transition.destinationStateId == stableId;
				});
				std::erase_if(controller.layers, [stableId](const auto& layer) { return layer.rootStateId == stableId; });
				for(auto& state : controller.states) if(state.parentStateId == stableId) state.parentStateId = 0U;
				for(auto& group : controller.syncGroups)
				{
					std::erase_if(group.markers, [stableId](const auto& marker) { return marker.stateId == stableId; });
				}
			}
			else if(action == "add_transition" || action == "set_transition")
			{
				AnimationControllerTransition* transition = FindByStableId(controller.transitions, stableId);
				if(action == "add_transition")
				{
					if(transition != nullptr) return Fail(ErrorCategory::Conflict, "A controller transition already uses stableId.");
					if(!payload.contains("sourceStateId") || !payload.contains("destinationStateId"))
					{
						return Fail(ErrorCategory::Validation, "add_transition requires sourceStateId and destinationStateId.");
					}
					controller.transitions.push_back({});
					transition = &controller.transitions.back();
					transition->stableId = stableId;
				}
				if(transition == nullptr) return Fail(ErrorCategory::MissingTarget, "The controller transition stableId was not found.");
				if(payload.contains("sourceStateId")) transition->sourceStateId = payload["sourceStateId"].get<std::uint64_t>();
				if(payload.contains("destinationStateId")) transition->destinationStateId = payload["destinationStateId"].get<std::uint64_t>();
				if(payload.contains("conditionParameterId")) transition->conditionParameterId = payload["conditionParameterId"].get<std::uint64_t>();
				if(payload.contains("durationTicks")) transition->durationTicks = payload["durationTicks"].get<std::int64_t>();
				if(payload.contains("exitTimeTicks")) transition->exitTimeTicks = payload["exitTimeTicks"].get<std::int64_t>();
				if(payload.contains("destinationOffsetNormalized")) transition->destinationOffsetNormalized = payload["destinationOffsetNormalized"].get<float>();
				if(payload.contains("priority")) transition->priority = payload["priority"].get<std::uint32_t>();
			}
			else if(action == "remove_transition")
			{
				if(!EraseByStableId(controller.transitions, stableId)) return Fail(ErrorCategory::MissingTarget, "The controller transition stableId was not found.");
			}
			else if(action == "add_layer" || action == "set_layer")
			{
				AnimationControllerLayer* layer = FindByStableId(controller.layers, stableId);
				if(action == "add_layer")
				{
					if(layer != nullptr) return Fail(ErrorCategory::Conflict, "A controller layer already uses stableId.");
					if(!payload.contains("rootStateId")) return Fail(ErrorCategory::Validation, "add_layer requires rootStateId.");
					controller.layers.push_back({});
					layer = &controller.layers.back();
					layer->stableId = stableId;
				}
				if(layer == nullptr) return Fail(ErrorCategory::MissingTarget, "The controller layer stableId was not found.");
				if(payload.contains("rootStateId")) layer->rootStateId = payload["rootStateId"].get<std::uint64_t>();
				if(payload.contains("additive")) layer->additive = payload["additive"].get<bool>();
				if(payload.contains("defaultWeight")) layer->defaultWeight = payload["defaultWeight"].get<float>();
			}
			else if(action == "remove_layer")
			{
				if(!EraseByStableId(controller.layers, stableId)) return Fail(ErrorCategory::MissingTarget, "The controller layer stableId was not found.");
			}
			else
			{
				return Fail(ErrorCategory::UnsupportedCapability, "The controller edit action is not supported by schema version 1.");
			}

			const AnimationStatus validation = ValidateAnimationControllerAsset(controller);
			if(validation != AnimationStatus::Ok)
			{
				return Fail(ErrorCategory::Validation,
					"The complete controller candidate was rejected: " + std::string(ToString(validation)) + ".");
			}
			AutomationDispatchResult result;
			result.ok = true;
			result.message = "Prepared and validated the complete controller candidate.";
			result.data = {
				{"action", action},
				{"stableId", stableId},
				{"counts", {
					{"parameters", controller.parameters.size()},
					{"nodes", controller.nodes.size()},
					{"states", controller.states.size()},
					{"transitions", controller.transitions.size()},
					{"layers", controller.layers.size()}
				}}
			};
			return result;
		}
	}

	AutomationDispatchResult ExecuteAnimationControllerOperation(
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
		if(operation == "animation.controller.validate")
		{
			// The service resolves and decodes the registered asset after this schema
			// check. Never synthesize a positive validation at the adapter boundary.
			validated.data["validationPending"] = true;
			return validated;
		}
		if(operation != "animation.controller.edit" || assetDatabase == nullptr)
		{
			return Fail(ErrorCategory::Unavailable,
				"Controller authoring requires the open project's AssetDatabase protected boundary.");
		}
		const std::optional<UUID> assetId = TerraEngine::ParseUUID(arguments.value("assetId", std::string{}));
		if(!assetId)
		{
			return Fail(ErrorCategory::Validation, "assetId is not a UUID.");
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
			[assetDatabase, assetId = *assetId, payload, dryRun]() {
				return CommitCookedAnimationAssetEdit(
					*assetDatabase,
					assetId,
					"AnimationControllerAssetImporter",
					64U * 1024U * 1024U,
					dryRun,
					[payload](std::span<const std::uint8_t> currentBytes,
						std::vector<std::uint8_t>& candidateBytes) {
						AnimationControllerAsset controller;
						const AnimationStatus decoded = DeserializeAnimationAssetForAuthoring(
							currentBytes, controller, DeserializeAnimationControllerAsset);
						if(decoded != AnimationStatus::Ok)
						{
							return Fail(ErrorCategory::Validation,
								"The registered controller could not be decoded: "
								+ std::string(ToString(decoded)) + ".");
						}
						AutomationDispatchResult edited = ApplyControllerEdit(payload, controller);
						if(!edited.ok)
						{
							return edited;
						}
						const AnimationStatus encoded = SerializeAnimationControllerAsset(controller, candidateBytes);
						if(encoded != AnimationStatus::Ok)
						{
							return Fail(ErrorCategory::Validation,
								"The complete controller candidate could not be serialized: "
								+ std::string(ToString(encoded)) + ".");
						}
						return edited;
					});
			});
	}
}
