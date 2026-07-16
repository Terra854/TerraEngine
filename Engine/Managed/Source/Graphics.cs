// Graphics.cs — Safe managed access to game-wide runtime rendering settings.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine
{
	public enum QualityPreset : uint
	{
		Low = 0,
		Medium = 1,
		High = 2,
		Ultra = 3,
		Custom = 4
	}

	public enum GraphicsStatus : uint
	{
		Success = 0,
		Accepted = 1,
		InvalidArgument = 2,
		VersionMismatch = 3,
		InvalidPreset = 4
	}

	public enum VirtualGeometryDebugMode : uint
	{
		None = 0,
		LOD = 1,
		MeshletId = 2,
		Bounds = 3,
		Culling = 4,
		HiZ = 5,
		Residency = 6,
		Error = 7,
		Cluster = 8
	}

	public struct VirtualGeometrySettings
	{
		public float LodErrorScale;
		public uint MaxLodDrop;
		public bool HiZOcclusionEnabled;
		public bool HiZSelfOcclusionGuardEnabled;
		public bool HiZNearPlaneGuardEnabled;
		public bool HiZThinGeometryGuardEnabled;
		public VirtualGeometryDebugMode DebugVisualizationMode;
		public ulong GeometryBudgetBytes;
		public float PreloadBias;
		public float LodTransitionHysteresisPixels;
		public uint LodTransitionStableFrames;
	}

	// This public value is never passed directly to native code. Conversion to a
	// fixed ABI record keeps ordinary C# bool layout and future API growth from
	// changing the buffer size understood by the engine DLL.
	public struct RenderSettings
	{
		public uint ShadowResolution;
		public uint ShadowFilteringSize;
		public uint MaxUploadedLights;
		public uint MaxDynamicLights;
		public uint MaxShadowLights;
		public uint MaxAreaLights;
		public bool EnableLTC;
		public bool EnableIBL;
		public float IndirectStrength;
		public float AmbientOcclusionStrength;
		public uint AmbientOcclusionQuality;
		public uint AnisotropicFiltering;
		// VRS and validation are reported for completeness. Backend limitations may
		// defer their visible effect until the renderer recreates startup resources.
		public bool VariableRateShading;
		public bool ValidationEnabled;
		public bool DebugEnabled;
		public VirtualGeometrySettings VirtualGeometry;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeGraphicsAbiHeader
	{
		public uint AbiVersion;
		public uint StructSize;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeGraphicsRenderSettings
	{
		internal const uint AbiVersion = 1;
		internal const uint Size = 120;

		public NativeGraphicsAbiHeader Header;
		public uint ShadowResolution;
		public uint ShadowFilteringSize;
		public uint MaxUploadedLights;
		public uint MaxDynamicLights;
		public uint MaxShadowLights;
		public uint MaxAreaLights;
		public uint EnableLTC;
		public uint EnableIBL;
		public float IndirectStrength;
		public float AmbientOcclusionStrength;
		public uint AmbientOcclusionQuality;
		public uint AnisotropicFiltering;
		public uint VariableRateShading;
		public uint ValidationEnabled;
		public uint DebugEnabled;
		public float VirtualGeometryLodErrorScale;
		public uint VirtualGeometryMaxLodDrop;
		public uint VirtualGeometryHiZOcclusionEnabled;
		public uint VirtualGeometryHiZSelfOcclusionGuardEnabled;
		public uint VirtualGeometryHiZNearPlaneGuardEnabled;
		public uint VirtualGeometryHiZThinGeometryGuardEnabled;
		public uint VirtualGeometryDebugVisualizationMode;
		public ulong VirtualGeometryBudgetBytes;
		public float VirtualGeometryPreloadBias;
		public float VirtualGeometryLodTransitionHysteresisPixels;
		public uint VirtualGeometryLodTransitionStableFrames;
		private uint _reserved;

		public static NativeGraphicsRenderSettings FromPublic(RenderSettings settings)
		{
			return new NativeGraphicsRenderSettings
			{
				Header = new NativeGraphicsAbiHeader { AbiVersion = AbiVersion, StructSize = Size },
				ShadowResolution = settings.ShadowResolution,
				ShadowFilteringSize = settings.ShadowFilteringSize,
				MaxUploadedLights = settings.MaxUploadedLights,
				MaxDynamicLights = settings.MaxDynamicLights,
				MaxShadowLights = settings.MaxShadowLights,
				MaxAreaLights = settings.MaxAreaLights,
				EnableLTC = settings.EnableLTC ? 1U : 0U,
				EnableIBL = settings.EnableIBL ? 1U : 0U,
				IndirectStrength = settings.IndirectStrength,
				AmbientOcclusionStrength = settings.AmbientOcclusionStrength,
				AmbientOcclusionQuality = settings.AmbientOcclusionQuality,
				AnisotropicFiltering = settings.AnisotropicFiltering,
				VariableRateShading = settings.VariableRateShading ? 1U : 0U,
				ValidationEnabled = settings.ValidationEnabled ? 1U : 0U,
				DebugEnabled = settings.DebugEnabled ? 1U : 0U,
				VirtualGeometryLodErrorScale = settings.VirtualGeometry.LodErrorScale,
				VirtualGeometryMaxLodDrop = settings.VirtualGeometry.MaxLodDrop,
				VirtualGeometryHiZOcclusionEnabled = settings.VirtualGeometry.HiZOcclusionEnabled ? 1U : 0U,
				VirtualGeometryHiZSelfOcclusionGuardEnabled = settings.VirtualGeometry.HiZSelfOcclusionGuardEnabled ? 1U : 0U,
				VirtualGeometryHiZNearPlaneGuardEnabled = settings.VirtualGeometry.HiZNearPlaneGuardEnabled ? 1U : 0U,
				VirtualGeometryHiZThinGeometryGuardEnabled = settings.VirtualGeometry.HiZThinGeometryGuardEnabled ? 1U : 0U,
				VirtualGeometryDebugVisualizationMode = (uint)settings.VirtualGeometry.DebugVisualizationMode,
				VirtualGeometryBudgetBytes = settings.VirtualGeometry.GeometryBudgetBytes,
				VirtualGeometryPreloadBias = settings.VirtualGeometry.PreloadBias,
				VirtualGeometryLodTransitionHysteresisPixels = settings.VirtualGeometry.LodTransitionHysteresisPixels,
				VirtualGeometryLodTransitionStableFrames = settings.VirtualGeometry.LodTransitionStableFrames
			};
		}

		public readonly RenderSettings ToPublic()
		{
			return new RenderSettings
			{
				ShadowResolution = ShadowResolution,
				ShadowFilteringSize = ShadowFilteringSize,
				MaxUploadedLights = MaxUploadedLights,
				MaxDynamicLights = MaxDynamicLights,
				MaxShadowLights = MaxShadowLights,
				MaxAreaLights = MaxAreaLights,
				EnableLTC = EnableLTC != 0,
				EnableIBL = EnableIBL != 0,
				IndirectStrength = IndirectStrength,
				AmbientOcclusionStrength = AmbientOcclusionStrength,
				AmbientOcclusionQuality = AmbientOcclusionQuality,
				AnisotropicFiltering = AnisotropicFiltering,
				VariableRateShading = VariableRateShading != 0,
				ValidationEnabled = ValidationEnabled != 0,
				DebugEnabled = DebugEnabled != 0,
				VirtualGeometry = new VirtualGeometrySettings
				{
					LodErrorScale = VirtualGeometryLodErrorScale,
					MaxLodDrop = VirtualGeometryMaxLodDrop,
					HiZOcclusionEnabled = VirtualGeometryHiZOcclusionEnabled != 0,
					HiZSelfOcclusionGuardEnabled = VirtualGeometryHiZSelfOcclusionGuardEnabled != 0,
					HiZNearPlaneGuardEnabled = VirtualGeometryHiZNearPlaneGuardEnabled != 0,
					HiZThinGeometryGuardEnabled = VirtualGeometryHiZThinGeometryGuardEnabled != 0,
					DebugVisualizationMode = (VirtualGeometryDebugMode)VirtualGeometryDebugVisualizationMode,
					GeometryBudgetBytes = VirtualGeometryBudgetBytes,
					PreloadBias = VirtualGeometryPreloadBias,
					LodTransitionHysteresisPixels = VirtualGeometryLodTransitionHysteresisPixels,
					LodTransitionStableFrames = VirtualGeometryLodTransitionStableFrames
				}
			};
		}
	}

	public static class Graphics
	{
		public static uint AbiVersion => Native_Graphics_GetAbiVersion();

		public static QualityPreset QualityPreset
		{
			get
			{
				GraphicsStatus status = Native_Graphics_ReadQualityPreset(out uint preset);
				EnsureReadable(status, nameof(QualityPreset));
				return (QualityPreset)preset;
			}
			set
			{
				GraphicsStatus status = Native_Graphics_SubmitRuntimeQualityPreset((uint)value);
				EnsureAccepted(status, nameof(QualityPreset));
			}
		}

		public static RenderSettings RenderSettings
		{
			get
			{
				NativeGraphicsRenderSettings settings = new()
				{
					Header = new NativeGraphicsAbiHeader
					{
						AbiVersion = NativeGraphicsRenderSettings.AbiVersion,
						StructSize = NativeGraphicsRenderSettings.Size
					}
				};
				GraphicsStatus status = Native_Graphics_ReadRenderSettings(ref settings);
				EnsureReadable(status, nameof(RenderSettings));
				return settings.ToPublic();
			}
			set => EnsureAccepted(ApplyRuntimeSettings(value), nameof(RenderSettings));
		}

		public static ulong RuntimeSettingsGeneration => Native_Graphics_GetRuntimeSettingsGeneration();

		public static GraphicsStatus ApplyRuntimeSettings(RenderSettings settings)
		{
			NativeGraphicsRenderSettings native = NativeGraphicsRenderSettings.FromPublic(settings);
			return Native_Graphics_SubmitRuntimeRenderSettings(ref native);
		}

		public static string LightingDiagnosticsSummary
			=> Marshal.PtrToStringAnsi(Native_Graphics_GetLightingDiagnosticsSummary(0U)) ?? string.Empty;

		private static void EnsureReadable(GraphicsStatus status, string operation)
		{
			if (status != GraphicsStatus.Success)
			{
				throw new InvalidOperationException($"Graphics {operation} read failed with status {status}.");
			}
		}

		private static void EnsureAccepted(GraphicsStatus status, string operation)
		{
			if (status != GraphicsStatus.Accepted && status != GraphicsStatus.Success)
			{
				throw new ArgumentException($"Graphics {operation} update was rejected with status {status}.");
			}
		}

		[DllImport("TerraEngine")]
		private static extern uint Native_Graphics_GetAbiVersion();

		[DllImport("TerraEngine")]
		private static extern GraphicsStatus Native_Graphics_ReadRenderSettings(ref NativeGraphicsRenderSettings settings);

		[DllImport("TerraEngine")]
		private static extern GraphicsStatus Native_Graphics_SubmitRuntimeRenderSettings(ref NativeGraphicsRenderSettings settings);

		[DllImport("TerraEngine")]
		private static extern GraphicsStatus Native_Graphics_SubmitRuntimeQualityPreset(uint preset);

		[DllImport("TerraEngine")]
		private static extern GraphicsStatus Native_Graphics_ReadQualityPreset(out uint preset);

		[DllImport("TerraEngine")]
		private static extern ulong Native_Graphics_GetRuntimeSettingsGeneration();

		[DllImport("TerraEngine")]
		private static extern IntPtr Native_Graphics_GetLightingDiagnosticsSummary(uint viewType);
	}
}
