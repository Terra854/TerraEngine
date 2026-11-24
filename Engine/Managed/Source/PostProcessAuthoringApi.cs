// Versioned copied authoring bridge. Native owns validation, queueing and frames.
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace TerraEngine
{
	public static class PostProcessAuthoring
	{
		public static PostProcessCameraAuthoring ForCamera(Camera camera)
		{
			ArgumentNullException.ThrowIfNull(camera);
			return new(camera);
		}
		public static PostProcessAuthoringReceipt? ReadRequest(ulong requestId)
			=> Graphics.ReadPostProcessAuthoringRequest(requestId);
	}

	/// <summary>Camera authoring and transient custom controls use separate operations.</summary>
	public sealed class PostProcessCameraAuthoring
	{
		private readonly Camera? _camera;
		private readonly object _binding = new();
		private GraphicsSettingsTarget _target;
		private bool _bound;
		internal PostProcessCameraAuthoring(Camera camera) { _camera = camera; }
		internal PostProcessCameraAuthoring(GraphicsSettingsTarget target) { _target = target; _bound = true; }
		public GraphicsSettingsTarget Target
		{
			get
			{
				lock (_binding)
				{
					// Retry only until the first valid binding. A retired handle never
					// silently adopts a recreated camera or another view lifetime.
					if (!_bound && _camera is not null)
					{
						_target = Graphics.ForCamera(_camera);
						_bound = _target.IsValid;
					}
					return _target;
				}
			}
		}
		public PostProcessCameraAuthoring WithView(uint viewType, uint outputKind, uint outputId, ulong lifetimeGeneration)
			=> new(Target.WithView(viewType, outputKind, outputId, lifetimeGeneration));
		public PostProcessAuthoringReadResult Read() => Graphics.ReadPostProcessAuthoring(Target);
		public GraphicsSettingsReadResult ReadResolvedSettings() => Graphics.ReadSettings(Target);
		/// <summary>Queues an authored Camera.PostProcessProfileId scene change.</summary>
		public PostProcessAuthoringReceipt SetProfile(PostProcessAssetReference assetId, PostProcessAuthoringSnapshot baseline)
			=> Graphics.SubmitPostProcessAuthoring(Target, baseline, "setCameraProfile", writer => {
				if (assetId.IsNone) writer.WriteNull("profileAssetId"); else writer.WriteString("profileAssetId", assetId.AssetId);
			});
		/// <summary>Applies session-owned custom controls without editing profile assets.</summary>
		public PostProcessAuthoringReceipt ApplyCustomEffects(IReadOnlyList<PostProcessCustomEffectPatch> effects, PostProcessAuthoringSnapshot baseline)
			=> Graphics.SubmitPostProcessAuthoring(Target, baseline, "patchCustomEffects", writer => Graphics.WritePostProcessEffects(writer, effects));
		public PostProcessAuthoringReceipt ResetCustomEffects(IReadOnlyList<PostProcessAssetReference> assetIds, PostProcessAuthoringSnapshot baseline, bool clearAll = false)
			=> Graphics.SubmitPostProcessAuthoring(Target, baseline, "resetCustomEffects", writer => {
				ArgumentNullException.ThrowIfNull(assetIds);
				if (assetIds.Count > 64 || (clearAll && assetIds.Count != 0) || (!clearAll && assetIds.Count == 0))
					throw new ArgumentException("Reset either an explicit nonempty set or clearAll with no IDs.", nameof(assetIds));
				var seen = new HashSet<PostProcessAssetReference>();
				writer.WriteStartArray("effectAssetIds");
				foreach (var asset in assetIds)
				{
					if (asset.IsNone || !seen.Add(asset)) throw new ArgumentException("Reset asset IDs must be nonempty and unique.", nameof(assetIds));
					writer.WriteStringValue(asset.AssetId);
				}
				writer.WriteEndArray(); writer.WriteBoolean("clearAll", clearAll);
			});
	}

	// A separate partial file reuses the existing bounded native JSON copier,
	// target decoder and v2 fixed receipt layout without changing v1/v2 graphics.
	public static unsafe partial class Graphics
	{
		private const uint AuthoringJsonVersion = 1;
		private static bool AuthoringBridgeAvailable()
		{
			try { return Native_PostProcessAuthoring_GetAbiVersion() == AuthoringJsonVersion && SettingsAbiVersion == GraphicsSettingsWire.Version; }
			catch (EntryPointNotFoundException) { return false; }
			catch (DllNotFoundException) { return false; }
		}
		private static PostProcessAuthoringReadResult AuthoringUnavailable(string detail) => new() {
			Reason = GraphicsRequestReason.TargetUnavailable, Detail = detail
		};
		private static PostProcessAuthoringReceipt AuthoringRejected(GraphicsRequestReason reason, string detail) => new() {
			Request = new() { State = GraphicsRequestState.Rejected, Reason = reason, Detail = detail }
		};

		internal static PostProcessAuthoringReadResult ReadPostProcessAuthoring(GraphicsSettingsTarget target)
		{
			if (!target.IsCamera || !target.IsValid) return AuthoringUnavailable("No valid camera authoring target is bound.");
			if (!AuthoringBridgeAvailable()) return AuthoringUnavailable("The native camera authoring API is unavailable or incompatible in this build.");
			try
			{
				NativeGraphicsSettingsTargetPacket native = target.Native;
				using JsonDocument? document = ReadCopiedJson((byte* output, uint capacity, out uint required) =>
					Native_PostProcessAuthoring_Read(ref native, output, capacity, out required), out var status);
				if (document is null) return AuthoringUnavailable("Native authoring query: " + status);
				JsonElement root = document.RootElement; CheckAuthoringVersion(root);
				JsonElement life = root.GetProperty("lifecycle");
				return new() {
					Snapshot = new() {
						Target = ReadTarget(root.GetProperty("target")),
						Lifecycle = new() {
							ProjectGeneration = life.GetProperty("projectGeneration").GetUInt64(),
							PlaySessionGeneration = life.GetProperty("playSessionGeneration").GetUInt64(),
							RendererSessionGeneration = life.GetProperty("rendererSessionGeneration").GetUInt64(),
							BackendResourceGeneration = life.GetProperty("backendResourceGeneration").GetUInt64()
						},
						Revision = root.GetProperty("revision").GetUInt64(),
						AuthoredProfileAssetId = ReadAuthoringAsset(root.GetProperty("authoredProfileAssetId"), true),
						SceneVisibleFrameId = root.GetProperty("visibleFrameId").GetUInt64(),
						EffectiveFrameId = root.GetProperty("effectiveFrameId").GetUInt64(),
						Descriptors = root.GetProperty("descriptors").EnumerateArray().Select(ReadAuthoringDescriptor).ToArray(),
						RequestedEffects = ReadAuthoringStates(root.GetProperty("requestedEffects")),
						EffectiveEffects = ReadAuthoringStates(root.GetProperty("effectiveEffects"))
					},
					Reason = (GraphicsRequestReason)root.GetProperty("reason").GetUInt32(),
					Detail = root.GetProperty("detail").GetString() ?? string.Empty
				};
			}
			catch (EntryPointNotFoundException) { return AuthoringUnavailable("Native authoring query is not installed."); }
			catch (DllNotFoundException) { return AuthoringUnavailable("Native authoring library is not loaded."); }
		}

		internal static PostProcessAuthoringReceipt? ReadPostProcessAuthoringRequest(ulong requestId)
		{
			if (requestId == 0 || !AuthoringBridgeAvailable()) return null;
			try
			{
				using JsonDocument? document = ReadCopiedJson((byte* output, uint capacity, out uint required) =>
					Native_PostProcessAuthoring_ReadReceipt(requestId, output, capacity, out required), out _);
				if (document is null) return null;
				JsonElement root = document.RootElement; CheckAuthoringVersion(root);
				JsonElement request = root.GetProperty("request");
				return new() {
					Request = new() {
						RequestId = request.GetProperty("requestId").GetUInt64(), Sequence = request.GetProperty("sequence").GetUInt64(),
						State = (GraphicsRequestState)request.GetProperty("state").GetUInt32(), Activation = (GraphicsActivation)request.GetProperty("activation").GetUInt32(),
						Reason = (GraphicsRequestReason)request.GetProperty("reason").GetUInt32(), Detail = request.GetProperty("detail").GetString() ?? string.Empty,
						RequestedRevision = request.GetProperty("requestedRevision").GetUInt64(), EffectiveRevision = request.GetProperty("effectiveRevision").GetUInt64(),
						AppliedFrameId = request.GetProperty("appliedFrameId").GetUInt64()
					},
					SceneVisibleFrameId = root.GetProperty("sceneVisibleFrameId").GetUInt64(),
					EffectiveFrameId = root.GetProperty("effectiveFrameId").GetUInt64()
				};
			}
			catch (EntryPointNotFoundException) { return null; }
			catch (DllNotFoundException) { return null; }
		}

		internal static PostProcessAuthoringReceipt SubmitPostProcessAuthoring(GraphicsSettingsTarget handle,
			PostProcessAuthoringSnapshot baseline, string operation, Action<Utf8JsonWriter> writeOperation)
		{
			ArgumentNullException.ThrowIfNull(baseline);
			if (!SameAuthoringOwner(handle, baseline.Target) || baseline.Revision == 0 || baseline.Lifecycle.ProjectGeneration == 0
				|| baseline.Lifecycle.RendererSessionGeneration == 0 || baseline.Lifecycle.BackendResourceGeneration == 0)
				return AuthoringRejected(GraphicsRequestReason.StaleLifetime, "The authoring baseline does not match this camera/view lifetime.");
			// Prepare and validate the complete copied payload before crossing native.
			// A malformed late parameter can never partially admit an earlier effect.
			byte[] bytes = BuildPostProcessAuthoringPayload(baseline, operation, writeOperation);
			if (!AuthoringBridgeAvailable()) return AuthoringRejected(GraphicsRequestReason.TargetUnavailable, "The native authoring API is unavailable or incompatible in this build.");
			NativeGraphicsSettingsTargetPacket target = baseline.Target.Native;
			var receipt = EmptyReceipt();
			try
			{
				fixed (byte* pointer = bytes)
				{
					var status = Native_PostProcessAuthoring_Submit(ref target, pointer, checked((uint)bytes.Length), ref receipt);
					return MapPostProcessAdmission(receipt, status);
				}
			}
			catch (EntryPointNotFoundException) { return AuthoringRejected(GraphicsRequestReason.TargetUnavailable, "Native authoring submission is not installed."); }
			catch (DllNotFoundException) { return AuthoringRejected(GraphicsRequestReason.TargetUnavailable, "Native authoring library is not loaded."); }
		}

		private static byte[] BuildPostProcessAuthoringPayload(PostProcessAuthoringSnapshot baseline, string operation, Action<Utf8JsonWriter> writeOperation)
		{
			var output = new ArrayBufferWriter<byte>();
			using (var writer = new Utf8JsonWriter(output))
			{
				writer.WriteStartObject(); writer.WriteNumber("abiVersion", AuthoringJsonVersion); writer.WriteString("operation", operation);
				writer.WriteNumber("baseRevision", baseline.Revision); writer.WriteStartObject("lifecycle");
				writer.WriteNumber("projectGeneration", baseline.Lifecycle.ProjectGeneration);
				writer.WriteNumber("playSessionGeneration", baseline.Lifecycle.PlaySessionGeneration);
				writer.WriteNumber("rendererSessionGeneration", baseline.Lifecycle.RendererSessionGeneration);
				writer.WriteNumber("backendResourceGeneration", baseline.Lifecycle.BackendResourceGeneration);
				writer.WriteEndObject(); writeOperation(writer); writer.WriteEndObject();
			}
			if (output.WrittenCount > GraphicsSettingsWire.MaximumBytes) throw new ArgumentOutOfRangeException(nameof(writeOperation), "Authoring payload exceeds1MiB.");
			return output.WrittenSpan.ToArray();
		}
		private static PostProcessAuthoringReceipt MapPostProcessAdmission(NativeGraphicsSettingsReceiptPacket receipt, NativeGraphicsSettingsStatus status)
		{
			GraphicsRequestReceipt request = FromReceipt(receipt, status);
			if ((status == NativeGraphicsSettingsStatus.Success || status == NativeGraphicsSettingsStatus.Accepted) && request.RequestId == 0)
				return AuthoringRejected(GraphicsRequestReason.PreparationFailed, "Native admission returned no request identity.");
			if (status != NativeGraphicsSettingsStatus.Success && status != NativeGraphicsSettingsStatus.Accepted)
				return new() { Request = new() {
					RequestId = request.RequestId, Sequence = request.Sequence, State = GraphicsRequestState.Rejected,
					Activation = request.Activation, Reason = request.Reason, AffectedSettings = request.AffectedSettings,
					Detail = request.Detail.Length != 0 ? request.Detail : status.ToString(), RequestedRevision = request.RequestedRevision,
					EffectiveRevision = request.EffectiveRevision, AppliedFrameId = request.AppliedFrameId
				} };
			// Fixed admission fields supply no separate scene/render proof.
			return new() { Request = request };
		}

		private static bool SameAuthoringOwner(GraphicsSettingsTarget handle, GraphicsSettingsTarget baseline)
		{
			if (!handle.IsCamera || !handle.IsValid || !baseline.IsCamera || !baseline.IsValid) return false;
			var a = handle.Native; var b = baseline.Native;
			return a.CameraId.First == b.CameraId.First && a.CameraId.Second == b.CameraId.Second
				&& a.SceneGeneration == b.SceneGeneration && a.ObjectLifetimeGeneration == b.ObjectLifetimeGeneration
				&& a.ComponentGeneration == b.ComponentGeneration && a.BindingGeneration == b.BindingGeneration
				&& (!handle.HasView || (baseline.HasView && a.ViewType == b.ViewType && a.OutputKind == b.OutputKind
					&& a.OutputId == b.OutputId && a.ViewLifetimeGeneration == b.ViewLifetimeGeneration));
		}
		private static void CheckAuthoringVersion(JsonElement value)
		{
			if (value.GetProperty("abiVersion").GetUInt32() != AuthoringJsonVersion) throw new InvalidOperationException("Authoring JSON ABI mismatch.");
		}
		private static PostProcessAssetReference ReadAuthoringAsset(JsonElement value, bool optional = false)
		{
			if (optional && (value.ValueKind == JsonValueKind.Null || value.GetString() == new string('0', 32))) return default;
			return new(value.GetString() ?? throw new JsonException("Missing authoring asset UUID."));
		}
		private static PostProcessParameterKind ReadAuthoringKind(string? value) => value switch {
			"boolean" => PostProcessParameterKind.Boolean, "signed" or "signed32" => PostProcessParameterKind.Signed, "unsigned" or "unsigned32" => PostProcessParameterKind.Unsigned,
			"scalar" => PostProcessParameterKind.Scalar, "vector2" => PostProcessParameterKind.Vector2, "vector3" => PostProcessParameterKind.Vector3,
			"vector4" => PostProcessParameterKind.Vector4, _ => throw new JsonException("Unknown custom parameter kind.")
		};
		private static PostProcessParameterValue ReadAuthoringValue(string name, PostProcessParameterKind kind, JsonElement value)
		{
			if (kind is PostProcessParameterKind.Vector2 or PostProcessParameterKind.Vector3 or PostProcessParameterKind.Vector4)
			{
				int count = kind == PostProcessParameterKind.Vector2 ? 2 : kind == PostProcessParameterKind.Vector3 ? 3 : 4;
				if (value.ValueKind != JsonValueKind.Array || value.GetArrayLength() != count) throw new JsonException("Custom vector width mismatch.");
				var vector = value.EnumerateArray().Select(item => item.GetSingle()).ToArray();
				return count == 2 ? PostProcessParameterValue.Vector2(name, new(vector[0], vector[1]))
					: count == 3 ? PostProcessParameterValue.Vector3(name, new(vector[0], vector[1], vector[2]))
					: PostProcessParameterValue.Vector4(name, new(vector[0], vector[1], vector[2], vector[3]));
			}
			return kind switch {
				PostProcessParameterKind.Boolean => PostProcessParameterValue.Boolean(name, value.GetBoolean()),
				PostProcessParameterKind.Signed => PostProcessParameterValue.Signed(name, value.GetInt32()),
				PostProcessParameterKind.Unsigned => PostProcessParameterValue.Unsigned(name, value.GetUInt32()),
				PostProcessParameterKind.Scalar => PostProcessParameterValue.Scalar(name, value.GetSingle()),
				_ => throw new JsonException("Unknown parameter value kind.")
			};
		}
		private static PostProcessEffectState[] ReadAuthoringStates(JsonElement values) => values.EnumerateArray().Select(effect => new PostProcessEffectState {
			AssetId = ReadAuthoringAsset(effect.GetProperty("assetId")), Enabled = effect.GetProperty("enabled").GetBoolean(),
			Parameters = effect.GetProperty("parameters").EnumerateArray().Select(parameter => ReadAuthoringValue(parameter.GetProperty("name").GetString() ?? string.Empty,
				ReadAuthoringKind(parameter.GetProperty("kind").GetString()), parameter.GetProperty("value"))).ToArray()
		}).ToArray();
		private static PostProcessEffectDescriptor ReadAuthoringDescriptor(JsonElement value) => new() {
			AssetId = ReadAuthoringAsset(value.GetProperty("assetId")), ShaderAssetId = ReadAuthoringAsset(value.GetProperty("shaderAssetId")),
			Revision = ulong.Parse(value.GetProperty("revision").GetString() ?? string.Empty, System.Globalization.CultureInfo.InvariantCulture),
			StableId = value.GetProperty("stableId").GetString() ?? string.Empty, EntryPoint = value.GetProperty("entryPoint").GetString() ?? string.Empty,
			Stage = value.GetProperty("stage").GetString() switch { "hdr-scene" => PostProcessInsertionPoint.HdrScene, "display-image" => PostProcessInsertionPoint.DisplayImage,
				"before-ui" => PostProcessInsertionPoint.BeforeUI, _ => throw new JsonException("Unknown insertion point.") },
			Domain = value.GetProperty("domain").GetString() switch { "linear-hdr" => PostProcessColorDomain.LinearHdr, "display-linear" => PostProcessColorDomain.DisplayLinear,
				_ => throw new JsonException("Unknown custom color domain.") },
			ParameterBytes = value.GetProperty("parameterBytes").GetUInt32(),
			RequiredCapabilities = ulong.Parse(value.GetProperty("requiredCapabilities").GetString() ?? string.Empty, System.Globalization.CultureInfo.InvariantCulture),
			Inputs = value.GetProperty("inputs").EnumerateArray().Select(input => new PostProcessInputDescriptor {
				Kind = input.GetProperty("kind").GetString() switch { "source-color" => PostProcessInputKind.SourceColor, "depth" => PostProcessInputKind.Depth,
					"normals" => PostProcessInputKind.Normals, "roughness" => PostProcessInputKind.Roughness, "motion" => PostProcessInputKind.Motion,
					"asset-texture" => PostProcessInputKind.AssetTexture, _ => throw new JsonException("Unknown custom input semantic.") },
				Binding = input.GetProperty("binding").GetUInt32(), CombinedSampler = input.GetProperty("combinedSampler").GetBoolean(), AssetId = ReadAuthoringAsset(input.GetProperty("assetId"), true)
			}).ToArray(),
			Parameters = value.GetProperty("parameters").EnumerateArray().Select(parameter => {
				string name = parameter.GetProperty("name").GetString() ?? string.Empty; var kind = ReadAuthoringKind(parameter.GetProperty("kind").GetString());
				return new PostProcessParameterDescriptor { Name = name, Kind = kind, OffsetBytes = parameter.GetProperty("offsetBytes").GetUInt32(),
					DefaultValue = ReadAuthoringValue(name, kind, parameter.GetProperty("default")),
					Minimum = parameter.GetProperty("minimum").ValueKind == JsonValueKind.Null ? null : ReadAuthoringValue(name, kind, parameter.GetProperty("minimum")),
					Maximum = parameter.GetProperty("maximum").ValueKind == JsonValueKind.Null ? null : ReadAuthoringValue(name, kind, parameter.GetProperty("maximum")) };
			}).ToArray(),
			AssetDependencies = value.GetProperty("assetDependencies").EnumerateArray().Select(asset => ReadAuthoringAsset(asset)).ToArray(),
			After = value.GetProperty("after").EnumerateArray().Select(item => item.GetString() ?? string.Empty).ToArray(),
			Available = value.GetProperty("available").GetBoolean(), Reason = (GraphicsRequestReason)value.GetProperty("reason").GetUInt32(), Detail = value.GetProperty("detail").GetString() ?? string.Empty
		};

		internal static void WritePostProcessEffects(Utf8JsonWriter writer, IReadOnlyList<PostProcessCustomEffectPatch> effects)
		{
			ArgumentNullException.ThrowIfNull(effects);
			if (effects.Count == 0 || effects.Count > 64) throw new ArgumentOutOfRangeException(nameof(effects));
			var assets = new HashSet<PostProcessAssetReference>(); writer.WriteStartArray("effects");
			foreach (var effect in effects)
			{
				ArgumentNullException.ThrowIfNull(effect); ArgumentNullException.ThrowIfNull(effect.Parameters); ArgumentNullException.ThrowIfNull(effect.ResetParameters);
				if (effect.AssetId.IsNone || !assets.Add(effect.AssetId) || effect.Parameters.Count > 32 || effect.ResetParameters.Count > 32
					|| (!effect.Enabled.HasValue && effect.Parameters.Count == 0 && effect.ResetParameters.Count == 0)) throw new ArgumentException("Invalid, duplicate or empty custom effect patch.", nameof(effects));
				writer.WriteStartObject(); writer.WriteString("assetId", effect.AssetId.AssetId);
				if (effect.Enabled.HasValue) writer.WriteBoolean("enabled", effect.Enabled.Value);
				var parameters = new HashSet<string>(StringComparer.Ordinal); writer.WriteStartArray("parameters");
				foreach (var value in effect.Parameters)
				{
					if (!parameters.Add(value.Name ?? string.Empty)) throw new ArgumentException("Duplicate custom parameter.", nameof(effects));
					WriteAuthoringValue(writer, value);
				}
				writer.WriteEndArray(); writer.WriteStartArray("resetParameters");
				foreach (string name in effect.ResetParameters)
				{
					_ = PostProcessParameterValue.Boolean(name, false); // Reuse exact name validation.
					if (!parameters.Add(name)) throw new ArgumentException("A parameter cannot be patched/reset twice.", nameof(effects));
					writer.WriteStringValue(name);
				}
				writer.WriteEndArray(); writer.WriteEndObject();
			}
			writer.WriteEndArray();
		}
		private static void WriteAuthoringValue(Utf8JsonWriter writer, PostProcessParameterValue value)
		{
			_ = PostProcessParameterValue.Boolean(value.Name, false); // Default/uninitialized values reject here.
			string kind = value.Kind switch { PostProcessParameterKind.Boolean => "boolean", PostProcessParameterKind.Signed => "signed", PostProcessParameterKind.Unsigned => "unsigned",
				PostProcessParameterKind.Scalar => "scalar", PostProcessParameterKind.Vector2 => "vector2", PostProcessParameterKind.Vector3 => "vector3", PostProcessParameterKind.Vector4 => "vector4",
				_ => throw new ArgumentException("Unknown custom parameter kind.") };
			writer.WriteStartObject(); writer.WriteString("name", value.Name); writer.WriteString("kind", kind); writer.WritePropertyName("value");
			switch (value.Kind)
			{
				case PostProcessParameterKind.Boolean: writer.WriteBooleanValue((bool)value.Value); break;
				case PostProcessParameterKind.Signed: writer.WriteNumberValue((int)value.Value); break;
				case PostProcessParameterKind.Unsigned: writer.WriteNumberValue((uint)value.Value); break;
				case PostProcessParameterKind.Scalar: writer.WriteNumberValue((float)value.Value); break;
				case PostProcessParameterKind.Vector2:
					var a = (Vector2)value.Value; writer.WriteStartArray(); writer.WriteNumberValue(a.X); writer.WriteNumberValue(a.Y); writer.WriteEndArray(); break;
				case PostProcessParameterKind.Vector3:
					var b = (Vector3)value.Value; writer.WriteStartArray(); writer.WriteNumberValue(b.X); writer.WriteNumberValue(b.Y); writer.WriteNumberValue(b.Z); writer.WriteEndArray(); break;
				case PostProcessParameterKind.Vector4:
					var c = (Vector4)value.Value; writer.WriteStartArray(); writer.WriteNumberValue(c.X); writer.WriteNumberValue(c.Y); writer.WriteNumberValue(c.Z); writer.WriteNumberValue(c.W); writer.WriteEndArray(); break;
			}
			writer.WriteEndObject();
		}

		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)] private static extern uint Native_PostProcessAuthoring_GetAbiVersion();
		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)] private static extern NativeGraphicsSettingsStatus Native_PostProcessAuthoring_Read(ref NativeGraphicsSettingsTargetPacket target, byte* output, uint capacity, out uint required);
		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)] private static extern NativeGraphicsSettingsStatus Native_PostProcessAuthoring_ReadReceipt(ulong requestId, byte* output, uint capacity, out uint required);
		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)] private static extern NativeGraphicsSettingsStatus Native_PostProcessAuthoring_Submit(ref NativeGraphicsSettingsTargetPacket target, byte* payload, uint bytes, ref NativeGraphicsSettingsReceiptPacket receipt);
	}
}
