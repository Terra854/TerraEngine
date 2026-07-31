// AnimationScriptAbi.h - Versioned copied records for native and managed gameplay animation.

#pragma once

#include "Core/Export.h"

#include <cstdint>

// ABI v1 never exposes native containers, strings, or pointers. Callers own
// every request/output record and use the required-count protocol for arrays.
inline constexpr std::uint16_t kNativeAnimationAbiVersion = 1U;
inline constexpr std::uint32_t kNativeAnimationEventTagBytes = 64U;
inline constexpr std::uint32_t kNativeAnimationEventPayloadBytes = 256U;
inline constexpr std::uint32_t kNativeAnimationMotionFeatureCount = 48U;
inline constexpr std::uint32_t kNativeAnimationMotionFeatureBlockCount = 9U;
inline constexpr std::uint32_t kNativeAnimationMotionMaximumExcludedSamples = 256U;
inline constexpr std::uint32_t kNativeAnimationMotionDefaultTopK = 32U;
inline constexpr std::uint32_t kNativeAnimationMotionMaximumTopK = 256U;
inline constexpr std::uint32_t kNativeAnimationMotionMaximumSamples = 16777216U;

#pragma pack(push, 8)
struct NativeAnimationAbiHeader {
	std::uint32_t size;
	std::uint16_t version;
	std::uint16_t kind;
};

enum class NativeAnimationRecordKind : std::uint16_t {
	Invalid = 0U,
	AnimatorHandle = 1U,
	CommandRequest = 2U,
	CommandReceipt = 3U,
	CancellationRequest = 4U,
	ReceiptQuery = 5U,
	CapabilityQuery = 6U,
	CapabilityResult = 7U,
	StateSnapshot = 8U,
	BufferInfo = 9U,
	NamedValue = 10U,
	BoneSnapshot = 11U,
	SocketSnapshot = 12U,
	Event = 13U,
	RootMotionResult = 14U,
	RootMotionApplicationRequest = 15U,
	RootMotionApplicationReceipt = 16U,
	RootMotionResultDetails = 17U,
	ProceduralGoalCommand = 18U,
	ProceduralGoalReceipt = 19U,
	ProceduralResultSnapshot = 20U,
	RetargetRequest = 21U,
	RetargetReceipt = 22U,
	RetargetSnapshot = 23U,
	MotionMatchRequest = 24U,
	MotionMatchReceipt = 25U,
	MotionMatchSnapshot = 26U,
	RagdollRequest = 27U,
	RagdollReceipt = 28U,
	RagdollSnapshot = 29U,
	DeterminismRequest = 30U,
	DeterminismReceipt = 31U,
	DeterminismSnapshot = 32U,
	// These additive kinds preserve the frozen v1 request/snapshot strides while
	// exposing the complete fixed motion-query and diagnostic schemas.
	MotionMatchQueryRequestV2 = 33U,
	MotionMatchQueryReceiptV2 = 34U,
	MotionMatchResultV2 = 35U
};

// Values 0-23 intentionally mirror AnimationStatus. New values must only be
// appended so old managed assemblies keep interpreting copied receipts safely.
enum class NativeAnimationStatus : std::uint16_t {
	Ok = 0U,
	Queued = 1U,
	NotReady = 2U,
	BufferTooSmall = 3U,
	InvalidArgument = 4U,
	InvalidType = 5U,
	MissingAsset = 6U,
	IncompatibleAsset = 7U,
	StaleHandle = 8U,
	Cancelled = 9U,
	Unsupported = 10U,
	DeterminismViolation = 11U,
	RendererUnavailable = 12U,
	Rejected = 13U,
	InternalError = 14U,
	UnsupportedMajorVersion = 15U,
	InvalidEnvelope = 16U,
	TruncatedInput = 17U,
	RangeOverflow = 18U,
	RangeOutOfBounds = 19U,
	HardLimitExceeded = 20U,
	UnknownRequiredChunk = 21U,
	InvalidChunk = 22U,
	ContentHashMismatch = 23U
};

enum class NativeAnimationCommandKind : std::uint32_t {
	SetParameter = 0U,
	FireTrigger = 1U,
	Play = 2U,
	Stop = 3U,
	Pause = 4U,
	Resume = 5U,
	Seek = 6U,
	SetLooping = 7U,
	SetReverse = 8U,
	SetSpeed = 9U,
	CrossFade = 10U,
	SetLayerWeight = 11U,
	SetIkGoal = 12U,
	ClearIkGoal = 13U,
	RequestRagdoll = 14U
};

