using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using TerraEngine;

// This controller runs only in a disposable validation project. The installed
// runner supplies the observation directory and scenario for each fresh process.
// Every pass comes from a native receipt or an effective published snapshot.
public sealed partial class GraphicsSettingsFixture : Script
{
	private enum Phase
	{
		Ready, LegacyPreset, LegacyPatch, LegacyRestore, DynamicBatch,
		InvalidBatch, Rebuild, Cameras, CameraRetired, StaleCamera,
		PreferenceSaved, PreferenceChanged, PreferenceRestored,
		StartupStaged, StartupObserved, StartupDenied, Finished
	}

	private const string RootVariable = "TERRAENGINE_GRAPHICS_FIXTURE_ROOT";
	private const string RunVariable = "TERRAENGINE_GRAPHICS_FIXTURE_RUN_ID";
	private const string ScenarioVariable = "TERRAENGINE_GRAPHICS_FIXTURE_SCENARIO";
	private readonly string _bootId = Guid.NewGuid().ToString("N");
	private readonly List<string> _checks = new();
	private readonly List<GraphicsRequestReceipt> _receipts = new();
	private string _root = string.Empty;
	private string _runId = string.Empty;
	private string _scenario = string.Empty;
	private string _failure = string.Empty;
	private string _pendingObservation = string.Empty;
	private Phase _phase;
	private bool _configured;
	private bool _finished;
	private long _phaseStarted;
	private ulong _updates;
	private ulong _requestId;
	private ulong _legacyGeneration;
	private RenderSettings _legacyOriginal;
	private uint _legacyFilter;
	private GraphicsSettingsSnapshot? _before;
	private GraphicsSettingsSnapshot? _observed;
	private GraphicsSettingsSnapshot? _cameraSnapshotA;
	private GraphicsSettingsSnapshot? _cameraSnapshotB;
	private GraphicsSettingsTarget _cameraTargetA;
	private GraphicsSettingsTarget _cameraTargetB;
	private TerraEngine.Object? _cameraObjectB;
	private GraphicsSettingValue[] _expected = Array.Empty<GraphicsSettingValue>();

	public override void OnStart()
	{
		// A normal developer opening the source fixture has not opted into this
		// destructive disposable-scene test until the runner provides all inputs.
		string? root = Environment.GetEnvironmentVariable(RootVariable);
		string? run = Environment.GetEnvironmentVariable(RunVariable);
		string? scenario = Environment.GetEnvironmentVariable(ScenarioVariable);
		if (string.IsNullOrWhiteSpace(root) && string.IsNullOrWhiteSpace(run) && string.IsNullOrWhiteSpace(scenario)) return;
		try
		{
			Require(!string.IsNullOrWhiteSpace(root) && !string.IsNullOrWhiteSpace(run) && !string.IsNullOrWhiteSpace(scenario), "Missing fixture environment inputs.");
			_root = Path.GetFullPath(root!);
			_runId = run!;
			_scenario = scenario!;
			Require(Directory.Exists(_root), "The runner must create its observation directory.");
			Require(_runId.All(character => char.IsAsciiLetterOrDigit(character) || character == '-'), "The run identity must contain ASCII letters, digits, or dashes.");
			Require(_scenario is "runtime" or "startup-stage" or "startup-observe-success" or "startup-observe-failure" or "startup-denied", "Unknown fixture scenario.");
			_configured = true;
			SetPhase(Phase.Ready);
			WriteObservation("pending");
		}
		catch (Exception error)
		{
			Fail(error);
		}
	}

	public override void OnUpdate()
	{
		if (!_configured) return;
		if (_finished)
		{
			// Windows readers can briefly deny an atomic replacement. Preserve the
			// final outcome and retry its observation until the runner receives it.
			if (_pendingObservation.Length != 0) WriteObservation(_pendingObservation);
			return;
		}
		++_updates;
		try
		{
			Require(Stopwatch.GetElapsedTime(_phaseStarted).TotalSeconds < 60.0, "Fixture phase exceeded its native publication deadline.");
			Advance();
			// Retain progress even while a real renderer rebuild is pending. The
			// runner can distinguish a live callback from an expired process.
			if (!_finished && _updates % 30U == 0U) WriteObservation("pending");
		}
		catch (Exception error)
		{
			Fail(error);
		}
	}

