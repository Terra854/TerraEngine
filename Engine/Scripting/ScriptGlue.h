// ScriptGlue.h — Contains declarations for the Script Glue file-specific types, helper functions, and module integration code used by native script glue, exported C# vtables, and scripting system lifecycle state.

#pragma once

#include "AnimationScriptAbi.h"
#include "Core/Export.h"
#include <cstddef>
#include <cstdint>

// The physics bridge deliberately uses fixed-width C records. Managed and
// NativeAOT callers copy these values; they never borrow a provider pointer,
// an STL container, or runtime-owned string storage.
inline constexpr std::uint32_t kNativePhysicsAbiVersion = 1U;
inline constexpr std::uint32_t kNativePhysicsDiagnosticBytes = 192U;
inline constexpr std::uint32_t kNativePhysicsLayerNameBytes = 64U;
inline constexpr std::uint32_t kNativePhysicsCollisionLayerCount = 32U;
inline constexpr std::uint32_t kNativeGraphicsAbiVersion = 1U;
inline constexpr std::uint32_t kNativeRuntimeAbiVersion = 1U;
inline constexpr std::uint32_t kNativeRuntimeDiagnosticBytes = 192U;
inline constexpr std::uint32_t kNativeInputKeyCapacity = 128U;
inline constexpr std::uint32_t kNativeInputMouseButtonCapacity = 8U;
inline constexpr std::uint32_t kNativeAudioAbiVersion = 1U;
inline constexpr std::uint32_t kNativeAudioDiagnosticBytes = 192U;

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

// Complete project/runtime physics policy. Layer names use bounded UTF-8 bytes
// so the managed bridge never borrows native strings or STL storage.
struct NativePhysicsCollisionLayer {
	std::uint32_t id;
	std::uint32_t broadPhaseGroup;
	std::uint32_t collisionMask;
	std::uint32_t enabled;
	char name[kNativePhysicsLayerNameBytes];
};

struct NativePhysicsSettingsRecord {
	NativePhysicsAbiHeader header;
	NativePhysicsStatus status;
	std::uint32_t settingsGeneration;
	NativePhysicsVector3 gravity;
	float fixedTimeStep;
	std::uint32_t maxSubsteps;
	float maxAccumulatedTime;
	std::uint32_t velocityIterations;
	std::uint32_t positionIterations;
	float maxCastDistance;
	float maxShapeExtent;
	std::uint32_t maxHitsPerQuery;
	std::uint32_t maxRequestsPerBatch;
	std::uint32_t retainedResultTicks;
	std::uint32_t debugVisualizationEnabled;
	std::uint32_t retainedDebugTicks;
	std::uint32_t maxDebugPrimitives;
	std::uint32_t transformPublicationChunkSize;
	std::uint32_t providerExtractionBatchSize;
	std::uint32_t parallelPreparationThreshold;
	std::uint32_t parallelExtractionThreshold;
	std::uint32_t aggregateDiagnosticsIntervalFrames;
	std::uint32_t continuousCollisionEnabled;
	std::uint64_t worldId;
	std::uint32_t worldGeneration;
	std::uint32_t reserved;
	NativePhysicsCollisionLayer layers[kNativePhysicsCollisionLayerCount];
	char diagnostic[kNativePhysicsDiagnosticBytes];
};
struct NativePhysicsIdentitySnapshot { NativePhysicsAbiHeader header; NativePhysicsStatus status; std::uint32_t reserved; NativePhysicsIdentity identity; char diagnostic[kNativePhysicsDiagnosticBytes]; };

