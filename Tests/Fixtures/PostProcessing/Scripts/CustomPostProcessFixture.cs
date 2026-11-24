using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using TerraEngine;

// Installed custom-effect lane. The host imports the real dedicated shader
// and assets, authors neutral fixed exposure EV0, and captures both native views.
// Queue admission never counts as rendering: each checkpoint needs native
// effective publication plus a matching external Raw/Processed/Composited ACK.
public sealed partial class CustomPostProcessFixture : Script
{
	private const string Prefix = "TERRAENGINE_CUSTOM_POST_PROCESS_FIXTURE_";
	private static readonly string[] Tags = { "baseline", "enabled", "parameter-changed", "disabled" };
	private enum Phase { Ready, Submit, Await, Capture, Done }
	private readonly string _boot = Guid.NewGuid().ToString("N");
	private readonly List<string> _checks = new();
	private readonly List<PostProcessAuthoringReceipt> _receipts = new();
	private readonly List<JsonElement> _acks = new();
	private string _root = "", _run = "", _failure = "", _status = "pending";
	private PostProcessAssetReference _effect, _shader, _profile;
	private PostProcessCameraAuthoring? _cameraA, _cameraB;
	private PostProcessAuthoringReceipt? _immediate;
	private PostProcessAuthoringSnapshot? _a, _b, _baselineB, _beforeInvalid, _checkpoint;
	private GraphicsSettingsTarget _targetA, _targetB;
	private GraphicsSettingsSnapshot? _graphicsA, _graphicsB;
	private Phase _phase;
	private ulong _request, _baseRequest;
	private readonly List<GraphicsRequestReceipt> _baseReceipts = new();
	private long _phaseStart;
	private int _operation, _capture, _updates;
	private bool _configured, _finished;

