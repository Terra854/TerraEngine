// Resolve opt-in color stages without consulting a live camera or device.
#include "PostProcessColorSettings.h"

#include <cmath>
#include <type_traits>
#include <utility>

namespace TerraEngine::Renderer::GraphicsSettings
{
	PostProcessColorSettings ResolvePostProcessColorSettings(std::span<const GraphicsSettingValue> values) noexcept
	{
		PostProcessColorSettings result;
		for(const auto& entry : values)
		{
			const auto copy = [&]<class Value>(Value& destination) noexcept {
				if(const auto* source = std::get_if<Value>(&entry.value))
				{
					if constexpr(std::is_same_v<Value, float>)
						if(!std::isfinite(*source)) return;
					destination = *source;
				}
			};
			// Exposure and tone mapping publish independent option ranges.
			const auto copyMode = [&]<class Mode>(Mode& destination, Mode last) noexcept {
				if(const auto* option = std::get_if<std::uint32_t>(&entry.value); option && *option <= static_cast<std::uint32_t>(last))
					destination = static_cast<Mode>(*option);
			};
			switch(entry.id)
			{
			case GraphicsSettingId::FixedExposureCompensation: copy(result.compensationEV); break;
			case GraphicsSettingId::ExposureMode: copyMode(result.exposure, ExposureMode::Automatic); break;
			case GraphicsSettingId::AutoExposureMinEV: copy(result.minEV); break;
			case GraphicsSettingId::AutoExposureMaxEV: copy(result.maxEV); break;
			case GraphicsSettingId::AutoExposureBrightenSpeed: copy(result.brightenSpeed); break;
			case GraphicsSettingId::AutoExposureDarkenSpeed: copy(result.darkenSpeed); break;
			case GraphicsSettingId::ToneMappingMode: copyMode(result.tone, ToneMappingMode::GT7); break;
			case GraphicsSettingId::ColorGradingEnabled: copy(result.grading); break;
			case GraphicsSettingId::ColorTemperatureKelvin: copy(result.temperature); break;
			case GraphicsSettingId::ColorTint: copy(result.tint); break;
			case GraphicsSettingId::ColorContrast: copy(result.contrast); break;
			case GraphicsSettingId::ColorSaturation: copy(result.saturation); break;
			case GraphicsSettingId::ColorLift: copy(result.lift); break;
			case GraphicsSettingId::ColorGain: copy(result.gain); break;
			case GraphicsSettingId::ColorCurve: copy(result.curve); break;
			case GraphicsSettingId::ColorCurveStrength: copy(result.curveStrength); break;
			case GraphicsSettingId::ColorLut: copy(result.lut); break;
			case GraphicsSettingId::ColorLutStrength: copy(result.lutStrength); break;
			case GraphicsSettingId::BloomEnabled: copy(result.bloom); break;
			case GraphicsSettingId::BloomThreshold: copy(result.bloomThreshold); break;
			case GraphicsSettingId::BloomIntensity: copy(result.bloomIntensity); break;
			case GraphicsSettingId::BloomSpread: copy(result.bloomSpread); break;
			case GraphicsSettingId::BloomSoftKnee: copy(result.bloomSoftKnee); break;
			default: break;
			}
		}
		return result;
	}

