// RuntimeApiSmoke executes the public C# facade against the real native DLL.
// Native unit tests cover mutation semantics in depth; this process catches
// P/Invoke entry-point, enum-order, packet-size, and marshalling regressions.

using TerraEngine;

static void Require(bool condition, string message)
{
	if (!condition)
	{
		throw new InvalidOperationException(message);
	}
}

TimeSnapshot time = Time.Snapshot;
Require(float.IsFinite(time.TimeScale), "The managed time packet did not marshal a finite scale.");

ObjectCreationRequest creation = TerraEngine.Object.Instantiate("Managed Host Smoke");
Require(creation.Accepted, $"Deferred object creation was rejected with {creation.Status}.");
Require(creation.StableId.Length == 32, "Deferred object creation did not return a stable UUID receipt.");
Require(creation.Resolve() == null, "A deferred object unexpectedly became visible without an engine flush.");

Require(Scene.LoadScene(string.Empty) == RuntimeStatus.InvalidArgument, "Scene path validation drifted across the managed boundary.");
Require(ResourceStreaming.Release(string.Empty, StreamableAssetType.Mesh) == RuntimeStatus.InvalidArgument,
	"Resource release validation drifted across the managed boundary.");

Console.WriteLine("TerraEngine managed runtime API smoke passed.");
