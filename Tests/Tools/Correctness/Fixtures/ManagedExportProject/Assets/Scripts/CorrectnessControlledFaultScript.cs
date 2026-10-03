using System;
#if !TERRAENGINE_EXPORT
using System.IO;
#endif
using TerraEngine;

// A source-authored managed exception exercises the real callback boundary.
// The runner creates fault.release only when it is ready to inspect that frame.
// One instance throws once; the healthy sibling must continue to make progress.
public sealed partial class CorrectnessControlledFaultScript : Script
{
	// The release gate and receipt belong to editor acceptance observations.
	// The actual exported script type remains registered with a safe callback.
#if !TERRAENGINE_EXPORT
	private readonly string? _observationRoot = Environment.GetEnvironmentVariable("TERRAENGINE_CORRECTNESS_OBSERVATION_ROOT");
	private bool _thrown;
#endif

	public override void OnUpdate()
	{
#if !TERRAENGINE_EXPORT
		if (_thrown || string.IsNullOrWhiteSpace(_observationRoot)
			|| !File.Exists(Path.Combine(_observationRoot, "fault.release"))) return;
		_thrown = true;
		// The receipt precedes the throw, correlating this controlled fault with
		// the retained managed exception log. Native script.status counts dispatch
		// failures; the managed bridge catches this callback exception internally.
		File.WriteAllText(Path.Combine(_observationRoot, "fault-observed.txt"), "Correctness.ControlledManagedFault");
		throw new InvalidOperationException("Correctness.ControlledManagedFault");
#endif
	}
}
