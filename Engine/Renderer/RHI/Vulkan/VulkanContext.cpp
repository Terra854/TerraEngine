// VulkanContext.cpp — Vulkan RHI Context Implementation
//
// Implements the core Vulkan rendering logic, including frame submission, synchronization, and command buffer management.
// It coordinates the execution of various render passes, such as shadow mapping, scene drawing, and UI rendering.
// This module is responsible for the final assembly and submission of graphics work to the Vulkan-compatible hardware.

#include "VulkanContext.h"

#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION

#include <cstdio>
#include "Core/Logger/Logger.h"
#include "Resource/ResourceManager.h"
#define VMA_LEAK_LOG_FORMAT(format, ...) do { \
		char _vma_buf[1024]; \
		std::snprintf(_vma_buf, sizeof(_vma_buf), (format), __VA_ARGS__); \
		TERRAENGINE_LOG(LogLevel::Error, "[VMA] " << _vma_buf); \
	} while(false)

#include "VulkanCommonIncludes.h"
#include "VulkanContextImpl.h"
#include "ThreadPool/ThreadPool.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace
{
	constexpr std::uint32_t kMaxSceneDynamicLights = 1024u;
	constexpr float kDirectionalShadowNear = 0.1f;
	constexpr float kDirectionalShadowFar = 180.0f;
	constexpr float kDirectionalShadowExtent = 40.0f;
	constexpr float kTemporalResetDistance = 25.0f;
	constexpr float kTemporalResetMatrixEpsilon = 0.75f;
	constexpr float kDepthHistoryMatrixEpsilon = 0.0005f;
	constexpr std::uint64_t kTemporalStateHistoryFrames = 240u;

	// struct ScoredCandidate groups related state and operations so callers have one documented contract.
	struct ScoredCandidate
	{
		float score = 0.0f;
		float baseScore = 0.0f;
		float screenInfluence = 0.0f;
		float gpuVisibilityScore = 0.0f;
		std::size_t uploadedIndex = 0u;
		bool stabilityBiasApplied = false;
	};

	// struct ShadowWinner groups related state and operations so callers have one documented contract.
	struct ShadowWinner
	{
		float score = 0.0f;
		std::size_t uploadedIndex = 0u;
		std::uint32_t slotIndex = kMaxShadowResourceSlots;
		bool reuseBonusApplied = false;
	};

	// GetMatrixRow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec4 GetMatrixRow(const glm::mat4& matrix, int rowIndex)
	{
		return glm::vec4(matrix[0][rowIndex], matrix[1][rowIndex], matrix[2][rowIndex], matrix[3][rowIndex]);
	}

	// NormalizePlane is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec4 NormalizePlane(const glm::vec4& plane)
	{
		const float length = glm::length(glm::vec3(plane));
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (length <= 1e-6f)
		{
			return glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
		}
		return plane / length;
	}

	// BuildFrustumPlanes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::array<glm::vec4, 6> BuildFrustumPlanes(const glm::mat4& viewProjection)
	{
		const glm::vec4 row0 = GetMatrixRow(viewProjection, 0);
		const glm::vec4 row1 = GetMatrixRow(viewProjection, 1);
		const glm::vec4 row2 = GetMatrixRow(viewProjection, 2);
		const glm::vec4 row3 = GetMatrixRow(viewProjection, 3);

		std::array<glm::vec4, 6> planes{};
		planes[0] = NormalizePlane(row3 + row0); // Left
		planes[1] = NormalizePlane(row3 - row0); // Right
		planes[2] = NormalizePlane(row3 + row1); // Bottom
		planes[3] = NormalizePlane(row3 - row1); // Top
		// The renderer uses zero-to-one clip-space depth, so the near plane comes
		// directly from row2. Using the OpenGL-style row3 + row2 form skews the
		// frustum test and can incorrectly reject visible light volumes as the
		// camera rotates or moves away.
		planes[4] = NormalizePlane(row2); // Near
		planes[5] = NormalizePlane(row3 - row2); // Far
		return planes;
	}

	// GetEffectiveLightInfluenceRadius is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	float GetEffectiveLightInfluenceRadius(const FrameLightCandidate& candidate)
	{
		const GPULight& light = candidate.gpuLight;
		float influenceRadius = std::max(light.directionAndRange.w, 0.001f);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((candidate.candidateFlags & kFrameLightCandidateFlagSpecialCost) != 0u)
		{
			const float specialExtent = std::max(light.coneAnglesAndArea.z, light.coneAnglesAndArea.w);
			influenceRadius = std::max(influenceRadius, specialExtent);
		}
		return influenceRadius;
	}

	// SphereIntersectsFrustum is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool SphereIntersectsFrustum(const glm::vec3& center, float radius, const std::array<glm::vec4, 6>& planes)
	{
		for (const glm::vec4& plane : planes)
		{
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (glm::dot(glm::vec3(plane), center) + plane.w < -radius)
			{
				return false;
			}
		}
		return true;
	}

	// SafeFrameTimeMs is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	float SafeFrameTimeMs(float deltaTimeSeconds)
	{
		if (deltaTimeSeconds <= 0.0f)
		{
			return 0.0f;
		}
		return deltaTimeSeconds * 1000.0f;
	}

	// SafeNormalize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
	{
		const float len = glm::length(value);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!std::isfinite(len) || len <= 1e-5f)
		{
			return fallback;
		}
		return value / len;
	}

	// MatricesApproximatelyEqual is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool MatricesApproximatelyEqual(const glm::mat4& lhs, const glm::mat4& rhs, float epsilon)
	{
		for (int column = 0; column < 4; ++column)
		{
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (int row = 0; row < 4; ++row)
			{
				if (std::abs(lhs[column][row] - rhs[column][row]) > epsilon)
				{
					return false;
				}
			}
		}
		return true;
	}

	// GetLightType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::uint32_t GetLightType(const GPULight& light)
	{
		return static_cast<std::uint32_t>(std::max(light.positionAndType.w, 0.0f) + 0.5f);
	}

	// GetShadowSlotIndex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::uint32_t GetShadowSlotIndex(const FrameLightCandidate& candidate)
	{
		switch (GetLightType(candidate.gpuLight))
		{
		case 0u: return 0u;
		case 1u: return 1u;
		case 2u: return 2u;
		case 3u: return 3u;
		default: return kMaxShadowResourceSlots;
		}
	}

	// GetShadowResourceKind is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	ShadowResourceKind GetShadowResourceKind(const FrameLightCandidate& candidate)
	{
		switch (GetLightType(candidate.gpuLight))
		{
		case 0u: return ShadowResourceKind::DirectionalCascade;
		case 1u: return ShadowResourceKind::PointCube;
		case 2u: return ShadowResourceKind::SpotAtlas;
		case 3u: return ShadowResourceKind::AreaAtlas;
		default: return ShadowResourceKind::Unsupported;
		}
	}

	// EstimateShadowResourceBytes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::uint64_t EstimateShadowResourceBytes(ShadowResourceKind kind, ShadowQualityTier qualityTier = ShadowQualityTier::FullResolution)
	{
		constexpr std::uint64_t kDepthBytesPerPixel = 4u;
		constexpr std::uint64_t kColorBytesPerPixel = 4u;
		constexpr std::uint64_t kShadowResolution = 1024u;
		constexpr std::uint64_t kPointShadowResolution = 1024u;
		const std::uint64_t shadowScaleDivisor = qualityTier == ShadowQualityTier::ReducedResolution ? 4u : 1u;
		const std::uint64_t shadowTexels = (kShadowResolution * kShadowResolution) / shadowScaleDivisor;
		const std::uint64_t pointShadowTexels = kPointShadowResolution * kPointShadowResolution * 6u;
		// The estimate intentionally counts depth and color attachments because
		// the current shadow render pass binds both for every allocated region.
		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (kind)
		{
		case ShadowResourceKind::DirectionalCascade:
		case ShadowResourceKind::SpotAtlas:
		case ShadowResourceKind::AreaAtlas:
			return shadowTexels * (kDepthBytesPerPixel + kColorBytesPerPixel);
		case ShadowResourceKind::PointCube:
			return pointShadowTexels * (kDepthBytesPerPixel + kColorBytesPerPixel);
		default:
			return 0u;
		}
	}

	float ComputeSceneInfluenceScore(
		const FrameLightCandidate& candidate,
		const glm::vec3& cameraWorldPosition,
		const std::array<glm::vec4, 6>& frustumPlanes)
	{
		const GPULight& light = candidate.gpuLight;
		const std::uint32_t lightType = GetLightType(light);
		const float authoredPriority = std::max(light.extraParams.w, 0.0f);
		const float luminousIntensity = std::max(light.colorAndIntensity.w, 0.0f);
		const float colorWeight = std::max({ light.colorAndIntensity.x, light.colorAndIntensity.y, light.colorAndIntensity.z, 0.0f });
		const float effectiveRange = GetEffectiveLightInfluenceRadius(candidate);
		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const float distanceToCamera = glm::length(lightPosition - cameraWorldPosition);
		const float distanceOutsideInfluence = std::max(distanceToCamera - effectiveRange, 0.0f);
		const bool intersectsFrustum = ((candidate.candidateFlags & kFrameLightCandidateFlagDirectional) != 0u)
			|| SphereIntersectsFrustum(lightPosition, effectiveRange, frustumPlanes);
		const float specialExtent = std::max(light.coneAnglesAndArea.z, light.coneAnglesAndArea.w);
		float score = luminousIntensity * std::max(colorWeight, 0.1f);
		score += authoredPriority * 100000.0f;
		// Prefer lights whose influence volume can actually reach the current
		// view. The previous camera-distance heuristic dropped valid lights once
		// the camera moved away, even while those lights were still affecting
		// visible meshes.
		score += intersectsFrustum ? 50000.0f : 0.0f;
		score += effectiveRange * 25.0f;
		score += ((candidate.candidateFlags & kFrameLightCandidateFlagShadowCapable) != 0u) ? 1000000.0f : 0.0f;
		score += ((candidate.candidateFlags & kFrameLightCandidateFlagDirectional) != 0u) ? 2000000.0f : 0.0f;
		score += ((candidate.candidateFlags & kFrameLightCandidateFlagSpecialCost) != 0u) ? specialExtent * 150.0f : 0.0f;
		score -= distanceOutsideInfluence * 2.0f;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (lightType == 0u)
		{
			score += 500000.0f;
		}
		return score;
	}

	float ComputeProjectedScreenInfluence(
		const FrameLightCandidate& candidate,
		const glm::mat4& viewMatrix,
		const glm::mat4& projectionMatrix,
		const std::array<glm::vec4, 6>& frustumPlanes)
	{
		const GPULight& light = candidate.gpuLight;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((candidate.candidateFlags & kFrameLightCandidateFlagDirectional) != 0u)
		{
			return 1.0f;
		}

		const float influenceRadius = GetEffectiveLightInfluenceRadius(candidate);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!SphereIntersectsFrustum(glm::vec3(light.positionAndType), influenceRadius, frustumPlanes))
		{
			return 0.0f;
		}

		const glm::vec4 viewPosition = viewMatrix * glm::vec4(glm::vec3(light.positionAndType), 1.0f);
		const float depth = -viewPosition.z;
		const glm::vec4 clipPosition = projectionMatrix * viewPosition;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!std::isfinite(clipPosition.w) || std::abs(clipPosition.w) <= 1e-5f)
		{
			const float intensity = std::max(light.colorAndIntensity.w, 0.0f);
			return 0.1f * (1.0f + intensity);
		}

		const glm::vec2 ndc = glm::vec2(clipPosition) / clipPosition.w;
		const glm::vec2 outside = glm::max(glm::abs(ndc) - glm::vec2(1.0f), glm::vec2(0.0f));
		const float centerDistanceOutside = glm::length(outside);
		const float projectionScale = std::max(std::abs(projectionMatrix[1][1]), 0.5f);
		const float projectedRadius = (influenceRadius / std::max(depth - influenceRadius, 0.1f)) * projectionScale;
		const float visibility = std::max(projectedRadius - centerDistanceOutside, 0.0f);
		const float intensity = std::max(light.colorAndIntensity.w, 0.0f);
		return std::max(visibility * visibility * (1.0f + intensity), 0.1f * (1.0f + intensity));
	}

	// ShouldResetTemporalViewState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool ShouldResetTemporalViewState(bool stateValid, const glm::vec3& lastCameraWorldPosition, const glm::mat4& lastProjectionMatrix, const FrameRenderData& frameData)
	{
		if (!stateValid)
		{
			return false;
		}

		const float cameraDistance = glm::length(frameData.cameraWorldPosition - lastCameraWorldPosition);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (cameraDistance > kTemporalResetDistance)
		{
			return true;
		}

		return !MatricesApproximatelyEqual(frameData.projectionMatrix, lastProjectionMatrix, kTemporalResetMatrixEpsilon);
	}

	// ComputeDirectionalShadowCenter is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 ComputeDirectionalShadowCenter(const GPULight& light, const glm::vec3& cameraWorldPosition)
	{
		const glm::vec3 lightDirection = SafeNormalize(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float texelWorldSize = (kDirectionalShadowExtent * 2.0f) / 1024.0f;

		// Directional shadows follow the camera, but snapping the light-space
		// center to whole shadow texels keeps the shadow map from crawling as the
		// camera moves by sub-texel amounts.
		const glm::vec3 provisionalLightPosition = cameraWorldPosition - lightDirection * 60.0f;
		const glm::mat4 provisionalView = glm::lookAtRH(provisionalLightPosition, cameraWorldPosition, worldUp);
		glm::vec4 snappedCenterLightSpace = provisionalView * glm::vec4(cameraWorldPosition, 1.0f);
		snappedCenterLightSpace.x = std::floor(snappedCenterLightSpace.x / texelWorldSize) * texelWorldSize;
		snappedCenterLightSpace.y = std::floor(snappedCenterLightSpace.y / texelWorldSize) * texelWorldSize;
		return glm::vec3(glm::inverse(provisionalView) * snappedCenterLightSpace);
	}

	// BuildDirectionalShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 BuildDirectionalShadowViewProjection(const GPULight& light, const glm::vec3& cameraWorldPosition)
	{
		const glm::vec3 lightDirection = SafeNormalize(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float shadowExtent = kDirectionalShadowExtent;
		const glm::vec3 snappedCenterWorld = ComputeDirectionalShadowCenter(light, cameraWorldPosition);

		const glm::vec3 lightPosition = snappedCenterWorld - lightDirection * 60.0f;
		glm::mat4 view = glm::lookAtRH(lightPosition, snappedCenterWorld, worldUp);
		glm::mat4 projection = glm::orthoRH_ZO(-shadowExtent, shadowExtent, -shadowExtent, shadowExtent, kDirectionalShadowNear, kDirectionalShadowFar);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

	// BuildSpotShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 BuildSpotShadowViewProjection(const GPULight& light)
	{
		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const glm::vec3 lightDirection = SafeNormalize(glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float range = std::max(light.directionAndRange.w, 0.5f);
		const float outerConeCos = std::clamp(light.coneAnglesAndArea.y, -0.999f, 0.999f);
		const float outerAngleRadians = std::max(0.1f, std::acos(outerConeCos) * 2.0f);
		glm::mat4 view = glm::lookAtRH(lightPosition, lightPosition + lightDirection, worldUp);
		glm::mat4 projection = glm::perspectiveRH_ZO(outerAngleRadians, 1.0f, 0.1f, range);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

	// BuildAreaShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 BuildAreaShadowViewProjection(const GPULight& light)
	{
		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const glm::vec3 lightDirection = SafeNormalize(glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float range = std::max(light.directionAndRange.w, 0.5f);
		const float width = std::max(light.coneAnglesAndArea.z, 0.5f);
		const float height = std::max(light.coneAnglesAndArea.w, 0.5f);
		const float fov = std::atan2(std::max(width, height), range) * 2.0f;
		glm::mat4 view = glm::lookAtRH(lightPosition, lightPosition + lightDirection, worldUp);
		glm::mat4 projection = glm::perspectiveRH_ZO(fov, 1.0f, 0.1f, range);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

	// BuildPointShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 BuildPointShadowViewProjection(const GPULight& light, uint32_t faceIndex)
	{
		static constexpr std::array<glm::vec3, 6> kFaceDirections = {
			glm::vec3(1.0f, 0.0f, 0.0f),
			glm::vec3(-1.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f),
			glm::vec3(0.0f, -1.0f, 0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f),
			glm::vec3(0.0f, 0.0f, -1.0f)
		};
		static constexpr std::array<glm::vec3, 6> kFaceUps = {
			glm::vec3(0.0f, -1.0f, 0.0f),
			glm::vec3(0.0f, -1.0f, 0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f),
			glm::vec3(0.0f, 0.0f, -1.0f),
			glm::vec3(0.0f, -1.0f, 0.0f),
			glm::vec3(0.0f, -1.0f, 0.0f)
		};

		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const float range = std::max(light.directionAndRange.w, 0.5f);
		const uint32_t safeFace = std::min(faceIndex, static_cast<uint32_t>(kFaceDirections.size() - 1u));
		const glm::vec3 targetDirection = kFaceDirections[safeFace];
		const glm::vec3 up = kFaceUps[safeFace];
		glm::mat4 view = glm::lookAtRH(lightPosition, lightPosition + targetDirection, up);
		glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, range);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

	// StoreShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void StoreShadowViewProjection(GPULight& light, const glm::mat4& viewProjection)
	{
		// GLM stores matrices as columns. Keep that column order in the GPU
		// payload so the fragment shader can reconstruct the exact same
		// clip-space position without relying on duplicated light math.
		light.shadowMatrixC0 = viewProjection[0];
		light.shadowMatrixC1 = viewProjection[1];
		light.shadowMatrixC2 = viewProjection[2];
		light.shadowMatrixC3 = viewProjection[3];
	}

	// LoadShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 LoadShadowViewProjection(const GPULight& light)
	{
		glm::mat4 viewProjection(1.0f);
		viewProjection[0] = light.shadowMatrixC0;
		viewProjection[1] = light.shadowMatrixC1;
		viewProjection[2] = light.shadowMatrixC2;
		viewProjection[3] = light.shadowMatrixC3;
		return viewProjection;
	}

	// ShouldMaterialCastShadow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool ShouldMaterialCastShadow(MaterialFileShadowBehavior behavior)
	{
		// Transparent materials are excluded from the first shadow milestone
		// because opaque fallback shadows would misrepresent glass, particles,
		// and other blended surfaces. Cutout and emissive-opacity materials keep
		// using the material shader clip path so silhouettes remain consistent.
		switch (behavior)
		{
		case MaterialFileShadowBehavior::TransparentNoShadow:
			return false;
		default:
			return true;
		}
	}
}

// VulkanContext::Impl::ApplyFrameLightBudgets is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VulkanContext::Impl::LightBudgetApplicationResult VulkanContext::Impl::ApplyFrameLightBudgets(const FrameRenderData& frameData, RenderViewType viewType)
{
	LightBudgetApplicationResult result{};
	result.diagnostics.viewType = viewType;
	result.diagnostics.activePreset = frameData.activePreset;
	result.diagnostics.customOverrideActive = frameData.customOverrideActive;
	result.diagnostics.candidateLightCount = static_cast<std::uint32_t>(std::min<std::size_t>(frameData.lightCandidates.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
	result.diagnostics.uploadedLightBudget = std::min(frameData.settings.maxUploadedLights, MAX_GLOBAL_LIGHTS);
	result.diagnostics.retainedLightBudget = std::min(frameData.settings.maxDynamicLights, result.diagnostics.uploadedLightBudget);
	result.diagnostics.shadowLightBudget = std::min(frameData.settings.maxShadowLights, std::min(result.diagnostics.retainedLightBudget, MAX_SHADOW_RESOURCE_SLOTS));
	result.diagnostics.specialLightBudget = std::min(frameData.settings.maxAreaLights, result.diagnostics.retainedLightBudget);
	result.diagnostics.gpuAssistActive = (lightAnalysisComputePipeline != VK_NULL_HANDLE);

	const std::uint32_t viewIndex = (viewType == RenderViewType::Scene) ? 1u : 0u;
	ViewLightingTemporalState& viewState = lightingTemporalStateByView[viewIndex];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ShouldResetTemporalViewState(viewState.valid, viewState.lastCameraWorldPosition, viewState.lastProjectionMatrix, frameData))
	{
		viewState.lights.clear();
		viewState.previousShadowOwners.fill(TerraEngine::EMPTY_UUID);
		viewState.valid = false;
	}
	viewState.frameSerial += 1u;
	const std::uint64_t frameSerial = viewState.frameSerial;

	const GpuLightAnalysisViewSnapshot previousGpuAnalysis = GetLatestGpuLightAnalysis(viewType);
	const std::array<glm::vec4, 6> frustumPlanes = BuildFrustumPlanes(frameData.projectionMatrix * frameData.viewMatrix);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (previousGpuAnalysis.valid)
	{
		result.diagnostics.gpuVisibleCandidateCount = previousGpuAnalysis.visibleCandidateCount;
	}

	// struct UploadScoredCandidate groups related state and operations so callers have one documented contract.
	struct UploadScoredCandidate
	{
		float score = 0.0f;
		std::size_t originalIndex = 0u;
	};

	std::vector<UploadScoredCandidate> uploadScores;
	uploadScores.reserve(frameData.lightCandidates.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t candidateIndex = 0; candidateIndex < frameData.lightCandidates.size(); ++candidateIndex)
	{
		UploadScoredCandidate entry{};
		entry.score = ComputeSceneInfluenceScore(frameData.lightCandidates[candidateIndex], frameData.cameraWorldPosition, frustumPlanes);
		entry.originalIndex = candidateIndex;
		uploadScores.push_back(entry);
	}
	std::stable_sort(uploadScores.begin(), uploadScores.end(), [](const UploadScoredCandidate& lhs, const UploadScoredCandidate& rhs) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (lhs.score == rhs.score)
		{
			return lhs.originalIndex < rhs.originalIndex;
		}
		return lhs.score > rhs.score;
	});
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (uploadScores.size() > static_cast<std::size_t>(result.diagnostics.uploadedLightBudget))
	{
		uploadScores.resize(result.diagnostics.uploadedLightBudget);
	}
	std::stable_sort(uploadScores.begin(), uploadScores.end(), [](const UploadScoredCandidate& lhs, const UploadScoredCandidate& rhs) {
		return lhs.originalIndex < rhs.originalIndex;
	});

	result.uploadedCandidates.reserve(uploadScores.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const UploadScoredCandidate& uploadScore : uploadScores)
	{
		result.uploadedCandidates.push_back(frameData.lightCandidates[uploadScore.originalIndex]);
	}
	result.diagnostics.uploadedLightCount = static_cast<std::uint32_t>(result.uploadedCandidates.size());
	result.diagnostics.uploadedReductionOccurred = result.diagnostics.uploadedLightCount < result.diagnostics.candidateLightCount;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (result.diagnostics.uploadedReductionOccurred)
	{
		result.diagnostics.reductionCategory = LightingReductionCategory::Uploaded;
	}

	std::vector<ScoredCandidate> scoredCandidates;
	scoredCandidates.reserve(result.uploadedCandidates.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t uploadedIndex = 0; uploadedIndex < result.uploadedCandidates.size(); ++uploadedIndex)
	{
		const FrameLightCandidate& candidate = result.uploadedCandidates[uploadedIndex];
		ScoredCandidate scored{};
		scored.uploadedIndex = uploadedIndex;
		scored.screenInfluence = ComputeProjectedScreenInfluence(candidate, frameData.viewMatrix, frameData.projectionMatrix, frustumPlanes);
		scored.gpuVisibilityScore = 0.0f;
		scored.baseScore = ComputeSceneInfluenceScore(candidate, frameData.cameraWorldPosition, frustumPlanes);
		scored.baseScore += scored.screenInfluence * 4000.0f;

		const auto gpuIt = previousGpuAnalysis.recordsByLightId.find(candidate.lightId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (gpuIt != previousGpuAnalysis.recordsByLightId.end())
		{
			scored.gpuVisibilityScore = std::max(gpuIt->second.gpuVisibilityScore, 0.0f);
			scored.baseScore += scored.gpuVisibilityScore * 250.0f;
		}

		const auto temporalIt = viewState.lights.find(candidate.lightId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (temporalIt != viewState.lights.end() && temporalIt->second.retainedLastFrame)
		{
			const float stabilityWeight = 0.08f + std::clamp(candidate.stabilityPreference, 0.0f, 1.0f) * 0.20f;
			scored.baseScore += std::max(2000.0f, std::abs(scored.baseScore) * stabilityWeight);
			scored.stabilityBiasApplied = true;
		}

		scored.score = scored.baseScore;
		scoredCandidates.push_back(scored);
	}

	std::vector<ScoredCandidate> sortedCandidates = scoredCandidates;
	std::stable_sort(sortedCandidates.begin(), sortedCandidates.end(), [](const ScoredCandidate& lhs, const ScoredCandidate& rhs) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (lhs.score == rhs.score)
		{
			return lhs.uploadedIndex < rhs.uploadedIndex;
		}
		return lhs.score > rhs.score;
	});

	std::vector<std::size_t> retainedIndices;
	retainedIndices.reserve(std::min<std::size_t>(sortedCandidates.size(), result.diagnostics.retainedLightBudget));
	std::uint32_t retainedSpecialCount = 0u;
	bool specialBudgetReduced = false;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ScoredCandidate& scored : sortedCandidates)
	{
		if (retainedIndices.size() >= static_cast<std::size_t>(result.diagnostics.retainedLightBudget))
		{
			break;
		}

		const FrameLightCandidate& candidate = result.uploadedCandidates[scored.uploadedIndex];
		const bool specialCost = (candidate.candidateFlags & kFrameLightCandidateFlagSpecialCost) != 0u;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (specialCost && retainedSpecialCount >= result.diagnostics.specialLightBudget)
		{
			specialBudgetReduced = true;
			continue;
		}

		retainedIndices.push_back(scored.uploadedIndex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (specialCost)
		{
			retainedSpecialCount += 1u;
		}
	}
	std::stable_sort(retainedIndices.begin(), retainedIndices.end());

	std::vector<bool> retainedMask(result.uploadedCandidates.size(), false);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t retainedIndex : retainedIndices)
	{
		retainedMask[retainedIndex] = true;
	}

	result.retainedLights.reserve(retainedIndices.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t retainedIndex : retainedIndices)
	{
		result.retainedLights.push_back(result.uploadedCandidates[retainedIndex]);
	}

	result.diagnostics.retainedLightCount = static_cast<std::uint32_t>(result.retainedLights.size());
	result.diagnostics.specialLightCount = retainedSpecialCount;
	result.diagnostics.specialReductionOccurred = specialBudgetReduced;
	result.diagnostics.budgetReductionOccurred = result.diagnostics.retainedLightCount < result.diagnostics.candidateLightCount
		|| result.diagnostics.specialReductionOccurred;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (result.diagnostics.reductionCategory == LightingReductionCategory::None
		&& result.diagnostics.specialReductionOccurred)
	{
		result.diagnostics.reductionCategory = LightingReductionCategory::Special;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (result.diagnostics.reductionCategory == LightingReductionCategory::None
		&& result.diagnostics.retainedLightCount < result.diagnostics.uploadedLightCount)
	{
		result.diagnostics.reductionCategory = LightingReductionCategory::Retained;
	}

	std::vector<TerraEngine::UUID> previousRetainedLights;
	previousRetainedLights.reserve(viewState.lights.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& [lightId, lightState] : viewState.lights)
	{
		if (lightState.retainedLastFrame)
		{
			previousRetainedLights.push_back(lightId);
		}
	}

	std::uint32_t retainedReplacementCount = 0u;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ScoredCandidate& scored : scoredCandidates)
	{
		if (!retainedMask[scored.uploadedIndex])
		{
			continue;
		}

		const FrameLightCandidate& candidate = result.uploadedCandidates[scored.uploadedIndex];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (scored.stabilityBiasApplied)
		{
			result.diagnostics.stabilityBiasAppliedCount += 1u;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((candidate.candidateFlags & kFrameLightCandidateFlagDirectional) != 0u)
		{
			result.diagnostics.retainedDirectionalLightCount += 1u;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (candidate.gpuLight.extraParams.w > 0.0f)
		{
			result.diagnostics.retainedAuthoredPriorityLightCount += 1u;
		}

		const auto temporalIt = viewState.lights.find(candidate.lightId);
		const bool retainedPreviously = temporalIt != viewState.lights.end() && temporalIt->second.retainedLastFrame;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!retainedPreviously)
		{
			retainedReplacementCount += 1u;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((candidate.candidateFlags & kFrameLightCandidateFlagShadowCapable) != 0u)
		{
			result.diagnostics.shadowEligibleLightCount += 1u;
		}
	}
	result.diagnostics.retainedLightReplacementCount = retainedReplacementCount;

	std::uint32_t retainedChurnCount = 0u;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const TerraEngine::UUID& previousLightId : previousRetainedLights)
	{
		bool stillRetained = false;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::size_t retainedIndex : retainedIndices)
		{
			if (result.uploadedCandidates[retainedIndex].lightId == previousLightId)
			{
				stillRetained = true;
				break;
			}
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!stillRetained)
		{
			retainedChurnCount += 1u;
		}
	}
	retainedChurnCount += retainedReplacementCount;
	result.diagnostics.retainedLightChurnCount = retainedChurnCount;

	std::vector<ShadowWinner> shadowWinners;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ScoredCandidate& scored : scoredCandidates)
	{
		if (!retainedMask[scored.uploadedIndex])
		{
			continue;
		}

		const FrameLightCandidate& candidate = result.uploadedCandidates[scored.uploadedIndex];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((candidate.candidateFlags & kFrameLightCandidateFlagShadowCapable) == 0u)
		{
			continue;
		}

		ShadowWinner winner{};
		winner.score = scored.score;
		winner.uploadedIndex = scored.uploadedIndex;
		winner.slotIndex = kMaxShadowResourceSlots;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::uint32_t previousSlotIndex = 0u; previousSlotIndex < MAX_SHADOW_RESOURCE_SLOTS; ++previousSlotIndex)
		{
			if (viewState.previousShadowOwners[previousSlotIndex] == candidate.lightId)
			{
				winner.score += 7500.0f;
				winner.slotIndex = previousSlotIndex;
				winner.reuseBonusApplied = true;
				break;
			}
		}

		const auto temporalIt = viewState.lights.find(candidate.lightId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (temporalIt != viewState.lights.end() && temporalIt->second.shadowedLastFrame)
		{
			winner.score += 1500.0f;
		}

		shadowWinners.push_back(winner);
	}
	std::stable_sort(shadowWinners.begin(), shadowWinners.end(), [](const ShadowWinner& lhs, const ShadowWinner& rhs) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (lhs.score == rhs.score)
		{
			return lhs.uploadedIndex < rhs.uploadedIndex;
		}
		return lhs.score > rhs.score;
	});
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (shadowWinners.size() > static_cast<std::size_t>(result.diagnostics.shadowLightBudget))
	{
		shadowWinners.resize(result.diagnostics.shadowLightBudget);
	}

	std::vector<bool> shadowedMask(result.uploadedCandidates.size(), false);
	std::vector<bool> shadowReuseMask(result.uploadedCandidates.size(), false);
	std::vector<bool> shadowReplacementMask(result.uploadedCandidates.size(), false);
	std::vector<std::uint32_t> assignedShadowSlots(result.uploadedCandidates.size(), kMaxShadowResourceSlots);
	std::array<TerraEngine::UUID, MAX_SHADOW_RESOURCE_SLOTS> currentShadowOwners{};
	std::array<std::uint32_t, 4> assignedShadowTypeCounts{};
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t winnerOrder = 0u; winnerOrder < shadowWinners.size(); ++winnerOrder)
	{
		ShadowWinner& winner = shadowWinners[winnerOrder];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (winner.uploadedIndex >= result.uploadedCandidates.size())
		{
			continue;
		}

		const FrameLightCandidate& candidate = result.uploadedCandidates[winner.uploadedIndex];
		const std::uint32_t lightType = GetLightType(candidate.gpuLight);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (lightType >= assignedShadowTypeCounts.size())
		{
			winner.slotIndex = kMaxShadowResourceSlots;
			continue;
		}

		std::uint32_t& typedSlotCounter = assignedShadowTypeCounts[lightType];
		winner.slotIndex = typedSlotCounter;
		typedSlotCounter += 1u;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (winner.slotIndex >= MAX_SHADOW_RESOURCE_SLOTS)
		{
			continue;
		}
		shadowedMask[winner.uploadedIndex] = true;
		shadowReuseMask[winner.uploadedIndex] = winner.reuseBonusApplied;
		assignedShadowSlots[winner.uploadedIndex] = winner.slotIndex;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (winnerOrder < currentShadowOwners.size())
		{
			currentShadowOwners[winnerOrder] = candidate.lightId;
			shadowReplacementMask[winner.uploadedIndex] =
				viewState.previousShadowOwners[winnerOrder] != TerraEngine::EMPTY_UUID
				&& viewState.previousShadowOwners[winnerOrder] != candidate.lightId;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (winner.reuseBonusApplied)
		{
			result.diagnostics.shadowReuseCount += 1u;
		}
	}
	result.diagnostics.shadowReplacementCount = static_cast<std::uint32_t>(std::count(shadowReplacementMask.begin(), shadowReplacementMask.end(), true));

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (FrameLightCandidate& retainedCandidate : result.retainedLights)
	{
		retainedCandidate.gpuLight.shadowAndPadding.x = 0.0f;
		retainedCandidate.gpuLight.shadowAndPadding.y = -1.0f;
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t retainedVectorIndex = 0u; retainedVectorIndex < retainedIndices.size(); ++retainedVectorIndex)
	{
		const std::size_t uploadedIndex = retainedIndices[retainedVectorIndex];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (shadowedMask[uploadedIndex])
		{
			result.retainedLights[retainedVectorIndex].gpuLight.shadowAndPadding.x = 1.0f;
			result.retainedLights[retainedVectorIndex].gpuLight.shadowAndPadding.y = static_cast<float>(assignedShadowSlots[uploadedIndex]);
			result.diagnostics.shadowedLightCount += 1u;
		}

		const FrameLightCandidate& retainedCandidate = result.uploadedCandidates[uploadedIndex];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((retainedCandidate.candidateFlags & kFrameLightCandidateFlagShadowCapable) != 0u)
		{
			const std::uint32_t slotIndex = assignedShadowSlots[uploadedIndex] < kMaxShadowResourceSlots
				? assignedShadowSlots[uploadedIndex]
				: GetShadowSlotIndex(retainedCandidate);

			ShadowResourceAssignmentDiagnostic assignment{};
			assignment.lightId = retainedCandidate.lightId;
			assignment.resourceKind = GetShadowResourceKind(retainedCandidate);
			assignment.resourceSlot = slotIndex;
			assignment.storageRegionId = slotIndex;
			assignment.assigned = shadowedMask[uploadedIndex];
			assignment.atlasId = static_cast<std::uint32_t>(assignment.resourceKind);
			const bool assigned2DShadow = assignment.assigned && assignment.resourceKind != ShadowResourceKind::PointCube;
			const bool reduced2DShadow = assigned2DShadow && slotIndex >= 16u;
			assignment.qualityTier = reduced2DShadow
				? ShadowQualityTier::ReducedResolution
				: (assignment.assigned ? ShadowQualityTier::FullResolution : ShadowQualityTier::Disabled);
			assignment.regionX = 0u;
			assignment.regionY = 0u;
			assignment.regionWidth = reduced2DShadow ? 512u : 1024u;
			assignment.regionHeight = reduced2DShadow ? 512u : 1024u;
			assignment.estimatedBytes = assignment.assigned
				? EstimateShadowResourceBytes(assignment.resourceKind, assignment.qualityTier)
				: 0u;
			assignment.frameSerial = frameSerial;
			assignment.reusedPreviousOwner = shadowReuseMask[uploadedIndex];
			assignment.replacedPreviousOwner = assignment.assigned && shadowReplacementMask[uploadedIndex];
			assignment.degradationReason = reduced2DShadow
				? ShadowDegradationReason::StorageBudget
				: (assignment.assigned ? ShadowDegradationReason::None : ShadowDegradationReason::LightBudget);
			assignment.refreshReason = assignment.assigned
				? ShadowRefreshReason::RenderedThisFrame
				: ShadowRefreshReason::Unavailable;
			assignment.refreshedThisFrame = assignment.assigned;
			const auto temporalIt = viewState.lights.find(retainedCandidate.lightId);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (temporalIt != viewState.lights.end())
			{
				assignment.framesSinceLastSeen = frameSerial - temporalIt->second.lastSeenFrameSerial;
				assignment.retentionScore = temporalIt->second.lastCompositeScore;
				assignment.shadowedPreviousFrame = temporalIt->second.shadowedLastFrame;
			}
			result.diagnostics.shadowAssignments.push_back(assignment);
		}
	}

	result.diagnostics.configuredShadowCapacity = kMaxShadowResourceSlots;
	result.diagnostics.activeShadowCount = result.diagnostics.shadowedLightCount;
	result.diagnostics.allocatedDirectionalRegionCount = assignedShadowTypeCounts[0u];
	result.diagnostics.allocatedPointRegionCount = assignedShadowTypeCounts[1u];
	result.diagnostics.allocatedSpotRegionCount = assignedShadowTypeCounts[2u];
	result.diagnostics.allocatedAreaRegionCount = assignedShadowTypeCounts[3u];
	result.diagnostics.allocated2DRegionCount =
		result.diagnostics.allocatedDirectionalRegionCount
		+ result.diagnostics.allocatedSpotRegionCount
		+ result.diagnostics.allocatedAreaRegionCount;
	result.diagnostics.allocatedRegionCount =
		result.diagnostics.allocated2DRegionCount
		+ result.diagnostics.allocatedPointRegionCount;
	result.diagnostics.estimatedShadowMemoryBytes = 0u;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ShadowResourceAssignmentDiagnostic& assignment : result.diagnostics.shadowAssignments)
	{
		result.diagnostics.estimatedShadowMemoryBytes += assignment.estimatedBytes;
	}
	result.diagnostics.cacheRefreshCount = result.diagnostics.shadowedLightCount;
	result.diagnostics.degradedShadowCount = 0u;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ShadowResourceAssignmentDiagnostic& assignment : result.diagnostics.shadowAssignments)
	{
		if (assignment.degradationReason != ShadowDegradationReason::None)
		{
			result.diagnostics.degradedShadowCount += 1u;
		}
	}
	result.diagnostics.shadowReductionOccurred = result.diagnostics.shadowedLightCount < result.diagnostics.shadowEligibleLightCount;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (result.diagnostics.reductionCategory == LightingReductionCategory::None
		&& result.diagnostics.shadowReductionOccurred)
	{
		result.diagnostics.reductionCategory = LightingReductionCategory::Shadow;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& [lightId, lightState] : viewState.lights)
	{
		lightState.retainedLastFrame = false;
		lightState.shadowedLastFrame = false;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ScoredCandidate& scored : scoredCandidates)
	{
		const FrameLightCandidate& candidate = result.uploadedCandidates[scored.uploadedIndex];
		TemporalLightSelectionState& lightState = viewState.lights[candidate.lightId];
		lightState.lastCompositeScore = scored.score;
		lightState.lastGpuVisibilityScore = scored.gpuVisibilityScore;
		lightState.lastScreenInfluence = scored.screenInfluence;
		lightState.lastSeenFrameSerial = frameSerial;
		lightState.retainedLastFrame = retainedMask[scored.uploadedIndex];
		lightState.shadowedLastFrame = shadowedMask[scored.uploadedIndex];
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto stateIt = viewState.lights.begin(); stateIt != viewState.lights.end(); )
	{
		if ((frameSerial - stateIt->second.lastSeenFrameSerial) > kTemporalStateHistoryFrames)
		{
			stateIt = viewState.lights.erase(stateIt);
		}
		else
		{
			++stateIt;
		}
	}

	viewState.previousShadowOwners = currentShadowOwners;
	viewState.lastCameraWorldPosition = frameData.cameraWorldPosition;
	viewState.lastViewMatrix = frameData.viewMatrix;
	viewState.lastProjectionMatrix = frameData.projectionMatrix;
	viewState.valid = true;

	result.diagnostics.valid = true;
	return result;
}

VulkanContext::VulkanContext() : impl(std::make_unique<Impl>()) {
	impl->latestStatsByView[0].viewType = RenderViewType::Game;
	impl->latestStatsByView[1].viewType = RenderViewType::Scene;
	impl->latestBudgetDiagnosticsByView[0].viewType = RenderViewType::Game;
	impl->latestBudgetDiagnosticsByView[1].viewType = RenderViewType::Scene;
	impl->latestLightingDiagnosticsByView[0].viewType = RenderViewType::Game;
	impl->latestLightingDiagnosticsByView[1].viewType = RenderViewType::Scene;
}
VulkanContext::~VulkanContext() = default;

// VulkanContext::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Initialize(WindowSystem& windowSystem) {
	// Build the Vulkan instance/device chain and prime per-frame resources.
	impl->InitVulkan(windowSystem);
}

// VulkanContext::Resize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Resize(int width, int height, ViewType view) {
	if (impl) {
		impl->framebufferResized = true;
	}
}