	private void Advance()
	{
		switch (_phase)
		{
			case Phase.Ready:
				StartScenario();
				break;
			case Phase.LegacyPreset:
				if (Graphics.RuntimeSettingsGeneration <= _legacyGeneration) return;
				Require(Graphics.QualityPreset == QualityPreset.High, "The legacy named preset did not become effective.");
				_checks.Add("legacy-preset");
				RenderSettings legacy = Graphics.RenderSettings;
				_legacyFilter = AlternateFilter(legacy.ShadowFilteringSize);
				legacy.ShadowFilteringSize = _legacyFilter;
				_legacyGeneration = Graphics.RuntimeSettingsGeneration;
				RequireAccepted(Graphics.ApplyRuntimeSettings(legacy));
				SetPhase(Phase.LegacyPatch);
				break;
			case Phase.LegacyPatch:
				if (Graphics.RuntimeSettingsGeneration <= _legacyGeneration) return;
				Require(Graphics.RenderSettings.ShadowFilteringSize == _legacyFilter, "The legacy settings packet did not apply.");
				_checks.Add("legacy-settings");
				_legacyGeneration = Graphics.RuntimeSettingsGeneration;
				RequireAccepted(Graphics.ApplyRuntimeSettings(_legacyOriginal));
				SetPhase(Phase.LegacyRestore);
				break;
			case Phase.LegacyRestore:
				if (Graphics.RuntimeSettingsGeneration <= _legacyGeneration) return;
				_before = ReadPublished();
				if (_before is null) return;
				_expected = new[] {
					new GraphicsSettingValue(GraphicsSettingId.ShadowFilteringSize, AlternateFilter(ReadUInt(_before, GraphicsSettingId.ShadowFilteringSize))),
					new GraphicsSettingValue(GraphicsSettingId.IndirectStrength, AlternateScalar(ReadFloat(_before, GraphicsSettingId.IndirectStrength)))
				};
				BeginRequest(Graphics.ApplySettings(_expected), Phase.DynamicBatch);
				break;
			case Phase.DynamicBatch:
				if (!ObserveApplied()) return;
				RequireValues(_observed!, _expected);
				_checks.Add("atomic-live-batch");
				_before = _observed;
				BeginRequest(Graphics.ApplySettings(new[] {
					new GraphicsSettingValue(GraphicsSettingId.ShadowFilteringSize, AlternateFilter(ReadUInt(_before!, GraphicsSettingId.ShadowFilteringSize))),
					// A finite wire value reaches the ordered controller. The ABI
					// rejects malformed floating-point payloads before assigning an ID.
					new GraphicsSettingValue(GraphicsSettingId.IndirectStrength, 9.0F)
				}), Phase.InvalidBatch);
				break;
			case Phase.InvalidBatch:
				if (ReadTerminal() is not { } invalid) return;
				Require(invalid.State == GraphicsRequestState.Rejected && invalid.Reason == GraphicsRequestReason.InvalidValue, "An invalid mixed batch must receive an InvalidValue rejection.");
				_observed = ReadPublished();
				if (_observed is null) return;
				RequireSnapshotsEqual(_before!, _observed);
				_checks.Add("rejected-batch-rollback");
				RequireCapability(GraphicsSettingId.ShadowResolution, GraphicsActivation.Rebuild);
				_expected = new[] { new GraphicsSettingValue(GraphicsSettingId.ShadowResolution,
					ReadUInt(_observed, GraphicsSettingId.ShadowResolution) == 1024U ? 2048U : 1024U) };
				BeginRequest(Graphics.ApplySettings(_expected), Phase.Rebuild);
				break;
			case Phase.Rebuild:
				if (!ObserveApplied()) return;
				RequireValues(_observed!, _expected);
				_checks.Add("target-rebuild");
				SetPhase(Phase.Cameras);
				break;
			case Phase.Cameras:
				ObserveCameras();
				break;
			case Phase.CameraRetired:
				if (TerraEngine.Object.Find("CameraB") is not null) return;
				BeginRequest(Graphics.ApplySettings(new[] {
					new GraphicsSettingValue(GraphicsSettingId.FixedExposureCompensation, 0.0F)
				}, _cameraTargetB), Phase.StaleCamera);
				break;
			case Phase.StaleCamera:
				if (ReadTerminal() is not { } stale) return;
				Require(stale.State == GraphicsRequestState.Rejected
					&& stale.Reason is GraphicsRequestReason.StaleLifetime or GraphicsRequestReason.TargetUnavailable,
					"A retired camera target must report a stale or unavailable target.");
				_cameraSnapshotA = ReadPublished(_cameraTargetA);
				if (_cameraSnapshotA is null) return;
				_checks.Add("stale-camera-rejection");
				_before = ReadPublished();
				if (_before is null) return;
				BeginRequest(Graphics.SavePreferences(PreferenceName), Phase.PreferenceSaved);
				break;
			case Phase.PreferenceSaved:
				if (!ObserveApplied(requirePublishedFrame: false)) return;
				BeginRequest(Graphics.ApplySettings(new[] {
					new GraphicsSettingValue(GraphicsSettingId.ShadowFilteringSize, AlternateFilter(ReadUInt(_before!, GraphicsSettingId.ShadowFilteringSize)))
				}), Phase.PreferenceChanged);
				break;
			case Phase.PreferenceChanged:
				if (!ObserveApplied()) return;
				Require(ReadUInt(_observed!, GraphicsSettingId.ShadowFilteringSize) != ReadUInt(_before!, GraphicsSettingId.ShadowFilteringSize), "The preference test must change the live value before restoring it.");
				BeginRequest(Graphics.RestorePreferences(PreferenceName), Phase.PreferenceRestored);
				break;
			case Phase.PreferenceRestored:
				if (!ObserveApplied()) return;
				RequireSnapshotsEqual(_before!, _observed!);
				_checks.Add("preference-save-restore");
				Pass();
				break;
			case Phase.StartupStaged:
				if (Graphics.ReadRequest(_requestId) is not { } staged) return;
				RememberReceipt(staged);
				Require(staged.State is not (GraphicsRequestState.Rejected or GraphicsRequestState.Cancelled), "Next-launch staging was rejected or cancelled.");
				if (staged.State != GraphicsRequestState.ScheduledRestart) return;
				_observed = ReadPublished();
				if (_observed is null) return;
				RequireSnapshotsEqual(_before!, _observed);
				WriteStartupState();
				_checks.Add("mixed-startup-staged-without-live-change");
				Pass();
				break;
			case Phase.StartupObserved:
				ObserveStartup();
				break;
			case Phase.StartupDenied:
				if (ReadTerminal() is not { } denied) return;
				Require(denied.State == GraphicsRequestState.Rejected && denied.Reason == GraphicsRequestReason.InvalidScope,
					"Editor Play Mode must reject exported startup transactions with InvalidScope.");
				_observed = ReadPublished();
				if (_observed is null) return;
				RequireSnapshotsEqual(_before!, _observed);
				Require(_before!.Revision == _observed.Revision, "A rejected editor startup request must not publish a new effective revision.");
				_checks.Add("editor-startup-denied-without-live-change");
				Pass();
				break;
		}
	}

