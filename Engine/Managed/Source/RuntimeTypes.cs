// RuntimeTypes.cs - fixed-width packets shared by managed runtime services.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine
{
	public enum RuntimeStatus : uint
	{
		Success = 0,
		Accepted = 1,
		InvalidArgument = 2,
		InvalidState = 3,
		NotFound = 4,
		Conflict = 5,
		VersionMismatch = 6
	}

	internal enum NativeRuntimeOperation : uint
	{
		ObjectGetParent = 0,
		ObjectSetParent = 1,
		ObjectCopyChildren = 2,
		ObjectCopyAll = 3,
		ObjectCopyWithTag = 4,
		InputReadSnapshot = 5,
		InputSetMousePosition = 6,
		TimeReadSnapshot = 7,
		TimeSetScale = 8,
		SceneGetActive = 9,
		SceneGetLoaded = 10,
		SceneInstantiatePrefab = 11,
		SceneUnloadAll = 12,
		SceneReadRenderSettings = 13,
		SceneSubmitRenderSettings = 14,
		ResourceReadStatus = 15,
		ResourceRequestRelease = 16,
		PhysicsReadSettings = 17,
		PhysicsSubmitSettings = 18,
		ObjectRequestCreate = 19,
		ObjectResolveStableId = 20,
		SceneRequestLoad = 21,
		SceneRequestLoadPrefab = 22,
		SceneRequestSetActive = 23
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeRuntimeAbiHeader
	{
		public const uint AbiVersion = 1;
		public uint Version;
		public uint StructSize;

		public static NativeRuntimeAbiHeader For<T>() where T : unmanaged
			=> new() { Version = AbiVersion, StructSize = (uint)Marshal.SizeOf<T>() };
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeRuntimeRequest
	{
		public NativeRuntimeAbiHeader Header;
		public NativeRuntimeOperation Operation;
		private uint _reserved;
		public uint ObjectId;
		public uint SecondaryId;
		public uint AssetType;
		public uint Capacity;
		public IntPtr Output;
		public IntPtr RequiredCount;
		public IntPtr Text;
		public double X;
		public double Y;
		public float Scalar;
		private uint _reserved2;

		public static NativeRuntimeRequest For(NativeRuntimeOperation operation)
			=> new() { Header = NativeRuntimeAbiHeader.For<NativeRuntimeRequest>(), Operation = operation };
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal unsafe struct NativeObjectCreationReceipt
	{
		public NativeRuntimeAbiHeader Header;
		public RuntimeStatus Status;
		private uint _reserved;
		public fixed byte StableIdBytes[40];

		public string StableId
		{
			get { fixed (byte* text = StableIdBytes) return Marshal.PtrToStringUTF8((IntPtr)text) ?? string.Empty; }
		}
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal unsafe struct NativeInputSnapshot
	{
		public const int KeyCapacity = 128;
		public const int MouseCapacity = 8;
		public NativeRuntimeAbiHeader Header;
		public RuntimeStatus Status;
		public uint KeyCount;
		public uint MouseButtonCount;
		public uint WorkerReadable;
		public ulong FrameRevision;
		public double CursorX;
		public double CursorY;
		public double ScrollX;
		public double ScrollY;
		public fixed byte KeyStates[KeyCapacity];
		public fixed byte MouseButtonStates[MouseCapacity];
		public fixed byte MousePressedThisFrame[MouseCapacity];
		public fixed byte DiagnosticBytes[192];

		public string Diagnostic
		{
			get { fixed (byte* text = DiagnosticBytes) return Marshal.PtrToStringUTF8((IntPtr)text) ?? string.Empty; }
		}
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeTimeSnapshot
	{
		public NativeRuntimeAbiHeader Header;
		public RuntimeStatus Status;
		private uint _reserved;
		public float DeltaTime;
		public float UnscaledDeltaTime;
		public float FixedDeltaTime;
		public float TimeScale;
		public double ElapsedTime;
		public double UnscaledElapsedTime;
		public ulong FrameCount;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeSceneRenderSettings
	{
		public NativeRuntimeAbiHeader Header;
		public RuntimeStatus Status;
		private uint _reserved;
		public float IndirectStrength;
		public float AmbientOcclusionStrength;
		public uint AmbientOcclusionQuality;
		private uint _reserved2;
		public ulong Generation;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal unsafe struct NativeResourceStreamingStatus
	{
		public NativeRuntimeAbiHeader Header;
		public RuntimeStatus Status;
		public uint AssetType;
		public uint PayloadState;
		public uint Resident;
		public ulong CpuBytes;
		public ulong GpuBytes;
		public ulong Generation;
		public fixed byte DiagnosticBytes[192];

		public string Diagnostic
		{
			get { fixed (byte* text = DiagnosticBytes) return Marshal.PtrToStringUTF8((IntPtr)text) ?? string.Empty; }
		}
	}
}
