// GraphicsSettingsApi.cs - Copied settings discovery and transactional requests.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace TerraEngine
{
	public static unsafe partial class Graphics
	{
		private static readonly UTF8Encoding SettingsUtf8 = new(false, true);
		private unsafe delegate NativeGraphicsSettingsStatus CopySettingsJson(byte* destination, uint capacity, out uint required);
		public static uint SettingsAbiVersion => Native_GraphicsSettings_GetAbiVersion();

		public static GraphicsSettingsTarget ForCamera(Camera camera)
		{
			ArgumentNullException.ThrowIfNull(camera);
			return camera.GetGraphicsSettingsTarget();
		}

		public static GraphicsSettingDescriptor[] DiscoverSettings(GraphicsSettingsTarget target = default)
		{
			// Resolve availability for this exact copied camera/view lifetime. The
			// native owner controls capability masks; managed code only filters scope.
			NativeGraphicsSettingsTargetPacket native = target.Native;
			using JsonDocument document = ReadCopiedJson(
				(byte* output, uint capacity, out uint required) => Native_GraphicsSettings_DiscoverScoped(ref native, output, capacity, out required), out var status)
				?? throw new InvalidOperationException("Graphics discovery is unavailable: " + status);
			JsonElement root = document.RootElement;
			if (root.GetProperty("abiVersion").GetUInt32() != GraphicsSettingsWire.Version)
				throw new InvalidOperationException("Graphics discovery returned an incompatible ABI version.");
			var result = new List<GraphicsSettingDescriptor>();
			foreach (JsonElement setting in root.GetProperty("settings").EnumerateArray())
			{
				GraphicsValueKind kind = (GraphicsValueKind)setting.GetProperty("kind").GetUInt32();
				bool cameraScope = setting.GetProperty("cameraScope").GetBoolean();
				if (target.IsCamera && !cameraScope) continue;
				result.Add(new() {
					Id = (GraphicsSettingId)setting.GetProperty("id").GetUInt32(),
					Name = setting.GetProperty("name").GetString() ?? string.Empty,
					Units = setting.GetProperty("units").GetString() ?? string.Empty,
					ValueKind = kind,
					DefaultValue = ReadValue(kind, setting.GetProperty("defaultValue")),
					Minimum = ReadOptionalValue(kind, setting.GetProperty("minimum")),
					Maximum = ReadOptionalValue(kind, setting.GetProperty("maximum")),
					Options = setting.GetProperty("options").EnumerateArray().Select(value => ReadValue(kind, value)).ToArray(),
					GameScope = setting.GetProperty("gameScope").GetBoolean(),
					CameraScope = cameraScope,
					AuthoringScope = (GraphicsAuthoringScope)setting.GetProperty("authoringScope").GetUInt32(),
					Activation = (GraphicsActivation)setting.GetProperty("activation").GetUInt32(),
					Available = setting.GetProperty("available").GetBoolean(),
					UnavailableReason = (GraphicsRequestReason)setting.GetProperty("unavailableReason").GetUInt32(),
					Detail = setting.GetProperty("detail").GetString() ?? string.Empty
				});
			}
			return result.ToArray();
		}

		public static GraphicsSettingsReadResult ReadSettings(GraphicsSettingsTarget target = default)
		{
			if (target.IsCamera && !target.IsValid)
				return new() { Reason = GraphicsRequestReason.TargetUnavailable, Detail = "The camera has no valid published graphics binding." };
			NativeGraphicsSettingsTargetPacket native = target.Native;
			using JsonDocument? document = ReadCopiedJson(
				(byte* output, uint capacity, out uint required) => Native_GraphicsSettings_Read(ref native, output, capacity, out required), out var status);
			if (document is null) return new() { Reason = StatusReason(status), Detail = status.ToString() };
			JsonElement root = document.RootElement;
			GraphicsSettingsSnapshot? effective = ReadSnapshot(root.GetProperty("effective"));
			return new() {
				Snapshot = effective,
				Requested = ReadSnapshot(root.GetProperty("requested")),
				Reason = effective is null ? GraphicsRequestReason.TargetUnavailable : GraphicsRequestReason.None,
				Detail = effective is null ? "No effective settings are published for this target." : string.Empty
			};
		}

		public static GraphicsRequestReceipt? ReadRequest(ulong requestId)
		{
			if (requestId == 0U) return null;
			using JsonDocument? document = ReadCopiedJson(
				(byte* output, uint capacity, out uint required) => Native_GraphicsSettings_ReadReceipt(requestId, output, capacity, out required), out _);
			if (document is null) return null;
			JsonElement value = document.RootElement;
			return new() {
				RequestId = value.GetProperty("requestId").GetUInt64(),
				Sequence = value.GetProperty("sequence").GetUInt64(),
				State = (GraphicsRequestState)value.GetProperty("state").GetUInt32(),
				Activation = (GraphicsActivation)value.GetProperty("activation").GetUInt32(),
				Reason = (GraphicsRequestReason)value.GetProperty("reason").GetUInt32(),
				AffectedSettings = value.GetProperty("affectedSettings").EnumerateArray().Select(item => (GraphicsSettingId)item.GetUInt32()).ToArray(),
				Detail = value.GetProperty("detail").GetString() ?? string.Empty,
				RequestedRevision = value.GetProperty("requestedRevision").GetUInt64(),
				EffectiveRevision = value.GetProperty("effectiveRevision").GetUInt64(),
				AppliedFrameId = value.GetProperty("appliedFrameId").GetUInt64()
			};
		}

		public static GraphicsRequestReceipt ApplySettings(ReadOnlySpan<GraphicsSettingValue> values,
			GraphicsSettingsTarget target = default, ulong expectedRevision = 0U)
			=> SubmitSettings(0U, values, target, expectedRevision);

		public static GraphicsRequestReceipt ResetSettings(ReadOnlySpan<GraphicsSettingId> ids,
			GraphicsSettingsTarget target = default, ulong expectedRevision = 0U)
		{
			if (ids.Length > GraphicsSettingsWire.MaximumValues) throw new ArgumentOutOfRangeException(nameof(ids));
			var values = new GraphicsSettingValue[ids.Length];
			for (int index = 0; index < ids.Length; ++index) values[index] = new(ids[index], false);
			return SubmitSettings(1U, values, target, expectedRevision);
		}

		public static GraphicsRequestReceipt ApplyPreset(QualityPreset preset,
			GraphicsSettingsTarget target = default, ulong expectedRevision = 0U)
			=> SubmitSettings(2U, ReadOnlySpan<GraphicsSettingValue>.Empty, target, expectedRevision, (uint)preset);

		public static GraphicsRequestReceipt StageNextLaunch(ReadOnlySpan<GraphicsSettingValue> values, ulong expectedRevision = 0U)
			=> SubmitSettings(3U, values, default, expectedRevision);

		public static GraphicsRequestReceipt SavePreferences(string name, GraphicsSettingsTarget target = default,
			ulong expectedStoreRevision = 0U)
		{
			byte[] text = EncodePreferenceName(name);
			NativeGraphicsSettingsTargetPacket native = target.Native;
			var receipt = EmptyReceipt();
			fixed (byte* pointer = text)
			{
				var status = Native_GraphicsSettings_SavePreferences(pointer, checked((uint)text.Length),
					ref native, expectedStoreRevision, ref receipt);
				return FromReceipt(receipt, status);
			}
		}

		public static GraphicsRequestReceipt RestorePreferences(string name, GraphicsSettingsTarget target = default)
		{
			byte[] text = EncodePreferenceName(name);
			NativeGraphicsSettingsTargetPacket native = target.Native;
			var receipt = EmptyReceipt();
			fixed (byte* pointer = text)
			{
				var status = Native_GraphicsSettings_RestorePreferences(pointer, checked((uint)text.Length), ref native, ref receipt);
				return FromReceipt(receipt, status);
			}
		}

		public static GraphicsRequestReceipt CancelStaged(ulong requestId)
		{
			var receipt = EmptyReceipt();
			var status = Native_GraphicsSettings_CancelStaged(requestId, ref receipt);
			return FromReceipt(receipt, status);
		}

		internal static bool TryResolveGraphicsCamera(uint entity, out GraphicsSettingsTarget target)
		{
			NativeGraphicsSettingsTargetPacket native = default;
			var status = Native_GraphicsSettings_ResolveCamera(entity, ref native);
			if (status == NativeGraphicsSettingsStatus.VersionMismatch) throw new InvalidOperationException("Graphics camera ABI mismatch.");
			target = new(native);
			return status == NativeGraphicsSettingsStatus.Success && target.IsCamera && target.IsValid;
		}

		private static GraphicsRequestReceipt SubmitSettings(uint operation, ReadOnlySpan<GraphicsSettingValue> values,
			GraphicsSettingsTarget target, ulong expectedRevision, uint preset = 0U)
		{
			if (values.Length > GraphicsSettingsWire.MaximumValues) throw new ArgumentOutOfRangeException(nameof(values));
			var records = new NativeGraphicsSettingsValuePacket[values.Length];
			var text = new List<byte>();
			for (int index = 0; index < values.Length; ++index)
			{
				GraphicsSettingValue value = values[index];
				if (value.Value is null) throw new ArgumentException("An uninitialized graphics setting has no typed value.", nameof(values));
				ref NativeGraphicsSettingsValuePacket record = ref records[index];
				record.SettingId = (uint)value.Id;
				record.ValueKind = (uint)value.ValueKind;
				switch (value.ValueKind)
				{
					case GraphicsValueKind.Boolean: record.ScalarBits = (bool)value.Value ? 1U : 0U; break;
					case GraphicsValueKind.Unsigned32: record.ScalarBits = (uint)value.Value; break;
					case GraphicsValueKind.Unsigned64: record.ScalarBits = (ulong)value.Value; break;
					case GraphicsValueKind.Scalar: record.ScalarBits = BitConverter.SingleToUInt32Bits((float)value.Value); break;
					case GraphicsValueKind.Asset: record.AssetId = (PhysicsUuid)value.Value; break;
					case GraphicsValueKind.String:
						string content = (string)value.Value;
						int bytes = SettingsUtf8.GetByteCount(content);
						if (bytes > GraphicsSettingsWire.MaximumBytes - text.Count) throw new ArgumentOutOfRangeException(nameof(values), "Graphics text exceeds the copied buffer limit.");
						record.TextOffset = checked((uint)text.Count);
						record.TextLength = checked((uint)bytes);
						text.AddRange(SettingsUtf8.GetBytes(content));
						break;
					default: throw new ArgumentException("Unknown graphics value type.", nameof(values));
				}
			}
			byte[] utf8 = text.ToArray();
			var request = new NativeGraphicsSettingsRequestPacket {
				Header = GraphicsSettingsWire.Header<NativeGraphicsSettingsRequestPacket>(),
				Operation = operation, ValueCount = checked((uint)records.Length), TextBytes = checked((uint)utf8.Length),
				Preset = preset, ExpectedRevision = expectedRevision, Target = target.Native
			};
			var receipt = EmptyReceipt();
			fixed (NativeGraphicsSettingsValuePacket* recordPointer = records)
			fixed (byte* textPointer = utf8)
			{
				var status = Native_GraphicsSettings_Submit(ref request, recordPointer, textPointer, ref receipt);
				return FromReceipt(receipt, status);
			}
		}

		private static byte[] EncodePreferenceName(string name)
		{
			ArgumentException.ThrowIfNullOrWhiteSpace(name);
			int count = SettingsUtf8.GetByteCount(name);
			if (count > GraphicsSettingsWire.MaximumBytes || name.Contains('\0')) throw new ArgumentException("Invalid graphics preference name.", nameof(name));
			return SettingsUtf8.GetBytes(name);
		}

		private static NativeGraphicsSettingsReceiptPacket EmptyReceipt() => new()
		{
			Header = GraphicsSettingsWire.Header<NativeGraphicsSettingsReceiptPacket>()
		};

		private static GraphicsRequestReceipt FromReceipt(NativeGraphicsSettingsReceiptPacket value, NativeGraphicsSettingsStatus status)
		{
			if (status == NativeGraphicsSettingsStatus.VersionMismatch) throw new InvalidOperationException("Graphics request ABI mismatch.");
			if (value.Header.AbiVersion != GraphicsSettingsWire.Version
				|| value.Header.StructSize != Marshal.SizeOf<NativeGraphicsSettingsReceiptPacket>())
				throw new InvalidOperationException("Graphics returned an incompatible receipt layout.");
			// Admission returns fixed fields. Detailed text and affected IDs are
			// available from the later copied receipt query without another mutation.
			return new() {
				RequestId = value.RequestId, Sequence = value.Sequence,
				State = status is NativeGraphicsSettingsStatus.InvalidArgument or NativeGraphicsSettingsStatus.Unavailable
					? GraphicsRequestState.Rejected : (GraphicsRequestState)value.State,
				Activation = (GraphicsActivation)value.Activation,
				Reason = value.Reason != 0U ? (GraphicsRequestReason)value.Reason : StatusReason(status),
				RequestedRevision = value.RequestedRevision, EffectiveRevision = value.EffectiveRevision,
				AppliedFrameId = value.AppliedFrameId
			};
		}

		private static GraphicsRequestReason StatusReason(NativeGraphicsSettingsStatus status) => status switch {
			NativeGraphicsSettingsStatus.Success or NativeGraphicsSettingsStatus.Accepted => GraphicsRequestReason.None,
			NativeGraphicsSettingsStatus.Unavailable => GraphicsRequestReason.TargetUnavailable,
			_ => GraphicsRequestReason.InvalidValue
		};

		private static JsonDocument? ReadCopiedJson(CopySettingsJson copy, out NativeGraphicsSettingsStatus status)
		{
			status = copy(null, 0U, out uint required);
			for (int attempt = 0; attempt < 3; ++attempt)
			{
				if (status == NativeGraphicsSettingsStatus.VersionMismatch) throw new InvalidOperationException("Graphics query ABI mismatch.");
				if (status != NativeGraphicsSettingsStatus.BufferTooSmall && status != NativeGraphicsSettingsStatus.Success) return null;
				if (required == 0U || required > GraphicsSettingsWire.MaximumBytes) throw new InvalidOperationException("Graphics query returned an invalid buffer bound.");
				var bytes = new byte[checked((int)required)];
				fixed (byte* pointer = bytes) status = copy(pointer, checked((uint)bytes.Length), out required);
				if (status == NativeGraphicsSettingsStatus.BufferTooSmall) continue;
				if (status != NativeGraphicsSettingsStatus.Success) return null;
				if (required == 0U || required > bytes.Length || bytes[required - 1U] != 0U)
					throw new InvalidOperationException("Graphics query did not return a terminated copied document.");
				// JsonDocument reads the exact owned byte range. Parsing uses explicit
				// fields below, so NativeAOT needs no reflection-based serializers.
				return JsonDocument.Parse(bytes.AsMemory(0, checked((int)required - 1)));
			}
			throw new InvalidOperationException("Graphics query kept growing during bounded copying.");
		}

		private static object? ReadOptionalValue(GraphicsValueKind kind, JsonElement value)
			=> value.ValueKind == JsonValueKind.Null ? null : ReadValue(kind, value);
		private static object ReadValue(GraphicsValueKind kind, JsonElement value) => kind switch {
			GraphicsValueKind.Boolean => value.GetBoolean(),
			GraphicsValueKind.Unsigned32 => value.GetUInt32(),
			GraphicsValueKind.Unsigned64 => value.GetUInt64(),
			GraphicsValueKind.Scalar => value.GetSingle(),
			GraphicsValueKind.String => value.GetString() ?? string.Empty,
			GraphicsValueKind.Asset => GraphicsSettingsWire.ParseUuid(value.GetString() ?? string.Empty),
			_ => throw new JsonException("Unknown graphics value kind.")
		};

		private static GraphicsSettingsSnapshot? ReadSnapshot(JsonElement value)
		{
			if (value.ValueKind == JsonValueKind.Null) return null;
			JsonElement lifecycle = value.GetProperty("lifecycle");
			return new() {
				Target = ReadTarget(value.GetProperty("target")),
				Lifecycle = new() {
					ProjectGeneration = lifecycle.GetProperty("projectGeneration").GetUInt64(),
					PlaySessionGeneration = lifecycle.GetProperty("playSessionGeneration").GetUInt64(),
					RendererSessionGeneration = lifecycle.GetProperty("rendererSessionGeneration").GetUInt64(),
					BackendResourceGeneration = lifecycle.GetProperty("backendResourceGeneration").GetUInt64()
				},
				Revision = value.GetProperty("revision").GetUInt64(),
				PublishedFrameId = value.GetProperty("publishedFrameId").GetUInt64(),
				Preset = value.GetProperty("preset").ValueKind == JsonValueKind.Null ? null : (QualityPreset)value.GetProperty("preset").GetUInt32(),
				Values = value.GetProperty("values").EnumerateArray().Select(item => {
					GraphicsValueKind kind = (GraphicsValueKind)item.GetProperty("kind").GetUInt32();
					return new GraphicsSettingValue((GraphicsSettingId)item.GetProperty("id").GetUInt32(), kind, ReadValue(kind, item.GetProperty("value")));
				}).ToArray(),
				InternalWidth = value.GetProperty("internalWidth").GetUInt32(),
				InternalHeight = value.GetProperty("internalHeight").GetUInt32(),
				OutputWidth = value.GetProperty("outputWidth").GetUInt32(),
				OutputHeight = value.GetProperty("outputHeight").GetUInt32(),
				SampleCount = value.GetProperty("sampleCount").GetUInt32(),
				PostProcessResourceGeneration = value.GetProperty("postProcessResourceGeneration").GetUInt64()
			};
		}

		private static GraphicsSettingsTarget ReadTarget(JsonElement value)
		{
			var target = new NativeGraphicsSettingsTargetPacket { Scope = value.GetProperty("scope").GetUInt32() };
			JsonElement camera = value.GetProperty("camera");
			if (camera.ValueKind != JsonValueKind.Null)
			{
				target.Flags |= 1U;
				target.CameraId = GraphicsSettingsWire.ParseUuid(camera.GetProperty("cameraId").GetString() ?? string.Empty);
				target.SceneGeneration = camera.GetProperty("sceneGeneration").GetUInt64();
				target.ObjectLifetimeGeneration = camera.GetProperty("objectLifetimeGeneration").GetUInt64();
				target.ComponentGeneration = camera.GetProperty("componentGeneration").GetUInt64();
				target.BindingGeneration = camera.GetProperty("bindingGeneration").GetUInt64();
			}
			JsonElement view = value.GetProperty("view");
			if (view.ValueKind != JsonValueKind.Null)
			{
				target.Flags |= 2U;
				target.ViewType = view.GetProperty("viewType").GetUInt32();
				target.OutputKind = view.GetProperty("outputKind").GetUInt32();
				target.OutputId = view.GetProperty("outputId").GetUInt32();
				target.ViewLifetimeGeneration = view.GetProperty("lifetimeGeneration").GetUInt64();
			}
			return new(target);
		}

		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)]
		private static extern uint Native_GraphicsSettings_GetAbiVersion();
		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)]
		private static extern NativeGraphicsSettingsStatus Native_GraphicsSettings_Discover(byte* destination, uint capacity, out uint required);
		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)]
		private static extern NativeGraphicsSettingsStatus Native_GraphicsSettings_DiscoverScoped(ref NativeGraphicsSettingsTargetPacket target,
			byte* destination, uint capacity, out uint required);
		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)]
		private static extern NativeGraphicsSettingsStatus Native_GraphicsSettings_Read(ref NativeGraphicsSettingsTargetPacket target, byte* destination, uint capacity, out uint required);
		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)]
		private static extern NativeGraphicsSettingsStatus Native_GraphicsSettings_ReadReceipt(ulong requestId, byte* destination, uint capacity, out uint required);
		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)]
		private static extern NativeGraphicsSettingsStatus Native_GraphicsSettings_ResolveCamera(uint objectId, ref NativeGraphicsSettingsTargetPacket target);
		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)]
		private static extern NativeGraphicsSettingsStatus Native_GraphicsSettings_Submit(ref NativeGraphicsSettingsRequestPacket request, NativeGraphicsSettingsValuePacket* values, byte* text, ref NativeGraphicsSettingsReceiptPacket receipt);
		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)]
		private static extern NativeGraphicsSettingsStatus Native_GraphicsSettings_SavePreferences(byte* name, uint nameBytes, ref NativeGraphicsSettingsTargetPacket target, ulong expectedStoreRevision, ref NativeGraphicsSettingsReceiptPacket receipt);
		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)]
		private static extern NativeGraphicsSettingsStatus Native_GraphicsSettings_RestorePreferences(byte* name, uint nameBytes, ref NativeGraphicsSettingsTargetPacket target, ref NativeGraphicsSettingsReceiptPacket receipt);
		[DllImport("TerraEngine", CallingConvention = CallingConvention.Cdecl)]
		private static extern NativeGraphicsSettingsStatus Native_GraphicsSettings_CancelStaged(ulong requestId, ref NativeGraphicsSettingsReceiptPacket receipt);
	}
}
