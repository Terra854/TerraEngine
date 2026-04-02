using System;

namespace TerraEngine
{
    public class Component
    {
        public Object Object { get; internal set; } = null!;
        public Transform Transform => Object.Transform;

        protected unsafe T GetProperty<T>(string propertyName) where T : unmanaged
        {
            T value = default;
            ScriptType type = GetScriptType<T>();
            if (type == (ScriptType)(-1))
            {
                Log.Error($"Unsupported type for property {propertyName}");
                return value;
            }

            InternalCalls.Native_GetComponentProperty(Object.ID, this.GetType().Name, propertyName, &value, (int)type);
            return value;
        }

        protected unsafe void SetProperty<T>(string propertyName, T value) where T : unmanaged
        {
            ScriptType type = GetScriptType<T>();
            if (type == (ScriptType)(-1))
            {
                Log.Error($"Unsupported type for property {propertyName}");
                return;
            }

            InternalCalls.Native_SetComponentProperty(Object.ID, this.GetType().Name, propertyName, &value, (int)type);
        }

        private static ScriptType GetScriptType<T>()
        {
            var type = typeof(T);
            if (type == typeof(bool)) return ScriptType.Bool;
            if (type == typeof(int)) return ScriptType.Int;
            if (type == typeof(float)) return ScriptType.Float;
            if (type == typeof(Vector2)) return ScriptType.Vector2;
            if (type == typeof(Vector3)) return ScriptType.Vector3;
            if (type == typeof(Vector4)) return ScriptType.Vector4;
            if (type == typeof(Quaternion)) return ScriptType.Quaternion;
            if (type == typeof(uint)) return ScriptType.EntityID;
            return (ScriptType)(-1);
        }
    }
}
