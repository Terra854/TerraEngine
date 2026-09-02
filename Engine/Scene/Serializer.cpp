// Serializer.cpp — Scene Serialization System Implementation
//
// Implements the detailed serialization and deserialization logic for the engine's scene and prefab formats.
// It manages the recursive processing of entity hierarchies and coordinates with the reflection system to handle component data.
// This module provides the backend support for scene persistence, allowing for stable asset storage and versioning.

#include "Scene/Serializer.h"
#include "Components/AnimatorComponent.h"
#include "Components/AudioListenerComponent.h"
#include "Components/AudioSourceComponent.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/TerrainComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Component.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/ConstraintSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <charconv>
#include <cmath>
#include <limits>

namespace {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Animation;

	nlohmann::json SerializeAnimator(const AnimatorComponent& animator)
	{
		// Runtime handles, effective execution mode, poses, cursors, and receipts
		// are deliberately absent. Missing asset UUIDs remain authored strings so a
		// later mount/reimport can resolve them without rewriting the scene.
		return {
			{"active", animator.active},
			{"schemaVersion", kAnimatorComponentSchemaVersion},
			{"controllerAssetId", TerraEngine::ToHexString(animator.controllerAssetId)},
			{"defaultClipAssetId", TerraEngine::ToHexString(animator.defaultClipAssetId)},
			{"loopDefaultClip", animator.loopDefaultClip},
			{"playAutomatically", animator.playAutomatically},
			{"defaultClipStartFrame", animator.defaultClipStartFrame},
			{"skeletonOverrideAssetId", TerraEngine::ToHexString(animator.skeletonOverrideAssetId)},
			{"avatarOverrideAssetId", TerraEngine::ToHexString(animator.avatarOverrideAssetId)},
			{"retargetProfileOverrideAssetId", TerraEngine::ToHexString(animator.retargetProfileOverrideAssetId)},
			{"ragdollProfileOverrideAssetId", TerraEngine::ToHexString(animator.ragdollProfileOverrideAssetId)},
			{"motionDatabaseOverrideAssetId", TerraEngine::ToHexString(animator.motionDatabaseOverrideAssetId)},
			{"requestedMode", static_cast<std::uint32_t>(animator.requestedMode)},
			{"rootMotionPolicy", static_cast<std::uint32_t>(animator.rootMotionPolicy)},
			{"updatePolicy", static_cast<std::uint32_t>(animator.updatePolicy)},
			{"timePolicy", static_cast<std::uint32_t>(animator.timePolicy)},
			{"initialParameters", animator.initialParametersJson},
			{"eventPolicy", static_cast<std::uint32_t>(animator.eventPolicy)},
			{"cullingPolicy", static_cast<std::uint32_t>(animator.cullingPolicy)},
			{"diagnosticOverrides", animator.diagnosticOverrides}
		};
	}

	bool ReadAnimatorUuid(
		const nlohmann::json& json,
		std::string_view key,
		TerraEngine::UUID& destination)
	{
		const auto found = json.find(std::string(key));
		if(found == json.end()) return true;
		if(!found->is_string()) return false;
		const std::optional<TerraEngine::UUID> parsed = TerraEngine::ParseUUID(found->get<std::string>());
		if(!parsed) return false;
		destination = *parsed;
		return true;
	}

	template<typename Enum>
	bool ReadAnimatorEnum(
		const nlohmann::json& json,
		std::string_view key,
		std::uint32_t maximum,
		Enum fallback,
		Enum& destination)
	{
		const auto found = json.find(std::string(key));
		if(found == json.end()) return true;
		// Persisted policy values are untrusted input. Keep the component alive
		// and restore the policy's documented safe default instead of dropping
		// the whole authored component when a value is unknown or malformed.
		if(!found->is_number_unsigned() && !found->is_number_integer()) {
			destination = fallback;
			return true;
		}
		if(found->is_number_unsigned()) {
			const std::uint64_t raw = found->get<std::uint64_t>();
			if(raw > maximum) {
				destination = fallback;
				return true;
			}
			destination = static_cast<Enum>(raw);
			return true;
		}
		const std::int64_t raw = found->get<std::int64_t>();
		if(raw < 0 || static_cast<std::uint64_t>(raw) > maximum) {
			destination = fallback;
			return true;
		}
		destination = static_cast<Enum>(raw);
		return true;
	}

	bool ReadAnimatorUnsigned(
		const nlohmann::json& json,
		std::string_view key,
		std::uint32_t& destination)
	{
		const auto found = json.find(std::string(key));
		if(found == json.end()) return true;
		// Authored frame controls are untrusted scene input. Reject negative,
		// fractional, and overflowing values before committing any component field.
		if(found->is_number_unsigned()) {
			const std::uint64_t raw = found->get<std::uint64_t>();
			if(raw > std::numeric_limits<std::uint32_t>::max()) return false;
			destination = static_cast<std::uint32_t>(raw);
			return true;
		}
		if(!found->is_number_integer()) return false;
		const std::int64_t raw = found->get<std::int64_t>();
		if(raw < 0
			|| static_cast<std::uint64_t>(raw) > std::numeric_limits<std::uint32_t>::max()) {
			return false;
		}
		destination = static_cast<std::uint32_t>(raw);
		return true;
	}

	bool DeserializeAnimator(const nlohmann::json& json, AnimatorComponent& destination)
	{
		if(!json.is_object()) return false;
		AnimatorComponent staged;
		try {
			const std::uint32_t schema = json.value("schemaVersion", kAnimatorComponentSchemaVersion);
			if(schema > kAnimatorComponentSchemaVersion) return false;
			staged.schemaVersion = kAnimatorComponentSchemaVersion;
			staged.active = json.value("active", staged.active);
			if(!ReadAnimatorUuid(json, "controllerAssetId", staged.controllerAssetId)
				|| !ReadAnimatorUuid(json, "defaultClipAssetId", staged.defaultClipAssetId)
				|| !ReadAnimatorUuid(json, "skeletonOverrideAssetId", staged.skeletonOverrideAssetId)
				|| !ReadAnimatorUuid(json, "avatarOverrideAssetId", staged.avatarOverrideAssetId)
				|| !ReadAnimatorUuid(json, "retargetProfileOverrideAssetId", staged.retargetProfileOverrideAssetId)
				|| !ReadAnimatorUuid(json, "ragdollProfileOverrideAssetId", staged.ragdollProfileOverrideAssetId)
				|| !ReadAnimatorUuid(json, "motionDatabaseOverrideAssetId", staged.motionDatabaseOverrideAssetId)) return false;
			if(const auto loop = json.find("loopDefaultClip"); loop != json.end()) {
				// Treat a malformed authored switch as a rejected component instead
				// of silently turning a one-shot clip into different runtime behavior.
				if(!loop->is_boolean()) return false;
				staged.loopDefaultClip = loop->get<bool>();
			}
			if(const auto autoplay = json.find("playAutomatically"); autoplay != json.end()) {
				// Reject malformed switches atomically instead of silently changing
				// whether a saved scene advances when Play Mode begins.
				if(!autoplay->is_boolean()) return false;
				staged.playAutomatically = autoplay->get<bool>();
			}
			if(!ReadAnimatorUnsigned(
				json, "defaultClipStartFrame", staged.defaultClipStartFrame)) return false;
			if(!ReadAnimatorEnum(json, "requestedMode",
				static_cast<std::uint32_t>(AnimationExecutionPolicy::ForceCpuDeterministicFallback), AnimationExecutionPolicy::Auto, staged.requestedMode)
				|| !ReadAnimatorEnum(json, "rootMotionPolicy",
					static_cast<std::uint32_t>(AnimatorRootMotionPolicy::ApplyToCharacter), AnimatorRootMotionPolicy::Ignore, staged.rootMotionPolicy)
				|| !ReadAnimatorEnum(json, "updatePolicy",
					static_cast<std::uint32_t>(AnimatorUpdatePolicy::Manual), AnimatorUpdatePolicy::EveryFrame, staged.updatePolicy)
				|| !ReadAnimatorEnum(json, "timePolicy",
					static_cast<std::uint32_t>(AnimatorTimePolicy::Fixed), AnimatorTimePolicy::Scaled, staged.timePolicy)
				|| !ReadAnimatorEnum(json, "eventPolicy",
					static_cast<std::uint32_t>(AnimatorEventPolicy::Suppress), AnimatorEventPolicy::Dispatch, staged.eventPolicy)
				|| !ReadAnimatorEnum(json, "cullingPolicy",
					static_cast<std::uint32_t>(AnimatorCullingPolicy::SuspendVisualWhenInvisible), AnimatorCullingPolicy::Automatic, staged.cullingPolicy)) return false;

			if(const auto parameters = json.find("initialParameters"); parameters != json.end()) {
				if(parameters->is_string()) staged.initialParametersJson = parameters->get<std::string>();
				else if(parameters->is_object()) staged.initialParametersJson = parameters->dump();
				else return false;
			}
			const nlohmann::json initialParameters = nlohmann::json::parse(staged.initialParametersJson);
			if(!initialParameters.is_object()) return false;
			staged.diagnosticOverrides = json.value("diagnosticOverrides", staged.diagnosticOverrides);
		} catch(const nlohmann::json::exception&) {
			return false;
		}

		// Commit only after every field and nested initial-parameter payload passed.
		// No parse/validation can fail after this point, so rejected components leave
		// no partially updated authored state behind.
		destination.active = staged.active;
		destination.schemaVersion = staged.schemaVersion;
		destination.controllerAssetId = staged.controllerAssetId;
		destination.defaultClipAssetId = staged.defaultClipAssetId;
		destination.loopDefaultClip = staged.loopDefaultClip;
		destination.playAutomatically = staged.playAutomatically;
		destination.defaultClipStartFrame = staged.defaultClipStartFrame;
		destination.skeletonOverrideAssetId = staged.skeletonOverrideAssetId;
		destination.avatarOverrideAssetId = staged.avatarOverrideAssetId;
		destination.retargetProfileOverrideAssetId = staged.retargetProfileOverrideAssetId;
		destination.ragdollProfileOverrideAssetId = staged.ragdollProfileOverrideAssetId;
		destination.motionDatabaseOverrideAssetId = staged.motionDatabaseOverrideAssetId;
		destination.requestedMode = staged.requestedMode;
		destination.rootMotionPolicy = staged.rootMotionPolicy;
		destination.updatePolicy = staged.updatePolicy;
		destination.timePolicy = staged.timePolicy;
		destination.initialParametersJson = std::move(staged.initialParametersJson);
		destination.eventPolicy = staged.eventPolicy;
		destination.cullingPolicy = staged.cullingPolicy;
		destination.diagnosticOverrides = staged.diagnosticOverrides;
		return true;
	}

