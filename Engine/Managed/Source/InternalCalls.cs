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
            try
            {
                Log.Info("TerraEngine Managed Initialized!");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[C#] Critical failure in Initialize:\n{ex}");
            }
        }

        [UnmanagedCallersOnly]
        public static void OnUpdate(uint objectId, IntPtr classNamePtr, float deltaTime)
        {
            try
            {
                string className = Marshal.PtrToStringAnsi(classNamePtr) ?? "Unknown";
                // TODO: Implement script instance lookup and update
            }
            catch (Exception ex)
            {
                Log.Error($"Exception in OnUpdate for Object {objectId}:\n{ex}");
            }
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
