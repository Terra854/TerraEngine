// Graphics.cs — Managed API for global rendering settings and quality presets.
//
// This file defines the C# interface for controlling the engine's renderer,
// including shadow resolution, light budgets, and ambient occlusion settings.
// It allows scripts to query and modify graphics quality presets and provides
// access to real-time lighting diagnostics for editor status displays.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine
{
    /// <summary>
    /// Predefined graphics quality levels for the engine.
    /// </summary>
    public enum QualityPreset : uint
    {
        /// <summary> Lowest quality, optimized for performance. </summary>
        Low = 0,
        /// <summary> Balanced quality and performance. </summary>
        Medium = 1,
        /// <summary> High quality, standard for modern PCs. </summary>
        High = 2,
        /// <summary> Maximum quality, for enthusiast hardware. </summary>
        Ultra = 3,
        /// <summary> User-defined custom settings. </summary>
        Custom = 4
    }

    /// <summary>
    /// Individual rendering parameters that can be adjusted.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct RenderSettings
    {
        /// <summary> Resolution of shadow map textures (e.g. 512, 1024, 2048, 4096). </summary>
        public uint ShadowResolution;
        /// <summary> Size of the PCF filter kernel (must be an odd number like 1, 3, 5, 7). </summary>
        public uint ShadowFilteringSize;
        /// <summary> Scene-wide candidate budget uploaded into the lightweight GPU analysis pass before the final retained-light list is chosen for shading. </summary>
        public uint MaxUploadedLights;
        /// <summary> Scene-wide maximum number of dynamic lights retained for shading each frame. Variable light bins no longer treat this as a per-cluster cap. </summary>
        public uint MaxDynamicLights;
        /// <summary> Scene-wide maximum number of retained lights that may still request shadow resources in the same frame. This value is clamped so it never exceeds MaxDynamicLights or the available shadow slots. </summary>
        public uint MaxShadowLights;
        /// <summary> Optional cap for retained area lights so grouped presets can bound higher-cost lighting classes independently. </summary>
        public uint MaxAreaLights;
        /// <summary> Toggles Linearly Transformed Cosines (LTC) for Area Lights. 1 = Enabled, 0 = Disabled. </summary>
        public uint EnableLTC;
        /// <summary> Toggles Image-Based Lighting (IBL). 1 = Enabled, 0 = Disabled. </summary>
        public uint EnableIBL;
        /// <summary> Scales indirect and environment lighting without changing direct light intensity. </summary>
        public float IndirectStrength;
        /// <summary> Scales contact darkening applied to indirect and environment lighting. </summary>
        public float AmbientOcclusionStrength;
        /// <summary> Quality tier for contact darkening, where 0 disables the effect and higher values increase fidelity. </summary>
        public uint AmbientOcclusionQuality;
        /// <summary> Anisotropic filtering level (1 to 16). </summary>
        public uint AnisotropicFiltering;
        /// <summary> Startup-only Variable Rate Shading toggle. 1 = Enabled, 0 = Disabled. </summary>
        public uint VariableRateShading;
    }

    // LightingDiagnosticCategory is the managed-facing contract for this subsystem; keep it predictable for scripts.
    public enum LightingDiagnosticCategory : uint
    {
        Shadow = 0,
        IndirectLighting = 1,
        AmbientOcclusion = 2,
        MaterialShadow = 3,
        Budget = 4
    }

    // LightingDiagnosticStatus is the managed-facing contract for this subsystem; keep it predictable for scripts.
    public enum LightingDiagnosticStatus : uint
    {
        Active = 0,
        Disabled = 1,
        Degraded = 2,
        Fallback = 3,
        Unavailable = 4
    }

    /// <summary>
    /// Provides access to global graphics and rendering settings.
    /// </summary>
    public static class Graphics
    {
        /// <summary>
        /// Gets or sets the current quality preset. Setting this automatically updates individual RenderSettings.
        /// </summary>
        public static QualityPreset QualityPreset
        {
            get => Native_Graphics_GetQualityPreset();
            set => Native_Graphics_SetQualityPreset((uint)value);
        }

        /// <summary>
        /// Gets or sets the individual rendering settings. Setting this automatically switches the QualityPreset to Custom.
        /// </summary>
        public static RenderSettings RenderSettings
        {
            get
            {
                Native_Graphics_GetRenderSettings(out RenderSettings settings);
                return settings;
            }
            set => Native_Graphics_SetRenderSettings(ref value);
        }

        /// <summary>
        /// Returns a compact human-readable lighting diagnostics summary for editor status displays.
        /// </summary>
        public static string LightingDiagnosticsSummary
        {
            get
            {
                IntPtr message = Native_Graphics_GetLightingDiagnosticsSummary(0u);
                return Marshal.PtrToStringAnsi(message) ?? string.Empty;
            }
        }

        [DllImport("TerraEngine")]
        // Native_Graphics_SetQualityPreset bridges managed callers to engine state; validate arguments before forwarding to native code.
        private static extern void Native_Graphics_SetQualityPreset(uint preset);

        [DllImport("TerraEngine")]
        // Native_Graphics_GetQualityPreset bridges managed callers to engine state; validate arguments before forwarding to native code.
        private static extern QualityPreset Native_Graphics_GetQualityPreset();

        [DllImport("TerraEngine")]
        // Native_Graphics_SetRenderSettings bridges managed callers to engine state; validate arguments before forwarding to native code.
        private static extern void Native_Graphics_SetRenderSettings(ref RenderSettings settings);

        [DllImport("TerraEngine")]
        // Native_Graphics_GetRenderSettings bridges managed callers to engine state; validate arguments before forwarding to native code.
        private static extern void Native_Graphics_GetRenderSettings(out RenderSettings settings);

        [DllImport("TerraEngine")]
        // Native_Graphics_GetLightingDiagnosticsSummary bridges managed callers to engine state; validate arguments before forwarding to native code.
        private static extern IntPtr Native_Graphics_GetLightingDiagnosticsSummary(uint viewType);
    }
}
