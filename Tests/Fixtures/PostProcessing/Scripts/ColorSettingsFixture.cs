using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using TerraEngine;

// Stage this controller instead of GraphicsSettingsFixture in the disposable
// color project. Native receipts/snapshots prove settings behavior; the external
// runner owns actual matched image capture and scoring at each checkpoint.
public sealed partial class ColorSettingsFixture : Script
{
	private enum Phase { Ready, AppliedA, AppliedB, ResetA, InvalidBatch, ControlBaseline, ControlApplied, AssetKnown, AssetInvalid, AssetZero, AssetReset, Capture, Finished }
	private readonly string _bootId = Guid.NewGuid().ToString("N");
	private readonly List<string> _checks = new();
	private readonly List<GraphicsRequestReceipt> _receipts = new();
	private string _root = string.Empty, _runId = string.Empty, _failure = string.Empty;
	private string _captureTag = string.Empty, _pendingStatus = string.Empty;
	private GraphicsSettingsTarget _targetA, _targetB;
	private GraphicsSettingsSnapshot? _a, _b, _baselineA, _baselineB, _beforeInvalid, _isolatedA, _isolatedB;
	private sealed record ControlCase(string Tag, GraphicsSettingValue[] Values);
	private readonly List<ControlCase> _controls = new();
	private GraphicsSettingValue[] _controlBaseline = Array.Empty<GraphicsSettingValue>();
	private int _controlIndex, _assetIndex;
	private PhysicsUuid _curveId, _lutId, _missingAssetId;
	private bool _assetInputsConfigured;
	private GraphicsSettingsSnapshot? _beforeAssetInvalid;
	private GraphicsSettingValue[] _assetResetExpected = Array.Empty<GraphicsSettingValue>();
	private static readonly GraphicsSettingId[] AssetSettings =
	{
		GraphicsSettingId.ColorCurve, GraphicsSettingId.ColorCurveStrength,
		GraphicsSettingId.ColorLut, GraphicsSettingId.ColorLutStrength, GraphicsSettingId.ColorGradingEnabled
	};
	private Phase _phase, _afterCapture;
	private ulong _requestId, _updates;
	private long _phaseStarted;
	private bool _configured, _finished;

	public override void OnStart()
	{
		string? root = Environment.GetEnvironmentVariable("TERRAENGINE_COLOR_FIXTURE_ROOT");
		string? run = Environment.GetEnvironmentVariable("TERRAENGINE_COLOR_FIXTURE_RUN_ID");
		if (string.IsNullOrWhiteSpace(root) && string.IsNullOrWhiteSpace(run)) return;
		try
		{
			Require(!string.IsNullOrWhiteSpace(root) && !string.IsNullOrWhiteSpace(run), "Missing color fixture inputs.");
			_root = Path.GetFullPath(root!); _runId = run!;
			Require(Directory.Exists(_root), "Runner must create the observation directory.");
			Require(_runId.All(c => char.IsAsciiLetterOrDigit(c) || c == '-'), "Invalid run identity.");
			_configured = true;
			SetPhase(Phase.Ready);
		}
		catch (Exception error) { Fail(error); }
	}

	public override void OnUpdate()
	{
		if (!_configured) return;
		if (_finished) { if (_pendingStatus.Length != 0) Observe(_pendingStatus); return; }
		++_updates;
		try
		{
			Require(Stopwatch.GetElapsedTime(_phaseStarted).TotalSeconds < 90, "Color fixture phase timed out.");
			Advance();
			if (!_finished && _updates % 30 == 0) Observe("pending");
		}
		catch (Exception error) { Fail(error); }
	}

