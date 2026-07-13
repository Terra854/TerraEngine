// ScriptGlue.h — Contains declarations for the Script Glue file-specific types, helper functions, and module integration code used by native script glue, exported C# vtables, and scripting system lifecycle state.

#pragma once

#include "Core/Export.h"
#include <cstdint>

// The physics bridge deliberately uses fixed-width C records. Managed and
// NativeAOT callers copy these values; they never borrow a provider pointer,
// an STL container, or runtime-owned string storage.
inline constexpr std::uint32_t kNativePhysicsAbiVersion = 1U;
inline constexpr std::uint32_t kNativePhysicsDiagnosticBytes = 192U;

#pragma pack(push, 8)
struct NativePhysicsAbiHeader { std::uint32_t abiVersion; std::uint32_t structSize; };
struct NativePhysicsUuid { std::uint64_t first; std::uint64_t second; };
struct NativePhysicsWorldHandle { std::uint64_t worldId; std::uint32_t worldGeneration; std::uint32_t reserved; };
struct NativePhysicsHandle { NativePhysicsWorldHandle world; std::uint32_t slotIndex; std::uint32_t slotGeneration; };
struct NativePhysicsQueryHandle { NativePhysicsWorldHandle world; std::uint32_t resultSlot; std::uint32_t resultGeneration; std::uint64_t submittedTick; std::uint64_t expiryTick; };
struct NativePhysicsVector3 { float x; float y; float z; };
struct NativePhysicsQuaternion { float x; float y; float z; float w; };
struct NativePhysicsPose { NativePhysicsVector3 position; NativePhysicsQuaternion rotation; };

enum class NativePhysicsStatus : std::uint32_t {
	Success = 0U, Accepted, InvalidArgument, InvalidState, StaleTarget,
	LimitExceeded, Conflict, Cancelled, Expired, ProviderFailure, VersionMismatch
};

enum class NativePhysicsBodyCommandKind : std::uint32_t {
	Force = 0U, Acceleration, Impulse, PointImpulse, VelocityChange,
	SetVelocity, Teleport, KinematicTarget, Wake, Sleep
};

enum class NativePhysicsCharacterCommandKind : std::uint32_t { Move = 0U, Jump, Resize, Teleport, RefreshConfiguration };
enum class NativePhysicsQueryKind : std::uint32_t { RayCast = 0U, ShapeCast, Overlap, Point };

struct NativePhysicsIdentity {
	NativePhysicsUuid objectId;
	std::uint32_t objectGeneration;
	std::uint32_t componentGeneration;
	NativePhysicsUuid recordId;
	std::uint32_t recordGeneration;
	std::uint32_t bindingGeneration;
};

struct NativePhysicsBodyCommandRequest {
	NativePhysicsAbiHeader header;
	NativePhysicsHandle body;
	NativePhysicsIdentity identity;
	NativePhysicsBodyCommandKind kind;
	std::uint32_t reserved;
	NativePhysicsVector3 value;
	NativePhysicsVector3 point;
	NativePhysicsPose pose;
	NativePhysicsVector3 angularValue;
	float duration;
	std::uint64_t effectiveTick;
	std::uint64_t producerId;
	std::uint64_t producerSequence;
	std::uint64_t correlationId;
};

struct NativePhysicsCharacterCommandRequest {
	NativePhysicsAbiHeader header;
	NativePhysicsHandle character;
	NativePhysicsIdentity identity;
	NativePhysicsCharacterCommandKind kind;
	std::uint32_t reserved;
	NativePhysicsVector3 desiredVelocity;
	NativePhysicsPose pose;
	float scalar;
	std::uint64_t effectiveTick;
	std::uint64_t producerId;
	std::uint64_t producerSequence;
	std::uint64_t correlationId;
};

struct NativePhysicsCommandReceipt {
	NativePhysicsAbiHeader header;
	NativePhysicsStatus status;
	std::uint32_t reserved;
	std::uint64_t commandId;
	std::uint64_t effectiveTick;
	std::uint64_t correlationId;
	char diagnostic[kNativePhysicsDiagnosticBytes];
};

struct NativePhysicsQueryRequest {
	NativePhysicsAbiHeader header;
	NativePhysicsWorldHandle world;
	NativePhysicsQueryKind kind;
	std::uint32_t hitMode;
	NativePhysicsPose pose;
	NativePhysicsVector3 direction;
	NativePhysicsVector3 shapeExtent;
	float distance;
	std::uint32_t maximumHits;
	std::uint32_t collisionMask;
	std::uint32_t bodyTypeMask;
	std::uint8_t includeSensors;
	std::uint8_t reserved[7];
	std::uint64_t submittedTick;
	std::uint64_t expiryTick;
	std::uint64_t correlationId;
};

