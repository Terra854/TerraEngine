#include "PostProcessEffectSettings.h"
#include <array>
#include <cmath>

namespace TerraEngine::Renderer::GraphicsSettings
{
	namespace
	{
		using Capability = PostProcessEffectCapability;
		using Kind = GraphicsValueKind;
		struct Field
		{
			GraphicsSettingId id;
			const char* name;
			const char* units;
			Capability capability;
			Kind kind;
			float defaultNumber = 0.0f, minimum = 0.0f, maximum = 0.0f;
		};
		// One table drives both discovery and scalar/quality validation. Resource
		// existence, formats, motion and native pass readiness remain outside it.
		constexpr std::array Fields{
			Field{GraphicsSettingId::GtaoEnabled, "postProcess.gtaoEnabled", "", Capability::GTAO, Kind::Boolean},
			Field{GraphicsSettingId::GtaoIntensity, "postProcess.gtaoIntensity", "multiplier", Capability::GTAO, Kind::Scalar, 1, 0, 4},
			Field{GraphicsSettingId::GtaoQuality, "postProcess.gtaoQuality", "tier", Capability::GTAO, Kind::Unsigned32, 1, 0, 2},
			Field{GraphicsSettingId::ScreenSpaceReflectionsEnabled, "postProcess.screenSpaceReflectionsEnabled", "", Capability::SSR, Kind::Boolean},
			Field{GraphicsSettingId::ScreenSpaceReflectionsIntensity, "postProcess.screenSpaceReflectionsIntensity", "multiplier", Capability::SSR, Kind::Scalar, 1, 0, 4},
			Field{GraphicsSettingId::ScreenSpaceReflectionsQuality, "postProcess.screenSpaceReflectionsQuality", "tier", Capability::SSR, Kind::Unsigned32, 1, 0, 2},
			Field{GraphicsSettingId::ScreenSpaceIndirectEnabled, "postProcess.screenSpaceIndirectEnabled", "", Capability::SSGI, Kind::Boolean},
			Field{GraphicsSettingId::ScreenSpaceIndirectIntensity, "postProcess.screenSpaceIndirectIntensity", "multiplier", Capability::SSGI, Kind::Scalar, 1, 0, 4},
			Field{GraphicsSettingId::ScreenSpaceIndirectQuality, "postProcess.screenSpaceIndirectQuality", "tier", Capability::SSGI, Kind::Unsigned32, 1, 0, 2},
			Field{GraphicsSettingId::DepthOfFieldEnabled, "postProcess.depthOfFieldEnabled", "", Capability::DepthOfField, Kind::Boolean},
			Field{GraphicsSettingId::DepthOfFieldFocusDistance, "postProcess.depthOfFieldFocusDistance", "m", Capability::DepthOfField, Kind::Scalar, 10, 0.01f, 10000},
			Field{GraphicsSettingId::DepthOfFieldFocusRange, "postProcess.depthOfFieldFocusRange", "m", Capability::DepthOfField, Kind::Scalar, 5, 0.01f, 10000},
			Field{GraphicsSettingId::DepthOfFieldBlurStrength, "postProcess.depthOfFieldBlurStrength", "px", Capability::DepthOfField, Kind::Scalar, 8, 0, 64},
			Field{GraphicsSettingId::MotionBlurEnabled, "postProcess.motionBlurEnabled", "", Capability::MotionBlur, Kind::Boolean},
			Field{GraphicsSettingId::MotionBlurStrength, "postProcess.motionBlurStrength", "multiplier", Capability::MotionBlur, Kind::Scalar, 1, 0, 2},
			Field{GraphicsSettingId::MotionBlurCameraContribution, "postProcess.motionBlurCameraContribution", "weight", Capability::MotionBlur, Kind::Scalar, 1, 0, 1},
			Field{GraphicsSettingId::MotionBlurObjectContribution, "postProcess.motionBlurObjectContribution", "weight", Capability::MotionBlur, Kind::Scalar, 1, 0, 1},
			Field{GraphicsSettingId::LensDirtEnabled, "postProcess.lensDirtEnabled", "", Capability::LensDirt, Kind::Boolean},
			Field{GraphicsSettingId::LensDirtIntensity, "postProcess.lensDirtIntensity", "multiplier", Capability::LensDirt, Kind::Scalar, 1, 0, 8},
			Field{GraphicsSettingId::LensDirtTexture, "postProcess.lensDirtTexture", "asset", Capability::LensDirt, Kind::Asset},
			Field{GraphicsSettingId::LensFlareEnabled, "postProcess.lensFlareEnabled", "", Capability::LensFlare, Kind::Boolean},
			Field{GraphicsSettingId::LensFlareIntensity, "postProcess.lensFlareIntensity", "multiplier", Capability::LensFlare, Kind::Scalar, 0.25f, 0, 8},
			Field{GraphicsSettingId::LensFlareThreshold, "postProcess.lensFlareThreshold", "linear luminance", Capability::LensFlare, Kind::Scalar, 1, 0, 64},
			Field{GraphicsSettingId::VignetteEnabled, "postProcess.vignetteEnabled", "", Capability::Presentation, Kind::Boolean},
			Field{GraphicsSettingId::VignetteIntensity, "postProcess.vignetteIntensity", "weight", Capability::Presentation, Kind::Scalar, 0.35f, 0, 1},
			Field{GraphicsSettingId::VignetteRadius, "postProcess.vignetteRadius", "image height", Capability::Presentation, Kind::Scalar, 0.65f, 0, 2},
			Field{GraphicsSettingId::VignetteSoftness, "postProcess.vignetteSoftness", "image height", Capability::Presentation, Kind::Scalar, 0.25f, 0.001f, 2},
			Field{GraphicsSettingId::FilmGrainEnabled, "postProcess.filmGrainEnabled", "", Capability::Presentation, Kind::Boolean},
			Field{GraphicsSettingId::FilmGrainIntensity, "postProcess.filmGrainIntensity", "linear RGB", Capability::Presentation, Kind::Scalar, 0.06f, 0, 1},
			Field{GraphicsSettingId::FilmGrainResponse, "postProcess.filmGrainResponse", "response", Capability::Presentation, Kind::Scalar, 1, 0, 8},
			Field{GraphicsSettingId::ChromaticAberrationEnabled, "postProcess.chromaticAberrationEnabled", "", Capability::Presentation, Kind::Boolean},
			Field{GraphicsSettingId::ChromaticAberrationPixels, "postProcess.chromaticAberrationPixels", "px", Capability::Presentation, Kind::Scalar, 2, -16, 16},
			Field{GraphicsSettingId::LensDistortionEnabled, "postProcess.lensDistortionEnabled", "", Capability::Presentation, Kind::Boolean},
			Field{GraphicsSettingId::LensDistortionK1, "postProcess.lensDistortionK1", "radial coefficient", Capability::Presentation, Kind::Scalar, 0.05f, -1, 1},
			Field{GraphicsSettingId::LensDistortionK2, "postProcess.lensDistortionK2", "radial coefficient", Capability::Presentation, Kind::Scalar, 0, -1, 1},
			Field{GraphicsSettingId::VolumetricFogEnabled, "postProcess.volumetricFogEnabled", "", Capability::Atmosphere, Kind::Boolean},
			Field{GraphicsSettingId::VolumetricFogDensity, "postProcess.volumetricFogDensity", "1/m", Capability::Atmosphere, Kind::Scalar, 0.015f, 0, 1},
			Field{GraphicsSettingId::VolumetricFogHeightFalloff, "postProcess.volumetricFogHeightFalloff", "1/m", Capability::Atmosphere, Kind::Scalar, 0.02f, 0, 1},
			Field{GraphicsSettingId::VolumetricFogBaseHeight, "postProcess.volumetricFogBaseHeight", "m", Capability::Atmosphere, Kind::Scalar, 0, -100000, 100000},
			Field{GraphicsSettingId::VolumetricFogMaxDistance, "postProcess.volumetricFogMaxDistance", "m", Capability::Atmosphere, Kind::Scalar, 500, 1, 10000},
			Field{GraphicsSettingId::SunShaftsEnabled, "postProcess.sunShaftsEnabled", "", Capability::Atmosphere, Kind::Boolean},
			Field{GraphicsSettingId::SunShaftsIntensity, "postProcess.sunShaftsIntensity", "multiplier", Capability::Atmosphere, Kind::Scalar, 0.25f, 0, 2},
			Field{GraphicsSettingId::OutputDitheringEnabled, "postProcess.outputDitheringEnabled", "", Capability::OutputDithering, Kind::Boolean}
		};
		const Field* FindField(GraphicsSettingId id) noexcept
		{
			for(const auto& field : Fields) if(static_cast<std::uint32_t>(field.id) == static_cast<std::uint32_t>(id)) return &field;
			return nullptr;
		}
		PostProcessEffectSettings Decode(std::span<const GraphicsSettingValue> values) noexcept
		{
			PostProcessEffectSettings result;
			// Decode is private and consumes validated values only. No conversion
			// between Boolean/UInt32/Scalar variants hides a malformed control.
			for(const auto& value : values)
			{
				const auto id = value.id;
				if(const auto enabled = std::get_if<bool>(&value.value)) switch(id)
				{
				case GraphicsSettingId::GtaoEnabled: result.gtao.enabled = *enabled; break;
				case GraphicsSettingId::ScreenSpaceReflectionsEnabled: result.reflections.enabled = *enabled; break;
				case GraphicsSettingId::ScreenSpaceIndirectEnabled: result.indirect.enabled = *enabled; break;
				case GraphicsSettingId::DepthOfFieldEnabled: result.depthOfField.enabled = *enabled; break;
				case GraphicsSettingId::MotionBlurEnabled: result.motionBlur.enabled = *enabled; break;
				case GraphicsSettingId::LensDirtEnabled: result.lensDirt.enabled = *enabled; break;
				case GraphicsSettingId::LensFlareEnabled: result.lensFlare.enabled = *enabled; break;
				case GraphicsSettingId::VignetteEnabled: result.presentation.vignetteEnabled = *enabled; break;
				case GraphicsSettingId::FilmGrainEnabled: result.presentation.grainEnabled = *enabled; break;
				case GraphicsSettingId::ChromaticAberrationEnabled: result.presentation.chromaticEnabled = *enabled; break;
				case GraphicsSettingId::LensDistortionEnabled: result.presentation.distortionEnabled = *enabled; break;
				case GraphicsSettingId::VolumetricFogEnabled: result.atmosphere.fogEnabled = *enabled; break;
				case GraphicsSettingId::SunShaftsEnabled: result.atmosphere.shaftsEnabled = *enabled; break;
				case GraphicsSettingId::OutputDitheringEnabled: result.outputDithering = *enabled; break;
				default: break;
				}
				if(const auto quality = std::get_if<std::uint32_t>(&value.value)) switch(id)
				{
				case GraphicsSettingId::GtaoQuality: result.gtao.quality = static_cast<ScreenSpaceEffectQuality>(*quality); break;
				case GraphicsSettingId::ScreenSpaceReflectionsQuality: result.reflections.quality = static_cast<ScreenSpaceEffectQuality>(*quality); break;
				case GraphicsSettingId::ScreenSpaceIndirectQuality: result.indirect.quality = static_cast<ScreenSpaceEffectQuality>(*quality); break;
				default: break;
				}
				if(const auto scalar = std::get_if<float>(&value.value)) switch(id)
				{
				case GraphicsSettingId::GtaoIntensity: result.gtao.intensity = *scalar; break;
				case GraphicsSettingId::ScreenSpaceReflectionsIntensity: result.reflections.intensity = *scalar; break;
				case GraphicsSettingId::ScreenSpaceIndirectIntensity: result.indirect.intensity = *scalar; break;
				case GraphicsSettingId::DepthOfFieldFocusDistance: result.depthOfField.focusDistance = *scalar; break;
				case GraphicsSettingId::DepthOfFieldFocusRange: result.depthOfField.focusRange = *scalar; break;
				case GraphicsSettingId::DepthOfFieldBlurStrength: result.depthOfField.blurStrength = *scalar; break;
				case GraphicsSettingId::MotionBlurStrength: result.motionBlur.strength = *scalar; break;
				case GraphicsSettingId::MotionBlurCameraContribution: result.motionBlur.cameraContribution = *scalar; break;
				case GraphicsSettingId::MotionBlurObjectContribution: result.motionBlur.objectContribution = *scalar; break;
				case GraphicsSettingId::LensDirtIntensity: result.lensDirt.intensity = *scalar; break;
				case GraphicsSettingId::LensFlareIntensity: result.lensFlare.intensity = *scalar; break;
				case GraphicsSettingId::LensFlareThreshold: result.lensFlare.threshold = *scalar; break;
				case GraphicsSettingId::VignetteIntensity: result.presentation.vignetteIntensity = *scalar; break;
				case GraphicsSettingId::VignetteRadius: result.presentation.vignetteRadius = *scalar; break;
				case GraphicsSettingId::VignetteSoftness: result.presentation.vignetteSoftness = *scalar; break;
				case GraphicsSettingId::FilmGrainIntensity: result.presentation.grainIntensity = *scalar; break;
				case GraphicsSettingId::FilmGrainResponse: result.presentation.grainResponse = *scalar; break;
				case GraphicsSettingId::ChromaticAberrationPixels: result.presentation.chromaticPixels = *scalar; break;
				case GraphicsSettingId::LensDistortionK1: result.presentation.distortionK1 = *scalar; break;
				case GraphicsSettingId::LensDistortionK2: result.presentation.distortionK2 = *scalar; break;
				case GraphicsSettingId::VolumetricFogDensity: result.atmosphere.density = *scalar; break;
				case GraphicsSettingId::VolumetricFogHeightFalloff: result.atmosphere.heightFalloff = *scalar; break;
				case GraphicsSettingId::VolumetricFogBaseHeight: result.atmosphere.baseHeight = *scalar; break;
				case GraphicsSettingId::VolumetricFogMaxDistance: result.atmosphere.maxDistance = *scalar; break;
				case GraphicsSettingId::SunShaftsIntensity: result.atmosphere.shaftsIntensity = *scalar; break;
				default: break;
				}
				if(id == GraphicsSettingId::LensDirtTexture) if(const auto texture = std::get_if<TerraEngine::UUID>(&value.value)) result.lensDirt.texture = *texture;
			}
			return result;
		}
	}

