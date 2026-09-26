// CharacterLocomotionTypes.cs - exact x64 copied records for locomotion ABI v1.
using System;
using System.Runtime.InteropServices;

namespace TerraEngine.Locomotion
{
	public enum LocomotionStatus : uint
	{
		Success, Queued, InvalidArgument, VersionMismatch, NotPlaying, NotReady,
		MissingBinding, StaleHandle, StaleConfiguration, OwnershipConflict,
		UnsupportedController, UnsupportedFrame, AnimationUnavailable,
		CapacityExceeded, ExpiredReceipt, SequenceConflict, BufferTooSmall, ProviderFailure
	}
	public enum LocomotionCommandKind : uint { Acquire, SetIntent, FollowNavigation, Stop, SetMode, Transfer, Release }
	public enum LocomotionReceiptState : uint { Queued, Applied, Adjusted, Rejected, Expired }
	public enum LocomotionReason : uint
	{
		None, InvalidArgument, StaleHandle, StaleConfiguration, OwnershipConflict,
		UnsupportedController, UnsupportedFrame, MissingBinding, AnimationUnavailable,
		CapacityExceeded, NotPlaying, ExpiredReceipt, SequenceConflict, Stopped,
		Revoked, ExternalExclusive
	}
	public enum LocomotionMotionMode : uint { Velocity, RootMotion }
	public enum LocomotionAuthorityState : uint { Idle, Controlled, Stopped, Suspended }
	public enum LocomotionIntentKind : uint { Player, Script, Navigation }
	public enum LocomotionDirectionSpace : uint { World, ControllerLocal }
	public enum LocomotionIntentLifetime : uint { OneUpdate, UntilStopped }

	internal enum NativeLocomotionRecordKind : ushort
	{
		Handle = 1, ResolveRequest, ResolveResult, CommandBatch, Command,
		ReceiptQuery, Receipt, ObservationQuery, Observation, BufferInfo
	}

