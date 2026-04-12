using System;
using System.Runtime.InteropServices;

namespace TerraEngine
{
	public static class Scene
	{
		public static string LoadScene(string path)
		{
			return InternalCalls.ReadNativeUtf8(Native_Scene_LoadScene(path));
		}

		public static string LoadPrefab(string path)
		{
			return InternalCalls.ReadNativeUtf8(Native_Scene_LoadPrefab(path));
		}

        public static bool SetActiveScene(string uuid)
        {
            return Native_Scene_SetActiveScene(uuid);
        }

		[DllImport("TerraEngine")]
		private static extern IntPtr Native_Scene_LoadScene(string path);

		[DllImport("TerraEngine")]
		private static extern IntPtr Native_Scene_LoadPrefab(string path);

        [DllImport("TerraEngine")]
        private static extern bool Native_Scene_SetActiveScene(string uuid);
    }
}
