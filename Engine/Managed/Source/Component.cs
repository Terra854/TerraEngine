// Component.cs — Managed base class for all C# script components.
//
// This file defines the foundational Component class for the managed scripting
// environment. It provides the infrastructure for accessing related components,
// the parent object, and provides the bridge for reading and writing reflected
// properties on native engine components.

using System;

namespace TerraEngine
{
	// Component is the managed-facing contract for this subsystem; keep it predictable for scripts.
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
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (type == (ScriptType)(-1))
			{
				Log.Error($"Unsupported type for property {propertyName}");
				return;
			}

			InternalCalls.Native_SetComponentProperty(Object.ID, ResolveNativeComponentTypeName(GetType()), propertyName, &value, (int)type);
		}

		// ResolveNativeComponentTypeName bridges managed callers to engine state; validate arguments before forwarding to native code.
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
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (type == typeof(uint)) return ScriptType.EntityID;
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (type == typeof(Vector2)) return ScriptType.Vector2;
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (type == typeof(Vector3)) return ScriptType.Vector3;
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (type == typeof(Vector4)) return ScriptType.Vector4;
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (type == typeof(Quaternion)) return ScriptType.Quaternion;
			return (ScriptType)(-1);
		}
	}
}