	private void Advance()
	{
		switch (_phase)
		{
			case Phase.Ready:
				Camera? ca = TerraEngine.Object.Find("CameraA")?.GetComponent<Camera>();
				Camera? cb = TerraEngine.Object.Find("CameraB")?.GetComponent<Camera>();
				if (ca is null || cb is null) return;
				_targetA = Graphics.ForCamera(ca); _targetB = Graphics.ForCamera(cb);
				if (!Refresh()) return;
				Require(_a!.Target.CameraId != _b!.Target.CameraId, "Cameras must have separate native identities.");
				_baselineA = _a; _baselineB = _b;
				var descriptor = Graphics.DiscoverSettings(_targetA).Single(d => d.Id == GraphicsSettingId.FixedExposureCompensation);
				// Enabling a nonzero legacy compensation can require a new native
				// target, but a protected rebuild still applies during this session.
				Require(descriptor.Available && descriptor.CameraScope && descriptor.Activation == GraphicsActivation.Rebuild,
					"Fixed exposure must be a supported camera setting with native preparation.");
				Capture("baseline", Phase.AppliedA);
				break;
			case Phase.AppliedA:
				if (_requestId == 0) { Submit(1.0F, _targetA); return; }
				if (!Applied(_targetA)) return;
				Require(Float(_a!, GraphicsSettingId.FixedExposureCompensation) == 1.0F, "CameraA exposure did not apply.");
				SameValues(_baselineB!, _b!);
				_isolatedA = _a;
				_checks.Add("camera-a-plus-one-isolated");
				Capture("camera-a-plus-one", Phase.AppliedB);
				break;
			case Phase.AppliedB:
				if (_requestId == 0) { Submit(-1.0F, _targetB); return; }
				if (!Applied(_targetB)) return;
				SameValues(_isolatedA!, _a!);
				Require(Float(_b!, GraphicsSettingId.FixedExposureCompensation) == -1.0F, "CameraB exposure did not apply.");
				_isolatedB = _b;
				_checks.Add("camera-b-minus-one-isolated");
				Capture("camera-b-minus-one", Phase.ResetA);
				break;
			case Phase.ResetA:
				if (_requestId == 0) { Begin(Graphics.ResetSettings(new[] { GraphicsSettingId.FixedExposureCompensation }, _targetA)); return; }
				if (!Applied(_targetA)) return;
				Require(Float(_a!, GraphicsSettingId.FixedExposureCompensation) == Float(_baselineA!, GraphicsSettingId.FixedExposureCompensation), "Reset must inherit CameraA's recorded underlying value.");
				SameValues(_isolatedB!, _b!);
				_checks.Add("camera-a-reset-inherits");
				Capture("camera-a-reset", Phase.InvalidBatch);
				break;
			case Phase.InvalidBatch:
				if (_requestId == 0)
				{
					if (!Refresh()) return;
					_beforeInvalid = _a;
					var range = Graphics.DiscoverSettings(_targetA).Single(d => d.Id == GraphicsSettingId.FixedExposureCompensation);
					Require(range.Maximum is float maximum && float.IsFinite(maximum), "Exposure needs a finite discovered maximum.");
					float outside = (float)range.Maximum! + 1.0F;
					Require(float.IsFinite(outside) && outside > (float)range.Maximum!, "No finite invalid exposure can be generated.");
					// A valid mode change accompanies the invalid scalar. The whole
					// batch must reject without switching A to automatic exposure.
					Begin(Graphics.ApplySettings(new[] { new GraphicsSettingValue(GraphicsSettingId.ExposureMode, 2U), new GraphicsSettingValue(GraphicsSettingId.FixedExposureCompensation, outside) }, _targetA));
					return;
				}
				var rejected = Terminal(); if (rejected is null) return;
				Require(rejected.State == GraphicsRequestState.Rejected && rejected.Reason == GraphicsRequestReason.InvalidValue, "Finite invalid mixed batch needs InvalidValue rejection.");
				if (!Refresh()) return;
				SameValues(_beforeInvalid!, _a!);
				SameValues(_isolatedB!, _b!);
				_checks.Add("finite-invalid-mixed-batch-rollback");
				PrepareControls();
				Capture("invalid-batch-preserved", Phase.ControlBaseline);
				break;
			case Phase.ControlBaseline:
				if (_requestId == 0) { Begin(Graphics.ApplySettings(_controlBaseline, _targetA)); return; }
				if (!Applied(_targetA)) return;
				RequireValues(_a!, _controlBaseline); SameValues(_isolatedB!, _b!);
				if (_controlIndex == _controls.Count) { Capture("controls-restored", Phase.AssetKnown); return; }
				Capture(_controls[_controlIndex].Tag + "-baseline", Phase.ControlApplied);
				break;
			case Phase.ControlApplied:
				ControlCase current = _controls[_controlIndex];
				if (_requestId == 0) { Begin(Graphics.ApplySettings(current.Values, _targetA)); return; }
				if (!Applied(_targetA)) return;
				RequireValues(_a!, current.Values); SameValues(_isolatedB!, _b!);
				_checks.Add(current.Tag + "-published-isolated");
				++_controlIndex;
				Capture(current.Tag, Phase.ControlBaseline);
				break;
			case Phase.AssetKnown:
				if (!_assetInputsConfigured) PrepareAssets();
				if (_assetIndex == 2) { SetPhase(Phase.Finished); return; }
				if (_requestId == 0) { Begin(Graphics.ApplySettings(AssetValues(1.0F), _targetA)); return; }
				if (!Applied(_targetA)) return;
				RequireValues(_a!, AssetValues(1.0F)); SameValues(_isolatedB!, _b!);
				_checks.Add(AssetTag + "-known-published-isolated");
				Capture(AssetTag + "-known", Phase.AssetInvalid);
				break;
			case Phase.AssetInvalid:
				if (_requestId == 0)
				{
					if (!Refresh()) return;
					_beforeAssetInvalid = _a;
					// This UUID is generated only for the negative case and is never
					// imported. A valid companion contrast change proves atomic rollback.
					do { _missingAssetId = PhysicsUuid.New(); }
					while (!_missingAssetId.IsValid || _missingAssetId.Equals(_curveId) || _missingAssetId.Equals(_lutId));
					Begin(Graphics.ApplySettings(new[]
					{
						new GraphicsSettingValue(SelectedAsset, _missingAssetId),
						new GraphicsSettingValue(GraphicsSettingId.ColorContrast, 0.75F)
					}, _targetA));
					return;
				}
				var missing = Terminal(); if (missing is null) return;
				Require(missing.State == GraphicsRequestState.Rejected
					&& missing.Reason is GraphicsRequestReason.InvalidValue or GraphicsRequestReason.PreparationFailed
						or GraphicsRequestReason.TargetUnavailable or GraphicsRequestReason.Unsupported,
					"An unregistered grading asset must reject as a resource/value failure, not a stale target or revision.");
				if (!Refresh()) return;
				SameValues(_beforeAssetInvalid!, _a!); SameValues(_isolatedB!, _b!);
				_checks.Add(AssetTag + "-unregistered-mixed-batch-rollback");
				_requestId = 0; SetPhase(Phase.AssetZero);
				break;
			case Phase.AssetZero:
				// Only strength changes here. The known UUID stays bound so native
				// descriptors and lookup ownership must survive a live zero blend.
				if (_requestId == 0) { Begin(Graphics.ApplySettings(new[] { new GraphicsSettingValue(SelectedStrength, 0.0F) }, _targetA)); return; }
				if (!Applied(_targetA)) return;
				RequireValues(_a!, AssetValues(0.0F)); SameValues(_isolatedB!, _b!);
				_checks.Add(AssetTag + "-zero-strength-isolated");
				Capture(AssetTag + "-zero", Phase.AssetReset);
				break;
			case Phase.AssetReset:
				if (_requestId == 0) { Begin(Graphics.ResetSettings(AssetSettings, _targetA)); return; }
				if (!Applied(_targetA)) return;
				RequireValues(_a!, _assetResetExpected); SameValues(_isolatedB!, _b!);
				_checks.Add(AssetTag + "-reset-inherits-isolated");
				++_assetIndex; _requestId = 0; SetPhase(Phase.AssetKnown);
				break;
			case Phase.Capture:
				// Acknowledgement is correlation only, never a claimed image pass.
				// The runner retains captures and evaluates pixels independently.
				// One immutable file per checkpoint allows the host to publish a
				// later acknowledgement while this reader still holds an earlier one.
				string ack = Path.Combine(_root, "color-capture-ack-" + _captureTag + ".json");
				if (!File.Exists(ack)) return;
				JsonDocument acknowledgement;
				try
				{
					// The host publishes the complete file by rename. Poll on a
					// transient Windows sharing race at the publication boundary.
					using var stream = new FileStream(ack, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
					acknowledgement = JsonDocument.Parse(stream);
				}
				catch (IOException) { return; }
				using (acknowledgement)
				{
					var value = acknowledgement.RootElement;
					if (value.GetProperty("runId").GetString() != _runId || value.GetProperty("bootId").GetString() != _bootId
						|| value.GetProperty("tag").GetString() != _captureTag) return;
				}
				_requestId = 0; _captureTag = string.Empty;
				SetPhase(_afterCapture);
				break;
			case Phase.Finished:
				_finished = true; Observe("passed"); break;
		}
	}

	private void PrepareControls()
	{
		// Every case starts from the same complete nonasset control baseline.
		// Enabling a selected effect is part of its patch, while unrelated effects
		// retain the baseline values. Images remain an independent runner result.
		void Add(string tag, params GraphicsSettingValue[] values) => _controls.Add(new(tag, values));
		GraphicsSettingValue U(GraphicsSettingId id, uint value) => new(id, value);
		GraphicsSettingValue F(GraphicsSettingId id, float value) => new(id, value);
		GraphicsSettingValue B(GraphicsSettingId id, bool value) => new(id, value);
		GraphicsSettingValue mode = U(GraphicsSettingId.ExposureMode, 2U);
		GraphicsSettingValue grade = B(GraphicsSettingId.ColorGradingEnabled, true);
		GraphicsSettingValue bloom = B(GraphicsSettingId.BloomEnabled, true);
		Add("exposure-legacy", U(GraphicsSettingId.ExposureMode, 0));
		Add("exposure-fixed", U(GraphicsSettingId.ExposureMode, 1), F(GraphicsSettingId.FixedExposureCompensation, 1));
		Add("exposure-automatic", mode);
		Add("exposure-limits", mode, F(GraphicsSettingId.AutoExposureMinEV, -2), F(GraphicsSettingId.AutoExposureMaxEV, 2));
		Add("exposure-brighten", mode, F(GraphicsSettingId.AutoExposureBrightenSpeed, 3));
		Add("exposure-darken", mode, F(GraphicsSettingId.AutoExposureDarkenSpeed, 1));
		Add("tone-legacy", U(GraphicsSettingId.ToneMappingMode, 0));
		Add("tone-neutral", U(GraphicsSettingId.ToneMappingMode, 1));
		Add("tone-filmic", U(GraphicsSettingId.ToneMappingMode, 2));
		Add("grade-off", B(GraphicsSettingId.ColorGradingEnabled, false));
		Add("grade-identity", grade);
		Add("grade-temperature", grade, F(GraphicsSettingId.ColorTemperatureKelvin, 5000));
		Add("grade-tint", grade, F(GraphicsSettingId.ColorTint, 0.2F));
		Add("grade-contrast", grade, F(GraphicsSettingId.ColorContrast, 1.2F));
		Add("grade-saturation", grade, F(GraphicsSettingId.ColorSaturation, 0.5F));
		Add("grade-lift", grade, F(GraphicsSettingId.ColorLift, 0.05F));
		Add("grade-gain", grade, F(GraphicsSettingId.ColorGain, 1.2F));
		Add("bloom-off", B(GraphicsSettingId.BloomEnabled, false));
		Add("bloom-on", bloom);
		Add("bloom-threshold", bloom, F(GraphicsSettingId.BloomThreshold, 1));
		Add("bloom-zero-intensity", bloom, F(GraphicsSettingId.BloomIntensity, 0));
		Add("bloom-intensity", bloom, F(GraphicsSettingId.BloomIntensity, 0.5F));
		Add("bloom-spread", bloom, F(GraphicsSettingId.BloomSpread, 2));
		Add("bloom-knee", bloom, F(GraphicsSettingId.BloomSoftKnee, 0.5F));
		var descriptors = Graphics.DiscoverSettings(_targetA).ToDictionary(d => d.Id);
		var ids = _controls.SelectMany(c => c.Values).Select(v => v.Id).Distinct().ToArray();
		foreach (var id in ids)
			Require(descriptors.TryGetValue(id, out var d) && d.Available && d.CameraScope && d.Activation == ExpectedActivation(id),
				"Required independent color capability unavailable: " + id);
		_controlBaseline = ids.Select(id => _a!.Values.Single(v => v.Id == id)).ToArray();
		// Identity controls are explicit rather than inherited, so grade/bloom
		// comparisons cannot accidentally start with an authored nonidentity look.
		var baseline = _controlBaseline.ToDictionary(v => v.Id);
		void Set(GraphicsSettingValue value) => baseline[value.Id] = value;
		Set(U(GraphicsSettingId.ExposureMode, 1)); Set(F(GraphicsSettingId.FixedExposureCompensation, 0));
		Set(U(GraphicsSettingId.ToneMappingMode, 1)); Set(B(GraphicsSettingId.ColorGradingEnabled, false));
		Set(F(GraphicsSettingId.ColorTemperatureKelvin, 6500)); Set(F(GraphicsSettingId.ColorTint, 0));
		Set(F(GraphicsSettingId.ColorContrast, 1)); Set(F(GraphicsSettingId.ColorSaturation, 1));
		Set(F(GraphicsSettingId.ColorLift, 0)); Set(F(GraphicsSettingId.ColorGain, 1));
		Set(B(GraphicsSettingId.BloomEnabled, false)); Set(F(GraphicsSettingId.BloomThreshold, 1));
		Set(F(GraphicsSettingId.BloomIntensity, 0.5F)); Set(F(GraphicsSettingId.BloomSpread, 1)); Set(F(GraphicsSettingId.BloomSoftKnee, 0.5F));
		_controlBaseline = baseline.Values.ToArray();
	}
	private string AssetTag => _assetIndex == 0 ? "grade-curve" : "grade-lut";
	private GraphicsSettingId SelectedAsset => _assetIndex == 0 ? GraphicsSettingId.ColorCurve : GraphicsSettingId.ColorLut;
	private GraphicsSettingId SelectedStrength => _assetIndex == 0 ? GraphicsSettingId.ColorCurveStrength : GraphicsSettingId.ColorLutStrength;
	private GraphicsSettingValue[] AssetValues(float selectedStrength) => new[]
	{
		new GraphicsSettingValue(GraphicsSettingId.ColorGradingEnabled, true),
		new GraphicsSettingValue(GraphicsSettingId.ColorCurve, _assetIndex == 0 ? _curveId : default),
		new GraphicsSettingValue(GraphicsSettingId.ColorCurveStrength, _assetIndex == 0 ? selectedStrength : 1.0F),
		new GraphicsSettingValue(GraphicsSettingId.ColorLut, _assetIndex == 1 ? _lutId : default),
		new GraphicsSettingValue(GraphicsSettingId.ColorLutStrength, _assetIndex == 1 ? selectedStrength : 1.0F)
	};
	private void PrepareAssets()
	{
		// These are native raw-byte UUIDs from actual imported asset metadata,
		// not System.Guid text (whose first fields have different byte ordering).
		// Missing required assets fail the final full phase after the existing
		// nonasset cases; they are never silently skipped or declared successful.
		_curveId = ImportedAsset("TERRAENGINE_COLOR_FIXTURE_CURVE_ID");
		_lutId = ImportedAsset("TERRAENGINE_COLOR_FIXTURE_LUT_ID");
		Require(!_curveId.Equals(_lutId), "Curve and LUT must be distinct actual imported assets.");
		var descriptors = Graphics.DiscoverSettings(_targetA).ToDictionary(d => d.Id);
		foreach (var id in AssetSettings)
			Require(descriptors.TryGetValue(id, out var descriptor) && descriptor.Available && descriptor.CameraScope
				&& descriptor.Activation == ExpectedActivation(id), "Required grading asset capability unavailable: " + id);
		_assetResetExpected = AssetSettings.Select(id => _baselineA!.Values.Single(value => value.Id == id)).ToArray();
		_assetInputsConfigured = true;
	}
	private static GraphicsActivation ExpectedActivation(GraphicsSettingId id) => id switch
	{
		// These settings can require a target, pipeline, or immutable texture
		// before publication. The remaining controls update existing parameters.
		GraphicsSettingId.FixedExposureCompensation or GraphicsSettingId.ExposureMode or GraphicsSettingId.ToneMappingMode
			or GraphicsSettingId.ColorGradingEnabled or GraphicsSettingId.ColorCurve or GraphicsSettingId.ColorLut
			or GraphicsSettingId.BloomEnabled or GraphicsSettingId.BloomIntensity => GraphicsActivation.Rebuild,
		_ => GraphicsActivation.Live
	};
	private static PhysicsUuid ImportedAsset(string environmentName)
	{
		string? value = Environment.GetEnvironmentVariable(environmentName);
		Require(value is { Length: 32 } && value.All(Uri.IsHexDigit), "Required imported native32hex asset ID missing or malformed: " + environmentName);
		byte[] bytes = Convert.FromHexString(value!);
		var result = new PhysicsUuid { First = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(0, 8)), Second = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(8, 8)) };
		Require(result.IsValid, "Imported asset ID must be nonzero: " + environmentName);
		return result;
	}
	private static string AssetHex(PhysicsUuid id)
	{
		Span<byte> bytes = stackalloc byte[16];
		BinaryPrimitives.WriteUInt64LittleEndian(bytes[..8], id.First);
		BinaryPrimitives.WriteUInt64LittleEndian(bytes[8..], id.Second);
		return Convert.ToHexString(bytes).ToLowerInvariant();
	}
	private static void RequireValues(GraphicsSettingsSnapshot snapshot, IEnumerable<GraphicsSettingValue> expected)
	{
		foreach (var value in expected) Require(Equals(snapshot.Values.Single(v => v.Id == value.Id).Value, value.Value), "Control did not publish: " + value.Id);
	}

	private bool Refresh()
	{
		_a = Graphics.ReadSettings(_targetA).Snapshot; _b = Graphics.ReadSettings(_targetB).Snapshot;
		return _a is { PublishedFrameId: > 0, Revision: > 0 } && _b is { PublishedFrameId: > 0, Revision: > 0 };
	}
	private void Submit(float ev, GraphicsSettingsTarget target) => Begin(Graphics.ApplySettings(new[] { new GraphicsSettingValue(GraphicsSettingId.ExposureMode, 1U), new GraphicsSettingValue(GraphicsSettingId.FixedExposureCompensation, ev) }, target));
	private void Begin(GraphicsRequestReceipt receipt)
	{
		_receipts.Add(receipt); _requestId = receipt.RequestId;
		Require(_requestId != 0, "Request did not receive a native identity: " + receipt.Reason);
	}
	private GraphicsRequestReceipt? Terminal()
	{
		var receipt = Graphics.ReadRequest(_requestId);
		if (receipt is null) return null;
		if (!_receipts.Any(r => r.RequestId == receipt.RequestId && r.State == receipt.State && r.EffectiveRevision == receipt.EffectiveRevision)) _receipts.Add(receipt);
		return receipt.State is GraphicsRequestState.Applied or GraphicsRequestState.Rejected or GraphicsRequestState.Cancelled ? receipt : null;
	}
	private bool Applied(GraphicsSettingsTarget target)
	{
		var receipt = Terminal(); if (receipt is null) return false;
		Require(receipt.State == GraphicsRequestState.Applied, "Native color request rejected: " + receipt.Reason);
		if (!Refresh()) return false;
		var snapshot = target.CameraId == _targetA.CameraId ? _a! : _b!;
		return receipt.EffectiveRevision > 0 && receipt.AppliedFrameId > 0
			&& snapshot.Revision >= receipt.EffectiveRevision && snapshot.PublishedFrameId >= receipt.AppliedFrameId;
	}
	private void Capture(string tag, Phase next)
	{
		_captureTag = tag; _afterCapture = next; SetPhase(Phase.Capture);
	}
	private void SetPhase(Phase phase) { _phase = phase; _phaseStarted = Stopwatch.GetTimestamp(); Observe("pending"); }
	private static float Float(GraphicsSettingsSnapshot s, GraphicsSettingId id) => (float)s.Values.Single(v => v.Id == id).Value;
	private static void SameValues(GraphicsSettingsSnapshot before, GraphicsSettingsSnapshot after)
	{
		Require(before.Values.Count == after.Values.Count, "Setting inventory changed.");
		foreach (var value in before.Values) Require(Equals(after.Values.Single(v => v.Id == value.Id).Value, value.Value), "Unexpected value change: " + value.Id);
	}
	private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
	private void Fail(Exception error)
	{
		_failure = error.ToString(); _finished = true; Log.Error("ColorSettingsFixture failed: " + error.Message);
		if (_configured) Observe("failed");
	}
	private void Observe(string status)
	{
		if (!_configured) return;
		_pendingStatus = status;
		try
		{
			using var bytes = new MemoryStream();
			using (var writer = new Utf8JsonWriter(bytes))
			{
				writer.WriteStartObject(); writer.WriteNumber("schemaVersion", 1);
				writer.WriteString("runId", _runId); writer.WriteString("bootId", _bootId);
				writer.WriteString("status", status); writer.WriteString("phase", _phase.ToString());
				writer.WriteString("validationDomain", "native-settings-only");
				writer.WriteString("captureTag", _captureTag); writer.WriteString("failure", _failure);
				writer.WriteNumber("controlIndex", _controlIndex); writer.WriteNumber("controlCount", _controls.Count);
				writer.WriteBoolean("requiredAssetInputsReady", _assetInputsConfigured); writer.WriteNumber("assetIndex", _assetIndex);
				writer.WriteString("curveAssetId", AssetHex(_curveId)); writer.WriteString("lutAssetId", AssetHex(_lutId));
				writer.WriteString("unregisteredAssetId", AssetHex(_missingAssetId));
				writer.WriteString("requestId", _requestId.ToString(CultureInfo.InvariantCulture)); writer.WriteNumber("updates", _updates);
				Snapshot(writer, "cameraA", _a); Snapshot(writer, "cameraB", _b);
				writer.WriteStartArray("checks"); foreach (string check in _checks) writer.WriteStringValue(check); writer.WriteEndArray();
				writer.WriteStartArray("receipts");
				foreach (var r in _receipts) { writer.WriteStartObject(); writer.WriteString("requestId", r.RequestId.ToString(CultureInfo.InvariantCulture)); writer.WriteString("state", r.State.ToString()); writer.WriteString("reason", r.Reason.ToString()); writer.WriteString("revision", r.EffectiveRevision.ToString(CultureInfo.InvariantCulture)); writer.WriteString("frameId", r.AppliedFrameId.ToString(CultureInfo.InvariantCulture)); writer.WriteEndObject(); }
				writer.WriteEndArray(); writer.WriteEndObject();
			}
			string path = Path.Combine(_root, "color-observation.json"), temp = path + "." + _bootId + ".pending";
			File.WriteAllBytes(temp, bytes.ToArray()); File.Move(temp, path, true); _pendingStatus = string.Empty;
		}
		catch (Exception error) when (error is IOException or UnauthorizedAccessException) { /* Runner deadline detects persistently unavailable evidence. */ }
	}
	private static void Snapshot(Utf8JsonWriter writer, string name, GraphicsSettingsSnapshot? s)
	{
		if (s is null) { writer.WriteNull(name); return; }
		writer.WriteStartObject(name); writer.WriteString("cameraId", s.Target.CameraId);
		writer.WriteString("revision", s.Revision.ToString(CultureInfo.InvariantCulture)); writer.WriteString("frameId", s.PublishedFrameId.ToString(CultureInfo.InvariantCulture));
		writer.WriteString("sceneGeneration", s.Target.SceneGeneration.ToString(CultureInfo.InvariantCulture)); writer.WriteString("objectLifetimeGeneration", s.Target.ObjectLifetimeGeneration.ToString(CultureInfo.InvariantCulture));
		writer.WriteString("componentGeneration", s.Target.ComponentGeneration.ToString(CultureInfo.InvariantCulture)); writer.WriteString("bindingGeneration", s.Target.BindingGeneration.ToString(CultureInfo.InvariantCulture)); writer.WriteString("viewLifetimeGeneration", s.Target.ViewLifetimeGeneration.ToString(CultureInfo.InvariantCulture)); writer.WriteNumber("outputId", s.Target.OutputId);
		writer.WriteNumber("internalWidth", s.InternalWidth); writer.WriteNumber("internalHeight", s.InternalHeight);
		writer.WriteNumber("outputWidth", s.OutputWidth); writer.WriteNumber("outputHeight", s.OutputHeight); writer.WriteNumber("sampleCount", s.SampleCount);
		writer.WriteString("rendererSessionGeneration", s.Lifecycle.RendererSessionGeneration.ToString(CultureInfo.InvariantCulture));
		writer.WriteString("backendResourceGeneration", s.Lifecycle.BackendResourceGeneration.ToString(CultureInfo.InvariantCulture));
		writer.WriteStartArray("values");
		foreach (var value in s.Values) { writer.WriteStartObject(); writer.WriteNumber("id", (uint)value.Id); writer.WriteString("kind", value.ValueKind.ToString()); writer.WriteString("value", value.Value is PhysicsUuid asset ? AssetHex(asset) : Convert.ToString(value.Value, CultureInfo.InvariantCulture)); writer.WriteEndObject(); }
		writer.WriteEndArray(); writer.WriteEndObject();
	}
}
