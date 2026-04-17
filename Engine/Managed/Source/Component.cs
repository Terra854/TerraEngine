using System;

namespace TerraEngine
{
	public class Component
	{
		public Object Object { get; internal set; } = null!;
		public Transform Transform => Object.Transform;

		public T? GetComponent<T>() where T : Component, new()
		{
			return Object.GetComponent<T>();
		}

		public T AddComponent<T>() where T : Component, new()
		{
			return Object.AddComponent<T>();
		}

		protected unsafe T GetProperty<T>(string propertyName) where T : unmanaged
		{
			T value = default;
			ScriptType type = GetScriptType<T>();
			if (type == (ScriptType)(-1))
			{
				Log.Error($"Unsupported type for property {propertyName}");
				return value;
			}

			InternalCalls.Native_GetComponentProperty(Object.ID, ResolveNativeComponentTypeName(GetType()), propertyName, &value, (int)type);
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

			InternalCalls.Native_SetComponentProperty(Object.ID, ResolveNativeComponentTypeName(GetType()), propertyName, &value, (int)type);
		}

		internal static string ResolveNativeComponentTypeName(Type managedType)
		{
			// Managed API names stay short and idiomatic while native reflection
			// keeps the original engine component names for serialization/tools.
			return managedType.Name switch
			{
				nameof(Camera) => "CameraComponent",
				nameof(Light) => "LightComponent",
				nameof(MeshRenderer) => "MeshRenderComponent",
				_ => managedType.Name
			};
		}

		private static ScriptType GetScriptType<T>()
		{
			Type type = typeof(T);

			// Keep the managed component bridge on the same fixed-size value-kind
			// numbering used by the NativeAOT export ABI.
			if (type == typeof(bool)) return ScriptType.Bool;
			if (type == typeof(int)) return ScriptType.Int;
			if (type == typeof(float)) return ScriptType.Float;
			if (type == typeof(uint)) return ScriptType.EntityID;
			if (type == typeof(Vector2)) return ScriptType.Vector2;
			if (type == typeof(Vector3)) return ScriptType.Vector3;
			if (type == typeof(Vector4)) return ScriptType.Vector4;
			if (type == typeof(Quaternion)) return ScriptType.Quaternion;
			return (ScriptType)(-1);
		}
	}
}
