// ScriptGlue.cpp — Contains the Script Glue file-specific types, helper functions, and module integration code implementation for native script glue, exported C# vtables, and scripting system lifecycle state.

#include "ScriptGlue.h"
#include "Core/Logger/Logger.h"
#include "Core/MainEngine.h"
#include "Core/Object/Component.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"
#include "Components/UIComponent.h"
#include "Components/Transform.h"
#include "Components/ScriptComponent.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/ConstraintSetComponent.h"
#include "DotNet/DotNetHost.h"
#include "ScriptingSystem.h"
#include "UI/UIElement.h"
#include "UI/UISystem.h"
#include "Input/InputSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Utilities/PlatformUtils.h"

#include <array>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Renderer/RenderSystem.h"
#include "Resource/ResourceManager.h"

static std::shared_mutex g_ScriptGlueMutex;
// Native_ScriptComponent_GetBindingPayload returns a raw const char*, so each
// thread keeps its own backing string alive until the next bridge call.
thread_local std::string g_ScriptBindingPayloadBuffer;
thread_local std::string g_ApplicationReturnBuffer;
std::mutex g_ApplicationBridgeMutex;
Engine* g_ApplicationHostEngine = nullptr;
ApplicationEditorStopPlayModeCallback g_ApplicationEditorStopCallback = nullptr;
thread_local std::string g_UIBridgeReturnBuffer;
thread_local std::string g_SceneBridgeReturnBuffer;
thread_local std::string g_RuntimeBridgeReturnBuffer;

namespace {
	struct ManagedPrefabInstantiationRequest
	{
		std::string path;
		TerraEngine::UUID parentObjectId = TerraEngine::EMPTY_UUID;
	};

	struct ManagedSceneLoadRequest
	{
		std::string path;
		bool prefab = false;
	};

	struct ManagedResourceReleaseRequest
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		AssetType assetType = AssetType::Mesh;
	};

	std::mutex g_ManagedRuntimeRequestMutex;
	std::optional<SceneRenderSettings> g_PendingManagedSceneRenderSettings;
	std::optional<TerraEngine::UUID> g_PendingManagedActiveScene;
	std::vector<ManagedSceneLoadRequest> g_PendingManagedSceneLoads;
	std::vector<ManagedPrefabInstantiationRequest> g_PendingManagedPrefabInstantiations;
	std::vector<ManagedResourceReleaseRequest> g_PendingManagedResourceReleases;
	bool g_PendingManagedSceneUnloadAll = false;
	std::uint64_t g_ManagedSceneRenderSettingsGeneration = 0U;
	std::uint64_t g_ManagedRuntimeFrameId = 0U;

	bool HasRuntimeAbiHeader(const NativeRuntimeAbiHeader& header, std::size_t requiredSize)
	{
		return header.abiVersion == kNativeRuntimeAbiVersion && header.structSize >= requiredSize;
	}

	void WriteRuntimeDiagnostic(char (&destination)[kNativeRuntimeDiagnosticBytes], std::string_view text)
	{
		std::memset(destination, 0, sizeof(destination));
		const std::size_t count = std::min(text.size(), sizeof(destination) - 1U);
		std::memcpy(destination, text.data(), count);
	}

	std::optional<AssetType> ManagedAssetType(int assetType)
	{
		switch (assetType)
		{
		case 0: return AssetType::Mesh;
		case 1: return AssetType::Texture;
		case 2: return AssetType::Material;
		case 3: return AssetType::ShaderCollection;
		default: return std::nullopt;
		}
	}

	void CollectPrefabUuidRemap(const nlohmann::json& value, std::unordered_map<std::string, std::string>& remap)
	{
		if (value.is_object())
		{
			if (const auto uuid = value.find("uuid"); uuid != value.end() && uuid->is_string())
			{
				remap.try_emplace(uuid->get<std::string>(), TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
			}
			for (const auto& [key, child] : value.items())
			{
				(void)key;
				CollectPrefabUuidRemap(child, remap);
			}
		}
		else if (value.is_array())
		{
			for (const nlohmann::json& child : value)
			{
				CollectPrefabUuidRemap(child, remap);
			}
		}
	}

	void RewritePrefabUuidReferences(nlohmann::json& value, const std::unordered_map<std::string, std::string>& remap)
	{
		if (value.is_string())
		{
			const std::string current = value.get<std::string>();
			if (const auto replacement = remap.find(current); replacement != remap.end())
			{
				value = replacement->second;
			}
			return;
		}
		if (value.is_object())
		{
			for (auto& [key, child] : value.items())
			{
				(void)key;
				RewritePrefabUuidReferences(child, remap);
			}
		}
		else if (value.is_array())
		{
			for (nlohmann::json& child : value)
			{
				RewritePrefabUuidReferences(child, remap);
			}
		}
	}
}

static std::optional<Object> ResolveManagedObject(uint32_t objectId)
{
	return ObjectManager::instance().getObject(static_cast<entt::entity>(objectId));
}

namespace {
	using namespace TerraEngine::Physics;

	// ABI validation happens before the bridge reads any caller-owned field. This
	// prevents an older or malformed managed packet from changing a live world.
	template<typename T>
	bool HasPhysicsAbiHeader(const T* value) {
		return value != nullptr
			&& value->header.abiVersion == kNativePhysicsAbiVersion
			&& value->header.structSize >= sizeof(T);
	}

	NativePhysicsStatus ToNativeStatus(PhysicsResultStatus status) {
		switch(status) {
		case PhysicsResultStatus::Success: return NativePhysicsStatus::Success;
		case PhysicsResultStatus::Accepted: return NativePhysicsStatus::Accepted;
		case PhysicsResultStatus::Cancelled: return NativePhysicsStatus::Cancelled;
		case PhysicsResultStatus::StaleTarget: return NativePhysicsStatus::StaleTarget;
		case PhysicsResultStatus::Expired: return NativePhysicsStatus::Expired;
		case PhysicsResultStatus::LimitExceeded: return NativePhysicsStatus::LimitExceeded;
		case PhysicsResultStatus::Conflict: return NativePhysicsStatus::Conflict;
		case PhysicsResultStatus::ProviderFailure: return NativePhysicsStatus::ProviderFailure;
		default: return NativePhysicsStatus::InvalidState;
		}
	}

	NativePhysicsStatus ToNativeStatus(PhysicsCommandRejectionReason reason) {
		switch(reason) {
		case PhysicsCommandRejectionReason::None: return NativePhysicsStatus::Accepted;
		case PhysicsCommandRejectionReason::GlobalCapacity:
		case PhysicsCommandRejectionReason::ProducerCapacity: return NativePhysicsStatus::LimitExceeded;
		case PhysicsCommandRejectionReason::IdempotencyConflict: return NativePhysicsStatus::Conflict;
		case PhysicsCommandRejectionReason::QueueStopped: return NativePhysicsStatus::InvalidState;
		default: return NativePhysicsStatus::InvalidArgument;
		}
	}

	NativePhysicsStatus ToNativeStatus(PhysicsQueryRejectionReason reason) {
		switch(reason) {
		case PhysicsQueryRejectionReason::None: return NativePhysicsStatus::Accepted;
		case PhysicsQueryRejectionReason::BatchLimit: return NativePhysicsStatus::LimitExceeded;
		case PhysicsQueryRejectionReason::StaleWorld:
		case PhysicsQueryRejectionReason::StaleHandle: return NativePhysicsStatus::StaleTarget;
		case PhysicsQueryRejectionReason::Cancelled: return NativePhysicsStatus::Cancelled;
		case PhysicsQueryRejectionReason::Expired: return NativePhysicsStatus::Expired;
		case PhysicsQueryRejectionReason::ResultUnavailable: return NativePhysicsStatus::InvalidState;
		default: return NativePhysicsStatus::InvalidArgument;
		}
	}

	void WriteDiagnostic(char (&destination)[kNativePhysicsDiagnosticBytes], std::string_view text) {
		std::memset(destination, 0, sizeof(destination));
		const std::size_t count = std::min(text.size(), sizeof(destination) - 1U);
		std::memcpy(destination, text.data(), count);
	}

	TerraEngine::UUID ToUuid(NativePhysicsUuid value) {
		TerraEngine::UUID result{};
		std::memcpy(result.data(), &value.first, sizeof(value.first));
		std::memcpy(result.data() + sizeof(value.first), &value.second, sizeof(value.second));
		return result;
	}

	NativePhysicsUuid FromUuid(const TerraEngine::UUID& value) {
		NativePhysicsUuid result{};
		std::memcpy(&result.first, value.data(), sizeof(result.first));
		std::memcpy(&result.second, value.data() + sizeof(result.first), sizeof(result.second));
		return result;
	}

	PhysicsWorldHandle ToWorld(NativePhysicsWorldHandle value) { return {{value.worldId}, value.worldGeneration}; }
	NativePhysicsWorldHandle FromWorld(PhysicsWorldHandle value) { return {value.worldId.value, value.worldGeneration, 0U}; }
	template<typename NativeHandle, typename Handle>
	Handle ToHandle(const NativeHandle& value) { return {{value.world.worldId}, value.world.worldGeneration, value.slotIndex, value.slotGeneration}; }
	template<typename Handle>
	NativePhysicsHandle FromHandle(Handle value) { return {FromWorld({value.worldId, value.worldGeneration}), value.slotIndex, value.slotGeneration}; }
	PhysicsQueryHandle ToQueryHandle(const NativePhysicsQueryHandle& value) { return {{value.world.worldId}, value.world.worldGeneration, value.resultSlot, value.resultGeneration, value.submittedTick, value.expiryTick}; }
	NativePhysicsQueryHandle FromQueryHandle(PhysicsQueryHandle value) { return {FromWorld({value.worldId, value.worldGeneration}), value.resultSlot, value.resultGeneration, value.submittedTick, value.expiryTick}; }
	PhysicsVector3 ToVector(NativePhysicsVector3 value) { return {value.x, value.y, value.z}; }
	NativePhysicsVector3 FromVector(PhysicsVector3 value) { return {value.x, value.y, value.z}; }
	PhysicsQuaternion ToQuaternion(NativePhysicsQuaternion value) { return {value.x, value.y, value.z, value.w}; }
	NativePhysicsQuaternion FromQuaternion(PhysicsQuaternion value) { return {value.x, value.y, value.z, value.w}; }
	PhysicsPose ToPose(NativePhysicsPose value) { return {ToVector(value.position), {value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w}}; }
	NativePhysicsPose FromPose(PhysicsPose value) { return {FromVector(value.position), {value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w}}; }

