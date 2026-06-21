// Object.cs — Managed interface for scene entities and component management.
//
// This file defines the foundational Object class in C#, serving as the primary
// handle for interacting with entities in the engine's ECS. It provides methods
// for creating and destroying objects, searching the scene hierarchy, and
// dynamically adding or retrieving both native and managed components.

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;

namespace TerraEngine
{
	// Object is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class Object
	{
		public const uint InvalidID = uint.MaxValue;

		private Transform? _transform;
		private readonly object _componentCacheSync = new();
		private readonly Dictionary<Type, Component?> _componentCache = new();
		private readonly Dictionary<Type, bool> _componentPresenceCache = new();
		private ulong _componentCacheGeneration;
		private ulong _bindingCacheGeneration;
		private string? _cachedName;
		private ulong _nameCacheGeneration;

		public readonly uint ID;

		public string Name
		{
			get
			{
				ulong currentGeneration = ResolveObjectGeneration();
				if (_cachedName != null && _nameCacheGeneration == currentGeneration)
				{
					InternalCalls.RecordManagedCacheHit();
					return _cachedName;
				}

				InternalCalls.RecordManagedCacheMiss();
				_cachedName = InternalCalls.ReadNativeUtf8(InternalCalls.Native_Object_GetName(ID));
				_nameCacheGeneration = currentGeneration;
				return _cachedName;
			}
			set
			{
				InternalCalls.Native_Object_SetName(ID, value);
				_cachedName = value;
				_nameCacheGeneration = InternalCalls.Native_Object_GetMetadataGeneration(ID);
				SeedDispatchCacheGenerations(ID, _nameCacheGeneration, ResolveComponentGeneration(), ResolveBindingGeneration());
			}
		}

		private readonly struct DispatchGenerationState
		{
			public DispatchGenerationState(ulong objectGeneration, ulong componentGeneration, ulong bindingGeneration)
			{
				ObjectGeneration = objectGeneration;
				ComponentGeneration = componentGeneration;
				BindingGeneration = bindingGeneration;
			}

			public ulong ObjectGeneration { get; }
			public ulong ComponentGeneration { get; }
			public ulong BindingGeneration { get; }
		}

		private static readonly ConcurrentDictionary<uint, DispatchGenerationState> s_dispatchGenerations = new();

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
			Type componentType = typeof(T);
			string typeName = componentType.Name;
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (typeof(Script).IsAssignableFrom(componentType))
			{
				string scriptName = componentType.FullName ?? typeName;
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (InternalCalls.Native_ScriptComponent_AddBinding(ID, scriptName))
				{
					T component = new() { Object = this };
					RefreshKnownGenerations();
					StoreCachedComponent(componentType, component);
					StoreCachedPresence(componentType, true, scriptBinding: true);
					return component;
				}

				Log.Error($"Could not add gameplay script {scriptName} to Object {ID}");
				return new T { Object = this };
			}

			string nativeTypeName = Component.ResolveNativeComponentTypeName(componentType);
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (InternalCalls.Native_AddComponent(ID, nativeTypeName))
			{
				T component = new() { Object = this };
				RefreshKnownGenerations();
				StoreCachedComponent(componentType, component);
				StoreCachedPresence(componentType, true, scriptBinding: false);
				return component;
			}

			Log.Error($"Could not add component {nativeTypeName} to Object {ID}");
			return new T { Object = this };
		}

		public T? GetComponent<T>() where T : Component, new()
		{
			Type componentType = typeof(T);
			if (TryGetCachedComponent(componentType, out T? cachedComponent))
			{
				return cachedComponent;
			}

			string typeName = componentType.Name;
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (typeof(Script).IsAssignableFrom(componentType))
			{
				string scriptName = componentType.FullName ?? typeName;
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (InternalCalls.Native_ScriptComponent_HasBinding(ID, scriptName))
				{
					T component = new() { Object = this };
					StoreCachedComponent(componentType, component);
					StoreCachedPresence(componentType, true, scriptBinding: true);
					return component;
				}

				StoreCachedComponent(componentType, null);
				StoreCachedPresence(componentType, false, scriptBinding: true);
				return null;
			}

			string nativeTypeName = Component.ResolveNativeComponentTypeName(componentType);
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (InternalCalls.Native_HasComponent(ID, nativeTypeName))
			{
				T component = new() { Object = this };
				StoreCachedComponent(componentType, component);
				StoreCachedPresence(componentType, true, scriptBinding: false);
				return component;
			}

			StoreCachedComponent(componentType, null);
			StoreCachedPresence(componentType, false, scriptBinding: false);
			return null;
		}

