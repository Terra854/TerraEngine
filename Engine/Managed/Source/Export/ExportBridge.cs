// ExportBridge.cs — Engine internal call forwarding for NativeAOT exported builds.
//
// In editor mode, managed scripts call back into the engine through
// TerraEngine.InternalCalls which resolves via hostfxr + UnmanagedCallersOnly.
// In export mode, the NativeAOT library cannot use hostfxr-managed delegates,
// so ExportBridge provides equivalent static wrappers that call the same native
// functions through DllImport (P/Invoke), which NativeAOT supports natively.
//
// This file is conditionally compiled only when the gameplay project targets
// NativeAOT publish (TERRAENGINE_EXPORT preprocessor symbol).

using System;
using System.Runtime.InteropServices;
using System.Text;

namespace TerraEngine.Export
{
	public static partial class ExportBridge
	{
		private const string EngineDll = "TerraEngine.dll";

		/// <summary>
		/// Writes a message to the engine's log stream.
		/// </summary>
		[LibraryImport(EngineDll, EntryPoint = "TerraEngine_Log")]
		public static partial void Log(int level, [MarshalAs(UnmanagedType.LPUTF8Str)] string message);

		/// <summary>
		/// Returns the current simulation time in seconds.
		/// </summary>
		[LibraryImport(EngineDll, EntryPoint = "TerraEngine_GetTime")]
		public static partial float GetTime();

		/// <summary>
		/// Returns the time elapsed since the last frame.
		/// </summary>
		[LibraryImport(EngineDll, EntryPoint = "TerraEngine_GetDeltaTime")]
		public static partial float GetDeltaTime();

		/// <summary>
		/// Returns whether a keyboard key is currently held down.
		/// </summary>
		[LibraryImport(EngineDll, EntryPoint = "TerraEngine_IsKeyDown")]
		[return: MarshalAs(UnmanagedType.Bool)]
		public static partial bool IsKeyDown(int keyCode);

		/// <summary>
		/// Returns whether a keyboard key was just pressed this frame.
		/// </summary>
		[LibraryImport(EngineDll, EntryPoint = "TerraEngine_IsKeyPressed")]
		[return: MarshalAs(UnmanagedType.Bool)]
		public static partial bool IsKeyPressed(int keyCode);

		public static unsafe bool TryWriteValue<T>(void* destination, int valueSize, T value) where T : unmanaged
		{
			if (destination == null || valueSize != sizeof(T))
			{
				return false;
			}

			*(T*)destination = value;
			return true;
		}

		public static unsafe bool TryReadValue<T>(void* source, int valueSize, out T value) where T : unmanaged
		{
			value = default;
			if (source == null || valueSize != sizeof(T))
			{
				return false;
			}

			value = *(T*)source;
			return true;
		}

		public static unsafe void CopyUtf8Name(ref ExportFieldDescriptor descriptor, string name)
		{
			byte[] utf8Name = Encoding.UTF8.GetBytes(name ?? string.Empty);
			int copyLength = Math.Min(63, utf8Name.Length);

			for (int index = 0; index < 64; ++index)
			{
				descriptor.NameUtf8[index] = 0;
			}

			for (int index = 0; index < copyLength; ++index)
			{
				descriptor.NameUtf8[index] = utf8Name[index];
			}
		}

		public static unsafe void CopyUtf8Name(byte* destination, int capacity, string name)
		{
			if (destination == null || capacity <= 0)
			{
				return;
			}

			for (int index = 0; index < capacity; ++index)
			{
				destination[index] = 0;
			}

			byte[] utf8Name = Encoding.UTF8.GetBytes(name ?? string.Empty);
			int copyLength = Math.Min(capacity - 1, utf8Name.Length);
			for (int index = 0; index < copyLength; ++index)
			{
				destination[index] = utf8Name[index];
			}
		}
	}
}
