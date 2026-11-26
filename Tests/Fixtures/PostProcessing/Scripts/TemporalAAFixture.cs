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

// Test-first managed ingress for the required Windows Vulkan coverage/temporal
// rows. Settings publications alone cannot pass: an independent native fixture
// must acknowledge real captures, mesh execution, motion and reset checks.
// The script never fabricates rendering evidence or changes a debug route.
public sealed partial class TemporalAAFixture : Script
{
	private const GraphicsSettingId Aa = (GraphicsSettingId)64;
	private const GraphicsSettingId Scale = (GraphicsSettingId)66;
	private const GraphicsSettingId Upscaling = (GraphicsSettingId)89;
	private const GraphicsSettingId Sharpness = (GraphicsSettingId)90;
	private sealed record RequiredCase(string Id, uint AaMode, uint UpscalingMode, float Scale, uint Samples, uint Width, uint Height);
	private static readonly RequiredCase[] Cases = {
		new("msaa4-native", 4U, 0U, 1F, 4U, 640U, 360U),
		new("taa-native", 5U, 0U, 1F, 1U, 640U, 360U),
		new("temporal-half-extent", 0U, 1U, 0.5F, 1U, 320U, 180U)
	};
	private enum Phase { Ready, Apply, Capture, RejectCombination, RollbackCapture, Done }
	private sealed record Publication(string CaseId, ulong RequestId, GraphicsSettingsSnapshot Snapshot, GraphicsRequestReceipt Receipt);
	private readonly List<Publication> _publications = new();
	private readonly List<GraphicsRequestReceipt> _receipts = new();
	private readonly List<JsonElement> _acknowledgements = new();
	private readonly List<string> _checks = new(), _blocked = new();
	private readonly string _boot = Guid.NewGuid().ToString("N");
	private GraphicsSettingDescriptor? _aa, _scale, _upscaling, _sharpness;
	private GraphicsSettingsSnapshot? _session, _effective, _before;
	private string _root = string.Empty, _run = string.Empty, _failure = string.Empty, _status = "pending";
	private Phase _phase;
	private long _phaseStarted;
	private int _caseIndex, _stable;
	private ulong _request, _updates;
	private bool _configured, _finished;

	public override void OnStart()
	{
		string? root = Environment.GetEnvironmentVariable("TERRAENGINE_TEMPORAL_AA_FIXTURE_ROOT");
		string? run = Environment.GetEnvironmentVariable("TERRAENGINE_TEMPORAL_AA_FIXTURE_RUN_ID");
		if (string.IsNullOrWhiteSpace(root) && string.IsNullOrWhiteSpace(run)) return;
		try {
			Require(!string.IsNullOrWhiteSpace(root) && !string.IsNullOrWhiteSpace(run), "Missing temporal fixture environment inputs.");
			_root = Path.GetFullPath(root!); _run = run!;
			Require(Directory.Exists(_root), "Runner must create the isolated observation directory.");
			Require(_run.All(c => char.IsAsciiLetterOrDigit(c) || c == '-'), "Invalid run identity.");
			_configured = true; SetPhase(Phase.Ready);
		} catch (Exception error) { Fail(error); }
	}

	public override void OnUpdate()
	{
		if (!_configured) return;
		if (_finished) { Observe(_status); return; }
		try {
			++_updates;
			Require(Stopwatch.GetElapsedTime(_phaseStarted).TotalSeconds < 120,
				"Temporal fixture deadline expired in " + _phase + ". Actual native capture ACK is mandatory.");
			Advance();
			if (!_finished && _updates % 30U == 0U) Observe("pending");
		} catch (Exception error) { Fail(error); }
	}