	// Every nested input carries an exact kind, size and zeroed reserved bytes.
	// Pack=8 caps member alignment; the 16-byte header itself aligns to four.
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeLocomotionAbiHeader
	{
		internal uint AbiVersion, StructSize;
		internal NativeLocomotionRecordKind RecordKind;
		internal ushort Reserved0;
		internal uint Reserved1;
		internal static NativeLocomotionAbiHeader For<T>(NativeLocomotionRecordKind kind) where T : unmanaged => new()
		{
			AbiVersion = CharacterLocomotion.AbiVersion,
			StructSize = checked((uint)Marshal.SizeOf<T>()),
			RecordKind = kind
		};
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct NativeLocomotionHandle
	{
		internal NativeLocomotionAbiHeader Header;
		public ulong SceneLifetime;
		public PhysicsUuid ObjectId;
		public ulong ObjectGeneration, AttachmentGeneration;
		public uint SlotIndex, SlotGeneration;
		public readonly bool IsValid => SceneLifetime != 0 && ObjectId.IsValid
			&& ObjectGeneration != 0 && AttachmentGeneration != 0
			&& SlotIndex != uint.MaxValue && SlotGeneration != 0;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeLocomotionResolveRequest
	{
		internal NativeLocomotionAbiHeader Header;
		internal PhysicsUuid ObjectId;
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeLocomotionResolveResult
	{
		internal NativeLocomotionAbiHeader Header;
		internal LocomotionStatus Status;
		internal LocomotionReason BindingReason;
		internal NativeLocomotionHandle Handle;
		internal ulong ConfigurationRevision, AuthorityEpoch;
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeLocomotionCommandBatch
	{
		internal NativeLocomotionAbiHeader Header;
		internal uint CommandCount, Reserved;
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeLocomotionCommand
	{
		internal NativeLocomotionAbiHeader Header;
		internal NativeLocomotionHandle Handle;
		internal LocomotionCommandKind Kind;
		internal LocomotionIntentKind IntentKind;
		internal LocomotionDirectionSpace DirectionSpace;
		internal LocomotionIntentLifetime Lifetime;
		internal LocomotionMotionMode Mode;
		internal uint Reserved;
		internal ulong ExpectedAuthorityEpoch, ExpectedConfigurationRevision, OwnerToken;
		internal ulong ProducerId, Sequence, CorrelationId;
		internal double DirectionX, DirectionY, DirectionZ, RequestedSpeed;
		internal ulong TransferProducerId;
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeLocomotionReceiptQuery
	{
		internal NativeLocomotionAbiHeader Header;
		internal NativeLocomotionHandle Handle;
		internal ulong ProducerId, Sequence, CorrelationId;
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeLocomotionReceipt
	{
		internal NativeLocomotionAbiHeader Header;
		internal NativeLocomotionHandle Handle;
		internal ulong ProducerId, Sequence, CorrelationId;
		internal LocomotionReceiptState State;
		internal LocomotionReason Reason;
		internal ulong AcceptedUpdateEpoch, AuthorityEpoch, OwnerToken;
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeLocomotionObservationQuery
	{
		internal NativeLocomotionAbiHeader Header;
		internal NativeLocomotionHandle Handle;
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeLocomotionObservation
	{
		internal NativeLocomotionAbiHeader Header;
		internal NativeLocomotionHandle Handle;
		internal LocomotionAuthorityState AuthorityState;
		internal LocomotionMotionMode MotionMode;
		internal LocomotionIntentKind IntentKind;
		internal LocomotionReason SuspendedReason, BindingReason, NavigationReason;
		internal uint NavigationState, NavigationOutcome, NavigationAgentReason, NavigationCompatibilityField;
		internal LocomotionReason AnimationReason;
		internal uint AnimationParameterField, AnimationParameterError;
		internal byte Grounded, MotionHistoryAvailable;
		internal ushort Reserved0;
		internal ulong OwnerToken, OwnerProducerId, AuthorityEpoch, ConfigurationRevision;
		internal ulong AcceptedSequence, PhysicalTick, PublicationEpoch, FrameId;
		internal uint FrameGeneration, Reserved1;
		internal double DesiredVelocityX, DesiredVelocityY, DesiredVelocityZ, ObservedPlanarSpeed;
		internal double ObservedLocalDirectionX, ObservedLocalDirectionY, ObservedLocalDirectionZ;
		internal double NavigationControllerValue, NavigationProfileValue, ObservationDurationSeconds;
		internal double RemainingRouteDistanceMetres;
		internal double RequestedTranslationX, RequestedTranslationY, RequestedTranslationZ;
		internal double RealizedTranslationX, RealizedTranslationY, RealizedTranslationZ;
		internal double RequestedYawRadians, RealizedYawRadians;
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeLocomotionBufferInfo
	{
		internal NativeLocomotionAbiHeader Header;
		internal LocomotionStatus Status;
		internal uint RequiredCount, WrittenCount, Capacity;
		internal ulong PublicationEpoch;
	}

	public readonly struct CharacterLocomotionReceipt
	{
		public LocomotionStatus Status { get; init; }
		public LocomotionReceiptState State { get; init; }
		public LocomotionReason Reason { get; init; }
		public ulong ProducerId { get; init; }
		public ulong Sequence { get; init; }
		public ulong CorrelationId { get; init; }
		public ulong AcceptedUpdateEpoch { get; init; }
		public ulong AuthorityEpoch { get; init; }
		public ulong OwnerToken { get; init; }
		public bool IsApplied => State == LocomotionReceiptState.Applied || State == LocomotionReceiptState.Adjusted;
	}

	// Managed callers receive values, never a pointer into the native root.
	public readonly struct CharacterLocomotionObservation
	{
		public NativeLocomotionHandle Handle { get; init; }
		public LocomotionAuthorityState AuthorityState { get; init; }
		public LocomotionMotionMode MotionMode { get; init; }
		public LocomotionReason BindingReason { get; init; }
		public LocomotionReason NavigationReason { get; init; }
		public LocomotionReason AnimationReason { get; init; }
		public ulong OwnerToken { get; init; }
		public ulong AuthorityEpoch { get; init; }
		public ulong ConfigurationRevision { get; init; }
		public ulong PhysicalTick { get; init; }
		public ulong PublicationEpoch { get; init; }
		public double DesiredVelocityX { get; init; }
		public double DesiredVelocityY { get; init; }
		public double DesiredVelocityZ { get; init; }
		public double ObservedPlanarSpeed { get; init; }
		public double RemainingRouteDistanceMetres { get; init; }
		public uint NavigationOutcome { get; init; }
		public bool Grounded { get; init; }
		public bool MotionHistoryAvailable { get; init; }
		public double RequestedYawRadians { get; init; }
		public double RealizedYawRadians { get; init; }
	}
}
