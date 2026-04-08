namespace TerraEngine
{
	public class Script : Component
	{
		private bool _started;

		public bool Enabled { get; set; } = true;
		public string BindingId { get; internal set; } = string.Empty;

		public virtual void OnAwake() { }
		public virtual void OnEnable() { }
		public virtual void OnStart() { }
		public virtual void OnUpdate() { }
		public virtual void OnDisable() { }

		/// <summary>
		/// Binds this managed script instance to the engine object that owns it.
		/// Exported NativeAOT builds call this before any lifecycle callback so
		/// gameplay code can safely read Object and BindingId from Awake or Start.
		/// </summary>
		public void BindEngineInstance(uint objectId, string? bindingId = null)
		{
			Object = new Object(objectId);
			BindingId = bindingId ?? string.Empty;
		}

		/// <summary>
		/// Runs one gameplay frame using the same Start-then-Update ordering as
		/// the editor runtime while keeping Time.DeltaTime in sync.
		/// </summary>
		public void DispatchFrame(float deltaTime)
		{
			Time.DeltaTime = deltaTime;
			InvokeStartIfNeeded();
			if (Enabled)
			{
				OnUpdate();
			}
			else
			{
				OnDisable();
			}
		}

		// --- Lifecycle Invokers ---
		// These methods are called by the engine's hosting layer or NativeAOT bootstrap.
		// They should not be called manually by gameplay code.

		public void InvokeCreated()
		{
			OnAwake();
			if (Enabled)
			{
				OnEnable();
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

		public void ResetLifecycle()
		{
			_started = false;
		}
	}
}
