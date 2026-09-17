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
#include "Animation/AnimationSystem.h"
#include "Audio/AudioSystem.h"
#include "Animation/Runtime/AnimationAttachments.h"
#include "Animation/Runtime/RootMotion.h"
#include "DotNet/DotNetHost.h"
#include "ScriptingSystem.h"
#include "UI/UIElement.h"
#include "UI/UISystem.h"
#include "Input/InputSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Navigation/NavigationSystem.h"
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
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
	template<typename T>
	bool IsAudioAbiRecord(const T* value) {
		return value != nullptr
			&& value->header.abiVersion == kNativeAudioAbiVersion
			&& value->header.structSize == sizeof(T);
	}

	void CopyAudioDiagnostic(char* destination, std::size_t capacity, const std::string& source) {
		if(destination == nullptr || capacity == 0U) return;
		const std::size_t count = std::min(capacity - 1U, source.size());
		std::memcpy(destination, source.data(), count);
		destination[count] = '\0';
	}

	NativeAudioStatus ToNativeAudioStatus(TerraEngine::Audio::AudioControlStatus status) {
		switch(status) {
		case TerraEngine::Audio::AudioControlStatus::Queued: return NativeAudioStatus::Queued;
		case TerraEngine::Audio::AudioControlStatus::Accepted: return NativeAudioStatus::Accepted;
		case TerraEngine::Audio::AudioControlStatus::Applied: return NativeAudioStatus::Success;
		case TerraEngine::Audio::AudioControlStatus::Rejected: return NativeAudioStatus::InvalidArgument;
		}
		return NativeAudioStatus::ProviderFailure;
	}
}
#include <vector>

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

	NativeAnimationStatus ToNativeAnimationStatus(TerraEngine::Animation::AnimationStatus status) noexcept
	{
		return static_cast<NativeAnimationStatus>(status);
	}

	NativeAnimationStatus ValidateAnimationHeader(
		const NativeAnimationAbiHeader* header,
		NativeAnimationRecordKind expected,
		std::size_t requiredSize) noexcept
	{
		if(header == nullptr) return NativeAnimationStatus::InvalidArgument;
		if(header->version != kNativeAnimationAbiVersion) return NativeAnimationStatus::UnsupportedMajorVersion;
		if(header->size < requiredSize) return NativeAnimationStatus::TruncatedInput;
		if(header->kind != static_cast<std::uint16_t>(expected)) return NativeAnimationStatus::InvalidType;
		return NativeAnimationStatus::Ok;
	}

	template<typename Record>
	NativeAnimationStatus ValidateAnimationRecord(
		const Record* record,
		NativeAnimationRecordKind expected) noexcept
	{
		return ValidateAnimationHeader(record == nullptr ? nullptr : &record->header, expected, sizeof(Record));
	}

	NativeAnimationUuid AnimationFromUuid(const TerraEngine::UUID& value) noexcept
	{
		NativeAnimationUuid result{};
		std::memcpy(&result.first, value.data(), sizeof(result.first));
		std::memcpy(&result.second, value.data() + sizeof(result.first), sizeof(result.second));
		return result;
	}

	NativeAnimationAnimatorHandle AnimationFromHandle(
		TerraEngine::Animation::AnimatorInstanceHandle value) noexcept
	{
		return {value.slotIndex, value.generation};
	}

	TerraEngine::Animation::AnimatorInstanceHandle AnimationToHandle(
		NativeAnimationAnimatorHandle value) noexcept
	{
		return {value.slotIndex, value.generation};
	}

	NativeAnimationVector3 AnimationFromVector(
		TerraEngine::Animation::AnimationFloat3 value) noexcept
	{
		return {value.x, value.y, value.z};
	}

	TerraEngine::Animation::AnimationFloat3 AnimationToVector(
		NativeAnimationVector3 value) noexcept
	{
		return {value.x, value.y, value.z};
	}

	NativeAnimationQuaternion AnimationFromQuaternion(
		TerraEngine::Animation::AnimationQuaternion value) noexcept
	{
		return {value.x, value.y, value.z, value.w};
	}

	TerraEngine::Animation::AnimationQuaternion AnimationToQuaternion(
		NativeAnimationQuaternion value) noexcept
	{
		return {value.x, value.y, value.z, value.w};
	}

	NativeAnimationTransform AnimationFromTransform(
		const TerraEngine::Animation::AnimationTransform& value) noexcept
	{
		return {
			AnimationFromVector(value.translation),
			AnimationFromQuaternion(value.rotation),
			AnimationFromVector(value.scale)};
	}

	TerraEngine::Animation::AnimationTransform AnimationToTransform(
		const NativeAnimationTransform& value) noexcept
	{
		return {
			AnimationToVector(value.translation),
			AnimationToQuaternion(value.rotation),
			AnimationToVector(value.scale)};
	}

	NativeAnimationAbiHeader AnimationHeader(
		NativeAnimationRecordKind kind,
		std::size_t size) noexcept
	{
		return {
			static_cast<std::uint32_t>(size),
			kNativeAnimationAbiVersion,
			static_cast<std::uint16_t>(kind)};
	}

	void FillAnimationReceipt(
		const TerraEngine::Animation::AnimationCommandReceipt& source,
		NativeAnimationCommandReceipt& destination) noexcept
	{
		destination = {};
		destination.header = AnimationHeader(
			NativeAnimationRecordKind::CommandReceipt, sizeof(destination));
		destination.status = ToNativeAnimationStatus(source.status);
		destination.animator = AnimationFromHandle(source.target);
		destination.producerId = source.producerId;
		destination.duplicate = source.duplicate ? 1U : 0U;
		destination.producerSequence = source.producerSequence;
		destination.correlationId = source.correlationId;
		destination.idempotencyKey = source.idempotencyKey;
		destination.cancellationToken = source.cancellationToken;
		destination.targetTick = source.targetTick;
		destination.observableTick = source.observableTick;
		destination.publicationGeneration = source.publicationGeneration;
	}

	void FillRootMotionReceipt(
		const TerraEngine::Animation::AnimationRootMotionApplicationReceipt& source,
		NativeAnimationRootMotionApplicationReceipt& destination) noexcept
	{
		destination = {};
		destination.header = AnimationHeader(
			NativeAnimationRecordKind::RootMotionApplicationReceipt, sizeof(destination));
		destination.status = ToNativeAnimationStatus(source.status);
		destination.animator = AnimationFromHandle(source.animator);
		destination.mode = static_cast<NativeAnimationRootMotionApplicationMode>(source.mode);
		destination.generation = source.generation;
		destination.producerId = source.producerId;
		destination.duplicate = source.duplicate ? 1U : 0U;
		destination.idempotencyConflict = source.idempotencyConflict ? 1U : 0U;
		destination.producerSequence = source.producerSequence;
		destination.correlationId = source.correlationId;
		destination.idempotencyKey = source.idempotencyKey;
		destination.publicationGeneration = source.publicationGeneration;
		destination.observableTick = source.observableTick;
		destination.physicsCommandId = source.physicsCommandId;
	}

	void FillAnimationBufferInfo(
		NativeAnimationBufferInfo& info,
		NativeAnimationStatus status,
		std::size_t required,
		std::size_t written,
		std::int64_t sourceTick,
		std::uint64_t publicationGeneration,
		bool overflowed = false,
		std::uint32_t droppedLowerBound = 0U) noexcept
	{
		info = {};
		info.header = AnimationHeader(NativeAnimationRecordKind::BufferInfo, sizeof(info));
		info.status = status;
		info.requiredCount = required > std::numeric_limits<std::uint32_t>::max()
			? std::numeric_limits<std::uint32_t>::max()
			: static_cast<std::uint32_t>(required);
		info.writtenCount = written > std::numeric_limits<std::uint32_t>::max()
			? std::numeric_limits<std::uint32_t>::max()
			: static_cast<std::uint32_t>(written);
		info.overflowed = overflowed ? 1U : 0U;
		info.droppedCountLowerBound = droppedLowerBound;
		info.sourceTick = sourceTick;
		info.publicationGeneration = publicationGeneration;
	}

	template<typename NativeValue, typename SourceValue, typename Converter>
	NativeAnimationStatus CopyAnimationSpan(
		std::span<const SourceValue> source,
		NativeValue* destination,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info,
		std::int64_t sourceTick,
		std::uint64_t publicationGeneration,
		Converter&& converter)
	{
		const NativeAnimationStatus infoStatus = ValidateAnimationRecord(
			info, NativeAnimationRecordKind::BufferInfo);
		if(infoStatus != NativeAnimationStatus::Ok) return infoStatus;
		if(source.size() > std::numeric_limits<std::uint32_t>::max()) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::HardLimitExceeded,
				source.size(), 0U, sourceTick, publicationGeneration, true, 1U);
			return info->status;
		}
		if(source.size() > capacity) {
			// The two-call contract is atomic: an undersized buffer receives no
			// prefix, while requiredCount reports the exact complete size.
			FillAnimationBufferInfo(*info, NativeAnimationStatus::BufferTooSmall,
				source.size(), 0U, sourceTick, publicationGeneration);
			return info->status;
		}
		if(!source.empty() && destination == nullptr) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::InvalidArgument,
				source.size(), 0U, sourceTick, publicationGeneration);
			return info->status;
		}
		for(std::size_t index = 0U; index < source.size(); ++index) {
			destination[index] = converter(source[index], static_cast<std::uint32_t>(index));
		}
		FillAnimationBufferInfo(*info, NativeAnimationStatus::Ok,
			source.size(), source.size(), sourceTick, publicationGeneration);
		return info->status;
	}

	const TerraEngine::Animation::AnimationAuthorityRow* LoadAuthorityRow(
		TerraEngine::Animation::AnimatorInstanceHandle animator,
		std::shared_ptr<const TerraEngine::Animation::AnimationAuthorityFrame>& authority)
	{
		authority = TerraEngine::Animation::AnimationSystem::Instance().LoadPublishedAuthorityFrame();
		return authority && authority->IsValid() ? authority->Find(animator) : nullptr;
	}

	bool CopyOrMaterializeCurrentSocketSnapshot(
		TerraEngine::Animation::AnimatorInstanceHandle animator,
		const TerraEngine::Animation::AnimationAuthorityRow* published,
		TerraEngine::Animation::AnimationSocketReadSnapshot& output)
	{
		using namespace TerraEngine::Animation;
		AnimationAttachmentSnapshotPublisher& publisher =
			AnimationAttachmentSnapshotPublisher::Instance();
		AnimationSocketReadSnapshot candidate;
		const bool copied = publisher.CopySocketSnapshot(animator, candidate);
		const auto isCurrent = [&](const AnimationSocketReadSnapshot& snapshot) {
			return published && published->snapshot && published->snapshot->pose
				&& snapshot.animator == animator
			&& snapshot.poseGeneration == published->poseGeneration
			&& snapshot.publicationGeneration == published->poseGeneration
				&& snapshot.sourceTick == published->snapshot->tick
				&& snapshot.skeleton == published->skeleton;
		};
		if(copied && isCurrent(candidate)) {
			output = std::move(candidate);
			return true;
		}

		// The first explicit managed read doubles as a subscription. Materialize
		// its initial result from the already sealed authority publication so the
		// API does not impose an artificial one-frame delay; later protected frames
		// refresh only this requested animator rather than the entire crowd.
		publisher.RetireSnapshot(animator);
		if(!published || !published->snapshot || !published->snapshot->pose
			|| !published->skeletonAsset || !published->sourceObjectWorldTransformCaptured) {
			return false;
		}
		AnimationBoneSocketSnapshotRequest request;
		request.animator = animator;
		request.sourceObjectId = published->ownerObjectId;
		request.sourceObjectGeneration = published->objectGeneration;
		request.sourceComponentGeneration = published->componentGeneration;
		request.sourceTick = published->snapshot->tick;
		request.publicationGeneration = published->poseGeneration;
		request.sourceObjectWorldTransform = published->sourceObjectWorldTransform;
		request.pose = published->snapshot->pose;
		request.skeleton = published->skeletonAsset;
		AnimationBoneSocketSnapshot snapshot;
		if(CaptureAnimationBoneSocketSnapshot(request, snapshot) != AnimationStatus::Ok
			|| !publisher.Publish(std::move(snapshot))) {
			return false;
		}
		candidate = {};
		if(!publisher.CopySocketSnapshot(animator, candidate) || !isCurrent(candidate)) return false;
		output = std::move(candidate);
		return true;
	}

	void CopyBoundedAnimationText(char* destination, std::size_t capacity, std::string_view source) noexcept
	{
		if(destination == nullptr || capacity == 0U) return;
		std::memset(destination, 0, capacity);
		const std::size_t count = std::min(source.size(), capacity - 1U);
		std::memcpy(destination, source.data(), count);
	}

	NativeAnimationEvent MakeNativeAnimationEvent(
		const TerraEngine::Animation::AnimationEventRecord& source,
		const TerraEngine::Animation::AnimationAuthorityFrame& authority) noexcept
	{
		using namespace TerraEngine::Animation;
		NativeAnimationEvent event{};
		event.header = AnimationHeader(NativeAnimationRecordKind::Event, sizeof(event));
		event.animator = AnimationFromHandle(source.identity.animator);
		event.clipAssetId = AnimationFromUuid(source.identity.clipAssetId.value);
		event.controllerStateId = source.identity.stateInstanceId;
		event.sourceStateId = source.sourceStateId;
		event.eventId = source.identity.eventId;
		event.sourceTick = source.identity.sourceTick;
		event.loopCount = source.identity.loopCount;
		event.layerIndex = source.layerIndex;
		event.payloadType = static_cast<std::uint32_t>(source.payloadType);
		event.direction = static_cast<std::int32_t>(source.direction);
		event.historical = source.historical ? 1U : 0U;
		event.producerOrder = source.producerOrder;
		event.correlationId = source.correlationId;
		event.publicationGeneration = authority.authorityGeneration;
		if(const AnimationAuthorityRow* row = authority.Find(source.identity.animator)) {
			event.controllerAssetId = AnimationFromUuid(row->controller.assetId.value);
			event.controllerGeneration = row->controller.generation;
			event.clipGeneration = row->clip.generation;
			event.poseGeneration = row->poseGeneration;
		}
		const std::string tag = std::to_string(source.identity.eventId);
		CopyBoundedAnimationText(event.tag, sizeof(event.tag), tag);
		CopyBoundedAnimationText(event.payload, sizeof(event.payload), source.payload);
		return event;
	}

	NativeAnimationRootMotionResultDetails MakeNativeRootMotionDetails(
		const TerraEngine::Animation::AnimationRootMotionApplicationResult& source) noexcept
	{
		NativeAnimationRootMotionResultDetails result{};
		result.header = AnimationHeader(
			NativeAnimationRecordKind::RootMotionResultDetails, sizeof(result));
		result.status = ToNativeAnimationStatus(source.status);
		result.animator = AnimationFromHandle(source.animator);
		result.disposition = static_cast<NativeAnimationRootMotionDisposition>(source.disposition);
		result.generation = source.generation;
		result.applicationMode = static_cast<NativeAnimationRootMotionApplicationMode>(
			source.applicationMode);
		result.authoredPolicy = static_cast<std::uint32_t>(source.authoredPolicy);
		result.ownerObjectId = AnimationFromUuid(source.ownerObjectId);
		result.objectGeneration = source.objectGeneration;
		result.componentGeneration = source.componentGeneration;
		result.sourceBeginTick = source.sourceBeginTick;
		result.sourceEndTick = source.sourceEndTick;
		result.localTranslation = AnimationFromVector(source.localTranslation);
		result.requestedTranslation = AnimationFromVector(source.requestedTranslation);
		result.appliedTranslation = AnimationFromVector(source.appliedTranslation);
		result.localYawRadians = source.localYawRadians;
		result.requestedYawRadians = source.requestedYawRadians;
		result.appliedYawRadians = source.appliedYawRadians;
		result.layerContribution = source.layerContribution;
		result.requestedRotation = AnimationFromQuaternion(source.requestedRotation);
		result.appliedRotation = AnimationFromQuaternion(source.appliedRotation);
		result.sourceStateId = source.sourceStateId;
		result.accumulatedCycles = source.accumulatedCycles;
		result.layerIndex = source.layerIndex;
		result.producerId = source.producerId;
		result.producerSequence = source.producerSequence;
		result.correlationId = source.correlationId;
		result.idempotencyKey = source.idempotencyKey;
		result.publicationGeneration = source.publicationGeneration;
		result.physicsCommandId = source.physicsCommandId;
		result.duplicate = source.duplicate ? 1U : 0U;
		result.idempotencyConflict = source.idempotencyConflict ? 1U : 0U;
		return result;
	}

	template <typename Snapshot, typename CopyFunction>
	NativeAnimationStatus CopyAdvancedAnimationSnapshots(
		const NativeAnimationAnimatorHandle* animator,
		Snapshot* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info,
		CopyFunction&& copy)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus infoStatus = ValidateAnimationRecord(
			info, NativeAnimationRecordKind::BufferInfo);
		if(infoStatus != NativeAnimationStatus::Ok) return infoStatus;
		if(animator == nullptr || (capacity != 0U && values == nullptr)) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::InvalidArgument,
				0U, 0U, 0, AnimationSystem::Instance().AuthorityPublicationGeneration());
			return info->status;
		}
		std::uint32_t requiredCount = 0U;
		const AnimationStatus status = copy(
			AnimationToHandle(*animator), std::span<Snapshot>(values, capacity), requiredCount);
		const NativeAnimationStatus nativeStatus = ToNativeAnimationStatus(status);
		const bool copied = status == AnimationStatus::Ok;
		std::int64_t sourceTick = 0;
		std::uint64_t publicationGeneration =
			AnimationSystem::Instance().AuthorityPublicationGeneration();
		if(copied && requiredCount != 0U) {
			const Snapshot& newest = values[requiredCount - 1U];
			if constexpr(requires { newest.observationTick; }) {
				sourceTick = newest.observationTick;
			} else {
				sourceTick = newest.tick;
			}
			publicationGeneration = newest.publicationGeneration;
		}
		FillAnimationBufferInfo(*info, nativeStatus, requiredCount,
			copied ? requiredCount : 0U, sourceTick, publicationGeneration);
		return info->status;
	}

	constexpr bool HasMotionMatchUuid(NativeAnimationUuid value) noexcept
	{
		return value.first != 0U || value.second != 0U;
	}

	bool ValidateMotionMatchQueryPayload(
		const NativeAnimationMotionMatchQueryRequestV2& request) noexcept
	{
		constexpr std::uint32_t requiredFields =
			static_cast<std::uint32_t>(NativeAnimationMotionMatchQueryField::Identity)
			| static_cast<std::uint32_t>(NativeAnimationMotionMatchQueryField::Features)
			| static_cast<std::uint32_t>(NativeAnimationMotionMatchQueryField::FeatureWeights);
		if(request.animator.generation == 0U || !HasMotionMatchUuid(request.databaseAssetId)
			|| request.databaseGeneration == 0U || request.schemaFingerprint == 0U
			|| request.featureCount != kNativeAnimationMotionFeatureCount
			|| request.queryId == 0U || request.correlationId == 0U
			|| request.targetTick < 0 || request.reserved16 != 0U || request.reserved32 != 0U
			|| (request.queryFields & requiredFields) != requiredFields
			|| (request.queryFields & ~kNativeAnimationMotionMatchAllQueryFields) != 0U
			|| request.maximumCandidates == 0U
			|| request.maximumCandidates > kNativeAnimationMotionMaximumTopK
			|| request.maximumSamplesToVisit == 0U
			|| request.maximumSamplesToVisit > kNativeAnimationMotionMaximumSamples
			|| request.maximumDiagnosticExclusions
				> kNativeAnimationMotionMaximumExcludedSamples
			|| request.excludedSampleCount > kNativeAnimationMotionMaximumExcludedSamples
			|| (request.requiredTags & request.excludedTags) != 0U
			|| (request.requiredTransitionFlags & request.excludedTransitionFlags) != 0U) {
			return false;
		}
		const bool hasExclusions = (request.queryFields & static_cast<std::uint32_t>(
			NativeAnimationMotionMatchQueryField::Exclusions)) != 0U;
		if(!hasExclusions && request.excludedSampleCount != 0U) return false;
		std::uint64_t previous = 0U;
		for(std::uint32_t index = 0U;
			index < kNativeAnimationMotionMaximumExcludedSamples; ++index) {
			const std::uint64_t sampleId = request.excludedSampleIds[index];
			if(index < request.excludedSampleCount) {
				if(sampleId == 0U || (index != 0U && sampleId <= previous)) return false;
				previous = sampleId;
			} else if(sampleId != 0U) {
				// Zeroing unused inline entries makes equivalent copied requests byte
				// stable and prevents caller scratch data from crossing the boundary.
				return false;
			}
		}
		return true;
	}

	// Navigation ABI helpers convert every caller value into provider-neutral
	// engine records before NavigationSystem receives it. No managed packet can
	// retain a native pointer or mutate a route in place.
	template<typename Record>
	bool HasNavigationAbiRecord(const Record* record) noexcept {
		return record != nullptr && record->header.abiVersion == kNativeNavigationAbiVersion
			&& record->header.structSize == sizeof(Record);
	}

	void WriteNavigationDiagnostic(char (&destination)[kNativeNavigationDiagnosticBytes], std::string_view message) noexcept {
		std::memset(destination, 0, sizeof(destination));
		const std::size_t count = std::min(message.size(), sizeof(destination) - 1U);
		std::memcpy(destination, message.data(), count);
	}

	TerraEngine::Navigation::NavigationWorldHandle ToNavigationWorld(NativeNavigationWorldHandle value) noexcept {
		return {{value.worldId}, value.worldGeneration};
	}
	NativeNavigationWorldHandle FromNavigationWorld(TerraEngine::Navigation::NavigationWorldHandle value) noexcept {
		return {value.worldId.value, value.worldGeneration};
	}
	TerraEngine::Navigation::NavigationOwnerHandle ToNavigationOwner(NativeNavigationOwnerHandle value) noexcept {
		return {ToNavigationWorld(value.world), {value.ownerId}, value.ownerGeneration};
	}
	NativeNavigationOwnerHandle FromNavigationOwner(TerraEngine::Navigation::NavigationOwnerHandle value) noexcept {
		return {FromNavigationWorld(value.world), value.ownerId.value, value.ownerGeneration};
	}
	TerraEngine::Navigation::NavigationRequestHandle ToNavigationRequest(NativeNavigationRequestHandle value) noexcept {
		return {ToNavigationOwner(value.owner), value.slotIndex, value.slotGeneration};
	}
	TerraEngine::Navigation::NavigationRouteHandle ToNavigationRoute(NativeNavigationRouteHandle value) noexcept {
		return {ToNavigationOwner(value.owner), value.slotIndex, value.slotGeneration};
	}
	NativeNavigationRequestHandle FromNavigationRequest(TerraEngine::Navigation::NavigationRequestHandle value) noexcept {
		return {FromNavigationOwner(value.owner), value.slotIndex, 0U, value.slotGeneration};
	}
	NativeNavigationRouteHandle FromNavigationRoute(TerraEngine::Navigation::NavigationRouteHandle value) noexcept {
		return {FromNavigationOwner(value.owner), value.slotIndex, 0U, value.slotGeneration};
	}

	bool ToNavigationLocation(NativeNavigationLocation value, TerraEngine::Navigation::NavigationLocation& destination) noexcept {
		if(!std::isfinite(value.offsetX) || !std::isfinite(value.offsetY) || !std::isfinite(value.offsetZ)) return false;
		TerraEngine::Scene::WorldPosition position{};
		position.cell = {value.cellX, value.cellY, value.cellZ};
		position.offsetMetres = {value.offsetX, value.offsetY, value.offsetZ};
		const TerraEngine::Scene::WorldCoordinateResult normalized = TerraEngine::Scene::NormalizeWorldPosition(position);
		// Preserve the exact canonical address supplied by managed code. Accepting
		// an address that only becomes canonical after normalization would make the
		// audit record differ from the packet that an owner actually submitted.
		if(!normalized.Succeeded() || normalized.position != position) return false;
		destination = {ToNavigationWorld(value.world), normalized.position};
		return TerraEngine::Navigation::ValidateNavigationLocation(destination).Succeeded();
	}
	NativeNavigationLocation FromNavigationLocation(TerraEngine::Navigation::NavigationLocation value) noexcept {
		return {FromNavigationWorld(value.world), value.position.cell.x, value.position.cell.y, value.position.cell.z,
			value.position.offsetMetres.x, value.position.offsetMetres.y, value.position.offsetMetres.z};
	}
	bool ToNavigationProfile(const NativeNavigationProfile& value, TerraEngine::Navigation::NavigationProfile& destination) noexcept {
		destination.radiusMetres = value.radiusMetres;
		destination.heightMetres = value.heightMetres;
		destination.stepHeightMetres = value.stepHeightMetres;
		destination.maxSlopeDegrees = value.maxSlopeDegrees;
		destination.clearanceMarginMetres = value.clearanceMarginMetres;
		destination.horizontalResolutionMetres = value.horizontalResolutionMetres;
		destination.verticalResolutionMetres = value.verticalResolutionMetres;
		destination.excludedAreasMask = value.excludedAreasMask;
		for(std::size_t index = 0U; index < destination.areaPenalties.size(); ++index)
			destination.areaPenalties[index] = value.areaPenalties[index];
		return destination.Validate().accepted;
	}

	NativeNavigationStatus ToNativeNavigationStatus(TerraEngine::Navigation::NavigationHandleStatus status) noexcept {
		using namespace TerraEngine::Navigation;
		switch(status) {
		case NavigationHandleStatus::Alive: return NativeNavigationStatus::Success;
		case NavigationHandleStatus::Expired: return NativeNavigationStatus::Expired;
		case NavigationHandleStatus::CapacityExceeded: return NativeNavigationStatus::LimitExceeded;
		case NavigationHandleStatus::InvalidOwner: return NativeNavigationStatus::InvalidOwner;
		case NavigationHandleStatus::StaleSlot:
		case NavigationHandleStatus::StaleOwner:
		case NavigationHandleStatus::StaleWorld:
		case NavigationHandleStatus::Destroyed:
		case NavigationHandleStatus::Retired: return NativeNavigationStatus::StaleTarget;
		default: return NativeNavigationStatus::InvalidArgument;
		}
	}
	NativeNavigationStatus ToNativeNavigationStatus(TerraEngine::Navigation::NavigationOutcome outcome) noexcept {
		using TerraEngine::Navigation::NavigationOutcome;
		switch(outcome) {
		case NavigationOutcome::Queued:
		case NavigationOutcome::PendingPreparation:
		case NavigationOutcome::Searching: return NativeNavigationStatus::Accepted;
		case NavigationOutcome::Complete:
		case NavigationOutcome::Partial:
		case NavigationOutcome::Cancelled: return NativeNavigationStatus::Success;
		case NavigationOutcome::Expired: return NativeNavigationStatus::Expired;
		case NavigationOutcome::RejectedCapacity:
		case NavigationOutcome::BudgetExhausted: return NativeNavigationStatus::LimitExceeded;
		case NavigationOutcome::UnavailableCoverage: return NativeNavigationStatus::Unavailable;
		case NavigationOutcome::Stale: return NativeNavigationStatus::StaleTarget;
		default: return NativeNavigationStatus::InvalidArgument;
		}
	}
	void FillNavigationSubmitReceipt(NativeNavigationSubmitReceipt& destination, NativeNavigationStatus status,
		TerraEngine::Navigation::NavigationRequestHandle request, std::uint64_t correlationId,
		std::string_view diagnostic) noexcept {
		destination.header = {kNativeNavigationAbiVersion, sizeof(destination)};
		destination.status = status;
		destination.reserved = 0U;
		destination.request = FromNavigationRequest(request);
		destination.correlationId = correlationId;
		WriteNavigationDiagnostic(destination.diagnostic, diagnostic);
	}
}