	private void Advance()
	{
		switch (_phase) {
		case Phase.Ready:
			_effective = Published(); if (_effective is null) return;
			if (_session is null || !SameLifetime(_session, _effective) || _session.Lifecycle.BackendResourceGeneration != _effective.Lifecycle.BackendResourceGeneration) {
				_session = _effective; _stable = 0; return;
			}
			if (++_stable < 3) return;
			var catalog = Graphics.DiscoverSettings();
			_aa = catalog.SingleOrDefault(d => d.Id == Aa); _scale = catalog.SingleOrDefault(d => d.Id == Scale);
			_upscaling = catalog.SingleOrDefault(d => d.Id == Upscaling); _sharpness = catalog.SingleOrDefault(d => d.Id == Sharpness);
			CheckDescriptor(_aa, GraphicsValueKind.Unsigned32, "AA ID64", required: true);
			CheckDescriptor(_scale, GraphicsValueKind.Scalar, "internal scale ID66", required: true);
			CheckDescriptor(_upscaling, GraphicsValueKind.Unsigned32, "upscaling ID89", required: true);
			foreach (uint mode in new uint[] { 0, 4, 5 }) if (!Option(_aa, mode)) _blocked.Add("Required AA option unavailable: " + mode);
			foreach (uint mode in new uint[] { 0, 1 }) if (!Option(_upscaling, mode)) _blocked.Add("Required upscaling option unavailable: " + mode);
			if (!ScalarOption(_scale, 1F) || !ScalarOption(_scale, 0.5F)) _blocked.Add("Required native/half internal scales unavailable.");
			// VRS is initialization-owned. The runner must launch the required
			// configuration; a live test cannot silently disable it or restart.
			if (_effective.Values.FirstOrDefault(v => v.Id == GraphicsSettingId.VariableRateShading).Value is true)
				_blocked.Add("Required native renderer must start with VRS disabled.");
			if (_blocked.Count != 0) { _finished = true; Observe("blocked"); return; }
			SetPhase(Phase.Apply); break;
		case Phase.Apply:
			if (_request == 0) {
				_before = Published(); if (_before is null) return;
				var row = Cases[_caseIndex];
				var values = new List<GraphicsSettingValue> { new(Aa, row.AaMode), new(Upscaling, row.UpscalingMode), new(Scale, row.Scale) };
				// Sharpening is explicitly optional. Zero is used when the actual
				// discovered control supports it, keeping unrelated styling fixed.
				if (ScalarOption(_sharpness, 0F)) values.Add(new(Sharpness, 0F));
				Begin(Graphics.ApplySettings(values.ToArray(), expectedRevision: _before.Revision)); return;
			}
			if (Terminal() is not { } receipt) return;
			Require(receipt.State == GraphicsRequestState.Applied, "Required temporal row did not apply: " + receipt.State + "/" + receipt.Reason);
			_effective = Published(); if (_effective is null) return;
			if (!Visible(receipt, _effective)) return;
			AssertCase(Cases[_caseIndex], _effective);
			_publications.Add(new(Cases[_caseIndex].Id, _request, _effective, receipt));
			_checks.Add(Cases[_caseIndex].Id + "-native-settings-published");
			SetPhase(Phase.Capture); break;
		case Phase.Capture:
			RequireCurrentPublication();
			if (!TryAcknowledge(Cases[_caseIndex].Id, rollback: false)) return;
			_checks.Add(Cases[_caseIndex].Id + "-external-native-capture-acknowledged");
			_request = 0;
			if (++_caseIndex < Cases.Length) SetPhase(Phase.Apply); else SetPhase(Phase.RejectCombination);
			break;
		case Phase.RejectCombination:
			if (_request == 0) {
				_before = Published(); if (_before is null) return;
				Begin(Graphics.ApplySettings(new[] { new GraphicsSettingValue(Aa, 4U), new GraphicsSettingValue(Upscaling, 1U),
					new GraphicsSettingValue(Scale, 0.5F) }, expectedRevision: _before.Revision)); return;
			}
			if (Terminal() is not { } rejected) return;
			Require(rejected.State == GraphicsRequestState.Rejected && rejected.Reason == GraphicsRequestReason.Unsupported
				&& !string.IsNullOrWhiteSpace(rejected.Detail), "MSAA with temporal upscaling must reject atomically as Unsupported with an explicit diagnostic.");
			_effective = Published(); if (_effective is null) return;
			Unchanged(_before!, _effective);
			_checks.Add("msaa-plus-upscaling-unsupported-atomic-settings-rollback");
			SetPhase(Phase.RollbackCapture); break;
		case Phase.RollbackCapture:
			RequireCurrentPublication();
			if (!TryAcknowledge("unsupported-msaa-upscale", rollback: true)) return;
			_checks.Add("msaa-plus-upscaling-external-native-rollback-acknowledged");
			SetPhase(Phase.Done); break;
		case Phase.Done:
			Require(_acknowledgements.Count == 4 && _publications.Count == 3, "Missing required native rows or rollback evidence.");
			_finished = true; Observe("passed"); break;
		}
	}