	void AppendPostProcessColorDescriptors(std::vector<GraphicsSettingDescriptor>& descriptors, bool capable)
	{
		const PostProcessColorSettings defaults;
		const auto add = [&](GraphicsSettingId id, const char* name, const char* units, GraphicsValue value,
			GraphicsActivation activation, std::optional<GraphicsValue> minimum = {},
			std::optional<GraphicsValue> maximum = {}, std::vector<GraphicsValue> options = {}) {
			GraphicsSettingDescriptor entry;
			entry.id = id;
			entry.name = name;
			entry.units = units;
			entry.valueKind = static_cast<GraphicsValueKind>(value.index());
			entry.defaultValue = std::move(value);
			entry.minimum = std::move(minimum);
			entry.maximum = std::move(maximum);
			entry.options = std::move(options);
			entry.gameScope = true;
			entry.cameraScope = true;
			entry.activation = activation;
			entry.available = capable;
			entry.unavailableReason = capable ? GraphicsRequestReason::None : GraphicsRequestReason::Unsupported;
			if(!capable) entry.detail = "This renderer cannot prepare the required linear HDR image usage.";
			descriptors.push_back(std::move(entry));
		};
		using Id = GraphicsSettingId;
		constexpr auto live = GraphicsActivation::Live;
		constexpr auto rebuild = GraphicsActivation::Rebuild;
		add(Id::FixedExposureCompensation, "postProcess.fixedExposureCompensation", "EV", defaults.compensationEV, rebuild, -16.0f, 16.0f);
		add(Id::ExposureMode, "postProcess.exposureMode", "", 0u, rebuild, {}, {}, {0u, 1u, 2u});
		add(Id::AutoExposureMinEV, "postProcess.autoExposureMinEV", "EV", defaults.minEV, live, -16.0f, 16.0f);
		add(Id::AutoExposureMaxEV, "postProcess.autoExposureMaxEV", "EV", defaults.maxEV, live, -16.0f, 16.0f);
		add(Id::AutoExposureBrightenSpeed, "postProcess.autoExposureBrightenSpeed", "1/s", defaults.brightenSpeed, live, 0.0f, 32.0f);
		add(Id::AutoExposureDarkenSpeed, "postProcess.autoExposureDarkenSpeed", "1/s", defaults.darkenSpeed, live, 0.0f, 32.0f);
		add(Id::ToneMappingMode, "postProcess.toneMappingMode", "", static_cast<std::uint32_t>(defaults.tone), rebuild, {}, {},
			{0u, 1u, 2u, static_cast<std::uint32_t>(ToneMappingMode::GT7)});
		add(Id::ColorGradingEnabled, "postProcess.colorGradingEnabled", "", defaults.grading, rebuild);
		add(Id::ColorTemperatureKelvin, "postProcess.colorTemperatureKelvin", "K", defaults.temperature, live, 2000.0f, 15000.0f);
		add(Id::ColorTint, "postProcess.colorTint", "", defaults.tint, live, -1.0f, 1.0f);
		add(Id::ColorContrast, "postProcess.colorContrast", "", defaults.contrast, live, 0.0f, 3.0f);
		add(Id::ColorSaturation, "postProcess.colorSaturation", "", defaults.saturation, live, 0.0f, 3.0f);
		add(Id::ColorLift, "postProcess.colorLift", "", defaults.lift, live, -1.0f, 1.0f);
		add(Id::ColorGain, "postProcess.colorGain", "", defaults.gain, live, 0.0f, 8.0f);
		add(Id::ColorCurve, "postProcess.colorCurve", "", defaults.curve, rebuild);
		add(Id::ColorCurveStrength, "postProcess.colorCurveStrength", "", defaults.curveStrength, live, 0.0f, 1.0f);
		add(Id::ColorLut, "postProcess.colorLut", "", defaults.lut, rebuild);
		add(Id::ColorLutStrength, "postProcess.colorLutStrength", "", defaults.lutStrength, live, 0.0f, 1.0f);
		add(Id::BloomEnabled, "postProcess.bloomEnabled", "", defaults.bloom, rebuild);
		add(Id::BloomThreshold, "postProcess.bloomThreshold", "linear luminance", defaults.bloomThreshold, live, 0.0f, 64.0f);
		add(Id::BloomIntensity, "postProcess.bloomIntensity", "", defaults.bloomIntensity, rebuild, 0.0f, 8.0f);
		add(Id::BloomSpread, "postProcess.bloomSpread", "", defaults.bloomSpread, live, 0.0f, 2.0f);
		add(Id::BloomSoftKnee, "postProcess.bloomSoftKnee", "", defaults.bloomSoftKnee, live, 0.0f, 1.0f);
	}
}
