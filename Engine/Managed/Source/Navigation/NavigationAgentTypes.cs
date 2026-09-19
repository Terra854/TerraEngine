// NavigationAgentTypes.cs - managed facade values and the additive native v1 ABI.
using System;
using System.Runtime.InteropServices;

namespace TerraEngine
{
	public enum NavigationAgentState : uint { Idle, Requesting, Following, Arrived, Stopped, Failed }
	public enum NavigationAgentReceiptState : uint { Queued, Applied, Rejected, Superseded, Expired }
	public readonly struct NavigationAgentObjectIdentity
	{
		public ulong ObjectIdLow { get; init; }
		public ulong ObjectIdHigh { get; init; }
		public ulong LifetimeGeneration { get; init; }
		public bool IsValid => LifetimeGeneration != 0;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationAgentObjectIdentity { internal ulong ObjectIdLow, ObjectIdHigh, LifetimeGeneration; }

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct NavigationAgentIdentity
	{
		internal NavigationWorldHandle World;
		internal NativeNavigationAgentObjectIdentity Object;
		internal ulong AttachmentGeneration;
		internal uint RuntimeSlot;
		internal uint Reserved;
		internal ulong RuntimeSlotGeneration;
		public readonly bool IsValid => World.IsValid && Object.LifetimeGeneration != 0 && AttachmentGeneration != 0
			&& RuntimeSlot != uint.MaxValue && RuntimeSlotGeneration != 0;
		public readonly NavigationAgentObjectIdentity ObjectIdentity => new() { ObjectIdLow = Object.ObjectIdLow, ObjectIdHigh = Object.ObjectIdHigh, LifetimeGeneration = Object.LifetimeGeneration };
		public readonly ulong Attachment => AttachmentGeneration;
		public readonly uint Slot => RuntimeSlot;
		public readonly ulong SlotGeneration => RuntimeSlotGeneration;
	}

	public readonly struct NavigationAgentReceipt
	{
		public NavigationAgentReceiptState State { get; init; }
		public NavigationOutcome Outcome { get; init; }
		public NavigationReason Reason { get; init; }
		public ulong CorrelationId { get; init; }
		public ulong AdmittedSequence { get; init; }
		public bool Accepted => State == NavigationAgentReceiptState.Queued || State == NavigationAgentReceiptState.Applied;
	}
	public readonly struct NavigationAgentStatus
	{
		public NavigationAgentState State { get; init; }
		public NavigationOutcome Outcome { get; init; }
		public NavigationReason Reason { get; init; }
		public ulong ConfigurationRevision { get; init; }
		public ulong IntentSequence { get; init; }
		public ulong PublicationRevision { get; init; }
	}
	public readonly struct NavigationAgentGuidance
	{
		// Carries the cached attachment identity alongside the publication evidence
		// so consumers can retain a snapshot without losing its owner generation.
		public NavigationAgentIdentity Identity { get; init; }
		public NavigationAgentState State { get; init; }
		public NavigationOutcome Outcome { get; init; }
		public NavigationReason Reason { get; init; }
		public NavigationLocation NextWaypoint { get; init; }
		public Vector3 SuggestedDirection { get; init; }
		public double RemainingDistanceMetres { get; init; }
		public bool Usable { get; init; }
		public ulong ConfigurationRevision { get; init; }
		public ulong IntentSequence { get; init; }
		public ulong PublicationGeneration { get; init; }
		public ulong RouteGeneration { get; init; }
		public uint DestinationKind { get; init; }
		public NavigationLocation DestinationLocation { get; init; }
		public NavigationAgentObjectIdentity DestinationObject { get; init; }
		public ulong SceneGeneration { get; init; }
		public ulong SourceTransformRevision { get; init; }
		public ulong DestinationTransformRevision { get; init; }
		public ulong TerrainPublicationGeneration { get; init; }
		public ulong WorldPublicationGeneration { get; init; }
		public ulong SourceIndexRevision { get; init; }
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationAgentAbiHeader
	{
		internal uint AbiVersion;
		internal uint StructSize;
		internal static NativeNavigationAgentAbiHeader For<T>() where T : unmanaged => new()
		{
			AbiVersion = NavigationAgent.AbiVersion,
			StructSize = checked((uint)Marshal.SizeOf<T>())
		};
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationAgentResolveRequest { internal NativeNavigationAgentAbiHeader Header; internal uint OwnerObjectEntity; internal uint Reserved; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationAgentIdentitySnapshot { internal NativeNavigationAgentAbiHeader Header; internal NavigationStatus Status; internal uint Reserved; internal NavigationAgentIdentity Identity; }
	internal enum NativeNavigationAgentCommandKind : uint { SetLocationDestination, SetObjectDestination, Stop }
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationAgentCommand
	{
		internal NativeNavigationAgentAbiHeader Header; internal NavigationAgentIdentity Identity;
		internal NativeNavigationAgentCommandKind Kind; internal uint TargetObjectEntity; internal NavigationLocation Location;
		internal ulong ExpectedConfigurationRevision, ExpectedIntentSequence, ProducerOrderingToken, CorrelationId;
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationAgentCommandReceipt
	{
		internal NativeNavigationAgentAbiHeader Header; internal NavigationStatus Status; internal uint ReceiptState;
		internal NavigationAgentIdentity Identity; internal uint Outcome, Reason; internal ulong CorrelationId, AdmittedSequence;
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationAgentReceiptQuery { internal NativeNavigationAgentAbiHeader Header; internal NavigationAgentIdentity Identity; internal ulong CorrelationId; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationAgentStatusQuery { internal NativeNavigationAgentAbiHeader Header; internal NavigationAgentIdentity Identity; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationAgentStatusSnapshot
	{
		internal NativeNavigationAgentAbiHeader Header; internal NavigationStatus Status; internal uint State, Outcome, Reason;
		internal ulong ConfigurationRevision, IntentSequence, PublicationRevision;
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationAgentGuidanceQuery { internal NativeNavigationAgentAbiHeader Header; internal NavigationAgentIdentity Identity; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationAgentGuidanceSnapshot
	{
		internal NativeNavigationAgentAbiHeader Header; internal NavigationStatus Status; internal uint State, Outcome, Reason;
		internal ulong ConfigurationRevision, IntentSequence, PublicationGeneration, RouteGeneration, SceneGeneration;
		internal ulong SourceTransformRevision, DestinationTransformRevision, TerrainPublicationGeneration, WorldPublicationGeneration, SourceIndexRevision;
		internal NavigationLocation NextWaypoint; internal double DirectionX, DirectionY, DirectionZ, RemainingDistanceMetres;
		internal uint Usable, Reserved, DestinationKind, DestinationReserved;
		internal NavigationLocation DestinationLocation; internal NativeNavigationAgentObjectIdentity DestinationObject;
	}
}