// VulkanContext::BeginFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::BeginFrame() {
	// Wait until the GPU is done with the resources for this frame before reusing them.
	if (!impl->initialized) return;

	vkWaitForFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame], VK_TRUE, UINT64_MAX);
	// The waited frame slot is now complete, so snapshot its GPU counters for UI stats.
	impl->PublishCompletedFrameStats(impl->currentFrame);
	{
		std::lock_guard<std::mutex> lock(impl->statsMutex);
		impl->latestRenderSnapshotDiagnostics = impl->pendingRenderSnapshotDiagnostics;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (RenderSnapshotOwnerSummary& owner : impl->latestRenderSnapshotDiagnostics.ownerSummaries) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (owner.state == RenderSnapshotState::Submitted || owner.state == RenderSnapshotState::Recorded) {
				owner.state = RenderSnapshotState::Completed;
			}
		}
		impl->latestRenderSnapshotDiagnostics.completedSnapshotCount = static_cast<std::uint32_t>(impl->latestRenderSnapshotDiagnostics.ownerSummaries.size());
	}

	// Release any extra primary command buffers used for prior camera passes
	// now that the fence guarantees they are idle.
	if (impl->currentFrame < impl->extraPrimaryCommandBuffers.size()) {
		auto& extras = impl->extraPrimaryCommandBuffers[impl->currentFrame];
		if (!extras.empty()) {
			vkFreeCommandBuffers(impl->device, impl->commandPools[impl->currentFrame], static_cast<uint32_t>(extras.size()), extras.data());
			extras.clear();
		}
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->currentFrame < impl->pendingPrimaryCommandBuffers.size()) {
		impl->pendingPrimaryCommandBuffers[impl->currentFrame].clear();
	}
	impl->activeRenderSnapshot = {};
	impl->pendingRenderSnapshotDiagnostics = {};
	impl->pendingRenderSnapshotDiagnostics.valid = true;
	impl->pendingRenderSnapshotDiagnostics.maxSnapshotsPerFrame = VulkanContext::Impl::MAX_RENDER_SNAPSHOTS_PER_FRAME;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->currentFrame < impl->retiredBuffersByFrame.size()) {
		auto& retiredBuffers = impl->retiredBuffersByFrame[impl->currentFrame];
		for (VulkanBuffer& retiredBuffer : retiredBuffers) {
			impl->DestroyBufferImmediate(retiredBuffer);
		}
		retiredBuffers.clear();
	}

	// Reset command pool so the main command buffer and any newly allocated
	// per-camera buffers start from a clean state each frame.
	vkResetCommandPool(impl->device, impl->commandPools[impl->currentFrame], 0);
	// Reset worker pools for this frame once so secondary buffers can be rerecorded safely.
	if (impl->currentFrame < impl->workerCommandPools.size()) {
		auto& framePools = impl->workerCommandPools[impl->currentFrame];
		for (VkCommandPool pool : framePools) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (pool != VK_NULL_HANDLE) {
				vkResetCommandPool(impl->device, pool, 0);
			}
		}
	}
	// Reset usage counts for this frame
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->currentFrame < impl->workerBufferUsage.size()) {
		std::fill(impl->workerBufferUsage[impl->currentFrame].begin(), impl->workerBufferUsage[impl->currentFrame].end(), 0);
	} else if (impl->workerBufferUsage.size() <= impl->currentFrame) {
		impl->workerBufferUsage.resize(impl->MAX_FRAMES_IN_FLIGHT);
	}

	impl->swapchainImageAcquired = false;
	impl->skipFrame = false;

	// Acquire the swapchain image once per frame so multiple camera passes can
	// render to the same back buffer and present only on the final camera.
	if (!impl->editorMode) {
		VkResult result = vkAcquireNextImageKHR(impl->device, impl->swapchain, UINT64_MAX, impl->imageAvailableSemaphores[impl->currentFrame], VK_NULL_HANDLE, &impl->acquiredImageIndex);
		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			impl->RecreateSwapChain();
			impl->skipFrame = true;
			return;
		} else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to acquire swap chain image! Result: " << result);
			impl->skipFrame = true;
			return;
		}
		impl->swapchainImageAcquired = true;
	}
}

// Create Grid Pipeline for Scene View
// VulkanContext::Impl::CreateSceneViewGridPipeline is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateSceneViewGridPipeline(VkRenderPass renderPass) {
	if (sceneViewGridPipeline != VK_NULL_HANDLE) return;

	// Load SPIR-V
	auto loadShader = [&](const std::string& path) -> VkShaderModule {
		std::ifstream file(path, std::ios::ate | std::ios::binary);
		if (!file.is_open()) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to open shader file: " << path);
			return VK_NULL_HANDLE;
		}
		size_t fileSize = (size_t)file.tellg();
		std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
		file.seekg(0);
		file.read((char*)buffer.data(), fileSize);
		file.close();

		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = buffer.size() * sizeof(uint32_t);
		createInfo.pCode = buffer.data();

		VkShaderModule shaderModule;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module for: " << path);
			return VK_NULL_HANDLE;
		}
		return shaderModule;
	};

	// These editor/debug shaders stay as raw SPIR-V payloads.
	VkShaderModule vertShader = loadShader("Shaders/Editor/SceneViewGrid.vert.spv");
	VkShaderModule fragShader = loadShader("Shaders/Editor/SceneViewGrid.frag.spv");

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!vertShader || !fragShader) {
		TERRAENGINE_LOG(LogLevel::Warning, "Grid shaders not found. Grid will not be rendered.");
		if (vertShader) vkDestroyShaderModule(device, vertShader, nullptr);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (fragShader) vkDestroyShaderModule(device, fragShader, nullptr);
		return;
	}

	VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
	vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
	vertShaderStageInfo.module = vertShader;
	vertShaderStageInfo.pName = "VSMain";

	VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
	fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	fragShaderStageInfo.module = fragShader;
	fragShaderStageInfo.pName = "PSMainWithDepth";

	VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	VkViewport viewport{}; // Dynamic
	VkRect2D scissor{}; // Dynamic

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = VK_CULL_MODE_NONE;
	rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE; 
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	colorBlendAttachment.blendEnable = VK_TRUE;
	colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &colorBlendAttachment;

	std::vector<VkDynamicState> dynamicStates = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

	VkPushConstantRange pushConstantRange{};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(GridPushConstants);

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 0;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &sceneViewGridPipelineLayout) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create grid pipeline layout!");
		vkDestroyShaderModule(device, vertShader, nullptr);
		vkDestroyShaderModule(device, fragShader, nullptr);
		return;
	}

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = sceneViewGridPipelineLayout;
	pipelineInfo.renderPass = renderPass;
	pipelineInfo.subpass = 0;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &sceneViewGridPipeline) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create grid pipeline!");
	}

	vkDestroyShaderModule(device, vertShader, nullptr);
	vkDestroyShaderModule(device, fragShader, nullptr);
}

// VulkanContext::Impl::RenderSceneViewGrid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RenderSceneViewGrid(VkCommandBuffer cmd, const FrameRenderData& frameData) {
	if (sceneViewGridPipeline == VK_NULL_HANDLE) return;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneViewGridPipeline);

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = (float)swapchainExtent.width;
	viewport.height = (float)swapchainExtent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.offset = {0, 0};
	scissor.extent = swapchainExtent;
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	GridPushConstants push{};
	push.view = frameData.viewMatrix;
	push.proj = frameData.projectionMatrix;
	push.inverseView = glm::inverse(frameData.viewMatrix);
	push.inverseProj = glm::inverse(frameData.projectionMatrix);

	vkCmdPushConstants(cmd, sceneViewGridPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GridPushConstants), &push);

	vkCmdDraw(cmd, 6, 1, 0, 0); // 6 vertices for fullscreen quad
}

