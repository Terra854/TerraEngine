using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using TerraEngine;

// Two-camera effects ingress. All settings use the normal typed transaction facade.
// An external native fixture owns the near/far geometry, camera/object motion,
// fixed HUD, actual image captures and pixel assertions. No ACK means no pass.
// Both views use authored fixed exposure at EV0 for every case. This neutral,
// unchanged color stage keeps genuine Processed/Composited outputs available
// when camera effects are Off; a legacy image is never relabeled as Processed.
public sealed partial class CameraEffectsFixture : Script
{
	private const GraphicsSettingId Dof = (GraphicsSettingId)104, Distance = (GraphicsSettingId)105,
		Range = (GraphicsSettingId)106, Blur = (GraphicsSettingId)107, Motion = (GraphicsSettingId)108,
		MotionStrength = (GraphicsSettingId)109, CameraMotion = (GraphicsSettingId)110, ObjectMotion = (GraphicsSettingId)111;
	private static readonly GraphicsSettingId[] DofFields = { Dof, Distance, Range, Blur };
	private static readonly GraphicsSettingId[] MotionFields = { Motion, MotionStrength, CameraMotion, ObjectMotion };
	private static readonly string[] Names = { "postProcess.depthOfFieldEnabled", "postProcess.depthOfFieldFocusDistance",
		"postProcess.depthOfFieldFocusRange", "postProcess.depthOfFieldBlurStrength", "postProcess.motionBlurEnabled",
		"postProcess.motionBlurStrength", "postProcess.motionBlurCameraContribution", "postProcess.motionBlurObjectContribution" };
	private static readonly string[] Cases = { "baseline", "camera-a-near-focus", "camera-a-far-focus", "camera-b-independent-focus",
		"camera-a-focus-reset", "invalid-focus-rollback", "camera-motion-only", "object-motion-only", "motion-disabled", "camera-a-all-reset", "both-cameras-reset" };
	private enum Phase { Ready, Submit, Await, Capture, Done }
	private sealed record Publication(string CaseId, ulong RequestId, GraphicsRequestReceipt Receipt, GraphicsSettingsSnapshot A, GraphicsSettingsSnapshot B);
	private readonly string _boot = Guid.NewGuid().ToString("N");
	private readonly List<string> _checks = new(), _blocked = new();
	private readonly List<GraphicsRequestReceipt> _receipts = new();
	private readonly List<Publication> _publications = new();
	private readonly List<JsonElement> _acknowledgements = new();
	private GraphicsSettingDescriptor[] _discoveryA = Array.Empty<GraphicsSettingDescriptor>(), _discoveryB = Array.Empty<GraphicsSettingDescriptor>();
	private GraphicsSettingsTarget _targetA, _targetB;
	private GraphicsSettingsSnapshot? _game, _a, _b, _baselineGame, _baselineA, _baselineB, _beforeA, _beforeB, _focusA, _focusB, _session;
	private string _root = string.Empty, _run = string.Empty, _failure = string.Empty, _status = "pending";
	private Phase _phase;
	private long _phaseStarted;
	private int _step, _stable;
	private ulong _request, _updates;
	private bool _configured, _finished, _neutralBaselineVerified;

	public override void OnStart()
	{
		string? root = Environment.GetEnvironmentVariable("TERRAENGINE_CAMERA_EFFECTS_FIXTURE_ROOT");
		string? run = Environment.GetEnvironmentVariable("TERRAENGINE_CAMERA_EFFECTS_FIXTURE_RUN_ID");
		if (string.IsNullOrWhiteSpace(root) && string.IsNullOrWhiteSpace(run)) return;
		try {
			Require(!string.IsNullOrWhiteSpace(root) && !string.IsNullOrWhiteSpace(run), "Missing camera-effects fixture environment inputs.");
			_root = Path.GetFullPath(root!); _run = run!;
			Require(Directory.Exists(_root) && _run.All(c => char.IsAsciiLetterOrDigit(c) || c == '-'), "Invalid isolated observation directory/run identity.");
			_configured = true; SetPhase(Phase.Ready);
		} catch (Exception error) { Fail(error); }
	}
	public override void OnUpdate()
	{
		if (!_configured) return;
		if (_finished) { Observe(_status); return; }
		try {
			++_updates;
			Require(Stopwatch.GetElapsedTime(_phaseStarted).TotalSeconds < 120, "Camera-effects phase timed out: " + _phase + "/" + Cases[_step]);
			Advance();
			if (!_finished && _updates % 30U == 0U) Observe("pending");
		} catch (Exception error) { Fail(error); }
	}