enum class NativeAnimationParameterType : std::uint32_t {
	Trigger = 0U,
	Boolean = 1U,
	Integer = 2U,
	Float = 3U,
	Vector3 = 4U,
	Quaternion = 5U,
	Transform = 6U
};

enum class NativeAnimationCapability : std::uint32_t {
	ParameterWrite = 0U,
	Trigger = 1U,
	Playback = 2U,
	StateTransition = 3U,
	Retarget = 4U,
	IkGoal = 5U,
	MotionMatching = 6U,
	RootMotion = 7U,
	Ragdoll = 8U,
	BoneRead = 9U,
	SocketRead = 10U,
	EventRead = 11U
};

enum class NativeAnimationCapabilityReason : std::uint32_t {
	Available = 0U,
	AnimatorNotReady = 1U << 0U,
	MissingController = 1U << 1U,
	MissingSkeleton = 1U << 2U,
	MissingPhysicsTarget = 1U << 3U,
	UnsupportedOperation = 1U << 4U,
	DeterministicModeRequired = 1U << 5U
};

enum class NativeAnimationRootMotionDisposition : std::uint32_t {
	Ignored = 0U,
	Extracted = 1U,
	Applied = 2U,
	Adjusted = 3U,
	Rejected = 4U,
	Deferred = 5U
};

enum class NativeAnimationRootMotionApplicationMode : std::uint32_t {
	Ignore = 0U,
	ExtractOnly = 1U,
	ApplyToTransform = 2U,
	ApplyToCharacter = 3U,
	ApplyToPhysicsBody = 4U,
	DeterministicNetwork = 5U
};

// Procedural values mirror the provider-neutral runtime enums. They are kept
// separate from the C++ types so the copied ABI never exposes an implementation
// header, native enum width, container, or pointer to managed callers.
enum class NativeAnimationProceduralCommandKind : std::uint32_t {
	SetGoal = 0U,
	ClearGoal = 1U
};

enum class NativeAnimationProceduralGoalKind : std::uint32_t {
	Position = 0U,
	Rotation = 1U,
	PositionAndRotation = 2U,
	Aim = 3U,
	Look = 4U,
	FootPlant = 5U,
	CopyTransform = 6U,
	Twist = 7U
};

enum class NativeAnimationProceduralCoordinateSpace : std::uint32_t {
	Local = 0U,
	Parent = 1U,
	Model = 2U,
	World = 3U
};

enum class NativeAnimationProceduralSolverKind : std::uint32_t {
	TwoBoneIk = 0U,
	ChainIk = 1U,
	Aim = 2U,
	Look = 3U,
	JointConstraint = 4U,
	CopyTransform = 5U,
	TwistDistribution = 6U,
	WeightedTargets = 7U,
	FootPlacement = 8U,
	PelvisCompensation = 9U
};

enum class NativeAnimationProceduralSolverState : std::uint32_t {
	Solved = 0U,
	Converged = 1U,
	MaxIterations = 2U,
	Unreachable = 3U,
	Singular = 4U,
	Clamped = 5U,
	StaleInput = 6U,
	NoContribution = 7U,
	InvalidInput = 8U
};

enum class NativeAnimationProceduralGoalFlag : std::uint32_t {
	UsePole = 1U << 0U,
	AllowStretch = 1U << 1U,
	Authoritative = 1U << 2U,
	Persistent = 1U << 3U
};

enum class NativeAnimationProceduralResultFlag : std::uint32_t {
	Converged = 1U << 0U,
	Clamped = 1U << 1U,
	Singular = 1U << 2U,
	Unreachable = 1U << 3U,
	StaleInput = 1U << 4U,
	CpuAuthoritative = 1U << 5U
};

// Advanced feature enums are deliberately ABI-owned.  They describe copied
// requests and observations only; their numeric values do not expose a native
// implementation enum whose layout could change independently of scripts.
enum class NativeAnimationRetargetOperation : std::uint32_t {
	ActivateProfile = 0U,
	DeactivateProfile = 1U,
	RequestRebuild = 2U
};

enum class NativeAnimationMotionMatchOperation : std::uint32_t {
	SetDatabase = 0U,
	Query = 1U,
	Clear = 2U
};

