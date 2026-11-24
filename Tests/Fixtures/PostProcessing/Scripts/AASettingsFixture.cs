using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using TerraEngine;

// Opt-in controller for a disposable generic validation project. The runner
// provides the observation directory and run identity. Requests use the normal
// protected native queue; receipts prove settings publication only. The external
// runner owns image, GPU execution and timing evidence independently.
public sealed partial class AASettingsFixture : Script
{
	private enum Phase { Ready, Modes, InvalidEnum, IncompatibleScale, Cameras, CameraReset, CameraPreferenceOverride, Save, Change, Restore, Done }
	private const GraphicsSettingId Aa = GraphicsSettingId.AntiAliasingMode;
	// These persisted uint values mirror the native spatial capability contract.
	// Use the typed setting ID with the public API until it exposes a mode enum.
	private enum SpatialMode : uint { Off = 0, FXAA = 1, SMAA = 2, CMAA2 = 3 }
	private static readonly uint[] Modes = { (uint)SpatialMode.Off, (uint)SpatialMode.CMAA2,
		(uint)SpatialMode.Off, (uint)SpatialMode.FXAA, (uint)SpatialMode.SMAA };
	private readonly string _boot = Guid.NewGuid().ToString("N");
	private readonly List<string> _checks = new(), _blocked = new(), _notTested = new();
	private readonly List<GraphicsRequestReceipt> _receipts = new();
	// Keep typed copied publications; NativeAOT reporting never asks a
	// reflection serializer to discover anonymous/object payload metadata.
	private sealed record PublicationEvidence(ulong RequestId, string Phase, GraphicsSettingsSnapshot Effective,
		ulong ReceiptEffectiveRevision, ulong ReceiptAppliedFrameId);
	private readonly List<PublicationEvidence> _publications = new();
	private readonly HashSet<ulong> _publicationIds = new();
	private string _root = string.Empty, _run = string.Empty, _failure = string.Empty, _lastStatus = "pending";
	private GraphicsSettingDescriptor? _descriptor, _cameraDescriptorA, _cameraDescriptorB;
	private GraphicsSettingsSnapshot? _session, _before, _game, _saved, _cameraBeforeB, _currentCameraA, _currentCameraB, _preferenceCameraA, _preferenceCameraB;
	private GraphicsSettingsTarget _cameraA, _cameraB;
	private Phase _phase;
	private long _started;
	private ulong _request, _updates, _preferenceSaveRequestId, _preferenceRestoreRequestId;
	private int _modeIndex, _stable;
	private bool _configured, _finished, _observationPending, _expectedSupported, _scaleEligible, _unavailableChecked;
	private string PreferenceName => "aa-fixture-" + _run;

	public override void OnStart()
	{
		string? root = Environment.GetEnvironmentVariable("TERRAENGINE_AA_FIXTURE_ROOT");
		string? run = Environment.GetEnvironmentVariable("TERRAENGINE_AA_FIXTURE_RUN_ID");
		if (string.IsNullOrWhiteSpace(root) && string.IsNullOrWhiteSpace(run)) return;
		try {
			Require(!string.IsNullOrWhiteSpace(root) && !string.IsNullOrWhiteSpace(run), "Missing AA fixture environment inputs.");
			_root = Path.GetFullPath(root!); _run = run!;
			Require(Directory.Exists(_root), "Runner must create the observation directory.");
			Require(_run.All(c => char.IsAsciiLetterOrDigit(c) || c == '-'), "Invalid run identity.");
			_configured = true; SetPhase(Phase.Ready);
		} catch (Exception error) { Fail(error); }
	}

	public override void OnUpdate()
	{
		if (!_configured) return;
		if (_finished) {
			// A completed report is immutable. Only an unsettled atomic write
			// needs another update while the reader waits for its publication.
			if (_observationPending) Observe(_lastStatus);
			return;
		}
		try {
			++_updates;
			Require(Stopwatch.GetElapsedTime(_started).TotalSeconds < 60, "Native AA fixture phase timed out: " + _phase);
			Advance();
		} catch (Exception error) { Fail(error); }
	}