// struct SkyPushConstants groups related state and operations so callers have one documented contract.
struct SkyPushConstants
{
	glm::vec4 cameraOrientation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	glm::vec4 horizonTintIntensity = glm::vec4(1.0f);
	glm::vec4 zenithTintExposure = glm::vec4(1.0f);
	glm::vec4 groundTintPadding = glm::vec4(1.0f);
	glm::vec4 sunDirectionTextureIndex = glm::vec4(0.0f, 0.35f, -1.0f, 0.0f);
	glm::vec4 skyParams = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
};

// VulkanContext::Impl::CreateSkyBackgroundPipeline is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateSkyBackgroundPipeline(VkRenderPass renderPass)
{
	if (skyBackgroundPipeline != VK_NULL_HANDLE)
	{
		return;
	}

	// struct LoadedSkyShader groups related state and operations so callers have one documented contract.
	struct LoadedSkyShader
	{
		VkShaderModule module = VK_NULL_HANDLE;
		const char* entryPoint = nullptr;
	};

	auto loadShader = [&](const std::string& relativePath) -> LoadedSkyShader {
		LoadedSkyShader loadedShader{};
		const std::filesystem::path resolvedPath = ResourceManager::Instance().ResolveEngineAssetPath(relativePath);

		// Sky shaders now use the same packaged TSPF path as the default mesh
		// pipeline so task/mesh stages inherit the Vulkan 1.3 target flags and
		// task-payload patching performed by TerraEngineShaderCompiler.
		const TerraEngine::UUID shaderId = ResourceManager::Instance().LoadShader(resolvedPath);
		Shader* shader = ResourceManager::Instance().GetShader(shaderId);
		if (shader == nullptr || shader->bytecode.empty())
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to load packaged sky shader: " << resolvedPath);
			return loadedShader;
		}

		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = shader->bytecode.size();
		createInfo.pCode = reinterpret_cast<const std::uint32_t*>(shader->bytecode.data());

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateShaderModule(device, &createInfo, nullptr, &loadedShader.module) != VK_SUCCESS)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module for: " << resolvedPath);
			loadedShader.module = VK_NULL_HANDLE;
			return loadedShader;
		}

		loadedShader.entryPoint = shader->stringTable.data() + shader->binary.entryPointOffset;
		return loadedShader;
	};

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!taskShaderSupported)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Cannot create sky background pipeline without task shader support.");
		return;
	}

	const LoadedSkyShader taskShader = loadShader("Shaders/Sky/SkyBackground.task.tspf");
	const LoadedSkyShader meshShader = loadShader("Shaders/Sky/SkyBackground.mesh.tspf");
	const LoadedSkyShader fragShader = loadShader("Shaders/Sky/SkyBackground.frag.tspf");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (taskShader.module == VK_NULL_HANDLE || meshShader.module == VK_NULL_HANDLE || fragShader.module == VK_NULL_HANDLE)
	{
		if (taskShader.module != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(device, taskShader.module, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (meshShader.module != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(device, meshShader.module, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (fragShader.module != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(device, fragShader.module, nullptr);
		}
		return;
	}

	VkPipelineShaderStageCreateInfo shaderStages[3]{};
	shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
	shaderStages[0].module = taskShader.module;
	shaderStages[0].pName = taskShader.entryPoint;
	shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
	shaderStages[1].module = meshShader.module;
	shaderStages[1].pName = meshShader.entryPoint;
	shaderStages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[2].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[2].module = fragShader.module;
	shaderStages[2].pName = fragShader.entryPoint;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = VK_CULL_MODE_NONE;
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_FALSE;
	depthStencil.depthWriteEnable = VK_FALSE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	colorBlendAttachment.blendEnable = VK_FALSE;

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &colorBlendAttachment;

	std::array<VkDynamicState, 2> dynamicStates = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

	VkPushConstantRange pushConstantRange{};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(SkyPushConstants);

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &globalDescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &skyBackgroundPipelineLayout) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to create sky background pipeline layout!");
		vkDestroyShaderModule(device, taskShader.module, nullptr);
		vkDestroyShaderModule(device, meshShader.module, nullptr);
		vkDestroyShaderModule(device, fragShader.module, nullptr);
		return;
	}

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 3;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = nullptr;
	pipelineInfo.pInputAssemblyState = nullptr;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = skyBackgroundPipelineLayout;
	pipelineInfo.renderPass = renderPass;
	pipelineInfo.subpass = 0;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &skyBackgroundPipeline) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to create sky background pipeline!");
	}

	vkDestroyShaderModule(device, taskShader.module, nullptr);
	vkDestroyShaderModule(device, meshShader.module, nullptr);
	vkDestroyShaderModule(device, fragShader.module, nullptr);
}

// VulkanContext::Impl::RenderSkyBackground is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RenderSkyBackground(VkCommandBuffer cmd, const FrameRenderData& frameData)
{
	if (!frameData.sky.renderBackground || skyBackgroundPipeline == VK_NULL_HANDLE || skyBackgroundPipelineLayout == VK_NULL_HANDLE)
	{
		return;
	}

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyBackgroundPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyBackgroundPipelineLayout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = static_cast<float>(swapchainExtent.width);
	viewport.height = static_cast<float>(swapchainExtent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.offset = {0, 0};
	scissor.extent = swapchainExtent;
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	const glm::mat4 inverseViewMatrix = glm::inverse(frameData.viewMatrix);
	const glm::quat cameraOrientation = glm::normalize(glm::quat_cast(glm::mat3(inverseViewMatrix)));
	const float tanHalfFovX = 1.0f / std::max(std::abs(frameData.projectionMatrix[0][0]), 0.0001f);
	const float tanHalfFovY = 1.0f / std::max(std::abs(frameData.projectionMatrix[1][1]), 0.0001f);

	SkyPushConstants push{};
	push.cameraOrientation = glm::vec4(cameraOrientation.x, cameraOrientation.y, cameraOrientation.z, cameraOrientation.w);
	push.horizonTintIntensity = glm::vec4(frameData.sky.horizonTint, frameData.sky.intensity);
	push.zenithTintExposure = glm::vec4(frameData.sky.zenithTint, frameData.sky.exposure);
	push.groundTintPadding = glm::vec4(frameData.sky.groundTint, 0.0f);
	push.sunDirectionTextureIndex = glm::vec4(frameData.sky.sunDirection, 0.0f);
	std::uint32_t skyFlags = 0u;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameData.sky.hasTexture)
	{
		skyFlags |= 1u;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameData.sky.forceBlack)
	{
		skyFlags |= 2u;
	}
	push.skyParams = glm::vec4(tanHalfFovX, tanHalfFovY, glm::radians(frameData.sky.rotationOffsetDegrees), static_cast<float>(skyFlags));

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameData.sky.hasTexture)
	{
		EnsureTexture(frameData.sky.textureId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (const auto textureIndexIt = textureIndexMap.find(frameData.sky.textureId); textureIndexIt != textureIndexMap.end())
		{
			push.sunDirectionTextureIndex.w = static_cast<float>(textureIndexIt->second);
		}
	}

	vkCmdPushConstants(cmd, skyBackgroundPipelineLayout, VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkyPushConstants), &push);
	vkCmdDrawMeshTasksEXT(cmd, 1u, 1u, 1u);
}

// UI Rendering Implementation
// VulkanContext::Impl::CreateUIPipelines is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateUIPipelines(VkRenderPass renderPass) {
	auto loadShader = [&](const std::string& path) -> VkShaderModule {
		const std::filesystem::path shaderPath = ResourceManager::Instance().ResolveEngineAssetPath(path);
		const std::vector<std::uint8_t> shaderBytes = ResourceManager::Instance().ReadFileBytes(shaderPath);
		// SPIR-V shader modules must be non-empty and aligned to 32-bit words.
		if (shaderBytes.empty() || (shaderBytes.size() % sizeof(std::uint32_t)) != 0) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to load valid UI shader bytes: " << shaderPath);
			return VK_NULL_HANDLE;
		}

		std::vector<std::uint32_t> buffer(shaderBytes.size() / sizeof(std::uint32_t));
		std::memcpy(buffer.data(), shaderBytes.data(), shaderBytes.size());

		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = buffer.size() * sizeof(std::uint32_t);
		createInfo.pCode = buffer.data();

		VkShaderModule shaderModule = VK_NULL_HANDLE;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module for: " << shaderPath);
			return VK_NULL_HANDLE;
		}

		return shaderModule;
	};

	VkVertexInputBindingDescription bindingDescription{};
	bindingDescription.binding = 0;
	bindingDescription.stride = sizeof(float) * 8;
	bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription attributeDescriptions[3]{};
	attributeDescriptions[0].binding = 0;
	attributeDescriptions[0].location = 0;
	attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
	attributeDescriptions[0].offset = 0;
	attributeDescriptions[1].binding = 0;
	attributeDescriptions[1].location = 1;
	attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
	attributeDescriptions[1].offset = sizeof(float) * 2;
	attributeDescriptions[2].binding = 0;
	attributeDescriptions[2].location = 2;
	attributeDescriptions[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributeDescriptions[2].offset = sizeof(float) * 4;

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = 1;
	vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
	vertexInputInfo.vertexAttributeDescriptionCount = 3;
	vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = (float)swapchainExtent.width;
	viewport.height = (float)swapchainExtent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor{};
	scissor.offset = { 0, 0 };
	scissor.extent = swapchainExtent;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports = &viewport;
	viewportState.scissorCount = 1;
	viewportState.pScissors = &scissor;

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = VK_CULL_MODE_NONE;
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_FALSE;
	depthStencil.depthWriteEnable = VK_FALSE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	colorBlendAttachment.blendEnable = VK_TRUE;
	colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &colorBlendAttachment;

	VkPushConstantRange pushConstantRange{};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(glm::vec2) + (sizeof(uint32_t) * 2);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (staticUiPipelineLayout == VK_NULL_HANDLE) {
		VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = 1;
		pipelineLayoutInfo.pSetLayouts = &globalDescriptorSetLayout;
		pipelineLayoutInfo.pushConstantRangeCount = 1;
		pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &staticUiPipelineLayout) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create static UI pipeline layout!");
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (staticUiPipeline == VK_NULL_HANDLE && staticUiPipelineLayout != VK_NULL_HANDLE) {
		VkShaderModule staticVert = loadShader("Shaders/UI/UI_Static.vert.spv");
		VkShaderModule staticFrag = loadShader("Shaders/UI/UI_Static.frag.spv");
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!staticVert || !staticFrag) {
			TERRAENGINE_LOG(LogLevel::Warning, "UI Static shaders not found. UI rendering may be disabled.");
			if (staticVert) vkDestroyShaderModule(device, staticVert, nullptr);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (staticFrag) vkDestroyShaderModule(device, staticFrag, nullptr);
		} else {
			VkPipelineShaderStageCreateInfo shaderStages[2]{};
			shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			shaderStages[0].module = staticVert;
			shaderStages[0].pName = "VSMain";
			shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			shaderStages[1].module = staticFrag;
			shaderStages[1].pName = "PSMain";

			VkGraphicsPipelineCreateInfo pipelineInfo{};
			pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			pipelineInfo.stageCount = 2;
			pipelineInfo.pStages = shaderStages;
			pipelineInfo.pVertexInputState = &vertexInputInfo;
			pipelineInfo.pInputAssemblyState = &inputAssembly;
			pipelineInfo.pViewportState = &viewportState;
			pipelineInfo.pRasterizationState = &rasterizer;
			pipelineInfo.pMultisampleState = &multisampling;
			pipelineInfo.pDepthStencilState = &depthStencil;
			pipelineInfo.pColorBlendState = &colorBlending;
			pipelineInfo.layout = staticUiPipelineLayout;
			pipelineInfo.renderPass = renderPass;
			pipelineInfo.subpass = 0;

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &staticUiPipeline) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "failed to create static UI pipeline!");
			}

			vkDestroyShaderModule(device, staticVert, nullptr);
			vkDestroyShaderModule(device, staticFrag, nullptr);
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (uiFontDescriptorSetLayout == VK_NULL_HANDLE) {
		VkDescriptorSetLayoutBinding textBindings[2]{};
		textBindings[0].binding = 0;
		textBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		textBindings[0].descriptorCount = 1;
		textBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		textBindings[1].binding = 1;
		textBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		textBindings[1].descriptorCount = 1;
		textBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
		setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		setLayoutInfo.bindingCount = 2;
		setLayoutInfo.pBindings = textBindings;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &uiFontDescriptorSetLayout) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create UI font descriptor set layout!");
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (uiFontDescriptorSet == VK_NULL_HANDLE && uiFontDescriptorSetLayout != VK_NULL_HANDLE) {
		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = descriptorPool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &uiFontDescriptorSetLayout;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkAllocateDescriptorSets(device, &allocInfo, &uiFontDescriptorSet) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to allocate UI font descriptor set!");
			uiFontDescriptorSet = VK_NULL_HANDLE;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (sdfTextPipelineLayout == VK_NULL_HANDLE && uiFontDescriptorSetLayout != VK_NULL_HANDLE) {
		VkPushConstantRange textPushConstantRange{};
		textPushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		textPushConstantRange.offset = 0;
		textPushConstantRange.size = sizeof(glm::vec2);

		VkPipelineLayoutCreateInfo textLayoutInfo{};
		textLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		textLayoutInfo.setLayoutCount = 1;
		textLayoutInfo.pSetLayouts = &uiFontDescriptorSetLayout;
		textLayoutInfo.pushConstantRangeCount = 1;
		textLayoutInfo.pPushConstantRanges = &textPushConstantRange;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreatePipelineLayout(device, &textLayoutInfo, nullptr, &sdfTextPipelineLayout) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create SDF text pipeline layout!");
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (sdfTextPipeline == VK_NULL_HANDLE && sdfTextPipelineLayout != VK_NULL_HANDLE) {
		VkShaderModule textVert = loadShader("Shaders/UI/UI_SDF_Text.vert.spv");
		VkShaderModule textFrag = loadShader("Shaders/UI/UI_SDF_Text.frag.spv");
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!textVert || !textFrag) {
			TERRAENGINE_LOG(LogLevel::Warning, "UI SDF text shaders not found. Runtime UI text rendering may be disabled.");
			if (textVert) vkDestroyShaderModule(device, textVert, nullptr);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (textFrag) vkDestroyShaderModule(device, textFrag, nullptr);
		} else {
			VkPipelineShaderStageCreateInfo shaderStages[2]{};
			shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			shaderStages[0].module = textVert;
			shaderStages[0].pName = "VSMain";
			shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			shaderStages[1].module = textFrag;
			shaderStages[1].pName = "PSMain";

			VkGraphicsPipelineCreateInfo pipelineInfo{};
			pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			pipelineInfo.stageCount = 2;
			pipelineInfo.pStages = shaderStages;
			pipelineInfo.pVertexInputState = &vertexInputInfo;
			pipelineInfo.pInputAssemblyState = &inputAssembly;
			pipelineInfo.pViewportState = &viewportState;
			pipelineInfo.pRasterizationState = &rasterizer;
			pipelineInfo.pMultisampleState = &multisampling;
			pipelineInfo.pDepthStencilState = &depthStencil;
			pipelineInfo.pColorBlendState = &colorBlending;
			pipelineInfo.layout = sdfTextPipelineLayout;
			pipelineInfo.renderPass = renderPass;
			pipelineInfo.subpass = 0;

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &sdfTextPipeline) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "failed to create SDF text pipeline!");
			}

			vkDestroyShaderModule(device, textVert, nullptr);
			vkDestroyShaderModule(device, textFrag, nullptr);
		}
	}
}

// struct UIStaticPushConstants groups related state and operations so callers have one documented contract.
struct UIStaticPushConstants
{
	glm::vec2 viewportSize;
	uint32_t textureIndex;
	uint32_t useTexture;
};

// VulkanContext::Impl::RenderUI is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RenderUI(VkCommandBuffer cmd, const UIRenderBatch& batch) {
	if (batch.staticQuads.empty() && batch.immediateQuads.empty() && batch.textRuns.empty() && batch.imageRuns.empty()) return;

	auto appendQuadVertices = [](std::vector<float>& vertices, const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, const glm::vec2& uvMin, const glm::vec2& uvMax) {
		const float x = position.x;
		const float y = position.y;
		const float w = size.x;
		const float h = size.y;

		auto addVertex = [&](float px, float py, float u, float v) {
			vertices.push_back(px); vertices.push_back(py);
			vertices.push_back(u); vertices.push_back(v);
			vertices.push_back(color.r); vertices.push_back(color.g);
			vertices.push_back(color.b); vertices.push_back(color.a);
		};

		addVertex(x, y, uvMin.x, uvMin.y);
		addVertex(x + w, y, uvMax.x, uvMin.y);
		addVertex(x, y + h, uvMin.x, uvMax.y);
		addVertex(x, y + h, uvMin.x, uvMax.y);
		addVertex(x + w, y, uvMax.x, uvMin.y);
		addVertex(x + w, y + h, uvMax.x, uvMax.y);
	};

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (staticUiPipeline != VK_NULL_HANDLE) {
		std::vector<float> quadVertices;
		struct ImageDraw {
			uint32_t firstVertex;
			std::string texturePath;
			glm::vec4 color;
		};
		std::vector<ImageDraw> imageDraws;

		quadVertices.reserve((batch.staticQuads.size() + batch.immediateQuads.size() + batch.imageRuns.size()) * 6 * 8);

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& quad : batch.staticQuads) {
			appendQuadVertices(quadVertices, quad.position, quad.size, quad.color, glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 1.0f));
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& quad : batch.immediateQuads) {
			appendQuadVertices(
				quadVertices,
				quad.position,
				quad.size,
				quad.color,
				glm::vec2(quad.uvRect.x, quad.uvRect.y),
				glm::vec2(quad.uvRect.z, quad.uvRect.w));
		}

		const uint32_t staticVertexCount = static_cast<uint32_t>(quadVertices.size() / 8);

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& image : batch.imageRuns) {
			ImageDraw draw;
			draw.firstVertex = static_cast<uint32_t>(quadVertices.size() / 8);
			draw.texturePath = image.texturePath;
			draw.color = image.color;
			imageDraws.push_back(draw);

			appendQuadVertices(quadVertices, image.position, image.size, image.color, glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 1.0f));
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!quadVertices.empty()) {
			const VkDeviceSize bufferSize = quadVertices.size() * sizeof(float);
			if (uiStaticVertexBuffer.buffer == VK_NULL_HANDLE || uiStaticVertexBuffer.size < bufferSize) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (uiStaticVertexBuffer.buffer != VK_NULL_HANDLE) {
					RetireBuffer(uiStaticVertexBuffer);
				}
				uiStaticVertexBuffer = CreateBuffer(bufferSize * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, "UIStaticVertexBuffer");
			}

			void* data = nullptr;
			vmaMapMemory(allocator, uiStaticVertexBuffer.allocation, &data);
			memcpy(data, quadVertices.data(), bufferSize);
			vmaUnmapMemory(allocator, uiStaticVertexBuffer.allocation);

			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, staticUiPipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, staticUiPipelineLayout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);

			VkBuffer vertexBuffers[] = { uiStaticVertexBuffer.buffer };
			VkDeviceSize offsets[] = { 0 };
			vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

			// Draw non-textured quads
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (staticVertexCount > 0) {
				UIStaticPushConstants push{};
				push.viewportSize = batch.viewportSize;
				push.textureIndex = 0;
				push.useTexture = 0;
				vkCmdPushConstants(cmd, staticUiPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(UIStaticPushConstants), &push);
				vkCmdDraw(cmd, staticVertexCount, 1, 0, 0);
			}

			// Draw each textured image run
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& draw : imageDraws) {
				UIStaticPushConstants push{};
				push.viewportSize = batch.viewportSize;
				push.useTexture = 0;
				push.textureIndex = 0;

				TerraEngine::UUID textureId = ResourceManager::Instance().ResolveAssetReference(draw.texturePath, AssetType::Texture);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (textureId != TerraEngine::EMPTY_UUID) {
					EnsureTexture(textureId);
					if (textureIndexMap.contains(textureId)) {
						push.textureIndex = textureIndexMap[textureId];
						push.useTexture = 1;
					}
				}				vkCmdPushConstants(cmd, staticUiPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(UIStaticPushConstants), &push);
				vkCmdDraw(cmd, 6, 1, draw.firstVertex, 0);
			}
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (batch.textRuns.empty() || sdfTextPipeline == VK_NULL_HANDLE || uiFontDescriptorSet == VK_NULL_HANDLE) {
		return;
	}

	const std::filesystem::path fontLogicalPath = "Fonts/Roboto-Medium.ttf";
	const std::filesystem::path fontPath = ResourceManager::Instance().ResolveEngineAssetPath(fontLogicalPath);
	const std::vector<std::uint8_t> fontBytes = ResourceManager::Instance().ReadFileBytes(fontPath);
	const std::vector<unsigned char> fontAtlasBytes(fontBytes.begin(), fontBytes.end());
	const UIFontAtlas& fontAtlas = uiTextRenderer.EnsureSignedDistanceFontAtlasFromBytes(fontLogicalPath.generic_string(), fontAtlasBytes, uiFontAtlasPixelHeight);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fontAtlas.ready || fontAtlas.width <= 0 || fontAtlas.height <= 0 || fontAtlas.pixels.empty()) {
		TERRAENGINE_LOG(LogLevel::Warning, "Runtime UI text atlas is unavailable. Missing or invalid font: " << fontPath);
		return;
	}

	auto destroyUIFontAtlasImage = [&]() {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (uiFontAtlasImage.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, uiFontAtlasImage.sampler, nullptr);
			uiFontAtlasImage.sampler = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (uiFontAtlasImage.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, uiFontAtlasImage.view, nullptr);
			uiFontAtlasImage.view = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (uiFontAtlasImage.image != VK_NULL_HANDLE && uiFontAtlasImage.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, uiFontAtlasImage.image, uiFontAtlasImage.allocation);
			uiFontAtlasImage.image = VK_NULL_HANDLE;
			uiFontAtlasImage.allocation = VK_NULL_HANDLE;
		}
		uiFontAtlasWidth = 0;
		uiFontAtlasHeight = 0;
	};

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (uiFontAtlasImage.image == VK_NULL_HANDLE || uiFontAtlasWidth != fontAtlas.width || uiFontAtlasHeight != fontAtlas.height) {
		destroyUIFontAtlasImage();

		const VkDeviceSize imageSize = static_cast<VkDeviceSize>(fontAtlas.pixels.size());
		VulkanBuffer stagingBuffer = CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "UIFontAtlasStagingBuffer");
		void* stagingData = nullptr;
		vmaMapMemory(allocator, stagingBuffer.allocation, &stagingData);
		memcpy(stagingData, fontAtlas.pixels.data(), static_cast<std::size_t>(imageSize));
		vmaUnmapMemory(allocator, stagingBuffer.allocation);

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = { static_cast<uint32_t>(fontAtlas.width), static_cast<uint32_t>(fontAtlas.height), 1 };
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = VK_FORMAT_R8_UNORM;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &uiFontAtlasImage.image, &uiFontAtlasImage.allocation, nullptr) != VK_SUCCESS) {
			vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
			TERRAENGINE_LOG(LogLevel::Error, "Failed to allocate runtime UI font atlas image.");
			return;
		}

		TransitionImageLayout(uiFontAtlasImage.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(stagingBuffer.buffer, uiFontAtlasImage.image, static_cast<uint32_t>(fontAtlas.width), static_cast<uint32_t>(fontAtlas.height));
		TransitionImageLayout(uiFontAtlasImage.image, imageInfo.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

		uiFontAtlasImage.view = CreateImageView(uiFontAtlasImage.image, imageInfo.format, VK_IMAGE_ASPECT_COLOR_BIT);

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.anisotropyEnable = VK_FALSE;
		samplerInfo.maxAnisotropy = 1.0f;
		samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		samplerInfo.compareEnable = VK_FALSE;
		samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateSampler(device, &samplerInfo, nullptr, &uiFontAtlasImage.sampler) != VK_SUCCESS) {
			destroyUIFontAtlasImage();
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create runtime UI font atlas sampler.");
			return;
		}

		uiFontAtlasWidth = fontAtlas.width;
		uiFontAtlasHeight = fontAtlas.height;

		VkDescriptorImageInfo sampledImageInfo{};
		sampledImageInfo.imageView = uiFontAtlasImage.view;
		sampledImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkDescriptorImageInfo samplerDescriptorInfo{};
		samplerDescriptorInfo.sampler = uiFontAtlasImage.sampler;

		VkWriteDescriptorSet writes[2]{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = uiFontDescriptorSet;
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[0].pImageInfo = &sampledImageInfo;
		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = uiFontDescriptorSet;
		writes[1].dstBinding = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		writes[1].pImageInfo = &samplerDescriptorInfo;
		vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
	}

	std::vector<float> textVertices;
	textVertices.reserve(batch.textRuns.size() * 96 * 8);

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const UITextRun& textRun : batch.textRuns) {
		if (textRun.text.empty() || textRun.fontSize <= 0.0f) {
			continue;
		}

		const float glyphScale = textRun.fontSize / std::max(fontAtlas.pixelHeight, 1.0f);
		float penX = textRun.position.x;
		float baselineY = textRun.position.y + textRun.fontSize;
		const float lineAdvance = textRun.fontSize * 1.2f;

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (char character : textRun.text) {
			if (character == '\n') {
				penX = textRun.position.x;
				baselineY += lineAdvance;
				continue;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (character == '\r') {
				continue;
			}

			const auto glyphIt = fontAtlas.glyphs.find(character);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (glyphIt == fontAtlas.glyphs.end()) {
				penX += textRun.fontSize * 0.35f;
				continue;
			}

			const UIFontGlyph& glyph = glyphIt->second;
			appendQuadVertices(
				textVertices,
				glm::vec2(penX + (glyph.bearing.x * glyphScale), baselineY + (glyph.bearing.y * glyphScale)),
				glyph.size * glyphScale,
				textRun.color,
				glyph.uvMin,
				glyph.uvMax);
			penX += glyph.advance * glyphScale;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (textVertices.empty()) {
		return;
	}

	const VkDeviceSize textBufferSize = textVertices.size() * sizeof(float);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (uiSdfVertexBuffer.buffer == VK_NULL_HANDLE || uiSdfVertexBuffer.size < textBufferSize) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (uiSdfVertexBuffer.buffer != VK_NULL_HANDLE) {
			RetireBuffer(uiSdfVertexBuffer);
		}
		uiSdfVertexBuffer = CreateBuffer(textBufferSize * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, "UISdfVertexBuffer");
	}

	void* textData = nullptr;
	vmaMapMemory(allocator, uiSdfVertexBuffer.allocation, &textData);
	memcpy(textData, textVertices.data(), textBufferSize);
	vmaUnmapMemory(allocator, uiSdfVertexBuffer.allocation);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sdfTextPipeline);

	glm::vec2 viewportSize = batch.viewportSize;
	vkCmdPushConstants(cmd, sdfTextPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::vec2), &viewportSize);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sdfTextPipelineLayout, 0, 1, &uiFontDescriptorSet, 0, nullptr);

	VkBuffer textVertexBuffers[] = { uiSdfVertexBuffer.buffer };
	VkDeviceSize textOffsets[] = { 0 };
	vkCmdBindVertexBuffers(cmd, 0, 1, textVertexBuffers, textOffsets);
	vkCmdDraw(cmd, static_cast<uint32_t>(textVertices.size() / 8), 1, 0, 0);
}