// Query fields are explicit so an adapter can report which copied values were
// validated and which values the currently installed runtime actually used.
enum class NativeAnimationMotionMatchQueryField : std::uint32_t {
	Identity = 1U << 0U,
	Features = 1U << 1U,
	FeatureWeights = 1U << 2U,
	Tags = 1U << 3U,
	Contacts = 1U << 4U,
	Phase = 1U << 5U,
	Transitions = 1U << 6U,
	Continuity = 1U << 7U,
	Exclusions = 1U << 8U,
	Costs = 1U << 9U,
	Budgets = 1U << 10U
};

inline constexpr std::uint32_t kNativeAnimationMotionMatchAllQueryFields =
	(1U << 11U) - 1U;

enum class NativeAnimationMotionMatchQueryReceiptFlag : std::uint32_t {
	None = 0U,
	PayloadRetained = 1U << 0U,
	RequiresRuntimeQueryIntegration = 1U << 1U
};

// Values mirror the deterministic motion matcher, but remain ABI-owned so the
// managed contract never depends on a native enum width or implementation header.
enum class NativeAnimationMotionMatchFallbackReason : std::uint32_t {
	None = 0U,
	EmptyDatabase = 1U,
	NoEligibleCandidates = 2U,
	CandidateBudgetExhausted = 3U,
	GpuUnavailable = 4U,
	GpuNotReady = 5U,
	GpuCancelled = 6U,
	StaleGpuResult = 7U,
	GpuBackendFailure = 8U,
	InvalidGpuCandidates = 9U
};

enum class NativeAnimationMotionMatchSelectionSource : std::uint32_t {
	None = 0U,
	GpuTopKCpuAuthoritative = 1U,
	CpuFullSearch = 2U
};

enum class NativeAnimationMotionMatchExclusionReason : std::uint32_t {
	None = 0U,
	MissingRequiredTags = 1U << 0U,
	ContainsExcludedTags = 1U << 1U,
	MissingRequiredContacts = 1U << 2U,
	PhaseOutsideWindow = 1U << 3U,
	ExplicitSample = 1U << 4U,
	DatabaseExclusionRegion = 1U << 5U,
	MissingRequiredTransitionFlags = 1U << 6U,
	ContainsExcludedTransitionFlags = 1U << 7U
};

enum class NativeAnimationMotionMatchResultField : std::uint32_t {
	None = 0U,
	Identity = 1U << 0U,
	DatabaseSchema = 1U << 1U,
	SelectedSample = 1U << 2U,
	SelectedSource = 1U << 3U,
	TotalCost = 1U << 4U,
	FeatureBlockCosts = 1U << 5U,
	MetadataCosts = 1U << 6U,
	Exclusions = 1U << 7U,
	Fallback = 1U << 8U,
	SelectionSource = 1U << 9U,
	GpuStatus = 1U << 10U,
	CandidateCounts = 1U << 11U
};

enum class NativeAnimationMotionMatchResultFlag : std::uint32_t {
	None = 0U,
	UsedFallback = 1U << 0U,
	UsedGpuCandidates = 1U << 1U,
	UsedCpuFallback = 1U << 2U,
	CandidateBudgetExhausted = 1U << 3U,
	LegacyAggregateScoreConversion = 1U << 4U,
	RuntimeUsedLegacyQuery = 1U << 5U
};

enum class NativeAnimationRagdollOperation : std::uint32_t {
	Activate = 0U,
	Deactivate = 1U,
	RequestRecovery = 2U
};

enum class NativeAnimationDeterminismOperation : std::uint32_t {
	Capture = 0U,
	Restore = 1U,
	ReplaySeek = 2U,
	RequestReplication = 3U
};

struct NativeAnimationUuid {
	std::uint64_t first;
	std::uint64_t second;
};

struct NativeAnimationAnimatorHandle {
	std::uint32_t slotIndex;
	std::uint32_t generation;
};

struct NativeAnimationVector3 {
	float x;
	float y;
	float z;
};

struct NativeAnimationQuaternion {
	float x;
	float y;
	float z;
	float w;
};

struct NativeAnimationTransform {
	NativeAnimationVector3 translation;
	NativeAnimationQuaternion rotation;
	NativeAnimationVector3 scale;
};

struct NativeAnimationAnimatorHandleRecord {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationUuid ownerObjectId;
	std::uint32_t componentGeneration;
	std::uint32_t reserved2;
};

