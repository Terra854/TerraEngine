// Time.cs — Contains the managed Time API surface, properties, native-call bindings, and script/editor workflow helpers for C# scripting API types that mirror native engine objects, components, and services.

namespace TerraEngine
{
	public readonly struct TimeSnapshot
	{
		internal TimeSnapshot(NativeTimeSnapshot value)
		{
			DeltaTime = value.DeltaTime;
			UnscaledDeltaTime = value.UnscaledDeltaTime;
			FixedDeltaTime = value.FixedDeltaTime;
			TimeScale = value.TimeScale;
			ElapsedTime = value.ElapsedTime;
			UnscaledElapsedTime = value.UnscaledElapsedTime;
			FrameCount = value.FrameCount;
		}

		public float DeltaTime { get; }
		public float UnscaledDeltaTime { get; }
		public float FixedDeltaTime { get; }
		public float TimeScale { get; }
		public double ElapsedTime { get; }
		public double UnscaledElapsedTime { get; }
		public ulong FrameCount { get; }
	}

    // Time is the managed-facing contract for this subsystem; keep it predictable for scripts.
    public static class Time
    {
		public static TimeSnapshot Snapshot
		{
			get
			{
				NativeTimeSnapshot native = new() { Header = NativeRuntimeAbiHeader.For<NativeTimeSnapshot>() };
				InternalCalls.Native_Time_ReadSnapshot(ref native);
				return new TimeSnapshot(native);
			}
		}

		public static float DeltaTime => Snapshot.DeltaTime;
		public static float UnscaledDeltaTime => Snapshot.UnscaledDeltaTime;
		public static float FixedDeltaTime => Snapshot.FixedDeltaTime;
		public static double ElapsedTime => Snapshot.ElapsedTime;
		public static double UnscaledElapsedTime => Snapshot.UnscaledElapsedTime;
		public static ulong FrameCount => Snapshot.FrameCount;
		public static float TimeScale
		{
			get => Snapshot.TimeScale;
			set
			{
				if (InternalCalls.Native_Time_SetTimeScale(value) != RuntimeStatus.Accepted)
					throw new System.ArgumentOutOfRangeException(nameof(value), "Time scale must be finite and between 0 and 100.");
			}
		}
    }
}