// Collider authoring uses one flattened tagged record. Shape-specific fields
// remain present for every tag so managed code never has to marshal a C++
// variant or retain native storage across a scripting call.
struct NativePhysicsColliderRecord {
	NativePhysicsAbiHeader header;
	NativePhysicsUuid recordId;
	std::uint32_t recordVersion;
	std::uint32_t shapeKind;
	NativePhysicsVector3 localCenter;
	NativePhysicsQuaternion localRotation;
	NativePhysicsVector3 halfExtents;
	float radius;
	float height;
	float topRadius;
	float bottomRadius;
	NativePhysicsUuid colliderAssetId;
	NativePhysicsUuid materialId;
	std::uint32_t queryFlags;
	std::uint8_t collisionLayerId;
	std::uint8_t eventPolicy;
	std::uint8_t useDefaultMaterial;
	std::uint8_t isTrigger;
	std::uint8_t useLayerOverride;
	std::uint8_t enabled;
	std::uint8_t hasDebugColorOverride;
	std::uint8_t reserved;
	float debugRed;
	float debugGreen;
	float debugBlue;
	float debugAlpha;
};

struct NativePhysicsConstraintLimits { std::uint32_t enabled; float minimum; float maximum; };
struct NativePhysicsConstraintSpring { std::uint32_t enabled; float stiffness; float damping; };
struct NativePhysicsConstraintMotor { std::uint32_t mode; float targetPosition; float targetVelocity; float maximumForce; };
struct NativePhysicsConstraintAxisSettings {
	NativePhysicsConstraintLimits limits;
	NativePhysicsConstraintSpring spring;
	NativePhysicsConstraintMotor motor;
};

// Six-DOF axis settings are stored in translation XYZ, then rotation XYZ order.
// A fixed six-element array keeps the packet pointer-free and versionable.
struct NativePhysicsConstraintRecord {
	NativePhysicsAbiHeader header;
	NativePhysicsUuid recordId;
	std::uint32_t recordVersion;
	std::uint32_t kind;
	std::uint32_t firstEndpointKind;
	std::uint32_t secondEndpointKind;
	NativePhysicsUuid firstObjectId;
	NativePhysicsUuid secondObjectId;
	NativePhysicsPose firstFrame;
	NativePhysicsPose secondFrame;
	NativePhysicsVector3 firstAxis;
	NativePhysicsVector3 secondAxis;
	NativePhysicsConstraintLimits limits;
	NativePhysicsConstraintSpring spring;
	NativePhysicsConstraintMotor motor;
	NativePhysicsConstraintAxisSettings axisSettings[6];
	NativePhysicsUuid firstGearConstraintId;
	NativePhysicsUuid secondGearConstraintId;
	float gearRatio;
	float breakForce;
	float breakTorque;
	std::uint32_t resetGeneration;
	std::uint8_t collideConnected;
	std::uint8_t enabled;
	std::uint8_t reserved[6];
};
#pragma pack(pop)

// These assertions turn accidental ABI drift into a compile error. The managed
// mirrors use the same Pack=8 layout, so pointer-free identity packets remain
// safe to copy between a NativeAOT game and the engine DLL.
static_assert(sizeof(NativePhysicsAbiHeader) == 8U);
static_assert(sizeof(NativePhysicsUuid) == 16U);
static_assert(sizeof(NativePhysicsWorldHandle) == 16U);
static_assert(offsetof(NativePhysicsBodyCommandRequest, header) == 0U);
static_assert(offsetof(NativePhysicsCharacterCommandRequest, header) == 0U);
static_assert(offsetof(NativePhysicsQueryRequest, header) == 0U);
static_assert(sizeof(NativePhysicsColliderRecord) == 152U);
static_assert(sizeof(NativePhysicsConstraintAxisSettings) == 40U);
static_assert(sizeof(NativePhysicsConstraintRecord) == 488U);
static_assert(sizeof(NativePhysicsCollisionLayer) == 80U);
static_assert(sizeof(NativePhysicsSettingsRecord) == 2872U);
static_assert(offsetof(NativePhysicsColliderRecord, header) == 0U);
static_assert(offsetof(NativePhysicsConstraintRecord, header) == 0U);