	nlohmann::json PhysicsVectorJson(const PhysicsVector3& value) {
		return {value.x, value.y, value.z};
	}

	nlohmann::json PhysicsRotationJson(const PhysicsQuaternion& value) {
		return {value.w, value.x, value.y, value.z};
	}

	bool ReadPhysicsVector(const nlohmann::json& value, PhysicsVector3& result) {
		if(!value.is_array() || value.size() != 3U) return false;
		try {
			result = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
			return result.IsFinite();
		} catch(...) {
			return false;
		}
	}

	bool ReadPhysicsRotation(const nlohmann::json& value, PhysicsQuaternion& result) {
		if(!value.is_array() || value.size() != 4U) return false;
		try {
			result = {value[1].get<float>(), value[2].get<float>(), value[3].get<float>(), value[0].get<float>()};
			return result.IsFinite();
		} catch(...) {
			return false;
		}
	}

	std::string_view MassModeName(PhysicsMassMode mode) {
		return mode == PhysicsMassMode::ExplicitMass ? "ExplicitMass" : "AutomaticFromDensity";
	}

	std::string_view InterpolationName(PhysicsInterpolationMode mode) {
		switch(mode) {
		case PhysicsInterpolationMode::None: return "None";
		case PhysicsInterpolationMode::Interpolate: return "Interpolate";
		case PhysicsInterpolationMode::Extrapolate: return "Extrapolate";
		}
		return "None";
	}

	nlohmann::json SerializeRigidBody(const RigidBodyComponent& body) {
		return {
			{"active", body.active},
			{"schemaVersion", body.schemaVersion},
			{"motionType", ToString(body.motionType)},
			{"massMode", MassModeName(body.massMode)},
			{"mass", body.mass},
			{"linearDamping", body.linearDamping},
			{"angularDamping", body.angularDamping},
			{"useGravity", body.useGravity},
			{"allowSleeping", body.allowSleeping},
			{"startAwake", body.startAwake},
			{"continuousCollisionMode", ToString(body.continuousCollisionMode)},
			{"initialLinearVelocity", PhysicsVectorJson(body.initialLinearVelocity)},
			{"initialAngularVelocity", PhysicsVectorJson(body.initialAngularVelocity)},
			{"maxLinearVelocity", body.maxLinearVelocity},
			{"maxAngularVelocity", body.maxAngularVelocity},
			{"allowedDegreesOfFreedom", body.allowedDegreesOfFreedom},
			{"interpolationMode", InterpolationName(body.interpolationMode)},
			{"collisionLayerId", body.collisionLayerId},
			{"useCollisionMaskOverride", body.useCollisionMaskOverride},
			{"collisionMaskOverride", body.collisionMaskOverride},
			{"enabled", body.enabled}
		};
	}

	bool DeserializeRigidBody(const nlohmann::json& json, RigidBodyComponent& destination) {
		if(!json.is_object()) return false;
		RigidBodyComponent body;
		try {
			body.active = json.value("active", body.active);
			body.schemaVersion = json.value("schemaVersion", body.schemaVersion);
			const std::string motion = json.value("motionType", std::string("Dynamic"));
			if(motion == "Static") body.motionType = PhysicsMotionType::Static;
			else if(motion == "Dynamic") body.motionType = PhysicsMotionType::Dynamic;
			else if(motion == "Kinematic") body.motionType = PhysicsMotionType::Kinematic;
			else return false;
			const std::string massMode = json.value("massMode", std::string("AutomaticFromDensity"));
			if(massMode == "AutomaticFromDensity") body.massMode = PhysicsMassMode::AutomaticFromDensity;
			else if(massMode == "ExplicitMass") body.massMode = PhysicsMassMode::ExplicitMass;
			else return false;
			body.mass = json.value("mass", body.mass);
			body.linearDamping = json.value("linearDamping", body.linearDamping);
			body.angularDamping = json.value("angularDamping", body.angularDamping);
			body.useGravity = json.value("useGravity", body.useGravity);
			body.allowSleeping = json.value("allowSleeping", body.allowSleeping);
			body.startAwake = json.value("startAwake", body.startAwake);
			const std::string quality = json.value("continuousCollisionMode", std::string("Discrete"));
			if(quality == "Discrete") body.continuousCollisionMode = PhysicsMotionQuality::Discrete;
			else if(quality == "LinearCast") body.continuousCollisionMode = PhysicsMotionQuality::LinearCast;
			else return false;
			if(const auto found = json.find("initialLinearVelocity"); found != json.end()
				&& !ReadPhysicsVector(*found, body.initialLinearVelocity)) return false;
			if(const auto found = json.find("initialAngularVelocity"); found != json.end()
				&& !ReadPhysicsVector(*found, body.initialAngularVelocity)) return false;
			body.maxLinearVelocity = json.value("maxLinearVelocity", body.maxLinearVelocity);
			body.maxAngularVelocity = json.value("maxAngularVelocity", body.maxAngularVelocity);
			body.allowedDegreesOfFreedom = json.value("allowedDegreesOfFreedom", body.allowedDegreesOfFreedom);
			const std::string interpolation = json.value("interpolationMode", std::string("Interpolate"));
			if(interpolation == "None") body.interpolationMode = PhysicsInterpolationMode::None;
			else if(interpolation == "Interpolate") body.interpolationMode = PhysicsInterpolationMode::Interpolate;
			else if(interpolation == "Extrapolate") body.interpolationMode = PhysicsInterpolationMode::Extrapolate;
			else return false;
			body.collisionLayerId = json.value("collisionLayerId", body.collisionLayerId);
			body.useCollisionMaskOverride = json.value("useCollisionMaskOverride", body.useCollisionMaskOverride);
			body.collisionMaskOverride = json.value("collisionMaskOverride", body.collisionMaskOverride);
			body.enabled = json.value("enabled", body.enabled);
		} catch(...) {
			return false;
		}
		if(!body.Validate().accepted) return false;
		destination = body;
		return true;
	}

	std::string_view EventPolicyName(PhysicsColliderEventPolicy policy) {
		switch(policy) {
		case PhysicsColliderEventPolicy::None: return "None";
		case PhysicsColliderEventPolicy::Collision: return "Collision";
		case PhysicsColliderEventPolicy::Trigger: return "Trigger";
		case PhysicsColliderEventPolicy::All: return "All";
		}
		return "None";
	}

