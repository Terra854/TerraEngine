// AnimationAutomationSchemas.cpp - versioned, deterministic, bounded contracts.

#include "AnimationAutomationSchemas.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		using Json = nlohmann::json;

		Json IdentifierSchema()
		{
			return {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
		}

		Json BoundedStringSchema(std::size_t maximumLength)
		{
			return {{"type", "string"}, {"minLength", 1}, {"maxLength", maximumLength}};
		}

		Json ObjectSchema(Json properties, Json required = Json::array())
		{
			Json result = {
				{"type", "object"},
				{"properties", std::move(properties)},
				{"additionalProperties", false}
			};
			if (!required.empty())
			{
				result["required"] = std::move(required);
			}
			return result;
		}

		std::optional<std::string> ValidateBoundedJson(
			const Json& value,
			std::string_view path,
			std::size_t depth,
			std::size_t& visitedNodes)
		{
			constexpr std::size_t kMaximumJsonDepth = 16U;
			constexpr std::size_t kMaximumJsonNodes = 4096U;
			constexpr std::size_t kMaximumStringBytes = 4096U;
			if (depth > kMaximumJsonDepth)
			{
				return std::string(path) + " exceeds the maximum JSON nesting depth.";
			}
			if (++visitedNodes > kMaximumJsonNodes)
			{
				return "Animation request exceeds the maximum total JSON node count.";
			}
			if (value.is_string() && value.get_ref<const std::string&>().size() > kMaximumStringBytes)
			{
				return std::string(path) + " exceeds the maximum bounded string size.";
			}
			if (value.is_number_float() && !std::isfinite(value.get<double>()))
			{
				return std::string(path) + " must be finite.";
			}
			if ((value.is_object() || value.is_array()) && value.size() > kAnimationAutomationMaximumItems)
			{
				return std::string(path) + " exceeds the maximum bounded child count.";
			}
			if (value.is_object())
			{
				for (const auto& [key, child] : value.items())
				{
					if (key.empty() || key.size() > 128U)
					{
						return std::string(path) + " contains an empty or overlong property name.";
					}
					if (const auto error = ValidateBoundedJson(child, std::string(path) + "." + key, depth + 1U, visitedNodes))
					{
						return error;
					}
				}
			}
			else if (value.is_array())
			{
				for (std::size_t index = 0U; index < value.size(); ++index)
				{
					if (const auto error = ValidateBoundedJson(value[index], std::string(path) + "[" + std::to_string(index) + "]", depth + 1U, visitedNodes))
					{
						return error;
					}
				}
			}
			return std::nullopt;
		}

		bool IsMutation(std::string_view operation)
		{
			return operation == "animation.asset.import"
				|| operation == "animation.asset.reimport"
				|| operation == "animation.controller.edit"
				|| operation == "animation.advanced.edit"
				|| operation == "animation.recording.start"
				|| operation == "animation.recording.stop"
				|| operation == "animation.preview.play"
				|| operation == "animation.runtime.parameters"
				|| operation == "animation.runtime.procedural.goal"
				|| operation == "animation.runtime.retarget.request"
				|| operation == "animation.runtime.motion_match.request"
				|| operation == "animation.runtime.ragdoll.request"
				|| operation == "animation.motion_database.build"
				|| operation == "animation.job.cancel";
		}

		std::optional<std::string> ValidateValueAgainstSchema(
			std::string_view key,
			const Json& value,
			const Json& schema)
		{
			const std::string type = schema.value("type", std::string{});
			if (type == "string" && !value.is_string())
			{
				return std::string(key) + " must be a string.";
			}
			if (type == "integer" && !value.is_number_unsigned())
			{
				return std::string(key) + " must be an unsigned integer.";
			}
			if (type == "number" && !value.is_number())
			{
				return std::string(key) + " must be a number.";
			}
			if (type == "boolean" && !value.is_boolean())
			{
				return std::string(key) + " must be boolean.";
			}
			if (type == "object" && !value.is_object())
			{
				return std::string(key) + " must be an object.";
			}
			if (type == "array" && !value.is_array())
			{
				return std::string(key) + " must be an array.";
			}
			if (value.is_string())
			{
				const std::size_t size = value.get_ref<const std::string&>().size();
				if (size < schema.value("minLength", 0U) || size > schema.value("maxLength", size))
				{
					return std::string(key) + " is outside its supported length.";
				}
			}
			if (value.is_number_float() && !std::isfinite(value.get<double>()))
			{
				return std::string(key) + " must be finite.";
			}
			if (value.is_object() && value.size() > schema.value("maxProperties", value.size()))
			{
				return std::string(key) + " exceeds the bounded property count.";
			}
			if (value.is_array() && value.size() > schema.value("maxItems", value.size()))
			{
				return std::string(key) + " exceeds the bounded item count.";
			}
			if (value.is_array() && value.size() < schema.value("minItems", 0U))
			{
				return std::string(key) + " has too few items.";
			}
			if (value.is_number())
			{
				const double number = value.get<double>();
				if (number < schema.value("minimum", -std::numeric_limits<double>::infinity())
					|| number > schema.value("maximum", std::numeric_limits<double>::infinity()))
				{
					return std::string(key) + " is outside its supported range.";
				}
			}
			if (schema.contains("enum")
				&& std::find(schema["enum"].begin(), schema["enum"].end(), value) == schema["enum"].end())
			{
				return std::string(key) + " is not one of the supported values.";
			}
			if (value.is_object() && schema.contains("properties") && schema["properties"].is_object())
			{
				for (const Json& required : schema.value("required", Json::array()))
				{
					const std::string requiredName = required.get<std::string>();
					if (!value.contains(requiredName))
					{
						return std::string(key) + " is missing required property " + requiredName + ".";
					}
				}
				for (const auto& [childName, childValue] : value.items())
				{
					if (!schema["properties"].contains(childName))
					{
						if (!schema.value("additionalProperties", true))
						{
							return std::string(key) + " contains unsupported property " + childName + ".";
						}
						continue;
					}
					if (const auto error = ValidateValueAgainstSchema(
						std::string(key) + "." + childName,
						childValue,
						schema["properties"][childName]))
					{
						return error;
					}
				}
			}
			if (value.is_array() && schema.contains("items") && schema["items"].is_object())
			{
				for (std::size_t index = 0U; index < value.size(); ++index)
				{
					if (const auto error = ValidateValueAgainstSchema(
						std::string(key) + "[" + std::to_string(index) + "]",
						value[index],
						schema["items"]))
					{
						return error;
					}
				}
			}
			return std::nullopt;
		}
	}

	std::vector<std::string> AnimationAutomationOperationNames()
	{
		return {
			"animation.asset.import",
			"animation.asset.reimport",
			"animation.asset.inspect",
			"animation.controller.edit",
			"animation.controller.validate",
			"animation.advanced.edit",
			"animation.preview.play",
			"animation.recording.start",
			"animation.recording.stop",
			"animation.runtime.parameters",
			"animation.runtime.procedural.goal",
			"animation.runtime.procedural.read",
			"animation.runtime.retarget.request",
			"animation.runtime.retarget.read",
			"animation.runtime.motion_match.request",
			"animation.runtime.motion_match.read",
			"animation.runtime.ragdoll.request",
			"animation.runtime.ragdoll.read",
			"animation.motion_database.build",
			// These read surfaces are deliberately separate from the broad diagnostics
			// query so an MCP client can request bounded residency or profiler facts
			// without accidentally enabling detailed collection.
			"animation.runtime.residency",
			"animation.runtime.profiler",
			"animation.runtime.diagnostics",
			"animation.camera.capture",
			"animation.job.get",
			"animation.job.cancel"
		};
	}

	nlohmann::json AnimationAutomationInputSchema(std::string_view operation)
	{
		const Json correlation = IdentifierSchema();
		const Json idempotency = IdentifierSchema();
		const Json generation = {{"type", "integer"}, {"minimum", 1}};
		if (operation == "animation.asset.import" || operation == "animation.asset.reimport")
		{
			return ObjectSchema({
				{"sourcePath", BoundedStringSchema(1024U)},
				{"idempotencyKey", idempotency},
				{"correlationId", correlation},
				{"expectedGeneration", generation},
				{"timeoutMs", {{"type", "integer"}, {"minimum", 1000}, {"maximum", 3600000}}},
				{"dryRun", {{"type", "boolean"}}}
			}, Json::array({"sourcePath", "idempotencyKey"}));
		}
		if (operation == "animation.asset.inspect")
		{
			return ObjectSchema({
				{"assetId", IdentifierSchema()},
				{"maximumItems", {{"type", "integer"}, {"minimum", 1}, {"maximum", kAnimationAutomationMaximumItems}}},
				{"correlationId", correlation}
			}, Json::array({"assetId"}));
		}
		if(operation == "animation.motion_database.build")
		{
			return ObjectSchema({
				{"authoringPath", BoundedStringSchema(1024U)},
				{"outputPath", BoundedStringSchema(1024U)},
				{"idempotencyKey", idempotency}, {"correlationId", correlation},
				{"expectedGeneration", generation}, {"dryRun", {{"type", "boolean"}}}
			}, Json::array({"authoringPath", "outputPath", "idempotencyKey", "correlationId", "expectedGeneration"}));
		}
		if (operation == "animation.controller.validate")
		{
			return ObjectSchema({
				{"assetId", IdentifierSchema()},
				{"maximumDiagnostics", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
				{"correlationId", correlation}
			}, Json::array({"assetId"}));
		}
		if(operation == "animation.controller.edit")
		{
			const Json vector3 = {
				{"type", "array"}, {"minItems", 3}, {"maxItems", 3},
				{"items", {{"type", "number"}}}
			};
			const Json quaternion = {
				{"type", "array"}, {"minItems", 4}, {"maxItems", 4},
				{"items", {{"type", "number"}}}
			};
			const Json transform = ObjectSchema({
				{"translation", vector3}, {"rotation", quaternion}, {"scale", vector3}
			}, Json::array({"translation", "rotation", "scale"}));
			const Json stableId = {
				{"type", "integer"}, {"minimum", 1},
				{"maximum", std::numeric_limits<std::uint64_t>::max()}
			};
			const Json payload = ObjectSchema({
				{"action", {{"type", "string"}, {"enum", Json::array({
					"add_parameter", "set_parameter_default", "remove_parameter",
					"add_node", "set_node", "remove_node",
					"add_state", "set_state", "remove_state",
					"add_transition", "set_transition", "remove_transition",
					"add_layer", "set_layer", "remove_layer"
				})}}},
				{"stableId", stableId},
				{"name", BoundedStringSchema(4096U)},
				{"parameterType", {{"type", "string"}, {"enum", Json::array({
					"float", "integer", "boolean", "trigger", "vector", "transform", "tag"
				})}}},
				{"floatValue", {{"type", "number"}}},
				{"integerValue", {{"type", "number"},
					{"minimum", std::numeric_limits<std::int32_t>::min()},
					{"maximum", std::numeric_limits<std::int32_t>::max()}}},
				{"booleanValue", {{"type", "boolean"}}},
				{"stringValue", {{"type", "string"}, {"maxLength", 4096U}}},
				{"vectorValue", vector3},
				{"transformValue", transform},
				{"nodeKind", {{"type", "string"}, {"enum", Json::array({
					"clip", "blend_tree_1d", "blend_tree_2d", "subgraph", "motion_match", "procedural_pose"
				})}}},
				{"flags", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"outOfDomainPolicy", {{"type", "string"}, {"enum", Json::array({"clamp", "extrapolate"})}}},
				{"dependencyAssetId", IdentifierSchema()},
				{"dependencyGeneration", stableId},
				{"dependencyFingerprint", stableId},
				{"nodeId", stableId},
				{"parentStateId", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"entry", {{"type", "boolean"}}},
				{"exit", {{"type", "boolean"}}},
				{"anyState", {{"type", "boolean"}}},
				{"sourceStateId", stableId},
				{"destinationStateId", stableId},
				{"conditionParameterId", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"durationTicks", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"exitTimeTicks", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"destinationOffsetNormalized", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
				{"priority", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"rootStateId", stableId},
				{"additive", {{"type", "boolean"}}},
				{"defaultWeight", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}}
			}, Json::array({"action", "stableId"}));
			return ObjectSchema({
				{"assetId", IdentifierSchema()},
				{"idempotencyKey", idempotency},
				{"correlationId", correlation},
				{"expectedGeneration", generation},
				{"dryRun", {{"type", "boolean"}}},
				{"payload", payload}
			}, Json::array({"assetId", "idempotencyKey", "expectedGeneration", "payload"}));
		}
		if(operation == "animation.advanced.edit")
		{
			const Json stableId = {
				{"type", "integer"}, {"minimum", 1},
				{"maximum", std::numeric_limits<std::uint64_t>::max()}
			};
			const Json boundedFloat = {
				{"type", "number"}, {"minimum", 0.0},
				{"maximum", static_cast<double>(std::numeric_limits<float>::max())}
			};
			const Json payload = ObjectSchema({
				{"action", {{"type", "string"}, {"enum", Json::array({
					"skeleton.set_joint", "skeleton.set_socket",
					"retarget.set_mapping", "retarget.set_contact", "retarget.set_quality",
					"motion.set_sample_metadata",
					"ragdoll.set_body", "ragdoll.set_blend", "ragdoll.set_constraint_limit"
				})}}},
				{"stableId", stableId},
				{"semantic", {{"type", "string"}, {"maxLength", 4096U}}},
				{"displayName", BoundedStringSchema(4096U)},
				{"deformsMesh", {{"type", "boolean"}}},
				{"jointStableId", stableId},
				{"sourceJointId", stableId},
				{"targetJointId", stableId},
				{"targetChainId", stableId},
				{"sourceSemantic", BoundedStringSchema(4096U)},
				{"targetSemantic", BoundedStringSchema(4096U)},
				{"required", {{"type", "boolean"}}},
				{"toleranceMeters", boundedFloat},
				{"preservePosition", {{"type", "boolean"}}},
				{"preserveOrientation", {{"type", "boolean"}}},
				{"reachableToleranceMeters", boundedFloat},
				{"reviewReason", {{"type", "string"}, {"maxLength", 4096U}}},
				{"authoredCoverage", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
				{"tagMask", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"contactMask", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"phaseQuantized", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint16_t>::max()}}},
				{"transitionFlags", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint16_t>::max()}}},
				{"continuityGroup", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"massKilograms", boundedFloat},
				{"linearDamping", boundedFloat},
				{"angularDamping", boundedFloat},
				{"gravityFactor", boundedFloat},
				{"allowSleep", {{"type", "boolean"}}},
				{"startAwake", {{"type", "boolean"}}},
				{"defaultWeight", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
				{"activationBlendSeconds", boundedFloat},
				{"recoveryBlendSeconds", boundedFloat},
				{"motorStrengthScale", boundedFloat},
				{"includeDescendants", {{"type", "boolean"}}},
				{"axis", {{"type", "integer"}, {"minimum", 0}, {"maximum", 5}}},
				{"limitMode", {{"type", "string"}, {"enum", Json::array({"free", "limited", "locked"})}}},
				{"minimum", {{"type", "number"}}},
				{"maximum", {{"type", "number"}}},
				{"restitution", boundedFloat},
				{"springFrequencyHertz", boundedFloat},
				{"springDampingRatio", boundedFloat}
			}, Json::array({"action"}));
			return ObjectSchema({
				{"assetId", IdentifierSchema()},
				{"idempotencyKey", idempotency},
				{"correlationId", correlation},
				{"expectedGeneration", generation},
				{"dryRun", {{"type", "boolean"}}},
				{"payload", payload}
			}, Json::array({"assetId", "idempotencyKey", "expectedGeneration", "payload"}));
		}
		if(operation == "animation.preview.play")
		{
			return ObjectSchema({
				{"assetId", IdentifierSchema()},
				{"idempotencyKey", idempotency},
				{"correlationId", correlation},
				{"expectedGeneration", generation},
				{"dryRun", {{"type", "boolean"}}},
				{"action", {{"type", "string"}, {"enum", Json::array({
					"open", "play", "pause", "resume", "stop", "seek", "step",
					"set_speed", "set_looping", "set_reverse", "tick", "close"
				})}}},
				{"seekTick", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"normalizedTime", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
				{"deltaTicks", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"deltaSeconds", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 3600.0}}},
				{"speed", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 100.0}}},
				{"flag", {{"type", "boolean"}}},
				{"looping", {{"type", "boolean"}}},
				{"reverse", {{"type", "boolean"}}}
			}, Json::array({"assetId", "idempotencyKey", "expectedGeneration", "action"}));
		}
		if(operation == "animation.recording.start")
		{
			const Json stableId = {{"type", "integer"}, {"minimum", 1},
				{"maximum", std::numeric_limits<std::uint64_t>::max()}};
			const Json target = ObjectSchema({
				{"objectId", IdentifierSchema()},
				{"animator", ObjectSchema({
					{"slotIndex", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
					{"generation", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}}
				}, Json::array({"slotIndex", "generation"}))}
			});
			const Json channel = ObjectSchema({
				{"stableChannelId", stableId}, {"stableTargetId", stableId},
				{"name", BoundedStringSchema(256U)},
				{"kind", {{"type", "string"}, {"enum", Json::array({"bone_translation", "bone_rotation", "bone_scale", "morph_weight", "curve", "event", "root_translation", "root_yaw"})}}},
				{"interpolation", {{"type", "string"}, {"enum", Json::array({"constant", "step", "linear", "cubic"})}}}
			}, Json::array({"stableChannelId", "stableTargetId", "name", "kind", "interpolation"}));
			return ObjectSchema({
				{"target", target}, {"assetId", IdentifierSchema()}, {"skeletonAssetId", IdentifierSchema()},
				{"sourceAnimatorId", IdentifierSchema()}, {"compatibilityFingerprint", stableId},
				{"publicationGeneration", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"bindCurrentPublication", {{"type", "boolean"}}},
				{"jointStableIds", {{"type", "array"}, {"minItems", 1}, {"maxItems", kAnimationAutomationMaximumItems}, {"items", stableId}}},
				{"destinationPath", BoundedStringSchema(1024U)},
				{"firstTick", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::int64_t>::max()}}},
				{"lastTick", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::int64_t>::max()}}},
				{"sampleRate", {{"type", "integer"}, {"minimum", 1}, {"maximum", 240}}},
				{"clock", {{"type", "string"}, {"enum", Json::array({"fixed_simulation", "scaled_game_time", "unscaled_game_time", "editor_preview", "manual"})}}},
				{"channels", {{"type", "array"}, {"minItems", 1}, {"maxItems", kAnimationAutomationMaximumItems}, {"items", channel}}},
				{"sourceSceneIdentity", BoundedStringSchema(1024U)}, {"sourceSessionIdentity", BoundedStringSchema(1024U)},
				{"samplesPerChunk", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}}},
				{"maximumCapturedSamples", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"maximumStagedBytes", {{"type", "integer"}, {"minimum", 64}, {"maximum", 268435456}}},
				{"segmentDurationTicks", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::int64_t>::max()}}},
				{"idempotencyKey", idempotency}, {"correlationId", correlation}, {"expectedGeneration", generation}
			}, Json::array({"target", "assetId", "skeletonAssetId", "sourceAnimatorId", "compatibilityFingerprint", "publicationGeneration", "jointStableIds", "destinationPath", "firstTick", "lastTick", "sampleRate", "clock", "channels", "sourceSceneIdentity", "sourceSessionIdentity", "segmentDurationTicks", "idempotencyKey", "expectedGeneration"}));
		}
		if(operation == "animation.recording.stop")
		{
			return ObjectSchema({{"jobId", BoundedStringSchema(128U)}, {"idempotencyKey", idempotency},
				{"correlationId", correlation}, {"expectedGeneration", generation}},
				Json::array({"jobId", "idempotencyKey", "correlationId", "expectedGeneration"}));
		}
		if (operation == "animation.camera.capture")
		{
			const Json vector = {
				{"type", "array"},
				{"minItems", 3},
				{"maxItems", 3},
				{"items", {{"type", "number"}}}
			};
			return ObjectSchema({
				{"source", {{"type", "string"}, {"enum", Json::array({"scene", "game", "camera", "custom"})}}},
				{"rendererAvailable", {{"type", "boolean"}}},
				{"correlationId", correlation},
				{"width", {{"type", "integer"}, {"minimum", 1}, {"maximum", 16384}}},
				{"height", {{"type", "integer"}, {"minimum", 1}, {"maximum", 16384}}},
				{"format", {{"type", "string"}, {"enum", Json::array({"image/png"})}}},
				{"freshness", {{"type", "string"}, {"enum", Json::array({"latestCompleted", "nextFrame"})}}},
				{"includeOverlays", {{"type", "boolean"}}},
				{"transparentBackground", {{"type", "boolean"}}},
				{"allowBackendFallback", {{"type", "boolean"}}},
				{"timeoutMs", {{"type", "integer"}, {"minimum", 1}, {"maximum", 60000}}},
				{"object", Json::object()},
				{"target", Json::object()},
				{"camera", {{"type", "object"}, {"maxProperties", 16}}},
				{"name", BoundedStringSchema(128U)},
				{"includeDisabled", {{"type", "boolean"}}},
				{"position", vector},
				{"lookAt", vector},
				{"forward", vector},
				{"up", vector},
				{"projection", {{"type", "string"}, {"enum", Json::array({"perspective", "orthographic"})}}},
				{"verticalFovDegrees", {{"type", "number"}}},
				{"orthographicHeight", {{"type", "number"}}},
				{"nearPlane", {{"type", "number"}}},
				{"farPlane", {{"type", "number"}}}
			}, Json::array({"source"}));
		}
		if(operation == "animation.runtime.parameters")
		{
			const Json vector3 = {
				{"type", "array"},
				{"minItems", 3},
				{"maxItems", 3},
				{"items", {{"type", "number"}}}
			};
			const Json quaternion = {
				{"type", "array"},
				{"minItems", 4},
				{"maxItems", 4},
				{"items", {{"type", "number"}}}
			};
			const Json transform = ObjectSchema({
				{"translation", vector3},
				{"rotation", quaternion},
				{"scale", vector3}
			}, Json::array({"translation", "rotation", "scale"}));
			const Json command = ObjectSchema({
				{"kind", {{"type", "string"}, {"enum", Json::array({
					"set_parameter", "fire_trigger", "play", "stop", "pause", "resume",
					"seek", "set_looping", "set_reverse", "set_speed"})}}},
				{"parameterId", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"parameterType", {{"type", "string"}, {"enum", Json::array({
					"boolean", "integer", "float", "vector3", "quaternion", "transform"})}}},
				{"booleanValue", {{"type", "boolean"}}},
				{"integerValue", {{"type", "number"}, {"minimum", std::numeric_limits<std::int32_t>::min()}, {"maximum", std::numeric_limits<std::int32_t>::max()}}},
				{"floatValue", {{"type", "number"}}},
				{"vectorValue", vector3},
				{"quaternionValue", quaternion},
				{"transformValue", transform},
				{"seekTick", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"flag", {{"type", "boolean"}}},
				{"speed", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 100.0}}},
				{"targetTick", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}}
			}, Json::array({"kind"}));
			const Json target = ObjectSchema({
				{"objectId", IdentifierSchema()},
				{"animator", ObjectSchema({
					{"slotIndex", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
					{"generation", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}}
				}, Json::array({"slotIndex", "generation"}))}
			});
			return ObjectSchema({
				{"target", target},
				{"commands", {{"type", "array"}, {"minItems", 1}, {"maxItems", kAnimationAutomationMaximumItems}, {"items", command}}},
				{"targetTick", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"idempotencyKey", idempotency},
				{"correlationId", correlation},
				{"expectedGeneration", generation},
				{"dryRun", {{"type", "boolean"}}}
			}, Json::array({"target", "commands", "idempotencyKey", "correlationId", "expectedGeneration"}));
		}
		if(operation == "animation.runtime.procedural.goal")
		{
			const Json vector3 = {{"type", "array"}, {"minItems", 3}, {"maxItems", 3},
				{"items", {{"type", "number"}}}};
			const Json quaternion = {{"type", "array"}, {"minItems", 4}, {"maxItems", 4},
				{"items", {{"type", "number"}}}};
			const Json transform = ObjectSchema({
				{"translation", vector3}, {"rotation", quaternion}, {"scale", vector3}
			}, Json::array({"translation", "rotation", "scale"}));
			const Json target = ObjectSchema({
				{"objectId", IdentifierSchema()},
				{"animator", ObjectSchema({
					{"slotIndex", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
					{"generation", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}}
				}, Json::array({"slotIndex", "generation"}))}
			});
			return ObjectSchema({
				{"target", target},
				{"action", {{"type", "string"}, {"enum", Json::array({"set", "clear"})}}},
				{"goalId", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"goalKind", {{"type", "string"}, {"enum", Json::array({"position", "rotation", "position_and_rotation", "aim", "look", "foot_plant", "copy_transform", "twist"})}}},
				{"coordinateSpace", {{"type", "string"}, {"enum", Json::array({"local", "parent", "model", "world"})}}},
				{"layerIndex", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"targetJointStableId", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"goalTransform", transform}, {"poleVector", vector3},
				{"priority", {{"type", "number"}, {"minimum", std::numeric_limits<std::int32_t>::min()}, {"maximum", std::numeric_limits<std::int32_t>::max()}}},
				{"positionWeight", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
				{"rotationWeight", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
				{"poleWeight", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
				{"maximumStretchScale", {{"type", "number"}, {"minimum", 1.0}, {"maximum", 4.0}}},
				{"flags", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"targetTick", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"expectedPoseGeneration", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"expectedPublicationGeneration", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"idempotencyKey", idempotency}, {"correlationId", correlation}, {"expectedGeneration", generation}, {"dryRun", {{"type", "boolean"}}}
			}, Json::array({"target", "action", "goalId", "idempotencyKey", "correlationId", "expectedGeneration"}));
		}
		if(operation == "animation.runtime.procedural.read" || operation == "animation.runtime.retarget.read")
		{
			const Json target = ObjectSchema({
				{"objectId", IdentifierSchema()},
				{"animator", ObjectSchema({
					{"slotIndex", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
					{"generation", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}}
				}, Json::array({"slotIndex", "generation"}))}
			});
			Json fields = {{"target", target}, {"maximumItems", {{"type", "integer"}, {"minimum", 1}, {"maximum", kAnimationAutomationMaximumItems}}}, {"correlationId", correlation}};
			if(operation == "animation.runtime.procedural.read") fields["includeResults"] = {{"type", "boolean"}};
			return ObjectSchema(std::move(fields), Json::array({"target"}));
		}
		if(operation == "animation.runtime.retarget.request")
		{
			const Json target = ObjectSchema({
				{"objectId", IdentifierSchema()},
				{"animator", ObjectSchema({
					{"slotIndex", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
					{"generation", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}}
				}, Json::array({"slotIndex", "generation"}))}
			});
			return ObjectSchema({
				{"target", target},
				{"action", {{"type", "string"}, {"enum", Json::array({"activate_profile", "deactivate_profile", "request_rebuild"})}}},
				{"profileAssetId", IdentifierSchema()},
				{"profileGeneration", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"layerIndex", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"expectedPoseGeneration", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"expectedPublicationGeneration", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"targetTick", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"idempotencyKey", idempotency}, {"correlationId", correlation}, {"expectedGeneration", generation}, {"dryRun", {{"type", "boolean"}}}
			}, Json::array({"target", "action", "idempotencyKey", "correlationId", "expectedGeneration"}));
		}
		if(operation == "animation.runtime.motion_match.request" || operation == "animation.runtime.ragdoll.request")
		{
			const Json target = ObjectSchema({{"objectId", IdentifierSchema()}, {"animator", ObjectSchema({
				{"slotIndex", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"generation", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}}
			}, Json::array({"slotIndex", "generation"}))}});
			const bool motion = operation == "animation.runtime.motion_match.request";
			const Json actions = motion ? Json::array({"set_database", "query", "clear"})
				: Json::array({"activate", "deactivate", "request_recovery"});
			return ObjectSchema({
				{"target", target}, {"action", {{"type", "string"}, {"enum", actions}}},
				{"assetId", IdentifierSchema()}, {"assetGeneration", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"featureCount", {{"type", "integer"}, {"minimum", 1}, {"maximum", 48}}},
				{"queryId", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"physicsWorldGeneration", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"blendWeight", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
				{"flags", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"targetTick", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint64_t>::max()}}},
				{"idempotencyKey", idempotency}, {"correlationId", correlation}, {"expectedGeneration", generation}, {"dryRun", {{"type", "boolean"}}}
			}, Json::array({"target", "action", "idempotencyKey", "correlationId", "expectedGeneration"}));
		}
		if(operation == "animation.runtime.motion_match.read" || operation == "animation.runtime.ragdoll.read")
		{
			const Json target = ObjectSchema({{"objectId", IdentifierSchema()}, {"animator", ObjectSchema({
				{"slotIndex", {{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
				{"generation", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}}
			}, Json::array({"slotIndex", "generation"}))}});
			return ObjectSchema({{"target", target}, {"maximumItems", {{"type", "integer"}, {"minimum", 1}, {"maximum", kAnimationAutomationMaximumItems}}}, {"correlationId", correlation}}, Json::array({"target"}));
		}
		if (operation == "animation.runtime.diagnostics")
		{
			return ObjectSchema({
				{"assetId", IdentifierSchema()},
				{"correlationId", correlation},
				{"maximumItems", {{"type", "integer"}, {"minimum", 1}, {"maximum", kAnimationAutomationMaximumItems}}},
				{"includeDetails", {{"type", "boolean"}}},
				{"rendererAvailable", {{"type", "boolean"}}},
				// Retain these optional version-one fields so existing clients do not
				// break when moving from the original coarse schema to this read contract.
				{"idempotencyKey", idempotency},
				{"expectedGeneration", generation}
			});
		}
		if (operation == "animation.runtime.residency" || operation == "animation.runtime.profiler")
		{
			return ObjectSchema({
				{"correlationId", correlation},
				{"maximumItems", {{"type", "integer"}, {"minimum", 1}, {"maximum", kAnimationAutomationMaximumItems}}},
				// A collapsed query is the safe default. It must not promote collection
				// to detailed mode or keep it alive after the response is copied.
				{"includeDetails", {{"type", "boolean"}}}
			});
		}
		if (operation == "animation.job.get" || operation == "animation.job.cancel")
		{
			return ObjectSchema({
				{"jobId", IdentifierSchema()},
				{"idempotencyKey", idempotency},
				{"correlationId", correlation}
			}, Json::array({"jobId"}));
		}

		return ObjectSchema({
			{"assetId", IdentifierSchema()},
			{"idempotencyKey", idempotency},
			{"correlationId", correlation},
			{"expectedGeneration", generation},
			{"dryRun", {{"type", "boolean"}}},
			{"payload", {{"type", "object"}, {"maxProperties", kAnimationAutomationMaximumItems}}},
			{"rendererAvailable", {{"type", "boolean"}}}
		}, Json::array({"assetId", "idempotencyKey", "expectedGeneration"}));
	}

	nlohmann::json AnimationAutomationResultSchema(bool jobResult)
	{
		return ObjectSchema({
			{"ok", {{"type", "boolean"}}},
			{"correlationId", IdentifierSchema()},
			{"generation", {{"type", "integer"}, {"minimum", 0}}},
			{"diagnostics", {{"type", "array"}, {"maxItems", 64}, {"items", {{"type", "object"}}}}},
			{"data", {{"type", "object"}}},
			{"jobId", IdentifierSchema()}
		}, jobResult ? Json::array({"ok", "jobId", "diagnostics"}) : Json::array({"ok", "diagnostics", "data"}));
	}

	std::optional<AutomationCapabilityDescriptor> DescribeAnimationAutomationOperation(std::string_view operation)
	{
		const std::vector<std::string> names = AnimationAutomationOperationNames();
		if (std::find(names.begin(), names.end(), operation) == names.end())
		{
			return std::nullopt;
		}

		AutomationCapabilityDescriptor descriptor;
		descriptor.name = std::string(operation);
		descriptor.version = kAnimationAutomationSchemaVersion;
		descriptor.category = "animation";
		descriptor.description = "Bounded animation authoring or runtime automation operation.";
		descriptor.inputSchema = AnimationAutomationInputSchema(operation);
		descriptor.resultSchema = AnimationAutomationResultSchema(operation.starts_with("animation.job."));
		const bool mutation = IsMutation(operation);
		const bool durableJob = operation == "animation.asset.import"
			|| operation == "animation.asset.reimport"
			|| operation == "animation.recording.start"
			|| operation == "animation.recording.stop"
			|| operation == "animation.motion_database.build";
		descriptor.limits = {
			{"maximumRequestProperties", kAnimationAutomationMaximumItems},
			{"maximumPayloadProperties", kAnimationAutomationMaximumItems},
			{"maximumNestedItems", kAnimationAutomationMaximumItems},
			{"maximumJsonDepth", 16U},
			{"maximumJsonNodes", 4096U},
			{"maximumStringBytes", 4096U},
			{"maximumResultDiagnostics", 64U},
			{"defaultConcurrentJobs", 16U},
			{"hardConcurrentJobs", 64U},
			{"retainedMutationReceipts", 256U}
		};
		descriptor.generationContract = {
			{"scope", operation.starts_with("animation.asset.import") || operation == "animation.asset.reimport"
				|| operation == "animation.recording.start" || operation == "animation.recording.stop"
				? "open_project_and_published_asset"
				: "target_animation_document_or_runtime_instance"},
			// Recording is durable but still begins against a selected editor/project
			// generation. Its schema therefore requires the same explicit precondition
			// rather than treating the job lifetime as permission to skip it.
			{"expectedGenerationRequired", (mutation && !durableJob)
				|| operation == "animation.recording.start"},
			{"staleApplication", "reject_atomically"},
			{"publication", mutation ? "protected_atomic_swap" : "immutable_snapshot"}
		};
		descriptor.jobBehavior = {
			{"returnsDurableJob", durableJob},
			{"pollOperation", durableJob ? "animation.job.get" : ""},
			{"cancelOperation", durableJob ? "animation.job.cancel" : ""},
			{"progressRange", nlohmann::json::array({0.0, 1.0})},
			{"retention", "bounded_for_editor_session"},
			{"reconnectReadable", durableJob},
			{"defaultTimeoutMs", durableJob ? 300000U : 0U},
			{"maximumTimeoutMs", durableJob ? 3600000U : 0U}
		};
		descriptor.diagnosticsContract = {
			{"maximumItems", 64U},
			{"requiredFields", nlohmann::json::array({
				"category", "status", "correlationId", "generation", "recommendedAction"
			})},
			{"bounded", true}
		};
		descriptor.permission = mutation ? AutomationPermission::ProjectWrite : AutomationPermission::Read;
		descriptor.approvalPolicy = operation == "animation.asset.import" || operation == "animation.asset.reimport"
			|| operation == "animation.recording.start" || operation == "animation.recording.stop"
			|| operation == "animation.motion_database.build"
			? AutomationApprovalPolicy::HighImpact
			: AutomationApprovalPolicy::None;
		// Capability metadata must name the state that is actually changed. Runtime
		// commands enter the protected animation queue, preview controls affect only
		// editor state, and job cancellation changes the durable editor job ledger.
		if (operation == "animation.runtime.parameters"
			|| operation == "animation.runtime.procedural.goal"
			|| operation == "animation.runtime.retarget.request"
			|| operation == "animation.runtime.motion_match.request"
			|| operation == "animation.runtime.ragdoll.request")
		{
			descriptor.mutationMode = MutationMode::SceneMutation;
		}
		else if (operation == "animation.preview.play")
		{
			descriptor.mutationMode = MutationMode::EditorSelection;
		}
		else if (operation == "animation.job.cancel")
		{
			descriptor.mutationMode = MutationMode::ProjectMutation;
		}
		else
		{
			descriptor.mutationMode = mutation ? MutationMode::AssetMutation : MutationMode::ReadOnly;
		}
		descriptor.availability.requiresProject = true;
		descriptor.execution.workerPreparation = true;
		descriptor.execution.protectedCommit = mutation;
			descriptor.execution.visibilityBoundary = operation == "animation.runtime.parameters"
			|| operation == "animation.runtime.procedural.goal"
			|| operation == "animation.runtime.retarget.request"
			|| operation == "animation.runtime.motion_match.request"
			|| operation == "animation.runtime.ragdoll.request"
			? "animation_protected_capture_then_publication"
			: (operation == "animation.recording.start" || operation == "animation.recording.stop"
				? "animation_protected_snapshot_capture_then_atomic_publish"
			: (operation == "animation.motion_database.build"
				? "motion_database_worker_then_atomic_publish"
			: (operation == "animation.runtime.residency" || operation == "animation.runtime.profiler"
				? "copied_animation_diagnostics_snapshot"
				: (mutation ? "animation_authoring_atomic_publish" : "immutable_animation_snapshot"))));
		descriptor.execution.preparationReads = {"AnimationSnapshot", "EditorAutomationSession"};
		if (operation == "animation.runtime.parameters")
		{
			descriptor.execution.commitWrites = {"AnimationCommandQueue"};
		}
		else if(operation == "animation.runtime.procedural.goal")
		{
			descriptor.execution.commitWrites = {"ProceduralCommandStore"};
		}
		else if(operation == "animation.runtime.retarget.request")
		{
			descriptor.execution.commitWrites = {"AdvancedAnimationCommandStore"};
		}
		else if(operation == "animation.runtime.motion_match.request" || operation == "animation.runtime.ragdoll.request")
		{
			descriptor.execution.commitWrites = {"AdvancedAnimationCommandStore"};
		}
		else if(operation == "animation.preview.play")
		{
			descriptor.execution.commitWrites = {"IsolatedAnimationPreviewWorld"};
		}
		else if(operation == "animation.recording.start" || operation == "animation.recording.stop")
		{
			descriptor.execution.commitWrites = {"EditorAutomationJobs", "AnimationRecorderSession"};
		}
		else if(operation == "animation.motion_database.build")
		{
			descriptor.execution.commitWrites = {"EditorAutomationJobs", "MotionDatabaseAuthoringSession"};
		}
		else if (mutation)
		{
			descriptor.execution.commitWrites = {"AnimationAssets", "EditorAutomationJobs"};
		}
		return descriptor;
	}

	AutomationDispatchResult ValidateAnimationAutomationRequest(
		std::string_view operation,
		const nlohmann::json& arguments)
	{
		AutomationDispatchResult result;
		if (!DescribeAnimationAutomationOperation(operation))
		{
			result.error = ErrorCategory::UnsupportedCapability;
			result.message = "Unknown animation automation operation.";
			return result;
		}
		if (!arguments.is_object() || arguments.size() > kAnimationAutomationMaximumItems)
		{
			result.error = ErrorCategory::Validation;
			result.message = "Animation request must be a bounded object.";
			return result;
		}
		std::size_t visitedNodes = 0U;
		if (const std::optional<std::string> boundedError = ValidateBoundedJson(
			arguments,
			"arguments",
			0U,
			visitedNodes))
		{
			result.error = ErrorCategory::Validation;
			result.message = *boundedError;
			return result;
		}

		const Json schema = AnimationAutomationInputSchema(operation);
		for (const Json& required : schema.value("required", Json::array()))
		{
			const std::string key = required.get<std::string>();
			if (!arguments.contains(key))
			{
				result.error = ErrorCategory::Validation;
				result.message = "Missing required animation argument: " + key;
				return result;
			}
		}
		const Json& properties = schema["properties"];
		for (const auto& [key, value] : arguments.items())
		{
			if (!properties.contains(key))
			{
				result.error = ErrorCategory::Validation;
				result.message = "Unsupported animation argument: " + key;
				return result;
			}
			if (const std::optional<std::string> error = ValidateValueAgainstSchema(key, value, properties[key]))
			{
				result.error = ErrorCategory::Validation;
				result.message = *error;
				return result;
			}
		}
		const auto rejectConditional = [&result](std::string message) -> AutomationDispatchResult {
			result.error = ErrorCategory::Validation;
			result.message = std::move(message);
			return result;
		};
		if(operation == "animation.controller.edit")
		{
			const Json& payload = arguments["payload"];
			const std::string action = payload.value("action", std::string{});
			if(action == "add_parameter"
				&& (!payload.contains("name") || !payload.contains("parameterType")))
			{
				return rejectConditional("add_parameter requires payload.name and payload.parameterType.");
			}
			if(action == "add_node" && !payload.contains("nodeKind"))
			{
				return rejectConditional("add_node requires payload.nodeKind.");
			}
			if(action == "add_state" && !payload.contains("nodeId"))
			{
				return rejectConditional("add_state requires payload.nodeId.");
			}
			if(action == "add_transition"
				&& (!payload.contains("sourceStateId") || !payload.contains("destinationStateId")))
			{
				return rejectConditional(
					"add_transition requires payload.sourceStateId and payload.destinationStateId.");
			}
			if(action == "add_layer" && !payload.contains("rootStateId"))
			{
				return rejectConditional("add_layer requires payload.rootStateId.");
			}
			if(payload.contains("integerValue")
				&& !payload["integerValue"].is_number_integer()
				&& !payload["integerValue"].is_number_unsigned())
			{
				return rejectConditional("payload.integerValue must be an exact integer.");
			}
		}
		if(operation == "animation.advanced.edit")
		{
			const Json& payload = arguments["payload"];
			const std::string action = payload.value("action", std::string{});
			if(action != "retarget.set_quality" && !payload.contains("stableId"))
			{
				return rejectConditional("The selected advanced edit action requires payload.stableId.");
			}
			if(action == "ragdoll.set_constraint_limit" && !payload.contains("axis"))
			{
				return rejectConditional("ragdoll.set_constraint_limit requires payload.axis.");
			}
		}
		if(operation == "animation.preview.play")
		{
			const std::string action = arguments.value("action", std::string{});
			if(action == "seek"
				&& (arguments.contains("seekTick") == arguments.contains("normalizedTime")))
			{
				return rejectConditional("seek requires exactly one of seekTick or normalizedTime.");
			}
			if(action == "step" && !arguments.contains("deltaTicks"))
			{
				return rejectConditional("step requires deltaTicks.");
			}
			if(action == "tick" && !arguments.contains("deltaSeconds"))
			{
				return rejectConditional("tick requires deltaSeconds.");
			}
			if(action == "set_speed" && !arguments.contains("speed"))
			{
				return rejectConditional("set_speed requires speed.");
			}
			if((action == "set_looping" || action == "set_reverse")
				&& !arguments.contains("flag"))
			{
				return rejectConditional("The preview flag command requires flag.");
			}
		}
		if(operation == "animation.runtime.procedural.goal")
		{
			const std::string action = arguments.value("action", std::string{});
			if(action == "set")
			{
				for(const char* field : {"goalKind", "coordinateSpace", "targetJointStableId", "goalTransform", "poleVector",
					"priority", "positionWeight", "rotationWeight", "poleWeight", "maximumStretchScale",
					"expectedPoseGeneration", "expectedPublicationGeneration"})
				{
					if(!arguments.contains(field)) return rejectConditional(
						std::string("procedural set requires ") + field + ".");
				}
				if(arguments.contains("targetObjectId") || arguments.contains("targetObjectGeneration")
					|| arguments.contains("targetComponentGeneration"))
				{
					return rejectConditional("procedural goals accept a copied direct transform, not an object-following target.");
				}
				if(!arguments["priority"].is_number_integer() && !arguments["priority"].is_number_unsigned())
				{
					return rejectConditional("procedural priority must be an exact signed 32-bit integer.");
				}
			}
			else
			{
				// Clear has a deliberately zero-only ABI payload. Rejecting set-only
				// inputs avoids silently accepting fields the protected store ignores.
				for(const char* field : {"goalKind", "coordinateSpace", "layerIndex", "targetJointStableId", "goalTransform",
					"poleVector", "priority", "positionWeight", "rotationWeight", "poleWeight", "maximumStretchScale",
					"flags", "expectedPoseGeneration", "expectedPublicationGeneration"})
				{
					if(arguments.contains(field)) return rejectConditional(
						std::string("procedural clear must not provide ") + field + ".");
				}
			}
		}
		if(operation == "animation.runtime.retarget.request")
		{
			const std::string action = arguments.value("action", std::string{});
			const bool needsProfile = action == "activate_profile" || action == "request_rebuild";
			if(needsProfile && (!arguments.contains("profileAssetId") || !arguments.contains("profileGeneration")))
			{
				return rejectConditional("retarget activation and rebuild require profileAssetId and profileGeneration.");
			}
			if(action == "deactivate_profile" && (arguments.contains("profileAssetId") || arguments.contains("profileGeneration")))
			{
				return rejectConditional("retarget deactivation must not provide a profile identity.");
			}
		}
		if(operation == "animation.runtime.motion_match.request")
		{
			const std::string action = arguments.value("action", std::string{});
			const bool needsDatabase = action == "set_database" || action == "query";
			if(needsDatabase && (!arguments.contains("assetId") || !arguments.contains("assetGeneration")))
			{
				return rejectConditional("motion database selection and queries require assetId and assetGeneration.");
			}
			if(action == "query" && !arguments.contains("queryId"))
			{
				return rejectConditional("motion queries require a caller-owned queryId.");
			}
			if(action == "clear" && (arguments.contains("assetId") || arguments.contains("assetGeneration")
				|| arguments.contains("featureCount") || arguments.contains("queryId")))
			{
				return rejectConditional("motion clear must not provide database or query fields.");
			}
		}
		if(operation == "animation.runtime.ragdoll.request")
		{
			for(const char* field : {"assetId", "assetGeneration", "physicsWorldGeneration"})
			{
				if(!arguments.contains(field)) return rejectConditional(
					std::string("ragdoll requests require ") + field + ".");
			}
		}

		result.ok = true;
		result.error = ErrorCategory::Internal;
		result.message = "Validated the bounded animation automation request.";
		result.data = {{"schemaVersion", kAnimationAutomationSchemaVersion}};
		return result;
	}
}