// VulkanContext::RenderFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::RenderFrame(const FrameRenderData& frameData, const RenderOutputRequest& outputRequest) {
	const ViewType view = (outputRequest.diagnosticsView == RenderViewType::Scene)
		? ViewType::Scene
		: ViewType::Game;
	RenderFrame(frameData, view, outputRequest);
}

// VulkanContext::RenderFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::RenderFrame(const FrameRenderData& frameData, ViewType view) {
	RenderOutputRequest outputRequest{};
	outputRequest.kind = (view == ViewType::Scene) ? RenderOutputKind::SceneView : RenderOutputKind::GameView;
	outputRequest.priority = RenderOutputPriority::Required;
	outputRequest.diagnosticsView = (view == ViewType::Scene) ? RenderViewType::Scene : RenderViewType::Game;
	RenderFrame(frameData, view, outputRequest);
}

// VulkanContext::RenderFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::RenderFrame(const FrameRenderData& frameData, ViewType view, const RenderOutputRequest& outputRequest) {
	// Skip rendering if initialization failed so we do not spam validation errors.
	if (!impl->initialized || impl->skipFrame) return;

	const uint32_t currentFrame = impl->currentFrame;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		impl->pendingPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
	}
	auto& pendingCmdsForSnapshot = impl->pendingPrimaryCommandBuffers[currentFrame];
	uint32_t snapshotSlotIndex = static_cast<uint32_t>(pendingCmdsForSnapshot.size());
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (snapshotSlotIndex >= VulkanContext::Impl::MAX_RENDER_SNAPSHOTS_PER_FRAME) {
		RenderSnapshotFallbackEvent fallback{};
		fallback.kind = outputRequest.kind;
		fallback.reason = RenderSnapshotFallbackReason::BudgetExhausted;
		fallback.outputId = outputRequest.outputId;
		fallback.required = outputRequest.priority == RenderOutputPriority::Required;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outputRequest.priority == RenderOutputPriority::Optional) {
			fallback.result = RenderSnapshotFallbackResult::SkippedOptionalOutput;
			impl->pendingRenderSnapshotDiagnostics.fallbackEvents.push_back(fallback);
			return;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!impl->editorMode) {
			fallback.result = RenderSnapshotFallbackResult::FrameRecovery;
			impl->pendingRenderSnapshotDiagnostics.fallbackEvents.push_back(fallback);
			TERRAENGINE_LOG(LogLevel::Error, "Render snapshot budget exhausted for a required runtime output; skipping the pass to avoid shared-buffer corruption.");
			return;
		}

		// Budget exhaustion is the only remaining correctness fallback that may
		// serialize editor views. Normal scene/game rendering stays queued and
		// uses separate snapshot ranges below.
		VkSubmitInfo pendingSubmitInfo{};
		pendingSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		pendingSubmitInfo.commandBufferCount = static_cast<uint32_t>(pendingCmdsForSnapshot.size());
		pendingSubmitInfo.pCommandBuffers = pendingCmdsForSnapshot.data();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkQueueSubmit(impl->graphicsQueue, 1, &pendingSubmitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to submit intermediate editor view command buffers after snapshot budget exhaustion");
			return;
		}
		vkQueueWaitIdle(impl->graphicsQueue);
		pendingCmdsForSnapshot.clear();
		fallback.result = RenderSnapshotFallbackResult::SerializedSafely;
		impl->pendingRenderSnapshotDiagnostics.fallbackEvents.push_back(fallback);
		snapshotSlotIndex = 0u;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!impl->editorMode && !impl->swapchainImageAcquired) {
		return;
	}

	// Handle Editor Mode resizing (lazy).
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->editorMode) {
		if (impl->sceneView.image == VK_NULL_HANDLE) {
			impl->InitEditorMode();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (impl->sceneView.framebuffer == VK_NULL_HANDLE) {
			impl->CreateOffscreenResources(impl->sceneView);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (impl->gameView.framebuffer == VK_NULL_HANDLE) {
			impl->CreateOffscreenResources(impl->gameView);
		}
	}

	// 1. Prepare All Resources (Pipelines, Meshes, Materials) BEFORE starting command recording.
	// This ensures descriptor set updates don't invalidate the recording command buffer.
	const TerraEngine::UUID defaultMeshCollectionId = ResourceManager::Instance().GetDefaultShaderCollectionID();
	const TerraEngine::UUID defaultTaskCollectionId = ResourceManager::Instance().GetDefaultTaskShaderCollectionID();
	const bool defaultTaskPipelineAvailable = impl->taskShaderSupported
		&& (ResourceManager::Instance().GetShaderCollection(defaultTaskCollectionId) != nullptr);

	VkRenderPass currentPass = impl->editorMode ? impl->offscreenRenderPass : impl->renderPass;

	const auto PrepareDrawResources = [&](const DrawCommand& command) {
		Material* material = ResourceManager::Instance().GetMaterial(command.materialId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!material) return;

		impl->EnsureMesh(command.meshId);
		
		TerraEngine::UUID collectionId = material->header.shaderCollectionId;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (collectionId == TerraEngine::EMPTY_UUID) {
			collectionId = defaultMeshCollectionId;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (collectionId == defaultMeshCollectionId && defaultTaskPipelineAvailable) {
			collectionId = defaultTaskCollectionId;
		}

		impl->EnsurePipeline(collectionId, currentPass, material->header.flags);
		impl->EnsureMaterialDescriptorSets(collectionId, command.materialId);
		
		// Fallback for failed task pipeline
		auto passIt = impl->pipelineCache.find(currentPass);
		if (passIt != impl->pipelineCache.end()) {
			PipelineKey pipeKey = {collectionId, material->header.flags};
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (passIt->second.find(pipeKey) == passIt->second.end() || passIt->second[pipeKey].pipeline == VK_NULL_HANDLE) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (collectionId == defaultTaskCollectionId) {
					impl->EnsurePipeline(defaultMeshCollectionId, currentPass, material->header.flags);
				}
			}
		}
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& command : frameData.commands) {
		PrepareDrawResources(command);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ShadowDrawList& shadowDrawList : frameData.shadowDrawLists) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const DrawCommand& command : shadowDrawList.commands) {
			PrepareDrawResources(command);
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameData.sky.hasTexture && frameData.sky.textureId != TerraEngine::EMPTY_UUID)
	{
		impl->EnsureTexture(frameData.sky.textureId);
	}

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	const bool useMainCommandBuffer = impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()
		|| impl->pendingPrimaryCommandBuffers[impl->currentFrame].empty();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (useMainCommandBuffer) {
		cmd = impl->commandBuffers[impl->currentFrame];
		vkResetCommandBuffer(cmd, 0);
	} else {
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = impl->commandPools[impl->currentFrame];
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkAllocateCommandBuffers(impl->device, &allocInfo, &cmd) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to allocate command buffer for additional camera pass");
			return;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (impl->currentFrame >= impl->extraPrimaryCommandBuffers.size()) {
			impl->extraPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
		}
		impl->extraPrimaryCommandBuffers[impl->currentFrame].push_back(cmd);
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
		return;
	}

	uint32_t imageIndex = impl->acquiredImageIndex;
	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!impl->editorMode) {
		renderPassInfo.renderPass = impl->renderPass;
		renderPassInfo.framebuffer = impl->framebuffers[imageIndex];
	} else {
		// Editor mode renders into per-view offscreen targets.
		renderPassInfo.renderPass = impl->offscreenRenderPass;
		renderPassInfo.framebuffer = (view == ViewType::Scene) ? impl->sceneView.framebuffer : impl->gameView.framebuffer;
	}

	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = impl->swapchainExtent;

	std::array<VkClearValue, 3> clearValues{};
	clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
	clearValues[1].depthStencil = {1.0f, 0};
	clearValues[2].color = {{0, 0, 0, 0}};
	renderPassInfo.clearValueCount = impl->UsesVrsAttachment() ? 3 : 2;
	renderPassInfo.pClearValues = clearValues.data();

	// Create Grid Pipeline if needed (lazy init).
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (view == ViewType::Scene) {
		impl->CreateSceneViewGridPipeline(renderPassInfo.renderPass);
	}
	impl->CreateSkyBackgroundPipeline(renderPassInfo.renderPass);

	const RenderViewType renderViewType = (view == ViewType::Scene) ? RenderViewType::Scene : RenderViewType::Game;
	const uint32_t viewIndex = (view == ViewType::Scene) ? 1u : 0u;
	const uint32_t statsSlotIndex = impl->GetStatsSlotIndex(impl->currentFrame, view);
	RenderSnapshotHandle renderSnapshot{};
	renderSnapshot.slotIndex = snapshotSlotIndex;
	renderSnapshot.statsSlotIndex = statsSlotIndex;
	renderSnapshot.lightDataOffset = impl->GetSnapshotLightDataOffset(snapshotSlotIndex);
	renderSnapshot.lightAnalysisInputOffset = impl->GetSnapshotLightAnalysisInputOffset(snapshotSlotIndex);
	renderSnapshot.clusterDataOffset = impl->GetSnapshotClusterDataOffset(snapshotSlotIndex);
	renderSnapshot.lightIndexOffset = impl->GetSnapshotLightIndexOffset(snapshotSlotIndex);
	renderSnapshot.clusterDepthOffset = impl->GetSnapshotClusterDepthOffset(snapshotSlotIndex);
	renderSnapshot.indirectDrawOffset = impl->GetSnapshotIndirectDrawOffset(snapshotSlotIndex);
	renderSnapshot.state = RenderSnapshotState::Acquired;
	renderSnapshot.valid = true;
	impl->activeRenderSnapshot = renderSnapshot;
	impl->pendingRenderSnapshotDiagnostics.valid = true;
	impl->pendingRenderSnapshotDiagnostics.maxSnapshotsPerFrame = VulkanContext::Impl::MAX_RENDER_SNAPSHOTS_PER_FRAME;
	impl->pendingRenderSnapshotDiagnostics.activeSnapshotCount += 1u;
	impl->pendingRenderSnapshotDiagnostics.peakSnapshotCount = std::max(
		impl->pendingRenderSnapshotDiagnostics.peakSnapshotCount,
		impl->pendingRenderSnapshotDiagnostics.activeSnapshotCount);
	RenderSnapshotOwnerSummary ownerSummary{};
	ownerSummary.kind = outputRequest.kind;
	ownerSummary.priority = outputRequest.priority;
	ownerSummary.state = RenderSnapshotState::Acquired;
	ownerSummary.targetSize = outputRequest.targetSize;
	ownerSummary.outputId = outputRequest.outputId;
	ownerSummary.slotIndex = snapshotSlotIndex;
	ownerSummary.lightingIsolated = true;
	ownerSummary.cullingIsolated = true;
	ownerSummary.clusteringIsolated = true;
	ownerSummary.shadowAssignmentsIsolated = true;
	ownerSummary.indirectDrawsIsolated = true;
	impl->pendingRenderSnapshotDiagnostics.ownerSummaries.push_back(ownerSummary);
	// Apply the long-term preset policy on the CPU before upload so clustered
	// lighting receives a predictable retained-light list and an independent
	// shadow subset while the lightweight GPU analysis path observes a larger
	// uploaded candidate set for the next frame.
	const VulkanContext::Impl::LightBudgetApplicationResult budgetResult = impl->ApplyFrameLightBudgets(frameData, renderViewType);
	const uint32_t requestedShadowSlotCount = std::min(budgetResult.diagnostics.shadowLightBudget, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS);
	VulkanContext::Impl::ShadowResourceRequest requestedShadowResources{};
	requestedShadowResources.directionalCount = std::min(budgetResult.diagnostics.allocatedDirectionalRegionCount, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS);
	requestedShadowResources.spotCount = std::min(budgetResult.diagnostics.allocatedSpotRegionCount, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS);
	requestedShadowResources.areaCount = std::min(budgetResult.diagnostics.allocatedAreaRegionCount, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS);
	requestedShadowResources.pointCount = std::min(budgetResult.diagnostics.allocatedPointRegionCount, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS);

	const auto EnsureShadowRenderResources = [&]() -> bool {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (requestedShadowSlotCount == 0u || requestedShadowResources.TotalCount() == 0u) {
			return true;
		}
		const bool directionalMissing = requestedShadowResources.directionalCount > 0u
			&& (impl->directionalShadowDepths[0].view == VK_NULL_HANDLE || impl->directionalShadowColors[0].view == VK_NULL_HANDLE);
		const bool spotMissing = requestedShadowResources.spotCount > 0u
			&& (impl->spotShadowDepths[0].view == VK_NULL_HANDLE || impl->spotShadowColors[0].view == VK_NULL_HANDLE);
		const bool areaMissing = requestedShadowResources.areaCount > 0u
			&& (impl->areaShadowDepths[0].view == VK_NULL_HANDLE || impl->areaShadowColors[0].view == VK_NULL_HANDLE);
		const bool pointMissing = requestedShadowResources.pointCount > 0u
			&& impl->pointShadowCubeDepths[0].view == VK_NULL_HANDLE;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!(impl->activeShadowResourceRequest == requestedShadowResources)
			|| directionalMissing
			|| spotMissing
			|| areaMissing
			|| pointMissing) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!impl->EnsureShadowRenderResources(requestedShadowResources)) {
				return false;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (impl->shadowRenderPass == VK_NULL_HANDLE) {
			VkAttachmentDescription colorAttachment{};
			colorAttachment.format = impl->swapchainImageFormat;
			colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription depthAttachment{};
			depthAttachment.format = impl->depthFormat;
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

			VkAttachmentReference colorAttachmentRef{};
			colorAttachmentRef.attachment = 0;
			colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentReference depthAttachmentRef{};
			depthAttachmentRef.attachment = 1;
			depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkSubpassDescription subpass{};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1;
			subpass.pColorAttachments = &colorAttachmentRef;
			subpass.pDepthStencilAttachment = &depthAttachmentRef;

			std::array<VkSubpassDependency, 2> dependencies{};
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

			std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };
			VkRenderPassCreateInfo renderPassInfo{};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			renderPassInfo.pAttachments = attachments.data();
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpass;
			renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
			renderPassInfo.pDependencies = dependencies.data();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vkCreateRenderPass(impl->device, &renderPassInfo, nullptr, &impl->shadowRenderPass) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Warning, "failed to create shadow render pass");
				impl->shadowRenderPass = VK_NULL_HANDLE;
				return false;
			}
		}

		const uint32_t pointShadowSize = VulkanContext::Impl::POINT_SHADOW_MAP_RESOLUTION;
		const auto Shadow2DRegionSizeForSlot = [](std::uint32_t typedSlot) {
			// Slots after the full-quality reserve are backed by smaller images,
			// so their framebuffers must be created at the same reduced extent.
			return typedSlot >= 16u
				? VulkanContext::Impl::SHADOW_MAP_RESOLUTION / 2u
				: VulkanContext::Impl::SHADOW_MAP_RESOLUTION;
		};

		auto EnsureFramebuffer = [&](VkFramebuffer& framebuffer, VkImageView colorView, VkImageView depthView, uint32_t width, uint32_t height) -> bool {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (framebuffer != VK_NULL_HANDLE) {
				return true;
			}
			std::array<VkImageView, 2> attachments = { colorView, depthView };
			VkFramebufferCreateInfo framebufferInfo{};
			framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			framebufferInfo.renderPass = impl->shadowRenderPass;
			framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			framebufferInfo.pAttachments = attachments.data();
			framebufferInfo.width = width;
			framebufferInfo.height = height;
			framebufferInfo.layers = 1;
			return vkCreateFramebuffer(impl->device, &framebufferInfo, nullptr, &framebuffer) == VK_SUCCESS;
		};

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t slotIndex = 0u; slotIndex < impl->activeShadowResourceRequest.directionalCount; ++slotIndex) {
			const std::uint32_t regionSize = Shadow2DRegionSizeForSlot(slotIndex);
			if (!EnsureFramebuffer(impl->directionalShadowFramebuffers[slotIndex], impl->directionalShadowColors[slotIndex].view, impl->directionalShadowDepths[slotIndex].view, regionSize, regionSize)) {
				return false;
			}
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t slotIndex = 0u; slotIndex < impl->activeShadowResourceRequest.spotCount; ++slotIndex) {
			const std::uint32_t regionSize = Shadow2DRegionSizeForSlot(slotIndex);
			if (!EnsureFramebuffer(impl->spotShadowFramebuffers[slotIndex], impl->spotShadowColors[slotIndex].view, impl->spotShadowDepths[slotIndex].view, regionSize, regionSize)) {
				return false;
			}
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t slotIndex = 0u; slotIndex < impl->activeShadowResourceRequest.areaCount; ++slotIndex) {
			const std::uint32_t regionSize = Shadow2DRegionSizeForSlot(slotIndex);
			if (!EnsureFramebuffer(impl->areaShadowFramebuffers[slotIndex], impl->areaShadowColors[slotIndex].view, impl->areaShadowDepths[slotIndex].view, regionSize, regionSize)) {
				return false;
			}
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t slotIndex = 0u; slotIndex < impl->activeShadowResourceRequest.pointCount; ++slotIndex) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (uint32_t faceIndex = 0u; faceIndex < 6u; ++faceIndex) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (impl->pointShadowColorFaceViewsBySlot[slotIndex][faceIndex] == VK_NULL_HANDLE || impl->pointShadowDepthFaceViewsBySlot[slotIndex][faceIndex] == VK_NULL_HANDLE) {
					return false;
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!EnsureFramebuffer(impl->pointShadowFaceFramebuffersBySlot[slotIndex][faceIndex], impl->pointShadowColorFaceViewsBySlot[slotIndex][faceIndex], impl->pointShadowDepthFaceViewsBySlot[slotIndex][faceIndex], pointShadowSize, pointShadowSize)) {
					return false;
				}
			}
		}

		return true;
	};

	const bool shadowResourcesReady = EnsureShadowRenderResources();

	std::vector<GPULight> analysisLightingPayload;
	analysisLightingPayload.reserve(budgetResult.uploadedCandidates.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const FrameLightCandidate& candidate : budgetResult.uploadedCandidates) {
		analysisLightingPayload.push_back(candidate.gpuLight);
	}

	std::vector<GPULight> lightingPayload;
	lightingPayload.reserve(budgetResult.retainedLights.size());
	const auto FindShadowAssignment = [&](const TerraEngine::UUID& lightId) -> const ShadowResourceAssignmentDiagnostic* {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ShadowResourceAssignmentDiagnostic& assignment : budgetResult.diagnostics.shadowAssignments) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (assignment.lightId == lightId) {
				return &assignment;
			}
		}
		return nullptr;
	};
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const FrameLightCandidate& candidate : budgetResult.retainedLights) {
		GPULight light = candidate.gpuLight;
		if (const ShadowResourceAssignmentDiagnostic* assignment = FindShadowAssignment(candidate.lightId)) {
			const float textureWidth = static_cast<float>(std::max(assignment->regionWidth, 1u));
			const float textureHeight = static_cast<float>(std::max(assignment->regionHeight, 1u));
			// Each current 2D assignment owns its own texture allocation. The
			// region vector still travels through the shader payload so a future
			// shared atlas can use offsets without changing the light layout.
			light.shadowRegion = glm::vec4(
				static_cast<float>(assignment->regionX) / textureWidth,
				static_cast<float>(assignment->regionY) / textureHeight,
				textureWidth / textureWidth,
				textureHeight / textureHeight);
		}
		const uint32_t lightType = static_cast<uint32_t>(std::max(0.0f, light.positionAndType.w));
		const bool castsShadows = light.shadowAndPadding.x > 0.5f;
		const float assignedShadowSlot = light.shadowAndPadding.y;
		light.shadowAndPadding.y = -1.0f;
		light.shadowAndPadding.z = static_cast<float>(impl->ltcAmplitudeTextureIndex);
		light.shadowAndPadding.w = static_cast<float>(impl->ltcInverseTextureIndex);
		// Every retained light must still reach the shader. Only the shadow
		// resource binding is conditional; non-shadowed retained lights still
		// contribute direct lighting in the forward pass.
		if (castsShadows) {
			// ApplyFrameLightBudgets stores the logical shadow slot in y. Keep
			// that CPU-side slot before y is repacked as the shader-visible
			// texture index, otherwise every shadowed light samples slot zero.
			const uint32_t shadowSlotIndex = static_cast<uint32_t>(std::clamp(assignedShadowSlot, 0.0f, static_cast<float>(VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS - 1u)));
			if (lightType == 0u) {
				const glm::mat4 lightViewProjection = BuildDirectionalShadowViewProjection(light, frameData.cameraWorldPosition);
				const glm::vec3 snappedShadowCenter = ComputeDirectionalShadowCenter(light, frameData.cameraWorldPosition);
				light.positionAndType.x = snappedShadowCenter.x;
				light.positionAndType.y = snappedShadowCenter.y;
				light.positionAndType.z = snappedShadowCenter.z;
				light.shadowAndPadding.y = static_cast<float>(impl->directionalShadowTextureIndices[shadowSlotIndex]);
				StoreShadowViewProjection(light, lightViewProjection);
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (lightType == 1u) {
				light.shadowAndPadding.y = static_cast<float>(shadowSlotIndex);
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (lightType == 2u) {
				light.shadowAndPadding.y = static_cast<float>(impl->spotShadowTextureIndices[shadowSlotIndex]);
				StoreShadowViewProjection(light, BuildSpotShadowViewProjection(light));
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (lightType == 3u) {
				light.shadowAndPadding.y = static_cast<float>(impl->areaShadowTextureIndices[shadowSlotIndex]);
				StoreShadowViewProjection(light, BuildAreaShadowViewProjection(light));
			}
		}
		lightingPayload.push_back(light);
	}
	const auto EnsureLightingComputePipelines = [&]() {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (impl->lightingComputePipelineLayout == VK_NULL_HANDLE) {
			VkPushConstantRange pushRange{};
			pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			pushRange.offset = 0;
			pushRange.size = 64; // Plenty of room for various compute push constants

			VkPipelineLayoutCreateInfo layoutInfo{};
			layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			layoutInfo.setLayoutCount = 1;
			layoutInfo.pSetLayouts = &impl->globalDescriptorSetLayout;
			layoutInfo.pushConstantRangeCount = 1;
			layoutInfo.pPushConstantRanges = &pushRange;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vkCreatePipelineLayout(impl->device, &layoutInfo, nullptr, &impl->lightingComputePipelineLayout) != VK_SUCCESS) {
				impl->lightingComputePipelineLayout = VK_NULL_HANDLE;
				return;
			}
		}

		auto LoadShaderModule = [&](const char* path) -> VkShaderModule {
			const std::filesystem::path shaderPath = ResourceManager::Instance().ResolveEngineAssetPath(path);
			const std::vector<std::uint8_t> shaderBytes = ResourceManager::Instance().ReadFileBytes(shaderPath);
			// SPIR-V shader modules must be non-empty and aligned to 32-bit words.
			if (shaderBytes.empty() || (shaderBytes.size() % sizeof(std::uint32_t)) != 0) {
				return VK_NULL_HANDLE;
			}
			std::vector<std::uint32_t> code(shaderBytes.size() / sizeof(std::uint32_t));
			std::memcpy(code.data(), shaderBytes.data(), shaderBytes.size());

			VkShaderModuleCreateInfo moduleInfo{};
			moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			moduleInfo.codeSize = shaderBytes.size();
			moduleInfo.pCode = code.data();
			VkShaderModule module = VK_NULL_HANDLE;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vkCreateShaderModule(impl->device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
				return VK_NULL_HANDLE;
			}
			return module;
		};

		auto CreateComputePipeline = [&](VkPipeline& pipeline, const char* shaderPath) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (pipeline != VK_NULL_HANDLE || impl->lightingComputePipelineLayout == VK_NULL_HANDLE) {
				return;
			}
			VkShaderModule module = LoadShaderModule(shaderPath);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (module == VK_NULL_HANDLE) {
				return;
			}

			VkPipelineShaderStageCreateInfo stageInfo{};
			stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			stageInfo.module = module;
			stageInfo.pName = "main";

			VkComputePipelineCreateInfo pipelineInfo{};
			pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			pipelineInfo.stage = stageInfo;
			pipelineInfo.layout = impl->lightingComputePipelineLayout;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vkCreateComputePipelines(impl->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
				pipeline = VK_NULL_HANDLE;
			}
			vkDestroyShaderModule(impl->device, module, nullptr);
		};

		CreateComputePipeline(impl->clusterGridComputePipeline, "Shaders/Lighting/ClusterGrid.comp.spv");
		CreateComputePipeline(impl->clusterDepthMaskComputePipeline, "Shaders/Lighting/ClusterDepthMask.comp.spv");
		CreateComputePipeline(impl->clusterLightCountComputePipeline, "Shaders/Lighting/ClusterLightCount.comp.spv");
		CreateComputePipeline(impl->clusterLightPrefixComputePipeline, "Shaders/Lighting/ClusterLightPrefix.comp.spv");
		CreateComputePipeline(impl->clusterLightFillComputePipeline, "Shaders/Lighting/ClusterLightFill.comp.spv");
		CreateComputePipeline(impl->lightAnalysisComputePipeline, "Shaders/Lighting/LightCull.comp.spv");
		CreateComputePipeline(impl->hiZComputePipeline, "Shaders/PostProcess/HiZGenerate.comp.spv");
		CreateComputePipeline(impl->vrsComputePipeline, "Shaders/PostProcess/VRSGenerate.comp.spv");
		CreateComputePipeline(impl->gtaoComputePipeline, "Shaders/PostProcess/GTAO.comp.spv");
		CreateComputePipeline(impl->gtaoBlurComputePipeline, "Shaders/PostProcess/GTAOBlur.comp.spv");
	};

	EnsureLightingComputePipelines();

	const glm::mat4 currentViewProjection = frameData.projectionMatrix * frameData.viewMatrix;
	VulkanContext::Impl::ViewDepthHistoryState& depthHistory = impl->depthHistoryByView[viewIndex];
	const bool firstPrimaryPassForFrame = useMainCommandBuffer;
	const bool previousDepthMatchesView = !impl->editorMode
		&& firstPrimaryPassForFrame
		&& depthHistory.valid
		&& MatricesApproximatelyEqual(currentViewProjection, depthHistory.lastViewProjectionMatrix, kDepthHistoryMatrixEpsilon);
	const bool hiZOcclusionActive = previousDepthMatchesView
		&& impl->hiZ.image != VK_NULL_HANDLE
		&& impl->hiZ.sampler != VK_NULL_HANDLE;

	FrameCullingDataGPU analysisCullingData{};
	analysisCullingData.frustumPlanes = BuildFrustumPlanes(currentViewProjection);
	analysisCullingData.cameraPositionAndEpsilon = glm::vec4(frameData.cameraWorldPosition, std::max(frameData.cullingEpsilon, 0.0001f));
	analysisCullingData.clusterGridAndLightCount = glm::uvec4(
		kLightingClusterDimX,
		kLightingClusterDimY,
		kLightingClusterDimZ,
		static_cast<uint32_t>(std::min<size_t>(analysisLightingPayload.size(), VulkanContext::Impl::MAX_GLOBAL_LIGHTS)));
	analysisCullingData.clusterDepthAndViewport = glm::vec4(
		0.1f,
		100.0f,
		static_cast<float>(std::max<uint32_t>(impl->swapchainExtent.width, 1u)),
		static_cast<float>(std::max<uint32_t>(impl->swapchainExtent.height, 1u)));
	analysisCullingData.viewMatrix = frameData.viewMatrix;
	analysisCullingData.invProjection = glm::inverse(frameData.projectionMatrix);
	analysisCullingData.renderSettings0 = glm::uvec4(
		frameData.settings.shadowResolution,
		frameData.settings.shadowFilteringSize,
		budgetResult.diagnostics.retainedLightBudget,
		frameData.settings.enableLTC ? 1u : 0u);
	analysisCullingData.renderSettings1 = glm::uvec4(
		frameData.settings.enableIBL ? 1u : 0u,
		frameData.settings.anisotropicFiltering,
		budgetResult.diagnostics.shadowLightBudget,
		budgetResult.diagnostics.uploadedLightBudget);
	// Upload artist-facing indirect and contact-darkening controls separately
	// from integer feature flags so HLSL receives the same type and alignment
	// that the CPU writes into the frame payload.
	analysisCullingData.renderSettings2 = glm::vec4(
		frameData.settings.indirectStrength,
		frameData.settings.ambientOcclusionStrength,
		previousDepthMatchesView ? static_cast<float>(frameData.settings.ambientOcclusionQuality) : 0.0f,
		hiZOcclusionActive ? 1.0f : 0.0f);
	analysisCullingData.globalTaskCullingEnabled = (frameData.globalTaskShaderCullingEnabled && impl->globalTaskShaderCullingEnabled) ? 1u : 0u;
	analysisCullingData.taskShaderSupported = impl->taskShaderSupported ? 1u : 0u;

	FrameCullingDataGPU cullingData = analysisCullingData;
	cullingData.clusterGridAndLightCount.w = static_cast<uint32_t>(std::min<size_t>(lightingPayload.size(), VulkanContext::Impl::MAX_GLOBAL_LIGHTS));

	const uint32_t clusterCount = kLightingClusterDimX * kLightingClusterDimY * kLightingClusterDimZ;
	const bool clusteredLightingAvailable = impl->lightingComputePipelineLayout != VK_NULL_HANDLE
		&& impl->clusterGridComputePipeline != VK_NULL_HANDLE
		&& impl->clusterDepthMaskComputePipeline != VK_NULL_HANDLE
		&& impl->clusterLightCountComputePipeline != VK_NULL_HANDLE
		&& impl->clusterLightPrefixComputePipeline != VK_NULL_HANDLE
		&& impl->clusterLightFillComputePipeline != VK_NULL_HANDLE;
	static bool loggedClusteredLightingUnavailable = false;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!clusteredLightingAvailable && cullingData.clusterGridAndLightCount.w > 0u) {
		// Warn once per outage so validation runs clearly explain why fragment
		// shading fell back to the full-light path instead of using cluster bins.
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!loggedClusteredLightingUnavailable) {
			TERRAENGINE_LOG(LogLevel::Warning, "Variable light bins are unavailable because one or more clustered-light compute pipelines failed to initialize. Falling back to full-light shading for this frame.");
			loggedClusteredLightingUnavailable = true;
		}
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (clusteredLightingAvailable) {
		loggedClusteredLightingUnavailable = false;
	}
	const bool gpuLightAnalysisAvailable = impl->lightingComputePipelineLayout != VK_NULL_HANDLE
		&& impl->lightAnalysisComputePipeline != VK_NULL_HANDLE;
	cullingData.clusteredLightingValid = clusteredLightingAvailable ? 1u : 0u;
	cullingData.clusteredLightingOverflowed = 0u;
	analysisCullingData.clusteredLightingValid = clusteredLightingAvailable ? 1u : 0u;
	analysisCullingData.clusteredLightingOverflowed = 0u;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (gpuLightAnalysisAvailable && !analysisLightingPayload.empty()) {
		struct AnalysisPushConstants
		{
			std::uint32_t lightCount = 0u;
			std::uint32_t outputOffset = 0u;
			std::uint32_t inputOffset = 0u;
			std::uint32_t cullingSlotIndex = 0u;
			std::uint32_t padding[12]{};
		};

		impl->UploadLightAnalysisInputData(analysisLightingPayload);
		impl->RecordPendingLightAnalysisCandidates(statsSlotIndex, budgetResult.uploadedCandidates);
		impl->UpdateFrameCullingData(statsSlotIndex, analysisCullingData);

		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->lightingComputePipelineLayout, 0, 1, &impl->globalDescriptorSets[currentFrame], 0, nullptr);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->lightAnalysisComputePipeline);

		AnalysisPushConstants push{};
		push.lightCount = static_cast<std::uint32_t>(std::min<size_t>(analysisLightingPayload.size(), VulkanContext::Impl::MAX_GLOBAL_LIGHTS));
		push.outputOffset = statsSlotIndex * VulkanContext::Impl::MAX_GLOBAL_LIGHTS;
		push.inputOffset = renderSnapshot.lightAnalysisInputOffset;
		push.cullingSlotIndex = statsSlotIndex;
		vkCmdPushConstants(cmd, impl->lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
		vkCmdDispatch(cmd, std::max<uint32_t>((push.lightCount + 63u) / 64u, 1u), 1u, 1u);

		VkMemoryBarrier analysisBarrier{};
		analysisBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		analysisBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		analysisBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &analysisBarrier, 0, nullptr, 0, nullptr);
	}
	else {
		impl->RecordPendingLightAnalysisCandidates(statsSlotIndex, {});
	}

	impl->UploadLightingData(lightingPayload);
	cullingData.clusteredLightingValid = clusteredLightingAvailable ? 1u : 0u;
	cullingData.clusteredLightingOverflowed = 0u;

	impl->UpdateFrameCullingData(statsSlotIndex, cullingData);
	impl->ResetMeshletCounters(statsSlotIndex);

	FrameBudgetDiagnosticsSnapshot pendingBudgetDiagnostics = budgetResult.diagnostics;
	pendingBudgetDiagnostics.viewType = renderViewType;
	impl->RecordPendingBudgetDiagnostics(statsSlotIndex, pendingBudgetDiagnostics);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->clusterValidationCaptureEnabled) {
		ClusterValidationSnapshot pendingClusterValidation{};
		pendingClusterValidation.valid = true;
		pendingClusterValidation.viewType = renderViewType;
		pendingClusterValidation.buildMetadata.clusterCount = clusterCount;
		pendingClusterValidation.buildMetadata.activeLightCount = cullingData.clusterGridAndLightCount.w;
		pendingClusterValidation.buildMetadata.lightIndexCapacity = static_cast<uint32_t>(std::min<std::uint64_t>(
			static_cast<std::uint64_t>(clusterCount) * static_cast<std::uint64_t>(cullingData.clusterGridAndLightCount.w),
			static_cast<std::uint64_t>(VulkanContext::Impl::MAX_LIGHT_INDEX_ENTRIES)));
		pendingClusterValidation.buildMetadata.clusterDataOffset = renderSnapshot.clusterDataOffset;
		pendingClusterValidation.buildMetadata.lightIndexOffset = renderSnapshot.lightIndexOffset;
		pendingClusterValidation.buildMetadata.clusteredLightingAvailable = clusteredLightingAvailable;
		impl->RecordPendingClusterValidationSnapshot(currentFrame, pendingClusterValidation);
	}

	// Prepare Indirect Buffer
	std::size_t shadowCommandSourceCount = 0u;
	for (const ShadowDrawList& shadowDrawList : frameData.shadowDrawLists) {
		shadowCommandSourceCount += shadowDrawList.commands.size();
	}
	std::vector<VkDrawMeshTasksIndirectCommandEXT> indirectCommands;
	// Reserve both the main-view commands and the light-specific shadow lists so
	// per-light culling does not thrash the indirect-command buffer on CPU.
	indirectCommands.reserve(frameData.commands.size() + shadowCommandSourceCount);

	// Hi-Z is generated from the depth buffer rendered by a previous stable
	// view. Editor scene/game views share one depth image, so history is disabled
	// there to prevent one docked viewport from culling against another.
	impl->GenerateHiZPyramid(cmd, impl->depthImageView);
	if (previousDepthMatchesView) {
		impl->GenerateAmbientOcclusion(cmd, cullingData);
	} else {
		impl->ambientOcclusionHistoryValid = false;
	}

	// Build the variable-length light list before graphics so clustered shading
	// reads a fresh contiguous range for every cluster.
	if (clusteredLightingAvailable) {
		struct ClusterBuildPushConstants
		{
			std::uint32_t cullingSlotIndex = 0u;
			std::uint32_t lightDataOffset = 0u;
			std::uint32_t clusterDataOffset = 0u;
			std::uint32_t lightIndexOffset = 0u;
			std::uint32_t clusterDepthOffset = 0u;
			std::uint32_t padding[11]{};
		};
		ClusterBuildPushConstants clusterPush{};
		clusterPush.cullingSlotIndex = statsSlotIndex;
		clusterPush.lightDataOffset = renderSnapshot.lightDataOffset;
		clusterPush.clusterDataOffset = renderSnapshot.clusterDataOffset;
		clusterPush.lightIndexOffset = renderSnapshot.lightIndexOffset;
		clusterPush.clusterDepthOffset = renderSnapshot.clusterDepthOffset;

		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->lightingComputePipelineLayout, 0, 1, &impl->globalDescriptorSets[currentFrame], 0, nullptr);
		vkCmdPushConstants(cmd, impl->lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(clusterPush), &clusterPush);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->clusterGridComputePipeline);
		vkCmdDispatch(cmd, (kLightingClusterDimX + 7u) / 8u, (kLightingClusterDimY + 7u) / 8u, std::max<uint32_t>(kLightingClusterDimZ, 1u));

		VkMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->clusterDepthMaskComputePipeline);
		vkCmdDispatch(cmd, (kLightingClusterDimX + 7u) / 8u, (kLightingClusterDimY + 7u) / 8u, 1u);
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->clusterLightCountComputePipeline);
		vkCmdDispatch(cmd, std::max<uint32_t>(clusterCount, 1u), 1u, 1u);
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->clusterLightPrefixComputePipeline);
		vkCmdDispatch(cmd, 1u, 1u, 1u);
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->clusterLightFillComputePipeline);
		vkCmdDispatch(cmd, std::max<uint32_t>(clusterCount, 1u), 1u, 1u);

		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT,
			0,
			1, &barrier,
			0, nullptr,
			0, nullptr
		);
	}

	// Prepare draw data.
	// struct PreparedDraw groups related state and operations so callers have one documented contract.
	struct PreparedDraw {
		VulkanMesh* mesh = nullptr;
		VulkanPipeline* pipeline = nullptr;
		std::vector<VkDescriptorSet> descriptorSets;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		PushConstants push{};
		std::uint32_t materialFlags = 0u;
		uint32_t indirectBufferIndex = 0;
	};
	std::vector<PreparedDraw> prepared;
	prepared.reserve(frameData.commands.size());
	std::vector<PreparedDraw> preparedShadow;
	preparedShadow.reserve(frameData.commands.size());
	// struct PreparedShadowList groups related state and operations so callers have one documented contract.
	struct PreparedShadowList {
		TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
		std::vector<PreparedDraw> draws;
	};
	std::vector<PreparedShadowList> preparedShadowLists;
	preparedShadowLists.reserve(frameData.shadowDrawLists.size());

	RenderStatsSnapshot pendingStats{};
	pendingStats.viewType = (view == ViewType::Scene) ? RenderViewType::Scene : RenderViewType::Game;
	pendingStats.frameTimeMs = SafeFrameTimeMs(frameData.deltaTimeSeconds);
	pendingStats.fps = pendingStats.frameTimeMs > 0.0f ? (1000.0f / pendingStats.frameTimeMs) : 0.0f;
	pendingStats.valid = true;

	const auto ResolvePipeline = [&](const TerraEngine::UUID& shaderCollectionId, VkRenderPass currentPass, uint32_t materialFlags) -> VulkanPipeline* {
		impl->EnsurePipeline(shaderCollectionId, currentPass, materialFlags);
		auto passIt = impl->pipelineCache.find(currentPass);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (passIt == impl->pipelineCache.end()) {
			return nullptr;
		}

		PipelineKey pipeKey = {shaderCollectionId, materialFlags};
		auto pipelineIt = passIt->second.find(pipeKey);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (pipelineIt == passIt->second.end()
			|| pipelineIt->second.pipeline == VK_NULL_HANDLE
			|| pipelineIt->second.layout == VK_NULL_HANDLE) {
			return nullptr;
		}

		return &pipelineIt->second;
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& command : frameData.commands) {
		Material* material = ResourceManager::Instance().GetMaterial(command.materialId);
		if (!material) continue;

		impl->EnsureMesh(command.meshId);
		auto meshIt = impl->meshCache.find(command.meshId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (meshIt == impl->meshCache.end()) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping draw because mesh GPU buffers are unavailable.");
			continue;
		}

		VkRenderPass currentPass = impl->editorMode ? impl->offscreenRenderPass : impl->renderPass;
		uint32_t materialFlags = material->header.flags;
		VulkanMesh& mesh = meshIt->second;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (mesh.meshShaderVertexBuffer.buffer == VK_NULL_HANDLE
			|| mesh.meshletVertexRemapBuffer.buffer == VK_NULL_HANDLE
			|| mesh.meshDecodeInfoBuffer.buffer == VK_NULL_HANDLE
			|| mesh.indexBuffer.buffer == VK_NULL_HANDLE
			|| mesh.meshletBuffer.buffer == VK_NULL_HANDLE
			|| mesh.meshletCount == 0) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping draw because meshlet payload buffers are unavailable.");
			continue;
		}

		TerraEngine::UUID collectionId = material->header.shaderCollectionId;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (collectionId == TerraEngine::EMPTY_UUID) {
			collectionId = defaultMeshCollectionId;
		}

		// Route default materials through the task pipeline whenever supported.
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (collectionId == defaultMeshCollectionId && defaultTaskPipelineAvailable) {
			collectionId = defaultTaskCollectionId;
		}

		VulkanPipeline* pipeline = ResolvePipeline(collectionId, currentPass, materialFlags);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (pipeline == nullptr && collectionId == defaultTaskCollectionId) {
			collectionId = defaultMeshCollectionId;
			pipeline = ResolvePipeline(collectionId, currentPass, materialFlags);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (pipeline == nullptr) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping draw because graphics pipeline creation failed.");
			continue;
		}

		impl->EnsureMaterialDescriptorSets(collectionId, command.materialId);
		VkDescriptorSet meshDescriptorSet = impl->EnsureMeshDescriptorSet(command.meshId, *pipeline);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (meshDescriptorSet == VK_NULL_HANDLE) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping draw because mesh shader descriptor bindings were not created.");
			continue;
		}

		const bool activeTaskCulling = pipeline->hasTaskStage
			&& frameData.globalTaskShaderCullingEnabled
			&& impl->globalTaskShaderCullingEnabled
			&& command.taskShaderCullingEnabled
			&& mesh.hasMeshletBounds;

		PushConstants push{};
		push.viewProj = frameData.projectionMatrix * frameData.viewMatrix;
		push.model = command.modelMatrix;
		push.textureIndex = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (material->header.textureCount > 0 && !material->textures.empty()) {
			TerraEngine::UUID texID = material->textures[0].textureUuid;
			if (impl->textureIndexMap.contains(texID)) {
				push.textureIndex = impl->textureIndexMap[texID];
			}
		}
		push.materialIndex = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (impl->materialIndexMap.contains(command.materialId)) {
			push.materialIndex = impl->materialIndexMap[command.materialId];
		}
		push.meshDataIndex = mesh.bindlessIndex;
		push.textureIndex = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (material->header.textureCount > 0 && !material->textures.empty()) {
			TerraEngine::UUID texID = material->textures[0].textureUuid;
			if (impl->textureIndexMap.contains(texID)) {
				push.textureIndex = impl->textureIndexMap[texID];
			}
		}
		push.vrsActive = 0u; 
		push.taskCullingEnabled = activeTaskCulling ? 1u : 0u;
		push.statsSlotIndex = statsSlotIndex;
		push.cullingEpsilon = std::max(frameData.cullingEpsilon, 0.0001f);
		push.lightDataOffset = renderSnapshot.lightDataOffset;
		push.clusterDataOffset = renderSnapshot.clusterDataOffset;
		push.lightIndexOffset = renderSnapshot.lightIndexOffset;
		push.indirectDrawOffset = renderSnapshot.indirectDrawOffset;

		PreparedDraw draw{};
		draw.mesh = &mesh;
		draw.pipeline = pipeline;
		draw.meshId = command.meshId;
		draw.materialId = command.materialId;
		// Set 0 is the global bindless heap; set 1 is per-mesh payload for mesh shader pipelines.
		draw.descriptorSets = {impl->globalDescriptorSets[currentFrame]};
		draw.descriptorSets.push_back(meshDescriptorSet);
		draw.push = push;
		draw.materialFlags = materialFlags;
		
		const uint32_t kTaskBatchSize = 32u;
		const uint32_t taskGroupCount = pipeline->hasTaskStage
			? ((mesh.meshletCount + (kTaskBatchSize - 1u)) / kTaskBatchSize)
			: mesh.meshletCount;

		VkDrawMeshTasksIndirectCommandEXT indirectCmd{};
		indirectCmd.groupCountX = taskGroupCount;
		indirectCmd.groupCountY = 1;
		indirectCmd.groupCountZ = 1;
		draw.indirectBufferIndex = static_cast<uint32_t>(indirectCommands.size());
		indirectCommands.push_back(indirectCmd);

		prepared.push_back(std::move(draw));

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (shadowResourcesReady && ShouldMaterialCastShadow(material->shadowBehavior)) {
			TerraEngine::UUID shadowCollectionId = collectionId;
			VulkanPipeline* shadowPipeline = ResolvePipeline(shadowCollectionId, impl->shadowRenderPass, materialFlags);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (shadowPipeline == nullptr && shadowCollectionId == defaultTaskCollectionId) {
				shadowCollectionId = defaultMeshCollectionId;
				shadowPipeline = ResolvePipeline(shadowCollectionId, impl->shadowRenderPass, materialFlags);
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (shadowPipeline != nullptr) {
				VkDescriptorSet shadowMeshDescriptorSet = impl->EnsureMeshDescriptorSet(command.meshId, *shadowPipeline);
				if (shadowMeshDescriptorSet != VK_NULL_HANDLE) {
					PreparedDraw shadowDraw{};
					shadowDraw.mesh = &mesh;
					shadowDraw.pipeline = shadowPipeline;
					shadowDraw.meshId = command.meshId;
					shadowDraw.materialId = command.materialId;
					shadowDraw.descriptorSets = {impl->globalDescriptorSets[currentFrame], shadowMeshDescriptorSet};
					shadowDraw.push = push;
					shadowDraw.materialFlags = materialFlags;
					// Shadow-map passes use a minimal fragment-shader branch:
					// alpha-tested materials can still clip, but lighting and
					// recursive shadow sampling are skipped while depth writes.
					shadowDraw.push.vrsActive = 2u;
					shadowDraw.push.taskCullingEnabled = 0u;
					
					VkDrawMeshTasksIndirectCommandEXT shadowIndirectCmd{};
					shadowIndirectCmd.groupCountX = taskGroupCount;
					shadowIndirectCmd.groupCountY = 1;
					shadowIndirectCmd.groupCountZ = 1;
					shadowDraw.indirectBufferIndex = static_cast<uint32_t>(indirectCommands.size());
					indirectCommands.push_back(shadowIndirectCmd);

					preparedShadow.push_back(std::move(shadowDraw));
				}
			}
		}

		pendingStats.drawCalls += 1u;
		pendingStats.triangleCount += (mesh.indexCount / 3u);
		pendingStats.meshletsTotal += mesh.meshletCount;
		pendingStats.taskCullingActive = pendingStats.taskCullingActive || activeTaskCulling;
		pendingStats.hiZActive = pendingStats.hiZActive || hiZOcclusionActive;
		pendingStats.vrsActive = pendingStats.vrsActive || impl->UsesVrsAttachment();

		// Mesh-only and pass-through fallback paths keep conservative CPU estimates.
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!pipeline->hasTaskStage || !activeTaskCulling) {
			pendingStats.meshletsDispatched += mesh.meshletCount;
		}
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ShadowDrawList& shadowDrawList : frameData.shadowDrawLists) {
		PreparedShadowList preparedList{};
		preparedList.lightId = shadowDrawList.lightId;
		preparedList.draws.reserve(shadowDrawList.commands.size());

		// The renderer already asked the spatial index for one list per light.
		// Rebuilding the shadow draws from that list preserves the culling win
		// all the way through pipeline selection and indirect submission.
		for (const DrawCommand& command : shadowDrawList.commands) {
			Material* material = ResourceManager::Instance().GetMaterial(command.materialId);
			if (material == nullptr || !shadowResourcesReady || !ShouldMaterialCastShadow(material->shadowBehavior)) {
				continue;
			}

			auto meshIt = impl->meshCache.find(command.meshId);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (meshIt == impl->meshCache.end()) {
				continue;
			}

			VulkanMesh& mesh = meshIt->second;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (mesh.meshShaderVertexBuffer.buffer == VK_NULL_HANDLE
				|| mesh.meshletVertexRemapBuffer.buffer == VK_NULL_HANDLE
				|| mesh.meshDecodeInfoBuffer.buffer == VK_NULL_HANDLE
				|| mesh.indexBuffer.buffer == VK_NULL_HANDLE
				|| mesh.meshletBuffer.buffer == VK_NULL_HANDLE
				|| mesh.meshletCount == 0) {
				continue;
			}

			uint32_t materialFlags = material->header.flags;
			TerraEngine::UUID collectionId = material->header.shaderCollectionId;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (collectionId == TerraEngine::EMPTY_UUID) {
				collectionId = defaultMeshCollectionId;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (collectionId == defaultMeshCollectionId && defaultTaskPipelineAvailable) {
				collectionId = defaultTaskCollectionId;
			}

			TerraEngine::UUID shadowCollectionId = collectionId;
			VulkanPipeline* shadowPipeline = ResolvePipeline(shadowCollectionId, impl->shadowRenderPass, materialFlags);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (shadowPipeline == nullptr && shadowCollectionId == defaultTaskCollectionId) {
				shadowCollectionId = defaultMeshCollectionId;
				shadowPipeline = ResolvePipeline(shadowCollectionId, impl->shadowRenderPass, materialFlags);
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (shadowPipeline == nullptr) {
				continue;
			}

			VkDescriptorSet shadowMeshDescriptorSet = impl->EnsureMeshDescriptorSet(command.meshId, *shadowPipeline);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (shadowMeshDescriptorSet == VK_NULL_HANDLE) {
				continue;
			}

			PreparedDraw shadowDraw{};
			shadowDraw.mesh = &mesh;
			shadowDraw.pipeline = shadowPipeline;
			shadowDraw.meshId = command.meshId;
			shadowDraw.materialId = command.materialId;
			shadowDraw.descriptorSets = {impl->globalDescriptorSets[currentFrame], shadowMeshDescriptorSet};
			shadowDraw.push.viewProj = frameData.projectionMatrix * frameData.viewMatrix;
			shadowDraw.push.model = command.modelMatrix;
			shadowDraw.push.materialIndex = 0u;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (impl->materialIndexMap.contains(command.materialId)) {
				shadowDraw.push.materialIndex = impl->materialIndexMap[command.materialId];
			}
			shadowDraw.push.meshDataIndex = mesh.bindlessIndex;
			shadowDraw.push.textureIndex = 0u;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (material->header.textureCount > 0 && !material->textures.empty()) {
				const TerraEngine::UUID textureId = material->textures[0].textureUuid;
				if (impl->textureIndexMap.contains(textureId)) {
					shadowDraw.push.textureIndex = impl->textureIndexMap[textureId];
				}
			}
			shadowDraw.push.vrsActive = 2u;
			shadowDraw.push.taskCullingEnabled = 0u;
			shadowDraw.push.statsSlotIndex = statsSlotIndex;
			shadowDraw.push.cullingEpsilon = std::max(frameData.cullingEpsilon, 0.0001f);
			shadowDraw.push.lightDataOffset = renderSnapshot.lightDataOffset;
			shadowDraw.push.clusterDataOffset = renderSnapshot.clusterDataOffset;
			shadowDraw.push.lightIndexOffset = renderSnapshot.lightIndexOffset;
			shadowDraw.push.indirectDrawOffset = renderSnapshot.indirectDrawOffset;
			shadowDraw.materialFlags = materialFlags;

			const uint32_t kTaskBatchSize = 32u;
			const uint32_t taskGroupCount = shadowPipeline->hasTaskStage
				? ((mesh.meshletCount + (kTaskBatchSize - 1u)) / kTaskBatchSize)
				: mesh.meshletCount;
			VkDrawMeshTasksIndirectCommandEXT shadowIndirectCmd{};
			shadowIndirectCmd.groupCountX = taskGroupCount;
			shadowIndirectCmd.groupCountY = 1u;
			shadowIndirectCmd.groupCountZ = 1u;
			shadowDraw.indirectBufferIndex = static_cast<uint32_t>(indirectCommands.size());
			indirectCommands.push_back(shadowIndirectCmd);

			preparedList.draws.push_back(std::move(shadowDraw));
		}

		preparedShadowLists.push_back(std::move(preparedList));
	}

	pendingBudgetDiagnostics.cameraVisibleShadowCasterCount = static_cast<std::uint32_t>(preparedShadow.size());
	pendingBudgetDiagnostics.lightVisibleShadowCasterCount = 0u;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const PreparedShadowList& preparedList : preparedShadowLists) {
		pendingBudgetDiagnostics.lightVisibleShadowCasterCount += static_cast<std::uint32_t>(preparedList.draws.size());
	}

	impl->RecordPendingStats(statsSlotIndex, pendingStats);

	// Shadow-map rendering and the main scene pass both consume the same
	// indirect draw command buffer. Upload it before any shadow pass records
	// indirect draws, otherwise the shadow map can render stale or empty draw
	// commands and blockers will not occlude the light.
	if (!indirectCommands.empty()) {
		void* data = nullptr;
		vmaMapMemory(impl->allocator, impl->indirectDrawBuffers[impl->currentFrame].allocation, &data);
		auto* snapshotIndirectCommands = static_cast<VkDrawMeshTasksIndirectCommandEXT*>(data) + renderSnapshot.indirectDrawOffset;
		std::memcpy(snapshotIndirectCommands, indirectCommands.data(), indirectCommands.size() * sizeof(VkDrawMeshTasksIndirectCommandEXT));
		vmaUnmapMemory(impl->allocator, impl->indirectDrawBuffers[impl->currentFrame].allocation);
	}

	const auto HashCombine = [](std::uint64_t& seed, std::uint64_t value) {
		seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
	};
	const auto HashFloat = [&](std::uint64_t& seed, float value) {
		std::uint32_t bits = 0u;
		std::memcpy(&bits, &value, sizeof(bits));
		HashCombine(seed, bits);
	};
	const auto HashVec4 = [&](std::uint64_t& seed, const glm::vec4& value) {
		HashFloat(seed, value.x);
		HashFloat(seed, value.y);
		HashFloat(seed, value.z);
		HashFloat(seed, value.w);
	};
	const auto HashMat4 = [&](std::uint64_t& seed, const glm::mat4& value) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (int column = 0; column < 4; ++column) {
			HashVec4(seed, value[column]);
		}
	};
	const auto HashUUID = [&](std::uint64_t& seed, const TerraEngine::UUID& value) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::uint8_t byte : value) {
			HashCombine(seed, byte);
		}
	};
	const auto BuildLightShadowSignature = [&](const GPULight& light) {
		std::uint64_t signature = 1469598103934665603ull;
		HashVec4(signature, light.positionAndType);
		HashVec4(signature, light.directionAndRange);
		HashVec4(signature, light.colorAndIntensity);
		HashVec4(signature, light.coneAnglesAndArea);
		HashVec4(signature, light.extraParams);
		HashVec4(signature, light.shadowMatrixC0);
		HashVec4(signature, light.shadowMatrixC1);
		HashVec4(signature, light.shadowMatrixC2);
		HashVec4(signature, light.shadowMatrixC3);
		return signature;
	};
	const auto BuildCasterShadowSignature = [&](const std::vector<PreparedDraw>& draws) {
		std::uint64_t signature = 1469598103934665603ull;
		HashCombine(signature, static_cast<std::uint64_t>(draws.size()));
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const PreparedDraw& draw : draws) {
			HashUUID(signature, draw.meshId);
			HashUUID(signature, draw.materialId);
			HashMat4(signature, draw.push.model);
			HashCombine(signature, draw.materialFlags);
			HashCombine(signature, draw.mesh != nullptr ? draw.mesh->meshletCount : 0u);
		}
		return signature;
	};

	// If this is a subsequent pass targeting the swapchain (multi-camera),
	// inject a barrier to wait for the previous pass to finish its writes.
	if (!useMainCommandBuffer && !impl->editorMode) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = impl->swapchainImages[imageIndex];
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier
		);
	}

	// struct ShadowLightRecord groups related state and operations so callers have one documented contract.
	struct ShadowLightRecord
	{
		const GPULight* light = nullptr;
		TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
		const std::vector<PreparedDraw>* preparedDraws = nullptr;
		uint32_t type = 0u;
		uint32_t slot = VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS;
		std::uint32_t atlasId = 0u;
		std::uint32_t regionX = 0u;
		std::uint32_t regionY = 0u;
		std::uint32_t regionWidth = VulkanContext::Impl::SHADOW_MAP_RESOLUTION;
		std::uint32_t regionHeight = VulkanContext::Impl::SHADOW_MAP_RESOLUTION;
		std::uint64_t lightSignature = 0u;
		std::uint64_t casterSignature = 0u;
		std::uint64_t viewSignature = 0u;
		bool reusedCachedResult = false;
		bool invalidatedThisFrame = false;
	};
	const auto BuildRecordViewShadowSignature = [&](const ShadowLightRecord& record) {
		std::uint64_t signature = 1469598103934665603ull;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (record.light == nullptr) {
			return signature;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (record.type == 1u) {
			for (uint32_t faceIndex = 0u; faceIndex < 6u; ++faceIndex) {
				HashMat4(signature, BuildPointShadowViewProjection(*record.light, faceIndex));
			}
		}
		else {
			HashMat4(signature, LoadShadowViewProjection(*record.light));
		}
		return signature;
	};
	std::vector<ShadowLightRecord> shadowLightRecords;
	shadowLightRecords.reserve(VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS);
	auto FindShadowSlotFromTextureIndex = [](const std::array<uint32_t, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS>& textureIndices, uint32_t textureIndex) -> uint32_t {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t slotIndex = 0u; slotIndex < VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS; ++slotIndex) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (textureIndices[slotIndex] == textureIndex) {
				return slotIndex;
			}
		}
		return VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS;
	};
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t payloadIndex = 0u; payloadIndex < lightingPayload.size(); ++payloadIndex) {
		const GPULight& light = lightingPayload[payloadIndex];
		if (light.shadowAndPadding.x <= 0.5f) {
			continue;
		}
		const FrameLightCandidate& retainedCandidate = budgetResult.retainedLights[payloadIndex];
		const uint32_t lightType = static_cast<uint32_t>(std::max(0.0f, light.positionAndType.w));
		const uint32_t shadowPayloadIndex = static_cast<uint32_t>(std::max(0.0f, light.shadowAndPadding.y));
		uint32_t slotIndex = VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (lightType == 0u) {
			slotIndex = FindShadowSlotFromTextureIndex(impl->directionalShadowTextureIndices, shadowPayloadIndex);
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (lightType == 1u) {
			slotIndex = std::min(shadowPayloadIndex, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS - 1u);
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (lightType == 2u) {
			slotIndex = FindShadowSlotFromTextureIndex(impl->spotShadowTextureIndices, shadowPayloadIndex);
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (lightType == 3u) {
			slotIndex = FindShadowSlotFromTextureIndex(impl->areaShadowTextureIndices, shadowPayloadIndex);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (slotIndex < VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS) {
			ShadowLightRecord record{};
			record.light = &light;
			record.lightId = retainedCandidate.lightId;
			record.type = lightType;
			record.slot = slotIndex;
			record.lightSignature = BuildLightShadowSignature(light);
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const PreparedShadowList& preparedList : preparedShadowLists) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (preparedList.lightId == retainedCandidate.lightId) {
					record.preparedDraws = &preparedList.draws;
					record.casterSignature = BuildCasterShadowSignature(preparedList.draws);
					break;
				}
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (record.preparedDraws == nullptr) {
				static const std::vector<PreparedDraw> kEmptyPreparedShadowDraws{};
				record.preparedDraws = &kEmptyPreparedShadowDraws;
				record.casterSignature = BuildCasterShadowSignature(*record.preparedDraws);
			}
			record.viewSignature = BuildRecordViewShadowSignature(record);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (const ShadowResourceAssignmentDiagnostic* assignment = FindShadowAssignment(retainedCandidate.lightId)) {
				record.atlasId = assignment->atlasId;
				record.regionX = assignment->regionX;
				record.regionY = assignment->regionY;
				record.regionWidth = std::max(assignment->regionWidth, 1u);
				record.regionHeight = std::max(assignment->regionHeight, 1u);
			}
			shadowLightRecords.push_back(record);
		}
	}

	pendingBudgetDiagnostics.cacheReuseCount = 0u;
	pendingBudgetDiagnostics.cacheRefreshCount = 0u;
	pendingBudgetDiagnostics.cacheInvalidationCount = 0u;
	const auto ShadowKindFromRecord = [](std::uint32_t lightType) {
		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (lightType)
		{
		case 0u: return ShadowResourceKind::DirectionalCascade;
		case 1u: return ShadowResourceKind::PointCube;
		case 2u: return ShadowResourceKind::SpotAtlas;
		case 3u: return ShadowResourceKind::AreaAtlas;
		default: return ShadowResourceKind::Unsupported;
		}
	};
	const std::uint64_t diagnosticsFrameSerial = pendingBudgetDiagnostics.shadowAssignments.empty()
		? 0u
		: pendingBudgetDiagnostics.shadowAssignments.front().frameSerial;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (ShadowLightRecord& record : shadowLightRecords) {
		VulkanContext::Impl::ShadowCacheEntry& cacheEntry = impl->shadowCacheEntries[record.lightId];
		const ShadowResourceKind recordKind = ShadowKindFromRecord(record.type);
		const bool cacheMatches =
			cacheEntry.valid
			&& cacheEntry.resourceKind == recordKind
			&& cacheEntry.storageRegionId == record.slot
			&& cacheEntry.lightSignature == record.lightSignature
			&& cacheEntry.casterSignature == record.casterSignature
			&& cacheEntry.viewSignature == record.viewSignature;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (cacheMatches) {
			record.reusedCachedResult = true;
			cacheEntry.state = VulkanContext::Impl::ShadowCacheEntryState::Cached;
			cacheEntry.lastTouchedFrameSerial = diagnosticsFrameSerial;
			pendingBudgetDiagnostics.cacheReuseCount += 1u;
		}
		else {
			record.invalidatedThisFrame = cacheEntry.valid;
			pendingBudgetDiagnostics.cacheRefreshCount += 1u;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (record.invalidatedThisFrame) {
				pendingBudgetDiagnostics.cacheInvalidationCount += 1u;
			}
			cacheEntry.lightId = record.lightId;
			cacheEntry.resourceKind = recordKind;
			cacheEntry.storageRegionId = record.slot;
			cacheEntry.lightSignature = record.lightSignature;
			cacheEntry.casterSignature = record.casterSignature;
			cacheEntry.viewSignature = record.viewSignature;
			cacheEntry.lastTouchedFrameSerial = diagnosticsFrameSerial;
			cacheEntry.state = VulkanContext::Impl::ShadowCacheEntryState::Refreshed;
			cacheEntry.valid = true;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (ShadowResourceAssignmentDiagnostic& assignment : pendingBudgetDiagnostics.shadowAssignments) {
			if (assignment.lightId != record.lightId || !assignment.assigned) {
				continue;
			}
			assignment.reusedCachedResult = record.reusedCachedResult;
			assignment.refreshedThisFrame = !record.reusedCachedResult;
			assignment.invalidatedThisFrame = record.invalidatedThisFrame;
			assignment.refreshReason = record.reusedCachedResult
				? ShadowRefreshReason::CacheHit
				: (record.invalidatedThisFrame ? ShadowRefreshReason::CacheInvalidated : ShadowRefreshReason::RenderedThisFrame);
			break;
		}
	}

	auto RecordShadowDraws = [&](const std::vector<PreparedDraw>& shadowDraws, const glm::mat4& lightViewProjection) {
		std::uint32_t recordedDrawCount = 0u;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const PreparedDraw& draw : shadowDraws) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (draw.pipeline == nullptr
				|| draw.pipeline->pipeline == VK_NULL_HANDLE
				|| draw.pipeline->layout == VK_NULL_HANDLE
				|| draw.mesh == nullptr
				|| draw.mesh->meshletCount == 0) {
				continue;
			}

			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->pipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->layout, 0, static_cast<uint32_t>(draw.descriptorSets.size()), draw.descriptorSets.data(), 0, nullptr);

			PushConstants shadowPush = draw.push;
			shadowPush.viewProj = lightViewProjection;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (draw.pipeline->supportsDefaultPushConstants) {
				vkCmdPushConstants(cmd, draw.pipeline->layout, draw.pipeline->defaultPushConstantStages, 0, sizeof(PushConstants), &shadowPush);
			}

			VkDeviceSize offset = static_cast<VkDeviceSize>(draw.push.indirectDrawOffset + draw.indirectBufferIndex) * sizeof(VkDrawMeshTasksIndirectCommandEXT);
			vkCmdDrawMeshTasksIndirectEXT(cmd, impl->indirectDrawBuffers[impl->currentFrame].buffer, offset, 1, sizeof(VkDrawMeshTasksIndirectCommandEXT));
			recordedDrawCount += 1u;
		}
		return recordedDrawCount;
	};

	auto RecordShadowPass = [&](VkFramebuffer framebuffer, uint32_t width, uint32_t height, const ShadowLightRecord& record, const glm::mat4& lightViewProjection) -> bool {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (framebuffer == VK_NULL_HANDLE || impl->shadowRenderPass == VK_NULL_HANDLE) {
			return false;
		}

		VkRenderPassBeginInfo shadowPassInfo{};
		shadowPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		shadowPassInfo.renderPass = impl->shadowRenderPass;
		shadowPassInfo.framebuffer = framebuffer;
		shadowPassInfo.renderArea.offset = { 0, 0 };
		shadowPassInfo.renderArea.extent = { width, height };

		VkClearValue clearValues[2];
		clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
		clearValues[1].depthStencil = {1.0f, 0};
		shadowPassInfo.clearValueCount = 2;
		shadowPassInfo.pClearValues = clearValues;

		vkCmdBeginRenderPass(cmd, &shadowPassInfo, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport shadowViewport{};
		shadowViewport.x = static_cast<float>(record.regionX);
		shadowViewport.y = static_cast<float>(record.regionY);
		shadowViewport.width = static_cast<float>(std::min(record.regionWidth, width));
		shadowViewport.height = static_cast<float>(std::min(record.regionHeight, height));
		shadowViewport.minDepth = 0.0f;
		shadowViewport.maxDepth = 1.0f;
		vkCmdSetViewport(cmd, 0u, 1u, &shadowViewport);

		VkRect2D shadowScissor{};
		shadowScissor.offset = { static_cast<std::int32_t>(record.regionX), static_cast<std::int32_t>(record.regionY) };
		shadowScissor.extent = { std::min(record.regionWidth, width), std::min(record.regionHeight, height) };
		vkCmdSetScissor(cmd, 0u, 1u, &shadowScissor);
		const std::vector<PreparedDraw>& shadowDraws = (record.preparedDraws != nullptr)
			? *record.preparedDraws
			: preparedShadow;
		pendingBudgetDiagnostics.shadowDrawSubmissionCount += RecordShadowDraws(shadowDraws, lightViewProjection);
		vkCmdEndRenderPass(cmd);
		return true;
	};

	bool recordedAnyShadowPass = false;
	const uint32_t pointShadowSize = VulkanContext::Impl::POINT_SHADOW_MAP_RESOLUTION;
	pendingBudgetDiagnostics.shadowDrawSubmissionCount = 0u;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (shadowResourcesReady && impl->shadowRenderPass != VK_NULL_HANDLE && !shadowLightRecords.empty()) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ShadowLightRecord& record : shadowLightRecords) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (record.light == nullptr || record.slot >= VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS) {
				continue;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (record.reusedCachedResult) {
				continue;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (record.type == 0u) {
				const glm::mat4 lightViewProjection = LoadShadowViewProjection(*record.light);
				recordedAnyShadowPass = RecordShadowPass(impl->directionalShadowFramebuffers[record.slot], record.regionWidth, record.regionHeight, record, lightViewProjection) || recordedAnyShadowPass;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (record.type == 1u) {
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (uint32_t faceIndex = 0u; faceIndex < 6u; ++faceIndex) {
					const glm::mat4 lightViewProjection = BuildPointShadowViewProjection(*record.light, faceIndex);
					recordedAnyShadowPass = RecordShadowPass(impl->pointShadowFaceFramebuffersBySlot[record.slot][faceIndex], pointShadowSize, pointShadowSize, record, lightViewProjection) || recordedAnyShadowPass;
				}
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (record.type == 2u) {
				const glm::mat4 lightViewProjection = LoadShadowViewProjection(*record.light);
				recordedAnyShadowPass = RecordShadowPass(impl->spotShadowFramebuffers[record.slot], record.regionWidth, record.regionHeight, record, lightViewProjection) || recordedAnyShadowPass;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (record.type == 3u) {
				const glm::mat4 lightViewProjection = LoadShadowViewProjection(*record.light);
				recordedAnyShadowPass = RecordShadowPass(impl->areaShadowFramebuffers[record.slot], record.regionWidth, record.regionHeight, record, lightViewProjection) || recordedAnyShadowPass;
			}
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (recordedAnyShadowPass) {
		VkMemoryBarrier shadowBarrier{};
		shadowBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		shadowBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		shadowBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0,
			1, &shadowBarrier,
			0, nullptr,
			0, nullptr
		);
	}
	pendingBudgetDiagnostics.shadowPassRecorded = recordedAnyShadowPass;
	impl->RecordPendingBudgetDiagnostics(statsSlotIndex, pendingBudgetDiagnostics);
	
	std::vector<VkCommandBuffer> secondaryBuffers;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!prepared.empty()) {
		const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
		const size_t workerCount = std::min(prepared.size(), hw);
		const size_t chunkSize = (prepared.size() + workerCount - 1) / workerCount;

		impl->EnsureWorkerPools(impl->currentFrame, workerCount);
		std::vector<std::future<VkCommandBuffer>> futures;
		futures.reserve(workerCount);

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t taskIndex = 0; taskIndex < workerCount; ++taskIndex) {
			const size_t start = taskIndex * chunkSize;
			if (start >= prepared.size()) break;
			const size_t end = std::min(prepared.size(), start + chunkSize);
			VkFramebuffer framebuffer = renderPassInfo.framebuffer;
			VkRenderPass renderPass = renderPassInfo.renderPass;
			const VkExtent2D renderExtent = renderPassInfo.renderArea.extent;
			const uint32_t frameIndex = currentFrame;
			VkCommandBuffer secondary = impl->AcquireWorkerCommandBuffer(frameIndex, taskIndex);

			auto task = ThreadPool::instance().createTask([secondary, renderPass, framebuffer, renderExtent, start, end, &prepared, this]() -> VkCommandBuffer {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (secondary == VK_NULL_HANDLE) return VK_NULL_HANDLE;
				vkResetCommandBuffer(secondary, 0);

				VkCommandBufferInheritanceInfo inherit{};
				inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
				inherit.renderPass = renderPass;
				inherit.subpass = 0;
				inherit.framebuffer = framebuffer;

				VkCommandBufferBeginInfo begin{};
				begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
				// Secondary draw buffers are recorded once for the current frame slot and
				// are reset only after that slot's fence completes in BeginFrame. That
				// lifetime means they are never submitted for simultaneous execution, so
				// avoid VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT and the optional
				// nestedCommandBufferSimultaneousUse device feature it requires.
				begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
				begin.pInheritanceInfo = &inherit;

				vkBeginCommandBuffer(secondary, &begin);
				VkViewport viewport{};
				viewport.x = 0.0f;
				viewport.y = 0.0f;
				viewport.width = static_cast<float>(renderExtent.width);
				viewport.height = static_cast<float>(renderExtent.height);
				viewport.minDepth = 0.0f;
				viewport.maxDepth = 1.0f;
				vkCmdSetViewport(secondary, 0u, 1u, &viewport);

				VkRect2D scissor{};
				scissor.offset = { 0, 0 };
				scissor.extent = renderExtent;
				vkCmdSetScissor(secondary, 0u, 1u, &scissor);

				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (size_t i = start; i < end; ++i) {
					const PreparedDraw& draw = prepared[i];
					if (draw.pipeline == nullptr
						|| draw.pipeline->pipeline == VK_NULL_HANDLE
						|| draw.pipeline->layout == VK_NULL_HANDLE
						|| draw.mesh == nullptr) {
						continue;
					}
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (draw.mesh->meshShaderVertexBuffer.buffer == VK_NULL_HANDLE
						|| draw.mesh->meshletVertexRemapBuffer.buffer == VK_NULL_HANDLE
						|| draw.mesh->meshDecodeInfoBuffer.buffer == VK_NULL_HANDLE
						|| draw.mesh->indexBuffer.buffer == VK_NULL_HANDLE
						|| draw.mesh->meshletBuffer.buffer == VK_NULL_HANDLE
						|| draw.mesh->meshletCount == 0) {
						continue;
					}

					vkCmdBindPipeline(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->pipeline);
					vkCmdBindDescriptorSets(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->layout, 0, static_cast<uint32_t>(draw.descriptorSets.size()), draw.descriptorSets.data(), 0, nullptr);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (draw.pipeline->supportsDefaultPushConstants) {
						vkCmdPushConstants(secondary, draw.pipeline->layout, draw.pipeline->defaultPushConstantStages, 0, sizeof(PushConstants), &draw.push);
					}

					VkDeviceSize offset = static_cast<VkDeviceSize>(draw.push.indirectDrawOffset + draw.indirectBufferIndex) * sizeof(VkDrawMeshTasksIndirectCommandEXT);
					vkCmdDrawMeshTasksIndirectEXT(secondary, impl->indirectDrawBuffers[impl->currentFrame].buffer, offset, 1, sizeof(VkDrawMeshTasksIndirectCommandEXT));
				}
				vkEndCommandBuffer(secondary);
				return secondary;
			});
			futures.push_back(task->getFuture());
			ThreadPool::instance().enqueue(task);
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& future : futures) {
			VkCommandBuffer secondary = future.get();
			if (secondary != VK_NULL_HANDLE) {
				secondaryBuffers.push_back(secondary);
			}
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameData.sky.renderBackground)
	{
		const size_t workerIndex = secondaryBuffers.size();
		impl->EnsureWorkerPools(impl->currentFrame, workerIndex + 1);
		VkCommandBuffer skyCmd = impl->AcquireWorkerCommandBuffer(impl->currentFrame, workerIndex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skyCmd != VK_NULL_HANDLE)
		{
			vkResetCommandBuffer(skyCmd, 0);

			VkCommandBufferInheritanceInfo inherit{};
			inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
			inherit.renderPass = renderPassInfo.renderPass;
			inherit.subpass = 0;
			inherit.framebuffer = renderPassInfo.framebuffer;

			VkCommandBufferBeginInfo begin{};
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			// The sky secondary buffer follows the same per-frame fence lifetime as
			// mesh worker buffers, so it can be executed once by the primary command
			// buffer without requesting Vulkan's optional nested simultaneous-use feature.
			begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
			begin.pInheritanceInfo = &inherit;

			vkBeginCommandBuffer(skyCmd, &begin);
			impl->RenderSkyBackground(skyCmd, frameData);
			vkEndCommandBuffer(skyCmd);
			secondaryBuffers.insert(secondaryBuffers.begin(), skyCmd);
		}
	}

	// Render grid (Scene View only).
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (view == ViewType::Scene) {
		const size_t workerIndex = secondaryBuffers.size();
		impl->EnsureWorkerPools(impl->currentFrame, workerIndex + 1);

		VkCommandBuffer gridCmd = impl->AcquireWorkerCommandBuffer(impl->currentFrame, workerIndex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (gridCmd != VK_NULL_HANDLE) {
			vkResetCommandBuffer(gridCmd, 0);

			VkCommandBufferInheritanceInfo inherit{};
			inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
			inherit.renderPass = renderPassInfo.renderPass;
			inherit.subpass = 0;
			inherit.framebuffer = renderPassInfo.framebuffer;

			VkCommandBufferBeginInfo begin{};
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			// The scene-grid secondary buffer is reset with the current frame's worker
			// pool after the GPU fence is signaled, so render-pass continuation is the
			// only usage flag needed for valid nested execution.
			begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
			begin.pInheritanceInfo = &inherit;

			vkBeginCommandBuffer(gridCmd, &begin);
			impl->RenderSceneViewGrid(gridCmd, frameData);
			vkEndCommandBuffer(gridCmd);
			secondaryBuffers.push_back(gridCmd);
		}
	}

	// Runtime UI belongs to the Game view only. The Scene view must stay free of
	// authored UI so editor camera work, picking, and gizmos are never obscured
	// by the game's overlay.
	if (view == ViewType::Game) {
		const size_t workerIndex = secondaryBuffers.size();
		impl->EnsureWorkerPools(impl->currentFrame, workerIndex + 1);
		VkCommandBuffer uiCmd = impl->AcquireWorkerCommandBuffer(impl->currentFrame, workerIndex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (uiCmd != VK_NULL_HANDLE) {
			vkResetCommandBuffer(uiCmd, 0);

			VkCommandBufferInheritanceInfo inherit{};
			inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
			inherit.renderPass = renderPassInfo.renderPass;
			inherit.subpass = 0;
			inherit.framebuffer = renderPassInfo.framebuffer;

			VkCommandBufferBeginInfo begin{};
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			// Runtime UI records into a per-frame secondary command buffer that is not
			// submitted concurrently, so keep the flags limited to render-pass
			// continuation and avoid requiring nestedCommandBufferSimultaneousUse.
			begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
			begin.pInheritanceInfo = &inherit;

			vkBeginCommandBuffer(uiCmd, &begin);
			impl->CreateUIPipelines(renderPassInfo.renderPass);
			impl->RenderUI(uiCmd, frameData.uiBatch);
			vkEndCommandBuffer(uiCmd);
			secondaryBuffers.push_back(uiCmd);
		}
	}

	VkSubpassContents contents = secondaryBuffers.empty() ? VK_SUBPASS_CONTENTS_INLINE : VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS;
	vkCmdBeginRenderPass(cmd, &renderPassInfo, contents);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!secondaryBuffers.empty()) {
		vkCmdExecuteCommands(cmd, static_cast<uint32_t>(secondaryBuffers.size()), secondaryBuffers.data());
	}

	vkCmdEndRenderPass(cmd);
			
				// VRS Generation for next frame
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (impl->UsesVrsAttachment()) {
					impl->GenerateVRSImage(cmd, (view == ViewType::Scene) ? impl->sceneView.view : impl->gameView.view);
				}
			
				vkEndCommandBuffer(cmd);
				// Record the command buffer to be submitted at EndFrame so multiple cameras
	// render sequentially before a single present.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		impl->pendingPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
	}
	impl->pendingPrimaryCommandBuffers[impl->currentFrame].push_back(cmd);
	impl->activeRenderSnapshot.state = RenderSnapshotState::Recorded;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!impl->pendingRenderSnapshotDiagnostics.ownerSummaries.empty()) {
		impl->pendingRenderSnapshotDiagnostics.ownerSummaries.back().state = RenderSnapshotState::Recorded;
	}

	depthHistory.lastViewProjectionMatrix = currentViewProjection;
	depthHistory.valid = true;
}

// VulkanContext::Impl::GenerateHiZPyramid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::GenerateHiZPyramid(VkCommandBuffer cmd, VkImageView depthView) {
	if (cmd == VK_NULL_HANDLE
		|| depthImage == VK_NULL_HANDLE
		|| lightingComputePipelineLayout == VK_NULL_HANDLE
		|| currentFrame >= globalDescriptorSets.size()
		|| globalDescriptorSets[currentFrame] == VK_NULL_HANDLE
		|| hiZ.image == VK_NULL_HANDLE
		|| hiZComputePipeline == VK_NULL_HANDLE
		|| depthView == VK_NULL_HANDLE
		|| hiZ.sampler == VK_NULL_HANDLE
		|| hiZ.mipViews.size() < hiZ.mipLevels
		|| hiZ.mipLevels == 0u
		|| hiZ.width == 0u
		|| hiZ.height == 0u) {
		return;
	}
	for (uint32_t mipIndex = 0; mipIndex < hiZ.mipLevels; ++mipIndex) {
		// Each compute descriptor update below requires a concrete image view;
		// skip Hi-Z for this frame if swapchain recreation left any mip view absent.
		if (hiZ.mipViews[mipIndex] == VK_NULL_HANDLE) {
			return;
		}
	}

	// 1. Transition main depth buffer to DEPTH_STENCIL_READ_ONLY_OPTIMAL so sampled depth reads are valid.
	VkImageMemoryBarrier depthBarrier{};
	depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	depthBarrier.image = depthImage;
	depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	depthBarrier.subresourceRange.baseMipLevel = 0;
	depthBarrier.subresourceRange.levelCount = 1;
	depthBarrier.subresourceRange.baseArrayLayer = 0;
	depthBarrier.subresourceRange.layerCount = 1;
	depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &depthBarrier);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiZComputePipeline);

	// 2. Generate Level 0 (Convert D32 to R32F)
	{
		VkImageMemoryBarrier hizBarrier{};
		hizBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		hizBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		hizBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		hizBarrier.image = hiZ.image;
		hizBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		hizBarrier.subresourceRange.baseMipLevel = 0;
		hizBarrier.subresourceRange.levelCount = 1;
		hizBarrier.subresourceRange.baseArrayLayer = 0;
		hizBarrier.subresourceRange.layerCount = 1;
		hizBarrier.srcAccessMask = 0;
		hizBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &hizBarrier);

		VkDescriptorImageInfo depthInfo{};
		depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		depthInfo.imageView = depthView;
		depthInfo.sampler = VK_NULL_HANDLE;

		VkDescriptorImageInfo outputInfo{};
		outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		outputInfo.imageView = hiZ.mipViews[0];

		std::array<VkWriteDescriptorSet, 2> writes{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = globalDescriptorSets[currentFrame];
		writes[0].dstBinding = 12;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[0].descriptorCount = 1;
		writes[0].pImageInfo = &depthInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = globalDescriptorSets[currentFrame];
		writes[1].dstBinding = 16;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[1].descriptorCount = 1;
		writes[1].pImageInfo = &outputInfo;

		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lightingComputePipelineLayout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);

		struct { uint32_t w, h, isL0; } push;
		push.w = hiZ.width; push.h = hiZ.height; push.isL0 = 1;
		vkCmdPushConstants(cmd, lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
		vkCmdDispatch(cmd, (hiZ.width + 7) / 8, (hiZ.height + 7) / 8, 1);

		hizBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		hizBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		hizBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		hizBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &hizBarrier);
	}

	// 3. Generate Level 1..N
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t i = 1; i < hiZ.mipLevels; ++i) {
		uint32_t mipW = std::max(1u, hiZ.width >> i);
		uint32_t mipH = std::max(1u, hiZ.height >> i);

		VkImageMemoryBarrier hizBarrier{};
		hizBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		hizBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		hizBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		hizBarrier.image = hiZ.image;
		hizBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		hizBarrier.subresourceRange.baseMipLevel = i;
		hizBarrier.subresourceRange.levelCount = 1;
		hizBarrier.subresourceRange.baseArrayLayer = 0;
		hizBarrier.subresourceRange.layerCount = 1;
		hizBarrier.srcAccessMask = 0;
		hizBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &hizBarrier);

		VkDescriptorImageInfo inputInfo{};
		inputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		inputInfo.imageView = hiZ.mipViews[i - 1];
		inputInfo.sampler = hiZ.sampler;

		VkDescriptorImageInfo outputInfo{};
		outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		outputInfo.imageView = hiZ.mipViews[i];

		std::array<VkWriteDescriptorSet, 2> writes{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = globalDescriptorSets[currentFrame];
		writes[0].dstBinding = 12;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[0].descriptorCount = 1;
		writes[0].pImageInfo = &inputInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = globalDescriptorSets[currentFrame];
		writes[1].dstBinding = 16;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[1].descriptorCount = 1;
		writes[1].pImageInfo = &outputInfo;

		vkUpdateDescriptorSets(device, 2, writes.data(), 0, nullptr);

		struct { uint32_t w, h, isL0; } push;
		push.w = mipW; push.h = mipH; push.isL0 = 0;
		vkCmdPushConstants(cmd, lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
		vkCmdDispatch(cmd, (mipW + 7) / 8, (mipH + 7) / 8, 1);

		hizBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		hizBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		hizBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		hizBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &hizBarrier);
	}

	// 4. Keep depth in read-only layout so the subsequent light-culling compute pass can sample it.
	depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	depthBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &depthBarrier);

	VkDescriptorImageInfo restoreDepthInfo{};
	// Keep the descriptor in read-only layout because binding 12 is sampled by compute passes.
	restoreDepthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	restoreDepthInfo.imageView = depthView;
	restoreDepthInfo.sampler = hiZ.sampler;

	VkWriteDescriptorSet restoreWrite{};
	restoreWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	restoreWrite.dstSet = globalDescriptorSets[currentFrame];
	restoreWrite.dstBinding = 12;
	restoreWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	restoreWrite.descriptorCount = 1;
	restoreWrite.pImageInfo = &restoreDepthInfo;
	vkUpdateDescriptorSets(device, 1, &restoreWrite, 0, nullptr);
}

