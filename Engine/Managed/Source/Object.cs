// Object.cs — Managed interface for scene entities and component management.
//
// This file defines the foundational Object class in C#, serving as the primary
// handle for interacting with entities in the engine's ECS. It provides methods
// for creating and destroying objects, searching the scene hierarchy, and
// dynamically adding or retrieving both native and managed components.

namespace TerraEngine
{
	// Object is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class Object
	{
		public const uint InvalidID = uint.MaxValue;

		private Transform? _transform;

		public readonly uint ID;

		public string Name
		{
			get => InternalCalls.ReadNativeUtf8(InternalCalls.Native_Object_GetName(ID));
			set => InternalCalls.Native_Object_SetName(ID, value);
		}

		internal Object(uint id)
		{
			ID = id;
		}

		public Transform Transform => _transform ??= new Transform { Object = this };

		// Instantiate bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static Object Instantiate(string name = "New Object")
		{
			return new Object(InternalCalls.Native_Object_Create(name));
		}

		// Find bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static Object? Find(string name)
		{
			uint objectId = InternalCalls.Native_Object_Find(name);
			return IsValidId(objectId) ? new Object(objectId) : null;
		}

		// FindWithTag bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static Object? FindWithTag(string tag)
		{
			uint objectId = InternalCalls.Native_Object_FindWithTag(tag);
			return IsValidId(objectId) ? new Object(objectId) : null;
		}

		// Destroy bridges managed callers to engine state; validate arguments before forwarding to native code.
		public void Destroy()
		{
			InternalCalls.Native_Object_Destroy(ID);
		}

		public unsafe T AddComponent<T>() where T : Component, new()
		{
			string typeName = typeof(T).Name;
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (typeof(Script).IsAssignableFrom(typeof(T)))
			{
				string scriptName = typeof(T).FullName ?? typeName;
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (InternalCalls.Native_ScriptComponent_AddBinding(ID, scriptName))
				{
					return new T { Object = this };
				}

				Log.Error($"Could not add gameplay script {scriptName} to Object {ID}");
				return new T { Object = this };
			}

			string nativeTypeName = Component.ResolveNativeComponentTypeName(typeof(T));
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (InternalCalls.Native_AddComponent(ID, nativeTypeName))
			{
				return new T { Object = this };
			}

			Log.Error($"Could not add component {nativeTypeName} to Object {ID}");
			return new T { Object = this };
		}

		public T? GetComponent<T>() where T : Component, new()
		{
			string typeName = typeof(T).Name;
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (typeof(Script).IsAssignableFrom(typeof(T)))
			{
				string scriptName = typeof(T).FullName ?? typeName;
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (InternalCalls.Native_ScriptComponent_HasBinding(ID, scriptName))
				{
					return new T { Object = this };
				}

				return null;
			}

			string nativeTypeName = Component.ResolveNativeComponentTypeName(typeof(T));
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (InternalCalls.Native_HasComponent(ID, nativeTypeName))
			{
				return new T { Object = this };
			}

			return null;
		}

		public bool HasComponent<T>() where T : Component
		{
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (typeof(Script).IsAssignableFrom(typeof(T)))
			{
				string scriptName = typeof(T).FullName ?? typeof(T).Name;
				return InternalCalls.Native_ScriptComponent_HasBinding(ID, scriptName);
			}

			return InternalCalls.Native_HasComponent(ID, Component.ResolveNativeComponentTypeName(typeof(T)));
		}

		// IsValidId bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static bool IsValidId(uint id)
		{
			// EnTT uses its null sentinel as the invalid object marker, which is
			// uint.MaxValue for the current 32-bit entity configuration.
			return id != InvalidID;
		}

		// bool bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static implicit operator bool(Object obj) => obj != null && IsValidId(obj.ID);
	}
}