// Graphics settings cross the managed boundary as an explicitly flattened,
// pointer-free record. It is intentionally separate from the native renderer
// structure so adding an internal field cannot silently overrun a C# buffer.
#pragma pack(push, 8)
struct NativeGraphicsAbiHeader { std::uint32_t abiVersion; std::uint32_t structSize; };

enum class NativeGraphicsStatus : std::uint32_t
{
	Success = 0U,
	Accepted = 1U,
	InvalidArgument = 2U,
	VersionMismatch = 3U,
	InvalidPreset = 4U
};

struct NativeGraphicsRenderSettings
{
	NativeGraphicsAbiHeader header;
	std::uint32_t shadowResolution;
	std::uint32_t shadowFilteringSize;
	std::uint32_t maxUploadedLights;
	std::uint32_t maxDynamicLights;
	std::uint32_t maxShadowLights;
	std::uint32_t maxAreaLights;
	std::uint32_t enableLTC;
	std::uint32_t enableIBL;
	float indirectStrength;
	float ambientOcclusionStrength;
	std::uint32_t ambientOcclusionQuality;
	std::uint32_t anisotropicFiltering;
	std::uint32_t variableRateShading;
	std::uint32_t validationEnabled;
	std::uint32_t debugEnabled;
	float virtualGeometryLodErrorScale;
	std::uint32_t virtualGeometryMaxLodDrop;
	std::uint32_t virtualGeometryHiZOcclusionEnabled;
	std::uint32_t virtualGeometryHiZSelfOcclusionGuardEnabled;
	std::uint32_t virtualGeometryHiZNearPlaneGuardEnabled;
	std::uint32_t virtualGeometryHiZThinGeometryGuardEnabled;
	std::uint32_t virtualGeometryDebugVisualizationMode;
	std::uint64_t virtualGeometryBudgetBytes;
	float virtualGeometryPreloadBias;
	float virtualGeometryLodTransitionHysteresisPixels;
	std::uint32_t virtualGeometryLodTransitionStableFrames;
	std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(NativeGraphicsAbiHeader) == 8U);
static_assert(sizeof(NativeGraphicsRenderSettings) == 120U);
static_assert(offsetof(NativeGraphicsRenderSettings, header) == 0U);
static_assert(offsetof(NativeGraphicsRenderSettings, virtualGeometryBudgetBytes) == 96U);

// Non-physics runtime services share a small versioned ABI. These packets are
// copied snapshots or queued requests and contain no native pointers.
#pragma pack(push, 8)
struct NativeRuntimeAbiHeader { std::uint32_t abiVersion; std::uint32_t structSize; };

enum class NativeRuntimeStatus : std::uint32_t
{
	Success = 0U,
	Accepted = 1U,
	InvalidArgument = 2U,
	InvalidState = 3U,
	NotFound = 4U,
	Conflict = 5U,
	VersionMismatch = 6U
};

// Less frequently used runtime services share one exported dispatcher. This
// avoids one Windows import-library archive member per managed helper.
enum class NativeRuntimeOperation : std::uint32_t
{
	ObjectGetParent = 0U,
	ObjectSetParent = 1U,
	ObjectCopyChildren = 2U,
	ObjectCopyAll = 3U,
	ObjectCopyWithTag = 4U,
	InputReadSnapshot = 5U,
	InputSetMousePosition = 6U,
	TimeReadSnapshot = 7U,
	TimeSetScale = 8U,
	SceneGetActive = 9U,
	SceneGetLoaded = 10U,
	SceneInstantiatePrefab = 11U,
	SceneUnloadAll = 12U,
	SceneReadRenderSettings = 13U,
	SceneSubmitRenderSettings = 14U,
	ResourceReadStatus = 15U,
	ResourceRequestRelease = 16U,
	PhysicsReadSettings = 17U,
	PhysicsSubmitSettings = 18U,
	// New v1 operations append instead of shifting existing values so an older
	// managed assembly can never reinterpret a valid dispatcher request.
	ObjectRequestCreate = 19U,
	ObjectResolveStableId = 20U,
	SceneRequestLoad = 21U,
	SceneRequestLoadPrefab = 22U,
	SceneRequestSetActive = 23U
};