// VulkanContext::Impl::GenerateAmbientOcclusion is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::GenerateAmbientOcclusion(VkCommandBuffer cmd, const FrameCullingDataGPU& frameData) {
	const bool aoEnabled = frameData.renderSettings2.y > 0.001f && frameData.renderSettings2.z > 0.5f;
	if (!aoEnabled
		|| cmd == VK_NULL_HANDLE
		|| depthImageView == VK_NULL_HANDLE
		|| lightingComputePipelineLayout == VK_NULL_HANDLE
		|| currentFrame >= globalDescriptorSets.size()
		|| globalDescriptorSets[currentFrame] == VK_NULL_HANDLE
		|| gtaoComputePipeline == VK_NULL_HANDLE
		|| gtaoBlurComputePipeline == VK_NULL_HANDLE
		|| ambientOcclusionRaw.image == VK_NULL_HANDLE
		|| ambientOcclusionBlurred.image == VK_NULL_HANDLE
		|| ambientOcclusionHistory.image == VK_NULL_HANDLE
		|| ambientOcclusionRaw.view == VK_NULL_HANDLE
		|| ambientOcclusionBlurred.view == VK_NULL_HANDLE
		|| ambientOcclusionHistory.view == VK_NULL_HANDLE) {
		return;
	}

	const uint32_t width = std::max(ambientOcclusionWidth, 1u);
	const uint32_t height = std::max(ambientOcclusionHeight, 1u);
	auto TransitionAoImage = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0u;
		barrier.subresourceRange.levelCount = 1u;
		barrier.subresourceRange.baseArrayLayer = 0u;
		barrier.subresourceRange.layerCount = 1u;
		barrier.srcAccessMask = srcAccess;
		barrier.dstAccessMask = dstAccess;
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	};

	// The AO targets persist across frames in shader-read layout. Each dispatch
	// temporarily moves one target to GENERAL, writes it, then makes it readable
	// before the next compute pass or material shading samples it.
	TransitionAoImage(
		ambientOcclusionRaw.image,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_ACCESS_SHADER_READ_BIT,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	VkDescriptorImageInfo depthInfo{};
	depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	depthInfo.imageView = depthImageView;
	depthInfo.sampler = VK_NULL_HANDLE;

	VkDescriptorImageInfo rawOutputInfo{};
	rawOutputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	rawOutputInfo.imageView = ambientOcclusionRaw.view;
	rawOutputInfo.sampler = VK_NULL_HANDLE;

	std::array<VkWriteDescriptorSet, 2> gtaoWrites{};
	gtaoWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	gtaoWrites[0].dstSet = globalDescriptorSets[currentFrame];
	gtaoWrites[0].dstBinding = 12;
	gtaoWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	gtaoWrites[0].descriptorCount = 1;
	gtaoWrites[0].pImageInfo = &depthInfo;

	gtaoWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	gtaoWrites[1].dstSet = globalDescriptorSets[currentFrame];
	gtaoWrites[1].dstBinding = 16;
	gtaoWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	gtaoWrites[1].descriptorCount = 1;
	gtaoWrites[1].pImageInfo = &rawOutputInfo;
	vkUpdateDescriptorSets(device, static_cast<uint32_t>(gtaoWrites.size()), gtaoWrites.data(), 0, nullptr);

	// struct GTAOPushConstants groups related state and operations so callers have one documented contract.
	struct GTAOPushConstants
	{
		glm::uvec2 viewportSize = glm::uvec2(1u);
		float radiusPixels = 1.0f;
		float strength = 1.0f;
	};

	const float quality = std::clamp(frameData.renderSettings2.z, 1.0f, 3.0f);
	GTAOPushConstants gtaoPush{};
	gtaoPush.viewportSize = glm::uvec2(width, height);
	gtaoPush.radiusPixels = 1.0f + quality * 1.5f;
	gtaoPush.strength = std::clamp(frameData.renderSettings2.y, 0.0f, 4.0f);

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lightingComputePipelineLayout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoComputePipeline);
	vkCmdPushConstants(cmd, lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(gtaoPush), &gtaoPush);
	vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1u);

	TransitionAoImage(
		ambientOcclusionRaw.image,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	TransitionAoImage(
		ambientOcclusionHistory.image,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_ACCESS_SHADER_READ_BIT,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	VkDescriptorImageInfo rawInputInfo{};
	rawInputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	rawInputInfo.imageView = ambientOcclusionRaw.view;
	rawInputInfo.sampler = VK_NULL_HANDLE;

	VkDescriptorImageInfo blurredOutputInfo{};
	blurredOutputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	blurredOutputInfo.imageView = ambientOcclusionHistory.view;
	blurredOutputInfo.sampler = VK_NULL_HANDLE;

	VkDescriptorImageInfo historyInputInfo{};
	historyInputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	historyInputInfo.imageView = ambientOcclusionBlurred.view;
	historyInputInfo.sampler = VK_NULL_HANDLE;

	std::array<VkWriteDescriptorSet, 3> blurWrites{};
	blurWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	blurWrites[0].dstSet = globalDescriptorSets[currentFrame];
	blurWrites[0].dstBinding = 19;
	blurWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	blurWrites[0].descriptorCount = 1;
	blurWrites[0].pImageInfo = &rawInputInfo;

	blurWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	blurWrites[1].dstSet = globalDescriptorSets[currentFrame];
	blurWrites[1].dstBinding = 16;
	blurWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	blurWrites[1].descriptorCount = 1;
	blurWrites[1].pImageInfo = &blurredOutputInfo;

	blurWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	blurWrites[2].dstSet = globalDescriptorSets[currentFrame];
	blurWrites[2].dstBinding = 21;
	blurWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	blurWrites[2].descriptorCount = 1;
	blurWrites[2].pImageInfo = &historyInputInfo;
	vkUpdateDescriptorSets(device, static_cast<uint32_t>(blurWrites.size()), blurWrites.data(), 0, nullptr);

	// struct GTAOBlurPushConstants groups related state and operations so callers have one documented contract.
	struct GTAOBlurPushConstants
	{
		glm::uvec2 viewportSize = glm::uvec2(1u);
		float depthSigma = 0.0025f;
		float historyBlend = 0.0f;
	};

	GTAOBlurPushConstants blurPush{};
	blurPush.viewportSize = glm::uvec2(width, height);
	blurPush.depthSigma = 0.0025f / quality;
	// First frame after resize must not sample undefined temporal content; later
	// frames use a modest blend to suppress flicker without smearing edits.
	blurPush.historyBlend = ambientOcclusionHistoryValid ? std::clamp(0.35f + quality * 0.1f, 0.0f, 0.65f) : 0.0f;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoBlurComputePipeline);
	vkCmdPushConstants(cmd, lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(blurPush), &blurPush);
	vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1u);

	TransitionAoImage(
		ambientOcclusionHistory.image,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	std::swap(ambientOcclusionBlurred, ambientOcclusionHistory);
	ambientOcclusionHistoryValid = true;

	VkDescriptorImageInfo blurredInputInfo{};
	blurredInputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	blurredInputInfo.imageView = ambientOcclusionBlurred.view;
	blurredInputInfo.sampler = VK_NULL_HANDLE;

	VkDescriptorImageInfo nextHistoryInfo{};
	nextHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	nextHistoryInfo.imageView = ambientOcclusionHistory.view;
	nextHistoryInfo.sampler = VK_NULL_HANDLE;

	std::array<VkWriteDescriptorSet, 2> fragmentAoWrites{};
	fragmentAoWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	fragmentAoWrites[0].dstSet = globalDescriptorSets[currentFrame];
	fragmentAoWrites[0].dstBinding = 19;
	fragmentAoWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	fragmentAoWrites[0].descriptorCount = 1;
	fragmentAoWrites[0].pImageInfo = &blurredInputInfo;

	fragmentAoWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	fragmentAoWrites[1].dstSet = globalDescriptorSets[currentFrame];
	fragmentAoWrites[1].dstBinding = 21;
	fragmentAoWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	fragmentAoWrites[1].descriptorCount = 1;
	fragmentAoWrites[1].pImageInfo = &nextHistoryInfo;
	vkUpdateDescriptorSets(device, static_cast<uint32_t>(fragmentAoWrites.size()), fragmentAoWrites.data(), 0, nullptr);
}

