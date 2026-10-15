// Component.cs — Managed base class for all C# script components.
//
// This file defines the foundational Component class for the managed scripting
// environment. It provides the infrastructure for accessing related components,
// the parent object, and provides the bridge for reading and writing reflected
// properties on native engine components.

using System;
using System.Collections.Concurrent;

namespace TerraEngine
{
	// Component is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class Component
	{
		private static readonly ConcurrentDictionary<Type, string> s_nativeTypeNames = new();
		private Object _object = null!;

		public Object Object
		{
			get => _object;
			internal set
			{
				_object = value;
				// Native wrappers such as Animator must copy their generation-safe
				// identity as soon as Object.GetComponent constructs them. Keeping the
				// hook assembly-internal prevents gameplay types from intercepting the
				// engine-owned binding step.
				OnObjectAssigned();
			}
		}

		internal virtual void OnObjectAssigned()
		{
			// Most reflected components need no extra runtime identity. Animation
			// overrides this hook without changing the existing generic wrappers.
		}

		public Transform Transform => Object.Transform;
		// Every native Component<Derived> reflects this shared active flag. Keeping
		// it on the base class gives all current and future wrappers one consistent
		// way to participate in runtime system filtering.
		public bool Enabled
		{
			get => GetProperty<bool>("active");
			set => SetProperty("active", value);
		}

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

		protected string GetStringProperty(string propertyName)
		{
			return InternalCalls.ReadNativeUtf8(InternalCalls.Native_GetComponentStringProperty(
				Object.ID, ResolveNativeComponentTypeName(GetType()), propertyName));
		}

		protected void SetStringProperty(string propertyName, string? value)
		{
			InternalCalls.Native_SetComponentStringProperty(
				Object.ID, ResolveNativeComponentTypeName(GetType()), propertyName, value ?? string.Empty);
		}

		// ResolveNativeComponentTypeName bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static string ResolveNativeComponentTypeName(Type managedType)
		{
			return s_nativeTypeNames.GetOrAdd(managedType, ResolveNativeComponentTypeNameUncached);
		}

		private static string ResolveNativeComponentTypeNameUncached(Type managedType)
		{
			// Managed API names stay short and idiomatic while native reflection
			// keeps the original engine component names for serialization/tools.
			return managedType.Name switch
			{
				nameof(Camera) => "CameraComponent",
				nameof(Light) => "LightComponent",
				nameof(MeshRenderer) => "MeshRenderComponent",
				nameof(Sky) => "SkyComponent",
				nameof(Rigidbody) => "RigidBodyComponent",
				nameof(ColliderSet) => "ColliderSetComponent",
				nameof(ConstraintSet) => "ConstraintSetComponent",
				nameof(CharacterController) => "CharacterControllerComponent",
				nameof(Animation.Animator) => "AnimatorComponent",
				// Public gameplay lookup and reflected audio fields must target the
				// registered native component, including a newly created facade.
				nameof(Audio.AudioSource) => "AudioSourceComponent",
				nameof(NavigationAgent) => "NavigationAgentComponent",
				nameof(Locomotion.CharacterLocomotion) => "CharacterLocomotionComponent",
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
