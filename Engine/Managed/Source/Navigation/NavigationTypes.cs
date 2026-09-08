// NavigationTypes.cs - fixed-width managed ground-navigation values and ABI packets.

using System;
using System.Runtime.InteropServices;
using System.Text;

namespace TerraEngine
{
	// Public values mirror the native enums exactly. Explicit numbers keep native,
	// editor-hosted managed, and NativeAOT builds semantically interchangeable.
	public enum NavigationStatus : uint
	{
		Success = 0, Accepted = 1, InvalidArgument = 2, InvalidOwner = 3,
		StaleTarget = 4, Expired = 5, LimitExceeded = 6, Unavailable = 7,
		Unsupported = 8, VersionMismatch = 9, Failed = 10
	}

	public enum NavigationRequestLifecycle : uint
	{
		Queued = 0, PendingPreparation = 1, Searching = 2, Terminal = 3
	}

	public enum NavigationOutcome : uint
	{
		Queued = 0, PendingPreparation = 1, Searching = 2, Complete = 3,
		Partial = 4, Unreachable = 5, InvalidRequest = 6, InvalidEndpoint = 7,
		UnavailableCoverage = 8, BudgetExhausted = 9, RejectedCapacity = 10,
		Cancelled = 11, Expired = 12, Stale = 13, Failed = 14
	}

	public enum NavigationReason : uint
	{
		None = 0, MissingSource = 1, DirtyCoverage = 2, MissingNeighbor = 3,
		DomainLimit = 4, ResolutionLimit = 5, NodeLimit = 6, OutputLimit = 7,
		InvalidStart = 8, InvalidDestination = 9, InsufficientClearance = 10,
		MissingSupport = 11, SupersededRequest = 12, SceneReplaced = 13,
		ProviderFailure = 14, Cancelled = 15, Expired = 16
	}

