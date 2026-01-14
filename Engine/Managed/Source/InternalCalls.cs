using System;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;

namespace TerraEngine
{
    internal enum ScriptType : int
    {
        Bool = 0,
        Int = 1,
        Float = 2,
        String = 3,
        Vector2 = 4,
        Vector3 = 5,
        Vector4 = 6,
        Quaternion = 7,
        EntityID = 8
    }

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

        private static System.Collections.Generic.Dictionary<uint, Script> _scriptInstances = new();

        [UnmanagedCallersOnly]
        public static void OnUpdate(uint objectId, IntPtr classNamePtr, float deltaTime)
        {
            try
            {
                Time.DeltaTime = deltaTime;
                if (!_scriptInstances.TryGetValue(objectId, out Script? script))
                {
                    string className = Marshal.PtrToStringAnsi(classNamePtr) ?? "";
                    if (string.IsNullOrEmpty(className)) return;

                    Type? scriptType = Type.GetType(className);
                    if (scriptType == null)
                    {
                        foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies())
                        {
                            scriptType = assembly.GetType(className);
                            if (scriptType != null) break;
                        }
                    }

                    if (scriptType == null)
                    {
                        Log.Error($"Could not find script class: {className}");
                        return;
                    }

                    script = Activator.CreateInstance(scriptType) as Script;
                    if (script == null)
                    {
                        Log.Error($"Class {className} does not inherit from Script.");
                        return;
                    }

                    script.Object = new Object(objectId);
                    _scriptInstances[objectId] = script;
                    script.OnStart();
                }

                script.OnUpdate();
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
        internal static extern bool Native_HasComponent(uint objectId, string componentType);

        [DllImport("TerraEngine")]
        internal static extern bool Native_AddComponent(uint objectId, string componentType);

        [DllImport("TerraEngine")]
        internal static extern unsafe void Native_GetComponentProperty(uint objectId, string componentType, string propertyName, void* outValue, int type);

        [DllImport("TerraEngine")]
        internal static extern unsafe void Native_SetComponentProperty(uint objectId, string componentType, string propertyName, void* inValue, int type);
    }
}
