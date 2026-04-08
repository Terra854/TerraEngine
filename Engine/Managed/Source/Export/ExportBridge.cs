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

namespace TerraEngine.Export
{
	internal static partial class ExportBridge
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
	}
}
