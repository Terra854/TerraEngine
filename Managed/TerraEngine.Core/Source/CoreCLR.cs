using System;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using System.Diagnostics;
using System.Reflection;

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
        internal static extern void Native_Log(int level, string message, string function, int line);

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
        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void Info(string message, [CallerLineNumber] int line = 0) 
            => InternalCalls.Native_Log((int)LogLevel.Info, message, GetCallerName(), line);

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void Debug(string message, [CallerLineNumber] int line = 0) 
            => InternalCalls.Native_Log((int)LogLevel.Debug, message, GetCallerName(), line);

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void Warning(string message, [CallerLineNumber] int line = 0) 
            => InternalCalls.Native_Log((int)LogLevel.Warning, message, GetCallerName(), line);

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void Error(string message, [CallerLineNumber] int line = 0) 
            => InternalCalls.Native_Log((int)LogLevel.Error, message, GetCallerName(), line);

        private static string GetCallerName()
        {
            var method = new StackFrame(2).GetMethod();
            if (method == null || method.DeclaringType == null) return "Unknown";
            return $"{method.DeclaringType.FullName}.{method.Name}";
        }
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