// VulkanContext::Impl::GenerateVRSImage is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::GenerateVRSImage(VkCommandBuffer cmd, VkImageView colorView) {
	if (cmd == VK_NULL_HANDLE
		|| lightingComputePipelineLayout == VK_NULL_HANDLE
		|| currentFrame >= globalDescriptorSets.size()
		|| globalDescriptorSets[currentFrame] == VK_NULL_HANDLE
		|| vrsComputePipeline == VK_NULL_HANDLE
		|| vrs.image == VK_NULL_HANDLE
		|| vrs.view == VK_NULL_HANDLE
		|| colorView == VK_NULL_HANDLE
		|| vrs.width == 0u
		|| vrs.height == 0u
		|| vrs.texelSize.width == 0u
		|| vrs.texelSize.height == 0u) {
		return;
	}

	// Transition VRS to GENERAL
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.image = vrs.image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	// Update descriptor set with current color view and VRS image
	VkDescriptorImageInfo colorInfo{};
	colorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	colorInfo.imageView = colorView;
	colorInfo.sampler = hiZ.sampler;

	VkDescriptorImageInfo vrsOutputInfo{};
	vrsOutputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	vrsOutputInfo.imageView = vrs.view;

	std::array<VkWriteDescriptorSet, 2> vrsWrites{};
	vrsWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	vrsWrites[0].dstSet = globalDescriptorSets[currentFrame];
	vrsWrites[0].dstBinding = 14;
	vrsWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	vrsWrites[0].descriptorCount = 1;
	vrsWrites[0].pImageInfo = &vrsOutputInfo;

	vrsWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	vrsWrites[1].dstSet = globalDescriptorSets[currentFrame];
	vrsWrites[1].dstBinding = 15;
	vrsWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	vrsWrites[1].descriptorCount = 1;
	vrsWrites[1].pImageInfo = &colorInfo;

	vkUpdateDescriptorSets(device, static_cast<uint32_t>(vrsWrites.size()), vrsWrites.data(), 0, nullptr);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vrsComputePipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lightingComputePipelineLayout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);

	struct {
		uint32_t width;
		uint32_t height;
		uint32_t texelSize;
	} push;
	push.width = swapchainExtent.width;
	push.height = swapchainExtent.height;
	push.texelSize = vrs.texelSize.width;
	vkCmdPushConstants(cmd, lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

	vkCmdDispatch(cmd, (vrs.width + 7) / 8, (vrs.height + 7) / 8, 1);

	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// VulkanContext::EndFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::EndFrame() {
	if (!impl->initialized || impl->skipFrame) return;

	// Submit all recorded camera command buffers together so the swapchain is
	// presented once using the final camera's output.
	if (impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		return;
	}

	auto& cmds = impl->pendingPrimaryCommandBuffers[impl->currentFrame];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (cmds.empty()) {
		return;
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	VkSemaphore waitSemaphores[] = {impl->imageAvailableSemaphores[impl->currentFrame]};
	VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!impl->editorMode && impl->swapchainImageAcquired) {
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
	}

	submitInfo.commandBufferCount = static_cast<uint32_t>(cmds.size());
	submitInfo.pCommandBuffers = cmds.data();

	// If not in editor mode, we must use the semaphore corresponding to the swapchain image index
	// to avoid validation errors about reusing semaphores still in use by the presentation engine.
	VkSemaphore signalSemaphore = impl->editorMode 
		? impl->renderFinishedSemaphores[impl->currentFrame] 
		: impl->renderFinishedSemaphores[impl->acquiredImageIndex];

	VkSemaphore signalSemaphores[] = {signalSemaphore};
	// Always signal render finished, so Editor pass can wait on it
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->editorMode) {
		// In Editor Mode, we defer the frame fence signal to the UI pass (PresentEditor)
		// so the frame is considered "in flight" until the UI is also done.
	VkResult submitResult = vkQueueSubmit(impl->graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
		if (submitResult != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to submit draw command buffers! Result: " << submitResult);
			impl->RecreateSwapChain();
			return;
		}
	} else {
		vkResetFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame]);
		VkResult submitResult = vkQueueSubmit(impl->graphicsQueue, 1, &submitInfo, impl->inFlightFences[impl->currentFrame]);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (submitResult != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "[Vulkan] QueueSubmit failed! Result: " << submitResult << ". Recreating swapchain to recover state.");
			impl->RecreateSwapChain();
			return;
		}
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (RenderSnapshotOwnerSummary& owner : impl->pendingRenderSnapshotDiagnostics.ownerSummaries) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (owner.state == RenderSnapshotState::Recorded) {
			owner.state = RenderSnapshotState::Submitted;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!impl->editorMode) {
		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;

		VkSwapchainKHR swapChains[] = {impl->swapchain};
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = swapChains;
		presentInfo.pImageIndices = &impl->acquiredImageIndex;

		VkResult result = vkQueuePresentKHR(impl->presentQueue, &presentInfo);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || impl->framebufferResized) {
			impl->framebufferResized = false;
			impl->RecreateSwapChain();
		} else if (result != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to present swap chain image! Result: " << result);
		}

		impl->currentFrame = (impl->currentFrame + 1) % impl->MAX_FRAMES_IN_FLIGHT;
	}
}