// All payload alternatives are present so validation can occur before the
// request is copied into the native typed command queue.
struct NativeAnimationCommandRequest {
	NativeAnimationAbiHeader header;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationCommandKind command;
	NativeAnimationParameterType parameterType;
	std::uint64_t parameterId;
	std::int32_t integerValue;
	float floatValue;
	std::uint32_t booleanValue;
	NativeAnimationVector3 parameterVector;
	NativeAnimationQuaternion parameterQuaternion;
	NativeAnimationTransform parameterTransform;
	std::uint32_t layerIndex;
	std::uint64_t stateId;
	std::int64_t playbackTick;
	float playbackSpeed;
	float transitionSeconds;
	std::uint32_t playbackFlag;
	// IK v1 is fully representable even when the queried runtime capability is
	// unsupported. This reserves a typed, generation-safe request shape for the
	// later procedural solver without changing this record's prefix or meaning.
	NativeAnimationTransform goalTransform;
	NativeAnimationVector3 poleVector;
	float positionWeight;
	float rotationWeight;
	float poleWeight;
	std::uint32_t ikFlags;
	NativeAnimationUuid targetObjectId;
	std::uint32_t targetObjectGeneration;
	std::uint32_t targetComponentGeneration;
	std::uint32_t producerId;
	std::uint64_t producerSequence;
	std::uint64_t correlationId;
	std::uint64_t idempotencyKey;
	std::uint64_t cancellationToken;
	std::int64_t targetTick;
};

struct NativeAnimationCommandReceipt {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	std::uint32_t producerId;
	std::uint32_t duplicate;
	std::uint64_t producerSequence;
	std::uint64_t correlationId;
	std::uint64_t idempotencyKey;
	std::uint64_t cancellationToken;
	std::int64_t targetTick;
	std::int64_t observableTick;
	std::uint64_t publicationGeneration;
};

struct NativeAnimationCancellationRequest {
	NativeAnimationAbiHeader header;
	std::uint64_t cancellationToken;
	std::uint64_t correlationId;
};

struct NativeAnimationReceiptQuery {
	NativeAnimationAbiHeader header;
	std::uint32_t producerId;
	std::uint32_t reserved;
	std::uint64_t producerSequence;
	std::uint64_t correlationId;
};

struct NativeAnimationCapabilityQuery {
	NativeAnimationAbiHeader header;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationCapability capability;
	std::uint32_t layerIndex;
	std::uint64_t stableId;
};

struct NativeAnimationCapabilityResult {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationCapability capability;
	std::uint32_t supported;
	std::uint32_t reasonMask;
};

struct NativeAnimationStateSnapshot {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	std::uint64_t controllerStateId;
	std::int64_t tick;
	// observationTick-ageTicks identifies exactly how stale the copied state was
	// when the bridge produced this record; neither value consults wall time.
	std::int64_t observationTick;
	std::int64_t ageTicks;
	float normalizedTime;
	float transitionProgress;
	float effectiveSpeed;
	std::int64_t loopCount;
	std::uint64_t publicationGeneration;
	std::uint32_t layerCount;
	std::uint32_t parameterCount;
	std::uint32_t curveCount;
	std::uint32_t morphCount;
	std::uint32_t boneCount;
	std::uint32_t socketCount;
	std::uint32_t eventCount;
	std::uint32_t hasRootMotion;
	std::uint32_t requestedExecutionPolicy;
	std::uint32_t effectiveExecutionMode;
	NativeAnimationUuid controllerAssetId;
	std::uint32_t controllerGeneration;
	std::uint32_t reserved2;
	NativeAnimationUuid clipAssetId;
	std::uint32_t clipGeneration;
	std::uint32_t reserved3;
	NativeAnimationUuid skeletonAssetId;
	std::uint32_t skeletonGeneration;
	std::uint32_t reserved4;
	std::uint32_t updatePolicy;
	std::uint32_t eventPolicy;
	std::uint32_t cullingPolicy;
	std::uint32_t rootMotionPolicy;
};

struct NativeAnimationBufferInfo {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	std::uint32_t requiredCount;
	std::uint32_t writtenCount;
	std::uint32_t overflowed;
	std::uint32_t droppedCountLowerBound;
	std::int64_t sourceTick;
	std::uint64_t publicationGeneration;
};

struct NativeAnimationNamedValue {
	NativeAnimationAbiHeader header;
	std::uint64_t stableId;
	NativeAnimationParameterType type;
	std::uint32_t booleanValue;
	std::int32_t integerValue;
	float floatValue;
	NativeAnimationVector3 vectorValue;
	NativeAnimationQuaternion quaternionValue;
	NativeAnimationTransform transformValue;
};

struct NativeAnimationBoneSnapshot {
	NativeAnimationAbiHeader header;
	std::uint64_t stableId;
	std::uint32_t jointIndex;
	std::uint32_t poseGeneration;
	NativeAnimationTransform localTransform;
	NativeAnimationTransform modelTransform;
};