	private void Advance()
	{
		switch (_phase) {
		case Phase.Ready:
			var cameraA = TerraEngine.Object.Find("CameraA")?.GetComponent<Camera>();
			var cameraB = TerraEngine.Object.Find("CameraB")?.GetComponent<Camera>();
			if (cameraA is null || cameraB is null) return;
			_targetA = Graphics.ForCamera(cameraA); _targetB = Graphics.ForCamera(cameraB);
			if (!_targetA.IsValid || !_targetB.IsValid || !Refresh()) return;
			Require(_targetA.CameraId != _targetB.CameraId, "Two distinct native camera owners are required.");
			if (_session is null || !SameLifetime(_session, _game!) || _session.Lifecycle.BackendResourceGeneration != _game!.Lifecycle.BackendResourceGeneration) { _session = _game; _stable = 0; return; }
			if (++_stable < 3) return;
			_discoveryA = Graphics.DiscoverSettings(_targetA); _discoveryB = Graphics.DiscoverSettings(_targetB);
			CheckDiscovery(_discoveryA, "CameraA"); CheckDiscovery(_discoveryB, "CameraB");
			if (_blocked.Count != 0) { _finished = true; Observe("blocked"); return; }
			if (!HasFields(_game!) || !HasFields(_a!) || !HasFields(_b!)) return;
			foreach (var snapshot in new[] { _game!, _a!, _b! })
				Require(Value(snapshot, GraphicsSettingId.ExposureMode) is uint exposure && exposure == 1U
					&& Value(snapshot, GraphicsSettingId.FixedExposureCompensation) is float compensation && compensation == 0F,
					"Native host must author fixed exposure at EV0 for the unchanged neutral color-stage baseline.");
			_neutralBaselineVerified = true;
			Require(!Bool(_a!, Dof) && !Bool(_b!, Dof) && !Bool(_a!, Motion) && !Bool(_b!, Motion), "The maintained fixture must begin with DoF/motion blur disabled.");
			// Pin actual published views so dedicated native captures cannot turn
			// subsequent effective reads into ambiguous no-view queries.
			Require(_a!.Target.HasView && _b!.Target.HasView, "Native cameras must have concrete published views.");
			_targetA = _a.Target; _targetB = _b!.Target;
			_baselineGame = _game; _baselineA = _a; _baselineB = _b; _beforeA = _a; _beforeB = _b;
			SetPhase(Phase.Capture); break;
		case Phase.Submit:
			if (!Refresh()) return;
			_beforeA = _a; _beforeB = _b;
			var target = _step is 3 or 10 ? _targetB : _targetA;
			var current = _step is 3 or 10 ? _b! : _a!;
			GraphicsRequestReceipt admission = _step switch {
				1 => Graphics.ApplySettings(new[] { new GraphicsSettingValue(Dof, true), new GraphicsSettingValue(Distance, 3F), new GraphicsSettingValue(Range, 1F), new GraphicsSettingValue(Blur, 12F) }, target, current.Revision),
				2 => Graphics.ApplySettings(new[] { new GraphicsSettingValue(Distance, 12F), new GraphicsSettingValue(Range, 2F) }, target, current.Revision),
				3 => Graphics.ApplySettings(new[] { new GraphicsSettingValue(Dof, true), new GraphicsSettingValue(Distance, 8F), new GraphicsSettingValue(Range, 2F), new GraphicsSettingValue(Blur, 10F) }, target, current.Revision),
				4 or 10 => Graphics.ResetSettings(DofFields, target, current.Revision),
				5 => Graphics.ApplySettings(new[] { new GraphicsSettingValue(Dof, true), new GraphicsSettingValue(Distance, 0F) }, target, current.Revision),
				6 => Graphics.ApplySettings(new[] { new GraphicsSettingValue(Motion, true), new GraphicsSettingValue(MotionStrength, 1F), new GraphicsSettingValue(CameraMotion, 1F), new GraphicsSettingValue(ObjectMotion, 0F) }, target, current.Revision),
				7 => Graphics.ApplySettings(new[] { new GraphicsSettingValue(CameraMotion, 0F), new GraphicsSettingValue(ObjectMotion, 1F) }, target, current.Revision),
				8 => Graphics.ApplySettings(new[] { new GraphicsSettingValue(Motion, false) }, target, current.Revision),
				9 => Graphics.ResetSettings(MotionFields, target, current.Revision),
				_ => throw new InvalidOperationException("Unknown camera-effects operation.")
			};
			_request = admission.RequestId; Remember(admission); Require(_request != 0, "Native request admission omitted its ID.");
			SetPhase(Phase.Await); break;
		case Phase.Await:
			var receipt = Graphics.ReadRequest(_request); if (receipt is null) return;
			Require(receipt.RequestId == _request, "Receipt lookup returned another request."); Remember(receipt);
			Require(receipt.State is not (GraphicsRequestState.RestartRequired or GraphicsRequestState.ScheduledRestart or GraphicsRequestState.Cancelled), "Camera effects must settle without restart or cancellation.");
			if (receipt.State is not (GraphicsRequestState.Applied or GraphicsRequestState.Rejected) || !Refresh()) return;
			if (_step == 5) {
				Require(receipt.State == GraphicsRequestState.Rejected && receipt.Reason == GraphicsRequestReason.InvalidValue, "Out-of-range focus must reject as InvalidValue.");
				Unchanged(_beforeA!, _a!); Unchanged(_beforeB!, _b!);
			} else {
				Require(receipt.State == GraphicsRequestState.Applied, "Camera effect failed: " + receipt.Reason + "/" + receipt.Detail);
				var effective = _step is 3 or 10 ? _b! : _a!;
				if (receipt.EffectiveRevision == 0 || receipt.AppliedFrameId == 0 || effective.Revision < receipt.EffectiveRevision || effective.PublishedFrameId < receipt.AppliedFrameId) return;
				VerifyStep();
			}
			SameValues(_baselineGame!, _game!);
			_publications.Add(new(Cases[_step], _request, receipt, _a!, _b!));
			_checks.Add(Cases[_step] + "-native-settings-verified"); SetPhase(Phase.Capture); break;
		case Phase.Capture:
			if (!Refresh()) return;
			if (_step == 0) { Unchanged(_baselineA!, _a!); Unchanged(_baselineB!, _b!); }
			else { Unchanged(_publications[^1].A, _a!); Unchanged(_publications[^1].B, _b!); }
			SameValues(_baselineGame!, _game!);
			if (!TryAcknowledge()) return;
			_checks.Add(Cases[_step] + "-native-images-hud-verified");
			_request = 0;
			if (_step + 1 == Cases.Length) SetPhase(Phase.Done); else { ++_step; SetPhase(Phase.Submit); }
			break;
		case Phase.Done:
			Require(_acknowledgements.Count == Cases.Length && _publications.Count == Cases.Length - 1, "Required two-camera image/settings evidence is missing.");
			_finished = true; Observe("passed"); break;
		}
	}

