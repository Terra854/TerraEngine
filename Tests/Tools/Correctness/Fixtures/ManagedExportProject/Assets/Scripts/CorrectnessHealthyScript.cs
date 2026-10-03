using System;
#if !TERRAENGINE_EXPORT
using System.Globalization;
using System.IO;
#endif
using TerraEngine;

// The runner supplies an isolated, precreated observation directory to this
// editor process. A stable instance identity and increasing count prove that
// export keeps the same gameplay instance usable without relying on log timing.
public sealed partial class CorrectnessHealthyScript : Script
{
	// Receipts observe the live editor instance; exported gameplay keeps only
	// its ordinary callbacks and update state, without acceptance-only file IO.
#if !TERRAENGINE_EXPORT
	private readonly string _instanceId = Guid.NewGuid().ToString("N");
	private readonly string? _observationRoot = Environment.GetEnvironmentVariable("TERRAENGINE_CORRECTNESS_OBSERVATION_ROOT");
#endif
	private ulong _updates;

	public override void OnStart()
	{
#if !TERRAENGINE_EXPORT
		Log.Info("Correctness.HealthyStarted instance=" + _instanceId);
#else
		Log.Info("Correctness.HealthyStarted");
#endif
	}

	public override void OnUpdate()
	{
		++_updates;
#if !TERRAENGINE_EXPORT
		if (string.IsNullOrWhiteSpace(_observationRoot)) return;
		// Replace a small complete receipt atomically so concurrent runner reads
		// cannot mistake a partially written JSON document for a callback failure.
		string receipt = "{\"instanceId\":\"" + _instanceId + "\",\"updates\":" + _updates.ToString(CultureInfo.InvariantCulture) + "}";
		string temporary = Path.Combine(_observationRoot, "healthy.pending");
		try
		{
			File.WriteAllText(temporary, receipt);
			File.Move(temporary, Path.Combine(_observationRoot, "healthy.json"), true);
		}
		catch (IOException)
		{
			// A runner read may temporarily deny replacement on Windows. Retry on
			// the next callback so observing the fixture cannot fault healthy work.
			// Persistent write failures still fail the runner's progress deadline.
		}
		catch (UnauthorizedAccessException)
		{
			// Windows replacement can also report access denied for observational
			// receipt IO. Retry next callback; persistent absence still times out.
		}
#endif
	}
}
