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
        /// <summary> Anisotropic filtering level (1 to 16). </summary>
        public uint AnisotropicFiltering;
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

        [DllImport("TerraEngine")]
        private static extern void Native_Graphics_SetQualityPreset(uint preset);

        [DllImport("TerraEngine")]
        private static extern QualityPreset Native_Graphics_GetQualityPreset();

        [DllImport("TerraEngine")]
        private static extern void Native_Graphics_SetRenderSettings(ref RenderSettings settings);

        [DllImport("TerraEngine")]
        private static extern void Native_Graphics_GetRenderSettings(out RenderSettings settings);
    }
}
