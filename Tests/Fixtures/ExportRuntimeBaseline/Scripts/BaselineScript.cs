using TerraEngine;

// The export generator discovers partial Script subclasses and emits the
// NativeAOT registry used by the standalone runtime.
public sealed partial class BaselineScript : Script
{
	public override void OnStart()
	{
		// A real UTF-8 data blob and scene name prove ordinary vocabulary remains
		// legal inside NativeAOT output, rather than merely inside source comments.
		System.ReadOnlySpan<byte> ordinaryWords = "legitimate placeholder runtime bytes"u8;
		Log.Info("ExportRuntimeBaseline.ScriptStarted PlaceholderDoor "
			+ System.Text.Encoding.UTF8.GetString(ordinaryWords));
	}
}
