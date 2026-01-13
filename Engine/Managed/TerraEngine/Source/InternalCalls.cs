using System;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;

namespace TerraEngine
{
    public static class InternalCalls
    {
        [UnmanagedCallersOnly]
        public static void Initialize()
        {
            Log.Info("TerraEngine Managed Initialized!");
        }

        [UnmanagedCallersOnly]
        public static void OnUpdate(uint objectId, IntPtr classNamePtr, float deltaTime)
        {
            string className = Marshal.PtrToStringAnsi(classNamePtr) ?? "Unknown";
            // TODO: Implement script instance lookup and update
        }

        // Native function imports
        [DllImport("TerraEngine")]
        internal static extern void Native_Log(int level, string message, string function, int line);     

        [DllImport("TerraEngine")]
        internal static extern uint Native_Object_Create(string? name);

        [DllImport("TerraEngine")]
        internal static extern void Native_Object_Destroy(uint objectId);

        [DllImport("TerraEngine")]
        internal static extern void Native_Transform_GetPosition(uint objectId, out float x, out float y, out float z);

        [DllImport("TerraEngine")]
        internal static extern void Native_Transform_SetPosition(uint objectId, float x, float y, float z);
    }
}
