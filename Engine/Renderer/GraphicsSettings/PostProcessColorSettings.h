// Copied color settings shared by the graph, recorder, and request validation.
#pragma once

#include "GraphicsSettingsTypes.h"

namespace TerraEngine::Renderer::GraphicsSettings
{
	enum class ExposureMode : std::uint32_t { Legacy, Fixed, Automatic };
	// These values are persisted and mirrored by the C# API and shader.
	enum class ToneMappingMode : std::uint32_t { Legacy = 0, Neutral = 1, Filmic = 2, GT7 = 3 };

	struct PostProcessColorSettings
	{
		ExposureMode exposure = ExposureMode::Legacy;
		float compensationEV = 0.0f;
		float minEV = -8.0f;
		float maxEV = 8.0f;
		float brightenSpeed = 3.0f;
		float darkenSpeed = 2.0f;
		ToneMappingMode tone = ToneMappingMode::Legacy;
		bool grading = false;
		float temperature = 6500.0f;
		float tint = 0.0f;
		float contrast = 1.0f;
		float saturation = 1.0f;
		float lift = 0.0f;
		float gain = 1.0f;
		TerraEngine::UUID curve = TerraEngine::EMPTY_UUID;
		float curveStrength = 1.0f;
		TerraEngine::UUID lut = TerraEngine::EMPTY_UUID;
		float lutStrength = 1.0f;
		bool bloom = false;
		float bloomThreshold = 1.0f;
		float bloomIntensity = 0.25f;
		float bloomSpread = 1.0f;
		float bloomSoftKnee = 0.5f;

		// Keep the original nonzero fixed compensation shorthand usable.
		bool HasExposure() const noexcept { return exposure != ExposureMode::Legacy || compensationEV != 0.0f; }
		bool HasToneAndGrading() const noexcept { return tone != ToneMappingMode::Legacy || grading; }
		bool HasBloom() const noexcept { return bloom && bloomIntensity > 0.0f; }
		bool Active() const noexcept { return HasExposure() || HasBloom() || HasToneAndGrading(); }
	};

	PostProcessColorSettings ResolvePostProcessColorSettings(std::span<const GraphicsSettingValue>) noexcept;

	// Create authored defaults and ranges once for the selected renderer. The
	// controller validates the whole candidate before a view borrows it.
	void AppendPostProcessColorDescriptors(std::vector<GraphicsSettingDescriptor>& descriptors, bool capable);
}
