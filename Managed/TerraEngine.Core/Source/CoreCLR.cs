using System;
using System.Runtime.InteropServices;

namespace TerraEngine.Core
{
    public enum LogLevel
    {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3,
        Fatal = 4
    }

    public static class InternalCalls
    {
        [UnmanagedCallersOnly]
        public static void Initialize()
        {
            Log.Info("TerraEngine.Core Managed Initialized!");
        }

        [UnmanagedCallersOnly]
        public static void OnUpdate(uint entityId, IntPtr classNamePtr, float deltaTime)
        {
            string className = Marshal.PtrToStringAnsi(classNamePtr) ?? "Unknown";
            // Logic for instance lookup and update would go here
        }

        // Native function imports
        [DllImport("TerraEngine-Engine")]
        internal static extern void Native_Log(int level, string message);

        [DllImport("TerraEngine-Engine")]
        internal static extern uint Native_Entity_Create(string? name);

        [DllImport("TerraEngine-Engine")]
        internal static extern void Native_Entity_Destroy(uint entityId);

        [DllImport("TerraEngine-Engine")]
        internal static extern void Native_Transform_GetPosition(uint entityId, out float x, out float y, out float z);

        [DllImport("TerraEngine-Engine")]
        internal static extern void Native_Transform_SetPosition(uint entityId, float x, float y, float z);
    }

    public static class Log
    {
        public static void Info(string message) => InternalCalls.Native_Log((int)LogLevel.Info, message);
        public static void Debug(string message) => InternalCalls.Native_Log((int)LogLevel.Debug, message);
        public static void Warning(string message) => InternalCalls.Native_Log((int)LogLevel.Warning, message);
        public static void Error(string message) => InternalCalls.Native_Log((int)LogLevel.Error, message);
    }

    public struct Entity
    {
        public readonly uint Id;

        public Entity(uint id)
        {
            Id = id;
        }

        public Transform Transform => new Transform(this);

        public static Entity Create(string? name = null) => new Entity(InternalCalls.Native_Entity_Create(name));
        public void Destroy() => InternalCalls.Native_Entity_Destroy(Id);
    }

    public class Transform
    {
        private readonly Entity _entity;

        internal Transform(Entity entity)
        {
            _entity = entity;
        }

        public Vector3 Position
        {
            get
            {
                InternalCalls.Native_Transform_GetPosition(_entity.Id, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set
            {
                InternalCalls.Native_Transform_SetPosition(_entity.Id, value.X, value.Y, value.Z);
            }
        }
    }
}