struct NativeRuntimeRequest
{
	NativeRuntimeAbiHeader header;
	NativeRuntimeOperation operation;
	std::uint32_t reserved = 0U;
	std::uint32_t objectId = 0U;
	std::uint32_t secondaryId = 0U;
	std::uint32_t assetType = 0U;
	std::uint32_t capacity = 0U;
	void* output = nullptr;
	std::uint32_t* requiredCount = nullptr;
	const char* text = nullptr;
	double x = 0.0;
	double y = 0.0;
	float scalar = 0.0F;
	std::uint32_t reserved2 = 0U;
};

struct NativeObjectCreationReceipt
{
	NativeRuntimeAbiHeader header;
	NativeRuntimeStatus status;
	std::uint32_t reserved = 0U;
	// UUIDs currently serialize as 32 hexadecimal characters. Extra capacity
	// keeps the v1 packet safe if separators are introduced later.
	char stableId[40]{};
};

struct NativeInputSnapshot
{
	NativeRuntimeAbiHeader header;
	NativeRuntimeStatus status;
	std::uint32_t keyCount;
	std::uint32_t mouseButtonCount;
	std::uint32_t workerReadable;
	std::uint64_t frameRevision;
	double cursorX;
	double cursorY;
	double scrollX;
	double scrollY;
	std::uint8_t keyStates[kNativeInputKeyCapacity];
	std::uint8_t mouseButtonStates[kNativeInputMouseButtonCapacity];
	std::uint8_t mousePressedThisFrame[kNativeInputMouseButtonCapacity];
	char diagnostic[kNativeRuntimeDiagnosticBytes];
};

struct NativeTimeSnapshot
{
	NativeRuntimeAbiHeader header;
	NativeRuntimeStatus status;
	std::uint32_t reserved;
	float deltaTime;
	float unscaledDeltaTime;
	float fixedDeltaTime;
	float timeScale;
	double elapsedTime;
	double unscaledElapsedTime;
	std::uint64_t frameCount;
};

struct NativeSceneRenderSettings
{
	NativeRuntimeAbiHeader header;
	NativeRuntimeStatus status;
	std::uint32_t reserved;
	float indirectStrength;
	float ambientOcclusionStrength;
	std::uint32_t ambientOcclusionQuality;
	std::uint32_t reserved2;
	std::uint64_t generation;
};

struct NativeResourceStreamingStatus
{
	NativeRuntimeAbiHeader header;
	NativeRuntimeStatus status;
	std::uint32_t assetType;
	std::uint32_t payloadState;
	std::uint32_t resident;
	std::uint64_t cpuBytes;
	std::uint64_t gpuBytes;
	std::uint64_t generation;
	char diagnostic[kNativeRuntimeDiagnosticBytes];
};
#pragma pack(pop)

static_assert(sizeof(NativeRuntimeAbiHeader) == 8U);
static_assert(sizeof(NativeRuntimeRequest) == 80U);
static_assert(sizeof(NativeObjectCreationReceipt) == 56U);
static_assert(static_cast<std::uint32_t>(NativeRuntimeOperation::PhysicsSubmitSettings) == 18U);
static_assert(static_cast<std::uint32_t>(NativeRuntimeOperation::ObjectRequestCreate) == 19U);
static_assert(static_cast<std::uint32_t>(NativeRuntimeOperation::SceneRequestSetActive) == 23U);
static_assert(offsetof(NativeInputSnapshot, header) == 0U);
static_assert(offsetof(NativeTimeSnapshot, header) == 0U);
static_assert(offsetof(NativeSceneRenderSettings, header) == 0U);
static_assert(offsetof(NativeResourceStreamingStatus, header) == 0U);