	private string PreferenceName => "graphics-fixture-" + _runId;

	private void StartScenario()
	{
		_observed = ReadPublished();
		if (_observed is null) return;
		Require(Graphics.AbiVersion == 1U, "The legacy ABI version must remain one.");
		if (_scenario == "startup-denied")
		{
			// Play entry admits the first script callback before the protected
			// renderer publishes the new session. Compare revisions within it.
			if (_observed.Lifecycle.PlaySessionGeneration == 0U) return;
			// This is a real mixed startup/live batch, but editor Play Mode has
			// no authority to stage the exported game's next-launch transaction.
			_before = _observed;
			_expected = new[] {
				new GraphicsSettingValue(GraphicsSettingId.ValidationEnabled, !ReadBool(_before, GraphicsSettingId.ValidationEnabled)),
				new GraphicsSettingValue(GraphicsSettingId.ShadowFilteringSize, AlternateFilter(ReadUInt(_before, GraphicsSettingId.ShadowFilteringSize)))
			};
			BeginRequest(Graphics.StageNextLaunch(_expected), Phase.StartupDenied);
			return;
		}
		if (_scenario.StartsWith("startup-observe-", StringComparison.Ordinal))
		{
			ReadStartupState();
			// Keep the first observed effective profile for the fresh-process check.
			SetPhase(Phase.StartupObserved);
			return;
		}
		if (_scenario == "startup-stage")
		{
			RequireCapability(GraphicsSettingId.VariableRateShading, GraphicsActivation.NextLaunch);
			RequireCapability(GraphicsSettingId.ExposureMode, GraphicsActivation.Rebuild);
			_before = _observed;
			// A fresh process must apply native flags and the prepared color chain in one receipt.
			_expected = new[] {
				new GraphicsSettingValue(GraphicsSettingId.VariableRateShading, !ReadBool(_before, GraphicsSettingId.VariableRateShading)),
				new GraphicsSettingValue(GraphicsSettingId.ShadowFilteringSize, AlternateFilter(ReadUInt(_before, GraphicsSettingId.ShadowFilteringSize))),
				new GraphicsSettingValue(GraphicsSettingId.ExposureMode, (uint)GraphicsExposureMode.Fixed),
				new GraphicsSettingValue(GraphicsSettingId.FixedExposureCompensation, 0.5f)
			};
			BeginRequest(Graphics.StageNextLaunch(_expected), Phase.StartupStaged);
			return;
		}
		_legacyOriginal = Graphics.RenderSettings;
		_legacyGeneration = Graphics.RuntimeSettingsGeneration;
		Graphics.QualityPreset = QualityPreset.High;
		SetPhase(Phase.LegacyPreset);
	}

