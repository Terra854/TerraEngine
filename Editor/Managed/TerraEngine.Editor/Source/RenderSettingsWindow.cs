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
        /// Shows a short tooltip for the most recently submitted ImGui item.
        /// This keeps each option description close to its label without
        /// cluttering the layout permanently.
        /// </summary>
        private static void ShowOptionTooltip(string description)
        {
            if (ImGui.IsItemHovered())
            {
                ImGui.SetTooltip(description);
            }
        }

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
            ShowOptionTooltip("Applies a predefined rendering profile. Choosing a preset updates grouped lighting, shadow, and texture budgets together so standard users do not need to retune every low-level override.");

            ImGui.Separator();

            bool changed = false;

            ImGui.TextDisabled("Lighting & Shadows");
            ShowOptionTooltip("Options in this section control uploaded candidates, retained shading lights, shadow ownership, and costly lighting classes.");
            
            int shadowRes = (int)settings.ShadowResolution;
            if (ImGui.InputInt("Shadow Resolution", ref shadowRes))
            {
                settings.ShadowResolution = (uint)Math.Max(128, shadowRes);
                changed = true;
            }
            ShowOptionTooltip("Sets the resolution used for generated shadow maps. Higher values improve sharpness but increase memory and GPU cost.");

            int filterSize = (int)settings.ShadowFilteringSize;
            if (ImGui.SliderInt("Shadow PCF Kernel", ref filterSize, 1, 15))
            {
                // Force odd numbers for centered kernels
                if (filterSize % 2 == 0) filterSize++;
                settings.ShadowFilteringSize = (uint)filterSize;
                changed = true;
            }
            ShowOptionTooltip("Controls the size of the percentage-closer filtering kernel. Larger kernels soften shadow edges but cost more samples.");

            int maxLights = (int)settings.MaxDynamicLights;
            if (ImGui.SliderInt("Retained Light Budget", ref maxLights, 0, 1024))
            {
                settings.MaxDynamicLights = (uint)maxLights;
                settings.MaxUploadedLights = Math.Max(settings.MaxUploadedLights, settings.MaxDynamicLights);
                settings.MaxShadowLights = Math.Min(settings.MaxShadowLights, Math.Min(settings.MaxDynamicLights, 4u));
                settings.MaxAreaLights = Math.Min(settings.MaxAreaLights, settings.MaxDynamicLights);
                changed = true;
            }
            ShowOptionTooltip("Limits how many lights survive the final screen-aware selection stage for shading in the current frame. Directional lights, shadow-capable lights, authored-priority lights, and strongly visible lights are prioritized when the scene exceeds this budget.");

            int maxShadowLights = (int)settings.MaxShadowLights;
            if (ImGui.SliderInt("Shadow Light Budget", ref maxShadowLights, 0, Math.Max(0, Math.Min(maxLights, 4))))
            {
                settings.MaxShadowLights = (uint)Math.Clamp(maxShadowLights, 0, Math.Min(maxLights, 4));
                changed = true;
            }
            ShowOptionTooltip("Limits how many retained lights may still own shadow resources in the current frame. The current renderer reuses one shadow slot per supported light type, so this budget is intentionally clamped to the real slot capacity.");

            if (ImGui.CollapsingHeader("Advanced Lighting Overrides"))
            {
                int maxUploadedLights = (int)settings.MaxUploadedLights;
                if (ImGui.SliderInt("Uploaded Light Budget", ref maxUploadedLights, Math.Max(1, maxLights), 1024))
                {
                    settings.MaxUploadedLights = (uint)Math.Max(maxUploadedLights, maxLights);
                    changed = true;
                }
                ShowOptionTooltip("Controls how many candidate lights are uploaded into the lightweight GPU analysis stage before the final retained-light list is chosen. Raising this can improve heavy-scene selection quality at additional per-frame cost.");

                int maxAreaLights = (int)settings.MaxAreaLights;
                if (ImGui.SliderInt("Area Light Budget", ref maxAreaLights, 0, Math.Max(0, maxLights)))
                {
                    settings.MaxAreaLights = (uint)Math.Clamp(maxAreaLights, 0, maxLights);
                    changed = true;
                }
                ShowOptionTooltip("Caps retained area lights separately from the overall retained-light budget so presets can bound higher-cost lighting classes without removing broad point or spot coverage.");
            }

            bool enableLTC = settings.EnableLTC > 0;
            if (ImGui.Checkbox("Enable Area Lights (LTC)", ref enableLTC))
            {
                settings.EnableLTC = enableLTC ? 1u : 0u;
                changed = true;
            }
            ShowOptionTooltip("Enables Linearly Transformed Cosines for rectangular area lights. Disable this to reduce cost if area lights are not needed.");

            bool enableIBL = settings.EnableIBL > 0;
            if (ImGui.Checkbox("Enable Indirect (IBL)", ref enableIBL))
            {
                settings.EnableIBL = enableIBL ? 1u : 0u;
                changed = true;
            }
            ShowOptionTooltip("Toggles image-based lighting contributions that add indirect ambient response to materials.");

            ImGui.Separator();
            ImGui.TextDisabled("Textures");
            ShowOptionTooltip("Texture settings affect sampling quality rather than clustered-lighting behavior.");

            int aniso = (int)settings.AnisotropicFiltering;
            if (ImGui.SliderInt("Anisotropic Filtering", ref aniso, 1, 16))
            {
                settings.AnisotropicFiltering = (uint)aniso;
                changed = true;
            }
            ShowOptionTooltip("Improves texture clarity at grazing view angles. Higher values increase texture sampling cost.");

            if (changed)
            {
                Graphics.RenderSettings = settings;
            }
        }
    }
}