	public override void OnStart()
	{
		string? root = Environment.GetEnvironmentVariable(Prefix + "ROOT");
		string? run = Environment.GetEnvironmentVariable(Prefix + "RUN_ID");
		if (string.IsNullOrWhiteSpace(root) && string.IsNullOrWhiteSpace(run)) return;
		try {
			Require(!string.IsNullOrWhiteSpace(root) && !string.IsNullOrWhiteSpace(run), "Incomplete custom fixture environment.");
			_root = Path.GetFullPath(root!); _run = run!;
			Require(Directory.Exists(_root) && _run.All(c => char.IsAsciiLetterOrDigit(c) || c == '-'), "Invalid isolated report directory/run ID.");
			_shader = Asset("SHADER_ID"); _effect = Asset("EFFECT_ID"); _profile = Asset("PROFILE_ID");
			_configured = true; SetPhase(Phase.Ready);
		} catch (Exception error) { Fail(error); }
	}
	public override void OnUpdate()
	{
		if (!_configured) return;
		if (_finished) { Observe(); return; }
		try {
			Require(Stopwatch.GetElapsedTime(_phaseStart).TotalSeconds < 120, "Custom effect phase timed out: " + _phase + "/" + _operation);
			Advance(); if (++_updates % 20 == 0) Observe();
		} catch (Exception error) { Fail(error); }
	}
	private void Advance()
	{
		switch (_phase) {
		case Phase.Ready:
			var ca = TerraEngine.Object.Find("CameraA")?.GetComponent<Camera>();
			var cb = TerraEngine.Object.Find("CameraB")?.GetComponent<Camera>();
			if (ca is null || cb is null) return;
			_cameraA = PostProcessAuthoring.ForCamera(ca); _cameraB = PostProcessAuthoring.ForCamera(cb);
			_targetA = Graphics.ForCamera(ca); _targetB = Graphics.ForCamera(cb);
			if (!PrepareCommonBase()) return;
			if (!Refresh()) return;
			Require(_graphicsA!.Target.CameraId == "75000000000000000000000000000001" && _graphicsB!.Target.CameraId == "75000000000000000000000000000002", "Fixture camera UUIDs differ from imported inputs.");
			// Pin actual normal-view identities before dedicated captures add outputs.
			_targetA = _graphicsA!.Target; _targetB = _graphicsB!.Target;
			_cameraA = _cameraA.WithView(_targetA.ViewType, _targetA.OutputKind, _targetA.OutputId, _targetA.ViewLifetimeGeneration);
			_cameraB = _cameraB.WithView(_targetB.ViewType, _targetB.OutputKind, _targetB.OutputId, _targetB.ViewLifetimeGeneration);
			Require(_targetA.HasView && _targetB.HasView, "Both real cameras need accepted normal outputs.");
			_baselineB = _b; SetPhase(Phase.Submit); break;
		case Phase.Submit:
			if (!Refresh()) return;
			PostProcessAuthoringReceipt admission;
			if (_operation == 0) admission = _cameraA!.SetProfile(_profile, _a!);
			else {
				if (_operation is 4 or 5) _beforeInvalid = _a;
				// Invalid controls are sent to native validation, never prefiltered
				// by fixture code; the preceding effective batch must survive intact.
				var parameters = _operation == 4 ? new[] { PostProcessParameterValue.Boolean("gain", true) }
					: _operation == 5 ? new[] { PostProcessParameterValue.Scalar("gain", 3F) }
					: new[] { PostProcessParameterValue.Vector4("tint", new Vector4(.5F, 1F, .75F, 1F)), PostProcessParameterValue.Scalar("gain", _operation >= 3 ? .5F : 1F) };
				admission = _cameraA!.ApplyCustomEffects(new[] { new PostProcessCustomEffectPatch { AssetId = _effect, Enabled = _operation is 2 or 3 or 4 or 5, Parameters = parameters } }, _a!);
			}
			_request = admission.Request.RequestId; _immediate = admission; Remember(admission);
			Require(_request != 0 || (_operation is 4 or 5 && admission.Request.State == GraphicsRequestState.Rejected), "Native authoring request did not supply queue ownership: " + admission.Request.Detail);
			SetPhase(Phase.Await); break;
		case Phase.Await:
			var receipt = _request == 0 ? _immediate : PostProcessAuthoring.ReadRequest(_request); if (receipt is null) return;
			Require(receipt.Request.RequestId == _request, "Another custom receipt was returned."); Remember(receipt);
			if (receipt.Request.State is not (GraphicsRequestState.Applied or GraphicsRequestState.Rejected)) {
				Require(receipt.Request.State is not (GraphicsRequestState.Cancelled or GraphicsRequestState.RestartRequired or GraphicsRequestState.ScheduledRestart), "Unexpected custom request termination."); return;
			}
			if (_operation is not (4 or 5)) Require(receipt.Request.State == GraphicsRequestState.Applied,
				"Custom effect rejected: " + receipt.Request.Reason + "/" + receipt.Request.Detail);
			if (!Refresh()) return;
			if (_operation is 4 or 5) {
				Require(receipt.Request.State == GraphicsRequestState.Rejected && receipt.Request.Reason == GraphicsRequestReason.InvalidValue, "Invalid custom kind/range must reject atomically as InvalidValue.");
				Require(_a!.Revision == _beforeInvalid!.Revision && SameEffects(_a.EffectiveEffects, _beforeInvalid.EffectiveEffects), "Rejected custom batch changed effective values/revision.");
				_checks.Add(_operation == 4 ? "custom-wrong-type-rollback" : "custom-range-rollback");
				++_operation; SetPhase(Phase.Submit); break;
			}
			if (receipt.EffectiveFrameId == 0 || receipt.Request.AppliedFrameId == 0 || receipt.Request.EffectiveRevision == 0 || _a!.EffectiveFrameId < receipt.EffectiveFrameId) return;
			if (_operation == 0) {
				Require(receipt.SceneVisibleFrameId > 0 && _a.AuthoredProfileAssetId == _profile, "Authored profile binding lacks protected scene visibility.");
				_checks.Add("authored-profile-binding"); ++_operation; SetPhase(Phase.Submit); break;
			}
			Require(_a.Descriptors.Any(d => d.AssetId == _effect && d.ShaderAssetId == _shader && d.Available && d.ParameterBytes == 20 && d.Parameters.Any(p => p.Name == "tint" && p.Kind == PostProcessParameterKind.Vector4 && p.OffsetBytes == 0) && d.Parameters.Any(p => p.Name == "gain" && p.Kind == PostProcessParameterKind.Scalar && p.OffsetBytes == 16)), "Actual native descriptor/reflection qualification differs.");
			var effect = _a.EffectiveEffects.SingleOrDefault(e => e.AssetId == _effect);
			Require(effect is not null && effect.Enabled == (_operation is 2 or 3), "Effective enabled state differs from terminal native request.");
			if (_operation is 2 or 3) Require(Gain(effect!) == (_operation == 2 ? 1F : .5F) && Tint(effect!).Equals(new Vector4(.5F, 1F, .75F, 1F)), "Independent effective tint/gain differs.");
			if (_operation == 2) _checks.Add("custom-toggle-on");
			if (_operation == 3) _checks.Add("custom-parameter-change");
			if (_operation == 6) _checks.Add("custom-toggle-off");
			_capture = _operation switch { 1 => 0, 2 => 1, 3 => 2, 6 => 3, _ => throw new InvalidOperationException("Unknown checkpoint.") };
			_checkpoint = _a; SetPhase(Phase.Capture); break;
		case Phase.Capture:
			if (!Refresh()) return;
			Require(_checkpoint is not null && _a!.Revision == _checkpoint.Revision && SameEffects(_a.EffectiveEffects, _checkpoint.EffectiveEffects), "Effective custom state changed during native capture.");
			if (!Acknowledge()) return;
			if (_capture == 3) { _checks.Add("custom-camera-isolation"); SetPhase(Phase.Done); }
			else { ++_operation; SetPhase(Phase.Submit); }
			break;
		case Phase.Done:
			Require(_acks.Count == Tags.Length, "Native capture acknowledgements are incomplete.");
			_status = "passed"; _finished = true; Observe(); break;
		}
	}
	// The same normal Game transaction supplies both cameras with a neutral
	// color chain, including CameraB which never binds the custom profile.
	private bool PrepareCommonBase()
	{
		var current = Graphics.ReadSettings().Snapshot; if (current is not { PublishedFrameId: > 0 }) return false;
		if (_baseRequest == 0) {
			var admission = Graphics.ApplySettings(new[] {
				new GraphicsSettingValue(GraphicsSettingId.ExposureMode, 1U),
				new GraphicsSettingValue(GraphicsSettingId.FixedExposureCompensation, 0F),
				new GraphicsSettingValue(GraphicsSettingId.ToneMappingMode, 0U),
				new GraphicsSettingValue(GraphicsSettingId.BloomEnabled, false)
			}, GraphicsSettingsTarget.Game, current.Revision);
			_baseRequest = admission.RequestId; _baseReceipts.Add(admission);
			Require(_baseRequest != 0, "Neutral Game base request was not admitted: " + admission.Detail); return false;
		}
		var receipt = Graphics.ReadRequest(_baseRequest); if (receipt is null) return false;
		if (!_baseReceipts.Any(r => r.RequestId == receipt.RequestId && r.State == receipt.State)) _baseReceipts.Add(receipt);
		if (receipt.State is not (GraphicsRequestState.Applied or GraphicsRequestState.Rejected)) return false;
		// Play startup can publish another settings revision before the first
		// request reaches capture. Retry from the newest actual snapshot.
		if (receipt is { State: GraphicsRequestState.Rejected, Reason: GraphicsRequestReason.StaleRevision }) { _baseRequest = 0; return false; }
		Require(receipt.State == GraphicsRequestState.Applied, "Neutral Game base failed: " + receipt.Reason + "/" + receipt.Detail);
		if (receipt.AppliedFrameId == 0 || current.PublishedFrameId < receipt.AppliedFrameId || current.Revision < receipt.EffectiveRevision) return false;
		if (!_checks.Contains("neutral-game-base-applied")) _checks.Add("neutral-game-base-applied");
		return true;
	}
	private bool Refresh()
	{
		_a = _cameraA!.Read().Snapshot; _b = _cameraB!.Read().Snapshot;
		_graphicsA = Graphics.ReadSettings(_targetA).Snapshot; _graphicsB = Graphics.ReadSettings(_targetB).Snapshot;
		if (_a is null || _b is null || _graphicsA is not { PublishedFrameId: > 0 } || _graphicsB is not { PublishedFrameId: > 0 }) return false;
		foreach (var graphics in new[] { _graphicsA, _graphicsB }) { bool neutral = graphics.Values.Any(v => v.Id == GraphicsSettingId.ExposureMode && v.Value is uint mode && mode == 1U)
			&& graphics.Values.Any(v => v.Id == GraphicsSettingId.FixedExposureCompensation && v.Value is float ev && ev == 0F)
			&& graphics.Values.Any(v => v.Id == GraphicsSettingId.ToneMappingMode && v.Value is uint tone && tone == 0U)
			&& graphics.Values.Any(v => v.Id == GraphicsSettingId.BloomEnabled && v.Value is bool bloom && !bloom);
			if (!neutral && _phase == Phase.Ready) return false;
			Require(neutral, "Both cameras must retain neutral fixed exposure EV0/legacy tone/BloomOff for every checkpoint."); }
		Require(_a.Target.CameraId == _graphicsA.Target.CameraId && _b.Target.CameraId == _graphicsB.Target.CameraId, "Authoring/effective camera identities differ.");
		if (_baselineB is not null) Require(_b.Revision == _baselineB.Revision && _b.AuthoredProfileAssetId == _baselineB.AuthoredProfileAssetId && SameEffects(_b.EffectiveEffects, _baselineB.EffectiveEffects), "CameraA custom edits escaped into CameraB.");
		return true;
	}
	private static bool SameEffects(IReadOnlyList<PostProcessEffectState> a, IReadOnlyList<PostProcessEffectState> b) => a.Count == b.Count && a.All(x => b.Any(y => x.AssetId == y.AssetId && x.Enabled == y.Enabled && x.Parameters.Count == y.Parameters.Count && x.Parameters.All(p => y.Parameters.Any(q => p.Name == q.Name && p.Kind == q.Kind && Equals(p.Value, q.Value)))));
	private static float Gain(PostProcessEffectState state) => (float)state.Parameters.Single(p => p.Name == "gain").Value;
	private static Vector4 Tint(PostProcessEffectState state) => (Vector4)state.Parameters.Single(p => p.Name == "tint").Value;
	private static PostProcessAssetReference Asset(string suffix) => new(Environment.GetEnvironmentVariable(Prefix + suffix) ?? throw new InvalidOperationException("Missing asset environment: " + suffix));
	private static void Require(bool condition, string detail) { if (!condition) throw new InvalidOperationException(detail); }
	private void SetPhase(Phase phase) { _phase = phase; _phaseStart = Stopwatch.GetTimestamp(); Observe(); }
	private void Remember(PostProcessAuthoringReceipt receipt) { if (!_receipts.Any(r => r.Request.RequestId == receipt.Request.RequestId && r.Request.State == receipt.Request.State && r.EffectiveFrameId == receipt.EffectiveFrameId)) _receipts.Add(receipt); }
	private bool Acknowledge()
	{
		string path = Path.Combine(_root, "custom-post-process-capture-ack-" + Tags[_capture] + ".json"); if (!File.Exists(path)) return false;
		using var json = JsonDocument.Parse(File.ReadAllBytes(path)); var value = json.RootElement;
		Require(value.GetProperty("schemaVersion").GetInt32() == 1 && value.GetProperty("runId").GetString() == _run && value.GetProperty("bootId").GetString() == _boot && value.GetProperty("tag").GetString() == Tags[_capture], "Native capture ACK identity differs.");
		_acks.Add(value.Clone()); _checks.Add(Tags[_capture] + "-native-captures"); return true;
	}
	private void Fail(Exception error) { _failure = error.ToString(); _status = "failed"; _finished = true; Observe(); }
	private static string Decimal(ulong value) => value.ToString(CultureInfo.InvariantCulture);
	// Explicit JSON avoids reflection serialization in exported/AOT runtime lanes.
	private void Observe()
	{
		if (string.IsNullOrEmpty(_root)) return;
		using var stream = new MemoryStream(); using (var w = new Utf8JsonWriter(stream)) {
			w.WriteStartObject(); w.WriteNumber("schemaVersion", 1); w.WriteString("runId", _run); w.WriteString("bootId", _boot); w.WriteString("status", _status); w.WriteString("phase", _phase.ToString()); w.WriteString("captureTag", Tags[_capture]); w.WriteString("requestId", Decimal(_request)); w.WriteString("failure", _failure);
			w.WriteString("effectAssetId", _effect.AssetId); w.WriteString("shaderAssetId", _shader.AssetId); w.WriteString("profileAssetId", _profile.AssetId);
			w.WriteStartArray("checks"); foreach (string check in _checks) w.WriteStringValue(check); w.WriteEndArray();
			w.WriteStartArray("receipts"); foreach (var receipt in _baseReceipts) { w.WriteStartObject(); w.WriteString("requestId", Decimal(receipt.RequestId)); w.WriteString("state", receipt.State.ToString()); w.WriteString("reason", receipt.Reason.ToString()); w.WriteString("effectiveRevision", Decimal(receipt.EffectiveRevision)); w.WriteString("appliedFrameId", Decimal(receipt.AppliedFrameId)); w.WriteString("detail", receipt.Detail); w.WriteEndObject(); } foreach (var receipt in _receipts) { w.WriteStartObject(); w.WriteString("requestId", Decimal(receipt.Request.RequestId)); w.WriteString("state", receipt.Request.State.ToString()); w.WriteString("reason", receipt.Request.Reason.ToString()); w.WriteString("effectiveRevision", Decimal(receipt.Request.EffectiveRevision)); w.WriteString("appliedFrameId", Decimal(receipt.Request.AppliedFrameId)); w.WriteString("sceneVisibleFrameId", Decimal(receipt.SceneVisibleFrameId)); w.WriteString("effectiveFrameId", Decimal(receipt.EffectiveFrameId)); w.WriteString("detail", receipt.Request.Detail); w.WriteEndObject(); } w.WriteEndArray();
			WriteCamera(w, "cameraA", _graphicsA, _a); WriteCamera(w, "cameraB", _graphicsB, _b); WriteEffect(w, _a);
			w.WriteStartArray("captureAcknowledgements"); foreach (var ack in _acks) ack.WriteTo(w); w.WriteEndArray(); w.WriteEndObject();
		}
		string path = Path.Combine(_root, "custom-post-process-observation.json"), temporary = path + ".tmp";
		// Windows may hold a short-lived read lease while the collector polls.
		// Keep the last complete observation and publish again on an update.
		try { File.WriteAllBytes(temporary, stream.ToArray()); File.Move(temporary, path, true); }
		catch (IOException) { }
		catch (UnauthorizedAccessException) { }
	}
	private void WriteCamera(Utf8JsonWriter w, string name, GraphicsSettingsSnapshot? graphics, PostProcessAuthoringSnapshot? authoring)
	{
		w.WritePropertyName(name); if (graphics is null || authoring is null) { w.WriteNullValue(); return; }
		w.WriteStartObject(); var target = graphics.Target; w.WriteString("cameraId", target.CameraId); w.WriteString("revision", Decimal(graphics.Revision)); w.WriteString("frameId", Decimal(graphics.PublishedFrameId)); w.WriteString("sceneGeneration", Decimal(target.SceneGeneration)); w.WriteString("objectLifetimeGeneration", Decimal(target.ObjectLifetimeGeneration)); w.WriteString("componentGeneration", Decimal(target.ComponentGeneration)); w.WriteString("bindingGeneration", Decimal(target.BindingGeneration)); w.WriteString("viewLifetimeGeneration", Decimal(target.ViewLifetimeGeneration)); w.WriteString("rendererSessionGeneration", Decimal(graphics.Lifecycle.RendererSessionGeneration)); w.WriteString("backendResourceGeneration", Decimal(graphics.Lifecycle.BackendResourceGeneration));
		w.WriteString("authoringRevision", Decimal(authoring.Revision)); w.WriteString("authoredProfileAssetId", authoring.AuthoredProfileAssetId.AssetId); w.WriteString("sceneVisibleFrameId", Decimal(authoring.SceneVisibleFrameId)); w.WriteString("effectiveFrameId", Decimal(authoring.EffectiveFrameId));
		WriteEffect(w, authoring); w.WriteEndObject();
	}
	private void WriteEffect(Utf8JsonWriter w, PostProcessAuthoringSnapshot? authoring)
	{
		w.WriteStartObject("customEffect"); var effect = authoring?.EffectiveEffects.SingleOrDefault(e => e.AssetId == _effect); w.WriteString("assetId", _effect.AssetId); w.WriteString("shaderAssetId", _shader.AssetId); w.WriteBoolean("enabled", effect?.Enabled ?? false);
		if (effect is not null) { w.WriteNumber("gain", Gain(effect)); var tint = Tint(effect); w.WriteStartArray("tint"); w.WriteNumberValue(tint.X); w.WriteNumberValue(tint.Y); w.WriteNumberValue(tint.Z); w.WriteNumberValue(tint.W); w.WriteEndArray(); }
		w.WriteEndObject();
	}
}
