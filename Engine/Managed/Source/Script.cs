// Script.cs — Managed base class for gameplay scripts with lifecycle hooks.
//
// This file defines the Script class, which provides the standard lifecycle
// callbacks (OnAwake, OnStart, OnUpdate, etc.) used by gameplay code. It
// implements the dispatch logic that mirrors MonoBehaviour-style semantics,
// ensuring consistent execution patterns for all managed scripts.

namespace TerraEngine
{
	// Script is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class Script : Component
	{
		private bool _enabled = true;
		private bool _started;
		private bool _enableCallbackActive;
		private Transform? _transform;

		public bool Enabled
		{
			get => _enabled;
			set => _enabled = value;
		}

		// Unity-style lowercase access keeps gameplay code familiar while the
		// PascalCase alias matches the rest of the managed TerraEngine API.
		public bool enabled
		{
			get => Enabled;
			set => Enabled = value;
		}

		public new Transform Transform => _transform ??= Object.Transform;

		internal string BindingId { get; set; } = string.Empty;

		// OnAwake bridges managed callers to engine state; validate arguments before forwarding to native code.
		public virtual void OnAwake() { }
		public virtual void OnEnable() { }
		public virtual void OnStart() { }
		// OnUpdate bridges managed callers to engine state; validate arguments before forwarding to native code.
		public virtual void OnUpdate() { }
		// OnDisable bridges managed callers to engine state; validate arguments before forwarding to native code.
		public virtual void OnDisable() { }
		// OnDestroy bridges managed callers to engine state; validate arguments before forwarding to native code.
		public virtual void OnDestroy() { }

		/// <summary>
		/// Binds this managed script instance to the engine object that owns it.
		/// The cached Transform wrapper is reset here so hot reloads or rebinding
		/// cannot leak a stale component reference across objects.
		/// </summary>
		public void BindEngineInstance(uint objectId, string? bindingId = null)
		{
			Object = new Object(objectId);
			BindingId = bindingId ?? string.Empty;
			_transform = null;
		}

		/// <summary>
		/// Runs one gameplay frame while mirroring MonoBehaviour semantics:
		/// enable/disable transitions fire once, Start waits until the script is
		/// active, and Update is skipped entirely when disabled.
		/// </summary>
		public void DispatchFrame(float deltaTime)
		{
			_ = deltaTime;

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (Enabled != _enableCallbackActive)
			{
				if (Enabled)
				{
					OnEnable();
					_enableCallbackActive = true;
				}
				else
				{
					OnDisable();
					_enableCallbackActive = false;
					return;
				}
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (!Enabled)
			{
				return;
			}

			InvokeStartIfNeeded();
			// UI callbacks are pumped once per engine frame from the managed side
			// so scripts can subscribe with standard C# events instead of polling.
			UIBridge.PumpPendingEvents();
			OnUpdate();
		}

		// --- Lifecycle Invokers ---
		// These methods are called by the engine's hosting layer or NativeAOT
		// bootstrap. Gameplay code should override the virtual methods above
		// instead of calling these dispatch helpers directly.

		public void InvokeCreated()
		{
			OnAwake();
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (Enabled)
			{
				OnEnable();
				_enableCallbackActive = true;
			}
			else
			{
				_enableCallbackActive = false;
			}
		}

		// InvokeStartIfNeeded bridges managed callers to engine state; validate arguments before forwarding to native code.
		public void InvokeStartIfNeeded()
		{
			if (_started)
			{
				return;
			}

			_started = true;
			OnStart();
		}

		// InvokeDestroyed bridges managed callers to engine state; validate arguments before forwarding to native code.
		public void InvokeDestroyed()
		{
			// Destroy should mirror a real shutdown sequence so gameplay cleanup
			// sees Disable before Destroy when the script was active.
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (_enableCallbackActive)
			{
				OnDisable();
				_enableCallbackActive = false;
			}

			OnDestroy();
		}

		// ResetLifecycle bridges managed callers to engine state; validate arguments before forwarding to native code.
		public void ResetLifecycle()
		{
			_started = false;
			_enableCallbackActive = false;
			_transform = null;
		}
	}
}