	private void CheckDescriptor(GraphicsSettingDescriptor? descriptor, GraphicsValueKind kind, string name, bool required)
	{
		if (descriptor is null || !descriptor.Available || !descriptor.GameScope) {
			if (required) _blocked.Add(name + " is unavailable: " + (descriptor?.Detail ?? "descriptor missing"));
			return;
		}
		Require(descriptor.ValueKind == kind && descriptor.Activation == GraphicsActivation.Rebuild, name + " must use its typed rebuild contract.");
	}
	private static bool Option(GraphicsSettingDescriptor? descriptor, uint value) => descriptor is { Available: true, GameScope: true }
		&& descriptor.Options.Any(v => Convert.ToUInt32(v, CultureInfo.InvariantCulture) == value);
	private static bool ScalarOption(GraphicsSettingDescriptor? descriptor, float value) => descriptor is { Available: true, GameScope: true, ValueKind: GraphicsValueKind.Scalar }
		&& (descriptor.Minimum is null || Convert.ToSingle(descriptor.Minimum, CultureInfo.InvariantCulture) <= value)
		&& (descriptor.Maximum is null || Convert.ToSingle(descriptor.Maximum, CultureInfo.InvariantCulture) >= value)
		&& (descriptor.Options.Count == 0 || descriptor.Options.Any(v => Convert.ToSingle(v, CultureInfo.InvariantCulture) == value));
	private static object Value(GraphicsSettingsSnapshot snapshot, GraphicsSettingId id) => snapshot.Values.Single(v => v.Id == id).Value;
	private static uint Unsigned(GraphicsSettingsSnapshot snapshot, GraphicsSettingId id) => Convert.ToUInt32(Value(snapshot, id), CultureInfo.InvariantCulture);
	private static bool SameLifetime(GraphicsSettingsSnapshot a, GraphicsSettingsSnapshot b) => a.Lifecycle.ProjectGeneration == b.Lifecycle.ProjectGeneration
		&& a.Lifecycle.PlaySessionGeneration == b.Lifecycle.PlaySessionGeneration && a.Lifecycle.RendererSessionGeneration == b.Lifecycle.RendererSessionGeneration;
	private GraphicsSettingsSnapshot? Published()
	{
		var snapshot = Graphics.ReadSettings().Snapshot;
		if (snapshot is not { Revision: > 0, PublishedFrameId: > 0 }) return null;
		Require(!snapshot.Target.IsCamera, "Game query returned another scope.");
		if (_phase != Phase.Ready) Require(_session is not null && SameLifetime(_session, snapshot), "Project, renderer or Play session changed during temporal validation.");
		return snapshot;
	}
	private static bool Visible(GraphicsRequestReceipt receipt, GraphicsSettingsSnapshot snapshot) => receipt.EffectiveRevision > 0
		&& receipt.AppliedFrameId > 0 && snapshot.Revision >= receipt.EffectiveRevision && snapshot.PublishedFrameId >= receipt.AppliedFrameId;
	private static void AssertCase(RequiredCase row, GraphicsSettingsSnapshot snapshot)
	{
		Require(Unsigned(snapshot, Aa) == row.AaMode && Unsigned(snapshot, Upscaling) == row.UpscalingMode
			&& Convert.ToSingle(Value(snapshot, Scale), CultureInfo.InvariantCulture) == row.Scale, "Required AA/upscale tuple differs from its native publication.");
		Require(snapshot.SampleCount == row.Samples && snapshot.InternalWidth == row.Width && snapshot.InternalHeight == row.Height
			&& snapshot.OutputWidth == 640U && snapshot.OutputHeight == 360U, "Required actual samples/internal/output extents differ.");
		Require(snapshot.PostProcessResourceGeneration != 0 && snapshot.PostProcessResourceGeneration == snapshot.Lifecycle.BackendResourceGeneration,
			"Required mode has no exact native preparation certificate.");
	}
	private static void Unchanged(GraphicsSettingsSnapshot before, GraphicsSettingsSnapshot after)
	{
		Require(before.Revision == after.Revision && before.SampleCount == after.SampleCount && before.InternalWidth == after.InternalWidth
			&& before.InternalHeight == after.InternalHeight && before.OutputWidth == after.OutputWidth && before.OutputHeight == after.OutputHeight
			&& before.PostProcessResourceGeneration == after.PostProcessResourceGeneration && before.Lifecycle.BackendResourceGeneration == after.Lifecycle.BackendResourceGeneration
			&& before.Values.Count == after.Values.Count && before.Values.All(v => after.Values.Any(w => v.Id == w.Id && Equals(v.Value, w.Value))),
			"Rejected or capture-pending transaction changed the original effective publication.");
	}
	private void RequireCurrentPublication()
	{
		var current = Published(); Require(current is not null && _effective is not null, "Required capture publication disappeared.");
		Unchanged(_effective!, current!);
	}
	private void Begin(GraphicsRequestReceipt receipt) { _request = receipt.RequestId; Remember(receipt); Require(_request != 0, "Native admission returned no request ID."); _phaseStarted = Stopwatch.GetTimestamp(); }
	private void Remember(GraphicsRequestReceipt receipt)
	{
		if (!_receipts.Any(r => r.RequestId == receipt.RequestId && r.State == receipt.State && r.EffectiveRevision == receipt.EffectiveRevision && r.AppliedFrameId == receipt.AppliedFrameId)) _receipts.Add(receipt);
	}
	private GraphicsRequestReceipt? Terminal()
	{
		var receipt = Graphics.ReadRequest(_request); if (receipt is null) return null;
		Require(receipt.RequestId == _request, "Receipt lookup returned another transaction."); Remember(receipt);
		Require(receipt.State is not (GraphicsRequestState.RestartRequired or GraphicsRequestState.ScheduledRestart or GraphicsRequestState.Cancelled), "Required temporal requests must settle without restart or cancellation.");
		return receipt.State is GraphicsRequestState.Applied or GraphicsRequestState.Rejected ? receipt : null;
	}

