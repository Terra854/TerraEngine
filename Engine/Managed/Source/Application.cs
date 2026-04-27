// Application.cs — Managed API for application-level state and OS integration.
//
// This file provides the C# interface for accessing global application properties
// such as data paths, platform identifiers, and project metadata. It bridges
// managed gameplay scripts to the engine's native application services via
// internal calls and provides common utilities like URL opening.

namespace TerraEngine
{
	/// <summary>
	/// Identifies which operating system TerraEngine is currently running on.
	/// </summary>
	public enum RuntimePlatform : uint
	{
		Unknown = 0,
		Windows = 1,
		Linux = 2,
		macOS = 3
	}

	/// <summary>
	/// Exposes application-level state and operating-system integration helpers.
	/// </summary>
	public static class Application
	{
		/// <summary>
		/// Gets the writable save-data directory for the current product.
		/// TerraEngine creates the directory on first access when possible.
		/// </summary>
		public static string PersistentDataPath
			=> InternalCalls.ReadNativeUtf8(InternalCalls.Native_Application_GetPersistentDataPath());

		/// <summary>
		/// Gets the directory that TerraEngine uses as the active asset root.
		/// In exports this maps to the mounted runtime content root.
		/// </summary>
		public static string StreamingAssetsPath
			=> InternalCalls.ReadNativeUtf8(InternalCalls.Native_Application_GetStreamingAssetsPath());

		/// <summary>
		/// Gets whether gameplay is currently running inside TerraEngine-Editor.
		/// </summary>
		public static bool IsEditor
			=> InternalCalls.Native_Application_IsEditor();

		/// <summary>
		/// Gets the host operating system TerraEngine is currently running on.
		/// </summary>
		public static RuntimePlatform Platform
			=> (RuntimePlatform)InternalCalls.Native_Application_GetPlatform();

		/// <summary>
		/// Gets the product name defined by the active project's game.json manifest.
		/// </summary>
		public static string ProductName
			=> InternalCalls.ReadNativeUtf8(InternalCalls.Native_Application_GetProductName());

		/// <summary>
		/// Gets the version string defined by the active project's game.json manifest.
		/// </summary>
		public static string Version
			=> InternalCalls.ReadNativeUtf8(InternalCalls.Native_Application_GetVersion());

		/// <summary>
		/// Stops play mode in the editor or shuts down the standalone runtime.
		/// </summary>
		public static void Quit()
			=> InternalCalls.Native_Application_Quit();

		/// <summary>
		/// Opens the supplied URL with the operating system's default handler.
		/// Invalid or unsupported input is logged by the native runtime.
		/// </summary>
		public static void OpenURL(string url)
			=> InternalCalls.Native_Application_OpenURL(url ?? string.Empty);
	}
}
