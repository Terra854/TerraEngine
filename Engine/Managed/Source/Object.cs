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
	/// <summary>
	/// Receipt for an object creation queued at a protected scene boundary.
	/// Resolve returns null until the engine publishes the request successfully.
	/// </summary>
	public readonly struct ObjectCreationRequest
	{
		internal ObjectCreationRequest(RuntimeStatus status, string stableId)
		{
			Status = status;
			StableId = stableId;
		}

		public RuntimeStatus Status { get; }
		public string StableId { get; }
		public bool Accepted => Status == RuntimeStatus.Accepted || Status == RuntimeStatus.Success;

		public Object? Resolve()
		{
			if (!Accepted || string.IsNullOrWhiteSpace(StableId)) return null;
			RuntimeStatus status = InternalCalls.Native_Object_ResolveStableId(StableId, out uint objectId);
			return status == RuntimeStatus.Success && Object.IsValidId(objectId) ? new Object(objectId) : null;
		}
	}

	// Object is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public unsafe class Object
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
				// The previous value remains authoritative until the protected flush.
				// Clear this wrapper's local copy instead of publishing the queued value.
				_cachedName = null;
			}
		}

		public string Tag
		{
			get => InternalCalls.ReadNativeUtf8(InternalCalls.Native_Object_GetTag(ID));
			set => InternalCalls.Native_Object_SetTag(ID, value ?? "Untagged");
		}

		public string Layer
		{
			get => InternalCalls.ReadNativeUtf8(InternalCalls.Native_Object_GetLayer(ID));
			set => InternalCalls.Native_Object_SetLayer(ID, value ?? "Default");
		}

		// Active is the authored checkbox. ActiveInHierarchy additionally accounts
		// for disabled ancestors and is therefore read-only runtime state.
		public bool Active
		{
			get => InternalCalls.Native_Object_GetActive(ID);
			set => InternalCalls.Native_Object_SetActive(ID, value);
		}

		public bool ActiveInHierarchy => InternalCalls.Native_Object_GetActiveInHierarchy(ID);

		public bool IsStatic
		{
			get => InternalCalls.Native_Object_GetStatic(ID);
			set => InternalCalls.Native_Object_SetStatic(ID, value);
		}

		// Parent changes use the transactional scene queue. The old hierarchy
		// remains visible until the engine reaches its protected flush boundary.
		public Object? Parent
		{
			get
			{
				uint parentId = InternalCalls.Native_Object_GetParent(ID);
				return IsValidId(parentId) ? new Object(parentId) : null;
			}
			set => SetParent(value);
		}

		public Object[] Children => CopyObjectIds((uint* output, uint capacity, out uint required) =>
			InternalCalls.Native_Object_CopyChildren(ID, output, capacity, out required));

		public RuntimeStatus SetParent(Object? parent)
		{
			return InternalCalls.Native_Object_SetParent(ID, parent?.ID ?? InvalidID);
		}

		public static Object[] All => CopyObjectIds(InternalCalls.Native_Object_CopyAll);

		public static Object[] FindAllWithTag(string tag)
		{
			if (tag == null) throw new ArgumentNullException(nameof(tag));
			return CopyObjectIds((uint* output, uint capacity, out uint required) =>
				InternalCalls.Native_Object_CopyWithTag(tag, output, capacity, out required));
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

		// Creation returns a stable receipt because publishing an EnTT entity id
		// during a managed callback would expose live storage before the protected
		// scene flush. Resolve the receipt on a later frame.
		public static ObjectCreationRequest Instantiate(string name = "New Object")
		{
			if (string.IsNullOrWhiteSpace(name)) return new ObjectCreationRequest(RuntimeStatus.InvalidArgument, string.Empty);
			NativeObjectCreationReceipt receipt = new() { Header = NativeRuntimeAbiHeader.For<NativeObjectCreationReceipt>() };
			RuntimeStatus status = InternalCalls.Native_Object_RequestCreate(name, ref receipt);
			return new ObjectCreationRequest(status, receipt.StableId);
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
					// The script binding becomes visible only after the scene mutation
					// flush, matching native component membership changes.
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
				Physics.BindComponent(component, this);
				// An accepted deferred add can be configured immediately because its
				// later property writes enter the same ordered mutation queue. Do not
				// claim membership through the read cache before publication.
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
				Physics.BindComponent(component, this);
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

		public bool RemoveComponent<T>() where T : Component
		{
			Type componentType = typeof(T);
			// Transform defines object spatial identity and cannot be removed through
			// the managed composition API.
			if (componentType == typeof(Transform) || typeof(Script).IsAssignableFrom(componentType))
			{
				return false;
			}

			bool removed = InternalCalls.Native_RemoveComponent(
				ID, Component.ResolveNativeComponentTypeName(componentType));
			// Keep the pre-flush membership observable. Generation changes after the
			// protected commit invalidate the cache naturally on the next read.
			return removed;
		}

		// IsValidId bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static bool IsValidId(uint id)
		{
			// EnTT uses its null sentinel as the invalid object marker, which is
			// uint.MaxValue for the current 32-bit entity configuration.
			return id != InvalidID;
		}

		private unsafe delegate RuntimeStatus CopyObjectIdsCall(uint* output, uint capacity, out uint requiredCount);

		private static unsafe Object[] CopyObjectIds(CopyObjectIdsCall copy)
		{
			RuntimeStatus probe = copy(null, 0, out uint count);
			if ((probe != RuntimeStatus.Success && probe != RuntimeStatus.Conflict) || count == 0)
			{
				return Array.Empty<Object>();
			}
			// The cap prevents corrupted native counts from forcing an unbounded
			// managed allocation across the ABI boundary.
			if (count > 1_000_000) throw new InvalidOperationException("Native object enumeration exceeded the managed safety limit.");
			uint[] ids = new uint[count];
			fixed (uint* output = ids)
			{
				RuntimeStatus status = copy(output, count, out uint copiedCount);
				if (status != RuntimeStatus.Success || copiedCount > count) return Array.Empty<Object>();
				Object[] objects = new Object[copiedCount];
				for (int index = 0; index < objects.Length; ++index) objects[index] = new Object(ids[index]);
				return objects;
			}
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