// Audio uses a deliberately small, fixed-width ABI. Native code copies every
// request into the protected control queue and reads only committed snapshots.
#pragma pack(push, 8)
struct NativeAudioAbiHeader { std::uint32_t abiVersion; std::uint32_t structSize; };

enum class NativeAudioStatus : std::uint32_t {
	Success = 0U, Queued, Accepted, NotFound, StaleTarget,
	InvalidArgument, LimitExceeded, ProviderFailure, VersionMismatch
};

enum class NativeAudioCommandKind : std::uint32_t { Play = 0U, Pause, Resume, Stop };

struct NativeAudioCommandRequest {
	NativeAudioAbiHeader header;
	std::uint32_t objectEntity;
	std::uint32_t componentGeneration;
	std::uint32_t playbackGeneration;
	NativeAudioCommandKind kind;
	std::uint64_t requestedFrame;
	std::uint64_t producerId;
	std::uint64_t correlationId;
	std::uint64_t idempotencyKey;
};

struct NativeAudioCommandReceipt {
	NativeAudioAbiHeader header;
	NativeAudioStatus status;
	std::uint32_t rejection;
	std::uint64_t batchSequence;
	std::uint64_t requestedFrame;
	std::uint64_t correlationId;
	char diagnostic[kNativeAudioDiagnosticBytes];
};

struct NativeAudioPlaybackState {
	NativeAudioAbiHeader header;
	NativeAudioStatus status;
	std::uint32_t state;
	std::uint32_t playbackGeneration;
	std::uint32_t audible;
	std::uint64_t frame;
	std::uint64_t logicalFrame;
	float gain;
	float pitch;
};

struct NativeAudioMixState {
	NativeAudioAbiHeader header;
	NativeAudioStatus status;
	std::uint32_t muted;
	std::uint64_t generation;
	float volume;
	std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(offsetof(NativeAudioCommandRequest, header) == 0U);
static_assert(offsetof(NativeAudioCommandReceipt, header) == 0U);
static_assert(offsetof(NativeAudioPlaybackState, header) == 0U);
static_assert(offsetof(NativeAudioMixState, header) == 0U);

class Engine;

// The editor executable registers a plain function pointer so gameplay code can
// stop Play Mode without the engine DLL depending directly on editor headers.
using ApplicationEditorStopPlayModeCallback = void (*)();

// MainEngine registers the active host engine so Application.Quit() can signal
// the standalone runtime loop from inside the script bridge.
TERRAENGINE_API void SetApplicationHostEngine(Engine* engine);
TERRAENGINE_API void SetApplicationEditorStopPlayModeCallback(ApplicationEditorStopPlayModeCallback callback);

// MainEngine calls this only at a protected mutation flush. It publishes typed
// managed requests for scene metadata, prefab lifecycle, and resource release.
TERRAENGINE_API void FlushManagedRuntimeRequests();

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
	TERRAENGINE_API void Native_Object_Destroy(uint32_t objectId);
	TERRAENGINE_API const char* Native_Object_GetName(uint32_t objectId);
	TERRAENGINE_API void Native_Object_SetName(uint32_t objectId, const char* name);
	TERRAENGINE_API const char* Native_Object_GetTag(uint32_t objectId);
	TERRAENGINE_API void Native_Object_SetTag(uint32_t objectId, const char* tag);
	TERRAENGINE_API const char* Native_Object_GetLayer(uint32_t objectId);
	TERRAENGINE_API void Native_Object_SetLayer(uint32_t objectId, const char* layer);
	TERRAENGINE_API bool Native_Object_GetActive(uint32_t objectId);
	TERRAENGINE_API bool Native_Object_GetActiveInHierarchy(uint32_t objectId);
	TERRAENGINE_API void Native_Object_SetActive(uint32_t objectId, bool active);
	TERRAENGINE_API bool Native_Object_GetStatic(uint32_t objectId);
	TERRAENGINE_API void Native_Object_SetStatic(uint32_t objectId, bool isStatic);
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
	TERRAENGINE_API const char* Native_GetComponentStringProperty(uint32_t objectId, const char* componentType, const char* propertyName);
	TERRAENGINE_API void Native_SetComponentStringProperty(uint32_t objectId, const char* componentType, const char* propertyName, const char* value);
	TERRAENGINE_API bool Native_RemoveComponent(uint32_t objectId, const char* componentType);

