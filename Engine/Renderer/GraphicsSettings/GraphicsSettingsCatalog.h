// GraphicsSettingsCatalog.h - Pure discovery and native/profile value mapping.
#pragma once
#include "GraphicsSettingsTypes.h"
#include <string_view>
struct RenderSettings;

namespace TerraEngine::Renderer::GraphicsSettings
{
	// These exported profile fields currently have no renderer consumer. Keeping
	// them separate prevents authoring metadata from masquerading as live scale.
	struct GraphicsProfileMetadata
	{
		std::uint32_t textureBudgetMegabytes = 512;
		float resolutionScale = 1.0f;
	};
	// The protected caller copies these existing scene/component values. This
	// catalog never resolves or changes live authored scene storage itself.
	struct GraphicsSceneSkyValues
	{
		float sceneIndirectStrength = 1.0f;
		float sceneAmbientOcclusionStrength = 1.0f;
		std::uint32_t sceneAmbientOcclusionQuality = 2;
		float skyExposure = 0.7f;
	};
	struct GraphicsCatalogCapabilities
	{
		bool vrsSupported = false;
		bool validationSupported = false;
		bool developmentDiagnostics = false;
		bool postProcessColorSupported = false;
		std::uint32_t maxShadowResolution = 16384;
		std::uint32_t maxAnisotropy = 16;
		std::uint32_t maxUploadedLights = 1024;
		std::uint32_t maxShadowFilteringSize = 31;
		// Actual native qualification supplies independent mode bits. Off needs
		// no effect resources; absent implementations remain unavailable.
		std::uint32_t spatialAAModeMask = 1u;
		// These flags are independent native implementation qualifications, not
		// physical-device feature guesses. Unimplemented variants stay unavailable.
		bool msaa4Supported = false;
		bool taaSupported = false;
		bool temporalUpscalingSupported = false;
		bool temporalSharpeningSupported = false;
		// Each bit requires its own actually installed native pass and inputs.
		std::uint32_t postProcessEffectMask = 0u;
	};
	// Defaults must be the resolved authored baseline, not a transient override;
	// reset then inherits the correct project/profile values.
	std::vector<GraphicsSettingDescriptor> BuildGraphicsSettingsCatalog(
		const RenderSettings& authoredDefaults, bool authoredTaskCulling,
		const GraphicsProfileMetadata& authoredProfile, const GraphicsSceneSkyValues& authoredSceneSky,
		const GraphicsCatalogCapabilities& capabilities);
	// These pure validators consume copied metadata only. A known wire mode that
	// is absent from the captured options is unsupported, rather than malformed.
	GraphicsRequestReason ValidateSpatialAAValue(const GraphicsSettingDescriptor&, const GraphicsValue&) noexcept;
	bool IsGraphicsAASetting(GraphicsSettingId) noexcept;
	GraphicsRequestReason ValidateGraphicsAASettingValue(const GraphicsSettingDescriptor&, const GraphicsValue&) noexcept;
	struct GraphicsAAConfiguration
	{
		GraphicsAntiAliasingMode antiAliasing = GraphicsAntiAliasingMode::Off;
		GraphicsUpscalingMode upscaling = GraphicsUpscalingMode::Off;
		float internalScale = 1.0f;
		float sharpening = 0.0f;
		std::uint32_t sampleCount = 1u;
	};
	// Resolve a complete copied candidate and derive samples from its mode. No
	// caller-supplied sample metadata can enable an unqualified raster variant.
	GraphicsRequestReason ResolveGraphicsAAConfiguration(std::span<const GraphicsSettingValue>, GraphicsAAConfiguration&) noexcept;
	// Zero remains a not-yet-bound native extent. Half scale rounds down and
	// clamps nonzero surfaces to one pixel; the backend uses this same rule.
	std::uint32_t ResolveGraphicsAAInternalDimension(std::uint32_t outputDimension, const GraphicsAAConfiguration&) noexcept;
	// Validate the whole resolved candidate so a mixed batch cannot bypass the
	// native-size, single-sample realization contract through a second field.
	GraphicsRequestReason ValidateSpatialAAConfiguration(std::span<const GraphicsSettingValue>, std::uint32_t sampleCount) noexcept;
	const GraphicsSettingDescriptor* FindGraphicsSetting(std::span<const GraphicsSettingDescriptor>, GraphicsSettingId) noexcept;
	const GraphicsSettingDescriptor* FindGraphicsSetting(std::span<const GraphicsSettingDescriptor>, std::string_view) noexcept;
	std::vector<GraphicsSettingValue> CaptureNativeGraphicsSettings(const RenderSettings&, bool taskCulling,
		const GraphicsProfileMetadata&, const GraphicsSceneSkyValues&);
	// This pure adapter validates/mutates copied native/profile state atomically.
	// It does not authorize an activation: the controller separately rejects
	// unavailable/profile/startup writes in an ordinary live request.
	bool ApplyNativeGraphicsSettings(std::span<const GraphicsSettingValue>,
		std::span<const GraphicsSettingDescriptor>, RenderSettings& inOutSettings,
		bool& inOutTaskCulling, GraphicsProfileMetadata& inOutProfile, std::string& reason);
	// Preserve v1: named tiers touch exactly thirteen native fields; Custom is
	// a successful no-op. Runtime definitions omit startup-only VRS (twelve).
	bool ApplyLegacyQualityPreset(RenderSettings&, QualityPreset) noexcept;
	std::vector<GraphicsPresetDefinition> BuildGraphicsPresetDefinitions();
}
