// Scene.cs — Managed API for scene and prefab lifecycle management.
//
// This file provides the C# interface for loading and switching between scenes
// and prefabs. It coordinates with the native SceneManager to handle asset
// resolution and runtime scene graph updates, allowing scripts to control the
// high-level organization of the gameplay environment.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine
{
	// Structural scene edits are synchronized by the native mutation queue at
	// the end of the engine update pass. Scripts can request scene changes
	// during gameplay, and systems observe them on the next stable frame
	// boundary instead of midway through component iteration.
	// Scene is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public static class Scene
	{
		public static string LoadScene(string path)
		{
			return InternalCalls.ReadNativeUtf8(Native_Scene_LoadScene(path));
		}

		// LoadPrefab bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static string LoadPrefab(string path)
		{
			return InternalCalls.ReadNativeUtf8(Native_Scene_LoadPrefab(path));
		}

        // SetActiveScene bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static bool SetActiveScene(string uuid)
        {
            return Native_Scene_SetActiveScene(uuid);
        }

		[DllImport("TerraEngine")]
		// Native_Scene_LoadScene bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static extern IntPtr Native_Scene_LoadScene(string path);

		[DllImport("TerraEngine")]
		// Native_Scene_LoadPrefab bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static extern IntPtr Native_Scene_LoadPrefab(string path);

        [DllImport("TerraEngine")]
        // Native_Scene_SetActiveScene bridges managed callers to engine state; validate arguments before forwarding to native code.
        private static extern bool Native_Scene_SetActiveScene(string uuid);
    }
}
