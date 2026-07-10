using TerraEngine;

// The export generator discovers partial Script subclasses and emits the
// NativeAOT registry used by the standalone runtime.
public sealed partial class BaselineScript : Script
{
	public override void OnStart()
	{
		Log.Info("ExportRuntimeBaseline.ScriptStarted");
	}
}
