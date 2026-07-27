// AnimationTypes.cs - public animation values and exact copied ABI v1 mirrors.
//
// Public scripts only retain fixed-width identities and copied snapshots. Native
// containers, strings, pointers, and mutable pose storage never cross this file's
// boundary, which keeps wrappers safe across scene, asset, and script reloads.

using System;
using System.Runtime.InteropServices;
using System.Text;

namespace TerraEngine.Animation
{
	/// <summary>The managed side of the append-only copied animation ABI.</summary>
	public static class AnimationAbi
	{
		public const ushort Version = 1;
	}

	/// <summary>Stable status values shared by native and managed animation APIs.</summary>
	public enum AnimationStatus : ushort
	{
		Ok = 0,
		Queued = 1,
		NotReady = 2,
		BufferTooSmall = 3,
		InvalidArgument = 4,
		InvalidType = 5,
		MissingAsset = 6,
		IncompatibleAsset = 7,
		StaleHandle = 8,
		Cancelled = 9,
		Unsupported = 10,
		DeterminismViolation = 11,
		RendererUnavailable = 12,
		Rejected = 13,
		InternalError = 14,
		UnsupportedMajorVersion = 15,
		InvalidEnvelope = 16,
		TruncatedInput = 17,
		RangeOverflow = 18,
		RangeOutOfBounds = 19,
		HardLimitExceeded = 20,
		UnknownRequiredChunk = 21,
		InvalidChunk = 22,
		ContentHashMismatch = 23
	}

	/// <summary>Authored policy. Auto is the safe default for every animator.</summary>
	public enum AnimationExecutionPolicy : uint
	{
		Auto = 0,
		ForceGpuVisual = 1,
		ForceHybridAuthoritative = 2,
		ForceCpuDeterministicFallback = 3
	}

	/// <summary>The read-only runtime mode selected at an animation boundary.</summary>
	public enum AnimationEffectiveExecutionMode : uint
	{
		GpuVisual = 0,
		HybridAuthoritative = 1,
		CpuDeterministicFallback = 2
	}

	public enum AnimatorRootMotionPolicy : uint
	{
		Ignore = 0,
		ExtractOnly = 1,
		ApplyToObject = 2,
		ApplyToCharacter = 3
	}

	public enum AnimatorUpdatePolicy : uint
	{
		EveryFrame = 0,
		FixedStep = 1,
		Manual = 2
	}

	public enum AnimatorTimePolicy : uint
	{
		Scaled = 0,
		Unscaled = 1,
		Fixed = 2
	}

	public enum AnimatorEventPolicy : uint
	{
		Dispatch = 0,
		Retain = 1,
		Suppress = 2
	}

	public enum AnimatorCullingPolicy : uint
	{
		Automatic = 0,
		AlwaysUpdate = 1,
		SuspendVisualWhenInvisible = 2
	}

	public enum AnimationCommandKind : uint
	{
		SetParameter = 0,
		FireTrigger = 1,
		Play = 2,
		Stop = 3,
		Pause = 4,
		Resume = 5,
		Seek = 6,
		SetLooping = 7,
		SetReverse = 8,
		SetSpeed = 9,
		CrossFade = 10,
		SetLayerWeight = 11,
		SetIkGoal = 12,
		ClearIkGoal = 13,
		RequestRagdoll = 14
	}

	public enum AnimationParameterType : uint
	{
		Trigger = 0,
		Boolean = 1,
		Integer = 2,
		Float = 3,
		Vector3 = 4,
		Quaternion = 5,
		Transform = 6
	}

	public enum AnimationCapability : uint
	{
		ParameterWrite = 0,
		Trigger = 1,
		Playback = 2,
		StateTransition = 3,
		Retarget = 4,
		IkGoal = 5,
		MotionMatching = 6,
		RootMotion = 7,
		Ragdoll = 8,
		BoneRead = 9,
		SocketRead = 10,
		EventRead = 11
	}

	[Flags]
	public enum AnimationCapabilityReason : uint
	{
		Available = 0,
		AnimatorNotReady = 1U << 0,
		MissingController = 1U << 1,
		MissingSkeleton = 1U << 2,
		MissingPhysicsTarget = 1U << 3,
		UnsupportedOperation = 1U << 4,
		DeterministicModeRequired = 1U << 5
	}