	nlohmann::json SerializeColliderShape(const PrimitiveColliderShape& shape) {
		return std::visit([](const auto& payload) -> nlohmann::json {
			using Payload = std::decay_t<decltype(payload)>;
			if constexpr(std::is_same_v<Payload, BoxColliderShape>)
				return {{"type", "Box"}, {"halfExtents", PhysicsVectorJson(payload.halfExtents)}};
			else if constexpr(std::is_same_v<Payload, SphereColliderShape>)
				return {{"type", "Sphere"}, {"radius", payload.radius}};
			else if constexpr(std::is_same_v<Payload, CapsuleColliderShape>)
				return {{"type", "Capsule"}, {"radius", payload.radius}, {"height", payload.height}};
			else if constexpr(std::is_same_v<Payload, CylinderColliderShape>)
				return {{"type", "Cylinder"}, {"radius", payload.radius}, {"height", payload.height}};
			else if constexpr(std::is_same_v<Payload, TaperedCapsuleColliderShape>)
				return {{"type", "TaperedCapsule"}, {"topRadius", payload.topRadius},
					{"bottomRadius", payload.bottomRadius}, {"height", payload.height}};
			else if constexpr(std::is_same_v<Payload, ConvexHullColliderShape>)
				return {{"type", "ConvexHull"}, {"colliderAssetId", TerraEngine::ToHexString(payload.colliderAssetId)}};
			else if constexpr(std::is_same_v<Payload, TriangleMeshColliderShape>)
				return {{"type", "TriangleMesh"}, {"colliderAssetId", TerraEngine::ToHexString(payload.colliderAssetId)}};
			else
				return {{"type", "Compound"}, {"colliderAssetId", TerraEngine::ToHexString(payload.colliderAssetId)}};
		}, shape);
	}

	bool DeserializeColliderShape(const nlohmann::json& json, PrimitiveColliderShape& shape) {
		if(!json.is_object()) return false;
		try {
			const std::string type = json.value("type", std::string());
			if(type == "Box") {
				BoxColliderShape value;
				const auto found = json.find("halfExtents");
				if(found == json.end() || !ReadPhysicsVector(*found, value.halfExtents)) return false;
				shape = value;
			} else if(type == "Sphere") {
				shape = SphereColliderShape{json.at("radius").get<float>()};
			} else if(type == "Capsule") {
				shape = CapsuleColliderShape{json.at("radius").get<float>(), json.at("height").get<float>()};
			} else if(type == "Cylinder") {
				shape = CylinderColliderShape{json.at("radius").get<float>(), json.at("height").get<float>()};
			} else if(type == "TaperedCapsule") {
				shape = TaperedCapsuleColliderShape{
					json.at("topRadius").get<float>(), json.at("bottomRadius").get<float>(), json.at("height").get<float>()};
			} else if(type == "ConvexHull" || type == "TriangleMesh" || type == "Compound") {
				const auto assetId = TerraEngine::ParseUUID(json.at("colliderAssetId").get<std::string>());
				if(!assetId || *assetId == TerraEngine::EMPTY_UUID) return false;
				if(type == "ConvexHull") shape = ConvexHullColliderShape{*assetId};
				else if(type == "TriangleMesh") shape = TriangleMeshColliderShape{*assetId};
				else shape = CompoundColliderShape{*assetId};
			} else return false;
			return true;
		} catch(...) {
			return false;
		}
	}

	nlohmann::json SerializeColliderSet(const ColliderSetComponent& set) {
		nlohmann::json colliders = nlohmann::json::array();
		for(const ColliderRecord& record : set.colliders) {
			nlohmann::json json{
				{"recordId", TerraEngine::ToHexString(record.recordId)},
				{"recordVersion", record.recordVersion},
				{"localCenter", PhysicsVectorJson(record.localCenter)},
				{"localRotation", PhysicsRotationJson(record.localRotation)},
				{"shape", SerializeColliderShape(record.shape)},
				{"materialId", TerraEngine::ToHexString(record.materialId)},
				{"useDefaultMaterial", record.useDefaultMaterial},
				{"isTrigger", record.isTrigger},
				{"useLayerOverride", record.useLayerOverride},
				{"collisionLayerId", record.collisionLayerId},
				{"eventPolicy", EventPolicyName(record.eventPolicy)},
				{"queryFlags", record.queryFlags},
				{"enabled", record.enabled},
				{"hasDebugColorOverride", record.hasDebugColorOverride}
			};
			if(record.hasDebugColorOverride) {
				json["debugColor"] = {record.debugColor.red, record.debugColor.green,
					record.debugColor.blue, record.debugColor.alpha};
			}
			colliders.push_back(std::move(json));
		}
		return {{"active", set.active}, {"schemaVersion", set.schemaVersion}, {"colliders", std::move(colliders)}};
	}

	bool DeserializeColliderSet(const nlohmann::json& json, ColliderSetComponent& destination) {
		if(!json.is_object() || !json.contains("colliders") || !json["colliders"].is_array()) return false;
		ColliderSetComponent set;
		try {
			set.active = json.value("active", set.active);
			set.schemaVersion = json.value("schemaVersion", set.schemaVersion);
			for(const nlohmann::json& source : json["colliders"]) {
				ColliderRecord record;
				const auto recordId = TerraEngine::ParseUUID(source.value("recordId", std::string()));
				const auto materialId = TerraEngine::ParseUUID(source.value("materialId", std::string(32U, '0')));
				if(!recordId || !materialId) return false;
				record.recordId = *recordId;
				record.materialId = *materialId;
				record.recordVersion = source.value("recordVersion", record.recordVersion);
				if(const auto found = source.find("localCenter"); found == source.end()
					|| !ReadPhysicsVector(*found, record.localCenter)) return false;
				if(const auto found = source.find("localRotation"); found == source.end()
					|| !ReadPhysicsRotation(*found, record.localRotation)) return false;
				if(const auto found = source.find("shape"); found == source.end()
					|| !DeserializeColliderShape(*found, record.shape)) return false;
				record.useDefaultMaterial = source.value("useDefaultMaterial", record.useDefaultMaterial);
				record.isTrigger = source.value("isTrigger", record.isTrigger);
				record.useLayerOverride = source.value("useLayerOverride", record.useLayerOverride);
				record.collisionLayerId = source.value("collisionLayerId", record.collisionLayerId);
				const std::string eventPolicy = source.value("eventPolicy", std::string("All"));
				if(eventPolicy == "None") record.eventPolicy = PhysicsColliderEventPolicy::None;
				else if(eventPolicy == "Collision") record.eventPolicy = PhysicsColliderEventPolicy::Collision;
				else if(eventPolicy == "Trigger") record.eventPolicy = PhysicsColliderEventPolicy::Trigger;
				else if(eventPolicy == "All") record.eventPolicy = PhysicsColliderEventPolicy::All;
				else return false;
				record.queryFlags = source.value("queryFlags", record.queryFlags);
				record.enabled = source.value("enabled", record.enabled);
				record.hasDebugColorOverride = source.value("hasDebugColorOverride", false);
				if(record.hasDebugColorOverride) {
					const auto color = source.find("debugColor");
					if(color == source.end() || !color->is_array() || color->size() != 4U) return false;
					record.debugColor = {(*color)[0].get<float>(), (*color)[1].get<float>(),
						(*color)[2].get<float>(), (*color)[3].get<float>()};
				}
				set.colliders.push_back(std::move(record));
			}
		} catch(...) {
			return false;
		}
		if(!set.Validate().accepted) return false;
		destination = set;
		return true;
	}

	// Poses always use an explicit object so position and rotation cannot be
	// confused when future schemas add more constraint-frame settings.
	nlohmann::json PhysicsPoseJson(const PhysicsPose& value) {
		return {{"position", PhysicsVectorJson(value.position)}, {"rotation", PhysicsRotationJson(value.rotation)}};
	}

	bool ReadPhysicsPose(const nlohmann::json& value, PhysicsPose& result) {
		if(!value.is_object()) return false;
		const auto position = value.find("position");
		const auto rotation = value.find("rotation");
		return position != value.end() && rotation != value.end()
			&& ReadPhysicsVector(*position, result.position) && ReadPhysicsRotation(*rotation, result.rotation);
	}

	std::string_view ConstraintKindName(PhysicsConstraintKind kind) {
		switch(kind) {
		case PhysicsConstraintKind::Fixed: return "Fixed";
		case PhysicsConstraintKind::Point: return "Point";
		case PhysicsConstraintKind::Distance: return "Distance";
		case PhysicsConstraintKind::Hinge: return "Hinge";
		case PhysicsConstraintKind::Slider: return "Slider";
		case PhysicsConstraintKind::Cone: return "Cone";
		case PhysicsConstraintKind::SwingTwist: return "SwingTwist";
		case PhysicsConstraintKind::SixDegreesOfFreedom: return "SixDegreesOfFreedom";
		case PhysicsConstraintKind::Gear: return "Gear";
		}
		return "Unknown";
	}

