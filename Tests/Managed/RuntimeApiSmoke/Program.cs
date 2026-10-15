// RuntimeApiSmoke executes the public C# facade against the real native DLL.
// Native unit tests cover mutation semantics in depth; this process catches
// P/Invoke entry-point, enum-order, packet-size, and marshalling regressions.

using TerraEngine;
using System.Runtime.InteropServices;

static void Require(bool condition, string message)
{
	if (!condition)
	{
		throw new InvalidOperationException(message);
	}
}

// Resolve internal transport records from the public runtime assembly. Exact
// widths/offsets catch accidental ABI drift without exposing packets to games.
void AudioLayout(string name, int size, params (string Name, int Offset)[] fields)
{
	Type type = typeof(TerraEngine.Audio.AudioSource).Assembly.GetType("TerraEngine.Audio." + name, true)!;
	Require(Marshal.SizeOf(type) == size, name + " has an incompatible size.");
	foreach (var field in fields)
		Require(Marshal.OffsetOf(type, field.Name).ToInt32() == field.Offset, name + "." + field.Name + " has an incompatible offset.");
}
AudioLayout("AudioAbiHeader", 8, ("AbiVersion", 0), ("StructSize", 4));
AudioLayout("NativeAudioSourceTarget", 40, ("ObjectId", 0), ("SceneGeneration", 16), ("ObjectGeneration", 24), ("ComponentGeneration", 32));
AudioLayout("NativeAudioSourceBinding", 56, ("Status", 8), ("ObjectEntity", 12), ("Target", 16));
AudioLayout("NativeAudioCommandRequest", 96, ("Kind", 8), ("TargetMode", 12), ("Target", 16), ("PlaybackGeneration", 56), ("RequestedFrame", 64), ("ProducerId", 72), ("CorrelationId", 80), ("IdempotencyKey", 88));
AudioLayout("NativeAudioCommandReceipt", 240, ("Status", 8), ("Rejection", 12), ("BatchSequence", 16), ("RequestedFrame", 24), ("CorrelationId", 32), ("ResolvedPlaybackGeneration", 40), ("Diagnostic", 48));
AudioLayout("NativeAudioPlaybackState", 56, ("PlaybackGeneration", 16), ("Audible", 24), ("Reserved", 28), ("Frame", 32), ("LogicalFrame", 40), ("Gain", 48), ("Pitch", 52));
AudioLayout("NativeAudioMixState", 32, ("Generation", 16), ("Volume", 24));

// Object.GetComponent and reflected properties share this cached resolver.
// Validate the actual resolver rather than a second table in the smoke test.
var componentNameResolver = typeof(Component).GetMethod("ResolveNativeComponentTypeName",
	System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)!;
Require((string?)componentNameResolver.Invoke(null, new object[] { typeof(TerraEngine.Audio.AudioSource) }) == "AudioSourceComponent",
	"AudioSource facade does not resolve the registered native AudioSourceComponent name.");

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
