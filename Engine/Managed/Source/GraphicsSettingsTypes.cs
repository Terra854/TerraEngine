// GraphicsSettingsTypes.cs - Managed values and fixed-layout graphics ABI v2.
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace TerraEngine
{
	// These numbers mirror persisted native setting IDs. Append new values while
	// retaining old IDs so saved preferences and old game assemblies stay readable.
	public enum GraphicsSettingId : uint
	{
		Invalid = 0, ShadowResolution = 1, ShadowFilteringSize = 2,
		MaxUploadedLights = 3, MaxDynamicLights = 4, MaxShadowLights = 5,
		MaxAreaLights = 6, EnableLTC = 7, EnableIBL = 8, IndirectStrength = 9,
		AmbientOcclusionStrength = 10, AmbientOcclusionQuality = 11,
		AnisotropicFiltering = 12, VariableRateShading = 13,
		ValidationEnabled = 14, DebugEnabled = 15, GlobalTaskShaderCullingEnabled = 16,
		VirtualGeometryLodErrorScale = 17, VirtualGeometryMaxLodDrop = 18,
		VirtualGeometryHiZOcclusionEnabled = 19,
		VirtualGeometryHiZSelfOcclusionGuardEnabled = 20,
		VirtualGeometryHiZNearPlaneGuardEnabled = 21,
		VirtualGeometryHiZThinGeometryGuardEnabled = 22,
		VirtualGeometryDebugVisualizationMode = 23, VirtualGeometryBudgetBytes = 24,
		VirtualGeometryPreloadBias = 25, VirtualGeometryLodTransitionHysteresisPixels = 26,
		VirtualGeometryLodTransitionStableFrames = 27,
		VirtualGeometryDeformationCurrentBudgetBytes = 28,
		VirtualGeometryDeformationHistoryBudgetBytes = 29,
		VirtualGeometryDeformationBoundsBudgetBytes = 30,
		TextureBudgetMegabytes = 31, ProfileResolutionScale = 32,
		SceneIndirectStrength = 33, SceneAmbientOcclusionStrength = 34,
		SceneAmbientOcclusionQuality = 35, SkyExposure = 36,
		AntiAliasingMode = 64, FixedExposureCompensation = 65,
		InternalResolutionScale = 66, ExposureMode = 67,
		AutoExposureMinEV = 68, AutoExposureMaxEV = 69,
		AutoExposureBrightenSpeed = 70, AutoExposureDarkenSpeed = 71,
		ToneMappingMode = 72, ColorGradingEnabled = 73,
		ColorTemperatureKelvin = 74, ColorTint = 75,
		ColorContrast = 76, ColorSaturation = 77, ColorLift = 78, ColorGain = 79,
		ColorCurve = 80, ColorCurveStrength = 81, ColorLut = 82, ColorLutStrength = 83,
		BloomEnabled = 84, BloomThreshold = 85, BloomIntensity = 86,
		BloomSpread = 87, BloomSoftKnee = 88,
		UpscalingMode = 89, UpscalingSharpness = 90, GeometryRasterMode = 91,
		GtaoEnabled = 92,
		GtaoIntensity = 93,
		GtaoQuality = 94,
		ScreenSpaceReflectionsEnabled = 95,
		ScreenSpaceReflectionsIntensity = 96,
		ScreenSpaceReflectionsQuality = 97,
		ScreenSpaceIndirectEnabled = 98,
		ScreenSpaceIndirectIntensity = 99,
		ScreenSpaceIndirectQuality = 100,
		DepthOfFieldEnabled = 104,
		DepthOfFieldFocusDistance = 105,
		DepthOfFieldFocusRange = 106,
		DepthOfFieldBlurStrength = 107,
		MotionBlurEnabled = 108,
		MotionBlurStrength = 109,
		MotionBlurCameraContribution = 110,
		MotionBlurObjectContribution = 111,
		LensDirtEnabled = 112,
		LensDirtIntensity = 113,
		LensDirtTexture = 114,
		LensFlareEnabled = 115,
		LensFlareIntensity = 116,
		LensFlareThreshold = 117,
		VignetteEnabled = 118,
		VignetteIntensity = 119,
		VignetteRadius = 120,
		VignetteSoftness = 121,
		FilmGrainEnabled = 122,
		FilmGrainIntensity = 123,
		FilmGrainResponse = 124,
		ChromaticAberrationEnabled = 125,
		ChromaticAberrationPixels = 126,
		LensDistortionEnabled = 127,
		LensDistortionK1 = 128,
		LensDistortionK2 = 129,
		VolumetricFogEnabled = 130, VolumetricFogDensity = 131,
		VolumetricFogHeightFalloff = 132, VolumetricFogBaseHeight = 133,
		VolumetricFogMaxDistance = 134, SunShaftsEnabled = 135,
		SunShaftsIntensity = 136, OutputDitheringEnabled = 137
	}

	/// <summary>Selects standalone AA. Query actual target support before combining with reconstruction.</summary>
	public enum GraphicsAntiAliasingMode : uint { Off = 0, FXAA = 1, SMAA = 2, CMAA2 = 3, MSAA = 4, TAA = 5 }

	/// <summary>Selects independent reconstruction; temporal upscaling requires standalone AA Off.</summary>
	public enum GraphicsUpscalingMode : uint { Off = 0, Temporal = 1 }

	/// <summary>Selects a real supported raster path without a validation or debug route override.</summary>
	public enum GraphicsGeometryRasterMode : uint { Automatic = 0, Indexed = 1 }

	/// <summary>Selects a fixed or adaptive exposure in a graphics settings batch.</summary>
	public enum GraphicsExposureMode : uint { Legacy = 0, Fixed = 1, Automatic = 2 }

	/// <summary>Selects the conversion from scene radiance to display-referred color.</summary>
	public enum GraphicsToneMappingMode : uint { Legacy = 0, Neutral = 1, Filmic = 2, GT7 = 3 }

	public enum GraphicsValueKind : uint
	{
		Boolean = 0, Unsigned32 = 1, Unsigned64 = 2, Scalar = 3, String = 4, Asset = 5
	}
	public enum GraphicsActivation : uint
	{
		Live = 0, Rebuild = 1, NextLaunch = 2, ProfileOnly = 3, DevelopmentOnly = 4
	}
	public enum GraphicsAuthoringScope : uint
	{
		None = 0, Scene = 1, SkyComponent = 2, BuildProfile = 3
	}
	public enum GraphicsRequestState : uint
	{
		Submitted = 0, Accepted = 1, PendingRebuild = 2, RestartRequired = 3,
		ScheduledRestart = 4, Applied = 5, Rejected = 6, Cancelled = 7
	}
	public enum GraphicsRequestReason : uint
	{
		None = 0, InvalidSetting = 1, InvalidValue = 2, Unsupported = 3,
		InvalidScope = 4, StaleLifetime = 5, StaleRevision = 6, TargetUnavailable = 7,
		AmbiguousView = 8, PreparationFailed = 9, StorageFailed = 10,
		ConflictingRequest = 11, InterruptedStartup = 12, ExplicitCancellation = 13,
		RestartRequired = 14, RequestCapacityExceeded = 15
	}

	public readonly struct GraphicsSettingValue
	{
		public GraphicsSettingId Id { get; }
		public GraphicsValueKind ValueKind { get; }
		public object Value { get; }
		public GraphicsSettingValue(GraphicsSettingId id, bool value) : this(id, GraphicsValueKind.Boolean, value) { }
		public GraphicsSettingValue(GraphicsSettingId id, uint value) : this(id, GraphicsValueKind.Unsigned32, value) { }
		public GraphicsSettingValue(GraphicsSettingId id, ulong value) : this(id, GraphicsValueKind.Unsigned64, value) { }
		public GraphicsSettingValue(GraphicsSettingId id, float value) : this(id, GraphicsValueKind.Scalar, value) { }
		public GraphicsSettingValue(GraphicsSettingId id, string value) : this(id, GraphicsValueKind.String, value ?? throw new ArgumentNullException(nameof(value))) { }
		public GraphicsSettingValue(GraphicsSettingId id, PhysicsUuid value) : this(id, GraphicsValueKind.Asset, value) { }
		internal GraphicsSettingValue(GraphicsSettingId id, GraphicsValueKind kind, object value)
		{
			Id = id;
			ValueKind = kind;
			Value = value;
		}
	}

	// The default value addresses game-wide settings. A camera target captures one
	// native attachment lifetime and never silently attaches to its replacement.
	public readonly struct GraphicsSettingsTarget
	{
		internal readonly NativeGraphicsSettingsTargetPacket Native;
		internal GraphicsSettingsTarget(NativeGraphicsSettingsTargetPacket value) => Native = value;
		public static GraphicsSettingsTarget Game => default;
		public bool IsCamera => Native.Scope == 1U;
		public bool IsValid => Native.Scope == 0U || (Native.Scope == 1U
			&& Native.CameraId.IsValid && Native.SceneGeneration != 0U
			&& Native.ObjectLifetimeGeneration != 0U && Native.ComponentGeneration != 0U);
		public string CameraId => IsCamera ? GraphicsSettingsWire.UuidText(Native.CameraId) : string.Empty;
		public ulong SceneGeneration => Native.SceneGeneration;
		public ulong ObjectLifetimeGeneration => Native.ObjectLifetimeGeneration;
		public ulong ComponentGeneration => Native.ComponentGeneration;
		public ulong BindingGeneration => Native.BindingGeneration;
		public bool HasView => (Native.Flags & 2U) != 0U;
		public uint ViewType => Native.ViewType;
		public uint OutputKind => Native.OutputKind;
		public uint OutputId => Native.OutputId;
		public ulong ViewLifetimeGeneration => Native.ViewLifetimeGeneration;

		public GraphicsSettingsTarget WithView(uint viewType, uint outputKind, uint outputId, ulong lifetimeGeneration)
		{
			if (!IsCamera || !IsValid || lifetimeGeneration == 0U)
				throw new ArgumentException("An explicit graphics view requires a valid camera target and view lifetime.");
			NativeGraphicsSettingsTargetPacket native = Native;
			native.Flags |= 2U;
			native.ViewType = viewType;
			native.OutputKind = outputKind;
			native.OutputId = outputId;
			native.ViewLifetimeGeneration = lifetimeGeneration;
			return new(native);
		}
	}

	public readonly struct GraphicsSettingsLifecycle
	{
		public ulong ProjectGeneration { get; init; }
		public ulong PlaySessionGeneration { get; init; }
		public ulong RendererSessionGeneration { get; init; }
		public ulong BackendResourceGeneration { get; init; }
	}

	public sealed class GraphicsSettingDescriptor
	{
		public GraphicsSettingId Id { get; init; }
		public string Name { get; init; } = string.Empty;
		public string Units { get; init; } = string.Empty;
		public GraphicsValueKind ValueKind { get; init; }
		public object DefaultValue { get; init; } = false;
		public object? Minimum { get; init; }
		public object? Maximum { get; init; }
		public IReadOnlyList<object> Options { get; init; } = Array.Empty<object>();
		public bool GameScope { get; init; }
		public bool CameraScope { get; init; }
		public GraphicsAuthoringScope AuthoringScope { get; init; }
		public GraphicsActivation Activation { get; init; }
		public bool Available { get; init; }
		public GraphicsRequestReason UnavailableReason { get; init; }
		public string Detail { get; init; } = string.Empty;
	}

	public sealed class GraphicsSettingsSnapshot
	{
		public GraphicsSettingsTarget Target { get; init; }
		public GraphicsSettingsLifecycle Lifecycle { get; init; }
		public ulong Revision { get; init; }
		public ulong PublishedFrameId { get; init; }
		public IReadOnlyList<GraphicsSettingValue> Values { get; init; } = Array.Empty<GraphicsSettingValue>();
		public QualityPreset? Preset { get; init; }
		public uint InternalWidth { get; init; }
		public uint InternalHeight { get; init; }
		public uint OutputWidth { get; init; }
		public uint OutputHeight { get; init; }
		public uint SampleCount { get; init; }
		public ulong PostProcessResourceGeneration { get; init; }
	}

	public sealed class GraphicsSettingsReadResult
	{
		public GraphicsSettingsSnapshot? Snapshot { get; init; }
		public GraphicsSettingsSnapshot? Requested { get; init; }
		public GraphicsRequestReason Reason { get; init; }
		public string Detail { get; init; } = string.Empty;
	}

	public sealed class GraphicsRequestReceipt
	{
		public ulong RequestId { get; init; }
		public ulong Sequence { get; init; }
		public GraphicsRequestState State { get; init; }
		public GraphicsActivation Activation { get; init; }
		public GraphicsRequestReason Reason { get; init; }
		public IReadOnlyList<GraphicsSettingId> AffectedSettings { get; init; } = Array.Empty<GraphicsSettingId>();
		public string Detail { get; init; } = string.Empty;
		public ulong RequestedRevision { get; init; }
		public ulong EffectiveRevision { get; init; }
		public ulong AppliedFrameId { get; init; }
	}

	internal enum NativeGraphicsSettingsStatus : uint
	{
		Success = 0, Accepted = 1, InvalidArgument = 2, VersionMismatch = 3,
		BufferTooSmall = 4, Unavailable = 5, Rejected = 6
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeGraphicsSettingsTargetPacket
	{
		internal uint Scope;
		internal uint Flags;
		internal PhysicsUuid CameraId;
		internal ulong SceneGeneration;
		internal ulong ObjectLifetimeGeneration;
		internal ulong ComponentGeneration;
		internal ulong BindingGeneration;
		internal uint ViewType;
		internal uint OutputKind;
		internal uint OutputId;
		internal uint Reserved;
		internal ulong ViewLifetimeGeneration;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeGraphicsSettingsValuePacket
	{
		internal uint SettingId;
		internal uint ValueKind;
		internal ulong ScalarBits;
		internal PhysicsUuid AssetId;
		internal uint TextOffset;
		internal uint TextLength;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeGraphicsSettingsRequestPacket
	{
		internal NativeGraphicsAbiHeader Header;
		internal uint Operation;
		internal uint ValueCount;
		internal uint TextBytes;
		internal uint Preset;
		internal ulong ExpectedRevision;
		internal NativeGraphicsSettingsTargetPacket Target;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeGraphicsSettingsReceiptPacket
	{
		internal NativeGraphicsAbiHeader Header;
		internal uint State;
		internal uint Activation;
		internal uint Reason;
		internal uint AffectedCount;
		internal uint DetailBytes;
		internal uint Reserved;
		internal ulong RequestId;
		internal ulong Sequence;
		internal ulong RequestedRevision;
		internal ulong EffectiveRevision;
		internal ulong AppliedFrameId;
	}

	internal static class GraphicsSettingsWire
	{
		internal const uint Version = 2;
		internal const int MaximumValues = 1024;
		internal const int MaximumBytes = 1024 * 1024;
		internal static NativeGraphicsAbiHeader Header<T>() where T : unmanaged => new()
		{
			AbiVersion = Version,
			StructSize = checked((uint)Marshal.SizeOf<T>())
		};
		internal static string UuidText(PhysicsUuid value)
		{
			Span<byte> bytes = stackalloc byte[16];
			BinaryPrimitives.WriteUInt64LittleEndian(bytes, value.First);
			BinaryPrimitives.WriteUInt64LittleEndian(bytes[8..], value.Second);
			return Convert.ToHexString(bytes).ToLowerInvariant();
		}
		internal static PhysicsUuid ParseUuid(string value)
		{
			if (value.Length != 32) throw new FormatException("Graphics UUID must contain 32 hexadecimal digits.");
			byte[] bytes = Convert.FromHexString(value);
			return new() {
				First = BinaryPrimitives.ReadUInt64LittleEndian(bytes),
				Second = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(8))
			};
		}
	}
}