struct NativePhysicsQueryReceipt {
	NativePhysicsAbiHeader header;
	NativePhysicsStatus status;
	std::uint32_t reserved;
	NativePhysicsQueryHandle handle;
	std::uint64_t correlationId;
	char diagnostic[kNativePhysicsDiagnosticBytes];
};

struct NativePhysicsQueryResultInfo {
	NativePhysicsAbiHeader header;
	NativePhysicsStatus status;
	std::uint32_t hitCount;
	std::uint32_t truncated;
	std::uint64_t tick;
	std::uint64_t correlationId;
	char diagnostic[kNativePhysicsDiagnosticBytes];
};

struct NativePhysicsQueryHit {
	NativePhysicsHandle body;
	NativePhysicsUuid objectId;
	std::uint32_t objectGeneration;
	std::uint32_t componentGeneration;
	NativePhysicsUuid materialId;
	NativePhysicsVector3 position;
	NativePhysicsVector3 normal;
	float distance;
	float fraction;
	std::uint32_t subShapeKey;
	std::uint8_t collisionLayer;
	std::uint8_t motionType;
	std::uint8_t sensor;
	std::uint8_t reserved;
};

struct NativePhysicsBodyState { NativePhysicsAbiHeader header; NativePhysicsHandle body; NativePhysicsIdentity identity; NativePhysicsPose pose; NativePhysicsVector3 linearVelocity; NativePhysicsVector3 angularVelocity; std::uint64_t tick; std::uint8_t sleeping; std::uint8_t reserved[7]; };
struct NativePhysicsCharacterState { NativePhysicsAbiHeader header; NativePhysicsHandle character; NativePhysicsIdentity identity; NativePhysicsPose pose; NativePhysicsVector3 linearVelocity; NativePhysicsVector3 groundNormal; NativePhysicsVector3 supportVelocity; NativePhysicsHandle supportBody; std::uint64_t tick; std::uint32_t groundState; std::uint32_t contactCount; std::uint8_t supported; std::uint8_t reserved[7]; };

struct NativePhysicsEventSubscriptionRequest { NativePhysicsAbiHeader header; std::uint32_t categoryMask; std::uint32_t collisionLayerMask; std::uint8_t includeSensors; std::uint8_t includeContactDetails; std::uint8_t reserved[6]; };
struct NativePhysicsEventSubscription { NativePhysicsAbiHeader header; NativePhysicsStatus status; std::uint32_t generation; std::uint64_t id; char diagnostic[kNativePhysicsDiagnosticBytes]; };
struct NativePhysicsEvent { NativePhysicsAbiHeader header; std::uint32_t type; std::uint32_t hasSecondParticipant; NativePhysicsWorldHandle world; std::uint64_t tick; NativePhysicsIdentity first; NativePhysicsIdentity second; NativePhysicsVector3 point; NativePhysicsVector3 normal; NativePhysicsVector3 relativeVelocity; float penetration; float impulse; std::uint64_t correlationId; };
struct NativePhysicsSettingsSnapshot { NativePhysicsAbiHeader header; NativePhysicsStatus status; std::uint32_t settingsGeneration; NativePhysicsVector3 gravity; float fixedTimeStep; std::uint32_t maxSubsteps; std::uint32_t maxQueryBatchSize; std::uint32_t maxHitsPerQuery; std::uint32_t maxEventHistory; std::uint64_t worldId; std::uint32_t worldGeneration; std::uint32_t reserved; char diagnostic[kNativePhysicsDiagnosticBytes]; };
struct NativePhysicsIdentitySnapshot { NativePhysicsAbiHeader header; NativePhysicsStatus status; std::uint32_t reserved; NativePhysicsIdentity identity; char diagnostic[kNativePhysicsDiagnosticBytes]; };
#pragma pack(pop)

class Engine;

// The editor executable registers a plain function pointer so gameplay code can
// stop Play Mode without the engine DLL depending directly on editor headers.
using ApplicationEditorStopPlayModeCallback = void (*)();

// MainEngine registers the active host engine so Application.Quit() can signal
// the standalone runtime loop from inside the script bridge.
TERRAENGINE_API void SetApplicationHostEngine(Engine* engine);
TERRAENGINE_API void SetApplicationEditorStopPlayModeCallback(ApplicationEditorStopPlayModeCallback callback);