struct NativeAnimationSocketSnapshot {
	NativeAnimationAbiHeader header;
	std::uint64_t stableId;
	std::uint64_t parentBoneStableId;
	std::uint32_t poseGeneration;
	std::uint32_t reserved;
	NativeAnimationTransform modelTransform;
};

struct NativeAnimationEvent {
	NativeAnimationAbiHeader header;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationUuid clipAssetId;
	NativeAnimationUuid controllerAssetId;
	std::uint32_t controllerGeneration;
	std::uint32_t reserved;
	std::uint64_t controllerStateId;
	std::uint64_t sourceStateId;
	std::uint64_t eventId;
	std::int64_t sourceTick;
	std::int64_t loopCount;
	std::uint32_t layerIndex;
	std::uint32_t payloadType;
	std::int32_t direction;
	std::uint32_t historical;
	std::uint32_t clipGeneration;
	std::uint32_t poseGeneration;
	std::uint64_t producerOrder;
	std::uint64_t correlationId;
	std::uint64_t publicationGeneration;
	char tag[kNativeAnimationEventTagBytes];
	char payload[kNativeAnimationEventPayloadBytes];
};

struct NativeAnimationRootMotionResult {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationRootMotionDisposition disposition;
	std::uint32_t generation;
	std::int64_t sourceBeginTick;
	std::int64_t sourceEndTick;
	NativeAnimationVector3 requestedTranslation;
	NativeAnimationVector3 appliedTranslation;
	float requestedYawRadians;
	float appliedYawRadians;
	std::uint64_t correlationId;
	std::uint64_t publicationGeneration;
};

// Additive request/receipt records keep the legacy 96-byte result stride intact.
// Gameplay names one exact immutable root publication; native code resolves and
// queues the copied delta without accepting a mutable pose reference.
struct NativeAnimationRootMotionApplicationRequest {
	NativeAnimationAbiHeader header;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationRootMotionApplicationMode mode;
	std::uint32_t expectedPoseGeneration;
	std::uint32_t producerId;
	std::uint32_t reserved;
	std::uint64_t expectedPublicationGeneration;
	std::uint64_t producerSequence;
	std::uint64_t correlationId;
	std::uint64_t idempotencyKey;
	float fixedDurationSeconds;
	std::uint32_t reserved2;
};

struct NativeAnimationRootMotionApplicationReceipt {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationRootMotionApplicationMode mode;
	std::uint32_t generation;
	std::uint32_t producerId;
	std::uint32_t duplicate;
	std::uint32_t idempotencyConflict;
	std::uint32_t reserved2;
	std::uint64_t producerSequence;
	std::uint64_t correlationId;
	std::uint64_t idempotencyKey;
	std::uint64_t publicationGeneration;
	std::int64_t observableTick;
	std::uint64_t physicsCommandId;
};

// FunctionalContract metadata uses a new record kind and copy function. Old callers keep
// their original stride while current callers receive the complete result.
struct NativeAnimationRootMotionResultDetails {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationRootMotionDisposition disposition;
	std::uint32_t generation;
	NativeAnimationRootMotionApplicationMode applicationMode;
	std::uint32_t authoredPolicy;
	NativeAnimationUuid ownerObjectId;
	std::uint32_t objectGeneration;
	std::uint32_t componentGeneration;
	std::int64_t sourceBeginTick;
	std::int64_t sourceEndTick;
	NativeAnimationVector3 localTranslation;
	NativeAnimationVector3 requestedTranslation;
	NativeAnimationVector3 appliedTranslation;
	float localYawRadians;
	float requestedYawRadians;
	float appliedYawRadians;
	float layerContribution;
	NativeAnimationQuaternion requestedRotation;
	NativeAnimationQuaternion appliedRotation;
	std::uint64_t sourceStateId;
	std::int64_t accumulatedCycles;
	std::uint32_t layerIndex;
	std::uint32_t producerId;
	std::uint64_t producerSequence;
	std::uint64_t correlationId;
	std::uint64_t idempotencyKey;
	std::uint64_t publicationGeneration;
	std::uint64_t physicsCommandId;
	std::uint32_t duplicate;
	std::uint32_t idempotencyConflict;
};