	public enum AnimationRootMotionDisposition : uint
	{
		Ignored = 0,
		Extracted = 1,
		Applied = 2,
		Adjusted = 3,
		Rejected = 4,
		Deferred = 5
	}

	public enum AnimationRootMotionApplicationMode : uint
	{
		Ignore = 0,
		ExtractOnly = 1,
		ApplyToTransform = 2,
		ApplyToCharacter = 3,
		ApplyToPhysicsBody = 4,
		DeterministicNetwork = 5
	}

	public enum AnimationAssetKind : uint
	{
		Skeleton = 0,
		Clip = 1,
		Controller = 2,
		Mask = 3,
		RetargetProfile = 4,
		MotionDatabase = 5,
		RagdollProfile = 6
	}

	[Flags]
	public enum AnimationIkGoalFlags : uint
	{
		None = 0,
		UsePole = 1U << 0,
		TargetIsWorldSpace = 1U << 1,
		AllowStretch = 1U << 2
	}

	/// <summary>An opaque copied 128-bit identity used for assets and objects.</summary>
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct AnimationUuid : IEquatable<AnimationUuid>
	{
		public ulong First;
		public ulong Second;

		public AnimationUuid(ulong first, ulong second)
		{
			First = first;
			Second = second;
		}

		public static AnimationUuid Empty => default;
		public readonly bool IsValid => First != 0 || Second != 0;
		public readonly bool Equals(AnimationUuid other) => First == other.First && Second == other.Second;
		public override readonly bool Equals(object? value) => value is AnimationUuid other && Equals(other);
		public override readonly int GetHashCode() => HashCode.Combine(First, Second);
		public static bool operator ==(AnimationUuid left, AnimationUuid right) => left.Equals(right);
		public static bool operator !=(AnimationUuid left, AnimationUuid right) => !left.Equals(right);
		public override readonly string ToString() => $"{First:x16}{Second:x16}";
	}