	bool ReadConstraintKind(const nlohmann::json& value, PhysicsConstraintKind& result) {
		if(!value.is_string()) return false;
		const std::string name = value.get<std::string>();
		if(name == "Fixed") result = PhysicsConstraintKind::Fixed;
		else if(name == "Point") result = PhysicsConstraintKind::Point;
		else if(name == "Distance") result = PhysicsConstraintKind::Distance;
		else if(name == "Hinge") result = PhysicsConstraintKind::Hinge;
		else if(name == "Slider") result = PhysicsConstraintKind::Slider;
		else if(name == "Cone") result = PhysicsConstraintKind::Cone;
		else if(name == "SwingTwist") result = PhysicsConstraintKind::SwingTwist;
		else if(name == "SixDegreesOfFreedom") result = PhysicsConstraintKind::SixDegreesOfFreedom;
		else if(name == "Gear") result = PhysicsConstraintKind::Gear;
		else return false;
		return true;
	}

	std::string_view EndpointKindName(PhysicsConstraintEndpointKind kind) {
		switch(kind) {
		case PhysicsConstraintEndpointKind::ConnectedObject: return "ConnectedObject";
		case PhysicsConstraintEndpointKind::WorldAnchor: return "WorldAnchor";
		}
		return "Unknown";
	}

	bool ReadEndpointKind(const nlohmann::json& value, PhysicsConstraintEndpointKind& result) {
		if(!value.is_string()) return false;
		const std::string name = value.get<std::string>();
		if(name == "ConnectedObject") result = PhysicsConstraintEndpointKind::ConnectedObject;
		else if(name == "WorldAnchor") result = PhysicsConstraintEndpointKind::WorldAnchor;
		else return false;
		return true;
	}

	std::string_view MotorModeName(PhysicsConstraintMotorMode mode) {
		switch(mode) {
		case PhysicsConstraintMotorMode::Disabled: return "Disabled";
		case PhysicsConstraintMotorMode::Position: return "Position";
		case PhysicsConstraintMotorMode::Velocity: return "Velocity";
		}
		return "Unknown";
	}

	bool ReadMotorMode(const nlohmann::json& value, PhysicsConstraintMotorMode& result) {
		if(!value.is_string()) return false;
		const std::string name = value.get<std::string>();
		if(name == "Disabled") result = PhysicsConstraintMotorMode::Disabled;
		else if(name == "Position") result = PhysicsConstraintMotorMode::Position;
		else if(name == "Velocity") result = PhysicsConstraintMotorMode::Velocity;
		else return false;
		return true;
	}

	// These compact helpers keep scalar joints and the six ordered SixDOF axes
	// on the same versioned schema without duplicating field spelling rules.
	nlohmann::json ConstraintLimitsJson(const PhysicsConstraintLimits& value) {
		return {{"enabled", value.enabled}, {"minimum", value.minimum}, {"maximum", value.maximum}};
	}

	bool ReadConstraintLimits(const nlohmann::json& value, PhysicsConstraintLimits& result) {
		if(!value.is_object()) return false;
		try {
			result.enabled = value.at("enabled").get<bool>();
			result.minimum = value.at("minimum").get<float>();
			result.maximum = value.at("maximum").get<float>();
			return true;
		} catch(...) {
			return false;
		}
	}

	nlohmann::json ConstraintSpringJson(const PhysicsConstraintSpring& value) {
		return {{"enabled", value.enabled}, {"stiffness", value.stiffness}, {"damping", value.damping}};
	}

	bool ReadConstraintSpring(const nlohmann::json& value, PhysicsConstraintSpring& result) {
		if(!value.is_object()) return false;
		try {
			result.enabled = value.at("enabled").get<bool>();
			result.stiffness = value.at("stiffness").get<float>();
			result.damping = value.at("damping").get<float>();
			return true;
		} catch(...) {
			return false;
		}
	}

	nlohmann::json ConstraintMotorJson(const PhysicsConstraintMotor& value) {
		return {{"mode", MotorModeName(value.mode)}, {"targetPosition", value.targetPosition},
			{"targetVelocity", value.targetVelocity}, {"maximumForce", value.maximumForce}};
	}

	bool ReadConstraintMotor(const nlohmann::json& value, PhysicsConstraintMotor& result) {
		if(!value.is_object()) return false;
		try {
			if(!ReadMotorMode(value.at("mode"), result.mode)) return false;
			result.targetPosition = value.at("targetPosition").get<float>();
			result.targetVelocity = value.at("targetVelocity").get<float>();
			result.maximumForce = value.at("maximumForce").get<float>();
			return true;
		} catch(...) {
			return false;
		}
	}

	// JSON has no infinity literal. The engine-owned spelling preserves the
	// documented unbreakable threshold without relying on a provider sentinel.
	nlohmann::json BreakThresholdJson(float value) {
		return std::isinf(value) ? nlohmann::json("Infinity") : nlohmann::json(value);
	}

	bool ReadBreakThreshold(const nlohmann::json& value, float& result) {
		if(value.is_string()) {
			if(value.get<std::string>() != "Infinity") return false;
			result = std::numeric_limits<float>::infinity();
			return true;
		}
		if(!value.is_number()) return false;
		result = value.get<float>();
		return std::isfinite(result);
	}

	nlohmann::json SerializeConstraintSet(const ConstraintSetComponent& set) {
		nlohmann::json constraints = nlohmann::json::array();
		for(const ConstraintRecord& record : set.constraints) {
			nlohmann::json axisLimits = nlohmann::json::array();
			nlohmann::json axisSprings = nlohmann::json::array();
			nlohmann::json axisMotors = nlohmann::json::array();
			for(std::size_t axisIndex = 0U; axisIndex < record.axisLimits.size(); ++axisIndex) {
				axisLimits.push_back(ConstraintLimitsJson(record.axisLimits[axisIndex]));
				axisSprings.push_back(ConstraintSpringJson(record.axisSprings[axisIndex]));
				axisMotors.push_back(ConstraintMotorJson(record.axisMotors[axisIndex]));
			}
			constraints.push_back({
				{"recordId", TerraEngine::ToHexString(record.recordId)},
				{"recordVersion", record.recordVersion},
				{"kind", ConstraintKindName(record.kind)},
				{"firstEndpointKind", EndpointKindName(record.firstEndpointKind)},
				{"firstObjectId", TerraEngine::ToHexString(record.firstObjectId)},
				{"secondEndpointKind", EndpointKindName(record.secondEndpointKind)},
				{"secondObjectId", TerraEngine::ToHexString(record.secondObjectId)},
				{"firstFrame", PhysicsPoseJson(record.firstFrame)},
				{"secondFrame", PhysicsPoseJson(record.secondFrame)},
				{"firstAxis", PhysicsVectorJson(record.firstAxis)},
				{"secondAxis", PhysicsVectorJson(record.secondAxis)},
				{"limits", ConstraintLimitsJson(record.limits)},
				{"spring", ConstraintSpringJson(record.spring)},
				{"motor", ConstraintMotorJson(record.motor)},
				{"axisLimits", std::move(axisLimits)},
				{"axisSprings", std::move(axisSprings)},
				{"axisMotors", std::move(axisMotors)},
				{"firstGearConstraintId", TerraEngine::ToHexString(record.firstGearConstraintId)},
				{"secondGearConstraintId", TerraEngine::ToHexString(record.secondGearConstraintId)},
				{"gearRatio", record.gearRatio},
				{"collideConnected", record.collideConnected},
				{"breakForce", BreakThresholdJson(record.breakForce)},
				{"breakTorque", BreakThresholdJson(record.breakTorque)},
				{"enabled", record.enabled},
				{"resetGeneration", record.resetGeneration}
			});
		}
		return {{"active", set.active}, {"schemaVersion", set.schemaVersion}, {"constraints", std::move(constraints)}};
	}