// Set and clear share one fixed record. Clear interprets only identity,
// ordering, generation, and timing fields; ignored payload bytes must be zero.
// The dedicated record avoids changing the pinned 288-byte v1 command shape.
struct NativeAnimationProceduralGoalCommand {
	NativeAnimationAbiHeader header;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationProceduralCommandKind command;
	NativeAnimationProceduralGoalKind goalKind;
	NativeAnimationProceduralCoordinateSpace coordinateSpace;
	std::uint32_t layerIndex;
	std::uint64_t goalId;
	std::uint64_t targetJointStableId;
	NativeAnimationUuid targetObjectId;
	std::uint32_t targetObjectGeneration;
	std::uint32_t targetComponentGeneration;
	NativeAnimationTransform goalTransform;
	NativeAnimationVector3 poleVector;
	std::int32_t priority;
	float positionWeight;
	float rotationWeight;
	float poleWeight;
	float maximumStretchScale;
	NativeAnimationProceduralGoalFlag flags;
	std::uint32_t producerId;
	std::uint64_t producerSequence;
	std::uint64_t correlationId;
	std::uint64_t idempotencyKey;
	std::uint64_t cancellationToken;
	std::int64_t targetTick;
	std::uint64_t expectedPublicationGeneration;
	std::uint32_t expectedPoseGeneration;
	std::uint32_t reserved;
};

struct NativeAnimationProceduralGoalReceipt {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationProceduralCommandKind command;
	std::uint32_t producerId;
	std::uint32_t duplicate;
	std::uint32_t idempotencyConflict;
	std::uint32_t reserved2;
	std::uint64_t producerSequence;
	std::uint64_t correlationId;
	std::uint64_t idempotencyKey;
	std::uint64_t cancellationToken;
	std::uint64_t goalId;
	std::int64_t observableTick;
	std::uint64_t publicationGeneration;
};

// Result snapshots are copied at a protected publication boundary. The caller
// uses NativeAnimationBufferInfo for exact count/capacity negotiation and never
// aliases the evaluator's retained diagnostic history.
struct NativeAnimationProceduralResultSnapshot {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationProceduralSolverKind solver;
	NativeAnimationProceduralSolverState state;
	std::uint64_t goalId;
	std::uint64_t targetJointStableId;
	NativeAnimationTransform resultTransform;
	float targetErrorMeters;
	float targetErrorRadians;
	float inputWeight;
	float finalContribution;
	std::uint32_t iterationCount;
	std::uint32_t iterationLimit;
	NativeAnimationProceduralResultFlag flags;
	std::uint32_t poseGeneration;
	std::uint32_t componentGeneration;
	std::uint32_t skeletonGeneration;
	std::uint32_t programGeneration;
	std::uint32_t reserved2;
	std::int64_t observationTick;
	std::uint64_t publicationGeneration;
	std::uint64_t producerOrder;
	std::uint64_t correlationId;
};

// The following v1 extension records are fixed-width envelopes for the later
// retarget, motion-match, ragdoll, and rollback owners.  A script can submit
// them before an owner is wired into AnimationSystem; that case returns the
// typed Unsupported result rather than borrowing a transient runtime object.
struct NativeAnimationRetargetRequest {
	NativeAnimationAbiHeader header;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationRetargetOperation operation;
	std::uint32_t layerIndex;
	NativeAnimationUuid profileAssetId;
	std::uint32_t profileGeneration;
	std::uint32_t expectedPoseGeneration;
	std::uint64_t expectedPublicationGeneration;
	std::uint64_t correlationId;
	std::int64_t targetTick;
};

struct NativeAnimationRetargetReceipt {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationRetargetOperation operation;
	std::uint32_t profileGeneration;
	std::uint64_t correlationId;
	std::int64_t observableTick;
	std::uint64_t publicationGeneration;
};

struct NativeAnimationRetargetSnapshot {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationUuid profileAssetId;
	std::uint32_t profileGeneration;
	std::uint32_t sourceSkeletonGeneration;
	std::uint32_t targetSkeletonGeneration;
	std::uint32_t poseGeneration;
	float contactErrorMeters;
	float reachableIkErrorMeters;
	std::int64_t observationTick;
	std::uint64_t publicationGeneration;
};

struct NativeAnimationMotionMatchRequest {
	NativeAnimationAbiHeader header;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationMotionMatchOperation operation;
	std::uint32_t flags;
	NativeAnimationUuid databaseAssetId;
	std::uint32_t databaseGeneration;
	std::uint32_t featureCount;
	std::uint64_t queryId;
	std::uint64_t correlationId;
	std::int64_t targetTick;
};