extern "C" {
	// Logging
	TERRAENGINE_API void Native_Log(int level, const char* message, const char* function, int line);

	// Application control and metadata
	TERRAENGINE_API void Native_Application_Quit();
	TERRAENGINE_API void Native_Application_OpenURL(const char* url);
	TERRAENGINE_API const char* Native_Application_GetPersistentDataPath();
	TERRAENGINE_API const char* Native_Application_GetStreamingAssetsPath();
	TERRAENGINE_API bool Native_Application_IsEditor();
	TERRAENGINE_API std::uint32_t Native_Application_GetPlatform();
	TERRAENGINE_API const char* Native_Application_GetProductName();
	TERRAENGINE_API const char* Native_Application_GetVersion();

	// Object Management
	TERRAENGINE_API uint32_t Native_Object_Create(const char* name);
	TERRAENGINE_API void Native_Object_Destroy(uint32_t objectId);
	TERRAENGINE_API const char* Native_Object_GetName(uint32_t objectId);
	TERRAENGINE_API void Native_Object_SetName(uint32_t objectId, const char* name);
	TERRAENGINE_API uint32_t Native_Object_Find(const char* name);
	TERRAENGINE_API uint32_t Native_Object_FindWithTag(const char* tag);
	TERRAENGINE_API const char* Native_MeshRenderer_GetMesh(uint32_t objectId);
	TERRAENGINE_API void Native_MeshRenderer_SetMesh(uint32_t objectId, const char* assetReference);
	TERRAENGINE_API const char* Native_MeshRenderer_GetMaterial(uint32_t objectId);
	TERRAENGINE_API void Native_MeshRenderer_SetMaterial(uint32_t objectId, const char* assetReference);

	// Reflection-backed component access
	TERRAENGINE_API bool Native_HasComponent(uint32_t objectId, const char* componentType);
	TERRAENGINE_API bool Native_AddComponent(uint32_t objectId, const char* componentType);
	TERRAENGINE_API std::uint64_t Native_Object_GetMetadataGeneration(uint32_t objectId);
	TERRAENGINE_API std::uint64_t Native_Object_GetComponentGeneration(uint32_t objectId);
	TERRAENGINE_API std::uint64_t Native_Object_GetBindingGeneration(uint32_t objectId);
	TERRAENGINE_API void Native_GetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* outValue, int type);
	TERRAENGINE_API void Native_SetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* inValue, int type);

	// Runtime UI queries let gameplay scripts react to authored .tuiml buttons
	// without reaching into editor-only state or native engine internals.
	TERRAENGINE_API bool Native_UI_WasElementClicked(const char* elementId);
	TERRAENGINE_API bool Native_UI_IsElementHovered(const char* elementId);
	TERRAENGINE_API std::uint64_t Native_UI_GetInteractionFrame();
	TERRAENGINE_API void* Native_UI_CreateElement(int elementType);
	TERRAENGINE_API void Native_UI_ReleaseHandle(void* elementHandle);
	TERRAENGINE_API void* Native_UI_GetRootElement(uint32_t objectId);
	TERRAENGINE_API void* Native_UI_FindElementById(void* elementHandle, const char* elementId);
	TERRAENGINE_API int Native_UI_GetElementType(void* elementHandle);
	TERRAENGINE_API bool Native_UI_IsHandleAlive(void* elementHandle);
	TERRAENGINE_API const char* Native_UI_GetElementId(void* elementHandle);
	TERRAENGINE_API void Native_UI_SetElementId(void* elementHandle, const char* elementId);
	TERRAENGINE_API const char* Native_UI_GetElementText(void* elementHandle);
	TERRAENGINE_API void Native_UI_SetElementText(void* elementHandle, const char* text);
	TERRAENGINE_API bool Native_UI_GetElementVisible(void* elementHandle);
	TERRAENGINE_API void Native_UI_SetElementVisible(void* elementHandle, bool visible);
	TERRAENGINE_API void Native_UI_AddChild(void* parentHandle, void* childHandle);
	TERRAENGINE_API void Native_UI_RemoveChild(void* parentHandle, void* childHandle);
	TERRAENGINE_API void Native_UI_ClearChildren(void* parentHandle);
	TERRAENGINE_API void Native_UI_SetStyleFloat(void* elementHandle, int property, float value);
	TERRAENGINE_API float Native_UI_GetStyleFloat(void* elementHandle, int property);
	TERRAENGINE_API void Native_UI_SetStyleColor(void* elementHandle, int property, float x, float y, float z, float w);
	TERRAENGINE_API void Native_UI_GetStyleColor(void* elementHandle, int property, float* outX, float* outY, float* outZ, float* outW);
	TERRAENGINE_API bool Native_UI_SetEventCallback(void* elementHandle, int eventType, bool enabled);

	// Scene transactions let managed callbacks submit structural/reflection
	// writes as one validated batch instead of mutating live ObjectManager state.
	TERRAENGINE_API const char* Native_Scene_SubmitTransactionJson(const char* transactionJson);

	// Script binding helpers bypass the generic property layer because gameplay
	// scripts live inside one ordered binding container component.
	TERRAENGINE_API bool Native_ScriptComponent_AddBinding(uint32_t objectId, const char* className);
	TERRAENGINE_API bool Native_ScriptComponent_HasBinding(uint32_t objectId, const char* className);
	TERRAENGINE_API const char* Native_ScriptComponent_GetBindingPayload(uint32_t objectId, const char* bindingId);
	TERRAENGINE_API std::uint64_t Native_ScriptComponent_GetBindingVersion(uint32_t objectId, const char* bindingId);
	TERRAENGINE_API void Native_ScriptComponent_ReportObservedVersion(uint32_t objectId, const char* bindingId, std::uint64_t observedVersion);
	TERRAENGINE_API bool Native_ScriptComponent_SetBindingValue(uint32_t objectId, const char* bindingId, const char* fieldPath, const char* valueKind, const char* valueJson, const char* compatibility, const char* sourceTypeFingerprint);
	TERRAENGINE_API bool Native_ScriptComponent_RemoveBindingValue(uint32_t objectId, const char* bindingId, const char* fieldPath);
	TERRAENGINE_API void Native_Scripting_PushDispatchContext(uint32_t objectId, const char* bindingId, std::uint64_t frameOrder, int workerDispatch, int exportMode, std::uint64_t objectGeneration, std::uint64_t componentGeneration, std::uint64_t bindingGeneration);
	TERRAENGINE_API void Native_Scripting_PopDispatchContext();
	TERRAENGINE_API void Native_Scripting_ReportBridgeCacheCounters(std::uint64_t hits, std::uint64_t misses, std::uint64_t avoidedCalls);

	// Physics ABI v1. Every record begins with NativePhysicsAbiHeader and is
	// copied across the boundary, making reload and world teardown safe.
	TERRAENGINE_API std::uint32_t Native_Physics_GetAbiVersion();
	TERRAENGINE_API NativePhysicsStatus Native_Physics_SubmitBodyCommand(const NativePhysicsBodyCommandRequest* request, NativePhysicsCommandReceipt* receipt);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_SubmitCharacterCommand(const NativePhysicsCharacterCommandRequest* request, NativePhysicsCommandReceipt* receipt);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_SubmitQuery(const NativePhysicsQueryRequest* request, NativePhysicsQueryReceipt* receipt);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_SubmitQueryBatch(const NativePhysicsQueryRequest* requests, std::uint32_t requestCount, NativePhysicsQueryReceipt* receipts, std::uint32_t receiptCapacity, std::uint32_t* requiredReceiptCount);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_ReadQuery(const NativePhysicsQueryHandle* handle, NativePhysicsQueryResultInfo* resultInfo, NativePhysicsQueryHit* hits, std::uint32_t hitCapacity, std::uint32_t* requiredHitCount);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_CancelQuery(const NativePhysicsQueryHandle* handle);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_CopyBodyStates(NativePhysicsBodyState* states, std::uint32_t stateCapacity, std::uint32_t* requiredStateCount);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_CopyCharacterStates(NativePhysicsCharacterState* states, std::uint32_t stateCapacity, std::uint32_t* requiredStateCount);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_SubscribeEvents(const NativePhysicsEventSubscriptionRequest* request, NativePhysicsEventSubscription* subscription);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_UnsubscribeEvents(const NativePhysicsEventSubscription* subscription);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_CopyEvents(NativePhysicsEvent* events, std::uint32_t eventCapacity, std::uint32_t* requiredEventCount, std::uint64_t* sourceTick);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_ReadSettings(NativePhysicsSettingsSnapshot* settings);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_ResolveIdentity(std::uint32_t objectEntity, NativePhysicsIdentitySnapshot* identity);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_ResolveBody(std::uint32_t objectEntity, NativePhysicsBodyState* state);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_ResolveCharacter(std::uint32_t objectEntity, NativePhysicsCharacterState* state);
}