	bool IsFinite(NativePhysicsVector3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
	bool IsFinite(NativePhysicsPose value) { return IsFinite(value.position) && std::isfinite(value.rotation.x) && std::isfinite(value.rotation.y) && std::isfinite(value.rotation.z) && std::isfinite(value.rotation.w); }
	bool SameWorld(NativePhysicsWorldHandle received, PhysicsWorldHandle active) { return ToWorld(received) == active && active.IsValid(); }
	NativePhysicsIdentity NativeIdentity(
		const TerraEngine::UUID& objectId,
		PhysicsGeneration objectGeneration,
		PhysicsGeneration componentGeneration,
		const TerraEngine::UUID& recordId = TerraEngine::EMPTY_UUID,
		PhysicsGeneration recordGeneration = 0U) {
		std::uint32_t bindingGeneration = 0U;
		if(const std::optional<Object> object = ObjectManager::instance().getObject(objectId)) {
			bindingGeneration = TerraEngine::CompressObjectGeneration(
				ObjectManager::instance().getObjectBindingGeneration(*object));
		}
		return {FromUuid(objectId), objectGeneration, componentGeneration, FromUuid(recordId), recordGeneration, bindingGeneration};
	}
	bool HasCurrentManagedIdentity(const NativePhysicsIdentity& identity) {
		// Resolve all generations from the authoritative live object. A wrapper
		// created before component recreation or managed assembly reload cannot
		// accidentally address a replacement with the same UUID.
		if((identity.objectId.first == 0U && identity.objectId.second == 0U)
			|| identity.objectGeneration == 0U || identity.componentGeneration == 0U
			|| identity.bindingGeneration == 0U) return false;
		const std::optional<Object> object = ObjectManager::instance().getObject(ToUuid(identity.objectId));
		if(!object) return false;
		ObjectManager& objects = ObjectManager::instance();
		return TerraEngine::CompressObjectGeneration(objects.getObjectLifetimeGeneration(*object)) == identity.objectGeneration
			&& TerraEngine::CompressObjectGeneration(objects.getObjectComponentGeneration(*object)) == identity.componentGeneration
			&& TerraEngine::CompressObjectGeneration(objects.getObjectBindingGeneration(*object)) == identity.bindingGeneration;
	}

	PhysicsCommandMetadata Metadata(std::uint64_t tick, std::uint64_t producer, std::uint64_t sequence, std::uint64_t correlation) {
		PhysicsCommandMetadata result;
		result.effectiveTick = tick;
		result.producerId = producer == 0U ? 1U : producer;
		result.producerSequence = sequence == 0U ? 1U : sequence;
		result.correlationId = "managed-" + std::to_string(correlation);
		return result;
	}

	PhysicsBodyCommandTarget BodyTarget(const NativePhysicsBodyCommandRequest& request) {
		return {ToWorld(request.body.world), ToUuid(request.identity.objectId), request.identity.objectGeneration, request.identity.componentGeneration};
	}
	PhysicsCharacterCommandTarget CharacterTarget(const NativePhysicsCharacterCommandRequest& request) {
		return {ToWorld(request.character.world), ToHandle<NativePhysicsHandle, PhysicsCharacterHandle>(request.character), ToUuid(request.identity.objectId), request.identity.objectGeneration, request.identity.componentGeneration};
	}

	void FillReceipt(NativePhysicsCommandReceipt& destination, NativePhysicsStatus status, const PhysicsCommandReceipt& receipt, std::string_view diagnostic = {}) {
		destination.header = {kNativePhysicsAbiVersion, sizeof(destination)};
		destination.status = status;
		destination.reserved = 0U;
		destination.commandId = receipt.commandId;
		destination.effectiveTick = receipt.effectiveTick;
		destination.correlationId = 0U;
		WriteDiagnostic(destination.diagnostic, diagnostic.empty() ? std::string_view(ToString(receipt.rejectionReason)) : diagnostic);
	}

	// Query submission has its own receipt shape, but it follows the same rule
	// as commands: callers receive a fully initialized copied result even when
	// validation rejects the request before it reaches the deferred queue.
	void FillQueryReceipt(NativePhysicsQueryReceipt& destination, NativePhysicsStatus status, std::uint64_t correlationId, std::string_view diagnostic) {
		destination.header = {kNativePhysicsAbiVersion, sizeof(destination)};
		destination.status = status;
		destination.reserved = 0U;
		destination.handle = {};
		destination.correlationId = correlationId;
		WriteDiagnostic(destination.diagnostic, diagnostic);
	}

	bool MatchesPublishedBody(const NativePhysicsBodyCommandRequest& request) {
		const auto snapshot = PhysicsSystem::Instance().LatestBodySnapshot();
		if(!snapshot || !snapshot->bodies) return false;
		const PhysicsBodyHandle requested = ToHandle<NativePhysicsHandle, PhysicsBodyHandle>(request.body);
		const TerraEngine::UUID objectId = ToUuid(request.identity.objectId);
		return std::any_of(snapshot->bodies->begin(), snapshot->bodies->end(), [&](const PhysicsBodyState& candidate) {
			return candidate.handle == requested
				&& candidate.objectId == objectId
				&& candidate.objectGeneration == request.identity.objectGeneration
				&& candidate.componentGeneration == request.identity.componentGeneration;
		});
	}

	bool MatchesPublishedCharacter(const NativePhysicsCharacterCommandRequest& request) {
		const auto snapshot = PhysicsSystem::Instance().LatestCharacterStatePacket();
		if(!snapshot || !snapshot->states) return false;
		const PhysicsCharacterHandle requested = ToHandle<NativePhysicsHandle, PhysicsCharacterHandle>(request.character);
		const TerraEngine::UUID objectId = ToUuid(request.identity.objectId);
		return std::any_of(snapshot->states->begin(), snapshot->states->end(), [&](const PhysicsCharacterState& candidate) {
			return candidate.handle == requested
				&& candidate.objectId == objectId
				&& candidate.objectGeneration == request.identity.objectGeneration
				&& candidate.componentGeneration == request.identity.componentGeneration;
		});
	}

	PhysicsQueryRequest ToQueryRequest(const NativePhysicsQueryRequest& request, PhysicsWorldHandle activeWorld) {
		PhysicsQueryRequest result;
		result.world = activeWorld;
		result.kind = static_cast<PhysicsQueryKind>(request.kind);
		result.hitMode = static_cast<PhysicsQueryHitMode>(request.hitMode);
		result.pose = ToPose(request.pose);
		result.direction = ToVector(request.direction);
		result.shapeExtent = ToVector(request.shapeExtent);
		result.distance = request.distance;
		result.maximumHits = request.maximumHits;
		result.filter.collisionMask = request.collisionMask;
		result.filter.bodyTypes = static_cast<PhysicsQueryBodyTypeMask>(request.bodyTypeMask);
		result.filter.includeSensors = request.includeSensors != 0U;
		result.submittedTick = request.submittedTick;
		result.expiryTick = request.expiryTick;
		result.correlationId = "managed-" + std::to_string(request.correlationId);
		return result;
	}

	std::uint64_t ManagedCorrelationId(std::string_view correlation) {
		constexpr std::string_view prefix = "managed-";
		if(!correlation.starts_with(prefix)) return 0U;
		std::uint64_t value = 0U;
		const char* begin = correlation.data() + prefix.size();
		const char* end = correlation.data() + correlation.size();
		const auto [parsed, error] = std::from_chars(begin, end, value);
		return error == std::errc{} && parsed == end ? value : 0U;
	}

	NativePhysicsStatus CopyQueryResult(const PhysicsQueryResult& result, NativePhysicsQueryResultInfo& info, NativePhysicsQueryHit* hits, std::uint32_t hitCapacity, std::uint32_t& requiredHitCount) {
		const std::size_t count = result.hits ? result.hits->size() : 0U;
		requiredHitCount = static_cast<std::uint32_t>(count);
		info.header = {kNativePhysicsAbiVersion, sizeof(info)};
		info.status = ToNativeStatus(result.status);
		info.hitCount = requiredHitCount;
		info.truncated = result.truncated ? 1U : 0U;
		info.tick = result.tick;
		// Physics owns a stable string correlation for producer ordering. Convert
		// only the bridge's documented numeric form back to the fixed-width ABI
		// field so immediate and scheduled managed results preserve correlation.
		info.correlationId = ManagedCorrelationId(result.correlationId);
		WriteDiagnostic(info.diagnostic, result.diagnostic);
		if(count > hitCapacity || (count != 0U && hits == nullptr)) return NativePhysicsStatus::LimitExceeded;
		for(std::size_t index = 0U; index < count; ++index) {
			const PhysicsQueryHit& source = result.hits->at(index);
			NativePhysicsQueryHit& destination = hits[index];
			destination.body = FromHandle(source.body);
			destination.objectId = FromUuid(source.objectId);
			destination.objectGeneration = source.objectGeneration;
			destination.componentGeneration = source.componentGeneration;
			destination.materialId = FromUuid(source.materialId);
			destination.position = FromVector(source.position);
			destination.normal = FromVector(source.normal);
			destination.distance = source.distance;
			destination.fraction = source.fraction;
			destination.subShapeKey = source.subShapeKey;
			destination.collisionLayer = source.collisionLayer;
			destination.motionType = static_cast<std::uint8_t>(source.motionType);
			destination.sensor = source.sensor ? 1U : 0U;
			destination.reserved = 0U;
		}
		return info.status;
	}

	constexpr std::uint32_t kMaximumManagedAuthoringRecords = 4096U;

	NativePhysicsColliderRecord FromColliderRecord(const ColliderRecord& source) {
		NativePhysicsColliderRecord destination{};
		destination.header = {kNativePhysicsAbiVersion, sizeof(destination)};
		destination.recordId = FromUuid(source.recordId);
		destination.recordVersion = source.recordVersion;
		destination.shapeKind = static_cast<std::uint32_t>(source.Kind());
		destination.localCenter = FromVector(source.localCenter);
		destination.localRotation = FromQuaternion(source.localRotation);
		std::visit([&](const auto& shape) {
			using Shape = std::decay_t<decltype(shape)>;
			if constexpr(std::is_same_v<Shape, BoxColliderShape>) destination.halfExtents = FromVector(shape.halfExtents);
			else if constexpr(std::is_same_v<Shape, SphereColliderShape>) destination.radius = shape.radius;
			else if constexpr(std::is_same_v<Shape, CapsuleColliderShape> || std::is_same_v<Shape, CylinderColliderShape>) {
				destination.radius = shape.radius;
				destination.height = shape.height;
			}
			else if constexpr(std::is_same_v<Shape, TaperedCapsuleColliderShape>) {
				destination.topRadius = shape.topRadius;
				destination.bottomRadius = shape.bottomRadius;
				destination.height = shape.height;
			}
			else destination.colliderAssetId = FromUuid(shape.colliderAssetId);
		}, source.shape);
		destination.materialId = FromUuid(source.materialId);
		destination.queryFlags = source.queryFlags;
		destination.collisionLayerId = source.collisionLayerId;
		destination.eventPolicy = static_cast<std::uint8_t>(source.eventPolicy);
		destination.useDefaultMaterial = source.useDefaultMaterial ? 1U : 0U;
		destination.isTrigger = source.isTrigger ? 1U : 0U;
		destination.useLayerOverride = source.useLayerOverride ? 1U : 0U;
		destination.enabled = source.enabled ? 1U : 0U;
		destination.hasDebugColorOverride = source.hasDebugColorOverride ? 1U : 0U;
		destination.debugRed = source.debugColor.red;
		destination.debugGreen = source.debugColor.green;
		destination.debugBlue = source.debugColor.blue;
		destination.debugAlpha = source.debugColor.alpha;
		return destination;
	}

	std::optional<ColliderRecord> ToColliderRecord(const NativePhysicsColliderRecord& source) {
		if(!HasPhysicsAbiHeader(&source)) return std::nullopt;
		ColliderRecord destination;
		destination.recordId = ToUuid(source.recordId);
		destination.recordVersion = source.recordVersion;
		destination.localCenter = ToVector(source.localCenter);
		destination.localRotation = ToQuaternion(source.localRotation);
		switch(static_cast<PhysicsShapeKind>(source.shapeKind)) {
		case PhysicsShapeKind::Box: destination.shape = BoxColliderShape{ToVector(source.halfExtents)}; break;
		case PhysicsShapeKind::Sphere: destination.shape = SphereColliderShape{source.radius}; break;
		case PhysicsShapeKind::Capsule: destination.shape = CapsuleColliderShape{source.radius, source.height}; break;
		case PhysicsShapeKind::Cylinder: destination.shape = CylinderColliderShape{source.radius, source.height}; break;
		case PhysicsShapeKind::TaperedCapsule: destination.shape = TaperedCapsuleColliderShape{source.topRadius, source.bottomRadius, source.height}; break;
		case PhysicsShapeKind::ConvexHull: destination.shape = ConvexHullColliderShape{ToUuid(source.colliderAssetId)}; break;
		case PhysicsShapeKind::Mesh: destination.shape = TriangleMeshColliderShape{ToUuid(source.colliderAssetId)}; break;
		case PhysicsShapeKind::Compound: destination.shape = CompoundColliderShape{ToUuid(source.colliderAssetId)}; break;
		default: return std::nullopt;
		}
		destination.materialId = ToUuid(source.materialId);
		destination.queryFlags = source.queryFlags;
		destination.collisionLayerId = source.collisionLayerId;
		destination.eventPolicy = static_cast<PhysicsColliderEventPolicy>(source.eventPolicy);
		destination.useDefaultMaterial = source.useDefaultMaterial != 0U;
		destination.isTrigger = source.isTrigger != 0U;
		destination.useLayerOverride = source.useLayerOverride != 0U;
		destination.enabled = source.enabled != 0U;
		destination.hasDebugColorOverride = source.hasDebugColorOverride != 0U;
		destination.debugColor = {source.debugRed, source.debugGreen, source.debugBlue, source.debugAlpha};
		return destination;
	}

	NativePhysicsConstraintLimits FromConstraintLimits(const PhysicsConstraintLimits& source) {
		return {source.enabled ? 1U : 0U, source.minimum, source.maximum};
	}

	NativePhysicsConstraintSpring FromConstraintSpring(const PhysicsConstraintSpring& source) {
		return {source.enabled ? 1U : 0U, source.stiffness, source.damping};
	}

	NativePhysicsConstraintMotor FromConstraintMotor(const PhysicsConstraintMotor& source) {
		return {static_cast<std::uint32_t>(source.mode), source.targetPosition, source.targetVelocity, source.maximumForce};
	}

	PhysicsConstraintLimits ToConstraintLimits(const NativePhysicsConstraintLimits& source) {
		return {source.enabled != 0U, source.minimum, source.maximum};
	}

	PhysicsConstraintSpring ToConstraintSpring(const NativePhysicsConstraintSpring& source) {
		return {source.enabled != 0U, source.stiffness, source.damping};
	}

	PhysicsConstraintMotor ToConstraintMotor(const NativePhysicsConstraintMotor& source) {
		return {static_cast<PhysicsConstraintMotorMode>(source.mode), source.targetPosition, source.targetVelocity, source.maximumForce};
	}

	NativePhysicsConstraintRecord FromConstraintRecord(const ConstraintRecord& source) {
		NativePhysicsConstraintRecord destination{};
		destination.header = {kNativePhysicsAbiVersion, sizeof(destination)};
		destination.recordId = FromUuid(source.recordId);
		destination.recordVersion = source.recordVersion;
		destination.kind = static_cast<std::uint32_t>(source.kind);
		destination.firstEndpointKind = static_cast<std::uint32_t>(source.firstEndpointKind);
		destination.secondEndpointKind = static_cast<std::uint32_t>(source.secondEndpointKind);
		destination.firstObjectId = FromUuid(source.firstObjectId);
		destination.secondObjectId = FromUuid(source.secondObjectId);
		destination.firstFrame = FromPose(source.firstFrame);
		destination.secondFrame = FromPose(source.secondFrame);
		destination.firstAxis = FromVector(source.firstAxis);
		destination.secondAxis = FromVector(source.secondAxis);
		destination.limits = FromConstraintLimits(source.limits);
		destination.spring = FromConstraintSpring(source.spring);
		destination.motor = FromConstraintMotor(source.motor);
		for(std::size_t index = 0U; index < source.axisLimits.size(); ++index) {
			destination.axisSettings[index].limits = FromConstraintLimits(source.axisLimits[index]);
			destination.axisSettings[index].spring = FromConstraintSpring(source.axisSprings[index]);
			destination.axisSettings[index].motor = FromConstraintMotor(source.axisMotors[index]);
		}
		destination.firstGearConstraintId = FromUuid(source.firstGearConstraintId);
		destination.secondGearConstraintId = FromUuid(source.secondGearConstraintId);
		destination.gearRatio = source.gearRatio;
		destination.breakForce = source.breakForce;
		destination.breakTorque = source.breakTorque;
		destination.resetGeneration = source.resetGeneration;
		destination.collideConnected = source.collideConnected ? 1U : 0U;
		destination.enabled = source.enabled ? 1U : 0U;
		return destination;
	}

	std::optional<ConstraintRecord> ToConstraintRecord(const NativePhysicsConstraintRecord& source) {
		if(!HasPhysicsAbiHeader(&source)) return std::nullopt;
		ConstraintRecord destination;
		destination.recordId = ToUuid(source.recordId);
		destination.recordVersion = source.recordVersion;
		destination.kind = static_cast<PhysicsConstraintKind>(source.kind);
		destination.firstEndpointKind = static_cast<PhysicsConstraintEndpointKind>(source.firstEndpointKind);
		destination.secondEndpointKind = static_cast<PhysicsConstraintEndpointKind>(source.secondEndpointKind);
		destination.firstObjectId = ToUuid(source.firstObjectId);
		destination.secondObjectId = ToUuid(source.secondObjectId);
		destination.firstFrame = ToPose(source.firstFrame);
		destination.secondFrame = ToPose(source.secondFrame);
		destination.firstAxis = ToVector(source.firstAxis);
		destination.secondAxis = ToVector(source.secondAxis);
		destination.limits = ToConstraintLimits(source.limits);
		destination.spring = ToConstraintSpring(source.spring);
		destination.motor = ToConstraintMotor(source.motor);
		for(std::size_t index = 0U; index < destination.axisLimits.size(); ++index) {
			destination.axisLimits[index] = ToConstraintLimits(source.axisSettings[index].limits);
			destination.axisSprings[index] = ToConstraintSpring(source.axisSettings[index].spring);
			destination.axisMotors[index] = ToConstraintMotor(source.axisSettings[index].motor);
		}
		destination.firstGearConstraintId = ToUuid(source.firstGearConstraintId);
		destination.secondGearConstraintId = ToUuid(source.secondGearConstraintId);
		destination.gearRatio = source.gearRatio;
		destination.breakForce = source.breakForce;
		destination.breakTorque = source.breakTorque;
		destination.resetGeneration = source.resetGeneration;
		destination.collideConnected = source.collideConnected != 0U;
		destination.enabled = source.enabled != 0U;
		return destination;
	}

	template<typename Record>
	NativePhysicsStatus ValidateManagedRecordInput(const Record* records, std::uint32_t recordCount) {
		if(recordCount > kMaximumManagedAuthoringRecords) return NativePhysicsStatus::LimitExceeded;
		if(recordCount != 0U && records == nullptr) return NativePhysicsStatus::InvalidArgument;
		for(std::uint32_t index = 0U; index < recordCount; ++index) {
			if(!HasPhysicsAbiHeader(&records[index])) return NativePhysicsStatus::VersionMismatch;
		}
		return NativePhysicsStatus::Success;
	}
}

extern "C" {
	std::uint32_t Native_Physics_GetAbiVersion() { return kNativePhysicsAbiVersion; }

	NativePhysicsStatus Native_Physics_SubmitBodyCommand(const NativePhysicsBodyCommandRequest* request, NativePhysicsCommandReceipt* receipt) {
		if(!HasPhysicsAbiHeader(request) || !HasPhysicsAbiHeader(receipt)) return NativePhysicsStatus::VersionMismatch;
		const PhysicsWorldHandle active = PhysicsSystem::Instance().WorldHandle();
		if(!SameWorld(request->body.world, active) || !ToHandle<NativePhysicsHandle, PhysicsBodyHandle>(request->body).IsValid() || !HasCurrentManagedIdentity(request->identity) || !MatchesPublishedBody(*request)) {
			FillReceipt(*receipt, NativePhysicsStatus::StaleTarget, {}, "Body handle or world generation is stale.");
			return receipt->status;
		}
		if(!IsFinite(request->value) || !IsFinite(request->point) || !IsFinite(request->pose) || !IsFinite(request->angularValue) || !std::isfinite(request->duration)) {
			FillReceipt(*receipt, NativePhysicsStatus::InvalidArgument, {}, "Physics command contains a non-finite value.");
			return receipt->status;
		}
		const PhysicsCommandMetadata metadata = Metadata(request->effectiveTick, request->producerId, request->producerSequence, request->correlationId);
		const PhysicsBodyCommandTarget target = BodyTarget(*request);
		PhysicsCommand command;
		switch(request->kind) {
		case NativePhysicsBodyCommandKind::Force: command = PhysicsCommand::MakeContinuousForce(target, ToVector(request->value), metadata); break;
		case NativePhysicsBodyCommandKind::Acceleration: command = PhysicsCommand::MakeAcceleration(target, ToVector(request->value), metadata); break;
		case NativePhysicsBodyCommandKind::Impulse: command = PhysicsCommand::MakeImpulse(target, ToVector(request->value), metadata); break;
		case NativePhysicsBodyCommandKind::PointImpulse: command = PhysicsCommand::MakePointImpulse(target, ToVector(request->value), ToVector(request->point), metadata); break;
		case NativePhysicsBodyCommandKind::VelocityChange: command = PhysicsCommand::MakeVelocityChange(target, ToVector(request->value), metadata); break;
		case NativePhysicsBodyCommandKind::SetVelocity: command = PhysicsCommand::MakeSetVelocity(target, ToVector(request->value), ToVector(request->angularValue), metadata); break;
		case NativePhysicsBodyCommandKind::Teleport: command = PhysicsCommand::MakeTeleport(target, ToPose(request->pose), ToVector(request->value), ToVector(request->angularValue), metadata); break;
		case NativePhysicsBodyCommandKind::KinematicTarget: command = PhysicsCommand::MakeKinematicTarget(target, ToPose(request->pose), request->duration, metadata); break;
		case NativePhysicsBodyCommandKind::Wake: command = PhysicsCommand::MakeWake(target, metadata); break;
		case NativePhysicsBodyCommandKind::Sleep: command = PhysicsCommand::MakeSleep(target, metadata); break;
		default: FillReceipt(*receipt, NativePhysicsStatus::InvalidArgument, {}, "Unknown body command kind."); return receipt->status;
		}
		const PhysicsCommandReceipt nativeReceipt = PhysicsSystem::Instance().SubmitCommand(std::move(command));
		const NativePhysicsStatus status = nativeReceipt.accepted ? NativePhysicsStatus::Accepted : ToNativeStatus(nativeReceipt.rejectionReason);
		FillReceipt(*receipt, status, nativeReceipt);
		return status;
	}

	NativePhysicsStatus Native_Physics_SubmitCharacterCommand(const NativePhysicsCharacterCommandRequest* request, NativePhysicsCommandReceipt* receipt) {
		if(!HasPhysicsAbiHeader(request) || !HasPhysicsAbiHeader(receipt)) return NativePhysicsStatus::VersionMismatch;
		const PhysicsWorldHandle active = PhysicsSystem::Instance().WorldHandle();
		if(!SameWorld(request->character.world, active) || !ToHandle<NativePhysicsHandle, PhysicsCharacterHandle>(request->character).IsValid() || !HasCurrentManagedIdentity(request->identity) || !MatchesPublishedCharacter(*request)) {
			FillReceipt(*receipt, NativePhysicsStatus::StaleTarget, {}, "Character handle or world generation is stale."); return receipt->status;
		}
		if(!IsFinite(request->desiredVelocity) || !IsFinite(request->pose) || !std::isfinite(request->scalar)) { FillReceipt(*receipt, NativePhysicsStatus::InvalidArgument, {}, "Character command contains a non-finite value."); return receipt->status; }
		const PhysicsCommandMetadata metadata = Metadata(request->effectiveTick, request->producerId, request->producerSequence, request->correlationId);
		const PhysicsCharacterCommandTarget target = CharacterTarget(*request);
		PhysicsCommand command;
		switch(request->kind) {
		case NativePhysicsCharacterCommandKind::Move: command = PhysicsCommand::MakeCharacterMove(target, ToVector(request->desiredVelocity), metadata); break;
		case NativePhysicsCharacterCommandKind::Jump: command = PhysicsCommand::MakeCharacterJump(target, request->scalar, metadata); break;
		case NativePhysicsCharacterCommandKind::Resize: command = PhysicsCommand::MakeCharacterResize(target, request->scalar, metadata); break;
		case NativePhysicsCharacterCommandKind::Teleport: command = PhysicsCommand::MakeCharacterTeleport(target, ToPose(request->pose), metadata); break;
		case NativePhysicsCharacterCommandKind::RefreshConfiguration: command = PhysicsCommand::MakeCharacterConfigurationUpdate(target, metadata); break;
		default: FillReceipt(*receipt, NativePhysicsStatus::InvalidArgument, {}, "Unknown character command kind."); return receipt->status;
		}
		const PhysicsCommandReceipt nativeReceipt = PhysicsSystem::Instance().SubmitCommand(std::move(command));
		const NativePhysicsStatus status = nativeReceipt.accepted ? NativePhysicsStatus::Accepted : ToNativeStatus(nativeReceipt.rejectionReason);
		FillReceipt(*receipt, status, nativeReceipt);
		return status;
	}

	NativePhysicsStatus Native_Physics_QueryImmediate(const NativePhysicsQueryRequest* request, NativePhysicsQueryResultInfo* info, NativePhysicsQueryHit* hits, std::uint32_t hitCapacity, std::uint32_t* requiredHitCount) {
		if(!HasPhysicsAbiHeader(request) || !HasPhysicsAbiHeader(info) || requiredHitCount == nullptr) return NativePhysicsStatus::VersionMismatch;
		const PhysicsWorldHandle active = PhysicsSystem::Instance().WorldHandle();
		if(!SameWorld(request->world, active)) {
			*requiredHitCount = 0U;
			info->header = {kNativePhysicsAbiVersion, sizeof(*info)};
			info->status = NativePhysicsStatus::StaleTarget;
			info->hitCount = 0U;
			info->truncated = 0U;
			info->tick = 0U;
			info->correlationId = request->correlationId;
			WriteDiagnostic(info->diagnostic, "Query world generation is stale.");
			return info->status;
		}
		if(request->kind > NativePhysicsQueryKind::Point || request->hitMode > static_cast<std::uint32_t>(PhysicsQueryHitMode::All)
			|| !IsFinite(request->pose) || !IsFinite(request->direction) || !IsFinite(request->shapeExtent) || !std::isfinite(request->distance)) {
			*requiredHitCount = 0U;
			info->header = {kNativePhysicsAbiVersion, sizeof(*info)};
			info->status = NativePhysicsStatus::InvalidArgument;
			info->hitCount = 0U;
			info->truncated = 0U;
			info->tick = 0U;
			info->correlationId = request->correlationId;
			WriteDiagnostic(info->diagnostic, "Query contains an unsupported or non-finite value.");
			return info->status;
		}
		const PhysicsQueryResult result = PhysicsSystem::Instance().QueryImmediate(ToQueryRequest(*request, active));
		return CopyQueryResult(result, *info, hits, hitCapacity, *requiredHitCount);
	}

	NativePhysicsStatus Native_Physics_SubmitQuery(const NativePhysicsQueryRequest* request, NativePhysicsQueryReceipt* receipt) {
		if(!HasPhysicsAbiHeader(request) || !HasPhysicsAbiHeader(receipt)) return NativePhysicsStatus::VersionMismatch;
		const PhysicsWorldHandle active = PhysicsSystem::Instance().WorldHandle();
		if(!SameWorld(request->world, active)) {
			FillQueryReceipt(*receipt, NativePhysicsStatus::StaleTarget, request->correlationId, "Query world generation is stale.");
			return receipt->status;
		}
		if(request->kind > NativePhysicsQueryKind::Point || request->hitMode > static_cast<std::uint32_t>(PhysicsQueryHitMode::All)
			|| !IsFinite(request->pose) || !IsFinite(request->direction) || !IsFinite(request->shapeExtent) || !std::isfinite(request->distance)) {
			FillQueryReceipt(*receipt, NativePhysicsStatus::InvalidArgument, request->correlationId, "Query contains an unsupported or non-finite value.");
			return receipt->status;
		}
		const PhysicsQueryReceipt nativeReceipt = PhysicsSystem::Instance().SubmitQuery(ToQueryRequest(*request, active));
		receipt->header = {kNativePhysicsAbiVersion, sizeof(*receipt)};
		receipt->status = nativeReceipt.accepted ? NativePhysicsStatus::Accepted : ToNativeStatus(nativeReceipt.rejectionReason);
		receipt->reserved = 0U;
		receipt->handle = FromQueryHandle(nativeReceipt.handle);
		receipt->correlationId = request->correlationId;
		WriteDiagnostic(receipt->diagnostic, nativeReceipt.diagnostic);
		return receipt->status;
	}

	NativePhysicsStatus Native_Physics_SubmitQueryBatch(const NativePhysicsQueryRequest* requests, std::uint32_t requestCount, NativePhysicsQueryReceipt* receipts, std::uint32_t receiptCapacity, std::uint32_t* requiredReceiptCount) {
		if(requiredReceiptCount == nullptr) return NativePhysicsStatus::InvalidArgument;
		*requiredReceiptCount = requestCount;
		if((requestCount != 0U && requests == nullptr) || (requestCount != 0U && (receipts == nullptr || receiptCapacity < requestCount))) return NativePhysicsStatus::LimitExceeded;
		NativePhysicsStatus firstFailure = NativePhysicsStatus::Accepted;
		for(std::uint32_t index = 0U; index < requestCount; ++index) {
			receipts[index].header = {kNativePhysicsAbiVersion, sizeof(NativePhysicsQueryReceipt)};
			const NativePhysicsStatus status = Native_Physics_SubmitQuery(&requests[index], &receipts[index]);
			if(status != NativePhysicsStatus::Accepted && firstFailure == NativePhysicsStatus::Accepted) firstFailure = status;
		}
		return firstFailure;
	}

	NativePhysicsStatus Native_Physics_ReadQuery(const NativePhysicsQueryHandle* handle, NativePhysicsQueryResultInfo* info, NativePhysicsQueryHit* hits, std::uint32_t hitCapacity, std::uint32_t* requiredHitCount) {
		if(handle == nullptr || !HasPhysicsAbiHeader(info) || requiredHitCount == nullptr) return NativePhysicsStatus::VersionMismatch;
		const PhysicsQueryResult result = PhysicsSystem::Instance().ReadQuery(ToQueryHandle(*handle));
		return CopyQueryResult(result, *info, hits, hitCapacity, *requiredHitCount);
	}

	NativePhysicsStatus Native_Physics_CancelQuery(const NativePhysicsQueryHandle* handle) {
		return handle != nullptr && PhysicsSystem::Instance().CancelQuery(ToQueryHandle(*handle)) ? NativePhysicsStatus::Success : NativePhysicsStatus::StaleTarget;
	}

	NativePhysicsStatus Native_Physics_CopyBodyStates(NativePhysicsBodyState* states, std::uint32_t capacity, std::uint32_t* required) {
		if(required == nullptr) return NativePhysicsStatus::InvalidArgument;
		const auto snapshot = PhysicsSystem::Instance().LatestBodySnapshot();
		const std::size_t count = snapshot && snapshot->bodies ? snapshot->bodies->size() : 0U;
		*required = static_cast<std::uint32_t>(count);
		if(count > capacity || (count != 0U && states == nullptr)) return NativePhysicsStatus::LimitExceeded;
		for(std::size_t index = 0U; index < count; ++index) {
			const PhysicsBodyState& source = snapshot->bodies->at(index);
			states[index] = {{kNativePhysicsAbiVersion, sizeof(NativePhysicsBodyState)}, FromHandle(source.handle), NativeIdentity(source.objectId, source.objectGeneration, source.componentGeneration), FromPose(source.worldPose), FromVector(source.linearVelocity), FromVector(source.angularVelocity), snapshot->tick, source.awake ? 0U : 1U, {}};
		}
		return NativePhysicsStatus::Success;
	}

	NativePhysicsStatus Native_Physics_CopyCharacterStates(NativePhysicsCharacterState* states, std::uint32_t capacity, std::uint32_t* required) {
		if(required == nullptr) return NativePhysicsStatus::InvalidArgument;
		const auto snapshot = PhysicsSystem::Instance().LatestCharacterStatePacket();
		const std::size_t count = snapshot && snapshot->states ? snapshot->states->size() : 0U;
		*required = static_cast<std::uint32_t>(count);
		if(count > capacity || (count != 0U && states == nullptr)) return NativePhysicsStatus::LimitExceeded;
		for(std::size_t index = 0U; index < count; ++index) {
			const PhysicsCharacterState& source = snapshot->states->at(index);
			states[index] = {{kNativePhysicsAbiVersion, sizeof(NativePhysicsCharacterState)}, FromHandle(source.handle), NativeIdentity(source.objectId, source.objectGeneration, source.componentGeneration), FromPose(source.pose), FromVector(source.linearVelocity), FromVector(source.groundNormal), FromVector(source.supportVelocity), FromHandle(source.supportBody), source.tick, static_cast<std::uint32_t>(source.groundState), source.contactCount, source.supported ? 1U : 0U, {}};
		}
		return NativePhysicsStatus::Success;
	}

	NativePhysicsStatus Native_Physics_SubscribeEvents(const NativePhysicsEventSubscriptionRequest* request, NativePhysicsEventSubscription* subscription) {
		if(!HasPhysicsAbiHeader(request) || !HasPhysicsAbiHeader(subscription)) return NativePhysicsStatus::VersionMismatch;
		PhysicsEventSubscriptionPolicy policy;
		policy.categories = static_cast<PhysicsEventCategory>(request->categoryMask);
		policy.collisionLayerMask = request->collisionLayerMask;
		policy.includeSensors = request->includeSensors != 0U;
		policy.includeContactDetails = request->includeContactDetails != 0U;
		const PhysicsEventSubscription nativeSubscription = PhysicsSystem::Instance().SubscribeEvents(std::move(policy));
		subscription->header = {kNativePhysicsAbiVersion, sizeof(*subscription)};
		subscription->status = nativeSubscription.id != 0U ? NativePhysicsStatus::Success : NativePhysicsStatus::InvalidState;
		subscription->generation = nativeSubscription.generation;
		subscription->id = nativeSubscription.id;
		WriteDiagnostic(subscription->diagnostic, nativeSubscription.id != 0U ? "" : "No active physics world is available.");
		return subscription->status;
	}

	NativePhysicsStatus Native_Physics_UnsubscribeEvents(const NativePhysicsEventSubscription* subscription) {
		if(!HasPhysicsAbiHeader(subscription)) return NativePhysicsStatus::VersionMismatch;
		return PhysicsSystem::Instance().UnsubscribeEvents(subscription->id, subscription->generation) ? NativePhysicsStatus::Success : NativePhysicsStatus::StaleTarget;
	}

	NativePhysicsStatus Native_Physics_CopyEvents(NativePhysicsEvent* events, std::uint32_t capacity, std::uint32_t* required, std::uint64_t* sourceTick) {
		if(required == nullptr || sourceTick == nullptr) return NativePhysicsStatus::InvalidArgument;
		const auto packet = PhysicsSystem::Instance().LatestEventPacket();
		const std::size_t count = packet && packet->events ? packet->events->size() : 0U;
		*required = static_cast<std::uint32_t>(count);
		*sourceTick = packet ? packet->tick : 0U;
		if(count > capacity || (count != 0U && events == nullptr)) return NativePhysicsStatus::LimitExceeded;
		for(std::size_t index = 0U; index < count; ++index) {
			const PhysicsEvent& source = packet->events->at(index);
			NativePhysicsEvent& destination = events[index];
			destination.header = {kNativePhysicsAbiVersion, sizeof(destination)};
			destination.type = static_cast<std::uint32_t>(source.type);
			destination.hasSecondParticipant = source.hasSecondParticipant ? 1U : 0U;
			destination.world = FromWorld(source.world);
			destination.tick = source.tick;
			destination.first = NativeIdentity(source.first.objectId, source.first.objectGeneration, source.first.componentGeneration, source.first.colliderRecordId, source.first.recordGeneration);
			destination.second = NativeIdentity(source.second.objectId, source.second.objectGeneration, source.second.componentGeneration, source.second.colliderRecordId, source.second.recordGeneration);
			destination.point = FromVector(source.point); destination.normal = FromVector(source.normal); destination.relativeVelocity = FromVector(source.relativeVelocity);
			destination.penetration = source.penetration; destination.impulse = source.impulse; destination.correlationId = source.causalCommandSequence;
		}
		return NativePhysicsStatus::Success;
	}

	NativePhysicsStatus Native_Physics_ReadSettings(NativePhysicsSettingsSnapshot* settings) {
		if(!HasPhysicsAbiHeader(settings)) return NativePhysicsStatus::VersionMismatch;
		const PhysicsSettings nativeSettings = PhysicsSystem::Instance().ConfiguredSettings();
		const PhysicsWorldHandle world = PhysicsSystem::Instance().WorldHandle();
		settings->header = {kNativePhysicsAbiVersion, sizeof(*settings)};
		settings->status = NativePhysicsStatus::Success;
		settings->settingsGeneration = static_cast<std::uint32_t>(PhysicsSystem::Instance().SettingsGeneration());
		settings->gravity = FromVector(nativeSettings.gravity);
		settings->fixedTimeStep = nativeSettings.fixedTimeStep;
		settings->maxSubsteps = nativeSettings.maxSubsteps;
		settings->maxQueryBatchSize = nativeSettings.maxRequestsPerBatch;
		settings->maxHitsPerQuery = nativeSettings.maxHitsPerQuery;
		settings->maxEventHistory = nativeSettings.retainedResultTicks;
		settings->worldId = world.worldId.value; settings->worldGeneration = world.worldGeneration; settings->reserved = 0U;
		WriteDiagnostic(settings->diagnostic, world.IsValid() ? "" : "Physics settings are configured but no world is active.");
		return settings->status;
	}

	static NativePhysicsStatus Native_Physics_ReadSettingsRecord(NativePhysicsSettingsRecord* settings) {
		if(!HasPhysicsAbiHeader(settings)) return NativePhysicsStatus::VersionMismatch;
		const PhysicsSettings source = PhysicsSystem::Instance().ConfiguredSettings();
		const PhysicsWorldHandle world = PhysicsSystem::Instance().WorldHandle();
		settings->header = {kNativePhysicsAbiVersion, sizeof(*settings)};
		settings->status = NativePhysicsStatus::Success;
		settings->settingsGeneration = static_cast<std::uint32_t>(PhysicsSystem::Instance().SettingsGeneration());
		settings->gravity = FromVector(source.gravity);
		settings->fixedTimeStep = source.fixedTimeStep;
		settings->maxSubsteps = source.maxSubsteps;
		settings->maxAccumulatedTime = source.maxAccumulatedTime;
		settings->velocityIterations = source.velocityIterations;
		settings->positionIterations = source.positionIterations;
		settings->maxCastDistance = source.maxCastDistance;
		settings->maxShapeExtent = source.maxShapeExtent;
		settings->maxHitsPerQuery = source.maxHitsPerQuery;
		settings->maxRequestsPerBatch = source.maxRequestsPerBatch;
		settings->retainedResultTicks = source.retainedResultTicks;
		settings->debugVisualizationEnabled = source.debugVisualizationEnabled ? 1U : 0U;
		settings->retainedDebugTicks = source.retainedDebugTicks;
		settings->maxDebugPrimitives = source.maxDebugPrimitives;
		settings->transformPublicationChunkSize = source.transformPublicationChunkSize;
		settings->providerExtractionBatchSize = source.providerExtractionBatchSize;
		settings->parallelPreparationThreshold = source.parallelPreparationThreshold;
		settings->parallelExtractionThreshold = source.parallelExtractionThreshold;
		settings->aggregateDiagnosticsIntervalFrames = source.aggregateDiagnosticsIntervalFrames;
		settings->continuousCollisionEnabled = source.continuousCollisionEnabled ? 1U : 0U;
		settings->worldId = world.worldId.value;
		settings->worldGeneration = world.worldGeneration;
		settings->reserved = 0U;
		for(std::size_t index = 0U; index < source.layers.size(); ++index) {
			NativePhysicsCollisionLayer& destination = settings->layers[index];
			const CollisionLayer& layer = source.layers[index];
			destination.id = layer.id;
			destination.broadPhaseGroup = layer.broadPhaseGroup;
			destination.collisionMask = layer.collisionMask;
			destination.enabled = layer.enabled ? 1U : 0U;
			std::memset(destination.name, 0, sizeof(destination.name));
			const std::size_t count = std::min(layer.name.size(), sizeof(destination.name) - 1U);
			std::memcpy(destination.name, layer.name.data(), count);
		}
		WriteDiagnostic(settings->diagnostic, world.IsValid() ? "" : "Physics settings are configured but no world is active.");
		return settings->status;
	}

	static NativePhysicsStatus Native_Physics_SubmitSettingsRecord(const NativePhysicsSettingsRecord* settings) {
		if(!HasPhysicsAbiHeader(settings)) return NativePhysicsStatus::VersionMismatch;
		PhysicsSettings candidate = PhysicsSettings::Defaults();
		candidate.gravity = ToVector(settings->gravity);
		candidate.fixedTimeStep = settings->fixedTimeStep;
		candidate.maxSubsteps = settings->maxSubsteps;
		candidate.maxAccumulatedTime = settings->maxAccumulatedTime;
		candidate.velocityIterations = settings->velocityIterations;
		candidate.positionIterations = settings->positionIterations;
		candidate.maxCastDistance = settings->maxCastDistance;
		candidate.maxShapeExtent = settings->maxShapeExtent;
		candidate.maxHitsPerQuery = settings->maxHitsPerQuery;
		candidate.maxRequestsPerBatch = settings->maxRequestsPerBatch;
		candidate.retainedResultTicks = settings->retainedResultTicks;
		candidate.debugVisualizationEnabled = settings->debugVisualizationEnabled != 0U;
		candidate.retainedDebugTicks = settings->retainedDebugTicks;
		candidate.maxDebugPrimitives = settings->maxDebugPrimitives;
		candidate.transformPublicationChunkSize = settings->transformPublicationChunkSize;
		candidate.providerExtractionBatchSize = settings->providerExtractionBatchSize;
		candidate.parallelPreparationThreshold = settings->parallelPreparationThreshold;
		candidate.parallelExtractionThreshold = settings->parallelExtractionThreshold;
		candidate.aggregateDiagnosticsIntervalFrames = settings->aggregateDiagnosticsIntervalFrames;
		candidate.continuousCollisionEnabled = settings->continuousCollisionEnabled != 0U;
		for(std::size_t index = 0U; index < candidate.layers.size(); ++index) {
			const NativePhysicsCollisionLayer& source = settings->layers[index];
			if(source.id > std::numeric_limits<std::uint8_t>::max()
				|| source.broadPhaseGroup > std::numeric_limits<std::uint8_t>::max()) {
				return NativePhysicsStatus::InvalidArgument;
			}
			CollisionLayer& layer = candidate.layers[index];
			layer.id = static_cast<std::uint8_t>(source.id);
			layer.broadPhaseGroup = static_cast<std::uint8_t>(source.broadPhaseGroup);
			layer.collisionMask = source.collisionMask;
			layer.enabled = source.enabled != 0U;
			const char* nameEnd = std::find(std::begin(source.name), std::end(source.name), '\0');
			layer.name.assign(source.name, nameEnd);
		}
		if(!candidate.Validate().accepted) return NativePhysicsStatus::InvalidArgument;
		return PhysicsSystem::Instance().QueueSettingsUpdate(std::move(candidate))
			? NativePhysicsStatus::Accepted
			: NativePhysicsStatus::Conflict;
	}

	NativePhysicsStatus Native_Physics_ResolveIdentity(std::uint32_t objectEntity, NativePhysicsIdentitySnapshot* identity) {
		if(!HasPhysicsAbiHeader(identity)) return NativePhysicsStatus::VersionMismatch;
		identity->header = {kNativePhysicsAbiVersion, sizeof(*identity)};
		identity->status = NativePhysicsStatus::StaleTarget;
		identity->reserved = 0U;
		identity->identity = {};
		const std::optional<Object> object = ObjectManager::instance().getObject(static_cast<entt::entity>(objectEntity));
		if(!object) {
			WriteDiagnostic(identity->diagnostic, "The object no longer exists.");
			return identity->status;
		}
		ObjectManager& objects = ObjectManager::instance();
		const TerraEngine::UUID objectId = objects.getUUID(*object);
		identity->identity = NativeIdentity(
			objectId,
			TerraEngine::CompressObjectGeneration(objects.getObjectLifetimeGeneration(*object)),
			TerraEngine::CompressObjectGeneration(objects.getObjectComponentGeneration(*object)));
		identity->status = NativePhysicsStatus::Success;
		WriteDiagnostic(identity->diagnostic, "");
		return identity->status;
	}

	NativePhysicsStatus Native_Physics_ResolveBody(std::uint32_t objectEntity, NativePhysicsBodyState* state) {
		if(!HasPhysicsAbiHeader(state)) return NativePhysicsStatus::VersionMismatch;
		const std::optional<Object> object = ObjectManager::instance().getObject(static_cast<entt::entity>(objectEntity));
		const auto snapshot = PhysicsSystem::Instance().LatestBodySnapshot();
		if(!object || !snapshot || !snapshot->bodies) return NativePhysicsStatus::StaleTarget;
		const TerraEngine::UUID objectId = ObjectManager::instance().getUUID(*object);
		const auto found = std::find_if(snapshot->bodies->begin(), snapshot->bodies->end(), [&objectId](const PhysicsBodyState& candidate) {
			return candidate.objectId == objectId;
		});
		if(found == snapshot->bodies->end()) return NativePhysicsStatus::StaleTarget;
		*state = {{kNativePhysicsAbiVersion, sizeof(*state)}, FromHandle(found->handle), NativeIdentity(found->objectId, found->objectGeneration, found->componentGeneration), FromPose(found->worldPose), FromVector(found->linearVelocity), FromVector(found->angularVelocity), snapshot->tick, found->awake ? 0U : 1U, {}};
		return NativePhysicsStatus::Success;
	}

	NativePhysicsStatus Native_Physics_ResolveCharacter(std::uint32_t objectEntity, NativePhysicsCharacterState* state) {
		if(!HasPhysicsAbiHeader(state)) return NativePhysicsStatus::VersionMismatch;
		const std::optional<Object> object = ObjectManager::instance().getObject(static_cast<entt::entity>(objectEntity));
		const auto snapshot = PhysicsSystem::Instance().LatestCharacterStatePacket();
		if(!object || !snapshot || !snapshot->states) return NativePhysicsStatus::StaleTarget;
		const TerraEngine::UUID objectId = ObjectManager::instance().getUUID(*object);
		const auto found = std::find_if(snapshot->states->begin(), snapshot->states->end(), [&objectId](const PhysicsCharacterState& candidate) {
			return candidate.objectId == objectId;
		});
		if(found == snapshot->states->end()) return NativePhysicsStatus::StaleTarget;
		*state = {{kNativePhysicsAbiVersion, sizeof(*state)}, FromHandle(found->handle), NativeIdentity(found->objectId, found->objectGeneration, found->componentGeneration), FromPose(found->pose), FromVector(found->linearVelocity), FromVector(found->groundNormal), FromVector(found->supportVelocity), FromHandle(found->supportBody), found->tick, static_cast<std::uint32_t>(found->groundState), found->contactCount, found->supported ? 1U : 0U, {}};
		return NativePhysicsStatus::Success;
	}

	NativePhysicsStatus Native_Physics_CopyColliderRecords(std::uint32_t objectEntity, NativePhysicsColliderRecord* records, std::uint32_t capacity, std::uint32_t* required) {
		if(required == nullptr) return NativePhysicsStatus::InvalidArgument;
		*required = 0U;
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectEntity));
		if(!object) return NativePhysicsStatus::StaleTarget;
		const ColliderSetComponent* component = objects.GetComponent<ColliderSetComponent>(*object);
		if(component == nullptr) return NativePhysicsStatus::InvalidState;
		if(component->colliders.size() > kMaximumManagedAuthoringRecords) return NativePhysicsStatus::LimitExceeded;
		*required = static_cast<std::uint32_t>(component->colliders.size());
		if(*required > capacity || (*required != 0U && records == nullptr)) return NativePhysicsStatus::LimitExceeded;
		for(std::uint32_t index = 0U; index < *required; ++index) records[index] = FromColliderRecord(component->colliders[index]);
		return NativePhysicsStatus::Success;
	}

	NativePhysicsStatus Native_Physics_ReplaceColliderRecords(std::uint32_t objectEntity, const NativePhysicsColliderRecord* records, std::uint32_t recordCount) {
		const NativePhysicsStatus inputStatus = ValidateManagedRecordInput(records, recordCount);
		if(inputStatus != NativePhysicsStatus::Success) return inputStatus;
		std::vector<ColliderRecord> replacements;
		replacements.reserve(recordCount);
		for(std::uint32_t index = 0U; index < recordCount; ++index) {
			std::optional<ColliderRecord> converted = ToColliderRecord(records[index]);
			if(!converted) return NativePhysicsStatus::InvalidArgument;
			replacements.push_back(std::move(*converted));
		}
		ColliderSetComponent candidate;
		candidate.colliders = replacements;
		if(!candidate.Validate().accepted) return NativePhysicsStatus::InvalidArgument;

		if(auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectEntity));
			if(!object) return NativePhysicsStatus::StaleTarget;
			if(objects.GetComponent<ColliderSetComponent>(*object) == nullptr) return NativePhysicsStatus::InvalidState;
			const TerraEngine::UUID objectId = objects.getUUID(*object);
			TerraEngine::Scene::TransactionalMutationBuilder builder(
				"managed-collider-set-" + std::to_string(context->frameOrder) + "-" + TerraEngine::ToHexString(objectId),
				TerraEngine::Scene::TransactionOrigin::ManagedCallback,
				context->sourceBindingId);
			builder.ObserveObject(objects, objectId)
				.UpdateComponentProperty(objectId, std::string(ColliderSetComponent::kComponentName), "colliders", std::move(replacements));
			TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(std::move(builder).Build());
			return NativePhysicsStatus::Accepted;
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectEntity));
		if(!object) return NativePhysicsStatus::StaleTarget;
		ColliderSetComponent* component = objects.GetComponent<ColliderSetComponent>(*object);
		if(component == nullptr) return NativePhysicsStatus::InvalidState;
		component->colliders = std::move(replacements);
		objects.notifyObjectStructureChanged(*object);
		return NativePhysicsStatus::Success;
	}

	NativePhysicsStatus Native_Physics_CopyConstraintRecords(std::uint32_t objectEntity, NativePhysicsConstraintRecord* records, std::uint32_t capacity, std::uint32_t* required) {
		if(required == nullptr) return NativePhysicsStatus::InvalidArgument;
		*required = 0U;
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectEntity));
		if(!object) return NativePhysicsStatus::StaleTarget;
		const ConstraintSetComponent* component = objects.GetComponent<ConstraintSetComponent>(*object);
		if(component == nullptr) return NativePhysicsStatus::InvalidState;
		if(component->constraints.size() > kMaximumManagedAuthoringRecords) return NativePhysicsStatus::LimitExceeded;
		*required = static_cast<std::uint32_t>(component->constraints.size());
		if(*required > capacity || (*required != 0U && records == nullptr)) return NativePhysicsStatus::LimitExceeded;
		for(std::uint32_t index = 0U; index < *required; ++index) records[index] = FromConstraintRecord(component->constraints[index]);
		return NativePhysicsStatus::Success;
	}

	NativePhysicsStatus Native_Physics_ReplaceConstraintRecords(std::uint32_t objectEntity, const NativePhysicsConstraintRecord* records, std::uint32_t recordCount) {
		const NativePhysicsStatus inputStatus = ValidateManagedRecordInput(records, recordCount);
		if(inputStatus != NativePhysicsStatus::Success) return inputStatus;
		std::vector<ConstraintRecord> replacements;
		replacements.reserve(recordCount);
		for(std::uint32_t index = 0U; index < recordCount; ++index) {
			std::optional<ConstraintRecord> converted = ToConstraintRecord(records[index]);
			if(!converted) return NativePhysicsStatus::InvalidArgument;
			replacements.push_back(std::move(*converted));
		}
		ConstraintSetComponent candidate;
		candidate.constraints = replacements;
		if(!candidate.Validate().accepted) return NativePhysicsStatus::InvalidArgument;

		if(auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectEntity));
			if(!object) return NativePhysicsStatus::StaleTarget;
			if(objects.GetComponent<ConstraintSetComponent>(*object) == nullptr) return NativePhysicsStatus::InvalidState;
			const TerraEngine::UUID objectId = objects.getUUID(*object);
			TerraEngine::Scene::TransactionalMutationBuilder builder(
				"managed-constraint-set-" + std::to_string(context->frameOrder) + "-" + TerraEngine::ToHexString(objectId),
				TerraEngine::Scene::TransactionOrigin::ManagedCallback,
				context->sourceBindingId);
			builder.ObserveObject(objects, objectId)
				.UpdateComponentProperty(objectId, std::string(ConstraintSetComponent::kComponentName), "constraints", std::move(replacements));
			TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(std::move(builder).Build());
			return NativePhysicsStatus::Accepted;
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectEntity));
		if(!object) return NativePhysicsStatus::StaleTarget;
		ConstraintSetComponent* component = objects.GetComponent<ConstraintSetComponent>(*object);
		if(component == nullptr) return NativePhysicsStatus::InvalidState;
		component->constraints = std::move(replacements);
		objects.notifyObjectStructureChanged(*object);
		return NativePhysicsStatus::Success;
	}
}