// VulkanContext::SetGlobalTaskShaderCullingEnabled is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::SetGlobalTaskShaderCullingEnabled(bool enabled) {
	if (impl) {
		impl->globalTaskShaderCullingEnabled = enabled;
	}
}

// VulkanContext::IsTaskShaderSupported is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::IsTaskShaderSupported() const {
	return impl != nullptr && impl->taskShaderSupported;
}

// VulkanContext::GetLatestRenderStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderStatsSnapshot VulkanContext::GetLatestRenderStats(RenderViewType viewType) const {
	if (!impl) {
		RenderStatsSnapshot snapshot{};
		snapshot.viewType = viewType;
		return snapshot;
	}
	return impl->GetLatestStats(viewType);
}

// VulkanContext::GetLatestFrameBudgetDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
FrameBudgetDiagnosticsSnapshot VulkanContext::GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const {
	if (!impl) {
		FrameBudgetDiagnosticsSnapshot snapshot{};
		snapshot.viewType = viewType;
		return snapshot;
	}
	return impl->GetLatestBudgetDiagnostics(viewType);
}

// VulkanContext::GetLatestLightingDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
LightingDiagnosticsSnapshot VulkanContext::GetLatestLightingDiagnostics(RenderViewType viewType) const {
	if (!impl) {
		LightingDiagnosticsSnapshot snapshot{};
		snapshot.viewType = viewType;
		return snapshot;
	}
	return impl->GetLatestLightingDiagnostics(viewType);
}

// VulkanContext::GetLatestRenderSnapshotDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderSnapshotDiagnosticsSnapshot VulkanContext::GetLatestRenderSnapshotDiagnostics() const {
	if (!impl) {
		return {};
	}
	std::lock_guard<std::mutex> lock(impl->statsMutex);
	return impl->latestRenderSnapshotDiagnostics;
}

// VulkanContext::InvalidateMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::InvalidateMaterial(const TerraEngine::UUID& materialId) {
	if (impl) {
		impl->InvalidateMaterial(materialId);
	}
}

// VulkanContext::InvalidateMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::InvalidateMesh(const TerraEngine::UUID& meshId) {
	if (impl) {
		impl->InvalidateMesh(meshId);
	}
}

// VulkanContext::InvalidateTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::InvalidateTexture(const TerraEngine::UUID& textureId) {
	if (impl) {
		impl->InvalidateTexture(textureId);
	}
}

// VulkanContext::InvalidateResource is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::InvalidateResource(const TerraEngine::UUID& id) {
	if (impl) {
		impl->InvalidateResource(id);
	}
}

// VulkanContext::WaitIdle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::WaitIdle() {
	if (impl && impl->device) {
		vkDeviceWaitIdle(impl->device);
	}
}

// VulkanContext::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Shutdown() {
	impl->Cleanup();
}

// VulkanContext::ClearResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::ClearResources() {
	impl->ClearResources();
}

// VulkanContext::GetEditorHandles is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::GetEditorHandles(EditorHandles& h) {
	h.instance = impl->instance;
	h.physicalDevice = impl->physicalDevice;
	h.device = impl->device;
	h.queue = impl->graphicsQueue;
	h.queueFamily = impl->FindQueueFamilies(impl->physicalDevice).graphicsFamily.value();
	h.descriptorPool = impl->descriptorPool;
	h.renderPass = impl->renderPass;
	h.minImageCount = impl->MAX_FRAMES_IN_FLIGHT;
	h.imageCount = static_cast<uint32_t>(impl->swapchainImages.size());
	h.sceneImageView = (void*)impl->sceneView.view;
	h.sceneSampler = (void*)impl->sceneView.sampler;
	h.gameImageView = (void*)impl->gameView.view;
	h.gameSampler = (void*)impl->gameView.sampler;
	h.vkGetInstanceProcAddr = (void*)vkGetInstanceProcAddr;
}

// VulkanContext::EnableEditorMode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::EnableEditorMode() {
	impl->InitEditorMode();
}

// VulkanContext::PresentEditor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::PresentEditor(std::function<void(void*)> renderUI) {
	if (!impl->editorMode || !impl->initialized) return;

	// Removed CPU Fence Wait. We now rely on Semaphores to synchronize Scene -> UI on the GPU.
	
	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(impl->device, impl->swapchain, UINT64_MAX, impl->imageAvailableSemaphores[impl->currentFrame], VK_NULL_HANDLE, &imageIndex);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		impl->RecreateSwapChain();
		impl->RecreateOffscreenResources();
		return;
	} else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to acquire swap chain image for Editor!");
		return;
	}

	VkCommandBuffer uiCmd;
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = impl->commandPools[impl->currentFrame];
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	vkAllocateCommandBuffers(impl->device, &allocInfo, &uiCmd);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->currentFrame >= impl->extraPrimaryCommandBuffers.size()) {
		impl->extraPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
	}
	impl->extraPrimaryCommandBuffers[impl->currentFrame].push_back(uiCmd);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(uiCmd, &beginInfo);

	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassInfo.renderPass = impl->renderPass;
	renderPassInfo.framebuffer = impl->framebuffers[imageIndex];
	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = impl->swapchainExtent;

	std::array<VkClearValue, 3> clearValues{};
	clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
	clearValues[1].depthStencil = {1.0f, 0};
	clearValues[2].color = {{0, 0, 0, 0}};

	renderPassInfo.clearValueCount = impl->UsesVrsAttachment() ? 3 : 2;
	renderPassInfo.pClearValues = clearValues.data();

	vkCmdBeginRenderPass(uiCmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (renderUI) renderUI(uiCmd);
	vkCmdEndRenderPass(uiCmd);
	vkEndCommandBuffer(uiCmd);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	
	// Wait for BOTH Swapchain Image (Available) AND Scene Render (RenderFinished)
	std::array<VkSemaphore, 2> waitSemaphores = {
		impl->imageAvailableSemaphores[impl->currentFrame],
		impl->renderFinishedSemaphores[impl->currentFrame]
	};
	std::array<VkPipelineStageFlags, 2> waitStages = {
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // Swapchain available
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT // Scene done (safe to sample in UI fragment shader)
	};
	
	submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
	submitInfo.pWaitSemaphores = waitSemaphores.data();
	submitInfo.pWaitDstStageMask = waitStages.data();

	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &uiCmd;

	VkSemaphore uiSemaphore = impl->uiSemaphores[imageIndex];
	
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &uiSemaphore;

	// Reset and use the main frame fence here, so BeginFrame waits for UI completion too.
	vkResetFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame]);

	VkResult uiSubmitResult = vkQueueSubmit(impl->graphicsQueue, 1, &submitInfo, impl->inFlightFences[impl->currentFrame]);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (uiSubmitResult != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "PresentEditor QueueSubmit failed: " << uiSubmitResult);
		vkFreeCommandBuffers(impl->device, impl->commandPools[impl->currentFrame], 1, &uiCmd);
		impl->RecreateSwapChain();
		return;
	}

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &uiSemaphore;
	
	VkSwapchainKHR swapChains[] = {impl->swapchain};
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapChains;
	presentInfo.pImageIndices = &imageIndex;

	VkResult presentResult = vkQueuePresentKHR(impl->presentQueue, &presentInfo);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || impl->framebufferResized) {
		impl->framebufferResized = false;
		impl->RecreateSwapChain();
		impl->RecreateOffscreenResources();
	}

	// Removed End-of-Frame Block. We proceed optimistically.
	
	impl->currentFrame = (impl->currentFrame + 1) % impl->MAX_FRAMES_IN_FLIGHT;
}

// Impl Extensions

void VulkanContext::Impl::InitEditorMode() {
	if (editorMode) return;
	editorMode = true;

	// Rebuild the swapchain render pass after flipping editorMode so the ImGui
	// presentation pass does not inherit the runtime VRS attachment.
	RecreateSwapChain();
	CreateOffscreenResources(sceneView);
	CreateOffscreenResources(gameView);
}

// VulkanContext::Impl::CreateOffscreenResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateOffscreenResources(OffscreenResources& res) {
	if (res.image != VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, res.image, res.allocation);
		vkDestroyImageView(device, res.view, nullptr);
		vkDestroyFramebuffer(device, res.framebuffer, nullptr);
		vkDestroySampler(device, res.sampler, nullptr);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (offscreenColorFormat == VK_FORMAT_UNDEFINED) {
		// Scene and game views are sampled by the editor after rendering, so the
		// format must support both color attachment writes and shader sampling.
		// The float format preserves smooth HDR/linear-light gradients; SRGB is
		// the best 8-bit fallback because it allocates more precision to shadows.
		const VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		const std::array<VkFormat, 3> candidates = {
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_FORMAT_R8G8B8A8_SRGB,
			VK_FORMAT_R8G8B8A8_UNORM
		};

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkFormat candidate : candidates) {
			VkFormatProperties properties{};
			vkGetPhysicalDeviceFormatProperties(physicalDevice, candidate, &properties);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if ((properties.optimalTilingFeatures & requiredFeatures) == requiredFeatures) {
				offscreenColorFormat = candidate;
				break;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (offscreenColorFormat == VK_FORMAT_UNDEFINED) {
			TERRAENGINE_LOG(LogLevel::Warning, "No sampled color-attachment format reported for editor offscreen rendering; falling back to R8G8B8A8_UNORM.");
			offscreenColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
		}
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (offscreenRenderPass == VK_NULL_HANDLE) {
		if (UsesVrsAttachment()) {
			VkAttachmentDescription2 colorAttachment{};
			colorAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
			colorAttachment.format = offscreenColorFormat;
			colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkAttachmentReference2 colorAttachmentRef{};
			colorAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
			colorAttachmentRef.attachment = 0;
			colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription2 depthAttachment{};
			depthAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
			depthAttachment.format = FindDepthFormat();
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkAttachmentReference2 depthAttachmentRef{};
			depthAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
			depthAttachmentRef.attachment = 1;
			depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription2 vrsAttachment{};
			vrsAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
			vrsAttachment.format = VK_FORMAT_R8_UINT;
			vrsAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			vrsAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			vrsAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			vrsAttachment.initialLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
			vrsAttachment.finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;

			VkAttachmentReference2 vrsAttachmentRef{};
			vrsAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
			vrsAttachmentRef.attachment = 2;
			vrsAttachmentRef.layout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;

			VkFragmentShadingRateAttachmentInfoKHR vrsInfo{};
			vrsInfo.sType = VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
			vrsInfo.pFragmentShadingRateAttachment = &vrsAttachmentRef;
			vrsInfo.shadingRateAttachmentTexelSize = vrs.texelSize;

			VkSubpassDescription2 subpass{};
			subpass.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1;
			subpass.pColorAttachments = &colorAttachmentRef;
			subpass.pDepthStencilAttachment = &depthAttachmentRef;
			subpass.pNext = &vrsInfo;

			std::array<VkSubpassDependency2, 1> dependencies{};
			dependencies[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			dependencies[0].srcAccessMask = 0;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

			std::array<VkAttachmentDescription2, 3> attachments = { colorAttachment, depthAttachment, vrsAttachment };
			VkRenderPassCreateInfo2 renderPassInfo{};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
			renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			renderPassInfo.pAttachments = attachments.data();
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpass;
			renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
			renderPassInfo.pDependencies = dependencies.data();

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vkCreateRenderPass2(device, &renderPassInfo, nullptr, &offscreenRenderPass) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "failed to create offscreen render pass 2!");
				return;
			}
		} else {
			// Create Render Pass using standard RenderPass API (max compatibility)
			VkAttachmentDescription colorAttachment{};
			colorAttachment.format = offscreenColorFormat;
			colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkAttachmentReference colorAttachmentRef{};
			colorAttachmentRef.attachment = 0;
			colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription depthAttachment{};
			depthAttachment.format = FindDepthFormat();
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkAttachmentReference depthAttachmentRef{};
			depthAttachmentRef.attachment = 1;
			depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkSubpassDescription subpass{};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1;
			subpass.pColorAttachments = &colorAttachmentRef;
			subpass.pDepthStencilAttachment = &depthAttachmentRef;

			std::array<VkSubpassDependency, 2> dependencies{};
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };
			VkRenderPassCreateInfo renderPassInfo{};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			renderPassInfo.pAttachments = attachments.data();
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpass;
			renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
			renderPassInfo.pDependencies = dependencies.data();

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &offscreenRenderPass) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "failed to create offscreen render pass!");
				return;
			}
		}
	}

	// Create Image
	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = swapchainExtent.width;
	imageInfo.extent.height = swapchainExtent.height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = offscreenColorFormat;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	vmaCreateImage(allocator, &imageInfo, &allocInfo, &res.image, &res.allocation, nullptr);
	vmaSetAllocationName(allocator, res.allocation, "Offscreen Render Target Image");

	// Create View
	res.view = CreateImageView(res.image, offscreenColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);

	// Create Framebuffer
	std::vector<VkImageView> fbAttachments = {
		res.view,
		depthImageView
	};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (UsesVrsAttachment()) {
		fbAttachments.push_back(vrs.view);
	}
	
	VkFramebufferCreateInfo framebufferInfo{};
	framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.renderPass = offscreenRenderPass;
	framebufferInfo.attachmentCount = static_cast<uint32_t>(fbAttachments.size());
	framebufferInfo.pAttachments = fbAttachments.data();
	framebufferInfo.width = swapchainExtent.width;
	framebufferInfo.height = swapchainExtent.height;
	framebufferInfo.layers = 1;

	vkCreateFramebuffer(device, &framebufferInfo, nullptr, &res.framebuffer);

	// Create Sampler
	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.anisotropyEnable = VK_FALSE;
	samplerInfo.maxAnisotropy = 1.0f;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

	vkCreateSampler(device, &samplerInfo, nullptr, &res.sampler);
}

// VulkanContext::Impl::RecreateOffscreenResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecreateOffscreenResources() {
	vkDeviceWaitIdle(device);
	CreateOffscreenResources(sceneView);
	CreateOffscreenResources(gameView);
}

// VulkanContext::Impl::ClearResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::ClearResources() {
	if (!device) return;
	vkDeviceWaitIdle(device);

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& [id, mesh] : meshCache) {
		if (!mesh.descriptorSetsByLayout.empty() && descriptorPool != VK_NULL_HANDLE) {
			std::vector<VkDescriptorSet> descriptorSets;
			descriptorSets.reserve(mesh.descriptorSetsByLayout.size());
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& [layout, set] : mesh.descriptorSetsByLayout) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (set != VK_NULL_HANDLE) {
					descriptorSets.push_back(set);
				}
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!descriptorSets.empty()) {
				vkFreeDescriptorSets(device, descriptorPool, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data());
			}
			mesh.descriptorSetsByLayout.clear();
		}

		vmaDestroyBuffer(allocator, mesh.indexBuffer.buffer, mesh.indexBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshShaderVertexBuffer.buffer, mesh.meshShaderVertexBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshletVertexRemapBuffer.buffer, mesh.meshletVertexRemapBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshSkinningBuffer.buffer, mesh.meshSkinningBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshDecodeInfoBuffer.buffer, mesh.meshDecodeInfoBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshletBuffer.buffer, mesh.meshletBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshletBoundsBuffer.buffer, mesh.meshletBoundsBuffer.allocation);
	}
	meshCache.clear();

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& [id, tex] : textureCache) {
		vkDestroySampler(device, tex.sampler, nullptr);
		vkDestroyImageView(device, tex.view, nullptr);
		vmaDestroyImage(allocator, tex.image, tex.allocation);
	}
	textureCache.clear();
	
	auto DestroyVulkanImage = [&](VulkanImage& image) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (device && image.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, image.sampler, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (device && image.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, image.view, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.image != VK_NULL_HANDLE && image.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
		}
		image.image = VK_NULL_HANDLE;
		image.allocation = VK_NULL_HANDLE;
		image.view = VK_NULL_HANDLE;
		image.sampler = VK_NULL_HANDLE;
	};

	auto DestroyFramebuffer = [&](VkFramebuffer& framebuffer) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (framebuffer != VK_NULL_HANDLE) {
			vkDestroyFramebuffer(device, framebuffer, nullptr);
			framebuffer = VK_NULL_HANDLE;
		}
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VkFramebuffer& framebuffer : directionalShadowFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VkFramebuffer& framebuffer : spotShadowFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VkFramebuffer& framebuffer : areaShadowFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::array<VkFramebuffer, 6>& faceFramebuffers : pointShadowFaceFramebuffersBySlot) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkFramebuffer& framebuffer : faceFramebuffers) {
			DestroyFramebuffer(framebuffer);
		}
	}

	auto DestroyImageView = [&](VkImageView& imageView) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (imageView != VK_NULL_HANDLE) {
			vkDestroyImageView(device, imageView, nullptr);
			imageView = VK_NULL_HANDLE;
		}
	};
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::array<VkImageView, 6>& faceViews : pointShadowDepthFaceViewsBySlot) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkImageView& faceView : faceViews) {
			DestroyImageView(faceView);
		}
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::array<VkImageView, 6>& faceViews : pointShadowColorFaceViewsBySlot) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkImageView& faceView : faceViews) {
			DestroyImageView(faceView);
		}
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : directionalShadowDepths) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : directionalShadowColors) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : spotShadowDepths) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : spotShadowColors) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : areaShadowDepths) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : areaShadowColors) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : pointShadowCubeDepths) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : pointShadowCubeColors) {
		DestroyVulkanImage(image);
	}
	DestroyVulkanImage(ltcAmplitudeTexture);
	DestroyVulkanImage(ltcInverseTexture);
	DestroyVulkanImage(environmentIrradianceTexture);
	DestroyVulkanImage(prefilteredEnvironmentTexture);
	DestroyVulkanImage(brdfIntegrationTexture);
	DestroyVulkanImage(ambientOcclusionRaw);
	DestroyVulkanImage(ambientOcclusionBlurred);
	DestroyVulkanImage(ambientOcclusionHistory);
	ambientOcclusionWidth = 0u;
	ambientOcclusionHeight = 0u;
	ambientOcclusionHistoryValid = false;

	textureIndexMap.clear();
	freeTextureIndices.clear();
	nextTextureIndex = 3; // Slots 0-2 are reserved for white, flat-normal, and material-mask defaults.

	// Reset shadow texture indices so they get reallocated on reopen.
	directionalShadowTextureIndices.fill(0u);
	spotShadowTextureIndices.fill(0u);
	areaShadowTextureIndices.fill(0u);
	activeShadowResourceRequest = {};
	shadowStorageRegions.fill({});
	shadowCacheEntries.clear();
	ltcAmplitudeTextureIndex = 0;
	ltcInverseTextureIndex = 0;
	environmentIrradianceTextureIndex = 0;
	prefilteredEnvironmentTextureIndex = 0;
	brdfIntegrationTextureIndex = 0;

	// Reset default texture descriptors to prevent stale views from being sampled if they were overwritten
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (defaultTexture.view != VK_NULL_HANDLE) {
		UpdateGlobalTexture(0, defaultTexture);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (defaultNormalTexture.view != VK_NULL_HANDLE) {
		UpdateGlobalTexture(1, defaultNormalTexture);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (defaultMaterialMaskTexture.view != VK_NULL_HANDLE) {
		UpdateGlobalTexture(2, defaultMaterialMaskTexture);
	}

	materialIndexMap.clear();
	freeMaterialIndices.clear();
	nextMaterialIndex = 0;
}