	private void Advance()
	{
		switch (_phase) {
		case Phase.Ready:
			_game = Published(); if (_game is null) return;
			// Play-entry can publish a new session after the first callback. Three
			// stable observations avoid treating a known previous session as ready.
			if (_session is null || !SameSession(_session, _game)) { _session = _game; _stable = 0; return; }
			if (++_stable < 3) return;
			_descriptor = Graphics.DiscoverSettings().SingleOrDefault(d => d.Id == Aa);
			Require(_descriptor is not null && _descriptor.ValueKind == GraphicsValueKind.Unsigned32 && _descriptor.GameScope,
				"AA ID64 discovery is missing or has the wrong kind/scope.");
			Require((uint)Aa == 64U && _descriptor!.Activation is GraphicsActivation.Live or GraphicsActivation.Rebuild,
				"Spatial AA must retain ID64 and apply through a same-session live/rebuild boundary.");
			Require(Supported(0), "Off must remain available.");
			SetPhase(Phase.Modes); break;
		case Phase.Modes:
			if (_modeIndex == Modes.Length) {
				if (!_unavailableChecked) _notTested.Add("Known-unavailable rejection needs a real device/path capability case with one declared spatial mode unavailable.");
				SetPhase(Phase.InvalidEnum); return;
			}
			uint mode = Modes[_modeIndex];
			if (_request == 0) {
				_before = Published(); if (_before is null) return;
				_expectedSupported = Supported(mode);
				Begin(Graphics.ApplySettings(new[] { new GraphicsSettingValue(Aa, mode) }, expectedRevision: _before.Revision)); return;
			}
			if (Terminal() is not { } receipt) return;
			if (_expectedSupported) {
				if (!Applied(default)) return;
				Require(Mode(_game!) == mode, "The active mode differs from the accepted AA request.");
				if (mode != (uint)SpatialMode.Off) RequireNativeSpatialExtent(_game!);
				_checks.Add("native-mode-" + mode + "-published");
			} else {
				Require(receipt.State == GraphicsRequestState.Rejected && receipt.Reason == GraphicsRequestReason.Unsupported,
					"A known unavailable AA mode must be Unsupported, not an invalid enum or fake fallback.");
				_game = Published(); if (_game is null) return;
				Unchanged(_before!, _game);
				_checks.Add("known-unavailable-" + mode + "-atomic-rejection");
				_unavailableChecked = true;
				_blocked.Add("Required mode unavailable: " + mode);
			}
			++_modeIndex; _request = 0; SetPhase(Phase.Modes); break;
		case Phase.InvalidEnum:
			if (_request == 0) {
				_before = Published(); if (_before is null) return;
				Begin(Graphics.ApplySettings(new[] { new GraphicsSettingValue(Aa, 999U) }, expectedRevision: _before.Revision)); return;
			}
			if (Terminal() is not { } invalid) return;
			Require(invalid.State == GraphicsRequestState.Rejected && invalid.Reason == GraphicsRequestReason.InvalidValue,
				"Unknown AA enum values must be InvalidValue rather than Unsupported.");
			_game = Published(); if (_game is null) return;
			Unchanged(_before!, _game); _checks.Add("invalid-enum-atomic-rejection");
			_request = 0; SetPhase(Phase.IncompatibleScale); break;
		case Phase.IncompatibleScale:
			if (_request == 0) {
				var scale = Graphics.DiscoverSettings().SingleOrDefault(d => d.Id == GraphicsSettingId.InternalResolutionScale);
				_scaleEligible = scale is { Available: true, GameScope: true } && ScalarOption(scale, 0.75F) && Supported(1);
				if (!_scaleEligible) { _notTested.Add("Incompatible scale needs independently available 0.75 scale and FXAA."); SetPhase(Phase.Cameras); return; }
				_before = Published(); if (_before is null) return;
				Begin(Graphics.ApplySettings(new[] { new GraphicsSettingValue(Aa, 1U), new GraphicsSettingValue(GraphicsSettingId.InternalResolutionScale, 0.75F) },
					expectedRevision: _before.Revision)); return;
			}
			if (Terminal() is not { } incompatible) return;
			Require(incompatible.State == GraphicsRequestState.Rejected && incompatible.Reason is GraphicsRequestReason.Unsupported or GraphicsRequestReason.InvalidValue,
				"A native-only spatial AA / lower-scale combination must reject with a typed compatibility reason.");
			Require(!string.IsNullOrWhiteSpace(incompatible.Detail), "Incompatible-scale rejection needs an explanatory diagnostic.");
			_game = Published(); if (_game is null) return;
			Unchanged(_before!, _game); _checks.Add("incompatible-scale-atomic-rejection-" + incompatible.Reason);
			_request = 0; SetPhase(Phase.Cameras); break;
		case Phase.Cameras:
			if (_request == 0) {
				Camera? a = TerraEngine.Object.Find("CameraA")?.GetComponent<Camera>();
				Camera? b = TerraEngine.Object.Find("CameraB")?.GetComponent<Camera>();
				if (_descriptor is not { CameraScope: true } || a is null || b is null) {
					_notTested.Add("Independent camera AA: two eligible named camera components are required."); SetPhase(Phase.Save); return;
				}
				_cameraA = Graphics.ForCamera(a); _cameraB = Graphics.ForCamera(b);
				if (!_cameraA.IsValid || !_cameraB.IsValid) return;
				// Copy discovery from the actual scoped public calls. It is metadata,
				// not proof that an advertised shader executed on either camera.
				_cameraDescriptorA = Graphics.DiscoverSettings(_cameraA).SingleOrDefault(d => d.Id == Aa);
				_cameraDescriptorB = Graphics.DiscoverSettings(_cameraB).SingleOrDefault(d => d.Id == Aa);
				Require(_cameraDescriptorA is { Available: true, CameraScope: true, ValueKind: GraphicsValueKind.Unsigned32 }
					&& _cameraDescriptorB is { Available: true, CameraScope: true, ValueKind: GraphicsValueKind.Unsigned32 },
					"Camera-scoped AA discovery is missing or unavailable.");
				var beforeA = Published(_cameraA); _cameraBeforeB = Published(_cameraB);
				if (beforeA is null || _cameraBeforeB is null) return;
				Require(_cameraA.CameraId != _cameraB.CameraId, "Camera targets must be distinct.");
				// Select an available value different from B, so isolation cannot
				// pass merely because both cameras coincidentally already use it.
				uint? isolated = new uint[] { 0, 1, 2, 3 }.Where(v => Supported(v) && Supported(_cameraDescriptorA, v)).Select(v => (uint?)v).FirstOrDefault(v => v != Mode(_cameraBeforeB));
				if (isolated is null) { _notTested.Add("Camera isolation needs two supported AA values."); SetPhase(Phase.Save); return; }
				BeforeCameraMode = isolated.Value;
				Begin(Graphics.ApplySettings(new[] { new GraphicsSettingValue(Aa, isolated.Value) }, _cameraA, beforeA.Revision)); return;
			}
			if (!Applied(_cameraA)) return;
			var currentA = Published(_cameraA); var currentB = Published(_cameraB);
			if (currentA is null || currentB is null) return;
			_currentCameraA = currentA; _currentCameraB = currentB;
			Require(Mode(currentA) == BeforeCameraMode, "Camera A AA override did not publish.");
			if (BeforeCameraMode != (uint)SpatialMode.Off) RequireNativeSpatialExtent(currentA);
			SameValues(_cameraBeforeB!, currentB); _checks.Add("camera-a-aa-isolated-from-b");
			Begin(Graphics.ResetSettings(new[] { Aa }, _cameraA, currentA.Revision)); SetPhase(Phase.CameraReset); break;
		case Phase.CameraReset:
			if (!Applied(_cameraA)) return;
			var resetA = Published(_cameraA); var retainedB = Published(_cameraB); _game = Published();
			if (resetA is null || retainedB is null || _game is null) return;
			_currentCameraA = resetA; _currentCameraB = retainedB;
			Require(Mode(resetA) == Mode(_game!), "Reset AA must restore game inheritance on camera A.");
			SameValues(_cameraBeforeB!, retainedB); _checks.Add("camera-aa-reset-inherits-game");
			_request = 0; SetPhase(Phase.CameraPreferenceOverride); break;
		case Phase.CameraPreferenceOverride:
			if (_request == 0) {
				var inheritedA = Published(_cameraA); _game = Published();
				if (inheritedA is null || _game is null) return;
				uint? retained = new uint[] { 0, 1, 2, 3 }.Where(v => Supported(v) && Supported(_cameraDescriptorA, v))
					.Select(v => (uint?)v).FirstOrDefault(v => v != Mode(_game!));
				if (retained is not { } retainedMode) throw new InvalidOperationException("Preference isolation needs an advertised camera mode different from game inheritance.");
				BeforeCameraMode = retainedMode;
				Begin(Graphics.ApplySettings(new[] { new GraphicsSettingValue(Aa, retainedMode) }, _cameraA, inheritedA.Revision)); return;
			}
			if (!Applied(_cameraA)) return;
			_currentCameraA = Published(_cameraA); _currentCameraB = Published(_cameraB); _game = Published();
			if (_currentCameraA is null || _currentCameraB is null || _game is null) return;
			Require(Mode(_currentCameraA) == BeforeCameraMode && Mode(_currentCameraA) != Mode(_game!), "Retained camera override did not publish independently of game AA.");
			SameValues(_cameraBeforeB!, _currentCameraB);
			_preferenceCameraA = _currentCameraA; _preferenceCameraB = _currentCameraB;
			_checks.Add("camera-aa-override-held-for-preferences");
			_request = 0; SetPhase(Phase.Save); break;
		case Phase.Save:
			if (_request == 0) {
				_saved = Published(); if (_saved is null) return;
				var save = Graphics.SavePreferences(PreferenceName); _preferenceSaveRequestId = save.RequestId; Begin(save); return;
			}
			if (Terminal() is not { } saved) return;
			Require(saved.State == GraphicsRequestState.Applied, "Explicit preference save did not become durable: " + saved.Reason);
			_checks.Add("explicit-aa-preference-saved"); _request = 0; SetPhase(Phase.Change); break;
		case Phase.Change:
			if (_request == 0) {
				Require(_saved is not null, "Missing saved preference snapshot.");
				uint? alternate = new uint[] { 0, 1, 2, 3 }.Where(Supported).Select(v => (uint?)v).FirstOrDefault(v => v != Mode(_saved!));
				if (alternate is null) { _blocked.Add("Preference roundtrip needs a supported alternate mode."); SetPhase(Phase.Done); return; }
				Begin(Graphics.ApplySettings(new[] { new GraphicsSettingValue(Aa, alternate.Value) })); return;
			}
			if (!Applied(default)) return;
			Require(Mode(_game!) != Mode(_saved!), "Preference test did not change active AA before restore.");
			var restore = Graphics.RestorePreferences(PreferenceName); _preferenceRestoreRequestId = restore.RequestId;
			Begin(restore); SetPhase(Phase.Restore); break;
		case Phase.Restore:
			if (!Applied(default)) return;
			SameValues(_saved!, _game!);
			if (_preferenceCameraA is not null && _preferenceCameraB is not null) {
				// Read the actual current publications after restore. Pre-roundtrip
				// snapshots alone cannot prove that a camera override survived.
				var finalA = Published(_cameraA); var finalB = Published(_cameraB);
				if (finalA is null || finalB is null) return;
				_currentCameraA = finalA; _currentCameraB = finalB;
				Require(Mode(finalA) == Mode(_preferenceCameraA) && Mode(finalA) != Mode(_game!),
					"Preference restore cleared or changed the retained camera AA override.");
				Require(Mode(finalB) == Mode(_game!), "Camera B did not retain game inheritance after preference restore.");
				SameValues(_preferenceCameraA, finalA); SameValues(_preferenceCameraB, finalB);
				if (Mode(finalA) != (uint)SpatialMode.Off) RequireNativeSpatialExtent(finalA);
				if (Mode(finalB) != (uint)SpatialMode.Off) RequireNativeSpatialExtent(finalB);
				_checks.Add("camera-aa-override-survives-preference-restore");
			}
			_checks.Add("explicit-aa-preference-restored"); SetPhase(Phase.Done); break;
		case Phase.Done:
			_finished = true; Observe(_blocked.Count == 0 ? "passed" : "blocked"); break;
		}
	}