	bool DeserializeConstraintSet(const nlohmann::json& json, ConstraintSetComponent& destination) {
		if(!json.is_object() || !json.contains("constraints") || !json["constraints"].is_array()) return false;
		ConstraintSetComponent set;
		try {
			set.active = json.value("active", set.active);
			set.schemaVersion = json.value("schemaVersion", set.schemaVersion);
			for(const nlohmann::json& source : json["constraints"]) {
				if(!source.is_object()) return false;
				ConstraintRecord record;
				const auto recordId = TerraEngine::ParseUUID(source.at("recordId").get<std::string>());
				const auto firstObjectId = TerraEngine::ParseUUID(source.at("firstObjectId").get<std::string>());
				const auto secondObjectId = TerraEngine::ParseUUID(source.at("secondObjectId").get<std::string>());
				const auto firstGearConstraintId = TerraEngine::ParseUUID(source.at("firstGearConstraintId").get<std::string>());
				const auto secondGearConstraintId = TerraEngine::ParseUUID(source.at("secondGearConstraintId").get<std::string>());
				if(!recordId || !firstObjectId || !secondObjectId || !firstGearConstraintId || !secondGearConstraintId) return false;
				record.recordId = *recordId;
				record.firstObjectId = *firstObjectId;
				record.secondObjectId = *secondObjectId;
				record.firstGearConstraintId = *firstGearConstraintId;
				record.secondGearConstraintId = *secondGearConstraintId;
				record.recordVersion = source.value("recordVersion", record.recordVersion);
				if(!ReadConstraintKind(source.at("kind"), record.kind)
					|| !ReadEndpointKind(source.at("firstEndpointKind"), record.firstEndpointKind)
					|| !ReadEndpointKind(source.at("secondEndpointKind"), record.secondEndpointKind)
					|| !ReadPhysicsPose(source.at("firstFrame"), record.firstFrame)
					|| !ReadPhysicsPose(source.at("secondFrame"), record.secondFrame)
					|| !ReadPhysicsVector(source.at("firstAxis"), record.firstAxis)
					|| !ReadPhysicsVector(source.at("secondAxis"), record.secondAxis)) return false;
				if(!ReadConstraintLimits(source.at("limits"), record.limits)
					|| !ReadConstraintSpring(source.at("spring"), record.spring)
					|| !ReadConstraintMotor(source.at("motor"), record.motor)) return false;
				const nlohmann::json& axisLimits = source.at("axisLimits");
				const nlohmann::json& axisSprings = source.at("axisSprings");
				const nlohmann::json& axisMotors = source.at("axisMotors");
				if(!axisLimits.is_array() || !axisSprings.is_array() || !axisMotors.is_array()
					|| axisLimits.size() != record.axisLimits.size() || axisSprings.size() != record.axisSprings.size()
					|| axisMotors.size() != record.axisMotors.size()) return false;
				for(std::size_t axisIndex = 0U; axisIndex < record.axisLimits.size(); ++axisIndex) {
					if(!ReadConstraintLimits(axisLimits[axisIndex], record.axisLimits[axisIndex])
						|| !ReadConstraintSpring(axisSprings[axisIndex], record.axisSprings[axisIndex])
						|| !ReadConstraintMotor(axisMotors[axisIndex], record.axisMotors[axisIndex])) return false;
				}
				record.gearRatio = source.at("gearRatio").get<float>();
				record.collideConnected = source.at("collideConnected").get<bool>();
				if(!ReadBreakThreshold(source.at("breakForce"), record.breakForce)
					|| !ReadBreakThreshold(source.at("breakTorque"), record.breakTorque)) return false;
				record.enabled = source.at("enabled").get<bool>();
				record.resetGeneration = source.at("resetGeneration").get<std::uint32_t>();
				set.constraints.push_back(std::move(record));
			}
		} catch(...) {
			return false;
		}
		if(!set.Validate().accepted) return false;
		destination = set;
		return true;
	}

	std::string_view CharacterShapeName(PhysicsCharacterShapeKind kind) {
		return kind == PhysicsCharacterShapeKind::Capsule ? "Capsule"
			: kind == PhysicsCharacterShapeKind::Cylinder ? "Cylinder" : "Unknown";
	}

	bool ReadCharacterShape(const nlohmann::json& value, PhysicsCharacterShapeKind& result) {
		if(!value.is_string()) return false;
		const std::string name = value.get<std::string>();
		if(name == "Capsule") result = PhysicsCharacterShapeKind::Capsule;
		else if(name == "Cylinder") result = PhysicsCharacterShapeKind::Cylinder;
		else return false;
		return true;
	}

	std::string_view RotationPolicyName(PhysicsCharacterRotationPolicy policy) {
		switch(policy) {
		case PhysicsCharacterRotationPolicy::Free: return "Free";
		case PhysicsCharacterRotationPolicy::AlignToUp: return "AlignToUp";
		case PhysicsCharacterRotationPolicy::Locked: return "Locked";
		}
		return "Unknown";
	}

	bool ReadRotationPolicy(const nlohmann::json& value, PhysicsCharacterRotationPolicy& result) {
		if(!value.is_string()) return false;
		const std::string name = value.get<std::string>();
		if(name == "Free") result = PhysicsCharacterRotationPolicy::Free;
		else if(name == "AlignToUp") result = PhysicsCharacterRotationPolicy::AlignToUp;
		else if(name == "Locked") result = PhysicsCharacterRotationPolicy::Locked;
		else return false;
		return true;
	}

	nlohmann::json SerializeCharacterController(const CharacterControllerComponent& character) {
		return {
			{"active", character.active}, {"schemaVersion", character.schemaVersion},
			{"shapeKind", CharacterShapeName(character.shapeKind)}, {"radius", character.radius}, {"height", character.height},
			{"localCenter", PhysicsVectorJson(character.localCenter)}, {"upDirection", PhysicsVectorJson(character.upDirection)},
			{"stepHeight", character.stepHeight}, {"supportVolumeRadius", character.supportVolumeRadius},
			{"supportVolumeHeight", character.supportVolumeHeight}, {"maximumSlopeAngleDegrees", character.maximumSlopeAngleDegrees},
			{"contactPadding", character.contactPadding}, {"mass", character.mass}, {"maximumStrength", character.maximumStrength},
			{"collisionLayerId", character.collisionLayerId}, {"collisionMask", character.collisionMask},
			{"groundAdhesion", character.groundAdhesion}, {"allowStairWalking", character.allowStairWalking},
			{"useGravity", character.useGravity}, {"gravityScale", character.gravityScale}, {"jumpSpeed", character.jumpSpeed},
			{"rotationPolicy", RotationPolicyName(character.rotationPolicy)}, {"enabled", character.enabled}
		};
	}

	bool DeserializeCharacterController(const nlohmann::json& json, CharacterControllerComponent& destination) {
		if(!json.is_object()) return false;
		CharacterControllerComponent character;
		try {
			character.active = json.value("active", character.active);
			character.schemaVersion = json.value("schemaVersion", character.schemaVersion);
			if(!ReadCharacterShape(json.at("shapeKind"), character.shapeKind)
				|| !ReadPhysicsVector(json.at("localCenter"), character.localCenter)
				|| !ReadPhysicsVector(json.at("upDirection"), character.upDirection)
				|| !ReadRotationPolicy(json.at("rotationPolicy"), character.rotationPolicy)) return false;
			character.radius = json.at("radius").get<float>();
			character.height = json.at("height").get<float>();
			character.stepHeight = json.at("stepHeight").get<float>();
			character.supportVolumeRadius = json.at("supportVolumeRadius").get<float>();
			character.supportVolumeHeight = json.at("supportVolumeHeight").get<float>();
			character.maximumSlopeAngleDegrees = json.at("maximumSlopeAngleDegrees").get<float>();
			character.contactPadding = json.at("contactPadding").get<float>();
			character.mass = json.at("mass").get<float>();
			character.maximumStrength = json.at("maximumStrength").get<float>();
			character.collisionLayerId = json.at("collisionLayerId").get<std::uint8_t>();
			character.collisionMask = json.at("collisionMask").get<std::uint32_t>();
			character.groundAdhesion = json.at("groundAdhesion").get<float>();
			character.allowStairWalking = json.at("allowStairWalking").get<bool>();
			character.useGravity = json.at("useGravity").get<bool>();
			character.gravityScale = json.at("gravityScale").get<float>();
			character.jumpSpeed = json.at("jumpSpeed").get<float>();
			character.enabled = json.at("enabled").get<bool>();
		} catch(...) {
			return false;
		}
		if(!character.Validate().accepted) return false;
		destination = character;
		return true;
	}

	nlohmann::json SerializeCanonicalTransform(const Transform& transform)
	{
		// Transform has no reflected float position alias.  Persist both exact
		// world authority and bounded hierarchy-relative data explicitly.
		return {{"WorldPose", {{"position", Serializer::SerializeWorldPosition(transform.worldPose.position)},
			{"rotation", {transform.worldPose.rotation.w, transform.worldPose.rotation.x, transform.worldPose.rotation.y, transform.worldPose.rotation.z}},
			{"scale", {transform.worldPose.scale.x, transform.worldPose.scale.y, transform.worldPose.scale.z}}}},
			{"ParentRelativePose", {{"translationMetres", {transform.parentRelativePose.translationMetres.x, transform.parentRelativePose.translationMetres.y, transform.parentRelativePose.translationMetres.z}},
			{"rotation", {transform.parentRelativePose.rotation.w, transform.parentRelativePose.rotation.x, transform.parentRelativePose.rotation.y, transform.parentRelativePose.rotation.z}},
			{"scale", {transform.parentRelativePose.scale.x, transform.parentRelativePose.scale.y, transform.parentRelativePose.scale.z}},
			{"transformSpaceId", transform.parentRelativePose.transformSpaceId}}}};
	}