	private bool TryAcknowledge(string caseId, bool rollback)
	{
		string path = Path.Combine(_root, "temporal-aa-ack-" + caseId + ".json");
		if (!File.Exists(path)) return false;
		JsonDocument document;
		try { using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete); document = JsonDocument.Parse(input); }
		catch (IOException) { return false; }
		catch (JsonException) { return false; } // Atomic writer may not have replaced a temporary partial file yet.
		using (document) {
			var ack = document.RootElement;
			Require(ack.GetProperty("schemaVersion").GetInt32() == 1 && ack.GetProperty("runId").GetString() == _run
				&& ack.GetProperty("bootId").GetString() == _boot && ack.GetProperty("caseId").GetString() == caseId
				&& ack.GetProperty("requestId").GetString() == Decimal(_request), "Native capture ACK belongs to another fixture publication.");
			var snapshot = _effective!;
			Require(ack.GetProperty("graphicsSettingsRevision").GetString() == Decimal(snapshot.Revision)
				&& ack.GetProperty("renderer").GetString() == Decimal(snapshot.Lifecycle.RendererSessionGeneration)
				&& ack.GetProperty("backend").GetString() == Decimal(snapshot.Lifecycle.BackendResourceGeneration)
				&& ulong.Parse(ack.GetProperty("frameId").GetString()!, CultureInfo.InvariantCulture) >= snapshot.PublishedFrameId,
				"Native capture ACK has stale settings, backend or frame provenance.");
			Require(ack.GetProperty("aaMode").GetUInt32() == Unsigned(snapshot, Aa) && ack.GetProperty("upscalingMode").GetUInt32() == Unsigned(snapshot, Upscaling)
				&& ack.GetProperty("sampleCount").GetUInt32() == snapshot.SampleCount, "Native capture ACK differs from the actual selected mode/raster/samples.");
			Require(Extent(ack.GetProperty("internalExtent"), snapshot.InternalWidth, snapshot.InternalHeight)
				&& Extent(ack.GetProperty("outputExtent"), snapshot.OutputWidth, snapshot.OutputHeight), "Native capture ACK has different actual extents.");
			// Draw counters refer to scene geometry in the captured view, excluding
			// sky/fullscreen/HUD work. The ordinary mesh path needs no effect geometry replay.
			Require(ack.GetProperty("nativeBackendReadback").GetBoolean() && !ack.GetProperty("deterministicFallback").GetBoolean()
				&& !ack.GetProperty("validationOverrideActive").GetBoolean() && !ack.GetProperty("vrsEnabled").GetBoolean()
				&& ack.GetProperty("indexedDraws").GetUInt64() == 0,
				"Required ACK needs native mesh execution, VRS off and no debug override/fallback.");
			Require(ack.GetProperty(rollback ? "atomicRollbackVerified" : "edgeQualityPassed").GetBoolean(), "Native image acceptance failed.");
			if (!rollback && (Unsigned(snapshot, Aa) == 5U || Unsigned(snapshot, Upscaling) == 1U))
				foreach (string flag in new[] { "shimmerQualityPassed", "cameraMotionVerified", "rigidMotionVerified", "skinnedMotionVerified", "historyResetVerified", "viewIsolationVerified" })
					Require(ack.GetProperty(flag).GetBoolean(), "External temporal proof omitted " + flag);
			VerifyCapture(ack, snapshot.OutputWidth, snapshot.OutputHeight);
			_acknowledgements.Add(ack.Clone()); return true;
		}
	}
	private static bool Extent(JsonElement value, uint width, uint height) => value.GetArrayLength() == 2 && value[0].GetUInt32() == width && value[1].GetUInt32() == height;
	private void VerifyCapture(JsonElement ack, uint width, uint height)
	{
		string path = Path.GetFullPath(Path.Combine(_root, ack.GetProperty("capturePath").GetString()!));
		Require(path.StartsWith(_root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase), "Capture ACK path escaped the isolated evidence directory.");
		byte[] bytes = File.ReadAllBytes(path);
		Require(bytes.Length >= 33 && bytes.AsSpan(0, 8).SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 })
			&& bytes.AsSpan(12, 4).SequenceEqual("IHDR"u8) && BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(16, 4)) == width
			&& BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(20, 4)) == height, "Native capture ACK omitted a PNG at the required output extent.");
		Require(Convert.ToHexString(SHA256.HashData(bytes)).Equals(ack.GetProperty("captureSha256").GetString(), StringComparison.OrdinalIgnoreCase), "Native capture bytes differ from the acknowledged digest.");
	}

	private void SetPhase(Phase phase) { _phase = phase; _phaseStarted = Stopwatch.GetTimestamp(); Observe("pending"); }
	private void Fail(Exception error) { _failure = error.ToString(); _finished = true; if (Directory.Exists(_root)) Observe("failed"); }
	private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
	private static string Decimal(ulong value) => value.ToString(CultureInfo.InvariantCulture);
	private static void WriteExtent(Utf8JsonWriter writer, string name, uint width, uint height) { writer.WriteStartArray(name); writer.WriteNumberValue(width); writer.WriteNumberValue(height); writer.WriteEndArray(); }
	private static void WriteSnapshot(Utf8JsonWriter writer, string name, GraphicsSettingsSnapshot? snapshot)
	{
		if (snapshot is null) { writer.WriteNull(name); return; }
		writer.WriteStartObject(name); writer.WriteString("revision", Decimal(snapshot.Revision)); writer.WriteString("frameId", Decimal(snapshot.PublishedFrameId));
		writer.WriteString("project", Decimal(snapshot.Lifecycle.ProjectGeneration)); writer.WriteString("play", Decimal(snapshot.Lifecycle.PlaySessionGeneration));
		writer.WriteString("renderer", Decimal(snapshot.Lifecycle.RendererSessionGeneration)); writer.WriteString("backend", Decimal(snapshot.Lifecycle.BackendResourceGeneration));
		writer.WriteString("postProcessResourceGeneration", Decimal(snapshot.PostProcessResourceGeneration)); writer.WriteNumber("sampleCount", snapshot.SampleCount);
		WriteExtent(writer, "internalExtent", snapshot.InternalWidth, snapshot.InternalHeight); WriteExtent(writer, "outputExtent", snapshot.OutputWidth, snapshot.OutputHeight);
		foreach (var pair in new[] { ("aaMode", Aa), ("upscalingMode", Upscaling) }) {
			var field = snapshot.Values.FirstOrDefault(v => v.Id == pair.Item2);
			if (field.Value is uint value) writer.WriteNumber(pair.Item1, value); else writer.WriteNull(pair.Item1);
		}
		var scale = snapshot.Values.FirstOrDefault(v => v.Id == Scale); if (scale.Value is float scalar) writer.WriteNumber("internalScale", scalar); else writer.WriteNull("internalScale");
		writer.WriteEndObject();
	}
	private static void WriteDescriptor(Utf8JsonWriter writer, string name, GraphicsSettingDescriptor? descriptor)
	{
		if (descriptor is null) { writer.WriteNull(name); return; }
		writer.WriteStartObject(name); writer.WriteNumber("id", (uint)descriptor.Id); writer.WriteString("name", descriptor.Name); writer.WriteString("kind", descriptor.ValueKind.ToString());
		writer.WriteBoolean("available", descriptor.Available); writer.WriteBoolean("gameScope", descriptor.GameScope); writer.WriteString("activation", descriptor.Activation.ToString());
		writer.WriteString("unavailableReason", descriptor.UnavailableReason.ToString()); writer.WriteString("detail", descriptor.Detail);
		foreach (var bound in new[] { ("minimum", descriptor.Minimum), ("maximum", descriptor.Maximum) }) {
			if (bound.Item2 is float scalar) writer.WriteNumber(bound.Item1, scalar);
			else if (bound.Item2 is uint integer) writer.WriteNumber(bound.Item1, integer);
			else writer.WriteNull(bound.Item1);
		}
		writer.WriteStartArray("options"); foreach (object value in descriptor.Options) { if (value is uint mode) writer.WriteNumberValue(mode); else if (value is float scale) writer.WriteNumberValue(scale); } writer.WriteEndArray();
		writer.WriteEndObject();
	}
	private static void WriteReceipt(Utf8JsonWriter writer, GraphicsRequestReceipt receipt)
	{
		writer.WriteStartObject(); writer.WriteString("requestId", Decimal(receipt.RequestId)); writer.WriteString("state", receipt.State.ToString());
		writer.WriteString("reason", receipt.Reason.ToString()); writer.WriteString("detail", receipt.Detail);
		writer.WriteString("effectiveRevision", Decimal(receipt.EffectiveRevision)); writer.WriteString("appliedFrameId", Decimal(receipt.AppliedFrameId)); writer.WriteEndObject();
	}
	private void Observe(string status)
	{
		_status = status;
		try {
			using var bytes = new MemoryStream();
			using (var writer = new Utf8JsonWriter(bytes, new JsonWriterOptions { Indented = true })) {
				writer.WriteStartObject(); writer.WriteNumber("schemaVersion", 1); writer.WriteString("runId", _run); writer.WriteString("bootId", _boot);
				writer.WriteString("status", status); writer.WriteString("phase", _phase.ToString()); writer.WriteString("failure", _failure); writer.WriteString("requestId", Decimal(_request)); writer.WriteNumber("updates", _updates);
				writer.WriteBoolean("externalNativeAcknowledgementRequired", true); writer.WriteBoolean("allNativeRowsAcknowledged", _acknowledgements.Count == 4);
				writer.WriteString("nativeResetOrchestration", "External native fixture owns cuts, resize, disocclusion and independent views; no generic managed reset hook is assumed.");
				writer.WriteStartObject("discovery"); WriteDescriptor(writer, "aa", _aa); WriteDescriptor(writer, "internalScale", _scale); WriteDescriptor(writer, "upscaling", _upscaling); WriteDescriptor(writer, "sharpness", _sharpness); writer.WriteEndObject();
				writer.WriteStartArray("requiredCases"); foreach (var row in Cases) { writer.WriteStartObject(); writer.WriteString("caseId", row.Id); writer.WriteNumber("aaMode", row.AaMode); writer.WriteNumber("upscalingMode", row.UpscalingMode); writer.WriteNumber("internalScale", row.Scale); writer.WriteNumber("sampleCount", row.Samples); WriteExtent(writer, "internalExtent", row.Width, row.Height); WriteExtent(writer, "outputExtent", 640U, 360U); writer.WriteEndObject(); } writer.WriteEndArray();
				writer.WriteStartArray("publications"); foreach (var row in _publications) { writer.WriteStartObject(); writer.WriteString("caseId", row.CaseId); writer.WriteString("requestId", Decimal(row.RequestId)); WriteSnapshot(writer, "effective", row.Snapshot); writer.WritePropertyName("receipt"); WriteReceipt(writer, row.Receipt); writer.WriteEndObject(); } writer.WriteEndArray();
				writer.WriteStartArray("receipts"); foreach (var receipt in _receipts) WriteReceipt(writer, receipt); writer.WriteEndArray();
				writer.WriteStartArray("nativeAcknowledgements"); foreach (var ack in _acknowledgements) ack.WriteTo(writer); writer.WriteEndArray();
				writer.WriteStartArray("checks"); foreach (string check in _checks) writer.WriteStringValue(check); writer.WriteEndArray();
				writer.WriteStartArray("blocked"); foreach (string blocked in _blocked) writer.WriteStringValue(blocked); writer.WriteEndArray();
				writer.WriteString("captureCaseId", _phase == Phase.RollbackCapture ? "unsupported-msaa-upscale" : Cases[Math.Min(_caseIndex, Cases.Length - 1)].Id);
				WriteSnapshot(writer, "effective", _effective); WriteSnapshot(writer, "before", _before); writer.WriteEndObject();
			}
			string path = Path.Combine(_root, "temporal-aa-observation.json"), temporary = path + "." + _boot + ".tmp";
			File.WriteAllBytes(temporary, bytes.ToArray()); File.Move(temporary, path, true);
		} catch (IOException) { /* A runner may briefly share the current observation; retry the atomic write next update. */ }
	}
}