extern "C" {
	std::uint32_t Native_Navigation_GetAbiVersion() { return kNativeNavigationAbiVersion; }

	NativeNavigationStatus Native_Navigation_RequestObjectToLocation(
		const NativeNavigationObjectToLocationRequest* request, NativeNavigationSubmitReceipt* receipt) {
		if(!HasNavigationAbiRecord(receipt)) return NativeNavigationStatus::VersionMismatch;
		if(!HasNavigationAbiRecord(request)) {
			FillNavigationSubmitReceipt(*receipt, NativeNavigationStatus::VersionMismatch, {}, 0U, "Object route ABI version or size is unsupported.");
			return receipt->status;
		}
		TerraEngine::Navigation::NavigationLocation destination{};
		TerraEngine::Navigation::NavigationProfile profile{};
		if(!ToNavigationLocation(request->destination, destination)
			|| !ToNavigationProfile(request->profile, profile) || request->routePolicy > 1U) {
			FillNavigationSubmitReceipt(*receipt, NativeNavigationStatus::InvalidArgument, {}, request->correlationId,
				"Object route has an invalid profile, canonical destination, or route policy.");
			return receipt->status;
		}
		ObjectManager& objects = ObjectManager::instance();
		const std::optional<Object> source = objects.getObject(static_cast<entt::entity>(request->sourceObjectEntity));
		if(!source) {
			FillNavigationSubmitReceipt(*receipt, NativeNavigationStatus::StaleTarget, {}, request->correlationId, "Source object is unavailable.");
			return receipt->status;
		}
		const auto submitted = TerraEngine::Navigation::NavigationSystem::Instance().RequestRoute(
			objects, objects.getUUID(*source), destination, profile,
			static_cast<TerraEngine::Navigation::NavigationRoutePolicy>(request->routePolicy), request->replacementKey);
		FillNavigationSubmitReceipt(*receipt, ToNativeNavigationStatus(submitted.outcome), submitted.request,
			request->correlationId, submitted.Succeeded() ? "" : TerraEngine::Navigation::ToString(submitted.reason));
		return receipt->status;
	}

	NativeNavigationStatus Native_Navigation_RequestObjectToObject(
		const NativeNavigationObjectToObjectRequest* request, NativeNavigationSubmitReceipt* receipt) {
		if(!HasNavigationAbiRecord(receipt)) return NativeNavigationStatus::VersionMismatch;
		if(!HasNavigationAbiRecord(request)) {
			FillNavigationSubmitReceipt(*receipt, NativeNavigationStatus::VersionMismatch, {}, 0U, "Object target route ABI version or size is unsupported.");
			return receipt->status;
		}
		TerraEngine::Navigation::NavigationProfile profile{};
		if(!ToNavigationProfile(request->profile, profile) || request->routePolicy > 1U || request->reserved != 0U) {
			FillNavigationSubmitReceipt(*receipt, NativeNavigationStatus::InvalidArgument, {}, request->correlationId, "Object target route has an invalid profile or route policy.");
			return receipt->status;
		}
		ObjectManager& objects = ObjectManager::instance();
		const std::optional<Object> source = objects.getObject(static_cast<entt::entity>(request->sourceObjectEntity));
		const std::optional<Object> target = objects.getObject(static_cast<entt::entity>(request->targetObjectEntity));
		if(!source || !target) {
			FillNavigationSubmitReceipt(*receipt, NativeNavigationStatus::StaleTarget, {}, request->correlationId, "Source or target object is unavailable.");
			return receipt->status;
		}
		const auto submitted = TerraEngine::Navigation::NavigationSystem::Instance().RequestRoute(
			objects, objects.getUUID(*source), objects.getUUID(*target), profile,
			static_cast<TerraEngine::Navigation::NavigationRoutePolicy>(request->routePolicy), request->replacementKey);
		FillNavigationSubmitReceipt(*receipt, ToNativeNavigationStatus(submitted.outcome), submitted.request,
			request->correlationId, submitted.Succeeded() ? "" : TerraEngine::Navigation::ToString(submitted.reason));
		return receipt->status;
	}

	NativeNavigationStatus Native_Navigation_ReadRequest(
		const NativeNavigationRequestHandle* request, NativeNavigationRequestSnapshot* snapshot) {
		if(request == nullptr || !HasNavigationAbiRecord(snapshot)) return NativeNavigationStatus::VersionMismatch;
		const auto handle = ToNavigationRequest(*request);
		if(!handle.IsValid()) return NativeNavigationStatus::InvalidArgument;
		const auto read = TerraEngine::Navigation::NavigationSystem::Instance().ReadRequest(handle);
		*snapshot = {};
		snapshot->header = {kNativeNavigationAbiVersion, sizeof(*snapshot)};
		snapshot->status = ToNativeNavigationStatus(read.status);
		if(!read.record) return snapshot->status;
		const auto route = TerraEngine::Navigation::NavigationSystem::Instance().PublishedRouteForRequest(handle);
		snapshot->lifecycle = static_cast<std::uint32_t>(read.record->lifecycle);
		snapshot->outcome = static_cast<std::uint32_t>(read.record->outcome);
		snapshot->reason = static_cast<std::uint32_t>(read.record->reason);
		snapshot->request = FromNavigationRequest(handle);
		snapshot->route = FromNavigationRoute(route);
		snapshot->intentGeneration = read.record->intentGeneration;
		snapshot->start = FromNavigationLocation(read.record->start);
		snapshot->destination = FromNavigationLocation(read.record->destination);
		snapshot->createdTick = read.record->createdTick;
		snapshot->expiryTick = read.record->expiryTick;
		return snapshot->status = ToNativeNavigationStatus(read.record->outcome);
	}

	NativeNavigationStatus Native_Navigation_CopyRoute(
		const NativeNavigationRouteHandle* route, NativeNavigationRoutePoint* points,
		std::uint32_t capacity, NativeNavigationBufferInfo* info) {
		if(route == nullptr || !HasNavigationAbiRecord(info)) return NativeNavigationStatus::VersionMismatch;
		const auto handle = ToNavigationRoute(*route);
		if(!handle.IsValid()) return NativeNavigationStatus::InvalidArgument;
		const auto retained = TerraEngine::Navigation::NavigationSystem::Instance().ReadRoute(handle);
		*info = {{kNativeNavigationAbiVersion, sizeof(*info)}, NativeNavigationStatus::Unavailable, 0U, 0U, 0U, retained.sourceRevision};
		if(!retained.IsValid()) return info->status;
		if(retained.points.size() > std::numeric_limits<std::uint32_t>::max()) {
			info->status = NativeNavigationStatus::LimitExceeded;
			return info->status;
		}
		info->requiredCount = static_cast<std::uint32_t>(retained.points.size());
		if(points == nullptr && capacity == 0U) { info->status = NativeNavigationStatus::Success; return info->status; }
		if(points == nullptr || capacity < info->requiredCount) { info->status = NativeNavigationStatus::LimitExceeded; return info->status; }
		for(std::size_t index = 0U; index < retained.points.size(); ++index)
			points[index] = {{kNativeNavigationAbiVersion, sizeof(NativeNavigationRoutePoint)}, FromNavigationLocation(retained.points[index])};
		info->copiedCount = info->requiredCount;
		info->status = NativeNavigationStatus::Success;
		return info->status;
	}

	NativeNavigationStatus Native_Navigation_CancelRequest(
		const NativeNavigationCancellationRequest* request, NativeNavigationSubmitReceipt* receipt) {
		if(!HasNavigationAbiRecord(receipt)) return NativeNavigationStatus::VersionMismatch;
		if(!HasNavigationAbiRecord(request) || !ToNavigationRequest(request->request).IsValid()) {
			FillNavigationSubmitReceipt(*receipt, NativeNavigationStatus::InvalidArgument, {}, 0U, "Cancellation request is malformed.");
			return receipt->status;
		}
		const auto handle = ToNavigationRequest(request->request);
		const bool cancelled = TerraEngine::Navigation::NavigationSystem::Instance().CancelRequest(handle);
		FillNavigationSubmitReceipt(*receipt, cancelled ? NativeNavigationStatus::Success : NativeNavigationStatus::StaleTarget,
			handle, 0U, cancelled ? "" : "Request is terminal, expired, or belongs to another owner.");
		return receipt->status;
	}

	NativeNavigationStatus Native_Navigation_ReleaseResult(
		const NativeNavigationCancellationRequest* request, NativeNavigationSubmitReceipt* receipt) {
		if(!HasNavigationAbiRecord(receipt)) return NativeNavigationStatus::VersionMismatch;
		if(!HasNavigationAbiRecord(request) || !ToNavigationRequest(request->request).IsValid()) {
			FillNavigationSubmitReceipt(*receipt, NativeNavigationStatus::InvalidArgument, {}, 0U,
				"Navigation result release is malformed.");
			return receipt->status;
		}
		const auto handle = ToNavigationRequest(request->request);
		const bool released = TerraEngine::Navigation::NavigationSystem::Instance().ReleaseRequestResult(handle);
		FillNavigationSubmitReceipt(*receipt, released ? NativeNavigationStatus::Success : NativeNavigationStatus::StaleTarget,
			handle, 0U, released ? "" : "Request is active, expired, or has already been released.");
		return receipt->status;
	}

	NativeNavigationStatus Native_Navigation_ValidateRoute(
		const NativeNavigationRouteValidationRequest* request, NativeNavigationRouteValidationResult* result) {
		if(!HasNavigationAbiRecord(result)) return NativeNavigationStatus::VersionMismatch;
		if(!HasNavigationAbiRecord(request)) return NativeNavigationStatus::VersionMismatch;
		const auto handle = ToNavigationRoute(request->route);
		if(!handle.IsValid()) return NativeNavigationStatus::InvalidArgument;
		const auto validation = TerraEngine::Navigation::NavigationSystem::Instance().ValidateRoute(handle);
		*result = {};
		result->header = {kNativeNavigationAbiVersion, sizeof(*result)};
		result->status = ToNativeNavigationStatus(validation.outcome);
		result->usable = validation.usable ? 1U : 0U;
		result->outcome = static_cast<std::uint32_t>(validation.outcome);
		result->reason = static_cast<std::uint32_t>(validation.reason);
		return result->status;
	}

	NativeNavigationStatus Native_Navigation_CopyInvalidations(
		const NativeNavigationInvalidationCursor* cursor, NativeNavigationInvalidationRecord* records,
		std::uint32_t capacity, NativeNavigationBufferInfo* info) {
		if(!HasNavigationAbiRecord(info)) return NativeNavigationStatus::VersionMismatch;
		if(!HasNavigationAbiRecord(cursor)) return NativeNavigationStatus::VersionMismatch;
		const auto owner = ToNavigationOwner(cursor->owner);
		if(!owner.IsValid()) return NativeNavigationStatus::InvalidOwner;
		const auto read = TerraEngine::Navigation::NavigationSystem::Instance().ReadInvalidations(owner, cursor->cursor);
		*info = {{kNativeNavigationAbiVersion, sizeof(*info)}, ToNativeNavigationStatus(read.status), 0U, 0U,
			read.overflowed ? 1U : 0U, read.newestCursor};
		if(read.records.size() > std::numeric_limits<std::uint32_t>::max()) { info->status = NativeNavigationStatus::LimitExceeded; return info->status; }
		info->requiredCount = static_cast<std::uint32_t>(read.records.size());
		if(records == nullptr && capacity == 0U) { info->status = NativeNavigationStatus::Success; return info->status; }
		if(records == nullptr || capacity < info->requiredCount) { info->status = NativeNavigationStatus::LimitExceeded; return info->status; }
		for(std::size_t index = 0U; index < read.records.size(); ++index) {
			const auto& source = read.records[index];
			records[index] = {{kNativeNavigationAbiVersion, sizeof(NativeNavigationInvalidationRecord)},
				FromNavigationOwner(source.owner), FromNavigationRoute(source.route), static_cast<std::uint32_t>(source.reason),
				static_cast<std::uint32_t>(source.outcome), source.sourceRevision, source.observationGeneration};
		}
		info->copiedCount = info->requiredCount;
		info->status = NativeNavigationStatus::Success;
		return info->status;
	}

	std::uint16_t Native_Animation_GetAbiVersion()
	{
		return kNativeAnimationAbiVersion;
	}

	NativeAnimationStatus Native_Animation_ResolveAnimator(
		std::uint32_t objectEntity,
		NativeAnimationAnimatorHandleRecord* animator)
	{
		const NativeAnimationStatus outputStatus = ValidateAnimationRecord(
			animator, NativeAnimationRecordKind::AnimatorHandle);
		if(outputStatus != NativeAnimationStatus::Ok) return outputStatus;
		const NativeAnimationAbiHeader header = animator->header;
		*animator = {};
		animator->header = header;
		animator->status = NativeAnimationStatus::StaleHandle;

		ObjectManager& objects = ObjectManager::instance();
		const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectEntity));
		if(!object) return animator->status;
		const TerraEngine::UUID objectId = objects.getUUID(*object);
		const TerraEngine::Animation::AnimatorInstanceHandle handle =
			TerraEngine::Animation::AnimationSystem::Instance().ResolveAnimatorForObject(objectId);
		if(!handle.IsValid()) {
			animator->status = NativeAnimationStatus::NotReady;
			return animator->status;
		}
		animator->status = NativeAnimationStatus::Ok;
		animator->animator = AnimationFromHandle(handle);
		animator->ownerObjectId = AnimationFromUuid(objectId);
		animator->componentGeneration = TerraEngine::CompressObjectGeneration(
			objects.getObjectComponentGeneration(*object));
		return animator->status;
	}

	NativeAnimationStatus Native_Animation_SubmitCommand(
		const NativeAnimationCommandRequest* request,
		NativeAnimationCommandReceipt* receipt)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus requestStatus = ValidateAnimationRecord(
			request, NativeAnimationRecordKind::CommandRequest);
		if(requestStatus != NativeAnimationStatus::Ok) return requestStatus;
		const NativeAnimationStatus receiptStatus = ValidateAnimationRecord(
			receipt, NativeAnimationRecordKind::CommandReceipt);
		if(receiptStatus != NativeAnimationStatus::Ok) return receiptStatus;

		AnimationCommand command;
		command.target = AnimationToHandle(request->animator);
		command.targetTick = request->targetTick;
		command.producerId = request->producerId;
		command.producerSequence = request->producerSequence;
		command.correlationId = request->correlationId;
		command.idempotencyKey = request->idempotencyKey;
		command.cancellationToken = request->cancellationToken;
		command.playbackTick = request->playbackTick;
		command.playbackSpeed = request->playbackSpeed;
		command.playbackFlag = request->playbackFlag != 0U;
		command.stateId = request->stateId;
		command.layerIndex = request->layerIndex;
		command.layerWeight = request->floatValue;

		AnimationCommandReceipt nativeReceipt;
		nativeReceipt.target = command.target;
		nativeReceipt.producerId = command.producerId;
		nativeReceipt.producerSequence = command.producerSequence;
		nativeReceipt.correlationId = command.correlationId;
		nativeReceipt.idempotencyKey = command.idempotencyKey;
		nativeReceipt.cancellationToken = command.cancellationToken;
		nativeReceipt.targetTick = command.targetTick;
		nativeReceipt.status = AnimationStatus::InvalidArgument;

		const bool commonIdentityValid = command.target.IsValid()
			&& command.producerId != 0U
			&& command.producerSequence != 0U
			&& command.correlationId != 0U
			&& command.targetTick >= 0;
		if(!commonIdentityValid) {
			FillAnimationReceipt(nativeReceipt, *receipt);
			return receipt->status;
		}

		switch(request->command) {
		case NativeAnimationCommandKind::SetParameter:
			if(request->parameterId == 0U
				|| request->parameterId > std::numeric_limits<std::uint32_t>::max()) break;
			command.kind = AnimationCommandKind::SetParameter;
			command.parameterId = static_cast<std::uint32_t>(request->parameterId);
			switch(request->parameterType) {
			case NativeAnimationParameterType::Boolean:
				if(request->booleanValue > 1U) break;
				command.parameterType = AnimationParameterType::Boolean;
				command.value = request->booleanValue != 0U;
				nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command));
				break;
			case NativeAnimationParameterType::Integer:
				command.parameterType = AnimationParameterType::Integer;
				command.value = request->integerValue;
				nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command));
				break;
			case NativeAnimationParameterType::Float:
				if(!std::isfinite(request->floatValue)) break;
				command.parameterType = AnimationParameterType::Float;
				command.value = request->floatValue;
				nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command));
				break;
			case NativeAnimationParameterType::Vector3:
				command.parameterType = AnimationParameterType::Vector3;
				command.value = AnimationToVector(request->parameterVector);
				nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command));
				break;
			case NativeAnimationParameterType::Quaternion:
				command.parameterType = AnimationParameterType::Quaternion;
				command.value = AnimationToQuaternion(request->parameterQuaternion);
				nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command));
				break;
			case NativeAnimationParameterType::Transform:
				command.parameterType = AnimationParameterType::Transform;
				command.value = AnimationToTransform(request->parameterTransform);
				nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command));
				break;
			default:
				nativeReceipt.status = AnimationStatus::Unsupported;
				break;
			}
			break;
		case NativeAnimationCommandKind::FireTrigger:
			if(request->parameterId == 0U
				|| request->parameterId > std::numeric_limits<std::uint32_t>::max()) break;
			command.kind = AnimationCommandKind::FireTrigger;
			command.parameterType = AnimationParameterType::Trigger;
			command.parameterId = static_cast<std::uint32_t>(request->parameterId);
			command.value = std::monostate{};
			nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command));
			break;
		case NativeAnimationCommandKind::Play:
			// State zero retains the already-bound state for ABI-v1 compatibility;
			// non-zero selects an explicit controller state at frame publication.
			if(command.playbackTick < 0) break;
			command.kind = AnimationCommandKind::Play;
			nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command));
			break;
		case NativeAnimationCommandKind::Stop: command.kind = AnimationCommandKind::Stop; nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command)); break;
		case NativeAnimationCommandKind::Pause: command.kind = AnimationCommandKind::Pause; nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command)); break;
		case NativeAnimationCommandKind::Resume: command.kind = AnimationCommandKind::Resume; nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command)); break;
		case NativeAnimationCommandKind::Seek: command.kind = AnimationCommandKind::Seek; nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command)); break;
		case NativeAnimationCommandKind::SetLooping: command.kind = AnimationCommandKind::SetLooping; nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command)); break;
		case NativeAnimationCommandKind::SetReverse: command.kind = AnimationCommandKind::SetReverse; nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command)); break;
		case NativeAnimationCommandKind::SetSpeed:
			if(!std::isfinite(command.playbackSpeed)) break;
			command.kind = AnimationCommandKind::SetSpeed;
			nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command));
			break;
		case NativeAnimationCommandKind::CrossFade: {
			if(command.stateId == 0U || !std::isfinite(request->transitionSeconds)
				|| request->transitionSeconds < 0.0F) break;
			const long double ticks = static_cast<long double>(request->transitionSeconds)
				* static_cast<long double>(kAnimationFixedTicksPerSecond);
			if(ticks > static_cast<long double>(
				std::numeric_limits<AnimationFixedTick>::max())) break;
			command.transitionTicks = static_cast<AnimationFixedTick>(std::llround(ticks));
			command.kind = AnimationCommandKind::CrossFade;
			nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command));
			break;
		}
		case NativeAnimationCommandKind::SetLayerWeight:
			if(!std::isfinite(command.layerWeight)
				|| command.layerWeight < 0.0F || command.layerWeight > 1.0F) break;
			command.kind = AnimationCommandKind::SetLayerWeight;
			nativeReceipt = AnimationSystem::Instance().SubmitCommand(std::move(command));
			break;
		case NativeAnimationCommandKind::SetIkGoal:
		case NativeAnimationCommandKind::ClearIkGoal:
		case NativeAnimationCommandKind::RequestRagdoll:
			nativeReceipt.status = AnimationStatus::Unsupported;
			break;
		default:
			nativeReceipt.status = AnimationStatus::InvalidArgument;
			break;
		}
		FillAnimationReceipt(nativeReceipt, *receipt);
		return receipt->status;
	}

	NativeAnimationStatus Native_Animation_CancelCommand(
		const NativeAnimationCancellationRequest* request,
		NativeAnimationCommandReceipt* receipt)
	{
		const NativeAnimationStatus requestStatus = ValidateAnimationRecord(
			request, NativeAnimationRecordKind::CancellationRequest);
		if(requestStatus != NativeAnimationStatus::Ok) return requestStatus;
		const NativeAnimationStatus receiptStatus = ValidateAnimationRecord(
			receipt, NativeAnimationRecordKind::CommandReceipt);
		if(receiptStatus != NativeAnimationStatus::Ok) return receiptStatus;
		const TerraEngine::Animation::AnimationCommandReceipt native =
			TerraEngine::Animation::AnimationSystem::Instance().CancelCommand(
				request->cancellationToken, request->correlationId);
		FillAnimationReceipt(native, *receipt);
		return receipt->status;
	}

	NativeAnimationStatus Native_Animation_ReadReceipt(
		const NativeAnimationReceiptQuery* query,
		NativeAnimationCommandReceipt* receipt)
	{
		const NativeAnimationStatus queryStatus = ValidateAnimationRecord(
			query, NativeAnimationRecordKind::ReceiptQuery);
		if(queryStatus != NativeAnimationStatus::Ok) return queryStatus;
		const NativeAnimationStatus receiptStatus = ValidateAnimationRecord(
			receipt, NativeAnimationRecordKind::CommandReceipt);
		if(receiptStatus != NativeAnimationStatus::Ok) return receiptStatus;
		const TerraEngine::Animation::AnimationCommandReceipt native =
			TerraEngine::Animation::AnimationSystem::Instance().ReadCommandReceipt(
				query->producerId, query->producerSequence, query->correlationId);
		FillAnimationReceipt(native, *receipt);
		return receipt->status;
	}

	NativeAnimationStatus Native_Animation_QueryCapability(
		const NativeAnimationCapabilityQuery* query,
		NativeAnimationCapabilityResult* result)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus queryStatus = ValidateAnimationRecord(
			query, NativeAnimationRecordKind::CapabilityQuery);
		if(queryStatus != NativeAnimationStatus::Ok) return queryStatus;
		const NativeAnimationStatus resultStatus = ValidateAnimationRecord(
			result, NativeAnimationRecordKind::CapabilityResult);
		if(resultStatus != NativeAnimationStatus::Ok) return resultStatus;
		*result = {};
		result->header = AnimationHeader(NativeAnimationRecordKind::CapabilityResult, sizeof(*result));
		result->capability = query->capability;
		const AnimatorInstanceHandle handle = AnimationToHandle(query->animator);
		if(!AnimationSystem::Instance().IsAnimatorInstanceAlive(handle)) {
			result->status = NativeAnimationStatus::StaleHandle;
			result->reasonMask = static_cast<std::uint32_t>(NativeAnimationCapabilityReason::AnimatorNotReady);
			return result->status;
		}
		switch(query->capability) {
		case NativeAnimationCapability::ParameterWrite:
		case NativeAnimationCapability::Trigger:
		case NativeAnimationCapability::Playback:
		case NativeAnimationCapability::EventRead:
			result->status = NativeAnimationStatus::Ok;
			result->supported = 1U;
			result->reasonMask = static_cast<std::uint32_t>(NativeAnimationCapabilityReason::Available);
			break;
		case NativeAnimationCapability::BoneRead: {
			std::shared_ptr<const AnimationAuthorityFrame> authority;
			const AnimationAuthorityRow* published = LoadAuthorityRow(handle, authority);
			if(!published || !published->snapshot || !published->snapshot->pose
				|| !published->snapshot->pose->IsSealed()) {
				result->status = NativeAnimationStatus::NotReady;
				result->reasonMask = static_cast<std::uint32_t>(
					NativeAnimationCapabilityReason::AnimatorNotReady);
				break;
			}
			const PoseBuffer& pose = *published->snapshot->pose;
			const std::span<const AnimationTransform> local = pose.LocalTransforms();
			const std::span<const AnimationTransform> model = pose.ModelTransforms();
			const bool currentCpuPose = pose.Generation().animator == handle
				&& pose.Generation().poseGeneration == published->poseGeneration
				&& model.size() == local.size()
				&& model.size() == published->jointStableIds.size();
			if(!currentCpuPose) {
				result->status = NativeAnimationStatus::NotReady;
				result->reasonMask = static_cast<std::uint32_t>(
					NativeAnimationCapabilityReason::AnimatorNotReady);
				break;
			}
			const bool requestedBoneAvailable = query->stableId == 0U
				? !model.empty()
				: std::find(published->jointStableIds.begin(), published->jointStableIds.end(),
					query->stableId) != published->jointStableIds.end();
			if(!requestedBoneAvailable) {
				result->status = NativeAnimationStatus::Unsupported;
				result->reasonMask = static_cast<std::uint32_t>(
					NativeAnimationCapabilityReason::UnsupportedOperation);
				break;
			}
			result->status = NativeAnimationStatus::Ok;
			result->supported = 1U;
			result->reasonMask = static_cast<std::uint32_t>(NativeAnimationCapabilityReason::Available);
			break;
		}
		case NativeAnimationCapability::SocketRead: {
			// A zero stable ID asks whether this runtime exposes the socket-read
			// operation at all. It does not ask whether a particular socket has
			// reached a protected publication yet, so an empty current set remains
			// a supported capability. Non-zero IDs continue through the strict
			// generation-matched availability checks below.
			if(query->stableId == 0U) {
				result->status = NativeAnimationStatus::Ok;
				result->supported = 1U;
				result->reasonMask = static_cast<std::uint32_t>(
					NativeAnimationCapabilityReason::Available);
				break;
			}
			std::shared_ptr<const AnimationAuthorityFrame> authority;
			const AnimationAuthorityRow* published = LoadAuthorityRow(handle, authority);
			AnimationSocketReadSnapshot socketSnapshot;
			const bool currentSnapshot = CopyOrMaterializeCurrentSocketSnapshot(
				handle, published, socketSnapshot);
			if(!currentSnapshot) {
				result->status = NativeAnimationStatus::NotReady;
				result->reasonMask = static_cast<std::uint32_t>(
					NativeAnimationCapabilityReason::AnimatorNotReady);
				break;
			}
			const bool requestedSocketAvailable = std::any_of(
				socketSnapshot.sockets.begin(), socketSnapshot.sockets.end(),
				[query](const AnimationAttachmentTransformSnapshot& value) {
					return value.stableId == query->stableId;
				});
			if(!requestedSocketAvailable) {
				result->status = NativeAnimationStatus::Unsupported;
				result->reasonMask = static_cast<std::uint32_t>(
					NativeAnimationCapabilityReason::UnsupportedOperation);
				break;
			}
			result->status = NativeAnimationStatus::Ok;
			result->supported = 1U;
			result->reasonMask = static_cast<std::uint32_t>(NativeAnimationCapabilityReason::Available);
			break;
		}
		case NativeAnimationCapability::RootMotion: {
			std::shared_ptr<const AnimationAuthorityFrame> authority;
			const AnimationAuthorityRow* published = LoadAuthorityRow(handle, authority);
			if(!published || !published->snapshot || !published->snapshot->pose
				|| !published->snapshot->pose->RootMotionDelta()) {
				result->status = NativeAnimationStatus::NotReady;
				result->reasonMask = static_cast<std::uint32_t>(
					NativeAnimationCapabilityReason::AnimatorNotReady);
				break;
			}
			const AnimationRootMotionDelta& delta =
				*published->snapshot->pose->RootMotionDelta();
			if(delta.animator != handle || delta.generation == 0U
				|| delta.generation != published->poseGeneration
				|| published->sourceGeneration == 0U) {
				result->status = NativeAnimationStatus::NotReady;
				result->reasonMask = static_cast<std::uint32_t>(
					NativeAnimationCapabilityReason::AnimatorNotReady);
				break;
			}
			if(published->rootMotionPolicy == AnimatorRootMotionPolicy::ApplyToObject
				&& (published->ownerObjectId == TerraEngine::EMPTY_UUID
					|| published->objectGeneration == 0U
					|| published->componentGeneration == 0U
					|| !published->sourceObjectWorldTransformCaptured)) {
				result->status = NativeAnimationStatus::NotReady;
				result->reasonMask = static_cast<std::uint32_t>(
					NativeAnimationCapabilityReason::AnimatorNotReady);
				break;
			}
			if(published->rootMotionPolicy == AnimatorRootMotionPolicy::ApplyToCharacter) {
				const std::shared_ptr<const TerraEngine::Physics::PhysicsCharacterStatePacket> states =
					TerraEngine::Physics::PhysicsSystem::Instance().LatestCharacterStatePacket();
				const bool matchingTarget = states && states->states
					&& std::any_of(states->states->begin(), states->states->end(), [&published](const auto& state) {
						return state.objectId == published->ownerObjectId
							&& state.objectGeneration == published->objectGeneration
							&& state.componentGeneration == published->componentGeneration;
					});
				if(!matchingTarget) {
					result->status = NativeAnimationStatus::NotReady;
					result->reasonMask = static_cast<std::uint32_t>(
						NativeAnimationCapabilityReason::MissingPhysicsTarget);
					break;
				}
			}
			result->status = NativeAnimationStatus::Ok;
			result->supported = 1U;
			result->reasonMask = static_cast<std::uint32_t>(NativeAnimationCapabilityReason::Available);
			break;
		}
		case NativeAnimationCapability::Retarget:
		case NativeAnimationCapability::IkGoal:
		case NativeAnimationCapability::MotionMatching:
		case NativeAnimationCapability::Ragdoll:
			// These operations have bounded copied request owners. Asset, pose, and
			// physics readiness are reported by each request/snapshot rather than
			// misrepresenting the native capability as absent.
			result->status = NativeAnimationStatus::Ok;
			result->supported = 1U;
			result->reasonMask = static_cast<std::uint32_t>(
				NativeAnimationCapabilityReason::Available);
			break;
		case NativeAnimationCapability::StateTransition:
			result->status = NativeAnimationStatus::Unsupported;
			result->reasonMask = static_cast<std::uint32_t>(NativeAnimationCapabilityReason::UnsupportedOperation);
			break;
		default:
			result->status = NativeAnimationStatus::InvalidArgument;
			break;
		}
		return result->status;
	}

	NativeAnimationStatus Native_Animation_ReadState(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationStateSnapshot* snapshot)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus snapshotStatus = ValidateAnimationRecord(
			snapshot, NativeAnimationRecordKind::StateSnapshot);
		if(snapshotStatus != NativeAnimationStatus::Ok) return snapshotStatus;
		if(animator == nullptr) return NativeAnimationStatus::InvalidArgument;
		const AnimatorInstanceHandle handle = AnimationToHandle(*animator);
		std::shared_ptr<const AnimationAuthorityFrame> authority;
		const AnimationAuthorityRow* published = LoadAuthorityRow(handle, authority);
		*snapshot = {};
		snapshot->header = AnimationHeader(NativeAnimationRecordKind::StateSnapshot, sizeof(*snapshot));
		snapshot->animator = *animator;
		if(!AnimationSystem::Instance().IsAnimatorInstanceAlive(handle)) {
			snapshot->status = NativeAnimationStatus::StaleHandle;
			return snapshot->status;
		}
		if(!published || !published->snapshot || !published->snapshot->pose) {
			snapshot->status = NativeAnimationStatus::NotReady;
			return snapshot->status;
		}
		const AnimationSnapshot& source = *published->snapshot;
		snapshot->status = ToNativeAnimationStatus(published->status);
		snapshot->controllerStateId = source.playback.currentStateId;
		snapshot->tick = source.tick;
		snapshot->observationTick = source.tick;
		snapshot->ageTicks = 0;
		snapshot->normalizedTime = source.playback.normalizedTime;
		snapshot->transitionProgress = source.playback.transitionProgress;
		snapshot->effectiveSpeed = source.playback.effectiveSpeed;
		snapshot->loopCount = source.playback.loopCount;
		snapshot->publicationGeneration = authority->authorityGeneration;
		snapshot->layerCount = static_cast<std::uint32_t>(source.playback.activeLayers.size());
		snapshot->parameterCount = static_cast<std::uint32_t>(source.playback.parameters.size());
		snapshot->curveCount = static_cast<std::uint32_t>(source.pose->CurveValues().size());
		snapshot->morphCount = static_cast<std::uint32_t>(source.pose->MorphValues().size());
		snapshot->boneCount = static_cast<std::uint32_t>(source.pose->ModelTransforms().size());
		snapshot->socketCount = static_cast<std::uint32_t>(
			AnimationAttachmentSnapshotPublisher::Instance().PublishedSocketCount(handle));
		snapshot->eventCount = static_cast<std::uint32_t>(source.pose->Events().size());
		snapshot->hasRootMotion = source.pose->RootMotionDelta().has_value() ? 1U : 0U;
		snapshot->requestedExecutionPolicy = static_cast<std::uint32_t>(published->requestedExecutionPolicy);
		snapshot->effectiveExecutionMode = static_cast<std::uint32_t>(published->effectiveExecutionMode);
		snapshot->controllerAssetId = AnimationFromUuid(published->controller.assetId.value);
		snapshot->controllerGeneration = published->controller.generation;
		snapshot->clipAssetId = AnimationFromUuid(published->clip.assetId.value);
		snapshot->clipGeneration = published->clip.generation;
		snapshot->skeletonAssetId = AnimationFromUuid(published->skeleton.assetId.value);
		snapshot->skeletonGeneration = published->skeleton.generation;
		snapshot->updatePolicy = static_cast<std::uint32_t>(published->updatePolicy);
		snapshot->eventPolicy = static_cast<std::uint32_t>(published->eventPolicy);
		snapshot->cullingPolicy = static_cast<std::uint32_t>(published->cullingPolicy);
		snapshot->rootMotionPolicy = static_cast<std::uint32_t>(published->rootMotionPolicy);
		return snapshot->status;
	}

	NativeAnimationStatus Native_Animation_CopyParameters(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationNamedValue* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus infoStatus = ValidateAnimationRecord(info, NativeAnimationRecordKind::BufferInfo);
		if(infoStatus != NativeAnimationStatus::Ok) return infoStatus;
		if(animator == nullptr) return NativeAnimationStatus::InvalidArgument;
		const AnimatorInstanceHandle handle = AnimationToHandle(*animator);
		if(!AnimationSystem::Instance().IsAnimatorInstanceAlive(handle)) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::StaleHandle, 0U, 0U, 0, 0U);
			return info->status;
		}
		const std::shared_ptr<const AnimationSnapshot> snapshot = AnimationSystem::Instance().LoadAnimatorSnapshot(handle);
		if(!snapshot) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::NotReady, 0U, 0U, 0, 0U);
			return info->status;
		}
		return CopyAnimationSpan<NativeAnimationNamedValue, AnimationParameterSnapshot>(
			snapshot->playback.parameters, values, capacity, info, snapshot->tick,
			AnimationSystem::Instance().AuthorityPublicationGeneration(),
			[](const AnimationParameterSnapshot& source, std::uint32_t) {
				NativeAnimationNamedValue value{};
				value.header = AnimationHeader(NativeAnimationRecordKind::NamedValue, sizeof(value));
				value.stableId = source.parameterId;
				value.type = static_cast<NativeAnimationParameterType>(source.type);
				if(const bool* booleanValue = std::get_if<bool>(&source.value)) value.booleanValue = *booleanValue ? 1U : 0U;
				else if(const std::int32_t* integerValue = std::get_if<std::int32_t>(&source.value)) value.integerValue = *integerValue;
				else if(const float* floatValue = std::get_if<float>(&source.value)) value.floatValue = *floatValue;
				else if(const AnimationFloat3* vectorValue = std::get_if<AnimationFloat3>(&source.value)) value.vectorValue = AnimationFromVector(*vectorValue);
				else if(const AnimationQuaternion* quaternionValue = std::get_if<AnimationQuaternion>(&source.value)) value.quaternionValue = AnimationFromQuaternion(*quaternionValue);
				else if(const AnimationTransform* transformValue = std::get_if<AnimationTransform>(&source.value)) value.transformValue = AnimationFromTransform(*transformValue);
				else if(source.type == AnimationParameterType::Trigger) value.booleanValue = 1U;
				return value;
			});
	}

	NativeAnimationStatus Native_Animation_CopyCurves(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationNamedValue* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus infoStatus = ValidateAnimationRecord(info, NativeAnimationRecordKind::BufferInfo);
		if(infoStatus != NativeAnimationStatus::Ok) return infoStatus;
		if(animator == nullptr) return NativeAnimationStatus::InvalidArgument;
		const AnimatorInstanceHandle handle = AnimationToHandle(*animator);
		if(!AnimationSystem::Instance().IsAnimatorInstanceAlive(handle)) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::StaleHandle, 0U, 0U, 0, 0U);
			return info->status;
		}
		const std::shared_ptr<const AnimationSnapshot> snapshot = AnimationSystem::Instance().LoadAnimatorSnapshot(handle);
		if(!snapshot || !snapshot->pose) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::NotReady, 0U, 0U, 0, 0U);
			return info->status;
		}
		return CopyAnimationSpan<NativeAnimationNamedValue, AnimationNamedFloatValue>(
			snapshot->pose->CurveValues(), values, capacity, info, snapshot->tick,
			AnimationSystem::Instance().AuthorityPublicationGeneration(),
			[](const AnimationNamedFloatValue& source, std::uint32_t) {
				NativeAnimationNamedValue value{};
				value.header = AnimationHeader(NativeAnimationRecordKind::NamedValue, sizeof(value));
				value.stableId = source.stableId;
				value.type = NativeAnimationParameterType::Float;
				value.floatValue = source.value;
				return value;
			});
	}

	NativeAnimationStatus Native_Animation_CopyMorphs(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationNamedValue* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus infoStatus = ValidateAnimationRecord(info, NativeAnimationRecordKind::BufferInfo);
		if(infoStatus != NativeAnimationStatus::Ok) return infoStatus;
		if(animator == nullptr) return NativeAnimationStatus::InvalidArgument;
		const AnimatorInstanceHandle handle = AnimationToHandle(*animator);
		if(!AnimationSystem::Instance().IsAnimatorInstanceAlive(handle)) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::StaleHandle, 0U, 0U, 0, 0U);
			return info->status;
		}
		const std::shared_ptr<const AnimationSnapshot> snapshot = AnimationSystem::Instance().LoadAnimatorSnapshot(handle);
		if(!snapshot || !snapshot->pose) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::NotReady, 0U, 0U, 0, 0U);
			return info->status;
		}
		return CopyAnimationSpan<NativeAnimationNamedValue, AnimationNamedFloatValue>(
			snapshot->pose->MorphValues(), values, capacity, info, snapshot->tick,
			AnimationSystem::Instance().AuthorityPublicationGeneration(),
			[](const AnimationNamedFloatValue& source, std::uint32_t) {
				NativeAnimationNamedValue value{};
				value.header = AnimationHeader(NativeAnimationRecordKind::NamedValue, sizeof(value));
				value.stableId = source.stableId;
				value.type = NativeAnimationParameterType::Float;
				value.floatValue = source.value;
				return value;
			});
	}

	NativeAnimationStatus Native_Animation_CopyBones(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationBoneSnapshot* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus infoStatus = ValidateAnimationRecord(info, NativeAnimationRecordKind::BufferInfo);
		if(infoStatus != NativeAnimationStatus::Ok) return infoStatus;
		if(animator == nullptr) return NativeAnimationStatus::InvalidArgument;
		const AnimatorInstanceHandle handle = AnimationToHandle(*animator);
		if(!AnimationSystem::Instance().IsAnimatorInstanceAlive(handle)) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::StaleHandle, 0U, 0U, 0, 0U);
			return info->status;
		}
		std::shared_ptr<const AnimationAuthorityFrame> authority;
		const AnimationAuthorityRow* published = LoadAuthorityRow(handle, authority);
		if(!published || !published->snapshot || !published->snapshot->pose) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::NotReady, 0U, 0U, 0, 0U);
			return info->status;
		}
		const std::span<const AnimationTransform> model = published->snapshot->pose->ModelTransforms();
		const std::span<const AnimationTransform> local = published->snapshot->pose->LocalTransforms();
		if(model.size() != local.size() || model.size() != published->jointStableIds.size()) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::InvalidEnvelope, 0U, 0U,
				published->snapshot->tick, authority->authorityGeneration);
			return info->status;
		}
		if(model.size() > capacity) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::BufferTooSmall, model.size(), 0U,
				published->snapshot->tick, authority->authorityGeneration);
			return info->status;
		}
		if(!model.empty() && values == nullptr) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::InvalidArgument, model.size(), 0U,
				published->snapshot->tick, authority->authorityGeneration);
			return info->status;
		}
		for(std::size_t index = 0U; index < model.size(); ++index) {
			NativeAnimationBoneSnapshot bone{};
			bone.header = AnimationHeader(NativeAnimationRecordKind::BoneSnapshot, sizeof(bone));
			bone.stableId = published->jointStableIds[index];
			bone.jointIndex = static_cast<std::uint32_t>(index);
			bone.poseGeneration = published->poseGeneration;
			bone.localTransform = AnimationFromTransform(local[index]);
			bone.modelTransform = AnimationFromTransform(model[index]);
			values[index] = bone;
		}
		FillAnimationBufferInfo(*info, NativeAnimationStatus::Ok, model.size(), model.size(),
			published->snapshot->tick, authority->authorityGeneration);
		return info->status;
	}

	NativeAnimationStatus Native_Animation_CopySockets(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationSocketSnapshot* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus infoStatus = ValidateAnimationRecord(info, NativeAnimationRecordKind::BufferInfo);
		if(infoStatus != NativeAnimationStatus::Ok) return infoStatus;
		if(animator == nullptr) return NativeAnimationStatus::InvalidArgument;
		const AnimatorInstanceHandle handle = AnimationToHandle(*animator);
		if(!AnimationSystem::Instance().IsAnimatorInstanceAlive(handle)) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::StaleHandle, 0U, 0U, 0, 0U);
			return info->status;
		}
		std::shared_ptr<const AnimationAuthorityFrame> authority;
		const AnimationAuthorityRow* published = LoadAuthorityRow(handle, authority);
		AnimationSocketReadSnapshot socketSnapshot;
		if(!CopyOrMaterializeCurrentSocketSnapshot(handle, published, socketSnapshot)) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::NotReady, 0U, 0U, 0, 0U);
			return info->status;
		}
		return CopyAnimationSpan<NativeAnimationSocketSnapshot, AnimationAttachmentTransformSnapshot>(
			socketSnapshot.sockets, values, capacity, info, socketSnapshot.sourceTick,
			socketSnapshot.publicationGeneration,
			[poseGeneration = socketSnapshot.poseGeneration](const AnimationAttachmentTransformSnapshot& source, std::uint32_t) {
				NativeAnimationSocketSnapshot socket{};
				socket.header = AnimationHeader(NativeAnimationRecordKind::SocketSnapshot, sizeof(socket));
				socket.stableId = source.stableId;
				socket.parentBoneStableId = source.parentBoneStableId;
				socket.poseGeneration = poseGeneration;
				socket.modelTransform = AnimationFromTransform(source.modelTransform);
				return socket;
			});
	}

	NativeAnimationStatus Native_Animation_CopyEvents(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationEvent* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus infoStatus = ValidateAnimationRecord(info, NativeAnimationRecordKind::BufferInfo);
		if(infoStatus != NativeAnimationStatus::Ok) return infoStatus;
		if(animator == nullptr) return NativeAnimationStatus::InvalidArgument;
		const AnimatorInstanceHandle handle = AnimationToHandle(*animator);
		if(!AnimationSystem::Instance().IsAnimatorInstanceAlive(handle)) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::StaleHandle, 0U, 0U, 0, 0U);
			return info->status;
		}
		std::shared_ptr<const AnimationAuthorityFrame> authority;
		const AnimationAuthorityRow* published = LoadAuthorityRow(handle, authority);
		if(!published || !published->snapshot || !published->snapshot->pose) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::NotReady, 0U, 0U, 0, 0U);
			return info->status;
		}
		return CopyAnimationSpan<NativeAnimationEvent, AnimationEventRecord>(
			published->snapshot->pose->Events(), values, capacity, info, published->snapshot->tick,
			authority->authorityGeneration,
			[&authority](const AnimationEventRecord& source, std::uint32_t) {
				return MakeNativeAnimationEvent(source, *authority);
			});
	}

	NativeAnimationStatus Native_Animation_CopyEventBatch(
		NativeAnimationEvent* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus infoStatus = ValidateAnimationRecord(info, NativeAnimationRecordKind::BufferInfo);
		if(infoStatus != NativeAnimationStatus::Ok) return infoStatus;
		const std::shared_ptr<const AnimationAuthorityFrame> authority =
			AnimationSystem::Instance().LoadPublishedAuthorityFrame();
		if(!authority || !authority->IsValid() || !authority->sidecars) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::NotReady, 0U, 0U, 0, 0U);
			return info->status;
		}
		// Events were deterministically ordered while the candidate was sealed.  The
		// scripting boundary therefore copies only the sparse event sidecar and never
		// scans every animator or materializes a temporary crowd-wide batch.
		return CopyAnimationSpan<NativeAnimationEvent, AnimationEventRecord>(
			authority->sidecars->events, values, capacity, info,
			static_cast<std::int64_t>(authority->frameId), authority->authorityGeneration,
			[&authority](const AnimationEventRecord& source, std::uint32_t) {
				return MakeNativeAnimationEvent(source, *authority);
			});
	}

	NativeAnimationStatus Native_Animation_ApplyRootMotion(
		const NativeAnimationRootMotionApplicationRequest* request,
		NativeAnimationRootMotionApplicationReceipt* receipt)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus requestStatus = ValidateAnimationRecord(
			request, NativeAnimationRecordKind::RootMotionApplicationRequest);
		if(requestStatus != NativeAnimationStatus::Ok) return requestStatus;
		const NativeAnimationStatus receiptStatus = ValidateAnimationRecord(
			receipt, NativeAnimationRecordKind::RootMotionApplicationReceipt);
		if(receiptStatus != NativeAnimationStatus::Ok) return receiptStatus;

		AnimationRootMotionDeferredApplicationRequest native;
		native.animator = AnimationToHandle(request->animator);
		native.mode = static_cast<AnimationRootMotionApplicationMode>(request->mode);
		native.expectedPoseGeneration = request->expectedPoseGeneration;
		native.expectedPublicationGeneration = request->expectedPublicationGeneration;
		native.producerId = request->producerId;
		native.producerSequence = request->producerSequence;
		native.correlationId = request->correlationId;
		native.idempotencyKey = request->idempotencyKey;
		native.fixedDurationSeconds = request->fixedDurationSeconds;
		const AnimationRootMotionApplicationReceipt nativeReceipt =
			AnimationSystem::Instance().SubmitRootMotionApplication(native);
		FillRootMotionReceipt(nativeReceipt, *receipt);
		return receipt->status;
	}

	NativeAnimationStatus Native_Animation_CopyRootMotionResults(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationRootMotionResult* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus infoStatus = ValidateAnimationRecord(info, NativeAnimationRecordKind::BufferInfo);
		if(infoStatus != NativeAnimationStatus::Ok) return infoStatus;
		if(animator == nullptr) return NativeAnimationStatus::InvalidArgument;
		const AnimatorInstanceHandle handle = AnimationToHandle(*animator);
		if(!AnimationSystem::Instance().IsAnimatorInstanceAlive(handle)) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::StaleHandle, 0U, 0U, 0, 0U);
			return info->status;
		}
		std::vector<AnimationRootMotionApplicationResult> results;
		AnimationRootMotionResultPublisher::Instance().CopyForAnimator(handle, results);
		const std::int64_t sourceTick = results.empty() ? 0 : results.back().sourceEndTick;
		return CopyAnimationSpan<NativeAnimationRootMotionResult, AnimationRootMotionApplicationResult>(
			results, values, capacity, info, sourceTick,
			AnimationSystem::Instance().AuthorityPublicationGeneration(),
			[](const AnimationRootMotionApplicationResult& source, std::uint32_t) {
				NativeAnimationRootMotionResult result{};
				result.header = AnimationHeader(NativeAnimationRecordKind::RootMotionResult, sizeof(result));
				result.status = ToNativeAnimationStatus(source.status);
				result.animator = AnimationFromHandle(source.animator);
				result.disposition = static_cast<NativeAnimationRootMotionDisposition>(source.disposition);
				result.generation = source.generation;
				result.sourceBeginTick = source.sourceBeginTick;
				result.sourceEndTick = source.sourceEndTick;
				result.requestedTranslation = AnimationFromVector(source.requestedTranslation);
				result.appliedTranslation = AnimationFromVector(source.appliedTranslation);
				result.requestedYawRadians = source.requestedYawRadians;
				result.appliedYawRadians = source.appliedYawRadians;
				result.correlationId = source.correlationId;
				result.publicationGeneration = source.publicationGeneration;
				return result;
			});
	}

	NativeAnimationStatus Native_Animation_CopyRootMotionResultDetails(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationRootMotionResultDetails* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus infoStatus = ValidateAnimationRecord(
			info, NativeAnimationRecordKind::BufferInfo);
		if(infoStatus != NativeAnimationStatus::Ok) return infoStatus;
		if(animator == nullptr) return NativeAnimationStatus::InvalidArgument;
		const AnimatorInstanceHandle handle = AnimationToHandle(*animator);
		if(!AnimationSystem::Instance().IsAnimatorInstanceAlive(handle)) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::StaleHandle, 0U, 0U, 0, 0U);
			return info->status;
		}
		std::vector<AnimationRootMotionApplicationResult> results;
		AnimationRootMotionResultPublisher::Instance().CopyForAnimator(handle, results);
		const std::int64_t sourceTick = results.empty() ? 0 : results.back().sourceEndTick;
		return CopyAnimationSpan<NativeAnimationRootMotionResultDetails,
			AnimationRootMotionApplicationResult>(
			results, values, capacity, info, sourceTick,
			AnimationSystem::Instance().AuthorityPublicationGeneration(),
			[](const AnimationRootMotionApplicationResult& source, std::uint32_t) {
				return MakeNativeRootMotionDetails(source);
			});
	}

	NativeAnimationStatus Native_Animation_SubmitProceduralGoal(
		const NativeAnimationProceduralGoalCommand* request,
		NativeAnimationProceduralGoalReceipt* receipt)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus requestStatus = ValidateAnimationRecord(
			request, NativeAnimationRecordKind::ProceduralGoalCommand);
		if(requestStatus != NativeAnimationStatus::Ok) return requestStatus;
		const NativeAnimationStatus receiptStatus = ValidateAnimationRecord(
			receipt, NativeAnimationRecordKind::ProceduralGoalReceipt);
		if(receiptStatus != NativeAnimationStatus::Ok) return receiptStatus;

		// The runtime owns the copied request after this call. Managed gameplay
		// never receives an alias to the evaluator's pending or active goal state.
		*receipt = AnimationSystem::Instance().SubmitProceduralGoal(*request);
		return receipt->status;
	}

	NativeAnimationStatus Native_Animation_CopyProceduralResults(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationProceduralResultSnapshot* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info)
	{
		using namespace TerraEngine::Animation;
		const NativeAnimationStatus infoStatus = ValidateAnimationRecord(
			info, NativeAnimationRecordKind::BufferInfo);
		if(infoStatus != NativeAnimationStatus::Ok) return infoStatus;
		if(animator == nullptr || (capacity != 0U && values == nullptr)) {
			FillAnimationBufferInfo(*info, NativeAnimationStatus::InvalidArgument,
				0U, 0U, 0, 0U);
			return info->status;
		}

		std::uint32_t requiredCount = 0U;
		const AnimationStatus copyStatus = AnimationSystem::Instance().CopyProceduralResults(
			AnimationToHandle(*animator),
			std::span<NativeAnimationProceduralResultSnapshot>(values, capacity),
			requiredCount);
		const NativeAnimationStatus nativeStatus = ToNativeAnimationStatus(copyStatus);
		const bool copied = copyStatus == AnimationStatus::Ok;
		const std::int64_t sourceTick = copied && requiredCount != 0U
			? values[requiredCount - 1U].observationTick : 0;
		const std::uint64_t publicationGeneration = copied && requiredCount != 0U
			? values[requiredCount - 1U].publicationGeneration
			: AnimationSystem::Instance().AuthorityPublicationGeneration();
		// Preserve the ABI's atomic two-call protocol: failed or undersized reads
		// report the exact count but never claim that a partial prefix was written.
		FillAnimationBufferInfo(*info, nativeStatus, requiredCount,
			copied ? requiredCount : 0U, sourceTick, publicationGeneration);
		return info->status;
	}

	bool IsKnownRetargetOperation(NativeAnimationRetargetOperation value) noexcept
	{
		return value == NativeAnimationRetargetOperation::ActivateProfile
			|| value == NativeAnimationRetargetOperation::DeactivateProfile
			|| value == NativeAnimationRetargetOperation::RequestRebuild;
	}

	bool IsKnownMotionMatchOperation(NativeAnimationMotionMatchOperation value) noexcept
	{
		return value == NativeAnimationMotionMatchOperation::SetDatabase
			|| value == NativeAnimationMotionMatchOperation::Query
			|| value == NativeAnimationMotionMatchOperation::Clear;
	}

	bool IsKnownRagdollOperation(NativeAnimationRagdollOperation value) noexcept
	{
		return value == NativeAnimationRagdollOperation::Activate
			|| value == NativeAnimationRagdollOperation::Deactivate
			|| value == NativeAnimationRagdollOperation::RequestRecovery;
	}

	bool IsKnownDeterminismOperation(NativeAnimationDeterminismOperation value) noexcept
	{
		return value == NativeAnimationDeterminismOperation::Capture
			|| value == NativeAnimationDeterminismOperation::Restore
			|| value == NativeAnimationDeterminismOperation::ReplaySeek
			|| value == NativeAnimationDeterminismOperation::RequestReplication;
	}

	NativeAnimationStatus Native_Animation_SubmitRetargetRequest(
		const NativeAnimationRetargetRequest* request,
		NativeAnimationRetargetReceipt* receipt)
	{
		const NativeAnimationStatus requestStatus = ValidateAnimationRecord(
			request, NativeAnimationRecordKind::RetargetRequest);
		if(requestStatus != NativeAnimationStatus::Ok) return requestStatus;
		const NativeAnimationStatus receiptStatus = ValidateAnimationRecord(
			receipt, NativeAnimationRecordKind::RetargetReceipt);
		if(receiptStatus != NativeAnimationStatus::Ok) return receiptStatus;
		if(!IsKnownRetargetOperation(request->operation)) return NativeAnimationStatus::InvalidArgument;
		*receipt = TerraEngine::Animation::AnimationSystem::Instance()
			.SubmitRetargetRequest(*request);
		return receipt->status;
	}

	NativeAnimationStatus Native_Animation_CopyRetargetSnapshots(
		const NativeAnimationAnimatorHandle* animator, NativeAnimationRetargetSnapshot* values,
		std::uint32_t capacity, NativeAnimationBufferInfo* info)
	{
		return CopyAdvancedAnimationSnapshots(animator, values, capacity, info,
			[](TerraEngine::Animation::AnimatorInstanceHandle handle,
				std::span<NativeAnimationRetargetSnapshot> destination,
				std::uint32_t& requiredCount) {
				return TerraEngine::Animation::AnimationSystem::Instance()
					.CopyRetargetSnapshots(
					handle, destination, requiredCount);
			});
	}

	NativeAnimationStatus Native_Animation_SubmitMotionMatchRequest(
		const NativeAnimationMotionMatchRequest* request,
		NativeAnimationMotionMatchReceipt* receipt)
	{
		const NativeAnimationStatus requestStatus = ValidateAnimationRecord(
			request, NativeAnimationRecordKind::MotionMatchRequest);
		if(requestStatus != NativeAnimationStatus::Ok) return requestStatus;
		const NativeAnimationStatus receiptStatus = ValidateAnimationRecord(
			receipt, NativeAnimationRecordKind::MotionMatchReceipt);
		if(receiptStatus != NativeAnimationStatus::Ok) return receiptStatus;
		if(!IsKnownMotionMatchOperation(request->operation)) return NativeAnimationStatus::InvalidArgument;
		*receipt = TerraEngine::Animation::AnimationSystem::Instance()
			.SubmitMotionMatchRequest(*request);
		return receipt->status;
	}

	NativeAnimationStatus Native_Animation_CopyMotionMatchSnapshots(
		const NativeAnimationAnimatorHandle* animator, NativeAnimationMotionMatchSnapshot* values,
		std::uint32_t capacity, NativeAnimationBufferInfo* info)
	{
		return CopyAdvancedAnimationSnapshots(animator, values, capacity, info,
			[](TerraEngine::Animation::AnimatorInstanceHandle handle,
				std::span<NativeAnimationMotionMatchSnapshot> destination,
				std::uint32_t& requiredCount) {
				return TerraEngine::Animation::AnimationSystem::Instance()
					.CopyMotionMatchSnapshots(
					handle, destination, requiredCount);
			});
	}

	NativeAnimationStatus Native_Animation_SubmitMotionMatchQueryV2(
		const NativeAnimationMotionMatchQueryRequestV2* request,
		NativeAnimationMotionMatchQueryReceiptV2* receipt)
	{
		const NativeAnimationStatus requestStatus = ValidateAnimationRecord(
			request, NativeAnimationRecordKind::MotionMatchQueryRequestV2);
		if(requestStatus != NativeAnimationStatus::Ok) return requestStatus;
		const NativeAnimationStatus receiptStatus = ValidateAnimationRecord(
			receipt, NativeAnimationRecordKind::MotionMatchQueryReceiptV2);
		if(receiptStatus != NativeAnimationStatus::Ok) return receiptStatus;
		if(!ValidateMotionMatchQueryPayload(*request)) {
			return NativeAnimationStatus::InvalidArgument;
		}

		// AnimationSystem owns the complete copied record. This keeps the managed
		// bridge pointer-free and ensures every validated V2 field reaches the same
		// protected capture/evaluation/publication lifecycle as native producers.
		*receipt = TerraEngine::Animation::AnimationSystem::Instance()
			.SubmitMotionMatchQueryV2(*request);
		return receipt->status;
	}

	NativeAnimationStatus Native_Animation_CopyMotionMatchResultsV2(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationMotionMatchResultV2* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info)
	{
		return CopyAdvancedAnimationSnapshots(animator, values, capacity, info,
			[](TerraEngine::Animation::AnimatorInstanceHandle handle,
				std::span<NativeAnimationMotionMatchResultV2> destination,
				std::uint32_t& requiredCount) {
				return TerraEngine::Animation::AnimationSystem::Instance()
					.CopyMotionMatchResultsV2(handle, destination, requiredCount);
			});
	}

	NativeAnimationStatus Native_Animation_SubmitRagdollRequest(
		const NativeAnimationRagdollRequest* request,
		NativeAnimationRagdollReceipt* receipt)
	{
		const NativeAnimationStatus requestStatus = ValidateAnimationRecord(
			request, NativeAnimationRecordKind::RagdollRequest);
		if(requestStatus != NativeAnimationStatus::Ok) return requestStatus;
		const NativeAnimationStatus receiptStatus = ValidateAnimationRecord(
			receipt, NativeAnimationRecordKind::RagdollReceipt);
		if(receiptStatus != NativeAnimationStatus::Ok) return receiptStatus;
		if(!IsKnownRagdollOperation(request->operation) || !std::isfinite(request->blendWeight)
			|| request->blendWeight < 0.0F || request->blendWeight > 1.0F) return NativeAnimationStatus::InvalidArgument;
		*receipt = TerraEngine::Animation::AnimationSystem::Instance()
			.SubmitRagdollRequest(*request);
		return receipt->status;
	}

	NativeAnimationStatus Native_Animation_CopyRagdollSnapshots(
		const NativeAnimationAnimatorHandle* animator, NativeAnimationRagdollSnapshot* values,
		std::uint32_t capacity, NativeAnimationBufferInfo* info)
	{
		return CopyAdvancedAnimationSnapshots(animator, values, capacity, info,
			[](TerraEngine::Animation::AnimatorInstanceHandle handle,
				std::span<NativeAnimationRagdollSnapshot> destination,
				std::uint32_t& requiredCount) {
				return TerraEngine::Animation::AnimationSystem::Instance()
					.CopyRagdollSnapshots(
					handle, destination, requiredCount);
			});
	}

	NativeAnimationStatus Native_Animation_SubmitDeterminismRequest(
		const NativeAnimationDeterminismRequest* request,
		NativeAnimationDeterminismReceipt* receipt)
	{
		const NativeAnimationStatus requestStatus = ValidateAnimationRecord(
			request, NativeAnimationRecordKind::DeterminismRequest);
		if(requestStatus != NativeAnimationStatus::Ok) return requestStatus;
		const NativeAnimationStatus receiptStatus = ValidateAnimationRecord(
			receipt, NativeAnimationRecordKind::DeterminismReceipt);
		if(receiptStatus != NativeAnimationStatus::Ok) return receiptStatus;
		if(!IsKnownDeterminismOperation(request->operation) || request->targetTick < request->sourceTick) {
			return NativeAnimationStatus::InvalidArgument;
		}
		*receipt = TerraEngine::Animation::AnimationSystem::Instance()
			.SubmitDeterminismRequest(*request);
		return receipt->status;
	}

	NativeAnimationStatus Native_Animation_CopyDeterminismSnapshots(
		const NativeAnimationAnimatorHandle* animator, NativeAnimationDeterminismSnapshot* values,
		std::uint32_t capacity, NativeAnimationBufferInfo* info)
	{
		return CopyAdvancedAnimationSnapshots(animator, values, capacity, info,
			[](TerraEngine::Animation::AnimatorInstanceHandle handle,
				std::span<NativeAnimationDeterminismSnapshot> destination,
				std::uint32_t& requiredCount) {
				return TerraEngine::Animation::AnimationSystem::Instance()
					.CopyDeterminismSnapshots(
					handle, destination, requiredCount);
			});
	}

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
		// This copied ABI deliberately carries one fixed-width payload for every
		// character command. Validate only the member the requested operation reads:
		// ordinary movement has no pose, while teleport is the sole pose consumer.
		bool validPayload = false;
		switch(request->kind) {
		case NativePhysicsCharacterCommandKind::Move:
			validPayload = IsFinite(request->desiredVelocity);
			break;
		case NativePhysicsCharacterCommandKind::Jump:
			validPayload = std::isfinite(request->scalar) && request->scalar >= 0.0F;
			break;
		case NativePhysicsCharacterCommandKind::Resize:
			validPayload = std::isfinite(request->scalar) && request->scalar > 0.0F;
			break;
		case NativePhysicsCharacterCommandKind::Teleport:
			validPayload = IsFinite(request->pose);
			break;
		case NativePhysicsCharacterCommandKind::RefreshConfiguration:
			validPayload = true;
			break;
		default:
			FillReceipt(*receipt, NativePhysicsStatus::InvalidArgument, {}, "Unknown character command kind.");
			return receipt->status;
		}
		if(!validPayload) { FillReceipt(*receipt, NativePhysicsStatus::InvalidArgument, {}, "Character command contains an invalid operation payload."); return receipt->status; }
		const PhysicsCommandMetadata metadata = Metadata(request->effectiveTick, request->producerId, request->producerSequence, request->correlationId);
		const PhysicsCharacterCommandTarget target = CharacterTarget(*request);
		PhysicsCommand command;
		switch(request->kind) {
		case NativePhysicsCharacterCommandKind::Move: command = PhysicsCommand::MakeCharacterMove(target, ToVector(request->desiredVelocity), metadata); break;
		case NativePhysicsCharacterCommandKind::Jump: command = PhysicsCommand::MakeCharacterJump(target, request->scalar, metadata); break;
		case NativePhysicsCharacterCommandKind::Resize: command = PhysicsCommand::MakeCharacterResize(target, request->scalar, metadata); break;
		case NativePhysicsCharacterCommandKind::Teleport: command = PhysicsCommand::MakeCharacterTeleport(target, ToPose(request->pose), metadata); break;
		case NativePhysicsCharacterCommandKind::RefreshConfiguration: command = PhysicsCommand::MakeCharacterConfigurationUpdate(target, metadata); break;
		default: return NativePhysicsStatus::InvalidArgument;
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

	const bool replacesSceneLifetime = unloadAll || activeScene.has_value()
		|| std::any_of(sceneLoads.begin(), sceneLoads.end(), [](const ManagedSceneLoadRequest& request) {
			return !request.prefab;
		});
	if(replacesSceneLifetime) {
		// Scene activation and replacement revoke the old generation before the
		// object store changes. Worker completions retained from that generation
		// therefore fail their owner check instead of publishing into the new scene.
		TerraEngine::Navigation::NavigationSystem::Instance().RevokeAllOwners(
			TerraEngine::Navigation::NavigationReason::SceneReplaced);
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

	std::uint32_t Native_Audio_GetAbiVersion() { return kNativeAudioAbiVersion; }

	NativeAudioStatus Native_Audio_SubmitCommand(
		const NativeAudioCommandRequest* request,
		NativeAudioCommandReceipt* receipt) {
		if(receipt == nullptr) return NativeAudioStatus::InvalidArgument;
		*receipt = {};
		receipt->header = {kNativeAudioAbiVersion, sizeof(*receipt)};
		if(!IsAudioAbiRecord(request)) {
			receipt->status = NativeAudioStatus::VersionMismatch;
			CopyAudioDiagnostic(receipt->diagnostic, sizeof(receipt->diagnostic), "Audio ABI header does not match v1.");
			return receipt->status;
		}

		ObjectManager& objects = ObjectManager::instance();
		const auto object = objects.getObject(static_cast<entt::entity>(request->objectEntity));
		if(!object) {
			receipt->status = NativeAudioStatus::NotFound;
			return receipt->status;
		}

		TerraEngine::Audio::AudioControlBatch batch;
		batch.requestedFrame = request->requestedFrame;
		batch.producerId = request->producerId;
		batch.correlationId = std::to_string(request->correlationId);
		batch.idempotencyKey = request->idempotencyKey == 0U ? std::string{} : std::to_string(request->idempotencyKey);
		batch.requestFingerprint = std::to_string(request->objectEntity) + ":"
			+ std::to_string(static_cast<std::uint32_t>(request->kind)) + ":"
			+ std::to_string(request->playbackGeneration);
		TerraEngine::Audio::AudioControlCommand command;
		command.source.objectId = objects.getUUID(*object);
		command.source.objectGeneration = object->generation;
		command.source.componentGeneration = request->componentGeneration;
		command.source.playbackGeneration = request->playbackGeneration;
		switch(request->kind) {
		case NativeAudioCommandKind::Play: command.kind = TerraEngine::Audio::AudioControlKind::Play; break;
		case NativeAudioCommandKind::Pause: command.kind = TerraEngine::Audio::AudioControlKind::Pause; break;
		case NativeAudioCommandKind::Resume: command.kind = TerraEngine::Audio::AudioControlKind::Resume; break;
		case NativeAudioCommandKind::Stop: command.kind = TerraEngine::Audio::AudioControlKind::Stop; break;
		default:
			receipt->status = NativeAudioStatus::InvalidArgument;
			return receipt->status;
		}
		batch.commands.push_back(command);
		const auto native = TerraEngine::Audio::AudioSystem::Instance().SubmitControlBatch(std::move(batch));
		receipt->status = ToNativeAudioStatus(native.status);
		receipt->rejection = static_cast<std::uint32_t>(native.rejection);
		receipt->batchSequence = native.batchSequence;
		receipt->requestedFrame = native.requestedFrame;
		receipt->correlationId = request->correlationId;
		CopyAudioDiagnostic(receipt->diagnostic, sizeof(receipt->diagnostic), native.diagnostic);
		return receipt->status;
	}

	NativeAudioStatus Native_Audio_ReadPlayback(
		std::uint32_t objectEntity,
		std::uint32_t componentGeneration,
		NativeAudioPlaybackState* state) {
		if(!IsAudioAbiRecord(state)) return NativeAudioStatus::VersionMismatch;
		const NativeAudioAbiHeader header = state->header;
		*state = {};
		state->header = header;
		ObjectManager& objects = ObjectManager::instance();
		const auto object = objects.getObject(static_cast<entt::entity>(objectEntity));
		const auto snapshot = TerraEngine::Audio::AudioSystem::Instance().LatestPlaybackSnapshot();
		if(!object || !snapshot) return state->status = NativeAudioStatus::NotFound;
		const TerraEngine::UUID uuid = objects.getUUID(*object);
		const auto found = std::find_if(snapshot->voices.begin(), snapshot->voices.end(), [&](const auto& voice) {
			return voice.identity.objectId == uuid && voice.identity.componentGeneration == componentGeneration;
		});
		if(found == snapshot->voices.end()) return state->status = NativeAudioStatus::NotFound;
		state->status = NativeAudioStatus::Success;
		state->state = found->paused ? 2U : 1U;
		state->playbackGeneration = found->identity.playbackGeneration;
		state->audible = found->audibility == TerraEngine::Audio::AudioAudibility::Audible ? 1U : 0U;
		state->frame = snapshot->frame;
		state->logicalFrame = found->logicalFrame;
		state->gain = found->gain;
		state->pitch = found->pitch;
		return state->status;
	}

	NativeAudioStatus Native_Audio_ReadMasterMix(NativeAudioMixState* state) {
		if(!IsAudioAbiRecord(state)) return NativeAudioStatus::VersionMismatch;
		const NativeAudioAbiHeader header = state->header;
		*state = {};
		state->header = header;
		const auto snapshot = TerraEngine::Audio::AudioSystem::Instance().LatestMixSnapshot();
		state->status = NativeAudioStatus::Success;
		state->muted = snapshot.master.muted ? 1U : 0U;
		state->generation = snapshot.generation;
		state->volume = snapshot.master.volume;
		return state->status;
	}

	NativeAudioStatus Native_Audio_SetMasterMix(const NativeAudioMixState* state) {
		if(!IsAudioAbiRecord(state) || !std::isfinite(state->volume)) return NativeAudioStatus::InvalidArgument;
		std::string diagnostic;
		return TerraEngine::Audio::AudioSystem::Instance().SetMasterMix(state->volume, state->muted != 0U, diagnostic)
			? NativeAudioStatus::Accepted : NativeAudioStatus::InvalidArgument;
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
		// Transform is spatial authority and cannot pass through the lossy generic
		// reflection bridge. Managed callers must use the canonical ABI below.
		if (componentType == nullptr || propertyName == nullptr || outValue == nullptr
			|| std::string_view(componentType) == Transform::kComponentName) {
			return;
		}
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
		if (componentType == nullptr || propertyName == nullptr || inValue == nullptr
			|| std::string_view(componentType) == Transform::kComponentName) {
			return;
		}
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
					const bool propertyWritten = it->second.entitySetter(
						objects.getRegistry(), entity, propertyName, val);
					if (propertyWritten
						&& std::string_view(componentType) != Transform::kComponentName) {
						// Reflected Transform setters publish their spatial revision
						// centrally. Other compatibility writes still invalidate
						// the component/structure generation through this bridge.
						objects.notifyObjectStructureChanged(*object);
					}
				}
			}
		}
	}

	NativeRuntimeStatus Native_Transform_ReadWorldPose(uint32_t objectId, NativeWorldPose* pose) {
		if (pose == nullptr) {
			return NativeRuntimeStatus::InvalidArgument;
		}

		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		ObjectManager& objects = ObjectManager::instance();
		const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId));
		if (!object.has_value()) {
			return NativeRuntimeStatus::NotFound;
		}
		const std::optional<TerraEngine::Scene::WorldPose> worldPose = objects.GetCanonicalWorldPose(*object);
		if (!worldPose.has_value()) {
			return NativeRuntimeStatus::NotFound;
		}

		pose->position = {
			{worldPose->position.cell.x, worldPose->position.cell.y, worldPose->position.cell.z},
			worldPose->position.offsetMetres.x,
			worldPose->position.offsetMetres.y,
			worldPose->position.offsetMetres.z};
		pose->rotation = {worldPose->rotation.x, worldPose->rotation.y, worldPose->rotation.z, worldPose->rotation.w};
		pose->scale = {worldPose->scale.x, worldPose->scale.y, worldPose->scale.z};
		pose->transformRevision = worldPose->transformRevision;
		pose->coordinateFrameRevision = worldPose->coordinateFrameRevision;
		return NativeRuntimeStatus::Success;
	}

	NativeRuntimeStatus Native_Transform_SubmitWorldPose(uint32_t objectId, const NativeWorldPose* pose) {
		if (pose == nullptr) {
			return NativeRuntimeStatus::InvalidArgument;
		}

		TerraEngine::Scene::WorldPose requested;
		requested.position = {
			{pose->position.cell.x, pose->position.cell.y, pose->position.cell.z},
			{pose->position.offsetX, pose->position.offsetY, pose->position.offsetZ}};
		requested.rotation = {pose->rotation.w, pose->rotation.x, pose->rotation.y, pose->rotation.z};
		requested.scale = {pose->scale.x, pose->scale.y, pose->scale.z};
		requested.transformRevision = pose->transformRevision;
		requested.coordinateFrameRevision = pose->coordinateFrameRevision;
		const TerraEngine::Scene::WorldCoordinateResult normalized = TerraEngine::Scene::NormalizeWorldPosition(requested.position);
		if (!normalized.Succeeded() || !(normalized.position == requested.position)
			|| !std::isfinite(requested.scale.x) || !std::isfinite(requested.scale.y) || !std::isfinite(requested.scale.z)
			|| !std::isfinite(requested.rotation.x) || !std::isfinite(requested.rotation.y)
			|| !std::isfinite(requested.rotation.z) || !std::isfinite(requested.rotation.w)) {
			return NativeRuntimeStatus::InvalidArgument;
		}

		TerraEngine::Scene::Transaction transaction;
		TerraEngine::UUID objectUuid = TerraEngine::EMPTY_UUID;
		{
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId));
			if (!object.has_value()) {
				return NativeRuntimeStatus::NotFound;
			}
			objectUuid = objects.getUUID(*object);
			TerraEngine::Scene::TransactionalMutationBuilder builder(
				"managed-world-pose-" + TerraEngine::ToHexString(objectUuid),
				TerraEngine::Scene::TransactionOrigin::ManagedCallback,
				"managed-canonical-transform");
			builder.SetWorldTransform(objects, objectUuid, requested);
			transaction = std::move(builder).Build();
		}

		if (const auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
			auto request = scriptingSystem.BuildScriptMutationRequest(
				TerraEngine::Scene::SceneMutationOperation::SetWorldTransform,
				*context,
				objectUuid);
			request.typedSpatialTransaction = std::move(transaction);
			scriptingSystem.RecordScriptMutationRequest(std::move(request));
		} else {
			TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(std::move(transaction));
		}
		return NativeRuntimeStatus::Accepted;
	}

	NativeRuntimeStatus Native_Transform_TranslateWorld(uint32_t objectId, const NativeWorldDelta* delta) {
		if (delta == nullptr) {
			return NativeRuntimeStatus::InvalidArgument;
		}
		TerraEngine::Scene::WorldDelta requested{
			{delta->cellDelta.x, delta->cellDelta.y, delta->cellDelta.z},
			{delta->offsetX, delta->offsetY, delta->offsetZ}};
		const TerraEngine::Scene::WorldDeltaResult normalized = TerraEngine::Scene::NormalizeWorldDelta(requested);
		if (!normalized.Succeeded() || !(normalized.delta == requested)) {
			return NativeRuntimeStatus::InvalidArgument;
		}

		TerraEngine::Scene::Transaction transaction;
		TerraEngine::UUID objectUuid = TerraEngine::EMPTY_UUID;
		{
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			ObjectManager& objects = ObjectManager::instance();
			const std::optional<Object> object = objects.getObject(static_cast<entt::entity>(objectId));
			if (!object.has_value()) {
				return NativeRuntimeStatus::NotFound;
			}
			objectUuid = objects.getUUID(*object);
			TerraEngine::Scene::TransactionalMutationBuilder builder(
				"managed-world-delta-" + TerraEngine::ToHexString(objectUuid),
				TerraEngine::Scene::TransactionOrigin::ManagedCallback,
				"managed-canonical-transform");
			builder.TranslateWorld(objects, objectUuid, requested);
			transaction = std::move(builder).Build();
		}

		if (const auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value()) {
			ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
			auto request = scriptingSystem.BuildScriptMutationRequest(
				TerraEngine::Scene::SceneMutationOperation::TranslateWorld,
				*context,
				objectUuid);
			request.typedSpatialTransaction = std::move(transaction);
			scriptingSystem.RecordScriptMutationRequest(std::move(request));
		} else {
			TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(std::move(transaction));
		}
		return NativeRuntimeStatus::Accepted;
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