	bool DeserializeCanonicalTransform(const nlohmann::json& json, Transform& destination)
	{
		if (!json.is_object() || !json.contains("WorldPose") || !json["WorldPose"].is_object() || !json.contains("ParentRelativePose") || !json["ParentRelativePose"].is_object()) return false;
		const nlohmann::json& world = json["WorldPose"];
		std::string positionError;
		const auto position = world.contains("position") ? Serializer::DeserializeWorldPosition(world["position"], &positionError) : std::nullopt;
		const nlohmann::json& relative = json["ParentRelativePose"];
		if (!position || !world.contains("rotation") || !world["rotation"].is_array() || world["rotation"].size() != 4U
			|| !world.contains("scale") || !world["scale"].is_array() || world["scale"].size() != 3U
			|| !relative.contains("translationMetres") || !relative["translationMetres"].is_array() || relative["translationMetres"].size() != 3U
			|| !relative.contains("rotation") || !relative["rotation"].is_array() || relative["rotation"].size() != 4U
			|| !relative.contains("scale") || !relative["scale"].is_array() || relative["scale"].size() != 3U
			|| !relative.contains("transformSpaceId") || !relative["transformSpaceId"].is_number_unsigned()) return false;
		try {
			TerraEngine::Scene::WorldPose pose;
			pose.position = *position;
			pose.rotation = {world["rotation"][0].get<float>(), world["rotation"][1].get<float>(), world["rotation"][2].get<float>(), world["rotation"][3].get<float>()};
			pose.scale = {world["scale"][0].get<float>(), world["scale"][1].get<float>(), world["scale"][2].get<float>()};
			TerraEngine::Scene::ParentRelativePose local;
			local.translationMetres = {relative["translationMetres"][0].get<double>(), relative["translationMetres"][1].get<double>(), relative["translationMetres"][2].get<double>()};
			local.rotation = {relative["rotation"][0].get<float>(), relative["rotation"][1].get<float>(), relative["rotation"][2].get<float>(), relative["rotation"][3].get<float>()};
			local.scale = {relative["scale"][0].get<float>(), relative["scale"][1].get<float>(), relative["scale"][2].get<float>()};
			local.transformSpaceId = relative["transformSpaceId"].get<std::uint64_t>();
			if (!TerraEngine::Scene::IsWorldPoseFinite(pose) || !TerraEngine::Scene::IsParentRelativePoseInRange(local)) return false;
			destination.worldPose = pose; destination.parentRelativePose = local;
			return true;
		} catch (const nlohmann::json::exception&) { return false; }
	}
}