	private void ObserveCameras()
	{
		TerraEngine.Object? cameraA = TerraEngine.Object.Find("CameraA");
		_cameraObjectB = TerraEngine.Object.Find("CameraB");
		if (cameraA?.GetComponent<Camera>() is not { } componentA
			|| _cameraObjectB?.GetComponent<Camera>() is not { } componentB) return;
		_cameraTargetA = Graphics.ForCamera(componentA);
		_cameraTargetB = Graphics.ForCamera(componentB);
		_cameraSnapshotA = ReadPublished(_cameraTargetA);
		_cameraSnapshotB = ReadPublished(_cameraTargetB);
		if (_cameraSnapshotA is null || _cameraSnapshotB is null) return;
		Require(_cameraSnapshotA.Target.CameraId != _cameraSnapshotB.Target.CameraId, "The two camera publications must identify different camera UUIDs.");
		Require(_cameraSnapshotA.Target.SceneGeneration != 0U
			&& _cameraSnapshotA.Target.SceneGeneration == _cameraSnapshotB.Target.SceneGeneration,
			"Camera snapshots must belong to the live scene generation.");
		Require(_cameraSnapshotA.Target.ObjectLifetimeGeneration != 0U && _cameraSnapshotB.Target.ObjectLifetimeGeneration != 0U
			&& _cameraSnapshotA.Target.ComponentGeneration != 0U && _cameraSnapshotB.Target.ComponentGeneration != 0U,
			"Camera publications require actual object and component generations.");
		_checks.Add("two-camera-publications");
		_cameraObjectB.Destroy();
		SetPhase(Phase.CameraRetired);
	}

	private void ObserveStartup()
	{
		if (ReadTerminal() is not { } receipt) return;
		Require(receipt.RequestId == _requestId, "Startup receipt identity changed across processes.");
		if (_scenario == "startup-observe-success")
		{
			Require(receipt.State == GraphicsRequestState.Applied, "The staged batch did not apply before startup completed.");
			RequireValues(_observed!, _expected);
			_checks.Add("startup-original-receipt-applied");
		}
		else
		{
			Require(receipt.State == GraphicsRequestState.Rejected && receipt.Reason != GraphicsRequestReason.None, "Injected startup failure needs a terminal rejection and reason.");
			RequireValues(_observed!, _before!.Values.ToArray());
			_checks.Add("startup-original-receipt-rejected-with-rollback");
		}
		Pass();
	}