	public enum NavigationCommandState : uint
	{
		Queued = 0, Applied = 1, Rejected = 2, Expired = 3
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct NavigationWorldHandle
	{
		public ulong WorldId;
		public ulong WorldGeneration;
		public readonly bool IsValid => WorldId != 0 && WorldGeneration != 0;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct NavigationOwnerHandle
	{
		public NavigationWorldHandle World;
		public ulong OwnerId;
		public ulong OwnerGeneration;
		public readonly bool IsValid => World.IsValid && OwnerId != 0 && OwnerGeneration != 0;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct NavigationRequestHandle
	{
		public NavigationOwnerHandle Owner;
		public uint SlotIndex;
		internal uint Reserved;
		public ulong SlotGeneration;
		public readonly bool IsValid => Owner.IsValid && SlotIndex != uint.MaxValue && SlotGeneration != 0;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct NavigationRouteHandle
	{
		public NavigationOwnerHandle Owner;
		public uint SlotIndex;
		internal uint Reserved;
		public ulong SlotGeneration;
		public readonly bool IsValid => Owner.IsValid && SlotIndex != uint.MaxValue && SlotGeneration != 0;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct NavigationAgentHandle
	{
		public NavigationOwnerHandle Owner;
		public uint SlotIndex;
		internal uint Reserved;
		public ulong SlotGeneration;
		public readonly bool IsValid => Owner.IsValid && SlotIndex != uint.MaxValue && SlotGeneration != 0;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct NavigationCommandHandle
	{
		public NavigationOwnerHandle Owner;
		public uint SlotIndex;
		internal uint Reserved;
		public ulong SlotGeneration;
		public readonly bool IsValid => Owner.IsValid && SlotIndex != uint.MaxValue && SlotGeneration != 0;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct NavigationLocation
	{
		public NavigationWorldHandle World;
		public long CellX;
		public long CellY;
		public long CellZ;
		public double OffsetX;
		public double OffsetY;
		public double OffsetZ;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public unsafe struct NavigationProfile
	{
		public ulong ProfileId;
		public ulong Revision;
		public double RadiusMetres;
		public double HeightMetres;
		public double StepHeightMetres;
		public double MaxSlopeDegrees;
		public double ClearanceMarginMetres;
		public double HorizontalResolutionMetres;
		public double VerticalResolutionMetres;
		public uint ExcludedAreasMask;
		internal uint Reserved;
		public fixed double AreaPenalties[32];
	}

	public sealed class NavigationRouteResult
	{
		public NavigationStatus Status { get; internal init; }
		public NavigationRequestLifecycle Lifecycle { get; internal init; }
		public NavigationOutcome Outcome { get; internal init; }
		public NavigationReason Reason { get; internal init; }
		public NavigationRequestHandle Request { get; internal init; }
		public NavigationRouteHandle Route { get; internal init; }
		public ulong IntentGeneration { get; internal init; }
		public NavigationLocation[] Points { get; internal init; } = Array.Empty<NavigationLocation>();
		public string Diagnostic { get; internal init; } = string.Empty;
		public bool Complete => Status == NavigationStatus.Success && Outcome == NavigationOutcome.Complete;
	}

	public sealed class NavigationCommandResult
	{
		public NavigationStatus Status { get; internal init; }
		public NavigationCommandState State { get; internal init; }
		public NavigationCommandHandle Command { get; internal init; }
		public NavigationRequestHandle ResultingRequest { get; internal init; }
		public ulong ResultingIntentGeneration { get; internal init; }
		public string Diagnostic { get; internal init; } = string.Empty;
	}

	public sealed class NavigationAgentState
	{
		public NavigationStatus Status { get; internal init; }
		public NavigationAgentHandle Agent { get; internal init; }
		public ulong IntentGeneration { get; internal init; }
		public ulong PoseGeneration { get; internal init; }
		public NavigationRequestHandle ActiveRequest { get; internal init; }
		public NavigationRouteHandle ActiveRoute { get; internal init; }
		public NavigationLocation CurrentPosition { get; internal init; }
		public NavigationLocation Destination { get; internal init; }
		public uint RoutePolicy { get; internal init; }
		public bool IsAvailable => Status == NavigationStatus.Success && Agent.IsValid;
	}

	public readonly struct NavigationRouteValidation
	{
		public NavigationStatus Status { get; init; }
		public bool Usable { get; init; }
		public NavigationReason Reason { get; init; }
		public ulong ObservedSourceRevision { get; init; }
	}

	public readonly struct NavigationInvalidation
	{
		public NavigationOwnerHandle Owner { get; init; }
		public NavigationRouteHandle Route { get; init; }
		public NavigationReason Reason { get; init; }
		public NavigationOutcome Outcome { get; init; }
		public ulong SourceRevision { get; init; }
		public ulong ObservationGeneration { get; init; }
	}

	public sealed class NavigationInvalidationResult
	{
		public NavigationStatus Status { get; internal init; }
		public ulong NextCursor { get; internal init; }
		public NavigationInvalidation[] Records { get; internal init; } = Array.Empty<NavigationInvalidation>();
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NavigationAbiHeader
	{
		internal uint AbiVersion;
		internal uint StructSize;

		internal static NavigationAbiHeader For<T>() where T : unmanaged => new()
		{
			AbiVersion = Navigation.AbiVersionConstant,
			StructSize = checked((uint)Marshal.SizeOf<T>())
		};
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal unsafe struct NativeNavigationRouteRequest
	{
		internal NavigationAbiHeader Header;
		internal NavigationOwnerHandle Owner;
		internal NavigationProfile Profile;
		internal NavigationLocation Start;
		internal NavigationLocation Destination;
		internal uint AllowPartial;
		internal uint AllowEndpointAdjustment;
		internal uint MaximumExpansions;
		internal uint Reserved;
		internal ulong CorrelationId;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal unsafe struct NativeNavigationSubmitReceipt
	{
		internal NavigationAbiHeader Header;
		internal NavigationStatus Status;
		internal uint Reserved;
		internal NavigationRequestHandle Request;
		internal ulong CorrelationId;
		internal fixed byte Diagnostic[Navigation.MaximumDiagnosticBytes];
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationRequestSnapshot
	{
		internal NavigationAbiHeader Header;
		internal NavigationStatus Status;
		internal NavigationRequestLifecycle Lifecycle;
		internal NavigationOutcome Outcome;
		internal NavigationReason Reason;
		internal NavigationRequestHandle Request;
		internal NavigationRouteHandle Route;
		internal ulong IntentGeneration;
		internal NavigationLocation Start;
		internal NavigationLocation Destination;
		internal ulong CreatedTick;
		internal ulong ExpiryTick;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationRoutePoint
	{
		internal NavigationAbiHeader Header;
		internal NavigationLocation Location;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationBufferInfo
	{
		internal NavigationAbiHeader Header;
		internal NavigationStatus Status;
		internal uint RequiredCount;
		internal uint CopiedCount;
		internal uint Reserved;
		internal ulong PinnedGeneration;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationRouteValidationRequest
	{
		internal NavigationAbiHeader Header;
		internal NavigationRouteHandle Route;
		internal NavigationLocation CurrentPosition;
		internal ulong ExpectedSourceRevision;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationRouteValidationResult
	{
		internal NavigationAbiHeader Header;
		internal NavigationStatus Status;
		internal uint Usable;
		internal NavigationReason Reason;
		internal uint Reserved;
		internal ulong ObservedSourceRevision;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationCancellationRequest
	{
		internal NavigationAbiHeader Header;
		internal NavigationRequestHandle Request;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationDestinationRequest
	{
		internal NavigationAbiHeader Header;
		internal NavigationAgentHandle Agent;
		internal NavigationLocation Destination;
		internal uint RoutePolicy;
		internal uint CapabilityMask;
		internal ulong ExpectedIntentGeneration;
		internal ulong CorrelationId;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal unsafe struct NativeNavigationCommandReceipt
	{
		internal NavigationAbiHeader Header;
		internal NavigationStatus Status;
		internal NavigationCommandState State;
		internal NavigationCommandHandle Command;
		internal NavigationRequestHandle ResultingRequest;
		internal ulong ResultingIntentGeneration;
		internal fixed byte Diagnostic[Navigation.MaximumDiagnosticBytes];
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationCommandQuery
	{
		internal NavigationAbiHeader Header;
		internal NavigationCommandHandle Command;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationAgentSnapshot
	{
		internal NavigationAbiHeader Header;
		internal NavigationStatus Status;
		internal uint RoutePolicy;
		internal NavigationAgentHandle Agent;
		internal ulong IntentGeneration;
		internal ulong PoseGeneration;
		internal NavigationRequestHandle ActiveRequest;
		internal NavigationRouteHandle ActiveRoute;
		internal NavigationLocation CurrentPosition;
		internal NavigationLocation Destination;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationInvalidationCursor
	{
		internal NavigationAbiHeader Header;
		internal NavigationOwnerHandle Owner;
		internal ulong Cursor;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeNavigationInvalidationRecord
	{
		internal NavigationAbiHeader Header;
		internal NavigationOwnerHandle Owner;
		internal NavigationRouteHandle Route;
		internal NavigationReason Reason;
		internal NavigationOutcome Outcome;
		internal ulong SourceRevision;
		internal ulong ObservationGeneration;
	}

	internal static unsafe class NavigationInterop
	{
		internal static string ReadDiagnostic(byte* text)
		{
			int length = 0;
			while (length < Navigation.MaximumDiagnosticBytes && text[length] != 0)
			{
				++length;
			}
			return length == 0 ? string.Empty : Encoding.UTF8.GetString(text, length);
		}
	}
}