		public bool HasComponent<T>() where T : Component
		{
			Type componentType = typeof(T);
			bool scriptBinding = typeof(Script).IsAssignableFrom(componentType);
			if (TryGetCachedPresence(componentType, scriptBinding, out bool cachedPresence))
			{
				return cachedPresence;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (scriptBinding)
			{
				string scriptName = componentType.FullName ?? componentType.Name;
				bool hasBinding = InternalCalls.Native_ScriptComponent_HasBinding(ID, scriptName);
				StoreCachedPresence(componentType, hasBinding, scriptBinding: true);
				return hasBinding;
			}

			bool hasComponent = InternalCalls.Native_HasComponent(ID, Component.ResolveNativeComponentTypeName(componentType));
			StoreCachedPresence(componentType, hasComponent, scriptBinding: false);
			return hasComponent;
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

		private bool TryGetCachedComponent<T>(Type componentType, out T? component) where T : Component
		{
			lock (_componentCacheSync)
			{
				InvalidateComponentCacheIfNeeded();
				if (_componentCache.TryGetValue(componentType, out Component? cachedComponent))
				{
					component = cachedComponent as T;
					InternalCalls.RecordManagedCacheHit();
					return true;
				}
			}

			InternalCalls.RecordManagedCacheMiss();
			component = null;
			return false;
		}

		private void StoreCachedComponent(Type componentType, Component? component)
		{
			lock (_componentCacheSync)
			{
				_componentCacheGeneration = ResolveComponentGeneration();
				_componentCache[componentType] = component;
			}
		}

		private void InvalidateComponentCacheIfNeeded()
		{
			ulong currentGeneration = ResolveComponentGeneration();
			if (currentGeneration == _componentCacheGeneration)
			{
				return;
			}

			_componentCache.Clear();
			_componentPresenceCache.Clear();
			_componentCacheGeneration = currentGeneration;
		}

		private bool TryGetCachedPresence(Type componentType, bool scriptBinding, out bool present)
		{
			lock (_componentCacheSync)
			{
				InvalidatePresenceCacheIfNeeded(scriptBinding);
				if (_componentPresenceCache.TryGetValue(componentType, out present))
				{
					InternalCalls.RecordManagedCacheHit();
					return true;
				}
			}

			InternalCalls.RecordManagedCacheMiss();
			present = false;
			return false;
		}

		private void StoreCachedPresence(Type componentType, bool present, bool scriptBinding)
		{
			lock (_componentCacheSync)
			{
				if (scriptBinding)
				{
					_bindingCacheGeneration = ResolveBindingGeneration();
				}
				else
				{
					_componentCacheGeneration = ResolveComponentGeneration();
				}
				_componentPresenceCache[componentType] = present;
			}
		}

		private void InvalidatePresenceCacheIfNeeded(bool scriptBinding)
		{
			ulong currentGeneration = scriptBinding ? ResolveBindingGeneration() : ResolveComponentGeneration();
			ulong cachedGeneration = scriptBinding ? _bindingCacheGeneration : _componentCacheGeneration;
			if (currentGeneration == cachedGeneration)
			{
				return;
			}

			_componentPresenceCache.Clear();
			if (scriptBinding)
			{
				_bindingCacheGeneration = currentGeneration;
			}
			else
			{
				_componentCache.Clear();
				_componentCacheGeneration = currentGeneration;
			}
		}

		internal static void SeedDispatchCacheGenerations(uint objectId, ulong objectGeneration, ulong componentGeneration, ulong bindingGeneration)
		{
			if (!IsValidId(objectId))
			{
				return;
			}

			s_dispatchGenerations[objectId] = new DispatchGenerationState(objectGeneration, componentGeneration, bindingGeneration);
		}

		private ulong ResolveObjectGeneration()
		{
			if (s_dispatchGenerations.TryGetValue(ID, out DispatchGenerationState state) && state.ObjectGeneration != 0)
			{
				return state.ObjectGeneration;
			}
			return InternalCalls.Native_Object_GetMetadataGeneration(ID);
		}

		private ulong ResolveComponentGeneration()
		{
			if (s_dispatchGenerations.TryGetValue(ID, out DispatchGenerationState state) && state.ComponentGeneration != 0)
			{
				return state.ComponentGeneration;
			}
			return InternalCalls.Native_Object_GetComponentGeneration(ID);
		}

		private ulong ResolveBindingGeneration()
		{
			if (s_dispatchGenerations.TryGetValue(ID, out DispatchGenerationState state) && state.BindingGeneration != 0)
			{
				return state.BindingGeneration;
			}
			return InternalCalls.Native_Object_GetBindingGeneration(ID);
		}

		private void RefreshKnownGenerations()
		{
			SeedDispatchCacheGenerations(
				ID,
				InternalCalls.Native_Object_GetMetadataGeneration(ID),
				InternalCalls.Native_Object_GetComponentGeneration(ID),
				InternalCalls.Native_Object_GetBindingGeneration(ID));
		}
	}
}