void FlushManagedRuntimeRequests()
{
	std::optional<SceneRenderSettings> sceneSettings;
	std::optional<TerraEngine::UUID> activeScene;
	std::vector<ManagedSceneLoadRequest> sceneLoads;
	std::vector<ManagedPrefabInstantiationRequest> prefabRequests;
	std::vector<ManagedResourceReleaseRequest> resourceReleases;
	bool unloadAll = false;
	std::uint64_t frameId = 0U;
	{
		std::scoped_lock lock(g_ManagedRuntimeRequestMutex);
		sceneSettings = std::move(g_PendingManagedSceneRenderSettings);
		g_PendingManagedSceneRenderSettings.reset();
		activeScene = std::move(g_PendingManagedActiveScene);
		g_PendingManagedActiveScene.reset();
		sceneLoads.swap(g_PendingManagedSceneLoads);
		prefabRequests.swap(g_PendingManagedPrefabInstantiations);
		resourceReleases.swap(g_PendingManagedResourceReleases);
		unloadAll = std::exchange(g_PendingManagedSceneUnloadAll, false);
		frameId = ++g_ManagedRuntimeFrameId;
	}

	if (unloadAll)
	{
		ObjectManager::instance().clear();
		SceneManager::Instance().UnloadAll();
		// Unload-all is an atomic lifecycle barrier for this frame. Requests that
		// were captured beside it cannot reactivate stale scenes or recreate prefab
		// objects after the world has been cleared.
		activeScene.reset();
		sceneLoads.clear();
		prefabRequests.clear();
	}
	for (const ManagedSceneLoadRequest& request : sceneLoads)
	{
		// File resolution, parsing, and cache publication all stay inside this
		// protected boundary. Managed callbacks only contribute copied paths.
		if (request.prefab)
		{
			SceneManager::Instance().LoadPrefab(request.path);
		}
		else
		{
			SceneManager::Instance().LoadScene(request.path);
		}
	}
	if (activeScene.has_value())
	{
		SceneManager::Instance().SetActiveScene(*activeScene);
	}
	if (sceneSettings.has_value())
	{
		SceneManager::Instance().SetActiveSceneRenderSettings(*sceneSettings);
		std::scoped_lock lock(g_ManagedRuntimeRequestMutex);
		++g_ManagedSceneRenderSettingsGeneration;
	}

	for (const ManagedPrefabInstantiationRequest& request : prefabRequests)
	{
		const TerraEngine::UUID prefabId = SceneManager::Instance().LoadPrefab(request.path);
		const SceneDocument* prefab = SceneManager::Instance().GetPrefab(prefabId);
		if (prefab == nullptr || !prefab->parsedJson.contains("objects") || !prefab->parsedJson["objects"].is_array())
		{
			TERRAENGINE_LOG(LogLevel::Error, "Managed prefab instantiation rejected invalid prefab: " << request.path);
			continue;
		}

		std::optional<entt::entity> parentEntity;
		if (request.parentObjectId != TerraEngine::EMPTY_UUID)
		{
			const std::optional<Object> parent = ObjectManager::instance().getObject(request.parentObjectId);
			if (!parent)
			{
				TERRAENGINE_LOG(LogLevel::Warning, "Managed prefab parent became stale before publication.");
				continue;
			}
			parentEntity = parent->EntityId();
		}

		std::vector<Object> createdRoots;
		bool published = true;
		try
		{
			for (nlohmann::json root : prefab->parsedJson["objects"])
			{
				std::unordered_map<std::string, std::string> remap;
				CollectPrefabUuidRemap(root, remap);
				RewritePrefabUuidReferences(root, remap);
				const entt::entity entity = Serializer::DeserializeObjectEntity(root, parentEntity);
				const std::optional<Object> created = ObjectManager::instance().getObject(entity);
				if (!created)
				{
					published = false;
					break;
				}
				createdRoots.push_back(*created);
			}
		}
		catch (const std::exception& exception)
		{
			published = false;
			TERRAENGINE_LOG(LogLevel::Error, "Managed prefab instantiation failed: " << exception.what());
		}
		if (!published)
		{
			for (auto it = createdRoots.rbegin(); it != createdRoots.rend(); ++it)
			{
				if (ObjectManager::instance().getObject(it->EntityId()))
				{
					ObjectManager::instance().removeObject(*it);
				}
			}
		}
		else if (!createdRoots.empty())
		{
			SceneManager::Instance().ForceMarkActiveSceneDirty();
		}
	}

	for (const ManagedResourceReleaseRequest& request : resourceReleases)
	{
		ResourceManager::Instance().ReleaseStreamableAssetForRuntime(request.assetId, request.assetType, frameId);
	}
}

