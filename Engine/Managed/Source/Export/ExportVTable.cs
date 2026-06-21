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

	[StructLayout(LayoutKind.Explicit, Size = 112)]
	public unsafe struct ManagedVTable
	{
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
	}
}