	// Audio controls use the same typed queue for native, managed, and editor
	// producers, while reads return only the latest protected commit snapshot.
	TERRAENGINE_API std::uint32_t Native_Audio_GetAbiVersion();
	TERRAENGINE_API NativeAudioStatus Native_Audio_SubmitCommand(
		const NativeAudioCommandRequest* request,
		NativeAudioCommandReceipt* receipt);
	TERRAENGINE_API NativeAudioStatus Native_Audio_ReadPlayback(
		std::uint32_t objectEntity,
		std::uint32_t componentGeneration,
		NativeAudioPlaybackState* state);
	TERRAENGINE_API NativeAudioStatus Native_Audio_ReadMasterMix(NativeAudioMixState* state);
	TERRAENGINE_API NativeAudioStatus Native_Audio_SetMasterMix(const NativeAudioMixState* state);

	// Graphics ABI v1 queues runtime-only settings for RenderSystem's protected
	// boundary. These calls never persist project authoring files from gameplay.
	TERRAENGINE_API std::uint32_t Native_Graphics_GetAbiVersion();
	TERRAENGINE_API NativeGraphicsStatus Native_Graphics_ReadRenderSettings(NativeGraphicsRenderSettings* settings);
	TERRAENGINE_API NativeGraphicsStatus Native_Graphics_SubmitRuntimeRenderSettings(const NativeGraphicsRenderSettings* settings);
	TERRAENGINE_API NativeGraphicsStatus Native_Graphics_SubmitRuntimeQualityPreset(std::uint32_t preset);
	TERRAENGINE_API NativeGraphicsStatus Native_Graphics_ReadQualityPreset(std::uint32_t* preset);
	TERRAENGINE_API std::uint64_t Native_Graphics_GetRuntimeSettingsGeneration();

	// Hierarchy, immutable input/time snapshots, scene lifecycle/settings,
	// resource residency, and full physics settings use one copied dispatcher.
	TERRAENGINE_API NativeRuntimeStatus Native_Runtime_Invoke(const NativeRuntimeRequest* request, void* packet);

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

	// Visibility streaming requests are non-blocking. Status reads copy the
	// current immutable record; release is deferred to the protected frame end.
	TERRAENGINE_API bool Native_ResourceStreaming_Preload(const char* assetReference, int assetType);

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