// ManagedUIStyleProperty names the supported states explicitly so serialization and switches stay readable.
enum class ManagedUIStyleProperty : int
{
	Width = 0,
	Height = 1,
	Color = 2,
	BackgroundColor = 3
};

// struct ManagedUIElementHandle groups related state and operations so callers have one documented contract.
struct ManagedUIElementHandle
{
	std::weak_ptr<UIElement> weakElement;
	std::shared_ptr<UIElement> ownedElement;
};

std::mutex g_UIHandleMutex;
std::unordered_map<UIElement*, std::unordered_set<ManagedUIElementHandle*>> g_UIHandleIndex;
std::once_flag g_UIEventDispatchResolveOnce;
bool g_UIEventDispatchResolveAttempted = false;
bool g_UIEventDispatchUnavailableLogged = false;

// ManagedUIEventType names the supported states explicitly so serialization and switches stay readable.
enum class ManagedUIEventType : int
{
	Click = 0,
	MouseEnter = 1,
	MouseLeave = 2
};

using ManagedUIDispatchEventFn = void (*)(void*, int, float, float);
ManagedUIDispatchEventFn g_ManagedUIDispatchEvent = nullptr;

// ScriptType names the supported states explicitly so serialization and switches stay readable.
enum class ScriptType : int
{
	Bool = 0,
	Int = 1,
	Float = 2,
	EntityID = 3,
	Vector2 = 4,
	Vector3 = 5,
	Vector4 = 6,
	Quaternion = 7
};

// MarshallFromNative is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static TerraEngine::Reflection::Variant MarshallFromNative(void* data, ScriptType type)
{
	switch (type)
	{
	case ScriptType::Bool: return *static_cast<bool*>(data);
	case ScriptType::Int: return *static_cast<int*>(data);
	case ScriptType::Float: return *static_cast<float*>(data);
	case ScriptType::EntityID: return *static_cast<uint32_t*>(data);
	case ScriptType::Vector2: return *static_cast<glm::vec2*>(data);
	case ScriptType::Vector3: return *static_cast<glm::vec3*>(data);
	case ScriptType::Vector4: return *static_cast<glm::vec4*>(data);
	case ScriptType::Quaternion: return *static_cast<glm::quat*>(data);
	}
	return {};
}

// MarshallToNative is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void MarshallToNative(const TerraEngine::Reflection::Variant& var, void* data, ScriptType type)
{
	if (!TerraEngine::Reflection::IsValid(var)) return;

	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (type)
	{
	case ScriptType::Bool: *static_cast<bool*>(data) = TerraEngine::Reflection::ConvertValue<bool>(var); break;
	case ScriptType::Int: *static_cast<int*>(data) = TerraEngine::Reflection::ConvertValue<int>(var); break;
	case ScriptType::Float: *static_cast<float*>(data) = TerraEngine::Reflection::ConvertValue<float>(var); break;
	case ScriptType::EntityID: *static_cast<uint32_t*>(data) = TerraEngine::Reflection::ConvertValue<uint32_t>(var); break;
	case ScriptType::Vector2: *static_cast<glm::vec2*>(data) = TerraEngine::Reflection::ConvertValue<glm::vec2>(var); break;
	case ScriptType::Vector3: *static_cast<glm::vec3*>(data) = TerraEngine::Reflection::ConvertValue<glm::vec3>(var); break;
	case ScriptType::Vector4: *static_cast<glm::vec4*>(data) = TerraEngine::Reflection::ConvertValue<glm::vec4>(var); break;
	case ScriptType::Quaternion: *static_cast<glm::quat*>(data) = TerraEngine::Reflection::ConvertValue<glm::quat>(var); break;
	}
}

static nlohmann::json MarshallNativeValueToJson(void* data, ScriptType type)
{
	switch (type)
	{
	case ScriptType::Bool: return *static_cast<bool*>(data);
	case ScriptType::Int: return *static_cast<int*>(data);
	case ScriptType::Float: return *static_cast<float*>(data);
	case ScriptType::EntityID: return *static_cast<uint32_t*>(data);
	case ScriptType::Vector2:
	{
		const glm::vec2 value = *static_cast<glm::vec2*>(data);
		return nlohmann::json::array({ value.x, value.y });
	}
	case ScriptType::Vector3:
	{
		const glm::vec3 value = *static_cast<glm::vec3*>(data);
		return nlohmann::json::array({ value.x, value.y, value.z });
	}
	case ScriptType::Vector4:
	{
		const glm::vec4 value = *static_cast<glm::vec4*>(data);
		return nlohmann::json::array({ value.x, value.y, value.z, value.w });
	}
	case ScriptType::Quaternion:
	{
		const glm::quat value = *static_cast<glm::quat*>(data);
		return nlohmann::json::array({ value.w, value.x, value.y, value.z });
	}
	}
	return {};
}

static bool HasValidGraphicsHeader(const NativeGraphicsAbiHeader& header)
{
	return header.abiVersion == kNativeGraphicsAbiVersion
		&& header.structSize == sizeof(NativeGraphicsRenderSettings);
}

static NativeGraphicsRenderSettings ToNativeGraphicsSettings(const RenderSettings& settings)
{
	NativeGraphicsRenderSettings result{};
	result.header = { kNativeGraphicsAbiVersion, sizeof(NativeGraphicsRenderSettings) };
	result.shadowResolution = settings.shadowResolution;
	result.shadowFilteringSize = settings.shadowFilteringSize;
	result.maxUploadedLights = settings.maxUploadedLights;
	result.maxDynamicLights = settings.maxDynamicLights;
	result.maxShadowLights = settings.maxShadowLights;
	result.maxAreaLights = settings.maxAreaLights;
	result.enableLTC = settings.enableLTC;
	result.enableIBL = settings.enableIBL;
	result.indirectStrength = settings.indirectStrength;
	result.ambientOcclusionStrength = settings.ambientOcclusionStrength;
	result.ambientOcclusionQuality = settings.ambientOcclusionQuality;
	result.anisotropicFiltering = settings.anisotropicFiltering;
	result.variableRateShading = settings.variableRateShading;
	result.validationEnabled = settings.validationEnabled;
	result.debugEnabled = settings.debugEnabled;
	result.virtualGeometryLodErrorScale = settings.virtualGeometry.lodErrorScale;
	result.virtualGeometryMaxLodDrop = settings.virtualGeometry.maxLodDrop;
	result.virtualGeometryHiZOcclusionEnabled = settings.virtualGeometry.hiZOcclusionEnabled ? 1U : 0U;
	result.virtualGeometryHiZSelfOcclusionGuardEnabled = settings.virtualGeometry.hiZSelfOcclusionGuardEnabled ? 1U : 0U;
	result.virtualGeometryHiZNearPlaneGuardEnabled = settings.virtualGeometry.hiZNearPlaneGuardEnabled ? 1U : 0U;
	result.virtualGeometryHiZThinGeometryGuardEnabled = settings.virtualGeometry.hiZThinGeometryGuardEnabled ? 1U : 0U;
	result.virtualGeometryDebugVisualizationMode = static_cast<std::uint32_t>(settings.virtualGeometry.debugVisualizationMode);
	result.virtualGeometryBudgetBytes = settings.virtualGeometry.geometryBudgetBytes;
	result.virtualGeometryPreloadBias = settings.virtualGeometry.preloadBias;
	result.virtualGeometryLodTransitionHysteresisPixels = settings.virtualGeometry.lodTransitionHysteresisPixels;
	result.virtualGeometryLodTransitionStableFrames = settings.virtualGeometry.lodTransitionStableFrames;
	return result;
}

static RenderSettings FromNativeGraphicsSettings(const NativeGraphicsRenderSettings& settings)
{
	RenderSettings result;
	result.shadowResolution = settings.shadowResolution;
	result.shadowFilteringSize = settings.shadowFilteringSize;
	result.maxUploadedLights = settings.maxUploadedLights;
	result.maxDynamicLights = settings.maxDynamicLights;
	result.maxShadowLights = settings.maxShadowLights;
	result.maxAreaLights = settings.maxAreaLights;
	result.enableLTC = settings.enableLTC;
	result.enableIBL = settings.enableIBL;
	result.indirectStrength = settings.indirectStrength;
	result.ambientOcclusionStrength = settings.ambientOcclusionStrength;
	result.ambientOcclusionQuality = settings.ambientOcclusionQuality;
	result.anisotropicFiltering = settings.anisotropicFiltering;
	result.variableRateShading = settings.variableRateShading;
	result.validationEnabled = settings.validationEnabled;
	result.debugEnabled = settings.debugEnabled;
	result.virtualGeometry.lodErrorScale = settings.virtualGeometryLodErrorScale;
	result.virtualGeometry.maxLodDrop = settings.virtualGeometryMaxLodDrop;
	result.virtualGeometry.hiZOcclusionEnabled = settings.virtualGeometryHiZOcclusionEnabled != 0U;
	result.virtualGeometry.hiZSelfOcclusionGuardEnabled = settings.virtualGeometryHiZSelfOcclusionGuardEnabled != 0U;
	result.virtualGeometry.hiZNearPlaneGuardEnabled = settings.virtualGeometryHiZNearPlaneGuardEnabled != 0U;
	result.virtualGeometry.hiZThinGeometryGuardEnabled = settings.virtualGeometryHiZThinGeometryGuardEnabled != 0U;
	result.virtualGeometry.debugVisualizationMode = static_cast<VirtualGeometryDebugVisualizationMode>(settings.virtualGeometryDebugVisualizationMode);
	result.virtualGeometry.geometryBudgetBytes = settings.virtualGeometryBudgetBytes;
	result.virtualGeometry.preloadBias = settings.virtualGeometryPreloadBias;
	result.virtualGeometry.lodTransitionHysteresisPixels = settings.virtualGeometryLodTransitionHysteresisPixels;
	result.virtualGeometry.lodTransitionStableFrames = settings.virtualGeometryLodTransitionStableFrames;
	result.virtualGeometry = SanitizeVirtualGeometryQualityProfile(result.virtualGeometry);
	return result;
}

static TerraEngine::Reflection::Variant MakeManagedTransactionVariant(const nlohmann::json& value, int valueKind)
{
	const auto readFloat = [&value](std::size_t index, float fallback) {
		return value.is_array() && index < value.size()
			? value.at(index).get<float>()
			: fallback;
	};

	switch (static_cast<ScriptType>(valueKind))
	{
	case ScriptType::Bool:
		return value.get<bool>();
	case ScriptType::Int:
		return value.get<int>();
	case ScriptType::Float:
		return value.get<float>();
	case ScriptType::EntityID:
		return value.get<std::uint32_t>();
	case ScriptType::Vector2:
		return glm::vec2(readFloat(0, 0.0f), readFloat(1, 0.0f));
	case ScriptType::Vector3:
		return glm::vec3(readFloat(0, 0.0f), readFloat(1, 0.0f), readFloat(2, 0.0f));
	case ScriptType::Vector4:
		return glm::vec4(readFloat(0, 0.0f), readFloat(1, 0.0f), readFloat(2, 0.0f), readFloat(3, 0.0f));
	case ScriptType::Quaternion:
		return glm::quat(readFloat(0, 1.0f), readFloat(1, 0.0f), readFloat(2, 0.0f), readFloat(3, 0.0f));
	default:
		return {};
	}
}