	/// <summary>A slot plus generation; no native address is retained.</summary>
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct AnimatorHandle : IEquatable<AnimatorHandle>
	{
		public const uint InvalidSlot = uint.MaxValue;
		public uint SlotIndex;
		public uint Generation;

		public readonly bool IsValid => SlotIndex != InvalidSlot && Generation != 0;
		public readonly bool Equals(AnimatorHandle other) => SlotIndex == other.SlotIndex && Generation == other.Generation;
		public override readonly bool Equals(object? value) => value is AnimatorHandle other && Equals(other);
		public override readonly int GetHashCode() => HashCode.Combine(SlotIndex, Generation);
		public static bool operator ==(AnimatorHandle left, AnimatorHandle right) => left.Equals(right);
		public static bool operator !=(AnimatorHandle left, AnimatorHandle right) => !left.Equals(right);
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct AnimationTransform
	{
		public Vector3 Translation;
		public Quaternion Rotation;
		public Vector3 Scale;

		public static AnimationTransform Identity => new()
		{
			Rotation = Quaternion.Identity,
			Scale = new Vector3(1.0f, 1.0f, 1.0f)
		};
	}

	/// <summary>A stable authored asset identity plus immutable publication generation.</summary>
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public readonly struct AnimationAssetReference
	{
		public AnimationAssetReference(AnimationUuid id, uint generation, AnimationAssetKind kind)
		{
			Id = id;
			Generation = generation;
			Kind = kind;
		}

		public AnimationUuid Id { get; }
		public uint Generation { get; }
		public AnimationAssetKind Kind { get; }
		public bool IsValid => Id.IsValid && Generation != 0;
	}

	public readonly struct SkeletonAsset { public SkeletonAsset(AnimationAssetReference reference) { Reference = reference; } public AnimationAssetReference Reference { get; } }
	public readonly struct AnimationClipAsset { public AnimationClipAsset(AnimationAssetReference reference) { Reference = reference; } public AnimationAssetReference Reference { get; } }
	public readonly struct AnimationControllerAsset { public AnimationControllerAsset(AnimationAssetReference reference) { Reference = reference; } public AnimationAssetReference Reference { get; } }
	public readonly struct AnimationMaskAsset { public AnimationMaskAsset(AnimationAssetReference reference) { Reference = reference; } public AnimationAssetReference Reference { get; } }
	public readonly struct RetargetProfileAsset { public RetargetProfileAsset(AnimationAssetReference reference) { Reference = reference; } public AnimationAssetReference Reference { get; } }
	public readonly struct MotionDatabaseAsset { public MotionDatabaseAsset(AnimationAssetReference reference) { Reference = reference; } public AnimationAssetReference Reference { get; } }
	public readonly struct RagdollProfileAsset { public RagdollProfileAsset(AnimationAssetReference reference) { Reference = reference; } public AnimationAssetReference Reference { get; } }

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public readonly struct AnimationParameterId
	{
		public AnimationParameterId(ulong value) { Value = value; }
		public ulong Value { get; }
		public bool IsValid => Value != 0;
		public static implicit operator AnimationParameterId(ulong value) => new(value);
	}

	/// <summary>Ordering and retry metadata copied into a queued command.</summary>
	public struct AnimationCommandOptions
	{
		public long TargetTick;
		public uint ProducerId;
		public ulong ProducerSequence;
		public ulong CorrelationId;
		public ulong IdempotencyKey;
		public ulong CancellationToken;
	}

	/// <summary>Ordering metadata for one deferred application of an exact published delta.</summary>
	public struct AnimationRootMotionApplicationOptions
	{
		public uint ProducerId;
		public ulong ProducerSequence;
		public ulong CorrelationId;
		public ulong IdempotencyKey;
		public float FixedDurationSeconds;
	}

	public readonly struct AnimationRootMotionApplicationReceipt
	{
		internal AnimationRootMotionApplicationReceipt(NativeAnimationRootMotionApplicationReceipt value)
		{
			Status = value.Status;
			Animator = value.Animator;
			Mode = value.Mode;
			Generation = value.Generation;
			ProducerId = value.ProducerId;
			ProducerSequence = value.ProducerSequence;
			CorrelationId = value.CorrelationId;
			IdempotencyKey = value.IdempotencyKey;
			PublicationGeneration = value.PublicationGeneration;
			ObservableTick = value.ObservableTick;
			PhysicsCommandId = value.PhysicsCommandId;
			Duplicate = value.Duplicate != 0;
			IdempotencyConflict = value.IdempotencyConflict != 0;
		}

		public AnimationStatus Status { get; }
		public AnimatorHandle Animator { get; }
		public AnimationRootMotionApplicationMode Mode { get; }
		public uint Generation { get; }
		public uint ProducerId { get; }
		public ulong ProducerSequence { get; }
		public ulong CorrelationId { get; }
		public ulong IdempotencyKey { get; }
		public ulong PublicationGeneration { get; }
		public long ObservableTick { get; }
		public ulong PhysicsCommandId { get; }
		public bool Duplicate { get; }
		public bool IdempotencyConflict { get; }
		public bool Accepted => Status == AnimationStatus.Ok || Status == AnimationStatus.Queued;
	}

	/// <summary>Generation-safe target information for a procedural IK request.</summary>
	public struct AnimationIkGoal
	{
		public ulong GoalId;
		public uint LayerIndex;
		public AnimationTransform Transform;
		public Vector3 PoleVector;
		public float PositionWeight;
		public float RotationWeight;
		public float PoleWeight;
		public AnimationIkGoalFlags Flags;
		public AnimationUuid TargetObjectId;
		public uint TargetObjectGeneration;
		public uint TargetComponentGeneration;
	}

	public readonly struct AnimationCommandReceipt
	{
		internal AnimationCommandReceipt(NativeAnimationCommandReceipt value)
		{
			Status = value.Status;
			Animator = value.Animator;
			ProducerId = value.ProducerId;
			ProducerSequence = value.ProducerSequence;
			CorrelationId = value.CorrelationId;
			IdempotencyKey = value.IdempotencyKey;
			CancellationToken = value.CancellationToken;
			TargetTick = value.TargetTick;
			ObservableTick = value.ObservableTick;
			PublicationGeneration = value.PublicationGeneration;
			Duplicate = value.Duplicate != 0;
		}

		public AnimationStatus Status { get; }
		public AnimatorHandle Animator { get; }
		public uint ProducerId { get; }
		public ulong ProducerSequence { get; }
		public ulong CorrelationId { get; }
		public ulong IdempotencyKey { get; }
		public ulong CancellationToken { get; }
		public long TargetTick { get; }
		public long ObservableTick { get; }
		public ulong PublicationGeneration { get; }
		public bool Duplicate { get; }
		public bool Accepted => Status == AnimationStatus.Ok || Status == AnimationStatus.Queued;
	}

	public readonly struct AnimationCapabilityResult
	{
		internal AnimationCapabilityResult(NativeAnimationCapabilityResult value)
		{
			Status = value.Status;
			Capability = value.Capability;
			Supported = value.Supported != 0;
			Reasons = (AnimationCapabilityReason)value.ReasonMask;
		}

		public AnimationStatus Status { get; }
		public AnimationCapability Capability { get; }
		public bool Supported { get; }
		public AnimationCapabilityReason Reasons { get; }
	}

	public readonly struct AnimatorStateSnapshot
	{
		internal AnimatorStateSnapshot(NativeAnimationStateSnapshot value)
		{
			Status = value.Status;
			Animator = value.Animator;
			ControllerStateId = value.ControllerStateId;
			Tick = value.Tick;
			ObservationTick = value.ObservationTick;
			AgeTicks = value.AgeTicks;
			NormalizedTime = value.NormalizedTime;
			TransitionProgress = value.TransitionProgress;
			EffectiveSpeed = value.EffectiveSpeed;
			LoopCount = value.LoopCount;
			PublicationGeneration = value.PublicationGeneration;
			LayerCount = value.LayerCount;
			ParameterCount = value.ParameterCount;
			CurveCount = value.CurveCount;
			MorphCount = value.MorphCount;
			BoneCount = value.BoneCount;
			SocketCount = value.SocketCount;
			EventCount = value.EventCount;
			HasRootMotion = value.HasRootMotion != 0;
			RequestedExecutionPolicy = (AnimationExecutionPolicy)value.RequestedExecutionPolicy;
			EffectiveExecutionMode = (AnimationEffectiveExecutionMode)value.EffectiveExecutionMode;
			Controller = new AnimationControllerAsset(new AnimationAssetReference(
				value.ControllerAssetId, value.ControllerGeneration, AnimationAssetKind.Controller));
			Clip = new AnimationClipAsset(new AnimationAssetReference(
				value.ClipAssetId, value.ClipGeneration, AnimationAssetKind.Clip));
			Skeleton = new SkeletonAsset(new AnimationAssetReference(
				value.SkeletonAssetId, value.SkeletonGeneration, AnimationAssetKind.Skeleton));
			UpdatePolicy = (AnimatorUpdatePolicy)value.UpdatePolicy;
			EventPolicy = (AnimatorEventPolicy)value.EventPolicy;
			CullingPolicy = (AnimatorCullingPolicy)value.CullingPolicy;
			RootMotionPolicy = (AnimatorRootMotionPolicy)value.RootMotionPolicy;
		}

		public AnimationStatus Status { get; }
		public AnimatorHandle Animator { get; }
		public ulong ControllerStateId { get; }
		public long Tick { get; }
		public long ObservationTick { get; }
		public long AgeTicks { get; }
		public float NormalizedTime { get; }
		public float TransitionProgress { get; }
		public float EffectiveSpeed { get; }
		public long LoopCount { get; }
		public ulong PublicationGeneration { get; }
		public uint LayerCount { get; }
		public uint ParameterCount { get; }
		public uint CurveCount { get; }
		public uint MorphCount { get; }
		public uint BoneCount { get; }
		public uint SocketCount { get; }
		public uint EventCount { get; }
		public bool HasRootMotion { get; }
		public AnimationExecutionPolicy RequestedExecutionPolicy { get; }
		public AnimationEffectiveExecutionMode EffectiveExecutionMode { get; }
		public AnimationControllerAsset Controller { get; }
		public AnimationClipAsset Clip { get; }
		public SkeletonAsset Skeleton { get; }
		public AnimatorUpdatePolicy UpdatePolicy { get; }
		public AnimatorEventPolicy EventPolicy { get; }
		public AnimatorCullingPolicy CullingPolicy { get; }
		public AnimatorRootMotionPolicy RootMotionPolicy { get; }
	}

	public readonly struct AnimationNamedValue
	{
		internal AnimationNamedValue(NativeAnimationNamedValue value)
		{
			StableId = value.StableId;
			Type = value.Type;
			BooleanValue = value.BooleanValue != 0;
			IntegerValue = value.IntegerValue;
			FloatValue = value.FloatValue;
			VectorValue = value.VectorValue;
			QuaternionValue = value.QuaternionValue;
			TransformValue = value.TransformValue;
		}

		public ulong StableId { get; }
		public AnimationParameterType Type { get; }
		public bool BooleanValue { get; }
		public int IntegerValue { get; }
		public float FloatValue { get; }
		public Vector3 VectorValue { get; }
		public Quaternion QuaternionValue { get; }
		public AnimationTransform TransformValue { get; }
	}

	public readonly struct AnimationBoneSnapshot
	{
		internal AnimationBoneSnapshot(NativeAnimationBoneSnapshot value)
		{
			StableId = value.StableId;
			JointIndex = value.JointIndex;
			PoseGeneration = value.PoseGeneration;
			LocalTransform = value.LocalTransform;
			ModelTransform = value.ModelTransform;
		}

		public ulong StableId { get; }
		public uint JointIndex { get; }
		public uint PoseGeneration { get; }
		public AnimationTransform LocalTransform { get; }
		public AnimationTransform ModelTransform { get; }
	}

	public readonly struct AnimationSocketSnapshot
	{
		internal AnimationSocketSnapshot(NativeAnimationSocketSnapshot value)
		{
			StableId = value.StableId;
			ParentBoneStableId = value.ParentBoneStableId;
			PoseGeneration = value.PoseGeneration;
			ModelTransform = value.ModelTransform;
		}

		public ulong StableId { get; }
		public ulong ParentBoneStableId { get; }
		public uint PoseGeneration { get; }
		public AnimationTransform ModelTransform { get; }
	}

	public readonly struct AnimationRootMotionResult
	{
		internal AnimationRootMotionResult(NativeAnimationRootMotionResultDetails value)
		{
			Status = value.Status;
			Animator = value.Animator;
			Disposition = value.Disposition;
			Generation = value.Generation;
			SourceBeginTick = value.SourceBeginTick;
			SourceEndTick = value.SourceEndTick;
			RequestedTranslation = value.RequestedTranslation;
			AppliedTranslation = value.AppliedTranslation;
			RequestedYawRadians = value.RequestedYawRadians;
			AppliedYawRadians = value.AppliedYawRadians;
			CorrelationId = value.CorrelationId;
			PublicationGeneration = value.PublicationGeneration;
			ApplicationMode = value.ApplicationMode;
			AuthoredPolicy = (AnimatorRootMotionPolicy)value.AuthoredPolicy;
			OwnerObjectId = value.OwnerObjectId;
			ObjectGeneration = value.ObjectGeneration;
			ComponentGeneration = value.ComponentGeneration;
			LocalTranslation = value.LocalTranslation;
			LocalYawRadians = value.LocalYawRadians;
			RequestedRotation = value.RequestedRotation;
			AppliedRotation = value.AppliedRotation;
			SourceStateId = value.SourceStateId;
			AccumulatedCycles = value.AccumulatedCycles;
			LayerIndex = value.LayerIndex;
			LayerContribution = value.LayerContribution;
			ProducerId = value.ProducerId;
			ProducerSequence = value.ProducerSequence;
			IdempotencyKey = value.IdempotencyKey;
			PhysicsCommandId = value.PhysicsCommandId;
			Duplicate = value.Duplicate != 0;
			IdempotencyConflict = value.IdempotencyConflict != 0;
		}

		public AnimationStatus Status { get; }
		public AnimatorHandle Animator { get; }
		public AnimationRootMotionDisposition Disposition { get; }
		public uint Generation { get; }
		public long SourceBeginTick { get; }
		public long SourceEndTick { get; }
		public Vector3 RequestedTranslation { get; }
		public Vector3 AppliedTranslation { get; }
		public float RequestedYawRadians { get; }
		public float AppliedYawRadians { get; }
		public ulong CorrelationId { get; }
		public ulong PublicationGeneration { get; }
		public AnimationRootMotionApplicationMode ApplicationMode { get; }
		public AnimatorRootMotionPolicy AuthoredPolicy { get; }
		public AnimationUuid OwnerObjectId { get; }
		public uint ObjectGeneration { get; }
		public uint ComponentGeneration { get; }
		public Vector3 LocalTranslation { get; }
		public float LocalYawRadians { get; }
		public Quaternion RequestedRotation { get; }
		public Quaternion AppliedRotation { get; }
		public ulong SourceStateId { get; }
		public long AccumulatedCycles { get; }
		public uint LayerIndex { get; }
		public float LayerContribution { get; }
		public uint ProducerId { get; }
		public ulong ProducerSequence { get; }
		public ulong IdempotencyKey { get; }
		public ulong PhysicsCommandId { get; }
		public bool Duplicate { get; }
		public bool IdempotencyConflict { get; }
	}

	/// <summary>One complete caller-owned array copy plus overflow metadata.</summary>
	public sealed class AnimationBufferResult<T>
	{
		internal AnimationBufferResult(AnimationStatus status, T[] values, NativeAnimationBufferInfo info)
		{
			Status = status;
			Values = values;
			RequiredCount = info.RequiredCount;
			Overflowed = info.Overflowed != 0;
			DroppedCountLowerBound = info.DroppedCountLowerBound;
			SourceTick = info.SourceTick;
			PublicationGeneration = info.PublicationGeneration;
		}

		public AnimationStatus Status { get; }
		public T[] Values { get; }
		public uint RequiredCount { get; }
		public bool Overflowed { get; }
		public uint DroppedCountLowerBound { get; }
		public long SourceTick { get; }
		public ulong PublicationGeneration { get; }
	}

	internal enum AnimationRecordKind : ushort
	{
		Invalid = 0,
		AnimatorHandle = 1,
		CommandRequest = 2,
		CommandReceipt = 3,
		CancellationRequest = 4,
		ReceiptQuery = 5,
		CapabilityQuery = 6,
		CapabilityResult = 7,
		StateSnapshot = 8,
		BufferInfo = 9,
		NamedValue = 10,
		BoneSnapshot = 11,
		SocketSnapshot = 12,
		Event = 13,
		RootMotionResult = 14,
		RootMotionApplicationRequest = 15,
		RootMotionApplicationReceipt = 16,
		RootMotionResultDetails = 17,
		ProceduralGoalCommand = 18,
		ProceduralGoalReceipt = 19,
		ProceduralResultSnapshot = 20,
		RetargetRequest = 21,
		RetargetReceipt = 22,
		RetargetSnapshot = 23,
		MotionMatchRequest = 24,
		MotionMatchReceipt = 25,
		MotionMatchSnapshot = 26,
		RagdollRequest = 27,
		RagdollReceipt = 28,
		RagdollSnapshot = 29,
		DeterminismRequest = 30,
		DeterminismReceipt = 31,
		DeterminismSnapshot = 32,
		MotionMatchQueryRequestV2 = 33,
		MotionMatchQueryReceiptV2 = 34,
		MotionMatchResultV2 = 35
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct AnimationAbiHeader
	{
		internal const ushort Version = AnimationAbi.Version;
		internal uint Size;
		internal ushort AbiVersion;
		internal AnimationRecordKind Kind;

		internal static AnimationAbiHeader For<T>(AnimationRecordKind kind) where T : unmanaged
			=> new() { Size = (uint)Marshal.SizeOf<T>(), AbiVersion = Version, Kind = kind };
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationAnimatorHandleRecord
	{
		internal AnimationAbiHeader Header;
		internal AnimationStatus Status;
		private ushort _reserved;
		internal AnimatorHandle Animator;
		internal AnimationUuid OwnerObjectId;
		internal uint ComponentGeneration;
		private uint _reserved2;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationCommandRequest
	{
		internal AnimationAbiHeader Header;
		internal AnimatorHandle Animator;
		internal AnimationCommandKind Command;
		internal AnimationParameterType ParameterType;
		internal ulong ParameterId;
		internal int IntegerValue;
		internal float FloatValue;
		internal uint BooleanValue;
		internal Vector3 ParameterVector;
		internal Quaternion ParameterQuaternion;
		internal AnimationTransform ParameterTransform;
		internal uint LayerIndex;
		internal ulong StateId;
		internal long PlaybackTick;
		internal float PlaybackSpeed;
		internal float TransitionSeconds;
		internal uint PlaybackFlag;
		internal AnimationTransform GoalTransform;
		internal Vector3 PoleVector;
		internal float PositionWeight;
		internal float RotationWeight;
		internal float PoleWeight;
		internal AnimationIkGoalFlags IkFlags;
		internal AnimationUuid TargetObjectId;
		internal uint TargetObjectGeneration;
		internal uint TargetComponentGeneration;
		internal uint ProducerId;
		internal ulong ProducerSequence;
		internal ulong CorrelationId;
		internal ulong IdempotencyKey;
		internal ulong CancellationToken;
		internal long TargetTick;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationCommandReceipt
	{
		internal AnimationAbiHeader Header;
		internal AnimationStatus Status;
		private ushort _reserved;
		internal AnimatorHandle Animator;
		internal uint ProducerId;
		internal uint Duplicate;
		internal ulong ProducerSequence;
		internal ulong CorrelationId;
		internal ulong IdempotencyKey;
		internal ulong CancellationToken;
		internal long TargetTick;
		internal long ObservableTick;
		internal ulong PublicationGeneration;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationCancellationRequest
	{
		internal AnimationAbiHeader Header;
		internal ulong CancellationToken;
		internal ulong CorrelationId;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationReceiptQuery
	{
		internal AnimationAbiHeader Header;
		internal uint ProducerId;
		private uint _reserved;
		internal ulong ProducerSequence;
		internal ulong CorrelationId;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationCapabilityQuery
	{
		internal AnimationAbiHeader Header;
		internal AnimatorHandle Animator;
		internal AnimationCapability Capability;
		internal uint LayerIndex;
		internal ulong StableId;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationCapabilityResult
	{
		internal AnimationAbiHeader Header;
		internal AnimationStatus Status;
		private ushort _reserved;
		internal AnimationCapability Capability;
		internal uint Supported;
		internal uint ReasonMask;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationStateSnapshot
	{
		internal AnimationAbiHeader Header;
		internal AnimationStatus Status;
		private ushort _reserved;
		internal AnimatorHandle Animator;
		internal ulong ControllerStateId;
		internal long Tick;
		internal long ObservationTick;
		internal long AgeTicks;
		internal float NormalizedTime;
		internal float TransitionProgress;
		internal float EffectiveSpeed;
		internal long LoopCount;
		internal ulong PublicationGeneration;
		internal uint LayerCount;
		internal uint ParameterCount;
		internal uint CurveCount;
		internal uint MorphCount;
		internal uint BoneCount;
		internal uint SocketCount;
		internal uint EventCount;
		internal uint HasRootMotion;
		internal uint RequestedExecutionPolicy;
		internal uint EffectiveExecutionMode;
		internal AnimationUuid ControllerAssetId;
		internal uint ControllerGeneration;
		private uint _reserved2;
		internal AnimationUuid ClipAssetId;
		internal uint ClipGeneration;
		private uint _reserved3;
		internal AnimationUuid SkeletonAssetId;
		internal uint SkeletonGeneration;
		private uint _reserved4;
		internal uint UpdatePolicy;
		internal uint EventPolicy;
		internal uint CullingPolicy;
		internal uint RootMotionPolicy;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationBufferInfo
	{
		internal AnimationAbiHeader Header;
		internal AnimationStatus Status;
		private ushort _reserved;
		internal uint RequiredCount;
		internal uint WrittenCount;
		internal uint Overflowed;
		internal uint DroppedCountLowerBound;
		internal long SourceTick;
		internal ulong PublicationGeneration;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationNamedValue
	{
		internal AnimationAbiHeader Header;
		internal ulong StableId;
		internal AnimationParameterType Type;
		internal uint BooleanValue;
		internal int IntegerValue;
		internal float FloatValue;
		internal Vector3 VectorValue;
		internal Quaternion QuaternionValue;
		internal AnimationTransform TransformValue;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationBoneSnapshot
	{
		internal AnimationAbiHeader Header;
		internal ulong StableId;
		internal uint JointIndex;
		internal uint PoseGeneration;
		internal AnimationTransform LocalTransform;
		internal AnimationTransform ModelTransform;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationSocketSnapshot
	{
		internal AnimationAbiHeader Header;
		internal ulong StableId;
		internal ulong ParentBoneStableId;
		internal uint PoseGeneration;
		private uint _reserved;
		internal AnimationTransform ModelTransform;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal unsafe struct NativeAnimationEvent
	{
		internal const int TagCapacity = 64;
		internal const int PayloadCapacity = 256;
		internal AnimationAbiHeader Header;
		internal AnimatorHandle Animator;
		internal AnimationUuid ClipAssetId;
		internal AnimationUuid ControllerAssetId;
		internal uint ControllerGeneration;
		private uint _reserved;
		internal ulong ControllerStateId;
		internal ulong SourceStateId;
		internal ulong EventId;
		internal long SourceTick;
		internal long LoopCount;
		internal uint LayerIndex;
		internal uint PayloadType;
		internal int Direction;
		internal uint Historical;
		internal uint ClipGeneration;
		internal uint PoseGeneration;
		internal ulong ProducerOrder;
		internal ulong CorrelationId;
		internal ulong PublicationGeneration;
		internal fixed byte TagBytes[TagCapacity];
		internal fixed byte PayloadBytes[PayloadCapacity];

		internal string ReadTag()
		{
			fixed (byte* text = TagBytes) return AnimationInterop.ReadFixedUtf8(text, TagCapacity);
		}

		internal string ReadPayload()
		{
			fixed (byte* text = PayloadBytes) return AnimationInterop.ReadFixedUtf8(text, PayloadCapacity);
		}
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationRootMotionResult
	{
		internal AnimationAbiHeader Header;
		internal AnimationStatus Status;
		private ushort _reserved;
		internal AnimatorHandle Animator;
		internal AnimationRootMotionDisposition Disposition;
		internal uint Generation;
		internal long SourceBeginTick;
		internal long SourceEndTick;
		internal Vector3 RequestedTranslation;
		internal Vector3 AppliedTranslation;
		internal float RequestedYawRadians;
		internal float AppliedYawRadians;
		internal ulong CorrelationId;
		internal ulong PublicationGeneration;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationRootMotionApplicationRequest
	{
		internal AnimationAbiHeader Header;
		internal AnimatorHandle Animator;
		internal AnimationRootMotionApplicationMode Mode;
		internal uint ExpectedPoseGeneration;
		internal uint ProducerId;
		private uint _reserved;
		internal ulong ExpectedPublicationGeneration;
		internal ulong ProducerSequence;
		internal ulong CorrelationId;
		internal ulong IdempotencyKey;
		internal float FixedDurationSeconds;
		private uint _reserved2;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationRootMotionApplicationReceipt
	{
		internal AnimationAbiHeader Header;
		internal AnimationStatus Status;
		private ushort _reserved;
		internal AnimatorHandle Animator;
		internal AnimationRootMotionApplicationMode Mode;
		internal uint Generation;
		internal uint ProducerId;
		internal uint Duplicate;
		internal uint IdempotencyConflict;
		private uint _reserved2;
		internal ulong ProducerSequence;
		internal ulong CorrelationId;
		internal ulong IdempotencyKey;
		internal ulong PublicationGeneration;
		internal long ObservableTick;
		internal ulong PhysicsCommandId;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationRootMotionResultDetails
	{
		internal AnimationAbiHeader Header;
		internal AnimationStatus Status;
		private ushort _reserved;
		internal AnimatorHandle Animator;
		internal AnimationRootMotionDisposition Disposition;
		internal uint Generation;
		internal AnimationRootMotionApplicationMode ApplicationMode;
		internal uint AuthoredPolicy;
		internal AnimationUuid OwnerObjectId;
		internal uint ObjectGeneration;
		internal uint ComponentGeneration;
		internal long SourceBeginTick;
		internal long SourceEndTick;
		internal Vector3 LocalTranslation;
		internal Vector3 RequestedTranslation;
		internal Vector3 AppliedTranslation;
		internal float LocalYawRadians;
		internal float RequestedYawRadians;
		internal float AppliedYawRadians;
		internal float LayerContribution;
		internal Quaternion RequestedRotation;
		internal Quaternion AppliedRotation;
		internal ulong SourceStateId;
		internal long AccumulatedCycles;
		internal uint LayerIndex;
		internal uint ProducerId;
		internal ulong ProducerSequence;
		internal ulong CorrelationId;
		internal ulong IdempotencyKey;
		internal ulong PublicationGeneration;
		internal ulong PhysicsCommandId;
		internal uint Duplicate;
		internal uint IdempotencyConflict;
	}

	internal static unsafe class AnimationInterop
	{
		internal static string ReadFixedUtf8(byte* text, int capacity)
		{
			if (text == null || capacity <= 0) return string.Empty;
			int length = 0;
			while (length < capacity && text[length] != 0) ++length;
			return length == 0 ? string.Empty : Encoding.UTF8.GetString(new ReadOnlySpan<byte>(text, length));
		}
	}
}
