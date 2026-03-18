using Hexa.NET.ImGui;
using System;
using TerraEngine.Editor.Framework;

namespace TerraEngine.Editor
{
    /// <summary>
    /// Editor window for configuring global graphics and rendering quality settings.
    /// </summary>
    public class RenderSettingsWindow : EditorWindow
    {
        /// <summary>
        /// Initializes a new instance of the RenderSettingsWindow class.
        /// </summary>
        public RenderSettingsWindow() : base("Render Settings")
        {
            IsVisible = false;
        }

        /// <summary>
        /// Renders the Render Settings GUI using ImGui.
        /// </summary>
        protected override void OnGui()
        {
            var currentPreset = Graphics.QualityPreset;
            var settings = Graphics.RenderSettings;

            if (ImGui.BeginCombo("Quality Preset", currentPreset.ToString()))
            {
                foreach (QualityPreset preset in Enum.GetValues(typeof(QualityPreset)))
                {
                    bool isSelected = (currentPreset == preset);
                    if (ImGui.Selectable(preset.ToString(), isSelected))
                    {
                        Graphics.QualityPreset = preset;
                    }
                    if (isSelected)
                    {
                        ImGui.SetItemDefaultFocus();
                    }
                }
                ImGui.EndCombo();
            }

            ImGui.Separator();

            bool changed = false;

            ImGui.TextDisabled("Lighting & Shadows");
            
            int shadowRes = (int)settings.ShadowResolution;
            if (ImGui.InputInt("Shadow Resolution", ref shadowRes))
            {
                settings.ShadowResolution = (uint)Math.Max(128, shadowRes);
                changed = true;
            }

            int filterSize = (int)settings.ShadowFilteringSize;
            if (ImGui.SliderInt("Shadow PCF Kernel", ref filterSize, 1, 15))
            {
                // Force odd numbers for centered kernels
                if (filterSize % 2 == 0) filterSize++;
                settings.ShadowFilteringSize = (uint)filterSize;
                changed = true;
            }

            int maxLights = (int)settings.MaxDynamicLights;
            if (ImGui.SliderInt("Max Dynamic Lights", ref maxLights, 0, 1024))
            {
                settings.MaxDynamicLights = (uint)maxLights;
                changed = true;
            }

            bool enableLTC = settings.EnableLTC > 0;
            if (ImGui.Checkbox("Enable Area Lights (LTC)", ref enableLTC))
            {
                settings.EnableLTC = enableLTC ? 1u : 0u;
                changed = true;
            }

            bool enableIBL = settings.EnableIBL > 0;
            if (ImGui.Checkbox("Enable Indirect (IBL)", ref enableIBL))
            {
                settings.EnableIBL = enableIBL ? 1u : 0u;
                changed = true;
            }

            ImGui.Separator();
            ImGui.TextDisabled("Textures");

            int aniso = (int)settings.AnisotropicFiltering;
            if (ImGui.SliderInt("Anisotropic Filtering", ref aniso, 1, 16))
            {
                settings.AnisotropicFiltering = (uint)aniso;
                changed = true;
            }

            if (changed)
            {
                Graphics.RenderSettings = settings;
            }
        }
    }
}