	private void VerifyStep()
	{
		if (_step is 3 or 10) SameValues(_beforeA!, _a!); else SameValues(_beforeB!, _b!);
		var before = _step is 3 or 10 ? _beforeB! : _beforeA!;
		var after = _step is 3 or 10 ? _b! : _a!;
		GraphicsSettingId[] changed = _step switch { 1 or 3 or 4 or 10 => DofFields, 2 => new[] { Distance, Range },
			6 or 9 => MotionFields, 7 => new[] { CameraMotion, ObjectMotion }, 8 => new[] { Motion }, _ => Array.Empty<GraphicsSettingId>() };
		Require(before.Values.Count == after.Values.Count && before.Values.Where(v => !changed.Contains(v.Id)).All(v => after.Values.Any(w => v.Id == w.Id && Equals(v.Value, w.Value)))
			&& before.SampleCount == after.SampleCount && before.InternalWidth == after.InternalWidth && before.InternalHeight == after.InternalHeight,
			"A camera effect transaction changed an unrelated control, sample count or internal quality setting.");
		if (_focusB is not null && _step is >= 4 and < 10) SameValues(_focusB, _b!);
		switch (_step) {
		case 1: Require(Bool(_a!, Dof) && Scalar(_a!, Distance) == 3F && Scalar(_a!, Range) == 1F && Scalar(_a!, Blur) == 12F, "CameraA near focus tuple differs."); break;
		case 2: Require(Bool(_a!, Dof) && Scalar(_a!, Distance) == 12F && Scalar(_a!, Range) == 2F, "CameraA far focus tuple differs."); _focusA = _a; break;
		case 3: SameValues(_focusA!, _a!); Require(Bool(_b!, Dof) && Scalar(_b!, Distance) == 8F, "CameraB independent focus differs."); _focusB = _b; break;
		case 4: Inherited(_a!, DofFields); break;
		case 6: Require(Bool(_a!, Motion) && Scalar(_a!, MotionStrength) == 1F && Scalar(_a!, CameraMotion) == 1F && Scalar(_a!, ObjectMotion) == 0F, "Camera-only motion contribution differs."); Inherited(_a!, DofFields); break;
		case 7: Require(Bool(_a!, Motion) && Scalar(_a!, CameraMotion) == 0F && Scalar(_a!, ObjectMotion) == 1F, "Object-only motion contribution differs."); break;
		case 8: Require(!Bool(_a!, Motion), "Motion disable did not become effective."); Inherited(_a!, DofFields); break;
		case 9: Inherited(_a!, MotionFields); Inherited(_a!, DofFields); break;
		case 10: Inherited(_b!, DofFields); Inherited(_a!, MotionFields); Inherited(_a!, DofFields); break;
		}
	}
	private void CheckDiscovery(GraphicsSettingDescriptor[] catalog, string camera)
	{
		for (int index = 0; index < Names.Length; ++index) {
			var descriptor = catalog.SingleOrDefault(d => (uint)d.Id == 104U + (uint)index);
			if (descriptor is not { Available: true, CameraScope: true }) { _blocked.Add(camera + ": " + Names[index] + " unavailable: " + (descriptor?.Detail ?? "descriptor missing")); continue; }
			Require(descriptor.Name == Names[index] && descriptor.ValueKind == (index is 0 or 4 ? GraphicsValueKind.Boolean : GraphicsValueKind.Scalar)
				&& descriptor.Activation == GraphicsActivation.Rebuild, "Camera discovery changed the agreed typed lens contract.");
		}
	}
	private static bool HasFields(GraphicsSettingsSnapshot snapshot) => Enumerable.Range(104, 8).All(id => snapshot.Values.Any(v => (uint)v.Id == (uint)id));
	private static object Value(GraphicsSettingsSnapshot snapshot, GraphicsSettingId id) => snapshot.Values.Single(v => v.Id == id).Value;
	private static bool Bool(GraphicsSettingsSnapshot snapshot, GraphicsSettingId id) => (bool)Value(snapshot, id);
	private static float Scalar(GraphicsSettingsSnapshot snapshot, GraphicsSettingId id) => (float)Value(snapshot, id);
	private void Inherited(GraphicsSettingsSnapshot snapshot, IEnumerable<GraphicsSettingId> ids) { foreach (var id in ids) Require(Equals(Value(snapshot, id), Value(_game!, id)), "Reset did not restore current Game inheritance."); }
	private static bool SameLifetime(GraphicsSettingsSnapshot a, GraphicsSettingsSnapshot b) => a.Lifecycle.ProjectGeneration == b.Lifecycle.ProjectGeneration && a.Lifecycle.PlaySessionGeneration == b.Lifecycle.PlaySessionGeneration && a.Lifecycle.RendererSessionGeneration == b.Lifecycle.RendererSessionGeneration;
	private static void SameValues(GraphicsSettingsSnapshot a, GraphicsSettingsSnapshot b) => Require(a.Values.Count == b.Values.Count && a.Values.All(v => b.Values.Any(w => v.Id == w.Id && Equals(v.Value, w.Value))), "Independent effective settings changed unexpectedly.");
	private static void Unchanged(GraphicsSettingsSnapshot a, GraphicsSettingsSnapshot b) { SameValues(a, b); Require(a.Revision == b.Revision && a.PostProcessResourceGeneration == b.PostProcessResourceGeneration, "Effective revision/resources changed while capturing or rejecting a batch."); }
	private bool Refresh()
	{
		_game = Graphics.ReadSettings().Snapshot; _a = Graphics.ReadSettings(_targetA).Snapshot; _b = Graphics.ReadSettings(_targetB).Snapshot;
		if (_game is not { Revision: > 0, PublishedFrameId: > 0 } || _a is not { Revision: > 0, PublishedFrameId: > 0 } || _b is not { Revision: > 0, PublishedFrameId: > 0 }) return false;
		Require(!_game.Target.IsCamera, "Game query returned a camera scope.");
		CheckTarget(_a, _targetA); CheckTarget(_b, _targetB);
		if (_phase != Phase.Ready) Require(_session is not null && SameLifetime(_session, _game) && SameLifetime(_game, _a) && SameLifetime(_game, _b), "Camera test crossed project/Play/renderer ownership.");
		return true;
	}
	private static void CheckTarget(GraphicsSettingsSnapshot snapshot, GraphicsSettingsTarget target)
	{
		Require(snapshot.Target.IsCamera && snapshot.Target.CameraId == target.CameraId && snapshot.Target.SceneGeneration == target.SceneGeneration
			&& snapshot.Target.ObjectLifetimeGeneration == target.ObjectLifetimeGeneration && snapshot.Target.ComponentGeneration == target.ComponentGeneration
			&& snapshot.Target.BindingGeneration == target.BindingGeneration, "Effective query returned another camera lifetime.");
		if (target.HasView) Require(snapshot.Target.HasView && snapshot.Target.ViewLifetimeGeneration == target.ViewLifetimeGeneration && snapshot.Target.OutputId == target.OutputId
			&& snapshot.Target.OutputKind == target.OutputKind && snapshot.Target.ViewType == target.ViewType, "Effective query returned another camera output.");
		Require(snapshot.OutputWidth == 640 && snapshot.OutputHeight == 360, "Maintained camera fixture requires actual 640x360 outputs.");
		if (HasFields(snapshot) && (Bool(snapshot, Dof) || Bool(snapshot, Motion))) Require(snapshot.PostProcessResourceGeneration != 0 && snapshot.PostProcessResourceGeneration == snapshot.Lifecycle.BackendResourceGeneration, "Active camera effect lacks exact native resource preparation.");
	}
	private void Remember(GraphicsRequestReceipt receipt) { if (!_receipts.Any(r => r.RequestId == receipt.RequestId && r.State == receipt.State && r.EffectiveRevision == receipt.EffectiveRevision && r.AppliedFrameId == receipt.AppliedFrameId)) _receipts.Add(receipt); }