	// Animation ABI v1. Reads copy complete protected-boundary snapshots; a null
	// data pointer with zero capacity is the supported count-query form.
	TERRAENGINE_API std::uint16_t Native_Animation_GetAbiVersion();
	TERRAENGINE_API NativeAnimationStatus Native_Animation_ResolveAnimator(
		std::uint32_t objectEntity,
		NativeAnimationAnimatorHandleRecord* animator);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_SubmitCommand(
		const NativeAnimationCommandRequest* request,
		NativeAnimationCommandReceipt* receipt);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CancelCommand(
		const NativeAnimationCancellationRequest* request,
		NativeAnimationCommandReceipt* receipt);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_ReadReceipt(
		const NativeAnimationReceiptQuery* query,
		NativeAnimationCommandReceipt* receipt);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_QueryCapability(
		const NativeAnimationCapabilityQuery* query,
		NativeAnimationCapabilityResult* result);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_ReadState(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationStateSnapshot* snapshot);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyParameters(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationNamedValue* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyCurves(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationNamedValue* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyMorphs(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationNamedValue* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyBones(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationBoneSnapshot* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopySockets(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationSocketSnapshot* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyEvents(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationEvent* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyEventBatch(
		NativeAnimationEvent* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyRootMotionResults(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationRootMotionResult* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_ApplyRootMotion(
		const NativeAnimationRootMotionApplicationRequest* request,
		NativeAnimationRootMotionApplicationReceipt* receipt);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyRootMotionResultDetails(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationRootMotionResultDetails* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_SubmitProceduralGoal(
		const NativeAnimationProceduralGoalCommand* request,
		NativeAnimationProceduralGoalReceipt* receipt);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyProceduralResults(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationProceduralResultSnapshot* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	// Advanced records are copied deferred requests.  Until their scheduler-owned
	// owners are connected, valid calls return Unsupported instead of aliasing a
	// compiler, physics world, or deterministic history object.
	TERRAENGINE_API NativeAnimationStatus Native_Animation_SubmitRetargetRequest(
		const NativeAnimationRetargetRequest* request,
		NativeAnimationRetargetReceipt* receipt);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyRetargetSnapshots(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationRetargetSnapshot* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_SubmitMotionMatchRequest(
		const NativeAnimationMotionMatchRequest* request,
		NativeAnimationMotionMatchReceipt* receipt);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyMotionMatchSnapshots(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationMotionMatchSnapshot* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	// V2 records are additive: existing callers retain the frozen v1 strides,
	// while typed queries and detailed result validity remain copied and bounded.
	TERRAENGINE_API NativeAnimationStatus Native_Animation_SubmitMotionMatchQueryV2(
		const NativeAnimationMotionMatchQueryRequestV2* request,
		NativeAnimationMotionMatchQueryReceiptV2* receipt);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyMotionMatchResultsV2(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationMotionMatchResultV2* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_SubmitRagdollRequest(
		const NativeAnimationRagdollRequest* request,
		NativeAnimationRagdollReceipt* receipt);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyRagdollSnapshots(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationRagdollSnapshot* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_SubmitDeterminismRequest(
		const NativeAnimationDeterminismRequest* request,
		NativeAnimationDeterminismReceipt* receipt);
	TERRAENGINE_API NativeAnimationStatus Native_Animation_CopyDeterminismSnapshots(
		const NativeAnimationAnimatorHandle* animator,
		NativeAnimationDeterminismSnapshot* values,
		std::uint32_t capacity,
		NativeAnimationBufferInfo* info);

	// Physics ABI v1. Every record begins with NativePhysicsAbiHeader and is
	// copied across the boundary, making reload and world teardown safe.
	TERRAENGINE_API std::uint32_t Native_Physics_GetAbiVersion();
	TERRAENGINE_API NativePhysicsStatus Native_Physics_SubmitBodyCommand(const NativePhysicsBodyCommandRequest* request, NativePhysicsCommandReceipt* receipt);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_SubmitCharacterCommand(const NativePhysicsCharacterCommandRequest* request, NativePhysicsCommandReceipt* receipt);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_QueryImmediate(const NativePhysicsQueryRequest* request, NativePhysicsQueryResultInfo* resultInfo, NativePhysicsQueryHit* hits, std::uint32_t hitCapacity, std::uint32_t* requiredHitCount);
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
	TERRAENGINE_API NativePhysicsStatus Native_Physics_CopyColliderRecords(std::uint32_t objectEntity, NativePhysicsColliderRecord* records, std::uint32_t recordCapacity, std::uint32_t* requiredRecordCount);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_ReplaceColliderRecords(std::uint32_t objectEntity, const NativePhysicsColliderRecord* records, std::uint32_t recordCount);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_CopyConstraintRecords(std::uint32_t objectEntity, NativePhysicsConstraintRecord* records, std::uint32_t recordCapacity, std::uint32_t* requiredRecordCount);
	TERRAENGINE_API NativePhysicsStatus Native_Physics_ReplaceConstraintRecords(std::uint32_t objectEntity, const NativePhysicsConstraintRecord* records, std::uint32_t recordCount);
}