	private GraphicsSettingsSnapshot? ReadPublished(GraphicsSettingsTarget target = default)
	{
		GraphicsSettingsReadResult result = Graphics.ReadSettings(target);
		if (result.Snapshot is not { } snapshot) return null;
		return snapshot.Revision != 0U && snapshot.PublishedFrameId != 0U ? snapshot : null;
	}

	private void BeginRequest(GraphicsRequestReceipt receipt, Phase phase)
	{
		Require(receipt.RequestId != 0U, "Every submitted fixture request needs an observable native identity.");
		_requestId = receipt.RequestId;
		RememberReceipt(receipt);
		SetPhase(phase);
	}

	private GraphicsRequestReceipt? ReadTerminal()
	{
		if (Graphics.ReadRequest(_requestId) is not { } receipt) return null;
		Require(receipt.RequestId == _requestId, "Receipt lookup returned another request.");
		RememberReceipt(receipt);
		return receipt.State is GraphicsRequestState.Applied or GraphicsRequestState.Rejected or GraphicsRequestState.Cancelled ? receipt : null;
	}

	private void RememberReceipt(GraphicsRequestReceipt receipt)
	{
		if (_receipts.Any(existing => existing.RequestId == receipt.RequestId && existing.State == receipt.State
			&& existing.EffectiveRevision == receipt.EffectiveRevision && existing.AppliedFrameId == receipt.AppliedFrameId)) return;
		_receipts.Add(receipt);
	}

	private bool ObserveApplied(bool requirePublishedFrame = true)
	{
		if (ReadTerminal() is not { } receipt) return false;
		Require(receipt.State == GraphicsRequestState.Applied, "Native request failed: " + receipt.Reason);
		if (!requirePublishedFrame) return true;
		_observed = ReadPublished();
		if (_observed is null) return false;
		return receipt.EffectiveRevision != 0U && _observed.Revision >= receipt.EffectiveRevision
			&& receipt.AppliedFrameId != 0U && _observed.PublishedFrameId >= receipt.AppliedFrameId;
	}

	private static void RequireCapability(GraphicsSettingId id, GraphicsActivation activation)
	{
		foreach (var descriptor in Graphics.DiscoverSettings())
		{
			if (descriptor.Id != id) continue;
			Require(descriptor.Available && descriptor.Activation == activation,
				"Required fixture capability is unavailable or has the wrong activation: " + id);
			return;
		}
		throw new InvalidOperationException("Required fixture setting was not discovered: " + id);
	}

	private static object ReadValue(GraphicsSettingsSnapshot snapshot, GraphicsSettingId id)
		=> snapshot.Values.Single(item => item.Id == id).Value;
	private static uint ReadUInt(GraphicsSettingsSnapshot snapshot, GraphicsSettingId id)
		=> Convert.ToUInt32(ReadValue(snapshot, id), CultureInfo.InvariantCulture);
	private static float ReadFloat(GraphicsSettingsSnapshot snapshot, GraphicsSettingId id)
		=> Convert.ToSingle(ReadValue(snapshot, id), CultureInfo.InvariantCulture);
	private static bool ReadBool(GraphicsSettingsSnapshot snapshot, GraphicsSettingId id)
		=> Convert.ToBoolean(ReadValue(snapshot, id), CultureInfo.InvariantCulture);
	private static uint AlternateFilter(uint current) => current == 3U ? 5U : 3U;
	private static float AlternateScalar(float current) => current == 0.5F ? 1.0F : 0.5F;

	private static void RequireValues(GraphicsSettingsSnapshot snapshot, IEnumerable<GraphicsSettingValue> values)
	{
		foreach (GraphicsSettingValue expected in values)
			Require(Equals(ReadValue(snapshot, expected.Id), expected.Value), "Effective setting differs from the expected value: " + expected.Id);
	}

	private static void RequireSnapshotsEqual(GraphicsSettingsSnapshot before, GraphicsSettingsSnapshot after)
	{
		Require(before.Values.Count() == after.Values.Count(), "The effective setting inventory changed unexpectedly.");
		RequireValues(after, before.Values);
	}

	private static void RequireAccepted(GraphicsStatus status)
		=> Require(status is GraphicsStatus.Success or GraphicsStatus.Accepted, "Legacy settings update was rejected: " + status);
	private static void Require(bool condition, string message)
	{
		if (!condition) throw new InvalidOperationException(message);
	}