	private uint BeforeCameraMode { get; set; }
	private bool Supported(uint mode) => Supported(_descriptor, mode);
	private static bool Supported(GraphicsSettingDescriptor? descriptor, uint mode) => descriptor is { Available: true } && descriptor.Options.Any(v => Convert.ToUInt32(v, CultureInfo.InvariantCulture) == mode);
	private static bool ScalarOption(GraphicsSettingDescriptor d, float value) =>
		(d.Minimum is null || Convert.ToSingle(d.Minimum, CultureInfo.InvariantCulture) <= value)
		&& (d.Maximum is null || Convert.ToSingle(d.Maximum, CultureInfo.InvariantCulture) >= value)
		&& (d.Options.Count == 0 || d.Options.Any(v => Convert.ToSingle(v, CultureInfo.InvariantCulture) == value));
	private static uint Mode(GraphicsSettingsSnapshot s) => Convert.ToUInt32(s.Values.Single(v => v.Id == Aa).Value, CultureInfo.InvariantCulture);
	private static bool SameSession(GraphicsSettingsSnapshot a, GraphicsSettingsSnapshot b) => a.Lifecycle.ProjectGeneration == b.Lifecycle.ProjectGeneration
		&& a.Lifecycle.PlaySessionGeneration == b.Lifecycle.PlaySessionGeneration && a.Lifecycle.RendererSessionGeneration == b.Lifecycle.RendererSessionGeneration
		&& a.Lifecycle.BackendResourceGeneration == b.Lifecycle.BackendResourceGeneration;
	private GraphicsSettingsSnapshot? Published(GraphicsSettingsTarget target = default)
	{
		var snapshot = Graphics.ReadSettings(target).Snapshot;
		if (snapshot is not { Revision: > 0, PublishedFrameId: > 0 }) return null;
		if (_phase != Phase.Ready) Require(_session is not null && SameSession(_session, snapshot), "Renderer/project/Play session changed within the AA test.");
		Require(snapshot.Target.IsCamera == target.IsCamera, "Native settings read returned another scope.");
		if (target.IsCamera) {
			Require(snapshot.Target.CameraId == target.CameraId && snapshot.Target.SceneGeneration == target.SceneGeneration
				&& snapshot.Target.ObjectLifetimeGeneration == target.ObjectLifetimeGeneration
				&& snapshot.Target.ComponentGeneration == target.ComponentGeneration && snapshot.Target.BindingGeneration == target.BindingGeneration,
				"Native settings read returned another camera component lifetime.");
			if (target.HasView) Require(snapshot.Target.HasView && snapshot.Target.ViewType == target.ViewType
				&& snapshot.Target.OutputKind == target.OutputKind && snapshot.Target.OutputId == target.OutputId
				&& snapshot.Target.ViewLifetimeGeneration == target.ViewLifetimeGeneration,
				"Native settings read returned another concrete camera view.");
		}
		return snapshot;
	}
	private static void RequireNativeSpatialExtent(GraphicsSettingsSnapshot snapshot)
	{
		// These are resolved native metadata checks, not evidence that the shader
		// executed or produced an acceptable image. Those proofs are separate.
		Require(snapshot.SampleCount == 1U && snapshot.InternalWidth > 0U && snapshot.InternalHeight > 0U
			&& snapshot.InternalWidth == snapshot.OutputWidth && snapshot.InternalHeight == snapshot.OutputHeight,
			"The native spatial scope must resolve scale1 and sample1 with real equal extents.");
	}
	private void Begin(GraphicsRequestReceipt receipt)
	{
		_request = receipt.RequestId; Remember(receipt);
		Require(_request != 0, "Native request admission returned no ID: " + receipt.Reason);
		_started = Stopwatch.GetTimestamp();
	}
	private GraphicsRequestReceipt? Terminal()
	{
		var receipt = Graphics.ReadRequest(_request); if (receipt is null) return null;
		Require(receipt.RequestId == _request, "Receipt lookup returned another request.");
		Remember(receipt);
		Require(receipt.State is not (GraphicsRequestState.RestartRequired or GraphicsRequestState.ScheduledRestart),
			"Spatial AA and explicit preferences must complete in this session without a restart.");
		return receipt.State is GraphicsRequestState.Applied or GraphicsRequestState.Rejected or GraphicsRequestState.Cancelled ? receipt : null;
	}
	private bool Applied(GraphicsSettingsTarget target)
	{
		var receipt = Terminal(); if (receipt is null) return false;
		Require(receipt.State == GraphicsRequestState.Applied, "Expected native AA publication, received " + receipt.State + ": " + receipt.Reason);
		var snapshot = Published(target); if (snapshot is null) return false;
		if (!target.IsCamera) _game = snapshot;
		bool visible = receipt.EffectiveRevision > 0 && receipt.AppliedFrameId > 0
			&& snapshot.Revision >= receipt.EffectiveRevision && snapshot.PublishedFrameId >= receipt.AppliedFrameId;
		if (visible && _publicationIds.Add(receipt.RequestId)) _publications.Add(new PublicationEvidence(
			receipt.RequestId, _phase.ToString(), snapshot, receipt.EffectiveRevision, receipt.AppliedFrameId));
		return visible;
	}
	private void Remember(GraphicsRequestReceipt r)
	{
		if (!_receipts.Any(v => v.RequestId == r.RequestId && v.State == r.State && v.EffectiveRevision == r.EffectiveRevision && v.AppliedFrameId == r.AppliedFrameId)) _receipts.Add(r);
	}
	private static void SameValues(GraphicsSettingsSnapshot a, GraphicsSettingsSnapshot b)
	{
		Require(a.Values.Count == b.Values.Count && a.Values.All(v => b.Values.Any(w => w.Id == v.Id && Equals(w.Value, v.Value))), "Native effective settings changed unexpectedly.");
	}
	private static void Unchanged(GraphicsSettingsSnapshot a, GraphicsSettingsSnapshot b) { SameValues(a, b); Require(a.Revision == b.Revision, "Rejected request changed the effective revision."); }
	private void SetPhase(Phase phase) { _phase = phase; _started = Stopwatch.GetTimestamp(); }
	private void Fail(Exception error) { _failure = error.ToString(); _finished = true; if (Directory.Exists(_root)) Observe("failed"); }
	private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
	private static string Decimal(ulong value) => value.ToString(CultureInfo.InvariantCulture);
	private static void WriteExtent(Utf8JsonWriter writer, string name, uint width, uint height)
	{
		writer.WriteStartArray(name); writer.WriteNumberValue(width); writer.WriteNumberValue(height); writer.WriteEndArray();
	}
	private static void WriteSnapshot(Utf8JsonWriter writer, string name, GraphicsSettingsSnapshot? snapshot)
	{
		if (snapshot is null) { writer.WriteNull(name); return; }
		writer.WriteStartObject(name);
		if (snapshot.Values.Any(v => v.Id == Aa)) writer.WriteNumber("mode", Mode(snapshot)); else writer.WriteNull("mode");
		writer.WriteString("revision", Decimal(snapshot.Revision)); writer.WriteString("frameId", Decimal(snapshot.PublishedFrameId));
		writer.WriteString("cameraId", snapshot.Target.CameraId); writer.WriteString("viewLifetime", Decimal(snapshot.Target.ViewLifetimeGeneration));
		writer.WriteString("project", Decimal(snapshot.Lifecycle.ProjectGeneration)); writer.WriteString("play", Decimal(snapshot.Lifecycle.PlaySessionGeneration));
		writer.WriteString("renderer", Decimal(snapshot.Lifecycle.RendererSessionGeneration)); writer.WriteString("backend", Decimal(snapshot.Lifecycle.BackendResourceGeneration));
		WriteExtent(writer, "internalExtent", snapshot.InternalWidth, snapshot.InternalHeight);
		WriteExtent(writer, "outputExtent", snapshot.OutputWidth, snapshot.OutputHeight);
		writer.WriteNumber("sampleCount", snapshot.SampleCount); writer.WriteString("postProcessResourceGeneration", Decimal(snapshot.PostProcessResourceGeneration));
		writer.WriteEndObject();
	}
	private static void WriteDiscovery(Utf8JsonWriter writer, string name, GraphicsSettingDescriptor? descriptor)
	{
		if (descriptor is null) { writer.WriteNull(name); return; }
		writer.WriteStartObject(name);
		writer.WriteNumber("id", (uint)descriptor.Id); writer.WriteString("name", descriptor.Name); writer.WriteString("kind", descriptor.ValueKind.ToString());
		writer.WriteBoolean("available", descriptor.Available); writer.WriteBoolean("gameScope", descriptor.GameScope); writer.WriteBoolean("cameraScope", descriptor.CameraScope);
		writer.WriteString("activation", descriptor.Activation.ToString());
		writer.WriteStartArray("options");
		foreach (object option in descriptor.Options) writer.WriteNumberValue(Convert.ToUInt32(option, CultureInfo.InvariantCulture));
		writer.WriteEndArray();
		writer.WriteString("unavailableReason", descriptor.UnavailableReason.ToString()); writer.WriteString("detail", descriptor.Detail);
		writer.WriteEndObject();
	}
	private static void WriteStrings(Utf8JsonWriter writer, string name, IEnumerable<string> values)
	{
		writer.WriteStartArray(name); foreach (string value in values) writer.WriteStringValue(value); writer.WriteEndArray();
	}
	private void Observe(string status)
	{
		_lastStatus = status;
		_observationPending = true;
		try {
			// Every field is written explicitly so the installed NativeAOT export
			// requires no reflection metadata or reflection-enabled JSON setting.
			using var bytes = new MemoryStream();
			using (var writer = new Utf8JsonWriter(bytes, new JsonWriterOptions { Indented = true })) {
				writer.WriteStartObject(); writer.WriteNumber("schemaVersion", 1);
				writer.WriteString("runId", _run); writer.WriteString("bootId", _boot); writer.WriteString("phase", _phase.ToString()); writer.WriteString("status", status);
				writer.WriteString("failure", _failure); writer.WriteBoolean("settingsOnly", true); writer.WriteBoolean("imageryVerified", false); writer.WriteBoolean("gpuExecutionVerified", false);
				writer.WriteNumber("updates", _updates); writer.WriteString("requestId", Decimal(_request));
				WriteStrings(writer, "checks", _checks); WriteStrings(writer, "blocked", _blocked); WriteStrings(writer, "notTested", _notTested);
				writer.WriteStartArray("publications");
				foreach (PublicationEvidence publication in _publications) {
					writer.WriteStartObject(); writer.WriteString("requestId", Decimal(publication.RequestId)); writer.WriteString("phase", publication.Phase);
					WriteSnapshot(writer, "effective", publication.Effective);
					writer.WriteString("receiptEffectiveRevision", Decimal(publication.ReceiptEffectiveRevision)); writer.WriteString("receiptAppliedFrameId", Decimal(publication.ReceiptAppliedFrameId));
					writer.WriteEndObject();
				}
				writer.WriteEndArray();
				WriteSnapshot(writer, "effective", _game); WriteSnapshot(writer, "cameraA", _currentCameraA); WriteSnapshot(writer, "cameraB", _currentCameraB);
				writer.WriteStartObject("discovery"); WriteDiscovery(writer, "game", _descriptor); WriteDiscovery(writer, "cameraA", _cameraDescriptorA); WriteDiscovery(writer, "cameraB", _cameraDescriptorB); writer.WriteEndObject();
				writer.WriteString("preferenceSaveRequestId", Decimal(_preferenceSaveRequestId)); writer.WriteString("preferenceRestoreRequestId", Decimal(_preferenceRestoreRequestId));
				if (_saved is null) writer.WriteNull("savedMode"); else writer.WriteNumber("savedMode", Mode(_saved));
				WriteSnapshot(writer, "preferenceCameraBeforeA", _preferenceCameraA); WriteSnapshot(writer, "preferenceCameraBeforeB", _preferenceCameraB);
				writer.WriteStartArray("receipts");
				foreach (GraphicsRequestReceipt receipt in _receipts) {
					writer.WriteStartObject(); writer.WriteString("requestId", Decimal(receipt.RequestId)); writer.WriteString("state", receipt.State.ToString()); writer.WriteString("reason", receipt.Reason.ToString()); writer.WriteString("detail", receipt.Detail);
					writer.WriteString("requestedRevision", Decimal(receipt.RequestedRevision)); writer.WriteString("effectiveRevision", Decimal(receipt.EffectiveRevision)); writer.WriteString("appliedFrameId", Decimal(receipt.AppliedFrameId));
					writer.WriteEndObject();
				}
				writer.WriteEndArray(); writer.WriteEndObject();
			}
			string file = Path.Combine(_root, "aa-settings-observation.json"), temporary = file + "." + _boot + ".tmp";
			// The runner supplies a fresh directory per app. Publish the terminal
			// report exactly once so Windows never has to replace an open file.
			File.WriteAllBytes(temporary, bytes.ToArray()); File.Move(temporary, file);
			_observationPending = false;
		} catch (IOException) { /* Retry a transient write failure on the next update. */ }
	}
}
