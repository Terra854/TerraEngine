// Copied screen-space, camera and presentation controls. These records own no
// engine/Vulkan pointers; the frame owner publishes them as immutable values.
#pragma once
#include "GraphicsSettingsTypes.h"

namespace TerraEngine::Renderer::GraphicsSettings
{
	// Each bit means the native receiver has qualified that complete effect for
	// the actual view. Shader files or general HDR support alone are insufficient.
	enum class PostProcessEffectCapability : std::uint32_t
	{
		None = 0,
		GTAO = 1u << 0u,
		SSR = 1u << 1u,
		SSGI = 1u << 2u,
		DepthOfField = 1u << 3u,
		MotionBlur = 1u << 4u,
		LensDirt = 1u << 5u,
		LensFlare = 1u << 6u,
		Presentation = 1u << 7u,
		Atmosphere = 1u << 8u,
		OutputDithering = 1u << 9u
	};
	constexpr PostProcessEffectCapability operator|(PostProcessEffectCapability left, PostProcessEffectCapability right) noexcept
	{
		return static_cast<PostProcessEffectCapability>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
	}
	constexpr bool HasPostProcessEffectCapability(PostProcessEffectCapability mask, PostProcessEffectCapability effect) noexcept
	{
		return (static_cast<std::uint32_t>(mask) & static_cast<std::uint32_t>(effect)) == static_cast<std::uint32_t>(effect);
	}

	enum class ScreenSpaceEffectQuality : std::uint32_t { Low = 0, Medium = 1, High = 2 };
	struct ScreenSpaceEffectControls
	{
		bool enabled = false;
		float intensity = 1.0f;
		ScreenSpaceEffectQuality quality = ScreenSpaceEffectQuality::Medium;
		bool Active() const noexcept { return enabled && intensity > 0.0f; }
	};
	struct DepthOfFieldControls
	{
		bool enabled = false;
		float focusDistance = 10.0f;
		float focusRange = 5.0f;
		float blurStrength = 8.0f;
		bool Active() const noexcept { return enabled && blurStrength > 0.0f; }
	};
	struct MotionBlurControls
	{
		bool enabled = false;
		float strength = 1.0f;
		float cameraContribution = 1.0f;
		float objectContribution = 1.0f;
		bool Active() const noexcept { return enabled && strength > 0.0f && (cameraContribution > 0.0f || objectContribution > 0.0f); }
	};
	struct LensDirtControls
	{
		bool enabled = false;
		float intensity = 1.0f;
		TerraEngine::UUID texture = TerraEngine::EMPTY_UUID;
		// Keep asset validity separate from contribution. Missing active assets
		// must reject instead of silently turning a requested effect into a no-op.
		bool Active() const noexcept { return enabled && intensity > 0.0f; }
	};
	struct LensFlareControls
	{
		bool enabled = false;
		float intensity = 0.25f;
		float threshold = 1.0f;
		bool Active() const noexcept { return enabled && intensity > 0.0f; }
	};
	struct LensPresentationControls
	{
		bool vignetteEnabled = false;
		float vignetteIntensity = 0.35f;
		float vignetteRadius = 0.65f;
		float vignetteSoftness = 0.25f;
		bool grainEnabled = false;
		float grainIntensity = 0.06f;
		float grainResponse = 1.0f;
		bool chromaticEnabled = false;
		float chromaticPixels = 2.0f;
		bool distortionEnabled = false;
		float distortionK1 = 0.05f;
		float distortionK2 = 0.0f;
		// These constants are adapter defaults, not unplanned user settings.
		static constexpr float kOpticalCenterX = 0.5f, kOpticalCenterY = 0.5f;
		bool Active() const noexcept
		{
			return (vignetteEnabled && vignetteIntensity > 0.0f) || (grainEnabled && grainIntensity > 0.0f)
				|| (chromaticEnabled && chromaticPixels != 0.0f) || (distortionEnabled && (distortionK1 != 0.0f || distortionK2 != 0.0f));
		}
		std::uint32_t EnabledMask() const noexcept
		{
			return (vignetteEnabled ? 1u : 0u) | (grainEnabled ? 2u : 0u) | (chromaticEnabled ? 4u : 0u) | (distortionEnabled ? 8u : 0u);
		}
	};
	// Density is measured at an authored absolute height. The shaft approximation
	// can run independently of the height fog and shares its depth input.
	struct AtmosphereControls
	{
		bool fogEnabled = false;
		float density = 0.015f;
		float heightFalloff = 0.02f;
		float baseHeight = 0.0f;
		float maxDistance = 500.0f;
		bool shaftsEnabled = false;
		float shaftsIntensity = 0.25f;
		bool Active() const noexcept { return (fogEnabled && density > 0.0f) || (shaftsEnabled && shaftsIntensity > 0.0f); }
	};
	struct PostProcessEffectSettings
	{
		ScreenSpaceEffectControls gtao, reflections, indirect;
		DepthOfFieldControls depthOfField;
		MotionBlurControls motionBlur;
		LensDirtControls lensDirt;
		LensFlareControls lensFlare;
		LensPresentationControls presentation;
		AtmosphereControls atmosphere;
		bool outputDithering = false;
		bool HasScreenSpace() const noexcept { return gtao.Active() || reflections.Active() || indirect.Active(); }
		bool HasHdrCameraEffects() const noexcept { return depthOfField.Active() || motionBlur.Active() || lensDirt.Active() || lensFlare.Active() || atmosphere.Active(); }
		bool Active() const noexcept { return HasScreenSpace() || HasHdrCameraEffects() || presentation.Active() || outputDithering; }
		// Requested flags determine availability even for a zero contribution.
		// Active() separately avoids allocating/recording identity-only work.
		PostProcessEffectCapability RequestedCapabilities() const noexcept;
	};

	bool IsPostProcessEffectSetting(GraphicsSettingId) noexcept;
	GraphicsRequestReason ValidatePostProcessEffectValues(std::span<const GraphicsSettingValue>) noexcept;
	// Validation and resolution are one atomic operation: a rejected tuple never
	// changes the caller's prior controls. Unrelated settings are ignored.
	GraphicsRequestReason ResolvePostProcessEffectSettings(std::span<const GraphicsSettingValue>, PostProcessEffectSettings&) noexcept;
	GraphicsRequestReason ValidatePostProcessEffectCapabilities(const PostProcessEffectSettings&, PostProcessEffectCapability) noexcept;
	// Registration never probes a device or advertises an implementation by
	// default. The protected native owner supplies independently qualified bits.
	void AppendPostProcessEffectDescriptors(std::vector<GraphicsSettingDescriptor>&,
		PostProcessEffectCapability supported = PostProcessEffectCapability::None);
}