// SerializeObjectRecursive snapshots authored scene state through the canonical
// Object handle path while still letting reflection read the underlying EnTT
// registry needed for component serialization.
nlohmann::json SerializeObjectRecursive(Object object) {
	ObjectManager& objManager = ObjectManager::instance();
	entt::registry& enttRegistry = objManager.getRegistry();
	const entt::entity entity = object.EntityId();
	nlohmann::json objJson;
	objJson["name"] = objManager.getObjectName(object);
	objJson["tag"] = objManager.getObjectTag(object);
	objJson["layer"] = objManager.getObjectLayer(object);
	objJson["active"] = objManager.isObjectActive(object);
	objJson["isStatic"] = objManager.isObjectStatic(object);
	
	auto uuid = objManager.getUUID(object);
	objJson["uuid"] = TerraEngine::ToHexString(uuid);
	
	objJson["components"] = nlohmann::json::object();

	// Assumes ComponentAccessRegistryMutex is already locked by the caller (SerializeObject)
	// to prevent deadlocks during recursive traversal of complex hierarchies.
	auto& registry = ComponentAccessRegistry();

	for (auto& [compName, functions] : registry)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (compName == "TagComponent") continue; // Skip TagComponent as it's now embedded in Object

		if (functions.entityHasComponent && functions.entityHasComponent(enttRegistry, entity))
		{
			if(compName == Transform::kComponentName) {
				if(const Transform* transform = enttRegistry.try_get<Transform>(entity)) {
					auto guard = transform->acquireReadLock();
					objJson["components"][compName] = SerializeCanonicalTransform(*transform);
				}
				continue;
			}
			if(compName == AnimatorComponent::kComponentName) {
				if(const AnimatorComponent* animator = enttRegistry.try_get<AnimatorComponent>(entity)) {
					auto guard = animator->acquireReadLock();
					objJson["components"][compName] = SerializeAnimator(*animator);
				}
				continue;
			}
			// Collection components need a typed serializer because reflection does
			// not flatten variants or vectors. Keep this special case at the protected
			// scene boundary; worker snapshots consume only the decoded typed values.
			if(compName == RigidBodyComponent::kComponentName) {
				if(const RigidBodyComponent* body = enttRegistry.try_get<RigidBodyComponent>(entity)) {
					auto guard = body->acquireReadLock();
					objJson["components"][compName] = SerializeRigidBody(*body);
				}
				continue;
			}
			if(compName == ColliderSetComponent::kComponentName) {
				if(const ColliderSetComponent* colliders = enttRegistry.try_get<ColliderSetComponent>(entity)) {
					auto guard = colliders->acquireReadLock();
					objJson["components"][compName] = SerializeColliderSet(*colliders);
				}
				continue;
			}
			if(compName == ConstraintSetComponent::kComponentName) {
				if(const ConstraintSetComponent* constraints = enttRegistry.try_get<ConstraintSetComponent>(entity)) {
					auto guard = constraints->acquireReadLock();
					objJson["components"][compName] = SerializeConstraintSet(*constraints);
				}
				continue;
			}
			if(compName == CharacterControllerComponent::kComponentName) {
				if(const CharacterControllerComponent* character = enttRegistry.try_get<CharacterControllerComponent>(entity)) {
					auto guard = character->acquireReadLock();
					objJson["components"][compName] = SerializeCharacterController(*character);
				}
				continue;
			}
			nlohmann::json compJson = nlohmann::json::object();
			TerraEngine::Reflection::Type type = functions.reflectionType;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (TerraEngine::Reflection::IsValid(type))
			{
				// Enumerate all reflected properties and convert their values into
				// JSON-compatible primitives or arrays.
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (const TerraEngine::Reflection::Property& prop : TerraEngine::Reflection::GetProperties(type))
				{
					std::string propName = TerraEngine::Reflection::GetPropertyName(prop);
					TerraEngine::Reflection::Variant val = functions.entityGetter(enttRegistry, entity, propName);
					
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (TerraEngine::Reflection::IsType<float>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<float>(val);
					else if (TerraEngine::Reflection::IsType<int>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<int>(val);
					else if (TerraEngine::Reflection::IsType<std::uint32_t>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<std::uint32_t>(val);
					else if (TerraEngine::Reflection::IsType<bool>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<bool>(val);
					else if (TerraEngine::Reflection::IsType<std::string>(val)) compJson[propName] = TerraEngine::Reflection::GetValue<std::string>(val);
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (TerraEngine::Reflection::IsType<glm::vec3>(val)) {
						auto v = TerraEngine::Reflection::GetValue<glm::vec3>(val);
						compJson[propName] = {v.x, v.y, v.z};
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (TerraEngine::Reflection::IsType<glm::quat>(val)) {
						auto q = TerraEngine::Reflection::GetValue<glm::quat>(val);
						compJson[propName] = {q.w, q.x, q.y, q.z};
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(val)) {
						auto uuid = TerraEngine::Reflection::GetValue<TerraEngine::UUID>(val);
						compJson[propName] = TerraEngine::ToHexString(uuid);
					}
				}
			}
			objJson["components"][compName] = compJson;
		}
	}
	
	auto children = objManager.getChildren(object);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!children.empty()) {
		objJson["children"] = nlohmann::json::array();
		for (Object child : children) {
			objJson["children"].push_back(SerializeObjectRecursive(child));
		}
	}
	return objJson;
}

// Serializer::SerializeObject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
nlohmann::json Serializer::SerializeObject(entt::entity entity) {
	// Acquire an exclusive lock once for the entire graph to ensure a consistent
	// snapshot while the recursion proceeds without further locking.
	std::lock_guard lock(ComponentAccessRegistryMutex());
	if (std::optional<Object> object = ObjectManager::instance().getObject(entity))
	{
		return SerializeObjectRecursive(*object);
	}
	return nlohmann::json::object();
}

nlohmann::json Serializer::SerializeObject(Object& obj) {
	return SerializeObject(obj.EntityId());
}

// Serializer::SerializeSceneRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
nlohmann::json Serializer::SerializeSceneRenderSettings(const SceneRenderSettings& settings)
{
	const SceneRenderSettings sanitizedSettings = SanitizeSceneRenderSettings(settings);
	nlohmann::json sceneSettingsJson = nlohmann::json::object();

	// Sky authoring now belongs to SkyComponent on scene objects. This scene
	// settings block intentionally writes only non-sky renderer settings.
	sceneSettingsJson["indirectStrength"] = sanitizedSettings.indirectStrength;
	sceneSettingsJson["ambientOcclusionStrength"] = sanitizedSettings.ambientOcclusionStrength;
	sceneSettingsJson["ambientOcclusionQuality"] = sanitizedSettings.ambientOcclusionQuality;
	return sceneSettingsJson;
}

// Serializer::DeserializeSceneRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
SceneRenderSettings Serializer::DeserializeSceneRenderSettings(const nlohmann::json& json)
{
	SceneRenderSettings settings = MakeDefaultSceneRenderSettings();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!json.is_object())
	{
		return settings;
	}

	// Obsolete sceneSettings.sky data is unsupported by design and ignored as
	// if absent. Do not convert it into component data here.

	settings.indirectStrength = json.value("indirectStrength", settings.indirectStrength);
	settings.ambientOcclusionStrength = json.value("ambientOcclusionStrength", settings.ambientOcclusionStrength);
	settings.ambientOcclusionQuality = json.value("ambientOcclusionQuality", settings.ambientOcclusionQuality);

	return SanitizeSceneRenderSettings(settings);
}

// Serializer::DeserializeObjectEntity restores authored state through manager
// APIs so stale identity and component access stay centralized.
entt::entity Serializer::DeserializeObjectEntity(const nlohmann::json& json, std::optional<entt::entity> parent) {
	// Disk data may name AnimatorComponent before any live instance has invoked
	// its CRTP constructor. Register its authored reflection contract first so
	// the generic component path can restore it while ignoring unknown runtime
	// keys from malformed or obsolete payloads.
	AnimatorComponent::registerType();
	AudioSourceComponent::registerType();
	AudioListenerComponent::registerType();
	// Scene files can be loaded before an editor window or renderer constructs
	// either component. Register their reflection contracts explicitly so an
	// authored camera/renderable is not silently skipped because startup order
	// happened not to instantiate that C++ type first.
	CameraComponent::registerType();
	MeshRenderComponent::registerType();
	// Register the small terrain-instance contract before generic reflection
	// deserialization attempts to create TerrainComponent from scene JSON.
	TerrainComponent::registerType();
	ObjectManager& objManager = ObjectManager::instance();
	entt::registry& enttRegistry = objManager.getRegistry();
	std::string name = json.value("name", "Unnamed");
	
	// Identity restoration is critical for maintaining cross-object references 
	// and correctly applying undo/redo steps to the right entities.
	TerraEngine::UUID uuid = TerraEngine::GenerateUUID();
	bool hasUuid = false;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (json.contains("uuid")) {
		auto& uuidVal = json["uuid"];
		if (uuidVal.is_string()) {
			auto parsed = TerraEngine::ParseUUID(uuidVal.get<std::string>());
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (parsed) {
				uuid = *parsed;
				hasUuid = true;
			}
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (uuidVal.is_array() && uuidVal.size() == 16) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (int i = 0; i < 16; ++i) uuid[i] = uuidVal[i].get<uint8_t>();
			hasUuid = true;
		}
	}

	Object obj = objManager.createObject(name);
	const entt::entity entity = obj.EntityId();
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hasUuid) {
		// Force the newly created object to use the original identity from the
		// JSON document instead of its automatically generated transient UUID.
		objManager.UpdateObjectUUID(obj, uuid);
	}

	objManager.setObjectTag(obj, json.value("tag", "Untagged"));
	objManager.setObjectLayer(obj, json.value("layer", "Default"));
	objManager.setObjectActive(obj, json.value("active", true));
	objManager.setObjectStatic(obj, json.value("isStatic", false));

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (parent) {
		if (std::optional<Object> parentObject = objManager.getObject(*parent))
		{
			objManager.setParent(obj, parentObject);
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (json.contains("components")) {
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto& registry = ComponentAccessRegistry();

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& [compName, compData] : json["components"].items()) {

			if (registry.count(compName)) {
				auto& functions = registry[compName];
				if (functions.entityAddComponent)
				{
					functions.entityAddComponent(enttRegistry, entity);
					objManager.notifyObjectStructureChanged(obj);
				}

				if(compName == RigidBodyComponent::kComponentName) {
					RigidBodyComponent* body = enttRegistry.try_get<RigidBodyComponent>(entity);
					if(body == nullptr || !DeserializeRigidBody(compData, *body)) {
						if(functions.entityRemoveComponent) functions.entityRemoveComponent(enttRegistry, entity);
					}
					continue;
				}
				if(compName == Transform::kComponentName) {
					Transform* transform = enttRegistry.try_get<Transform>(entity);
					if(transform == nullptr || !DeserializeCanonicalTransform(compData, *transform)) {
						if(functions.entityRemoveComponent) functions.entityRemoveComponent(enttRegistry, entity);
					}
					continue;
				}
				if(compName == AnimatorComponent::kComponentName) {
					AnimatorComponent* animator = enttRegistry.try_get<AnimatorComponent>(entity);
					if(animator == nullptr || !DeserializeAnimator(compData, *animator)) {
						if(functions.entityRemoveComponent) functions.entityRemoveComponent(enttRegistry, entity);
					}
					continue;
				}
				if(compName == ColliderSetComponent::kComponentName) {
					ColliderSetComponent* colliders = enttRegistry.try_get<ColliderSetComponent>(entity);
					if(colliders == nullptr || !DeserializeColliderSet(compData, *colliders)) {
						if(functions.entityRemoveComponent) functions.entityRemoveComponent(enttRegistry, entity);
					}
					continue;
				}
				if(compName == ConstraintSetComponent::kComponentName) {
					ConstraintSetComponent* constraints = enttRegistry.try_get<ConstraintSetComponent>(entity);
					if(constraints == nullptr || !DeserializeConstraintSet(compData, *constraints)) {
						if(functions.entityRemoveComponent) functions.entityRemoveComponent(enttRegistry, entity);
					}
					continue;
				}
				if(compName == CharacterControllerComponent::kComponentName) {
					CharacterControllerComponent* character = enttRegistry.try_get<CharacterControllerComponent>(entity);
					if(character == nullptr || !DeserializeCharacterController(compData, *character)) {
						if(functions.entityRemoveComponent) functions.entityRemoveComponent(enttRegistry, entity);
					}
					continue;
				}

					TerraEngine::Reflection::Type type = functions.reflectionType;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (TerraEngine::Reflection::IsValid(type)) {
					// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
					for (auto& [propName, propVal] : compData.items()) {
						TerraEngine::Reflection::Property prop = TerraEngine::Reflection::FindProperty(type, propName);
						if (TerraEngine::Reflection::IsValid(prop)) {
							TerraEngine::Reflection::Variant val;
							// Heuristic-based type reconstruction for JSON variants.
							// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
							if (propVal.is_number_float()) val = propVal.get<float>();
							else if (propVal.is_number_integer()) {
								if (TerraEngine::Reflection::IsType<std::uint32_t>(TerraEngine::Reflection::GetPropertyType(prop))) {
									val = propVal.get<std::uint32_t>();
								} else {
									val = propVal.get<int>();
								}
							}
							else if (propVal.is_boolean()) val = propVal.get<bool>();
							// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
							else if (propVal.is_string()) {
								// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
								if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(TerraEngine::Reflection::GetPropertyType(prop))) {
									auto parsed = TerraEngine::ParseUUID(propVal.get<std::string>());
									if (parsed) val = *parsed;
								} else {
									val = propVal.get<std::string>();
								}
							}
							// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
							else if (propVal.is_array()) {
								// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
								if (propVal.size() == 3) {
									val = glm::vec3(propVal[0], propVal[1], propVal[2]);
								} else if (propVal.size() == 4) {
									// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
									if (TerraEngine::Reflection::IsType<glm::quat>(TerraEngine::Reflection::GetPropertyType(prop))) {
										val = glm::quat(propVal[0], propVal[1], propVal[2], propVal[3]); // w, x, y, z
									}
								} else if (propVal.size() == 16 && TerraEngine::Reflection::IsType<TerraEngine::UUID>(TerraEngine::Reflection::GetPropertyType(prop))) {
									TerraEngine::UUID u;
									// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
									for(int i=0; i<16; ++i) u[i] = propVal[i].get<uint8_t>();
									val = u;
								}
							}
							
							// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
							if (TerraEngine::Reflection::IsValid(val)) {
								functions.entitySetter(enttRegistry, entity, propName, val);
							}
						}
					}
				}
			}
		}
	}

	// Recurse to build out the full hierarchy as defined in the source JSON.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (json.contains("children") && json["children"].is_array()) {
		for (auto& childJson : json["children"]) {
			DeserializeObjectEntity(childJson, entity);
		}
	}
	
	return entity;
}

