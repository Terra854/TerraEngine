// ExportVTable.cs — C# mirror of the C-compatible ManagedVTable interop contract.
//
// This file is part of the core engine source and describes the memory layout
// used to bridge the C++ engine and NativeAOT gameplay library (AC-003).

using System;
using System.Runtime.InteropServices;

namespace TerraEngine.Export
{
	// ExportFieldValueKind is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public enum ExportFieldValueKind : int
	{
		Bool = 0,
		Int = 1,
		Float = 2,
		EntityReference = 3,
		Vector2 = 4,
		Vector3 = 5,
		Vector4 = 6,
		Quaternion = 7,
		String = 8
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public unsafe struct ExportFieldDescriptor
	{
		public int FieldIndex;
		public int ValueKind;
		public int ValueSize;
		public ulong NameHash;
		public fixed byte NameUtf8[64];
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public unsafe struct ExportScriptBatchItem
	{
		public IntPtr Handle;
		public float DeltaTime;
		public ulong FrameOrder;
		public ulong ObjectGeneration;
		public ulong ComponentGeneration;
		public ulong BindingGeneration;
		public uint ObjectId;
		public int WorkerDispatch;
	}

	[StructLayout(LayoutKind.Explicit, Size = 160)]
	public unsafe struct ManagedVTable
	{
		// This released ABI size belongs to InitManagedRuntimeV3 forever. A future
		// tail must add a new constant and initializer instead of changing this one.
		public const ulong V3Size = 152;
		// V4 appends the navigation ABI probe. V1 through V3 initializers retain
		// their released prefixes and sizes, so older NativeAOT libraries remain
		// loadable without a write past their host-owned destination.
		public const ulong V4Size = 160;

		[FieldOffset(0)]  public ulong Version;
		[FieldOffset(8)]  public delegate* unmanaged[Cdecl]<int>                                          GetScriptCount;
		[FieldOffset(16)] public delegate* unmanaged[Cdecl]<ulong*, int, void>                            GetScriptIds;
		[FieldOffset(24)] public delegate* unmanaged[Cdecl]<ulong, uint, IntPtr>                          CreateScript;
		[FieldOffset(32)] public delegate* unmanaged[Cdecl]<IntPtr, void>                                 DestroyScript;
		[FieldOffset(40)] public delegate* unmanaged[Cdecl]<IntPtr, void>                                 InvokeAwake;
		[FieldOffset(48)] public delegate* unmanaged[Cdecl]<IntPtr, void>                                 InvokeStart;
		[FieldOffset(56)] public delegate* unmanaged[Cdecl]<IntPtr, float, void>                          InvokeUpdate;
		[FieldOffset(64)] public delegate* unmanaged[Cdecl]<IntPtr, void>                                 InvokeDisable;
		[FieldOffset(72)] public delegate* unmanaged[Cdecl]<ulong, int>                                   GetFieldCount;
		[FieldOffset(80)] public delegate* unmanaged[Cdecl]<ulong, ExportFieldDescriptor*, int, void>     GetFieldDescriptors;
		[FieldOffset(88)] public delegate* unmanaged[Cdecl]<IntPtr, int, void*, int, void>                GetFieldValue;
		[FieldOffset(96)] public delegate* unmanaged[Cdecl]<IntPtr, int, void*, int, void>                SetFieldValue;
		[FieldOffset(104)] public delegate* unmanaged[Cdecl]<ExportScriptBatchItem*, int, void>          InvokeUpdateBatch;
		// Appended v2 tail. Existing v1 offsets above are never shifted.
		[FieldOffset(112)] public ulong TableSize;
		[FieldOffset(120)] public delegate* unmanaged[Cdecl]<uint> GetPhysicsAbiVersion;
		[FieldOffset(128)] public delegate* unmanaged[Cdecl]<void*, uint, ulong, uint, ulong, void> DispatchPhysicsEventBatch;
		// Appended v3 tail. The engine validates both Version and TableSize before
		// calling either animation pointer, preserving v1/v2 binary compatibility.
		[FieldOffset(136)] public delegate* unmanaged[Cdecl]<ushort> GetAnimationAbiVersion;
		[FieldOffset(144)] public delegate* unmanaged[Cdecl]<void*, uint, long, ulong, uint, uint, void> DispatchAnimationEventBatch;
		// Appended v4 tail. The native host validates the v2 object-route ABI before
		// enabling NativeAOT navigation calls.
		[FieldOffset(152)] public delegate* unmanaged[Cdecl]<uint> GetNavigationAbiVersion;
	}
}