	private void SetPhase(Phase phase)
	{
		_phase = phase;
		_phaseStarted = Stopwatch.GetTimestamp();
		WriteObservation("pending");
	}

	private void Pass()
	{
		_finished = true;
		_phase = Phase.Finished;
		WriteObservation("passed");
	}

	private void Fail(Exception error)
	{
		_finished = true;
		_failure = error.ToString();
		Log.Error("GraphicsSettingsFixture failed: " + error.Message);
		if (!string.IsNullOrEmpty(_root) && Directory.Exists(_root)) WriteObservation("failed");
	}

	private void WriteObservation(string status)
	{
		if (!_configured) return;
		_pendingObservation = status;
		try
		{
			WriteObservationDocument(status);
			_pendingObservation = string.Empty;
		}
		catch (Exception error) when (error is IOException or UnauthorizedAccessException)
		{
			// Observation failure leaves the runner waiting. It cannot create a pass
			// from partial JSON, and a persistent failure reaches its own deadline.
		}
	}

	private void WriteObservationDocument(string status)
	{
		WriteJson("graphics-observation.json", writer => {
			writer.WriteStartObject();
			writer.WriteNumber("schemaVersion", 1);
			writer.WriteString("runId", _runId);
			writer.WriteString("bootId", _bootId);
			writer.WriteString("scenario", _scenario);
			writer.WriteString("phase", _phase.ToString());
			writer.WriteString("status", status);
			writer.WriteString("requestId", _requestId.ToString(CultureInfo.InvariantCulture));
			writer.WriteNumber("updates", _updates);
			writer.WriteString("failure", _failure);
			writer.WriteStartArray("receipts");
			foreach (GraphicsRequestReceipt receipt in _receipts)
			{
				writer.WriteStartObject();
				writer.WriteString("requestId", receipt.RequestId.ToString(CultureInfo.InvariantCulture));
				writer.WriteString("state", receipt.State.ToString());
				writer.WriteString("reason", receipt.Reason.ToString());
				writer.WriteString("effectiveRevision", receipt.EffectiveRevision.ToString(CultureInfo.InvariantCulture));
				writer.WriteString("appliedFrameId", receipt.AppliedFrameId.ToString(CultureInfo.InvariantCulture));
				writer.WriteEndObject();
			}
			writer.WriteEndArray();
			WriteSnapshot(writer, "effective", _observed);
			WriteSnapshot(writer, "cameraA", _cameraSnapshotA);
			WriteSnapshot(writer, "cameraB", _cameraSnapshotB);
			writer.WriteStartArray("checks");
			foreach (string check in _checks) writer.WriteStringValue(check);
			writer.WriteEndArray();
			writer.WriteEndObject();
		});
	}

	private static void WriteSnapshot(Utf8JsonWriter writer, string name, GraphicsSettingsSnapshot? snapshot)
	{
		writer.WritePropertyName(name);
		if (snapshot is null) { writer.WriteNullValue(); return; }
		writer.WriteStartObject();
		writer.WriteString("revision", snapshot.Revision.ToString(CultureInfo.InvariantCulture));
		writer.WriteString("publishedFrameId", snapshot.PublishedFrameId.ToString(CultureInfo.InvariantCulture));
		writer.WriteString("cameraId", snapshot.Target.CameraId);
		writer.WriteString("sceneGeneration", snapshot.Target.SceneGeneration.ToString(CultureInfo.InvariantCulture));
		writer.WriteString("objectLifetimeGeneration", snapshot.Target.ObjectLifetimeGeneration.ToString(CultureInfo.InvariantCulture));
		writer.WriteString("componentGeneration", snapshot.Target.ComponentGeneration.ToString(CultureInfo.InvariantCulture));
		writer.WriteString("bindingGeneration", snapshot.Target.BindingGeneration.ToString(CultureInfo.InvariantCulture));
		writer.WriteString("viewLifetimeGeneration", snapshot.Target.ViewLifetimeGeneration.ToString(CultureInfo.InvariantCulture));
		writer.WriteNumber("outputId", snapshot.Target.OutputId);
		WriteValues(writer, "values", snapshot.Values);
		writer.WriteEndObject();
	}