	PostProcessEffectCapability PostProcessEffectSettings::RequestedCapabilities() const noexcept
	{
		auto result = Capability::None;
		if(gtao.enabled) result = result | Capability::GTAO;
		if(reflections.enabled) result = result | Capability::SSR;
		if(indirect.enabled) result = result | Capability::SSGI;
		if(depthOfField.enabled) result = result | Capability::DepthOfField;
		if(motionBlur.enabled) result = result | Capability::MotionBlur;
		if(lensDirt.enabled) result = result | Capability::LensDirt;
		if(lensFlare.enabled) result = result | Capability::LensFlare;
		if(presentation.EnabledMask() != 0u) result = result | Capability::Presentation;
		if(atmosphere.fogEnabled || atmosphere.shaftsEnabled) result = result | Capability::Atmosphere;
		if(outputDithering) result = result | Capability::OutputDithering;
		return result;
	}
	bool IsPostProcessEffectSetting(GraphicsSettingId id) noexcept { return FindField(id) != nullptr; }
	GraphicsRequestReason ValidatePostProcessEffectValues(std::span<const GraphicsSettingValue> values) noexcept
	{
		// Use the descriptor ordinal so new stable IDs may include gaps.
		std::array<bool, Fields.size()> seen{};
		for(const auto& value : values)
		{
			const auto* field = FindField(value.id); if(!field) continue;
			const auto index = static_cast<std::size_t>(field - Fields.data());
			if(seen[index]) return GraphicsRequestReason::InvalidSetting;
			seen[index] = true;
			if(value.value.index() != static_cast<std::size_t>(field->kind)) return GraphicsRequestReason::InvalidValue;
			if(const auto scalar = std::get_if<float>(&value.value))
				if(!std::isfinite(*scalar) || *scalar < field->minimum || *scalar > field->maximum) return GraphicsRequestReason::InvalidValue;
			if(const auto quality = std::get_if<std::uint32_t>(&value.value))
				if(*quality > 2u) return GraphicsRequestReason::InvalidValue;
		}
		const auto settings = Decode(values);
		if(settings.lensDirt.Active() && settings.lensDirt.texture == TerraEngine::EMPTY_UUID) return GraphicsRequestReason::InvalidValue;
		return GraphicsRequestReason::None;
	}
	GraphicsRequestReason ResolvePostProcessEffectSettings(std::span<const GraphicsSettingValue> values, PostProcessEffectSettings& result) noexcept
	{
		const auto reason = ValidatePostProcessEffectValues(values);
		if(reason == GraphicsRequestReason::None) result = Decode(values);
		return reason;
	}
	GraphicsRequestReason ValidatePostProcessEffectCapabilities(const PostProcessEffectSettings& settings, PostProcessEffectCapability supported) noexcept
	{
		const auto requested = static_cast<std::uint32_t>(settings.RequestedCapabilities());
		return (requested & ~static_cast<std::uint32_t>(supported)) == 0u ? GraphicsRequestReason::None : GraphicsRequestReason::Unsupported;
	}
	void AppendPostProcessEffectDescriptors(std::vector<GraphicsSettingDescriptor>& descriptors, PostProcessEffectCapability supported)
	{
		for(const auto& field : Fields)
		{
			GraphicsSettingDescriptor descriptor;
			descriptor.id = field.id;
			descriptor.name = field.name; descriptor.units = field.units; descriptor.valueKind = field.kind;
			switch(field.kind)
			{
			case Kind::Boolean: descriptor.defaultValue = false; break;
			case Kind::Scalar: descriptor.defaultValue = field.defaultNumber; descriptor.minimum = field.minimum; descriptor.maximum = field.maximum; break;
			case Kind::Unsigned32: descriptor.defaultValue = static_cast<std::uint32_t>(field.defaultNumber); descriptor.minimum = 0u; descriptor.maximum = 2u; descriptor.options = {0u, 1u, 2u}; break;
			case Kind::Asset: descriptor.defaultValue = TerraEngine::EMPTY_UUID; break;
			default: break;
			}
			descriptor.gameScope = descriptor.cameraScope = true;
			// Even a parameter crossing zero can require resources/input history.
			// A rebuild boundary can reuse compatible owners after real preparation.
			descriptor.activation = GraphicsActivation::Rebuild;
			descriptor.available = HasPostProcessEffectCapability(supported, field.capability);
			descriptor.unavailableReason = descriptor.available ? GraphicsRequestReason::None : GraphicsRequestReason::Unsupported;
			descriptor.detail = descriptor.available
				? "Native capability qualifies this effect; exact view inputs, assets and pass resources still prepare atomically."
				: "The native renderer/view has not qualified this effect's inputs, formats and executable passes.";
			descriptors.push_back(std::move(descriptor));
		}
	}
}