static std::optional<TerraEngine::UUID> ResolveManagedTransactionObjectId(const nlohmann::json& value)
{
	if (value.is_string())
	{
		return TerraEngine::ParseUUID(value.get<std::string>());
	}
	if (value.is_number_unsigned())
	{
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(value.get<std::uint32_t>());
		if (std::optional<Object> object = objects.getObject(entity))
		{
			return objects.getUUID(*object);
		}
		return std::nullopt;
	}
	if (!value.is_object())
	{
		return std::nullopt;
	}
	if (const auto uuidIt = value.find("uuid"); uuidIt != value.end() && uuidIt->is_string())
	{
		return TerraEngine::ParseUUID(uuidIt->get<std::string>());
	}
	if (const auto uuidIt = value.find("objectUuid"); uuidIt != value.end() && uuidIt->is_string())
	{
		return TerraEngine::ParseUUID(uuidIt->get<std::string>());
	}
	if (const auto entityIt = value.find("entity"); entityIt != value.end() && entityIt->is_number_unsigned())
	{
		return ResolveManagedTransactionObjectId(*entityIt);
	}
	if (const auto entityIt = value.find("objectId"); entityIt != value.end() && entityIt->is_number_unsigned())
	{
		return ResolveManagedTransactionObjectId(*entityIt);
	}
	return std::nullopt;
}

static TerraEngine::Scene::TransactionObjectRef ParseManagedTransactionObjectRef(const nlohmann::json& value)
{
	if (value.is_null())
	{
		return {};
	}
	if (value.is_object())
	{
		if (const auto localIt = value.find("localObjectId"); localIt != value.end() && localIt->is_number_integer())
		{
			return TerraEngine::Scene::LocalObject(localIt->get<std::int32_t>());
		}
	}
	if (const std::optional<TerraEngine::UUID> objectId = ResolveManagedTransactionObjectId(value))
	{
		return TerraEngine::Scene::ExistingObject(*objectId);
	}
	return {};
}

static TerraEngine::Scene::Transaction ParseManagedSceneTransaction(const nlohmann::json& payload)
{
	if (!payload.is_object())
	{
		throw std::invalid_argument("Transaction payload must be a JSON object.");
	}

	const std::string transactionId = payload.value("id", std::string("managed-transaction"));
	TerraEngine::Scene::TransactionalMutationBuilder builder(
		transactionId,
		TerraEngine::Scene::TransactionOrigin::ManagedCallback,
		payload.value("originName", std::string("managed-callback")));
	if (payload.value("preflightOnly", false))
	{
		builder.SetMode(TerraEngine::Scene::TransactionMode::PreflightOnly);
	}
	if (const std::string undoGroup = payload.value("undoGroup", std::string{}); !undoGroup.empty())
	{
		builder.SetUndoGroup(undoGroup);
	}

	const auto writesIt = payload.find("writes");
	if (writesIt == payload.end() || !writesIt->is_array())
	{
		throw std::invalid_argument("Transaction payload needs a writes array.");
	}

	for (const nlohmann::json& write : *writesIt)
	{
		const std::string op = write.value("op", write.value("kind", std::string{}));
		if (op == "create" || op == "createObject")
		{
			builder.CreateObject(
				write.value("localObjectId", 1),
				write.value("name", write.value("displayName", std::string("Managed Object"))),
				ParseManagedTransactionObjectRef(write.value("parent", nlohmann::json{})));
		}
		else if (op == "delete" || op == "deleteObject")
		{
			if (const std::optional<TerraEngine::UUID> target = ResolveManagedTransactionObjectId(write.value("object", nlohmann::json{})))
			{
				builder.DeleteObject(*target);
			}
		}
		else if (op == "reparent" || op == "reparentObject")
		{
			if (const std::optional<TerraEngine::UUID> target = ResolveManagedTransactionObjectId(write.value("object", nlohmann::json{})))
			{
				builder.ReparentObject(*target, ParseManagedTransactionObjectRef(write.value("parent", nlohmann::json{})));
			}
		}
		else if (op == "setName")
		{
			if (const std::optional<TerraEngine::UUID> target = ResolveManagedTransactionObjectId(write.value("object", nlohmann::json{})))
			{
				builder.SetName(*target, write.value("name", std::string{}));
			}
		}
		else if (op == "setTag")
		{
			if (const std::optional<TerraEngine::UUID> target = ResolveManagedTransactionObjectId(write.value("object", nlohmann::json{})))
			{
				builder.SetTag(*target, write.value("tag", std::string{}));
			}
		}
		else if (op == "setLayer")
		{
			if (const std::optional<TerraEngine::UUID> target = ResolveManagedTransactionObjectId(write.value("object", nlohmann::json{})))
			{
				builder.SetLayer(*target, write.value("layer", std::string{}));
			}
		}
		else if (op == "setActive")
		{
			if (const std::optional<TerraEngine::UUID> target = ResolveManagedTransactionObjectId(write.value("object", nlohmann::json{})))
			{
				builder.SetActive(*target, write.value("active", true));
			}
		}
		else if (op == "setStatic")
		{
			if (const std::optional<TerraEngine::UUID> target = ResolveManagedTransactionObjectId(write.value("object", nlohmann::json{})))
			{
				builder.SetStatic(*target, write.value("static", write.value("isStatic", false)));
			}
		}
		else if (op == "addComponent")
		{
			if (const std::optional<TerraEngine::UUID> target = ResolveManagedTransactionObjectId(write.value("object", nlohmann::json{})))
			{
				builder.AddComponent(*target, write.value("component", write.value("componentName", std::string{})));
			}
		}
		else if (op == "removeComponent")
		{
			if (const std::optional<TerraEngine::UUID> target = ResolveManagedTransactionObjectId(write.value("object", nlohmann::json{})))
			{
				builder.RemoveComponent(*target, write.value("component", write.value("componentName", std::string{})));
			}
		}
		else if (op == "updateComponentProperty")
		{
			if (const std::optional<TerraEngine::UUID> target = ResolveManagedTransactionObjectId(write.value("object", nlohmann::json{})))
			{
				const int valueKind = write.value("valueKind", -1);
				builder.UpdateComponentProperty(
					*target,
					write.value("component", write.value("componentName", std::string{})),
					write.value("property", write.value("propertyName", std::string{})),
					MakeManagedTransactionVariant(write.value("value", nlohmann::json{}), valueKind));
			}
		}
		else
		{
			throw std::invalid_argument("Unsupported managed transaction op: " + op);
		}
	}
	return builder.Build();
}

// ResolveBinding is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static ScriptBinding* ResolveBinding(entt::entity object, const char* bindingId)
{
	if (bindingId == nullptr || bindingId[0] == '\0')
	{
		return nullptr;
	}

	std::optional<Object> objectHandle = ObjectManager::instance().getObject(object);
	ScriptComponent* scriptComponent = objectHandle ? ObjectManager::instance().GetComponent<ScriptComponent>(*objectHandle) : nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (scriptComponent == nullptr)
	{
		return nullptr;
	}

	return scriptComponent->FindBinding(bindingId);
}

// InvalidObjectId is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static uint32_t InvalidObjectId()
{
	return static_cast<uint32_t>(entt::null);
}

// WriteApplicationReturnText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static const char* WriteApplicationReturnText(std::string text)
{
	g_ApplicationReturnBuffer = std::move(text);
	return g_ApplicationReturnBuffer.c_str();
}

// WriteUIBridgeReturnText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static const char* WriteUIBridgeReturnText(std::string text)
{
	g_UIBridgeReturnBuffer = std::move(text);
	return g_UIBridgeReturnBuffer.c_str();
}

static const char* WriteSceneBridgeReturnText(std::string text)
{
	g_SceneBridgeReturnBuffer = std::move(text);
	return g_SceneBridgeReturnBuffer.c_str();
}

// GetApplicationHostEngine is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static Engine* GetApplicationHostEngine()
{
	std::lock_guard<std::mutex> lock(g_ApplicationBridgeMutex);
	return g_ApplicationHostEngine;
}

// GetApplicationEditorStopCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static ApplicationEditorStopPlayModeCallback GetApplicationEditorStopCallback()
{
	std::lock_guard<std::mutex> lock(g_ApplicationBridgeMutex);
	return g_ApplicationEditorStopCallback;
}

// GetMeshRendererAssetReference is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static const char* GetMeshRendererAssetReference(uint32_t objectId, std::string_view propertyName, AssetType type)
{
	static thread_local std::string assetReferenceBuffer;
	assetReferenceBuffer.clear();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	ObjectManager& objects = ObjectManager::instance();
	const entt::entity entity = static_cast<entt::entity>(objectId);
	if (std::optional<Object> object = objects.getObject(entity))
	{
		entt::registry& enttRegistry = objects.getRegistry();
		auto& registry = ComponentAccessRegistry();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto it = registry.find("MeshRenderComponent"); it != registry.end())
		{
			TerraEngine::Reflection::Variant value = it->second.entityGetter(enttRegistry, entity, propertyName);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(value))
			{
				assetReferenceBuffer = ResourceManager::Instance().DescribeAssetReference(
					TerraEngine::Reflection::GetValue<TerraEngine::UUID>(value),
					type).generic_string();
			}
		}
	}

	return assetReferenceBuffer.c_str();
}

// SetMeshRendererAssetReference is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void SetMeshRendererAssetReference(uint32_t objectId, std::string_view propertyName, AssetType type, const char* assetReference)
{
	ObjectManager& objects = ObjectManager::instance();
	const entt::entity entity = static_cast<entt::entity>(objectId);
	if (std::optional<Object> object = objects.getObject(entity))
	{
		entt::registry& enttRegistry = objects.getRegistry();
		auto& registry = ComponentAccessRegistry();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto it = registry.find("MeshRenderComponent"); it != registry.end())
		{
			TerraEngine::UUID resolvedId = TerraEngine::EMPTY_UUID;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (assetReference != nullptr && assetReference[0] != '\0')
			{
				resolvedId = ResourceManager::Instance().ResolveAssetReference(assetReference, type);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (resolvedId == TerraEngine::EMPTY_UUID)
				{
					TERRAENGINE_LOG(LogLevel::Warning,
						"[Scripting] Could not resolve asset reference '" << assetReference
						<< "' for MeshRenderComponent property '" << propertyName << "'.");
					return;
				}
			}

			// Gameplay callbacks publish the resolved UUID through
			// the same typed deferred component path as ordinary reflected setters so
			// mesh streaming and render snapshots never observe a partial live write.
			if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value())
			{
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				TerraEngine::Scene::SceneMutationRequest request = scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::UpdateComponent,
					*context,
					objects.getUUID(*object),
					{},
					"MeshRenderComponent");
				request.typedComponentPayload.propertyName = std::string(propertyName);
				request.typedComponentPayload.value = resolvedId;
				request.typedComponentPayload.valid = true;
				request.coalescingKey = std::to_string(context->frameOrder)
					+ "|" + TerraEngine::ToHexString(objects.getUUID(*object))
					+ "|MeshRenderComponent|" + std::string(propertyName);
				scriptingSystem.RecordScriptMutationRequest(std::move(request));
				return;
			}

			it->second.entitySetter(enttRegistry, entity, propertyName, resolvedId);
			objects.notifyObjectStructureChanged(*object);
		}
	}
}

// ForgetUIHandleIndexLocked is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void ForgetUIHandleIndexLocked(ManagedUIElementHandle* handle)
{
	for (auto iterator = g_UIHandleIndex.begin(); iterator != g_UIHandleIndex.end();)
	{
		iterator->second.erase(handle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (iterator->second.empty())
		{
			iterator = g_UIHandleIndex.erase(iterator);
		}
		else
		{
			++iterator;
		}
	}
}

// BindUIHandleToElement is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void BindUIHandleToElement(ManagedUIElementHandle* handle, const std::shared_ptr<UIElement>& element, bool keepDetachedOwnership)
{
	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	ForgetUIHandleIndexLocked(handle);
	handle->weakElement = element;
	handle->ownedElement = keepDetachedOwnership ? element : nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (element)
	{
		g_UIHandleIndex[element.get()].insert(handle);
	}
}

// CreateUIHandle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static ManagedUIElementHandle* CreateUIHandle(const std::shared_ptr<UIElement>& element, bool keepDetachedOwnership)
{
	if (!element)
	{
		return nullptr;
	}

	ManagedUIElementHandle* handle = new ManagedUIElementHandle();
	BindUIHandleToElement(handle, element, keepDetachedOwnership);
	return handle;
}

// ResolveUIHandle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static std::shared_ptr<UIElement> ResolveUIHandle(void* handlePointer)
{
	if (handlePointer == nullptr)
	{
		return nullptr;
	}

	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	ManagedUIElementHandle* handle = static_cast<ManagedUIElementHandle*>(handlePointer);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (handle->ownedElement)
	{
		return handle->ownedElement;
	}

	return handle->weakElement.lock();
}

// PromoteDetachedUIOwnership is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void PromoteDetachedUIOwnership(const std::shared_ptr<UIElement>& element)
{
	if (!element)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto iterator = g_UIHandleIndex.find(element.get()); iterator != g_UIHandleIndex.end())
	{
		for (ManagedUIElementHandle* handle : iterator->second)
		{
			handle->ownedElement = element;
		}
	}
}

// DropDetachedUIOwnership is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void DropDetachedUIOwnership(const std::shared_ptr<UIElement>& element)
{
	if (!element)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto iterator = g_UIHandleIndex.find(element.get()); iterator != g_UIHandleIndex.end())
	{
		for (ManagedUIElementHandle* handle : iterator->second)
		{
			handle->ownedElement.reset();
		}
	}
}

// RefreshManagedElementStyle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void RefreshManagedElementStyle(UIElement& element)
{
	// Runtime setters act like inline style overrides layered on top of the
	// already-authored computed style, so only the touched property needs to be
	// copied through before the display style is refreshed.
	element.RefreshDisplayStyle();
	element.MarkDirty();
}

// ResolveManagedBridgeAssemblyPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static std::filesystem::path ResolveManagedBridgeAssemblyPath()
{
	const auto resolveExecutableDirectory = []() -> std::filesystem::path {
		const std::filesystem::path executableDirectory = TerraEngine::PlatformUtils::GetExecutableDirectory();
		if (!executableDirectory.empty())
		{
			return executableDirectory;
		}

		return std::filesystem::current_path();
	};

	const std::filesystem::path executableDirectory = resolveExecutableDirectory();
	const std::filesystem::path current = std::filesystem::current_path();
	const std::filesystem::path candidates[] = {
		executableDirectory / "Managed" / "TerraEngine.CSharp.dll",
		executableDirectory / "Editor" / "Managed" / "TerraEngine.CSharp.dll",
		executableDirectory.parent_path() / "Editor" / "Managed" / "TerraEngine.CSharp.dll",
		current / "Managed" / "TerraEngine.CSharp.dll",
		current / "Editor" / "Managed" / "TerraEngine.CSharp.dll",
		current.parent_path() / "Editor" / "Managed" / "TerraEngine.CSharp.dll"
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const std::filesystem::path& candidate : candidates)
	{
		if (std::filesystem::exists(candidate))
		{
			return candidate;
		}
	}

	return candidates[0];
}

// ResolveManagedUIEventDispatcher is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static ManagedUIDispatchEventFn ResolveManagedUIEventDispatcher()
{
	std::call_once(g_UIEventDispatchResolveOnce, []() {
		g_UIEventDispatchResolveAttempted = true;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!DotNetHost::Instance().IsInitialized() || DotNetHost::Instance().IsExportMode())
		{
			return;
		}

		g_ManagedUIDispatchEvent = reinterpret_cast<ManagedUIDispatchEventFn>(DotNetHost::Instance().GetManagedFunction(
			ResolveManagedBridgeAssemblyPath(),
			L"TerraEngine.InternalCalls, TerraEngine.CSharp",
			L"DispatchNativeUIEvent"));
	});

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (g_ManagedUIDispatchEvent == nullptr && g_UIEventDispatchResolveAttempted && !g_UIEventDispatchUnavailableLogged)
	{
		g_UIEventDispatchUnavailableLogged = true;
		const LogLevel level = DotNetHost::Instance().IsExportMode() ? LogLevel::Debug : LogLevel::Warning;
		TERRAENGINE_LOG(level, "[UI] Managed UI event dispatch is unavailable. Falling back to managed polling events where possible.");
	}

	return g_ManagedUIDispatchEvent;
}

// FindFirstUIHandleForElement is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static ManagedUIElementHandle* FindFirstUIHandleForElement(UIElement& element)
{
	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto iterator = g_UIHandleIndex.find(&element); iterator != g_UIHandleIndex.end() && !iterator->second.empty())
	{
		return *iterator->second.begin();
	}

	return nullptr;
}

// DispatchManagedUIEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void DispatchManagedUIEvent(UIElement& element, ManagedUIEventType eventType, const glm::vec2& pointerPosition)
{
	ManagedUIDispatchEventFn dispatcher = ResolveManagedUIEventDispatcher();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (dispatcher == nullptr)
	{
		return;
	}

	ManagedUIElementHandle* handle = FindFirstUIHandleForElement(element);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (handle == nullptr)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "[UI] Tried to dispatch an event for element '" << element.id << "' without an active managed handle.");
		return;
	}

	dispatcher(handle, static_cast<int>(eventType), pointerPosition.x, pointerPosition.y);
}

// DispatchManagedClickEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void DispatchManagedClickEvent(UIElement& element, const glm::vec2& pointerPosition)
{
	DispatchManagedUIEvent(element, ManagedUIEventType::Click, pointerPosition);
}

// DispatchManagedMouseEnterEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void DispatchManagedMouseEnterEvent(UIElement& element, const glm::vec2& pointerPosition)
{
	DispatchManagedUIEvent(element, ManagedUIEventType::MouseEnter, pointerPosition);
}

// DispatchManagedMouseLeaveEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void DispatchManagedMouseLeaveEvent(UIElement& element, const glm::vec2& pointerPosition)
{
	DispatchManagedUIEvent(element, ManagedUIEventType::MouseLeave, pointerPosition);
}

// SetApplicationHostEngine is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SetApplicationHostEngine(Engine* engine)
{
	std::lock_guard<std::mutex> lock(g_ApplicationBridgeMutex);
	g_ApplicationHostEngine = engine;
}

// SetApplicationEditorStopPlayModeCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SetApplicationEditorStopPlayModeCallback(ApplicationEditorStopPlayModeCallback callback)
{
	std::lock_guard<std::mutex> lock(g_ApplicationBridgeMutex);
	g_ApplicationEditorStopCallback = callback;
}