struct NativeAnimationMotionMatchReceipt {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationMotionMatchOperation operation;
	std::uint32_t databaseGeneration;
	std::uint64_t queryId;
	std::uint64_t correlationId;
	std::int64_t observableTick;
	std::uint64_t publicationGeneration;
};

struct NativeAnimationMotionMatchSnapshot {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationUuid databaseAssetId;
	std::uint64_t queryId;
	std::uint64_t selectedSampleId;
	std::uint32_t databaseGeneration;
	std::uint32_t selectedClipGeneration;
	float totalScore;
	float trajectoryScore;
	float poseScore;
	std::uint32_t usedFallback;
	std::int64_t observationTick;
	std::uint64_t publicationGeneration;
};

// V2 is an additive fixed record rather than an enlargement of the frozen v1
// request. Arrays are inline so native and managed callers exchange one copied
// value without pointers, pinning, or evaluator-owned lifetime.
struct NativeAnimationMotionMatchQueryRequestV2 {
	NativeAnimationAbiHeader header;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationUuid databaseAssetId;
	std::uint32_t databaseGeneration;
	std::uint32_t featureCount;
	std::uint64_t schemaFingerprint;
	std::uint64_t queryId;
	std::uint64_t correlationId;
	std::int64_t targetTick;
	std::uint64_t requiredTags;
	std::uint64_t excludedTags;
	std::uint64_t preferredTags;
	std::uint64_t lastSampleId;
	std::uint64_t lastSourceClipId;
	std::uint32_t requiredContacts;
	std::uint32_t preferredContacts;
	std::uint32_t preferredContinuityGroup;
	std::uint32_t excludedRegionFlags;
	std::uint16_t preferredPhase;
	std::uint16_t maximumPhaseDistance;
	std::uint16_t requiredTransitionFlags;
	std::uint16_t excludedTransitionFlags;
	std::uint16_t preferredTransitionFlags;
	std::uint16_t reserved16;
	std::uint32_t phaseWeight;
	std::uint32_t continuityPenalty;
	std::uint32_t continuityBreakPenalty;
	std::uint32_t sameClipPenalty;
	std::uint32_t missingPreferredTagPenalty;
	std::uint32_t contactMismatchPenalty;
	std::uint32_t transitionMismatchPenalty;
	std::uint32_t maximumCandidates;
	std::uint32_t maximumSamplesToVisit;
	std::uint32_t maximumDiagnosticExclusions;
	std::uint32_t excludedSampleCount;
	std::uint32_t queryFields;
	std::uint32_t reserved32;
	std::int16_t features[kNativeAnimationMotionFeatureCount];
	std::uint16_t featureWeights[kNativeAnimationMotionFeatureCount];
	std::uint64_t excludedSampleIds[kNativeAnimationMotionMaximumExcludedSamples];
};

struct NativeAnimationMotionMatchQueryReceiptV2 {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	std::uint32_t databaseGeneration;
	std::uint32_t reserved2;
	std::uint64_t schemaFingerprint;
	std::uint64_t queryId;
	std::uint64_t correlationId;
	std::int64_t observableTick;
	std::uint64_t publicationGeneration;
	std::uint32_t validatedFields;
	std::uint32_t scheduledFields;
	std::uint32_t flags;
	std::uint32_t reserved3;
};

// A result marks every populated field. Rejected queries therefore expose only
// the identity/schema data that was actually validated, while successful V2
// evaluation can publish exact source, cost, exclusion, fallback, and GPU state.
struct NativeAnimationMotionMatchResultV2 {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	NativeAnimationStatus gpuStatus;
	NativeAnimationMotionMatchFallbackReason fallbackReason;
	NativeAnimationMotionMatchSelectionSource selectionSource;
	std::uint32_t populatedFields;
	std::uint32_t flags;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationUuid databaseAssetId;
	std::uint64_t queryId;
	std::uint64_t selectedSampleId;
	std::uint64_t selectedSourceClipId;
	std::int64_t selectedSourceTick;
	std::uint32_t databaseGeneration;
	std::uint32_t selectedClipGeneration;
	std::uint64_t schemaFingerprint;
	std::uint64_t totalCost;
	std::uint64_t featureCost;
	std::uint64_t featureBlockCosts[kNativeAnimationMotionFeatureBlockCount];
	std::uint64_t phaseCost;
	std::uint64_t tagCost;
	std::uint64_t contactCost;
	std::uint64_t continuityCost;
	std::uint64_t transitionCost;
	std::uint32_t visitedSamples;
	std::uint32_t evaluatedSamples;
	std::uint32_t excludedSamples;
	std::uint32_t exclusionReasonMask;
	std::uint32_t gpuCandidateCount;
	std::uint32_t reserved;
	std::int64_t observationTick;
	std::uint64_t publicationGeneration;
};