Object Serializer::DeserializeObject(const nlohmann::json& json, std::optional<Object> parent) {
	std::optional<entt::entity> parentEntity;
	if (parent)
	{
		parentEntity = parent->EntityId();
	}
	const entt::entity entity = DeserializeObjectEntity(json, parentEntity);
	return *ObjectManager::instance().getObject(entity);
}

TerraEngine::Scene::PreparedSceneDecode Serializer::PrepareSceneDecode(
	const nlohmann::json& sceneJson,
	const std::function<bool()>& cancellationRequested)
{
	TerraEngine::Scene::PreparedSceneDecode result;
	if (!ValidateUnboundedSceneDocument(sceneJson, &result.reason))
	{
		const std::string observedFormat = sceneJson.is_object() && sceneJson.contains("format")
			? sceneJson["format"].dump()
			: "<missing>";
		result.reason = "scene graph decode observed format " + observedFormat + ": " + result.reason;
		return result;
	}

	const nlohmann::json* roots = nullptr;
	if (const auto objects = sceneJson.find("objects"); objects != sceneJson.end() && objects->is_array())
	{
		roots = &*objects;
	}
	else if (const auto entities = sceneJson.find("entities"); entities != sceneJson.end() && entities->is_array())
	{
		roots = &*entities;
	}
	else
	{
		result.reason = "authored scene does not contain an objects or entities array";
		return result;
	}

	std::function<bool(const nlohmann::json&, std::optional<std::size_t>, const std::string&)> appendObject;
	appendObject = [&](const nlohmann::json& source, std::optional<std::size_t> parentIndex, const std::string& stablePath) {
		if (cancellationRequested && cancellationRequested())
		{
			result.cancelled = true;
			result.reason = "scene decode was cancelled";
			return false;
		}
		if (!source.is_object())
		{
			result.reason = "scene object record is not an object";
			return false;
		}
		TerraEngine::Scene::PreparedSceneObjectRecord record;
		record.object = source;
		// Child records are represented by their parent index, avoiding repeated
		// recursive payloads in the worker-to-protected packet.
		record.object.erase("children");
		record.stablePath = stablePath;
		record.parentRecordIndex = parentIndex;
		const std::size_t recordIndex = result.objects.size();
		result.objects.push_back(std::move(record));
		++result.objectCount;
		if (parentIndex.has_value()) ++result.hierarchyCount;
		if (const auto components = source.find("components"); components != source.end())
		{
			if (!components->is_object())
			{
				result.reason = "scene component table is not an object";
				return false;
			}
			result.componentCount += components->size();
		}
		if (const auto children = source.find("children"); children != source.end())
		{
			if (!children->is_array())
			{
				result.reason = "scene children field is not an array";
				return false;
			}
			for (std::size_t childIndex = 0U; childIndex < children->size(); ++childIndex)
			{
				if (!appendObject((*children)[childIndex], recordIndex, stablePath + "/" + std::to_string(childIndex))) return false;
			}
		}
		return true;
	};

	for (std::size_t rootIndex = 0U; rootIndex < roots->size(); ++rootIndex)
	{
		if (!appendObject((*roots)[rootIndex], std::nullopt, std::to_string(rootIndex))) return result;
	}
	result.accepted = true;
	return result;
}

TerraEngine::Scene::PreparedSceneDecode Serializer::PreparePrefabDecode(
	const nlohmann::json& prefabJson,
	const std::function<bool()>& cancellationRequested)
{
	TerraEngine::Scene::PreparedSceneDecode rejected;
	if (!ValidateUnboundedPrefabDocument(prefabJson, &rejected.reason))
	{
		return rejected;
	}

	// Scene and prefab files deliberately keep different public format tags.
	// Lower only this private value copy so the existing worker-safe graph
	// decoder can be shared without accepting a prefab through the scene API.
	nlohmann::json graphDocument = prefabJson;
	graphDocument["format"] = std::string(TerraEngine::Scene::kTerraSceneUnboundedFormat);
	if (!ValidateUnboundedSceneDocument(graphDocument, &rejected.reason))
	{
		rejected.reason = "prefab graph lowering failed: " + rejected.reason;
		return rejected;
	}
	TerraEngine::Scene::PreparedSceneDecode decoded = PrepareSceneDecode(graphDocument, cancellationRequested);
	if (!decoded.accepted && !decoded.reason.empty())
	{
		decoded.reason = "prefab graph decode failed: " + decoded.reason;
	}
	return decoded;
}

nlohmann::json Serializer::SerializeWorldPosition(const TerraEngine::Scene::WorldPosition& position)
{
	const auto normalized = TerraEngine::Scene::NormalizeWorldPosition(position);
	if (!normalized.Succeeded()) return nlohmann::json::object();
	return {{"cell", {{"x", std::to_string(normalized.position.cell.x)}, {"y", std::to_string(normalized.position.cell.y)}, {"z", std::to_string(normalized.position.cell.z)}}}, {"offsetMetres", {normalized.position.offsetMetres.x, normalized.position.offsetMetres.y, normalized.position.offsetMetres.z}}};
}

std::optional<TerraEngine::Scene::WorldPosition> Serializer::DeserializeWorldPosition(const nlohmann::json& value, std::string* outError)
{
	auto fail = [outError](std::string reason) -> std::optional<TerraEngine::Scene::WorldPosition> { if (outError) *outError = std::move(reason); return std::nullopt; };
	if (!value.is_object() || !value.contains("cell") || !value["cell"].is_object() || !value.contains("offsetMetres") || !value["offsetMetres"].is_array() || value["offsetMetres"].size() != 3U) return fail("world position must contain decimal cells and three offsets");
	TerraEngine::Scene::WorldPosition result{};
	auto readCell = [&value](std::string_view axis, std::int64_t& destination) { const auto found = value["cell"].find(std::string(axis)); if (found == value["cell"].end() || !found->is_string()) return false; const std::string text = found->get<std::string>(); if (text.empty() || (text.size() > 1U && text.front() == '+')) return false; const auto parsed = std::from_chars(text.data(), text.data() + text.size(), destination); return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(); };
	if (!readCell("x", result.cell.x) || !readCell("y", result.cell.y) || !readCell("z", result.cell.z)) return fail("world position cells must be exact decimal int64 strings");
	for (std::size_t index = 0U; index < 3U; ++index) { if (!value["offsetMetres"][index].is_number()) return fail("world position offsets must be numbers"); const double offset = value["offsetMetres"][index].get<double>(); if (!std::isfinite(offset)) return fail("world position offsets must be finite"); result.offsetMetres[static_cast<glm::length_t>(index)] = offset; }
	const auto normalized = TerraEngine::Scene::NormalizeWorldPosition(result);
	if (!normalized.Succeeded()) return fail("world position overflows during normalization");
	if (normalized.position.cell != result.cell || normalized.position.offsetMetres != result.offsetMetres) return fail("world position offsets are not normalized");
	return result;
}

bool Serializer::ValidateUnboundedSceneDocument(const nlohmann::json& document, std::string* outError)
{
	auto fail = [outError](std::string reason) { if (outError) *outError = std::move(reason); return false; };
	if (!document.is_object()) return fail("TerraSceneUnboundedV1 root is not an object");
	const auto format = document.find("format");
	if (format == document.end() || !format->is_string() || format->get<std::string>() != TerraEngine::Scene::kTerraSceneUnboundedFormat) return fail("scene format must be TerraSceneUnboundedV1");
	return document.contains("objects") && document["objects"].is_array() ? true : fail("TerraSceneUnboundedV1 requires an objects array");
}

bool Serializer::ValidateUnboundedPrefabDocument(const nlohmann::json& document, std::string* outError)
{
	auto fail = [outError](std::string reason) { if (outError) *outError = std::move(reason); return false; };
	if (!document.is_object()) return fail("TerraPrefabUnboundedV1 root is not an object");
	const auto format = document.find("format");
	if (format == document.end() || !format->is_string() || format->get<std::string>() != TerraEngine::Scene::kTerraPrefabUnboundedFormat) return fail("prefab format must be TerraPrefabUnboundedV1");
	if (!document.contains("sourceId") || !document["sourceId"].is_string()) return fail("TerraPrefabUnboundedV1 requires a string sourceId");
	return document.contains("objects") && document["objects"].is_array() ? true : fail("TerraPrefabUnboundedV1 requires an objects array");
}