extern "C" {

	// Native_Log is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Log(int level, const char* message, const char* function, int line) {
		LogLevel logLevel = static_cast<LogLevel>(level);
		Logger::Instance().Log(logLevel, "C#", line, function, message, true);
	}

	// Native_Application_Quit is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Application_Quit() {
		if (ScriptingSystem::Instance().IsExportMode()) {
			if (Engine* hostEngine = GetApplicationHostEngine()) {
				hostEngine->SignalShutdown();
				return;
			}

			TERRAENGINE_LOG(LogLevel::Warning, "Application.Quit() was called in export mode before the host engine was registered.");
			return;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ApplicationEditorStopPlayModeCallback stopPlayMode = GetApplicationEditorStopCallback()) {
			stopPlayMode();
			return;
		}

		// Fall back to stopping script execution so a missing editor callback does
		// not keep runaway gameplay scripts alive forever during development.
		TERRAENGINE_LOG(LogLevel::Warning, "Application.Quit() could not reach the editor Play Mode stop callback; stopping scripts without restoring the scene snapshot.");
		ScriptingSystem::Instance().SetRunning(false);
		ScriptingSystem::Instance().SetPaused(false);
	}

	// Native_Application_OpenURL is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Application_OpenURL(const char* url) {
		TerraEngine::PlatformUtils::OpenUrl(url != nullptr ? url : "");
	}

	// Native_Application_GetPersistentDataPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Application_GetPersistentDataPath() {
		const std::filesystem::path persistentDataPath = TerraEngine::PlatformUtils::GetPersistentDataPath(
			ResourceManager::Instance().GetProductName());
		return WriteApplicationReturnText(persistentDataPath.string());
	}

	// Native_Application_GetStreamingAssetsPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Application_GetStreamingAssetsPath() {
		return WriteApplicationReturnText(ResourceManager::Instance().GetAssetRoot().string());
	}

	// Native_Application_IsEditor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_Application_IsEditor() {
		return !ScriptingSystem::Instance().IsExportMode();
	}

	// Native_Application_GetPlatform is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::uint32_t Native_Application_GetPlatform() {
		return static_cast<std::uint32_t>(TerraEngine::PlatformUtils::GetRuntimePlatform());
	}

	void Native_Scripting_PushDispatchContext(uint32_t objectId, const char* bindingId, std::uint64_t frameOrder, int workerDispatch, int exportMode, std::uint64_t objectGeneration, std::uint64_t componentGeneration, std::uint64_t bindingGeneration) {
		ScriptingSystem::ScriptDispatchContext context;
		context.workerDispatch = workerDispatch != 0;
		context.exportMode = exportMode != 0;
		context.sourceObjectId = objectId;
		context.sourceBindingId = bindingId != nullptr ? bindingId : "";
		context.frameOrder = frameOrder;
		context.objectGeneration = objectGeneration;
		context.componentGeneration = componentGeneration;
		context.bindingGeneration = bindingGeneration;
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			context.sourceObjectUuid = objects.getUUID(*object);
		}
		ScriptingSystem::SetCurrentScriptDispatchContext(std::move(context));
	}

	void Native_Scripting_PopDispatchContext() {
		ScriptingSystem::ClearCurrentScriptDispatchContext();
	}

	void Native_Scripting_ReportBridgeCacheCounters(std::uint64_t hits, std::uint64_t misses, std::uint64_t avoidedCalls) {
		ScriptingSystem::RecordBridgeCacheCounters(
			static_cast<std::size_t>(hits),
			static_cast<std::size_t>(misses),
			static_cast<std::size_t>(avoidedCalls));
	}

	// Native_Application_GetProductName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Application_GetProductName() {
		return WriteApplicationReturnText(ResourceManager::Instance().GetProductName());
	}

	// Native_Application_GetVersion is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Application_GetVersion() {
		return WriteApplicationReturnText(ResourceManager::Instance().GetVersion());
	}

	// Native_Object_Destroy is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Object_Destroy(uint32_t objectId) {
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			const entt::entity entity = static_cast<entt::entity>(objectId);
			if (std::optional<Object> object = objects.getObject(entity)) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				scriptingSystem.RecordScriptMutationRequest(scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::DeleteObject,
					*context,
					objects.getUUID(*object)));
			}
			return;
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		// Calls outside a gameplay dispatch can run only on an already protected
		// host path. Every managed lifecycle callback returns through the queued
		// branch above, regardless of which scheduler thread invoked it.
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			objects.removeObject(*object);
		}
	}

	// Native_Object_GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Object_GetName(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		static thread_local std::string nameBuffer;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			nameBuffer = objects.getObjectName(*object);
			return nameBuffer.c_str();
		}
		return "";
	}

	// Native_Object_SetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Object_SetName(uint32_t objectId, const char* name) {
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				auto request = scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::SetObjectName, *context, objects.getUUID(*object));
				request.metadataValue = name ? name : "";
				request.coalescingKey = "metadata|" + TerraEngine::ToHexString(objects.getUUID(*object)) + "|name";
				scriptingSystem.RecordScriptMutationRequest(std::move(request));
			}
			return;
		}
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			objects.setObjectName(*object, name ? name : "");
		}
	}

	// Native_Object_Find is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	uint32_t Native_Object_Find(const char* name) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (name == nullptr || name[0] == '\0') {
			return InvalidObjectId();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto object = ObjectManager::instance().FindObjectByName(name)) {
			return static_cast<uint32_t>(object->EntityId());
		}

		return InvalidObjectId();
	}

	// Native_Object_FindWithTag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	uint32_t Native_Object_FindWithTag(const char* tag) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (tag == nullptr || tag[0] == '\0') {
			return InvalidObjectId();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto object = ObjectManager::instance().FindObjectByTag(tag)) {
			return static_cast<uint32_t>(object->EntityId());
		}

		return InvalidObjectId();
	}

	static NativeRuntimeStatus Native_Object_RequestCreate(const char* name, NativeObjectCreationReceipt* receipt) {
		if(receipt == nullptr || !HasRuntimeAbiHeader(receipt->header, sizeof(*receipt))) return NativeRuntimeStatus::VersionMismatch;
		receipt->header = {kNativeRuntimeAbiVersion, sizeof(*receipt)};
		receipt->status = NativeRuntimeStatus::InvalidArgument;
		receipt->reserved = 0U;
		std::memset(receipt->stableId, 0, sizeof(receipt->stableId));
		if(name == nullptr || name[0] == '\0') return receipt->status;

		const TerraEngine::UUID stableId = TerraEngine::GenerateUUID();
		TerraEngine::Scene::SceneMutationRequest mutation;
		if(const auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			mutation = ScriptingSystem::Instance().BuildScriptMutationRequest(
				TerraEngine::Scene::SceneMutationOperation::CreateObject,
				*context,
				stableId,
				name);
		} else {
			mutation.operation = TerraEngine::Scene::SceneMutationOperation::CreateObject;
			mutation.targetObjectId = stableId;
			mutation.displayName = name;
			mutation.requestId = "managed-object-create-" + TerraEngine::ToHexString(stableId);
		}
		ScriptingSystem::Instance().RecordScriptMutationRequest(std::move(mutation));

		const std::string text = TerraEngine::ToHexString(stableId);
		const std::size_t count = std::min(text.size(), sizeof(receipt->stableId) - 1U);
		std::memcpy(receipt->stableId, text.data(), count);
		receipt->status = NativeRuntimeStatus::Accepted;
		return receipt->status;
	}

	static NativeRuntimeStatus Native_Object_ResolveStableId(const char* stableId, std::uint32_t* objectId) {
		if(objectId == nullptr) return NativeRuntimeStatus::InvalidArgument;
		*objectId = InvalidObjectId();
		if(stableId == nullptr || stableId[0] == '\0') return NativeRuntimeStatus::InvalidArgument;
		const std::optional<TerraEngine::UUID> parsed = TerraEngine::ParseUUID(stableId);
		if(!parsed) return NativeRuntimeStatus::InvalidArgument;
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		const std::optional<Object> object = ObjectManager::instance().getObject(*parsed);
		if(!object) return NativeRuntimeStatus::NotFound;
		*objectId = static_cast<std::uint32_t>(object->EntityId());
		return NativeRuntimeStatus::Success;
	}

	static uint32_t Native_Object_GetParent(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId));
		if(!object) return InvalidObjectId();
		const std::optional<Object> parent = objects.getParent(*object);
		return parent ? static_cast<uint32_t>(parent->EntityId()) : InvalidObjectId();
	}

	static NativeRuntimeStatus Native_Object_SetParent(uint32_t objectId, uint32_t parentObjectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId));
		if(!object) return NativeRuntimeStatus::NotFound;
		const TerraEngine::UUID objectUuid = objects.getUUID(*object);
		TerraEngine::Scene::TransactionObjectRef parentRef{};
		if(parentObjectId != InvalidObjectId()) {
			const std::optional<Object> parent = objects.getObject(static_cast<entt::entity>(parentObjectId));
			if(!parent || parent->EntityId() == object->EntityId()) return NativeRuntimeStatus::InvalidArgument;
			parentRef = TerraEngine::Scene::ExistingObject(objects.getUUID(*parent));
		}
		TerraEngine::Scene::TransactionalMutationBuilder builder(
			"managed-reparent-" + TerraEngine::ToHexString(objectUuid),
			TerraEngine::Scene::TransactionOrigin::ManagedCallback,
			"managed-hierarchy");
		builder.ObserveObject(objects, objectUuid).ReparentObject(objectUuid, parentRef);
		TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(std::move(builder).Build());
		return NativeRuntimeStatus::Accepted;
	}

	static NativeRuntimeStatus Native_Object_CopyChildren(uint32_t objectId, uint32_t* children, std::uint32_t capacity, std::uint32_t* requiredCount) {
		if(requiredCount == nullptr) return NativeRuntimeStatus::InvalidArgument;
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId));
		if(!object) { *requiredCount = 0U; return NativeRuntimeStatus::NotFound; }
		const std::vector<Object> values = objects.getChildren(*object);
		*requiredCount = static_cast<std::uint32_t>(values.size());
		if(values.size() > capacity || (!values.empty() && children == nullptr)) return NativeRuntimeStatus::Conflict;
		for(std::size_t index = 0U; index < values.size(); ++index) children[index] = static_cast<uint32_t>(values[index].EntityId());
		return NativeRuntimeStatus::Success;
	}

	static NativeRuntimeStatus Native_Object_CopyAll(uint32_t* output, std::uint32_t capacity, std::uint32_t* requiredCount) {
		if(requiredCount == nullptr) return NativeRuntimeStatus::InvalidArgument;
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		std::vector<Object> values = ObjectManager::instance().getObjectsWithComponentsIncludingInactive<Transform>();
		ObjectManager::instance().SortObjectsByStableId(values);
		*requiredCount = static_cast<std::uint32_t>(values.size());
		if(values.size() > capacity || (!values.empty() && output == nullptr)) return NativeRuntimeStatus::Conflict;
		for(std::size_t index = 0U; index < values.size(); ++index) output[index] = static_cast<uint32_t>(values[index].EntityId());
		return NativeRuntimeStatus::Success;
	}

	static NativeRuntimeStatus Native_Object_CopyWithTag(const char* tag, uint32_t* output, std::uint32_t capacity, std::uint32_t* requiredCount) {
		if(tag == nullptr || requiredCount == nullptr) return NativeRuntimeStatus::InvalidArgument;
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		std::vector<Object> values = objects.getObjectsWithComponentsIncludingInactive<Transform>();
		values.erase(std::remove_if(values.begin(), values.end(), [&](const Object& object) { return objects.getObjectTag(object) != tag; }), values.end());
		objects.SortObjectsByStableId(values);
		*requiredCount = static_cast<std::uint32_t>(values.size());
		if(values.size() > capacity || (!values.empty() && output == nullptr)) return NativeRuntimeStatus::Conflict;
		for(std::size_t index = 0U; index < values.size(); ++index) output[index] = static_cast<uint32_t>(values[index].EntityId());
		return NativeRuntimeStatus::Success;
	}

	// Native_MeshRenderer_GetMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_MeshRenderer_GetMesh(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		return GetMeshRendererAssetReference(objectId, "meshId", AssetType::Mesh);
	}

	// Native_MeshRenderer_SetMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_MeshRenderer_SetMesh(uint32_t objectId, const char* assetReference) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		SetMeshRendererAssetReference(objectId, "meshId", AssetType::Mesh, assetReference);
	}

	// Native_MeshRenderer_GetMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_MeshRenderer_GetMaterial(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		return GetMeshRendererAssetReference(objectId, "materialId", AssetType::Material);
	}

	// Native_MeshRenderer_SetMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_MeshRenderer_SetMaterial(uint32_t objectId, const char* assetReference) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		SetMeshRendererAssetReference(objectId, "materialId", AssetType::Material, assetReference);
	}

	// Native_ResourceStreaming_Preload bridges C# gameplay hints into the same
	// streaming state used by renderer/editor needs. It schedules background
	// loading but does not block the script frame.
	bool Native_ResourceStreaming_Preload(const char* assetReference, int assetType) {
		if (assetReference == nullptr || assetReference[0] == '\0') {
			return false;
		}
		const std::optional<AssetType> resolvedType = ManagedAssetType(assetType);
		if(!resolvedType) return false;
		const AssetType type = *resolvedType;
		ResourceManager& resources = ResourceManager::Instance();
		const TerraEngine::UUID id = resources.ResolveAssetReference(assetReference, type);
		if (id == TerraEngine::EMPTY_UUID) {
			return false;
		}
		resources.RequestStreamableLoad(
			id,
			type,
			TerraEngine::Resource::StreamingNeedType::RuntimeExplicit,
			0u,
			"managed explicit preload");
		return true;
	}

	// Native_HasComponent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_HasComponent(uint32_t objectId, const char* componentType) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (objects.getObject(entity).has_value()) {
			auto& reg = ComponentAccessRegistry();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto it = reg.find(componentType); it != reg.end()) {
				return it->second.entityHasComponent(objects.getRegistry(), entity);
			}
		}
		return false;
	}

	// Native_AddComponent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_AddComponent(uint32_t objectId, const char* componentType) {
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			if (componentType == nullptr || componentType[0] == '\0') {
				return false;
			}
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			const entt::entity entity = static_cast<entt::entity>(objectId);
			if (std::optional<Object> object = objects.getObject(entity)) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				scriptingSystem.RecordScriptMutationRequest(scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::AddComponent,
					*context,
					objects.getUUID(*object),
					{},
					componentType));
				return true;
			}
			return false;
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			auto& reg = ComponentAccessRegistry();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto it = reg.find(componentType); it != reg.end()) {
				it->second.entityAddComponent(objects.getRegistry(), entity);
				objects.notifyObjectStructureChanged(*object);
				return true;
			}
		}
		return false;
	}

	bool Native_RemoveComponent(uint32_t objectId, const char* componentType) {
		if (componentType == nullptr || componentType[0] == '\0' || std::string_view(componentType) == Transform::kComponentName) {
			return false;
		}
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			const entt::entity entity = static_cast<entt::entity>(objectId);
			if (std::optional<Object> object = objects.getObject(entity)) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				scriptingSystem.RecordScriptMutationRequest(scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::RemoveComponent,
					*context,
					objects.getUUID(*object),
					{},
					componentType));
				return true;
			}
			return false;
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			auto& registry = ComponentAccessRegistry();
			if (auto iterator = registry.find(componentType); iterator != registry.end()
				&& iterator->second.entityHasComponent(objects.getRegistry(), entity)) {
				iterator->second.entityRemoveComponent(objects.getRegistry(), entity);
				objects.notifyObjectStructureChanged(*object);
				return true;
			}
		}
		return false;
	}

	std::uint64_t Native_Object_GetComponentGeneration(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			return objects.getObjectComponentGeneration(*object);
		}
		return 0;
	}

	std::uint64_t Native_Object_GetMetadataGeneration(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			return objects.getObjectMetadataGeneration(*object);
		}
		return 0;
	}

	std::uint64_t Native_Object_GetBindingGeneration(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			return objects.getObjectBindingGeneration(*object);
		}
		return 0;
	}

	// Native_ScriptComponent_AddBinding is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_ScriptComponent_AddBinding(uint32_t objectId, const char* className) {
		if (className == nullptr || className[0] == '\0') {
			return false;
		}
		if (const auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId));
			if(!object) return false;
			ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
			auto request = scriptingSystem.BuildScriptMutationRequest(
				TerraEngine::Scene::SceneMutationOperation::AddScriptBinding,
				*context,
				objects.getUUID(*object));
			request.metadataValue = className;
			request.coalescingKey = std::to_string(context->frameOrder)
				+ "|" + TerraEngine::ToHexString(objects.getUUID(*object))
				+ "|ScriptBinding|" + className;
			scriptingSystem.RecordScriptMutationRequest(std::move(request));
			return true;
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			ScriptComponent* scriptComponent = objects.GetComponent<ScriptComponent>(*object);
			if (scriptComponent == nullptr) {
				scriptComponent = objects.AddComponent<ScriptComponent>(*object);
			}
			if (scriptComponent == nullptr) {
				return false;
			}

			auto guard = scriptComponent->acquireWriteLock();
			scriptComponent->AddBinding(TerraEngine::EMPTY_UUID, className);
			objects.notifyObjectStructureChanged(*object);
			return true;
		}

		return false;
	}

	// Native_ScriptComponent_HasBinding is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_ScriptComponent_HasBinding(uint32_t objectId, const char* className) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (className == nullptr || className[0] == '\0') {
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			if (ScriptComponent* scriptComponent = objects.GetComponent<ScriptComponent>(*object)) {
				auto guard = scriptComponent->acquireReadLock();
				return scriptComponent->HasBinding(className);
			}
		}

		return false;
	}

	// Native_ScriptComponent_GetBindingPayload is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_ScriptComponent_GetBindingPayload(uint32_t objectId, const char* bindingId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		g_ScriptBindingPayloadBuffer.clear();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (bindingId == nullptr || bindingId[0] == '\0') {
			return g_ScriptBindingPayloadBuffer.c_str();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			if (ScriptComponent* scriptComponent = objects.GetComponent<ScriptComponent>(*object)) {
				auto guard = scriptComponent->acquireReadLock();
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (const ScriptBinding* binding = scriptComponent->FindBinding(bindingId)) {
					// Return the authored binding payload only. Live runtime state is
					// reconstructed on demand inside the managed bridge.
					g_ScriptBindingPayloadBuffer = nlohmann::json(*binding).dump();
				}
			}
		}

		return g_ScriptBindingPayloadBuffer.c_str();
	}

	std::uint64_t Native_ScriptComponent_GetBindingVersion(uint32_t objectId, const char* bindingId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (bindingId == nullptr || bindingId[0] == '\0') {
			return 0;
		}

		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			if (ScriptComponent* scriptComponent = objects.GetComponent<ScriptComponent>(*object)) {
				auto guard = scriptComponent->acquireReadLock();
				if (const ScriptBinding* binding = scriptComponent->FindBinding(bindingId)) {
					return binding->authoredStateVersion;
				}
			}
		}

		return 0;
	}

	// Reports the runtime observed version of a script binding back to the engine.
	// Input: The object ID, the binding ID, and the newly observed version number.
	// Output: None. Updates the binding's snapshotVersion in the ScriptComponent.
	void Native_ScriptComponent_ReportObservedVersion(uint32_t objectId, const char* bindingId, std::uint64_t observedVersion) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (bindingId == nullptr || bindingId[0] == '\0') {
			return;
		}

		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			if (ScriptComponent* scriptComponent = objects.GetComponent<ScriptComponent>(*object)) {
				auto guard = scriptComponent->acquireWriteLock();
				if (ScriptBinding* binding = scriptComponent->FindBinding(bindingId)) {
					binding->snapshotVersion = observedVersion;
				}
			}
		}
	}

	// Native_ScriptComponent_SetBindingValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_ScriptComponent_SetBindingValue(uint32_t objectId, const char* bindingId, const char* fieldPath, const char* valueKind, const char* valueJson, const char* compatibility, const char* sourceTypeFingerprint) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (bindingId == nullptr || bindingId[0] == '\0' || fieldPath == nullptr || fieldPath[0] == '\0' || valueJson == nullptr) {
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			ScriptComponent* scriptComponent = objects.GetComponent<ScriptComponent>(*object);
			if (scriptComponent == nullptr) {
				return false;
			}

			{
				auto guard = scriptComponent->acquireWriteLock();
				ScriptBinding* binding = ResolveBinding(entity, bindingId);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (binding == nullptr) {
					return false;
				}

				ScriptFieldValue& fieldValue = binding->UpsertSerializedValue(fieldPath, valueKind != nullptr ? valueKind : "");
				try {
					// Inspector writes already serialize through JSON, so preserve the
					// structured payload when possible instead of flattening to strings.
					fieldValue.value = nlohmann::json::parse(valueJson);
				} catch (...) {
					fieldValue.value = valueJson;
				}

				fieldValue.fieldPath = fieldPath;
				fieldValue.fieldName = fieldPath;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (valueKind != nullptr) {
					fieldValue.valueKind = valueKind;
				}
				fieldValue.compatibility = compatibility != nullptr && compatibility[0] != '\0' ? compatibility : "Current";
				fieldValue.persistenceState = fieldValue.compatibility;
				fieldValue.sourceTypeFingerprint = sourceTypeFingerprint != nullptr ? sourceTypeFingerprint : "";
				scriptComponent->SynchronizeStorageFromBindings();
			}
			objects.notifyObjectStructureChanged(*object);
			return true;
		}

		return false;
	}

	// Native_ScriptComponent_RemoveBindingValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_ScriptComponent_RemoveBindingValue(uint32_t objectId, const char* bindingId, const char* fieldPath) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (bindingId == nullptr || bindingId[0] == '\0' || fieldPath == nullptr || fieldPath[0] == '\0') {
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			ScriptComponent* scriptComponent = objects.GetComponent<ScriptComponent>(*object);
			if (scriptComponent == nullptr) {
				return false;
			}

			{
				auto guard = scriptComponent->acquireWriteLock();
				ScriptBinding* binding = ResolveBinding(entity, bindingId);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (binding == nullptr || !binding->RemoveSerializedValue(fieldPath)) {
					return false;
				}

				scriptComponent->SynchronizeStorageFromBindings();
			}
			objects.notifyObjectStructureChanged(*object);
			return true;
		}

		return false;
	}

	// Native_GetComponentProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_GetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* outValue, int type) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (objects.getObject(entity).has_value()) {
			auto& reg = ComponentAccessRegistry();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto it = reg.find(componentType); it != reg.end()) {
				TerraEngine::Reflection::Variant val = it->second.entityGetter(objects.getRegistry(), entity, propertyName);
				MarshallToNative(val, outValue, static_cast<ScriptType>(type));
			}
		}
	}

	// Native_SetComponentProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_SetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* inValue, int type) {
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			if (componentType == nullptr || componentType[0] == '\0' || propertyName == nullptr || propertyName[0] == '\0' || inValue == nullptr) {
				return;
			}

			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			const entt::entity entity = static_cast<entt::entity>(objectId);
			if (std::optional<Object> object = objects.getObject(entity)) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				TerraEngine::Scene::SceneMutationRequest request = scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::UpdateComponent,
					*context,
					objects.getUUID(*object),
					{},
					componentType);
				request.typedComponentPayload.propertyName = propertyName;
				request.typedComponentPayload.valueKind = type;
				request.typedComponentPayload.value = MarshallFromNative(inValue, static_cast<ScriptType>(type));
				request.typedComponentPayload.valid = TerraEngine::Reflection::IsValid(request.typedComponentPayload.value);
				if (!request.typedComponentPayload.valid) {
					nlohmann::json payload{
						{ "propertyName", propertyName },
						{ "valueKind", type },
						{ "value", MarshallNativeValueToJson(inValue, static_cast<ScriptType>(type)) }
					};
					request.payloadJson = payload.dump();
					request.usedJsonFallback = true;
				}
				request.coalescingKey = std::to_string(context->frameOrder)
					+ "|" + TerraEngine::ToHexString(objects.getUUID(*object))
					+ "|" + componentType
					+ "|" + propertyName;
				scriptingSystem.RecordScriptMutationRequest(std::move(request));
			}
			return;
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			auto& reg = ComponentAccessRegistry();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (componentType != nullptr && propertyName != nullptr && inValue != nullptr && componentType[0] != '\0' && propertyName[0] != '\0') {
				if (auto it = reg.find(componentType); it != reg.end()) {
					TerraEngine::Reflection::Variant val = MarshallFromNative(inValue, static_cast<ScriptType>(type));
					it->second.entitySetter(objects.getRegistry(), entity, propertyName, val);
					if (std::string_view(componentType) == Transform::kComponentName) {
						objects.MarkObjectTransformDirty(*object);
					} else {
						objects.notifyObjectStructureChanged(*object);
					}
				}
			}
		}
	}

	const char* Native_Object_GetTag(uint32_t objectId) {
		static thread_local std::string valueBuffer;
		valueBuffer = "Untagged";
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
			valueBuffer = objects.getObjectTag(*object);
		}
		return valueBuffer.c_str();
	}

	void Native_Object_SetTag(uint32_t objectId, const char* tag) {
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				auto request = scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::SetObjectTag, *context, objects.getUUID(*object));
				request.metadataValue = tag ? tag : "Untagged";
				request.coalescingKey = "metadata|" + TerraEngine::ToHexString(objects.getUUID(*object)) + "|tag";
				scriptingSystem.RecordScriptMutationRequest(std::move(request));
			}
			return;
		}
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
			objects.setObjectTag(*object, tag ? tag : "Untagged");
		}
	}

	const char* Native_Object_GetLayer(uint32_t objectId) {
		static thread_local std::string valueBuffer;
		valueBuffer = "Default";
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
			valueBuffer = objects.getObjectLayer(*object);
		}
		return valueBuffer.c_str();
	}

	void Native_Object_SetLayer(uint32_t objectId, const char* layer) {
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				auto request = scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::SetObjectLayer, *context, objects.getUUID(*object));
				request.metadataValue = layer ? layer : "Default";
				request.coalescingKey = "metadata|" + TerraEngine::ToHexString(objects.getUUID(*object)) + "|layer";
				scriptingSystem.RecordScriptMutationRequest(std::move(request));
			}
			return;
		}
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
			objects.setObjectLayer(*object, layer ? layer : "Default");
		}
	}

	bool Native_Object_GetActive(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
			return objects.isObjectActive(*object);
		}
		return false;
	}

	bool Native_Object_GetActiveInHierarchy(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
			return objects.isObjectActiveInHierarchy(*object);
		}
		return false;
	}

	void Native_Object_SetActive(uint32_t objectId, bool active) {
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				auto request = scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::SetObjectActive, *context, objects.getUUID(*object));
				request.metadataBooleanValue = active;
				request.coalescingKey = "metadata|" + TerraEngine::ToHexString(objects.getUUID(*object)) + "|active";
				scriptingSystem.RecordScriptMutationRequest(std::move(request));
			}
			return;
		}
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
			objects.setObjectActive(*object, active);
		}
	}

	bool Native_Object_GetStatic(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
			return objects.isObjectStatic(*object);
		}
		return false;
	}

	void Native_Object_SetStatic(uint32_t objectId, bool isStatic) {
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				auto request = scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::SetObjectStatic, *context, objects.getUUID(*object));
				request.metadataBooleanValue = isStatic;
				request.coalescingKey = "metadata|" + TerraEngine::ToHexString(objects.getUUID(*object)) + "|static";
				scriptingSystem.RecordScriptMutationRequest(std::move(request));
			}
			return;
		}
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		if (std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId))) {
			objects.setObjectStatic(*object, isStatic);
		}
	}

	const char* Native_GetComponentStringProperty(uint32_t objectId, const char* componentType, const char* propertyName) {
		static thread_local std::string valueBuffer;
		valueBuffer.clear();
		if (componentType == nullptr || propertyName == nullptr) {
			return valueBuffer.c_str();
		}

		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (objects.getObject(entity).has_value()) {
			auto& registry = ComponentAccessRegistry();
			if (auto iterator = registry.find(componentType); iterator != registry.end()) {
				TerraEngine::Reflection::Variant value = iterator->second.entityGetter(objects.getRegistry(), entity, propertyName);
				if (TerraEngine::Reflection::IsType<std::string>(value)) {
					valueBuffer = TerraEngine::Reflection::GetValue<std::string>(value);
				}
			}
		}
		return valueBuffer.c_str();
	}

	void Native_SetComponentStringProperty(uint32_t objectId, const char* componentType, const char* propertyName, const char* value) {
		if (componentType == nullptr || componentType[0] == '\0' || propertyName == nullptr
			|| propertyName[0] == '\0' || value == nullptr) {
			return;
		}
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			const entt::entity entity = static_cast<entt::entity>(objectId);
			if (std::optional<Object> object = objects.getObject(entity)) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				TerraEngine::Scene::SceneMutationRequest request = scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::UpdateComponent,
					*context,
					objects.getUUID(*object),
					{},
					componentType);
				request.typedComponentPayload.propertyName = propertyName;
				request.typedComponentPayload.value = std::string(value);
				request.typedComponentPayload.valid = true;
				request.coalescingKey = std::to_string(context->frameOrder)
					+ "|" + TerraEngine::ToHexString(objects.getUUID(*object))
					+ "|" + componentType + "|" + propertyName;
				scriptingSystem.RecordScriptMutationRequest(std::move(request));
			}
			return;
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		if (std::optional<Object> object = objects.getObject(entity)) {
			auto& registry = ComponentAccessRegistry();
			if (auto iterator = registry.find(componentType); iterator != registry.end()) {
				iterator->second.entitySetter(objects.getRegistry(), entity, propertyName, std::string(value));
				objects.notifyObjectStructureChanged(*object);
			}
		}
	}

	std::uint32_t Native_Graphics_GetAbiVersion() {
		return kNativeGraphicsAbiVersion;
	}

	NativeGraphicsStatus Native_Graphics_ReadRenderSettings(NativeGraphicsRenderSettings* settings) {
		if (settings == nullptr) {
			return NativeGraphicsStatus::InvalidArgument;
		}
		if (!HasValidGraphicsHeader(settings->header)) {
			return NativeGraphicsStatus::VersionMismatch;
		}
		*settings = ToNativeGraphicsSettings(RenderSystem::Instance().GetRuntimeRenderSettingsSnapshot());
		return NativeGraphicsStatus::Success;
	}

	NativeGraphicsStatus Native_Graphics_SubmitRuntimeRenderSettings(const NativeGraphicsRenderSettings* settings) {
		if (settings == nullptr) {
			return NativeGraphicsStatus::InvalidArgument;
		}
		if (!HasValidGraphicsHeader(settings->header)) {
			return NativeGraphicsStatus::VersionMismatch;
		}
		return RenderSystem::Instance().QueueRuntimeRenderSettings(FromNativeGraphicsSettings(*settings))
			? NativeGraphicsStatus::Accepted
			: NativeGraphicsStatus::InvalidArgument;
	}

	NativeGraphicsStatus Native_Graphics_SubmitRuntimeQualityPreset(std::uint32_t preset) {
		if (preset > static_cast<std::uint32_t>(QualityPreset::Ultra)) {
			return NativeGraphicsStatus::InvalidPreset;
		}
		return RenderSystem::Instance().QueueRuntimeQualityPreset(static_cast<QualityPreset>(preset))
			? NativeGraphicsStatus::Accepted
			: NativeGraphicsStatus::InvalidPreset;
	}

	NativeGraphicsStatus Native_Graphics_ReadQualityPreset(std::uint32_t* preset) {
		if (preset == nullptr) {
			return NativeGraphicsStatus::InvalidArgument;
		}
		*preset = static_cast<std::uint32_t>(RenderSystem::Instance().GetRuntimeQualityPresetSnapshot());
		return NativeGraphicsStatus::Success;
	}

	std::uint64_t Native_Graphics_GetRuntimeSettingsGeneration() {
		return RenderSystem::Instance().GetRuntimeRenderSettingsGeneration();
	}

	// Native_Graphics_GetLightingDiagnosticsSummary is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Graphics_GetLightingDiagnosticsSummary(uint32_t viewType) {
		static std::string summary;
		const RenderViewType renderView = viewType == 1u ? RenderViewType::Scene : RenderViewType::Game;
		const LightingDiagnosticsSnapshot diagnostics = RenderSystem::Instance().GetLatestLightingDiagnostics(renderView);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!diagnostics.valid) {
			summary = "Lighting diagnostics are not available for the latest completed frame.";
			return summary.c_str();
		}

		summary.clear();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const LightingDiagnosticEntry& entry : diagnostics.entries) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!summary.empty()) {
				summary += "\n";
			}
			summary += entry.message;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!entry.recommendedAction.empty()) {
				summary += " ";
				summary += entry.recommendedAction;
			}
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (summary.empty()) {
			summary = "Lighting diagnostics are active and no degraded state was reported.";
		}
		return summary.c_str();
	}

	// Native_ResourceManager_GetAssetRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_ResourceManager_GetAssetRoot() {
		static std::string assetRoot;
		assetRoot = ResourceManager::Instance().GetAssetRoot().string();
		return assetRoot.c_str();
	}

	// Native_Input_IsKeyPressed is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_Input_IsKeyPressed(int key) {
		return InputSystem::Instance().IsKeyPressed(static_cast<Key>(key));
	}

	// Native_Input_IsMouseButtonPressed is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_Input_IsMouseButtonPressed(int button) {
		return InputSystem::Instance().IsMouseButtonPressed(static_cast<MouseButton>(button));
	}

	// Native_Input_GetMousePosition is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Input_GetMousePosition(double* outX, double* outY) {
		auto [x, y] = InputSystem::Instance().GetCursorPosition();
		if (outX) *outX = x;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outY) *outY = y;
	}

	static NativeRuntimeStatus Native_Input_ReadSnapshot(NativeInputSnapshot* snapshot) {
		if(snapshot == nullptr || !HasRuntimeAbiHeader(snapshot->header, sizeof(*snapshot))) return NativeRuntimeStatus::VersionMismatch;
		static_assert(KeyCount() <= kNativeInputKeyCapacity);
		static_assert(MouseButtonCount() <= kNativeInputMouseButtonCapacity);
		const InputSystem::InputSnapshot source = InputSystem::Instance().GetLatestWorkerSnapshot();
		snapshot->header = {kNativeRuntimeAbiVersion, sizeof(*snapshot)};
		snapshot->status = NativeRuntimeStatus::Success;
		snapshot->keyCount = static_cast<std::uint32_t>(KeyCount());
		snapshot->mouseButtonCount = static_cast<std::uint32_t>(MouseButtonCount());
		snapshot->workerReadable = source.workerReadable ? 1U : 0U;
		snapshot->frameRevision = source.frameRevision;
		snapshot->cursorX = source.cursorPosition.first;
		snapshot->cursorY = source.cursorPosition.second;
		snapshot->scrollX = source.scrollDelta.first;
		snapshot->scrollY = source.scrollDelta.second;
		std::memset(snapshot->keyStates, 0, sizeof(snapshot->keyStates));
		std::memset(snapshot->mouseButtonStates, 0, sizeof(snapshot->mouseButtonStates));
		std::memset(snapshot->mousePressedThisFrame, 0, sizeof(snapshot->mousePressedThisFrame));
		for(std::size_t index = 0U; index < source.keyStates.size(); ++index) snapshot->keyStates[index] = static_cast<std::uint8_t>(source.keyStates[index]);
		for(std::size_t index = 0U; index < source.mouseButtonStates.size(); ++index) {
			snapshot->mouseButtonStates[index] = static_cast<std::uint8_t>(source.mouseButtonStates[index]);
			snapshot->mousePressedThisFrame[index] = source.mouseButtonPressedThisFrame[index] ? 1U : 0U;
		}
		WriteRuntimeDiagnostic(snapshot->diagnostic, source.boundarySummary);
		return snapshot->status;
	}

	static void Native_Input_SetMousePosition(double x, double y) {
		if(std::isfinite(x) && std::isfinite(y)) InputSystem::Instance().SetCursorPosition(x, y);
	}

	// Native_UI_WasElementClicked is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_UI_WasElementClicked(const char* elementId) {
		if (elementId == nullptr || elementId[0] == '\0') {
			return false;
		}

		return UISystem::Instance().WasElementClicked(elementId);
	}

	// Native_UI_IsElementHovered is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_UI_IsElementHovered(const char* elementId) {
		if (elementId == nullptr || elementId[0] == '\0') {
			return false;
		}

		return UISystem::Instance().IsElementHovered(elementId);
	}

	// Native_UI_GetInteractionFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::uint64_t Native_UI_GetInteractionFrame() {
		return UISystem::Instance().GetInteractionFrame();
	}

	// Native_UI_CreateElement is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void* Native_UI_CreateElement(int elementType) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr element = UIElement::CreateDefault(static_cast<UIElementType>(elementType));
		return CreateUIHandle(element, true);
	}

	// Native_UI_ReleaseHandle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_ReleaseHandle(void* elementHandle) {
		if (elementHandle == nullptr) {
			return;
		}

		std::lock_guard<std::mutex> lock(g_UIHandleMutex);
		ManagedUIElementHandle* handle = static_cast<ManagedUIElementHandle*>(elementHandle);
		ForgetUIHandleIndexLocked(handle);
		delete handle;
	}

	// Native_UI_GetRootElement is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void* Native_UI_GetRootElement(uint32_t objectId) {
		ObjectManager& objects = ObjectManager::instance();
		const entt::entity entity = static_cast<entt::entity>(objectId);
		{
			std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (std::optional<Object> object = objects.getObject(entity)) {
				UIComponent* component = objects.GetComponent<UIComponent>(*object);
				if (component == nullptr) {
					return nullptr;
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (UIElement::Ptr root = component->GetRoot()) {
					return CreateUIHandle(root, false);
				}
			}
			else {
				return nullptr;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::optional<Object> object = objects.getObject(entity)) {
			if (UIComponent* component = objects.GetComponent<UIComponent>(*object)) {
				UISystem::Instance().EnsureDocumentReady(entity, *component);
			}
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::optional<Object> object = objects.getObject(entity)) {
			UIComponent* component = objects.GetComponent<UIComponent>(*object);
			if (component == nullptr) {
				return nullptr;
			}

			UIElement::Ptr root = component->GetRoot();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!root) {
				root = UIElement::Create(UIElementType::Root);
				root->typeName = "UIRoot";
				component->SetRoot(root);
				component->MarkDirty();
			}

			return CreateUIHandle(root, false);
		}

		return nullptr;
	}

	// Native_UI_FindElementById is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void* Native_UI_FindElementById(void* elementHandle, const char* elementId) {
		if (elementId == nullptr || elementId[0] == '\0') {
			return nullptr;
		}

		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr element = ResolveUIHandle(elementHandle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!element) {
			return nullptr;
		}

		return CreateUIHandle(element->FindById(elementId), false);
	}

	// Native_UI_GetElementType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	int Native_UI_GetElementType(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			return static_cast<int>(element->type);
		}

		return static_cast<int>(UIElementType::VisualElement);
	}

	// Native_UI_IsHandleAlive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_UI_IsHandleAlive(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		return ResolveUIHandle(elementHandle) != nullptr;
	}

	// Native_UI_GetElementId is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_UI_GetElementId(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			return WriteUIBridgeReturnText(element->id);
		}

		return WriteUIBridgeReturnText({});
	}

	// Native_UI_SetElementId is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_SetElementId(void* elementHandle, const char* elementId) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			element->id = elementId != nullptr ? elementId : "";
			element->MarkDirty();
		}
	}

	// Native_UI_GetElementText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_UI_GetElementText(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			return WriteUIBridgeReturnText(element->text);
		}

		return WriteUIBridgeReturnText({});
	}

	// Native_UI_SetElementText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_SetElementText(void* elementHandle, const char* text) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			element->text = text != nullptr ? text : "";
			element->MarkDirty();
		}
	}

	// Native_UI_GetElementVisible is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_UI_GetElementVisible(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			return element->IsVisible();
		}

		return false;
	}

	// Native_UI_SetElementVisible is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_SetElementVisible(void* elementHandle, bool visible) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			element->SetVisible(visible);
		}
	}

	// Native_UI_AddChild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_AddChild(void* parentHandle, void* childHandle) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr parent = ResolveUIHandle(parentHandle);
		UIElement::Ptr child = ResolveUIHandle(childHandle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!parent || !child) {
			return;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (parent->InsertChild(parent->children.size(), child)) {
			DropDetachedUIOwnership(child);
		}
	}

	// Native_UI_RemoveChild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_RemoveChild(void* parentHandle, void* childHandle) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr parent = ResolveUIHandle(parentHandle);
		UIElement::Ptr child = ResolveUIHandle(childHandle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!parent || !child) {
			return;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (parent->RemoveChild(child)) {
			PromoteDetachedUIOwnership(child);
		}
	}

	// Native_UI_ClearChildren is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_ClearChildren(void* parentHandle) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr parent = ResolveUIHandle(parentHandle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!parent) {
			return;
		}

		std::vector<UIElement::Ptr> detachedChildren = parent->children;
		parent->ClearChildren();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const UIElement::Ptr& child : detachedChildren) {
			PromoteDetachedUIOwnership(child);
		}
	}

	// Native_UI_SetStyleFloat is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_SetStyleFloat(void* elementHandle, int property, float value) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			// Dispatch by explicit engine state so each case documents one supported behavior.
			switch (static_cast<ManagedUIStyleProperty>(property)) {
			case ManagedUIStyleProperty::Width:
				element->inlineStyle.width = UIValue::Points(value);
				element->computedStyle.width = element->inlineStyle.width;
				break;
			case ManagedUIStyleProperty::Height:
				element->inlineStyle.height = UIValue::Points(value);
				element->computedStyle.height = element->inlineStyle.height;
				break;
			default:
				return;
			}

			RefreshManagedElementStyle(*element);
		}
	}

	// Native_UI_GetStyleFloat is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	float Native_UI_GetStyleFloat(void* elementHandle, int property) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			// Dispatch by explicit engine state so each case documents one supported behavior.
			switch (static_cast<ManagedUIStyleProperty>(property)) {
			case ManagedUIStyleProperty::Width:
				return element->displayStyle.width.has_value() ? element->displayStyle.width->value : 0.0f;
			case ManagedUIStyleProperty::Height:
				return element->displayStyle.height.has_value() ? element->displayStyle.height->value : 0.0f;
			default:
				return 0.0f;
			}
		}

		return 0.0f;
	}

	// Native_UI_SetStyleColor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_SetStyleColor(void* elementHandle, int property, float x, float y, float z, float w) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			const glm::vec4 color(x, y, z, w);
			// Dispatch by explicit engine state so each case documents one supported behavior.
			switch (static_cast<ManagedUIStyleProperty>(property)) {
			case ManagedUIStyleProperty::Color:
				element->inlineStyle.color = color;
				element->computedStyle.color = color;
				break;
			case ManagedUIStyleProperty::BackgroundColor:
				element->inlineStyle.backgroundColor = color;
				element->computedStyle.backgroundColor = color;
				break;
			default:
				return;
			}

			RefreshManagedElementStyle(*element);
		}
	}

	// Native_UI_GetStyleColor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_GetStyleColor(void* elementHandle, int property, float* outX, float* outY, float* outZ, float* outW) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		std::array<float*, 4> outputs = { outX, outY, outZ, outW };
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (float* output : outputs) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (output != nullptr) {
				*output = 0.0f;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			std::optional<glm::vec4> color;
			switch (static_cast<ManagedUIStyleProperty>(property)) {
			case ManagedUIStyleProperty::Color:
				color = element->displayStyle.color;
				break;
			case ManagedUIStyleProperty::BackgroundColor:
				color = element->displayStyle.backgroundColor;
				break;
			default:
				return;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (color.has_value()) {
				if (outX) *outX = color->x;
				if (outY) *outY = color->y;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (outZ) *outZ = color->z;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (outW) *outW = color->w;
			}
		}
	}

	// Native_UI_SetEventCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_UI_SetEventCallback(void* elementHandle, int eventType, bool enabled) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr element = ResolveUIHandle(elementHandle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!element) {
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ResolveManagedUIEventDispatcher() == nullptr) {
			return false;
		}

		UIElement::UIEventCallback callback = nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (enabled) {
			// Dispatch by explicit engine state so each case documents one supported behavior.
			switch (static_cast<ManagedUIEventType>(eventType)) {
			case ManagedUIEventType::Click:
				callback = &DispatchManagedClickEvent;
				break;
			case ManagedUIEventType::MouseEnter:
				callback = &DispatchManagedMouseEnterEvent;
				break;
			case ManagedUIEventType::MouseLeave:
				callback = &DispatchManagedMouseLeaveEvent;
				break;
			default:
				return false;
			}
		}

		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (static_cast<ManagedUIEventType>(eventType)) {
		case ManagedUIEventType::Click:
			element->OnClick = callback;
			break;
		case ManagedUIEventType::MouseEnter:
			element->OnMouseEnter = callback;
			break;
		case ManagedUIEventType::MouseLeave:
			element->OnMouseLeave = callback;
			break;
		default:
			return false;
		}

		return true;
	}

	static NativeRuntimeStatus Native_Scene_RequestLoad(const char* path, bool prefab) {
		if(path == nullptr || path[0] == '\0') return NativeRuntimeStatus::InvalidArgument;
		// The managed string is copied now; filesystem access and cache mutation
		// are deferred to FlushManagedRuntimeRequests at the frame-end boundary.
		std::scoped_lock lock(g_ManagedRuntimeRequestMutex);
		g_PendingManagedSceneLoads.push_back({path, prefab});
		return NativeRuntimeStatus::Accepted;
	}

	static NativeRuntimeStatus Native_Scene_RequestSetActive(const char* uuidHex) {
		if(uuidHex == nullptr || uuidHex[0] == '\0') return NativeRuntimeStatus::InvalidArgument;
		const std::optional<TerraEngine::UUID> id = TerraEngine::ParseUUID(uuidHex);
		if(!id) return NativeRuntimeStatus::InvalidArgument;
		if(SceneManager::Instance().GetScene(*id) == nullptr) return NativeRuntimeStatus::NotFound;
		std::scoped_lock lock(g_ManagedRuntimeRequestMutex);
		g_PendingManagedActiveScene = *id;
		return NativeRuntimeStatus::Accepted;
	}

	static NativeRuntimeStatus Native_ResourceStreaming_ReadStatus(const char* assetReference, int assetType, NativeResourceStreamingStatus* status) {
		if(status == nullptr || !HasRuntimeAbiHeader(status->header, sizeof(*status))) return NativeRuntimeStatus::VersionMismatch;
		status->header = {kNativeRuntimeAbiVersion, sizeof(*status)};
		status->status = NativeRuntimeStatus::InvalidArgument;
		status->assetType = static_cast<std::uint32_t>(assetType);
		status->payloadState = 0U;
		status->resident = 0U;
		status->cpuBytes = 0U;
		status->gpuBytes = 0U;
		status->generation = 0U;
		WriteRuntimeDiagnostic(status->diagnostic, "Asset reference and type are required.");
		if(assetReference == nullptr || assetReference[0] == '\0') return status->status;
		const std::optional<AssetType> type = ManagedAssetType(assetType);
		if(!type) return status->status;
		ResourceManager& resources = ResourceManager::Instance();
		const TerraEngine::UUID id = resources.ResolveAssetReference(assetReference, *type);
		if(id == TerraEngine::EMPTY_UUID) {
			status->status = NativeRuntimeStatus::NotFound;
			WriteRuntimeDiagnostic(status->diagnostic, "Asset reference could not be resolved.");
			return status->status;
		}
		const std::optional<TerraEngine::Resource::StreamableAssetRecord> record = resources.GetStreamableAsset(id);
		status->resident = resources.IsResourceResident(id) ? 1U : 0U;
		if(record) {
			status->payloadState = static_cast<std::uint32_t>(record->payloadState);
			status->cpuBytes = record->exactCpuBytes.status == TerraEngine::Resource::ExactResidencyByteStatus::Unavailable
				? record->estimatedCpuBytes : record->exactCpuBytes.bytes;
			status->gpuBytes = record->exactGpuBytes.status == TerraEngine::Resource::ExactResidencyByteStatus::Unavailable
				? record->estimatedGpuBytes : record->exactGpuBytes.bytes;
			status->generation = record->residencyGeneration;
			WriteRuntimeDiagnostic(status->diagnostic, record->failureReason);
		} else {
			WriteRuntimeDiagnostic(status->diagnostic, status->resident != 0U ? "Resource is resident but has no visibility-streaming record." : "Resource is registered but not resident.");
		}
		status->status = NativeRuntimeStatus::Success;
		return status->status;
	}

	static NativeRuntimeStatus Native_ResourceStreaming_RequestRelease(const char* assetReference, int assetType) {
		if(assetReference == nullptr || assetReference[0] == '\0') return NativeRuntimeStatus::InvalidArgument;
		const std::optional<AssetType> type = ManagedAssetType(assetType);
		if(!type) return NativeRuntimeStatus::InvalidArgument;
		ResourceManager& resources = ResourceManager::Instance();
		const TerraEngine::UUID id = resources.ResolveAssetReference(assetReference, *type);
		if(id == TerraEngine::EMPTY_UUID) return NativeRuntimeStatus::NotFound;
		std::scoped_lock lock(g_ManagedRuntimeRequestMutex);
		const bool alreadyQueued = std::any_of(g_PendingManagedResourceReleases.begin(), g_PendingManagedResourceReleases.end(), [&](const ManagedResourceReleaseRequest& request) {
			return request.assetId == id && request.assetType == *type;
		});
		if(!alreadyQueued) g_PendingManagedResourceReleases.push_back({id, *type});
		return NativeRuntimeStatus::Accepted;
	}

	static const char* Native_Scene_GetActiveScene() {
		g_RuntimeBridgeReturnBuffer = TerraEngine::ToHexString(SceneManager::Instance().GetActiveSceneId());
		return g_RuntimeBridgeReturnBuffer.c_str();
	}

	static const char* Native_Scene_GetLoadedScenes() {
		nlohmann::json values = nlohmann::json::array();
		for(const TerraEngine::UUID& id : SceneManager::Instance().GetLoadedSceneIds()) values.push_back(TerraEngine::ToHexString(id));
		g_RuntimeBridgeReturnBuffer = values.dump();
		return g_RuntimeBridgeReturnBuffer.c_str();
	}

	static NativeRuntimeStatus Native_Scene_InstantiatePrefab(const char* path, uint32_t parentObjectId) {
		if(path == nullptr || path[0] == '\0') return NativeRuntimeStatus::InvalidArgument;
		TerraEngine::UUID parentId = TerraEngine::EMPTY_UUID;
		if(parentObjectId != InvalidObjectId()) {
			const std::optional<Object> parent = ObjectManager::instance().getObject(static_cast<entt::entity>(parentObjectId));
			if(!parent) return NativeRuntimeStatus::NotFound;
			parentId = ObjectManager::instance().getUUID(*parent);
		}
		// Resolution and cache publication happen at the protected frame boundary.
		// A managed worker only copies the path and stable parent UUID here.
		std::scoped_lock lock(g_ManagedRuntimeRequestMutex);
		g_PendingManagedPrefabInstantiations.push_back({path, parentId});
		return NativeRuntimeStatus::Accepted;
	}

	static NativeRuntimeStatus Native_Scene_RequestUnloadAll() {
		std::scoped_lock lock(g_ManagedRuntimeRequestMutex);
		g_PendingManagedSceneUnloadAll = true;
		return NativeRuntimeStatus::Accepted;
	}

	static NativeRuntimeStatus Native_Scene_ReadRenderSettings(NativeSceneRenderSettings* settings) {
		if(settings == nullptr || !HasRuntimeAbiHeader(settings->header, sizeof(*settings))) return NativeRuntimeStatus::VersionMismatch;
		const SceneRenderSettings source = SceneManager::Instance().GetActiveSceneRenderSettings();
		settings->header = {kNativeRuntimeAbiVersion, sizeof(*settings)};
		settings->status = NativeRuntimeStatus::Success;
		settings->reserved = 0U;
		settings->indirectStrength = source.indirectStrength;
		settings->ambientOcclusionStrength = source.ambientOcclusionStrength;
		settings->ambientOcclusionQuality = source.ambientOcclusionQuality;
		settings->reserved2 = 0U;
		{
			std::scoped_lock lock(g_ManagedRuntimeRequestMutex);
			settings->generation = g_ManagedSceneRenderSettingsGeneration;
		}
		return settings->status;
	}

	static NativeRuntimeStatus Native_Scene_SubmitRenderSettings(const NativeSceneRenderSettings* settings) {
		if(settings == nullptr || !HasRuntimeAbiHeader(settings->header, sizeof(*settings))) return NativeRuntimeStatus::VersionMismatch;
		if(!std::isfinite(settings->indirectStrength) || !std::isfinite(settings->ambientOcclusionStrength)) return NativeRuntimeStatus::InvalidArgument;
		SceneRenderSettings candidate;
		candidate.indirectStrength = settings->indirectStrength;
		candidate.ambientOcclusionStrength = settings->ambientOcclusionStrength;
		candidate.ambientOcclusionQuality = settings->ambientOcclusionQuality;
		candidate = SanitizeSceneRenderSettings(candidate);
		std::scoped_lock lock(g_ManagedRuntimeRequestMutex);
		g_PendingManagedSceneRenderSettings = candidate;
		return NativeRuntimeStatus::Accepted;
	}

	// Native_Scene_SubmitTransactionJson is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Scene_SubmitTransactionJson(const char* transactionJson) {
		if (transactionJson == nullptr || transactionJson[0] == '\0') {
			return WriteSceneBridgeReturnText(R"({"ok":false,"error":"transactionJson is required"})");
		}

		try {
			TerraEngine::Scene::Transaction transaction = ParseManagedSceneTransaction(nlohmann::json::parse(transactionJson));
			const std::string transactionId = transaction.id;
			if (transaction.mode == TerraEngine::Scene::TransactionMode::PreflightOnly) {
				const TerraEngine::Scene::TransactionResult preflight = SceneManager::Instance().PreflightTransaction(transaction);
				nlohmann::json response{
					{ "ok", preflight.status == TerraEngine::Scene::TransactionStatus::PreflightAccepted },
					{ "transactionId", transactionId },
					{ "status", std::string(TerraEngine::Scene::ToString(preflight.status)) },
					{ "conflicts", preflight.conflicts.size() }
				};
				return WriteSceneBridgeReturnText(response.dump());
			}

			TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(std::move(transaction));
			nlohmann::json response{
				{ "ok", true },
				{ "transactionId", transactionId },
				{ "pending", TerraEngine::Scene::SceneMutationQueue::Instance().Size() }
			};
			return WriteSceneBridgeReturnText(response.dump());
		} catch (const std::exception& exception) {
			nlohmann::json response{
				{ "ok", false },
				{ "error", exception.what() }
			};
			return WriteSceneBridgeReturnText(response.dump());
		}
	}

	// Native_Time_GetDeltaTime is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	float Native_Time_GetDeltaTime() {
		return ScriptingSystem::Instance().GetLastDeltaTime();
	}

	static NativeRuntimeStatus Native_Time_ReadSnapshot(NativeTimeSnapshot* snapshot) {
		if(snapshot == nullptr || !HasRuntimeAbiHeader(snapshot->header, sizeof(*snapshot))) return NativeRuntimeStatus::VersionMismatch;
		const ScriptingSystem::RuntimeTimeSnapshot source = ScriptingSystem::Instance().GetRuntimeTimeSnapshot();
		snapshot->header = {kNativeRuntimeAbiVersion, sizeof(*snapshot)};
		snapshot->status = NativeRuntimeStatus::Success;
		snapshot->reserved = 0U;
		snapshot->deltaTime = source.deltaTime;
		snapshot->unscaledDeltaTime = source.unscaledDeltaTime;
		snapshot->fixedDeltaTime = TerraEngine::Physics::PhysicsSystem::Instance().FixedTimeStep();
		snapshot->timeScale = source.timeScale;
		snapshot->elapsedTime = source.elapsedTime;
		snapshot->unscaledElapsedTime = source.unscaledElapsedTime;
		snapshot->frameCount = source.frameCount;
		return snapshot->status;
	}

	static NativeRuntimeStatus Native_Time_SetTimeScale(float timeScale) {
		return ScriptingSystem::Instance().QueueRuntimeTimeScale(timeScale)
			? NativeRuntimeStatus::Accepted
			: NativeRuntimeStatus::InvalidArgument;
	}

	NativeRuntimeStatus Native_Runtime_Invoke(const NativeRuntimeRequest* request, void* packet) {
		if(request == nullptr || !HasRuntimeAbiHeader(request->header, sizeof(*request))) return NativeRuntimeStatus::VersionMismatch;
		const auto physicsStatus = [](NativePhysicsStatus status) {
			switch(status) {
			case NativePhysicsStatus::Success: return NativeRuntimeStatus::Success;
			case NativePhysicsStatus::Accepted: return NativeRuntimeStatus::Accepted;
			case NativePhysicsStatus::InvalidArgument: return NativeRuntimeStatus::InvalidArgument;
			case NativePhysicsStatus::InvalidState: return NativeRuntimeStatus::InvalidState;
			case NativePhysicsStatus::VersionMismatch: return NativeRuntimeStatus::VersionMismatch;
			default: return NativeRuntimeStatus::Conflict;
			}
		};
		// Every operation copies through caller-owned fixed-width packets. Pointer
		// fields are valid only for this synchronous dispatch and are never queued.
		switch(request->operation) {
		case NativeRuntimeOperation::ObjectGetParent:
			if(packet == nullptr) return NativeRuntimeStatus::InvalidArgument;
			*static_cast<std::uint32_t*>(packet) = Native_Object_GetParent(request->objectId);
			return NativeRuntimeStatus::Success;
		case NativeRuntimeOperation::ObjectSetParent:
			return Native_Object_SetParent(request->objectId, request->secondaryId);
		case NativeRuntimeOperation::ObjectCopyChildren:
			return Native_Object_CopyChildren(request->objectId, static_cast<std::uint32_t*>(request->output), request->capacity, request->requiredCount);
		case NativeRuntimeOperation::ObjectCopyAll:
			return Native_Object_CopyAll(static_cast<std::uint32_t*>(request->output), request->capacity, request->requiredCount);
		case NativeRuntimeOperation::ObjectCopyWithTag:
			return Native_Object_CopyWithTag(request->text, static_cast<std::uint32_t*>(request->output), request->capacity, request->requiredCount);
		case NativeRuntimeOperation::ObjectRequestCreate:
			return Native_Object_RequestCreate(request->text, static_cast<NativeObjectCreationReceipt*>(packet));
		case NativeRuntimeOperation::ObjectResolveStableId:
			return Native_Object_ResolveStableId(request->text, static_cast<std::uint32_t*>(packet));
		case NativeRuntimeOperation::InputReadSnapshot:
			return Native_Input_ReadSnapshot(static_cast<NativeInputSnapshot*>(packet));
		case NativeRuntimeOperation::InputSetMousePosition:
			Native_Input_SetMousePosition(request->x, request->y);
			return NativeRuntimeStatus::Success;
		case NativeRuntimeOperation::TimeReadSnapshot:
			return Native_Time_ReadSnapshot(static_cast<NativeTimeSnapshot*>(packet));
		case NativeRuntimeOperation::TimeSetScale:
			return Native_Time_SetTimeScale(request->scalar);
		case NativeRuntimeOperation::SceneGetActive:
			if(packet == nullptr) return NativeRuntimeStatus::InvalidArgument;
			*static_cast<const char**>(packet) = Native_Scene_GetActiveScene();
			return NativeRuntimeStatus::Success;
		case NativeRuntimeOperation::SceneGetLoaded:
			if(packet == nullptr) return NativeRuntimeStatus::InvalidArgument;
			*static_cast<const char**>(packet) = Native_Scene_GetLoadedScenes();
			return NativeRuntimeStatus::Success;
		case NativeRuntimeOperation::SceneRequestLoad:
			return Native_Scene_RequestLoad(request->text, false);
		case NativeRuntimeOperation::SceneRequestLoadPrefab:
			return Native_Scene_RequestLoad(request->text, true);
		case NativeRuntimeOperation::SceneRequestSetActive:
			return Native_Scene_RequestSetActive(request->text);
		case NativeRuntimeOperation::SceneInstantiatePrefab:
			return Native_Scene_InstantiatePrefab(request->text, request->objectId);
		case NativeRuntimeOperation::SceneUnloadAll:
			return Native_Scene_RequestUnloadAll();
		case NativeRuntimeOperation::SceneReadRenderSettings:
			return Native_Scene_ReadRenderSettings(static_cast<NativeSceneRenderSettings*>(packet));
		case NativeRuntimeOperation::SceneSubmitRenderSettings:
			return Native_Scene_SubmitRenderSettings(static_cast<const NativeSceneRenderSettings*>(packet));
		case NativeRuntimeOperation::ResourceReadStatus:
			return Native_ResourceStreaming_ReadStatus(request->text, static_cast<int>(request->assetType), static_cast<NativeResourceStreamingStatus*>(packet));
		case NativeRuntimeOperation::ResourceRequestRelease:
			return Native_ResourceStreaming_RequestRelease(request->text, static_cast<int>(request->assetType));
		case NativeRuntimeOperation::PhysicsReadSettings:
			return physicsStatus(Native_Physics_ReadSettingsRecord(static_cast<NativePhysicsSettingsRecord*>(packet)));
		case NativeRuntimeOperation::PhysicsSubmitSettings:
			return physicsStatus(Native_Physics_SubmitSettingsRecord(static_cast<const NativePhysicsSettingsRecord*>(packet)));
		default:
			return NativeRuntimeStatus::InvalidArgument;
		}
	}

}