struct NativeAnimationRagdollRequest {
	NativeAnimationAbiHeader header;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationRagdollOperation operation;
	std::uint32_t flags;
	NativeAnimationUuid profileAssetId;
	std::uint32_t profileGeneration;
	std::uint32_t physicsWorldGeneration;
	float blendWeight;
	std::uint32_t reserved;
	std::uint64_t correlationId;
	std::int64_t targetTick;
};

struct NativeAnimationRagdollReceipt {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationRagdollOperation operation;
	std::uint32_t profileGeneration;
	std::uint64_t correlationId;
	std::int64_t observableTick;
	std::uint64_t publicationGeneration;
};

struct NativeAnimationRagdollSnapshot {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationUuid profileAssetId;
	std::uint32_t profileGeneration;
	std::uint32_t physicsWorldGeneration;
	std::uint32_t lifecycle;
	std::uint32_t bodyCount;
	float blendWeight;
	std::uint32_t recoveryBlocked;
	std::int64_t observationTick;
	std::uint64_t publicationGeneration;
};

struct NativeAnimationDeterminismRequest {
	NativeAnimationAbiHeader header;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationDeterminismOperation operation;
	std::uint32_t flags;
	std::int64_t sourceTick;
	std::int64_t targetTick;
	std::uint64_t expectedChecksum;
	std::uint64_t expectedPublicationGeneration;
	std::uint64_t correlationId;
};

struct NativeAnimationDeterminismReceipt {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	NativeAnimationDeterminismOperation operation;
	std::uint32_t reserved2;
	std::uint64_t correlationId;
	std::int64_t observableTick;
	std::uint64_t publicationGeneration;
};

struct NativeAnimationDeterminismSnapshot {
	NativeAnimationAbiHeader header;
	NativeAnimationStatus status;
	std::uint16_t reserved;
	NativeAnimationAnimatorHandle animator;
	std::int64_t tick;
	std::uint64_t stateChecksum;
	std::uint64_t poseChecksum;
	std::uint64_t publicationGeneration;
	std::uint64_t controllerStateId;
	std::uint32_t controllerGeneration;
	std::uint32_t eventWatermark;
	std::uint32_t divergenceKind;
	std::uint32_t reserved2;
};
#pragma pack(pop)

static_assert(sizeof(NativeAnimationRootMotionResult) == 96U,
	"Animation ABI v1 legacy root-motion result stride must never change.");
static_assert(sizeof(NativeAnimationRootMotionApplicationRequest) == 72U);
static_assert(sizeof(NativeAnimationRootMotionApplicationReceipt) == 96U);
static_assert(sizeof(NativeAnimationRootMotionResultDetails) == 240U);
static_assert(sizeof(NativeAnimationProceduralGoalCommand) == 208U);
static_assert(sizeof(NativeAnimationProceduralGoalReceipt) == 96U);
static_assert(sizeof(NativeAnimationProceduralResultSnapshot) == 168U);
static_assert(sizeof(NativeAnimationRetargetRequest) == 72U);
static_assert(sizeof(NativeAnimationRetargetReceipt) == 56U);
static_assert(sizeof(NativeAnimationRetargetSnapshot) == 80U);
static_assert(sizeof(NativeAnimationMotionMatchRequest) == 72U);
static_assert(sizeof(NativeAnimationMotionMatchReceipt) == 64U);
static_assert(sizeof(NativeAnimationMotionMatchSnapshot) == 96U);
static_assert(sizeof(NativeAnimationMotionMatchQueryRequestV2) == 2432U);
static_assert(sizeof(NativeAnimationMotionMatchQueryReceiptV2) == 88U);
static_assert(sizeof(NativeAnimationMotionMatchResultV2) == 272U);
static_assert(sizeof(NativeAnimationRagdollRequest) == 72U);
static_assert(sizeof(NativeAnimationRagdollReceipt) == 56U);
static_assert(sizeof(NativeAnimationRagdollSnapshot) == 80U);
static_assert(sizeof(NativeAnimationDeterminismRequest) == 64U);
static_assert(sizeof(NativeAnimationDeterminismReceipt) == 56U);
static_assert(sizeof(NativeAnimationDeterminismSnapshot) == 80U);
