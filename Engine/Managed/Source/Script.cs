namespace TerraEngine
{
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

		public virtual void OnAwake() { }
		public virtual void OnEnable() { }
		public virtual void OnStart() { }
		public virtual void OnUpdate() { }
		public virtual void OnDisable() { }
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

		public void InvokeStartIfNeeded()
		{
			if (_started)
			{
				return;
			}

			_started = true;
			OnStart();
		}

		public void InvokeDestroyed()
		{
			// Destroy should mirror a real shutdown sequence so gameplay cleanup
			// sees Disable before Destroy when the script was active.
			if (_enableCallbackActive)
			{
				OnDisable();
				_enableCallbackActive = false;
			}

			OnDestroy();
		}

		public void ResetLifecycle()
		{
			_started = false;
			_enableCallbackActive = false;
			_transform = null;
		}
	}
}