	private bool TryAcknowledge()
	{
		string path = Path.Combine(_root, "camera-effects-ack-" + Cases[_step] + ".json");
		if (!File.Exists(path)) return false;
		JsonDocument document;
		try { using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete); document = JsonDocument.Parse(stream); }
		catch (IOException) { return false; } catch (JsonException) { return false; }
		using (document) {
			var ack = document.RootElement;
			Require(ack.GetProperty("schemaVersion").GetInt32() == 1 && ack.GetProperty("runId").GetString() == _run && ack.GetProperty("bootId").GetString() == _boot
				&& ack.GetProperty("caseId").GetString() == Cases[_step] && ack.GetProperty("requestId").GetString() == Decimal(_request), "Native image ACK belongs to another camera test transaction.");
			foreach (string flag in new[] { "nativeImagesVerified", "hudUnchangedVerified", "cameraIsolationVerified", "focusDepthInputVerified" }) Require(ack.GetProperty(flag).GetBoolean(), "Native ACK omitted " + flag);
			string[] flags = _step switch {
				0 => new[] { "disabledMatchesReferenceVerified" },
				1 or 3 => new[] { "focusVerified" }, 2 => new[] { "focusVerified", "focusShiftVerified" },
				4 or 9 or 10 => new[] { "resetMatchesReferenceVerified" }, 5 => new[] { "atomicRollbackVerified" },
				6 => new[] { "cameraMotionContributionVerified", "objectMotionContributionDisabledVerified" },
				7 => new[] { "objectMotionContributionVerified", "cameraMotionContributionDisabledVerified" },
				8 => new[] { "motionDisabledMatchesReferenceVerified", "depthOfFieldUnchangedVerified" },
				_ => throw new InvalidOperationException("Unknown native image ACK case.")
			};
			foreach (string flag in flags) Require(ack.GetProperty(flag).GetBoolean(), "Native image assertion failed: " + flag);
			CheckImagePair(ack.GetProperty("cameraA"), _a!); CheckImagePair(ack.GetProperty("cameraB"), _b!);
			_acknowledgements.Add(ack.Clone()); return true;
		}
	}
	private void CheckImagePair(JsonElement row, GraphicsSettingsSnapshot snapshot)
	{
		Require(row.GetProperty("cameraId").GetString() == snapshot.Target.CameraId && row.GetProperty("settingsViewLifetime").GetString() == Decimal(snapshot.Target.ViewLifetimeGeneration)
			&& row.GetProperty("renderer").GetString() == Decimal(snapshot.Lifecycle.RendererSessionGeneration) && row.GetProperty("backend").GetString() == Decimal(snapshot.Lifecycle.BackendResourceGeneration), "Native capture camera/view/backend provenance differs.");
		foreach (string stage in new[] { "processed", "composited" }) {
			Require(row.GetProperty(stage + "NativeBackendReadback").GetBoolean() && !row.GetProperty(stage + "DeterministicFallback").GetBoolean()
				&& row.GetProperty(stage + "GraphicsSettingsRevision").GetString() == Decimal(snapshot.Revision)
				&& ulong.Parse(row.GetProperty(stage + "FrameId").GetString()!, CultureInfo.InvariantCulture) >= snapshot.PublishedFrameId
				&& ulong.Parse(row.GetProperty(stage + "SourceTargetGeneration").GetString()!, CultureInfo.InvariantCulture) > 0, "Native capture is fallback, stale or missing an actual target generation.");
			VerifyPng(row.GetProperty(stage + "CapturePath").GetString()!, row.GetProperty(stage + "Sha256").GetString()!, snapshot.OutputWidth, snapshot.OutputHeight);
		}
	}
	private void VerifyPng(string relative, string digest, uint width, uint height)
	{
		string path = Path.GetFullPath(Path.Combine(_root, relative));
		Require(path.StartsWith(_root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase), "Native ACK capture escaped its isolated evidence directory.");
		byte[] bytes = File.ReadAllBytes(path);
		Require(bytes.Length >= 33 && bytes.AsSpan(0, 8).SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }) && bytes.AsSpan(12, 4).SequenceEqual("IHDR"u8)
			&& BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(16, 4)) == width && BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(20, 4)) == height
			&& Convert.ToHexString(SHA256.HashData(bytes)).Equals(digest, StringComparison.OrdinalIgnoreCase), "Native image ACK has wrong extent, PNG bytes or digest.");
	}

	private void SetPhase(Phase phase) { _phase = phase; _phaseStarted = Stopwatch.GetTimestamp(); Observe("pending"); }
	private void Fail(Exception error) { _failure = error.ToString(); _finished = true; if (Directory.Exists(_root)) Observe("failed"); }
	private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
	private static string Decimal(ulong value) => value.ToString(CultureInfo.InvariantCulture);
	private static void WriteSnapshot(Utf8JsonWriter writer, string name, GraphicsSettingsSnapshot? snapshot)
	{
		if (snapshot is null) { writer.WriteNull(name); return; }
		writer.WriteStartObject(name); writer.WriteString("cameraId", snapshot.Target.CameraId); writer.WriteString("viewLifetime", Decimal(snapshot.Target.ViewLifetimeGeneration));
		writer.WriteString("sceneGeneration", Decimal(snapshot.Target.SceneGeneration)); writer.WriteString("objectLifetimeGeneration", Decimal(snapshot.Target.ObjectLifetimeGeneration)); writer.WriteString("componentGeneration", Decimal(snapshot.Target.ComponentGeneration)); writer.WriteString("bindingGeneration", Decimal(snapshot.Target.BindingGeneration));
		writer.WriteString("revision", Decimal(snapshot.Revision)); writer.WriteString("frameId", Decimal(snapshot.PublishedFrameId)); writer.WriteString("project", Decimal(snapshot.Lifecycle.ProjectGeneration)); writer.WriteString("play", Decimal(snapshot.Lifecycle.PlaySessionGeneration));
		writer.WriteString("renderer", Decimal(snapshot.Lifecycle.RendererSessionGeneration)); writer.WriteString("backend", Decimal(snapshot.Lifecycle.BackendResourceGeneration)); writer.WriteString("postProcessResourceGeneration", Decimal(snapshot.PostProcessResourceGeneration));
		writer.WriteNumber("outputWidth", snapshot.OutputWidth); writer.WriteNumber("outputHeight", snapshot.OutputHeight); writer.WriteStartArray("cameraEffects");
		foreach (var field in snapshot.Values.Where(v => (uint)v.Id is >= 104 and <= 111)) { writer.WriteStartObject(); writer.WriteNumber("id", (uint)field.Id); if (field.Value is bool enabled) writer.WriteBoolean("value", enabled); else writer.WriteNumber("value", (float)field.Value); writer.WriteEndObject(); }
		writer.WriteEndArray(); writer.WriteEndObject();
	}
	private static void WriteReceipt(Utf8JsonWriter writer, GraphicsRequestReceipt receipt)
	{
		writer.WriteStartObject(); writer.WriteString("requestId", Decimal(receipt.RequestId)); writer.WriteString("state", receipt.State.ToString()); writer.WriteString("reason", receipt.Reason.ToString()); writer.WriteString("detail", receipt.Detail);
		writer.WriteString("effectiveRevision", Decimal(receipt.EffectiveRevision)); writer.WriteString("appliedFrameId", Decimal(receipt.AppliedFrameId)); writer.WriteEndObject();
	}
	private static void WriteDiscovery(Utf8JsonWriter writer, string name, IEnumerable<GraphicsSettingDescriptor> catalog)
	{
		writer.WriteStartArray(name); foreach (var descriptor in catalog.Where(d => (uint)d.Id is >= 104 and <= 111)) { writer.WriteStartObject(); writer.WriteNumber("id", (uint)descriptor.Id); writer.WriteString("name", descriptor.Name); writer.WriteString("kind", descriptor.ValueKind.ToString()); writer.WriteBoolean("available", descriptor.Available); writer.WriteBoolean("cameraScope", descriptor.CameraScope); writer.WriteString("activation", descriptor.Activation.ToString()); writer.WriteString("detail", descriptor.Detail); writer.WriteEndObject(); } writer.WriteEndArray();
	}
	private void Observe(string status)
	{
		_status = status;
		try {
			using var bytes = new MemoryStream(); using (var writer = new Utf8JsonWriter(bytes, new JsonWriterOptions { Indented = true })) {
				writer.WriteStartObject(); writer.WriteNumber("schemaVersion", 1); writer.WriteString("runId", _run); writer.WriteString("bootId", _boot); writer.WriteString("phase", _phase.ToString()); writer.WriteString("caseId", Cases[_step]); writer.WriteString("requestId", Decimal(_request)); writer.WriteString("status", status); writer.WriteString("failure", _failure); writer.WriteNumber("updates", _updates);
				writer.WriteBoolean("externalNativeAcknowledgementRequired", true); writer.WriteBoolean("allNativeImagesAcknowledged", _acknowledgements.Count == Cases.Length); writer.WriteBoolean("neutralFixedExposureEV0Verified", _neutralBaselineVerified); writer.WriteString("nativeOrchestration", "External fixture owns fixed exposure EV0 for both views, fixed near/far geometry, camera/object motion, real depth/motion inputs and unchanged HUD. No managed reset or motion hook is fabricated.");
				WriteDiscovery(writer, "discoveryA", _discoveryA); WriteDiscovery(writer, "discoveryB", _discoveryB);
				WriteSnapshot(writer, "game", _game); WriteSnapshot(writer, "cameraA", _a); WriteSnapshot(writer, "cameraB", _b); WriteSnapshot(writer, "baselineA", _baselineA); WriteSnapshot(writer, "baselineB", _baselineB);
				writer.WriteStartArray("publications"); foreach (var publication in _publications) { writer.WriteStartObject(); writer.WriteString("caseId", publication.CaseId); writer.WriteString("requestId", Decimal(publication.RequestId)); writer.WritePropertyName("receipt"); WriteReceipt(writer, publication.Receipt); WriteSnapshot(writer, "cameraA", publication.A); WriteSnapshot(writer, "cameraB", publication.B); writer.WriteEndObject(); } writer.WriteEndArray();
				writer.WriteStartArray("receipts"); foreach (var receipt in _receipts) WriteReceipt(writer, receipt); writer.WriteEndArray(); writer.WriteStartArray("nativeAcknowledgements"); foreach (var ack in _acknowledgements) ack.WriteTo(writer); writer.WriteEndArray();
				writer.WriteStartArray("checks"); foreach (string check in _checks) writer.WriteStringValue(check); writer.WriteEndArray(); writer.WriteStartArray("blocked"); foreach (string blocked in _blocked) writer.WriteStringValue(blocked); writer.WriteEndArray(); writer.WriteEndObject();
			}
			string path = Path.Combine(_root, "camera-effects-observation.json"), temporary = path + "." + _boot + ".tmp"; File.WriteAllBytes(temporary, bytes.ToArray()); File.Move(temporary, path, true);
		} catch (IOException) { /* Retry an atomic observation after transient runner sharing. */ }
	}
}
