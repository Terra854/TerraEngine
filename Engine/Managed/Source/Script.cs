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

		internal void InvokeCreated()
		{
			OnAwake();
			if (Enabled)
			{
				OnEnable();
			}
		}

		internal void InvokeStartIfNeeded()
		{
			if (_started)
			{
				return;
			}

			_started = true;
			OnStart();
		}

		internal void ResetLifecycle()
		{
			_started = false;
		}
	}
}