	private void WriteStartupState()
	{
		WriteJson("graphics-startup-state.json", writer => {
			writer.WriteStartObject();
			writer.WriteNumber("schemaVersion", 1);
			writer.WriteString("runId", _runId);
			writer.WriteString("requestId", _requestId.ToString(CultureInfo.InvariantCulture));
			WriteValues(writer, "before", _before!.Values);
			WriteValues(writer, "expected", _expected);
			writer.WriteEndObject();
		});
	}

	private void ReadStartupState()
	{
		using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(_root, "graphics-startup-state.json")));
		JsonElement state = document.RootElement;
		Require(state.GetProperty("schemaVersion").GetInt32() == 1 && state.GetProperty("runId").GetString() == _runId,
			"The staged receipt observation belongs to a different test run.");
		_requestId = ulong.Parse(state.GetProperty("requestId").GetString()!, CultureInfo.InvariantCulture);
		Require(_requestId != 0U, "The saved startup request identity is empty.");
		_expected = ReadValues(state.GetProperty("expected"));
		_before = new GraphicsSettingsSnapshot { Values = ReadValues(state.GetProperty("before")) };
	}

	private static void WriteValues(Utf8JsonWriter writer, string name, IEnumerable<GraphicsSettingValue> values)
	{
		writer.WriteStartArray(name);
		foreach (GraphicsSettingValue item in values)
		{
			writer.WriteStartObject();
			writer.WriteNumber("id", (uint)item.Id);
			writer.WriteString("kind", item.Value.GetType().Name);
			writer.WritePropertyName("value");
			switch (item.Value)
			{
				case bool value: writer.WriteBooleanValue(value); break;
				case uint value: writer.WriteNumberValue(value); break;
				case ulong value: writer.WriteStringValue(value.ToString(CultureInfo.InvariantCulture)); break;
				case float value: writer.WriteNumberValue(value); break;
				case string value: writer.WriteStringValue(value); break;
				case PhysicsUuid value:
					// Each half is kept as a uint64, including zero for an unassigned lookup.
					writer.WriteStartArray();
					writer.WriteNumberValue(value.First);
					writer.WriteNumberValue(value.Second);
					writer.WriteEndArray();
					break;
				default: throw new InvalidOperationException("Unsupported fixture value type.");
			}
			writer.WriteEndObject();
		}
		writer.WriteEndArray();
	}

	private static GraphicsSettingValue[] ReadValues(JsonElement values)
	{
		var result = new List<GraphicsSettingValue>();
		foreach (JsonElement element in values.EnumerateArray())
		{
			GraphicsSettingId id = (GraphicsSettingId)element.GetProperty("id").GetUInt32();
			JsonElement value = element.GetProperty("value");
			result.Add(element.GetProperty("kind").GetString() switch {
				"Boolean" => new GraphicsSettingValue(id, value.GetBoolean()),
				"UInt32" => new GraphicsSettingValue(id, value.GetUInt32()),
				"UInt64" => new GraphicsSettingValue(id, ulong.Parse(value.GetString()!, CultureInfo.InvariantCulture)),
				"Single" => new GraphicsSettingValue(id, value.GetSingle()),
				"String" => new GraphicsSettingValue(id, value.GetString()!),
				"PhysicsUuid" => new GraphicsSettingValue(id, new PhysicsUuid { First = value[0].GetUInt64(), Second = value[1].GetUInt64() }),
				_ => throw new InvalidDataException("Unknown stored fixture value type.")
			});
		}
		return result.ToArray();
	}

	private void WriteJson(string name, Action<Utf8JsonWriter> body)
	{
		// Explicit writer calls work in NativeAOT without reflection metadata.
		// A checked complete-file replacement keeps a concurrent reader from
		// observing only the first half of a receipt or cross-launch state file.
		using var bytes = new MemoryStream();
		using (var writer = new Utf8JsonWriter(bytes)) body(writer);
		string path = Path.Combine(_root, name);
		string pending = path + "." + _bootId + ".pending";
		File.WriteAllBytes(pending, bytes.ToArray());
		File.Move(pending, path, overwrite: true);
	}
}
