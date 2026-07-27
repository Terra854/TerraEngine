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
		// Use the logical library name so .NET resolves TerraEngine.dll on
		// Windows and libTerraEngine.so on Linux from the local launcher folder.
		private const string EngineDll = "TerraEngine";
		public const int MaxExportStringBytes = 256;

		// NativeAOT uses the same fixed-width physics ABI as editor-hosted scripts.
		// The vtable advertises this version before the protected event dispatcher
		// is used, preventing a mixed engine/gameplay build from exchanging packets.
		[LibraryImport(EngineDll, EntryPoint = "Native_Physics_GetAbiVersion")]
		public static partial uint GetPhysicsAbiVersion();

		// Export diagnostics can compare the loaded native animation ABI with the
		// managed AnimationAbi.Version constant before exchanging copied packets.
		[LibraryImport(EngineDll, EntryPoint = "Native_Animation_GetAbiVersion")]
		public static partial ushort GetAnimationAbiVersion();

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
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (source == null || valueSize != sizeof(T))
			{
				return false;
			}

			value = *(T*)source;
			return true;
		}

		// TryWriteString bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static unsafe bool TryWriteString(void* destination, int valueSize, string? value)
		{
			if (destination == null || valueSize <= 0)
			{
				return false;
			}

			byte* bytes = (byte*)destination;
			for (int index = 0; index < valueSize; ++index)
			{
				bytes[index] = 0;
			}

			byte[] utf8Value = Encoding.UTF8.GetBytes(value ?? string.Empty);
			int copyLength = Math.Min(valueSize - 1, utf8Value.Length);
			for (int index = 0; index < copyLength; ++index)
			{
				bytes[index] = utf8Value[index];
			}

			return true;
		}

		// TryReadString bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static unsafe bool TryReadString(void* source, int valueSize, out string value)
		{
			value = string.Empty;
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (source == null || valueSize <= 0)
			{
				return false;
			}

			byte* bytes = (byte*)source;
			int length = 0;
			while (length < valueSize && bytes[length] != 0)
			{
				++length;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (length == 0)
			{
				return true;
			}

			byte[] utf8Value = new byte[length];
			for (int index = 0; index < length; ++index)
			{
				utf8Value[index] = bytes[index];
			}

			value = Encoding.UTF8.GetString(utf8Value);
			return true;
		}

		// CopyUtf8Name bridges managed callers to engine state; validate arguments before forwarding to native code.
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

		// CopyUtf8Name bridges managed callers to engine state; validate arguments before forwarding to native code.
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
