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
#include "VulkanFrameDiagnostics.h"
#include "VulkanReadbackConversion.h"
#include "Renderer/RenderCaptureEncoding.h"
#include "Profiling/ProfilerScope.h"
#include "ThreadPool/ThreadPool.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace
{
	using BackendClock = std::chrono::steady_clock;

	constexpr std::uint32_t kMaxSceneDynamicLights = 1024u;
	constexpr float kDirectionalShadowNear = 0.1f;
	constexpr float kDirectionalShadowFar = 180.0f;
	constexpr float kDirectionalShadowExtent = 40.0f;
	constexpr float kTemporalResetDistance = 25.0f;
	constexpr float kTemporalResetMatrixEpsilon = 0.75f;
	constexpr float kDepthHistoryMatrixEpsilon = 0.0005f;
	constexpr std::uint64_t kTemporalStateHistoryFrames = 240u;
	constexpr std::size_t kVulkanParallelRecordingThreshold = 64u;
	constexpr std::size_t kMaxVulkanCaptureReadbacks = 4u;

	void RecordCaptureImageBarrier(
		VkCommandBuffer commandBuffer,
		VkImage image,
		VkImageLayout oldLayout,
		VkImageLayout newLayout)
	{
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

		VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT;
		VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
		{
			barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		}
		else
		{
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT;
		}

		vkCmdPipelineBarrier(
			commandBuffer,
			sourceStage,
			destinationStage,
			0,
			0,
			nullptr,
			0,
			nullptr,
			1,
			&barrier);
	}

	float ElapsedMilliseconds(BackendClock::time_point start)
	{
		return std::chrono::duration<float, std::milli>(BackendClock::now() - start).count();
	}

	std::uint32_t EstimateSelectedPrimitiveCount(
		const TerraEngine::UUID& meshId,
		std::uint32_t fallbackPrimitiveCount,
		std::uint32_t meshletOffset,
		std::uint32_t meshletCount)
	{
		// Render stats are editor diagnostics, not render-critical state. Look
		// up the CPU meshlet table so the visible triangle counter follows the
		// selected virtual-geometry range instead of always showing LOD0.
		const Mesh* cpuMesh = ResourceManager::Instance().GetMesh(meshId);
		if (cpuMesh == nullptr || cpuMesh->meshletRecords.empty())
		{
			return fallbackPrimitiveCount;
		}

		const std::uint32_t clampedOffset = std::min<std::uint32_t>(meshletOffset, static_cast<std::uint32_t>(cpuMesh->meshletRecords.size()));
		const std::uint32_t availableCount = static_cast<std::uint32_t>(cpuMesh->meshletRecords.size()) - clampedOffset;
		const std::uint32_t clampedCount = meshletCount == 0u
			? availableCount
			: std::min(meshletCount, availableCount);

		std::uint32_t primitiveCount = 0u;
		for (std::uint32_t localMeshlet = 0u; localMeshlet < clampedCount; ++localMeshlet)
		{
			const MeshletRecord& record = cpuMesh->meshletRecords[static_cast<std::size_t>(clampedOffset) + localMeshlet];
			primitiveCount += record.primitiveIndexCount / 3u;
		}
		return primitiveCount == 0u ? fallbackPrimitiveCount : primitiveCount;
	}

	void HashVulkanHiZDepthSignatureValue(std::uint64_t& seed, std::uint64_t value)
	{
		seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
	}

	void HashVulkanHiZDepthSignatureFloat(std::uint64_t& seed, float value)
	{
		std::uint32_t bits = 0u;
		std::memcpy(&bits, &value, sizeof(bits));
		HashVulkanHiZDepthSignatureValue(seed, bits);
	}

	void HashVulkanHiZDepthSignatureUuid(std::uint64_t& seed, const TerraEngine::UUID& value)
	{
		for (std::uint8_t byte : value)
		{
			HashVulkanHiZDepthSignatureValue(seed, byte);
		}
	}

	void HashVulkanHiZDepthSignatureMat4(std::uint64_t& seed, const glm::mat4& value)
	{
		for (int column = 0; column < 4; ++column)
		{
			for (int row = 0; row < 4; ++row)
			{
				HashVulkanHiZDepthSignatureFloat(seed, value[column][row]);
			}
		}
	}

	std::uint64_t BuildVulkanHiZDepthSceneSignature(const FrameRenderData& frameData)
	{
		std::uint64_t signature = 1469598103934665603ull;
		HashVulkanHiZDepthSignatureValue(signature, static_cast<std::uint64_t>(frameData.commands.size()));

		for (const DrawCommand& command : frameData.commands)
		{
			// Hi-Z is a previous-frame acceleration structure. Record the exact
			// depth-producing draw identity that created it for diagnostics, but
			// do not use this signature as a hard gate: interactive movement
			// must keep Hi-Z active and let the shader fail open per meshlet.
			HashVulkanHiZDepthSignatureUuid(signature, command.objectId);
			HashVulkanHiZDepthSignatureUuid(signature, command.meshId);
			HashVulkanHiZDepthSignatureUuid(signature, command.materialId);
			HashVulkanHiZDepthSignatureMat4(signature, command.modelMatrix);
			HashVulkanHiZDepthSignatureValue(signature, command.materialIndex);
			HashVulkanHiZDepthSignatureValue(signature, command.meshDataIndex);
			HashVulkanHiZDepthSignatureValue(signature, command.textureIndex);
			HashVulkanHiZDepthSignatureValue(signature, command.virtualMeshletOffset);
			HashVulkanHiZDepthSignatureValue(signature, command.virtualMeshletCount);
			HashVulkanHiZDepthSignatureValue(signature, command.selectedVirtualLod);
			HashVulkanHiZDepthSignatureValue(signature, command.virtualGeometryFlags);
			// Page streaming changes which resident payload backs the same
			// meshlet span. Include the page/source byte identity in the
			// diagnostic depth signature so tooling can see that transition,
			// while validation still keeps ready current pyramids active.
			HashVulkanHiZDepthSignatureValue(signature, command.virtualGeometryPageId);
			HashVulkanHiZDepthSignatureValue(signature, command.virtualGeometryFallbackPageId);
			HashVulkanHiZDepthSignatureValue(signature, static_cast<std::uint64_t>(command.virtualGeometryPayloadSource));
			HashVulkanHiZDepthSignatureValue(signature, static_cast<std::uint64_t>(command.virtualGeometryEmittedSource));
			HashVulkanHiZDepthSignatureValue(signature, command.virtualGeometryPagePayloadByteOffset);
			HashVulkanHiZDepthSignatureValue(signature, command.virtualGeometryPagePayloadByteSize);
			// The selected packet offsets are per-view allocator positions, not
			// visible geometry. Hash counts and meshlet ranges instead so the
			// diagnostic signature changes when coverage changes, not when a
			// transient packet lands at another offset.
			HashVulkanHiZDepthSignatureValue(signature, command.selectedVirtualClusterCount);
			HashVulkanHiZDepthSignatureValue(signature, command.selectedVirtualPageNeedCount);
			HashVulkanHiZDepthSignatureValue(signature, command.taskShaderCullingEnabled ? 1u : 0u);

			const Material* material = ResourceManager::Instance().GetMaterial(command.materialId);
			if (material == nullptr)
			{
				HashVulkanHiZDepthSignatureValue(signature, 0u);
				continue;
			}

			HashVulkanHiZDepthSignatureValue(signature, 1u);
			HashVulkanHiZDepthSignatureValue(signature, material->header.flags);
			HashVulkanHiZDepthSignatureValue(signature, material->header.shadingModel);
			HashVulkanHiZDepthSignatureValue(signature, material->header.alphaMode);
			HashVulkanHiZDepthSignatureUuid(signature, material->header.shaderCollectionId);
			HashVulkanHiZDepthSignatureFloat(signature, material->header.alphaCutoff);
			HashVulkanHiZDepthSignatureValue(signature, static_cast<std::uint64_t>(material->shadowBehavior));
			HashVulkanHiZDepthSignatureValue(signature, material->header.textureCount);
			for (const TextureBinding& binding : material->textures)
			{
				HashVulkanHiZDepthSignatureValue(signature, static_cast<std::uint64_t>(binding.semantic));
				HashVulkanHiZDepthSignatureUuid(signature, binding.textureUuid);
				HashVulkanHiZDepthSignatureValue(signature, static_cast<std::uint64_t>(binding.uvSetIndex));
				HashVulkanHiZDepthSignatureFloat(signature, binding.uvScale[0]);
				HashVulkanHiZDepthSignatureFloat(signature, binding.uvScale[1]);
				HashVulkanHiZDepthSignatureFloat(signature, binding.uvOffset[0]);
				HashVulkanHiZDepthSignatureFloat(signature, binding.uvOffset[1]);
			}
		}

		return signature;
	}

	bool PushDefaultMeshConstants(VkCommandBuffer commandBuffer, const VulkanPipeline& pipeline, const PushConstants& push)
	{
		if (commandBuffer == VK_NULL_HANDLE || pipeline.layout == VK_NULL_HANDLE)
		{
			return false;
		}

		const auto* pushBytes = reinterpret_cast<const std::uint8_t*>(&push);
		bool pushedAnyRange = false;
		for (const VkPushConstantRange& range : pipeline.pushConstants)
		{
			if (range.stageFlags == 0u || range.size == 0u)
			{
				continue;
			}
			if (range.offset >= sizeof(PushConstants) || range.size > sizeof(PushConstants) - range.offset)
			{
				continue;
			}

			// Reflected shader collections can legally expose different
			// push-constant byte ranges per stage while sharing one pipeline
			// layout. Push each declared range separately so the task shader
			// keeps receiving Hi-Z/culling constants even if another stage has a
			// smaller legacy range.
			vkCmdPushConstants(
				commandBuffer,
				pipeline.layout,
				range.stageFlags,
				range.offset,
				range.size,
				pushBytes + range.offset);
			pushedAnyRange = true;
		}
		return pushedAnyRange;
	}

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

// VulkanContext::Impl::MarkDeviceLost is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::MarkDeviceLost(VkResult result, std::string_view operation)
{
	if (result != VK_ERROR_DEVICE_LOST)
	{
		return false;
	}
	bool expectedActive = false;
	if (!deviceLost.compare_exchange_strong(expectedActive, true))
	{
		// Another queue boundary already published the first failure. Returning
		// true still tells this caller to stop, without racing frame containers or
		// replacing the diagnostic that identifies the original fault.
		return true;
	}

	const std::string checkpointSummary = CollectDiagnosticCheckpointSummary();
	skipFrame = true;
	editorRenderSubmitted = false;
	swapchainImageAcquired = false;
	swapchainAcquireWaitConsumed = false;
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestRendererRecoverySnapshot = TerraEngine::Renderer::RecordDeviceLossIncident(
			latestRendererRecoverySnapshot,
			"VK_ERROR_DEVICE_LOST",
			operation,
			++rendererRecoveryFrameSerial,
			renderSessionGeneration);
		TerraEngine::Renderer::Vulkan::ApplyVulkanFirstFailureDiagnostics(
			latestRendererRecoverySnapshot,
			TerraEngine::Renderer::Vulkan::BuildVulkanFirstFailureDiagnostics(
				latestRendererRecoverySnapshot,
				std::string(operation),
				selectedPhysicalDeviceName,
				selectedDriverVersionString,
				selectedApiVersionString));
		latestRendererRecoverySnapshot.lastError = checkpointSummary;
	}
	if (!deviceLostLogged.exchange(true))
	{
		TERRAENGINE_LOG(LogLevel::Critical, "Vulkan device lost during " << operation << ". " << checkpointSummary << " Rendering is disabled for this Vulkan context to avoid a submit/reset recovery loop.");
	}
	return true;
}

void VulkanContext::Impl::RecordDiagnosticCheckpoint(VkCommandBuffer commandBuffer, const char* marker) const
{
	if (!diagnosticCheckpointsEnabled || commandBuffer == VK_NULL_HANDLE || marker == nullptr)
	{
		return;
	}
	// The extension returns this exact pointer after device loss. Callers pass
	// process-lifetime string literals so the marker remains valid until query.
	vkCmdSetCheckpointNV(commandBuffer, const_cast<char*>(marker));
}

std::string VulkanContext::Impl::CollectDiagnosticCheckpointSummary()
{
	if (!diagnosticCheckpointsEnabled || graphicsQueue == VK_NULL_HANDLE || vkGetQueueCheckpointDataNV == nullptr)
	{
		return "GPU checkpoints unavailable.";
	}

	auto StageName = [](VkPipelineStageFlagBits stage) -> const char* {
		switch (stage)
		{
		case VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT: return "TopOfPipe";
		case VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT: return "DrawIndirect";
		case VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT: return "TaskShader";
		case VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT: return "MeshShader";
		case VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT: return "FragmentShader";
		case VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT: return "EarlyFragmentTests";
		case VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT: return "LateFragmentTests";
		case VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT: return "ColorAttachmentOutput";
		case VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT: return "ComputeShader";
		case VK_PIPELINE_STAGE_TRANSFER_BIT: return "Transfer";
		case VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT: return "BottomOfPipe";
		default: return "Other";
		}
	};

	std::lock_guard<std::mutex> queueLock(queueAccessMutex);
	uint32_t checkpointCount = 0u;
	vkGetQueueCheckpointDataNV(graphicsQueue, &checkpointCount, nullptr);
	if (checkpointCount == 0u)
	{
		return "GPU checkpoints enabled, but the driver returned no completed marker.";
	}
	std::vector<VkCheckpointDataNV> checkpoints(checkpointCount);
	for (VkCheckpointDataNV& checkpoint : checkpoints)
	{
		checkpoint.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
	}
	vkGetQueueCheckpointDataNV(graphicsQueue, &checkpointCount, checkpoints.data());

	std::ostringstream summary;
	summary << "GPU checkpoints:";
	for (uint32_t index = 0u; index < checkpointCount; ++index)
	{
		const VkCheckpointDataNV& checkpoint = checkpoints[index];
		const char* marker = static_cast<const char*>(checkpoint.pCheckpointMarker);
		summary << " [stage=" << StageName(checkpoint.stage)
			<< ", marker=" << (marker != nullptr ? marker : "<none>") << "]";
	}
	return summary.str();
}

VkResult VulkanContext::Impl::SubmitGraphicsQueue(std::uint32_t submitCount, const VkSubmitInfo* submits, VkFence fence)
{
	// The same VkQueue is used by frame presentation and background resource
	// uploads. Vulkan requires host access to a queue handle to be externally
	// synchronized, even when the submitted command buffers are independent.
	std::lock_guard<std::mutex> lock(queueAccessMutex);
	return vkQueueSubmit(graphicsQueue, submitCount, submits, fence);
}

VkResult VulkanContext::Impl::PresentSwapchain(const VkPresentInfoKHR* presentInfo)
{
	// Use the graphics-queue mutex for presentation too. On the common queue
	// family path graphicsQueue and presentQueue are the same native handle.
	std::lock_guard<std::mutex> lock(queueAccessMutex);
	return vkQueuePresentKHR(presentQueue, presentInfo);
}

VkResult VulkanContext::Impl::WaitGraphicsQueueIdle()
{
	// Keep worker uploads from entering the queue while a resize/recovery path
	// is proving that all previously submitted graphics work has completed.
	std::lock_guard<std::mutex> lock(queueAccessMutex);
	return vkQueueWaitIdle(graphicsQueue);
}

VkResult VulkanContext::Impl::WaitDeviceIdle()
{
	// Device-idle boundaries destroy or replace queue-owned resources. Block all
	// host queue access until that lifetime transition has completed.
	std::lock_guard<std::mutex> lock(queueAccessMutex);
	return vkDeviceWaitIdle(device);
}

void VulkanContext::Impl::RecordInitializationFailure(std::string_view result, std::string_view operation)
{
	deviceLost.store(true);
	skipFrame = true;
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestRendererRecoverySnapshot = TerraEngine::Renderer::RecordDeviceLossIncident(
			latestRendererRecoverySnapshot,
			result,
			operation,
			++rendererRecoveryFrameSerial,
			renderSessionGeneration);
		TerraEngine::Renderer::Vulkan::ApplyVulkanFirstFailureDiagnostics(
			latestRendererRecoverySnapshot,
			TerraEngine::Renderer::Vulkan::BuildVulkanFirstFailureDiagnostics(
				latestRendererRecoverySnapshot,
				std::string(operation),
				selectedPhysicalDeviceName,
				selectedDriverVersionString,
				selectedApiVersionString));
		latestRendererRecoverySnapshot.message = "Renderer initialization failed before a usable Vulkan session was available.";
	}
	if (!deviceLostLogged.exchange(true))
	{
		TERRAENGINE_LOG(LogLevel::Critical, "Vulkan renderer initialization failed during " << operation << " (" << result << "). Rendering is disabled until recovery can create a fresh renderer session.");
	}
}

VulkanContext::VulkanContext() : impl(std::make_unique<Impl>()) {
	impl->latestStatsByView[0].viewType = RenderViewType::Game;
	impl->latestStatsByView[1].viewType = RenderViewType::Scene;
	impl->latestBudgetDiagnosticsByView[0].viewType = RenderViewType::Game;
	impl->latestBudgetDiagnosticsByView[1].viewType = RenderViewType::Scene;
	impl->latestLightingDiagnosticsByView[0].viewType = RenderViewType::Game;
	impl->latestLightingDiagnosticsByView[1].viewType = RenderViewType::Scene;
	impl->latestRendererRecoverySnapshot = TerraEngine::Renderer::MakeActiveRecoverySnapshot(impl->renderSessionGeneration);
}
VulkanContext::~VulkanContext() = default;

void VulkanContext::SetValidationLayersRequested(bool enabled)
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	// Validation layers must be decided before vkCreateInstance because the
	// layer list and debug-utils extension are instance creation inputs. Export
	// and editor settings call this during RenderSystem startup.
	if (impl->initialized || impl->instance != VK_NULL_HANDLE)
	{
		if (impl->validationLayersRequested != enabled)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Ignoring Vulkan validation-layer change after instance creation. Restart the renderer to apply the new validation setting.");
		}
		return;
	}
	impl->validationLayersRequested = enabled;
	impl->validationLayersEnabled = enabled;
}

void VulkanContext::SetVariableRateShadingEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	// The VRS attachment is baked into render passes and framebuffers. Allow
	// callers to choose it before initialization, but reject late changes until
	// the backend grows a dedicated render-pass recreation path.
	if (impl->initialized)
	{
		if (impl->vrsAttachmentRuntimeEnabled != enabled)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Ignoring variable-rate shading change after Vulkan initialization. Recreate the backend to change the VRS attachment layout.");
		}
		return;
	}
	impl->vrsAttachmentRuntimeEnabled = enabled;
}

// VulkanContext::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Initialize(WindowSystem& windowSystem) {
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	// Build the Vulkan instance/device chain and prime per-frame resources.
	impl->InitVulkan(windowSystem);
}

// VulkanContext::Resize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Resize(int width, int height, ViewType view) {
	if (impl) {
		std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
		impl->framebufferResized = true;
		// The protected frame boundary reads the latest native extent. Do not start
		// a trailing timer here: that made the editor intentionally unresponsive
		// after the user released the window edge.
	}
}

// VulkanContext::BeginFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::BeginFrame() {
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	// Wait until the GPU is done with the resources for this frame before reusing them.
	if (!impl->initialized || impl->deviceLost) return;

	const auto beginFrameStart = BackendClock::now();
	{
		std::lock_guard<std::mutex> lock(impl->statsMutex);
		impl->latestBackendFrameBreakdown = impl->pendingBackendFrameBreakdown;
		impl->pendingBackendFrameBreakdown = TerraEngine::Renderer::Vulkan::CreateVulkanFrameBreakdown(++impl->backendFrameSerial);
	}

	const auto fenceStart = BackendClock::now();
	VkResult fenceWaitResult = VK_SUCCESS;
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: BeginFrame Wait Frame Fence", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if (!impl->editorMode && !impl->inFlightFences.empty()) {
			// Runtime rendering still shares several GPU images across frame slots
			// (depth, shadows, Hi-Z, and AO). Until those resources are per-frame,
			// the backend must not overlap GPU frames in standalone runtime.
			fenceWaitResult = vkWaitForFences(
				impl->device,
				static_cast<std::uint32_t>(impl->inFlightFences.size()),
				impl->inFlightFences.data(),
				VK_TRUE,
				UINT64_MAX);
		} else {
			fenceWaitResult = vkWaitForFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame], VK_TRUE, UINT64_MAX);
		}
	}
	if (impl->MarkDeviceLost(fenceWaitResult, "begin frame fence wait")) {
		return;
	}
	if (fenceWaitResult != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "BeginFrame failed while waiting for frame fence: " << fenceWaitResult);
		impl->skipFrame = true;
		return;
	}
	if (impl->editorMode) {
		// This is the only point where the current frame slot's UI fence is
		// known complete before any new Scene/Game command buffers are recorded.
		// Publish completion and retire resources here; PresentEditor runs after
		// EndFrame has already submitted new work for this same slot.
		impl->editorCompletionTracker.RecordFenceCompletion(impl->currentFrame);
		impl->DrainRetiredOffscreenResources();
	}
	TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
		impl->pendingBackendFrameBreakdown,
		{ TerraEngine::Renderer::BackendFrameCostCategory::FenceWait, TerraEngine::Renderer::BackendSubmitStage::FenceWait, RenderViewType::Game, 0u, impl->editorMode ? "In-flight fence wait" : "Shared attachment frame fence wait", true, false },
		ElapsedMilliseconds(fenceStart),
		0.25f);
	if (impl->editorMode && impl->editorRenderSubmitted) {
		// Editor rendering normally hands renderFinishedSemaphores[currentFrame]
		// to PresentEditor, where the ImGui submit waits on it before the frame
		// slot advances. If the editor UI path was skipped after EndFrame, the
		// binary semaphore is still signaled even though the old frame fence is
		// signaled from an earlier UI frame. Consume that stranded signal before
		// clearing the bookkeeping flag or reusing the frame slot.
		VkSemaphore waitSemaphore = impl->renderFinishedSemaphores[impl->currentFrame];
		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo waitSubmitInfo{};
		waitSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		waitSubmitInfo.waitSemaphoreCount = 1;
		waitSubmitInfo.pWaitSemaphores = &waitSemaphore;
		waitSubmitInfo.pWaitDstStageMask = &waitStage;
		waitSubmitInfo.commandBufferCount = 0;

		const VkResult drainResult = impl->SubmitGraphicsQueue(1, &waitSubmitInfo, VK_NULL_HANDLE);
		if (impl->MarkDeviceLost(drainResult, "stranded editor render semaphore drain submit")) {
			return;
		}
		if (drainResult == VK_SUCCESS) {
			const VkResult idleResult = impl->WaitGraphicsQueueIdle();
			if (impl->MarkDeviceLost(idleResult, "stranded editor render semaphore drain idle")) {
				return;
			}
		} else {
			TERRAENGINE_LOG(LogLevel::Error, "BeginFrame failed to drain stranded editor render semaphore: " << drainResult);
			const VkResult idleResult = impl->WaitGraphicsQueueIdle();
			if (impl->MarkDeviceLost(idleResult, "stranded editor render semaphore failure idle")) {
				return;
			}
		}
		impl->editorRenderSubmitted = false;
		if (impl->currentFrame < impl->pendingPrimaryCommandBuffers.size()) {
			impl->pendingPrimaryCommandBuffers[impl->currentFrame].clear();
		}
		if (impl->currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] = 0u;
		}
	}
	// The fence proves the previous write images for this frame slot are no
	// longer in flight. Promote each view before descriptors are frozen so Scene
	// and Game command buffers sample their own stable completed Hi-Z pyramids
	// while compute writes the other image for the next reuse of this slot.
	bool promotedHiZ = false;
	for (uint32_t hiZViewIndex = 0u; hiZViewIndex < VulkanContext::Impl::kHiZViewCount; ++hiZViewIndex)
	{
		promotedHiZ = impl->PromoteCompletedHiZWrite(hiZViewIndex, impl->currentFrame) || promotedHiZ;
	}
	if (promotedHiZ)
	{
		impl->UpdateGlobalHiZ(impl->currentFrame);
	}
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: BeginFrame Publish Completed Stats", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// The waited frame slot is now complete, so snapshot its GPU counters for UI stats.
		impl->PublishCompletedFrameStats(impl->currentFrame);
	}
	bool resizedHiZ = false;
	for (uint32_t hiZViewIndex = 0u; hiZViewIndex < VulkanContext::Impl::kHiZViewCount; ++hiZViewIndex)
	{
		const glm::uvec2 desiredExtent = impl->desiredHiZExtentByView[hiZViewIndex];
		if (desiredExtent.x == 0u || desiredExtent.y == 0u)
		{
			continue;
		}

		std::array<VulkanHiZ, 2>& frameHiZPair = impl->hiZByViewFrame[hiZViewIndex][impl->currentFrame];
		const auto hiZMatchesDesiredExtent = [&](const VulkanHiZ& hiZ) {
			return hiZ.image != VK_NULL_HANDLE
				&& hiZ.fullView != VK_NULL_HANDLE
				&& hiZ.sampler != VK_NULL_HANDLE
				&& hiZ.mipLevels > 0u
				&& hiZ.width == desiredExtent.x
				&& hiZ.height == desiredExtent.y;
		};
		if (hiZMatchesDesiredExtent(frameHiZPair[0]) && hiZMatchesDesiredExtent(frameHiZPair[1]))
		{
			continue;
		}

		// Scene and Game views resize independently from the swapchain. Rebuild
		// this frame slot only after its fence has completed; doing this inside
		// RenderFrame can destroy a sampler that an earlier view command buffer
		// already bound for the same editor frame.
		const VkFormat depthFmt = impl->FindDepthFormat();
		for (VulkanHiZ& hiZ : frameHiZPair)
		{
			hiZ.Destroy(impl->device, impl->allocator);
			hiZ.Create(impl->device, impl->allocator, desiredExtent.x, desiredExtent.y, depthFmt);
		}
		impl->hiZReadIndexByViewFrame[hiZViewIndex][impl->currentFrame] = 0u;
		impl->hiZWritePendingSwapByViewFrame[hiZViewIndex][impl->currentFrame] = false;
		resizedHiZ = true;
	}
	if (resizedHiZ)
	{
		impl->UpdateGlobalHiZ(impl->currentFrame);
	}
	// Freeze the completed Hi-Z metadata for this CPU frame. RenderFrame may
	// record multiple outputs before the GPU executes any of them; every output
	// must validate against the pyramid that actually exists at BeginFrame,
	// not against a write recorded earlier in the same CPU frame.
	for (uint32_t hiZViewIndex = 0u; hiZViewIndex < VulkanContext::Impl::kHiZViewCount; ++hiZViewIndex)
	{
		impl->readableHiZHistoryByViewFrame[hiZViewIndex][impl->currentFrame] = impl->CurrentHiZ(hiZViewIndex).history;
	}
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

	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: BeginFrame Retire GPU Resources", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Release any extra primary command buffers used for prior camera passes
		// now that the fence guarantees they are idle.
		if (impl->currentFrame < impl->extraPrimaryCommandBuffers.size()) {
			auto& extras = impl->extraPrimaryCommandBuffers[impl->currentFrame];
			if (!extras.empty()) {
				const auto commandLifetimeStart = BackendClock::now();
				vkFreeCommandBuffers(impl->device, impl->commandPools[impl->currentFrame], static_cast<uint32_t>(extras.size()), extras.data());
				TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
					impl->pendingBackendFrameBreakdown,
					{ TerraEngine::Renderer::BackendFrameCostCategory::QueueIdleWait, TerraEngine::Renderer::BackendSubmitStage::CommandBufferLifetime, RenderViewType::Game, 0u, "Free extra primary command buffers", false, false },
					ElapsedMilliseconds(commandLifetimeStart),
					0.25f);
				extras.clear();
			}
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (impl->currentFrame < impl->pendingPrimaryCommandBuffers.size()) {
			impl->pendingPrimaryCommandBuffers[impl->currentFrame].clear();
		}
		if (impl->currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] = 0u;
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
			if (impl->currentFrame < impl->pendingVirtualGeometryPageRetirementsByFrame.size()) {
				// Page-retirement plans are keyed by frame slot, matching buffer
				// retirement. Once this slot's fence is complete, command buffers
				// that referenced those page payloads are idle and the value plans
				// can be drained before new work records into the same slot.
				impl->pendingVirtualGeometryPageRetirementsByFrame[impl->currentFrame].clear();
			}
		}

	// Reset command pool so the main command buffer and any newly allocated
	// per-camera buffers start from a clean state each frame.
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: BeginFrame Reset Command Pools", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const auto resetStart = BackendClock::now();
		const VkResult poolResetResult = vkResetCommandPool(
			impl->device, impl->commandPools[impl->currentFrame], 0);
		if (impl->MarkDeviceLost(poolResetResult, "begin frame command pool reset")) {
			return;
		}
		if (poolResetResult != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "BeginFrame could not reset the completed frame command pool: " << poolResetResult);
			impl->skipFrame = true;
			return;
		}
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::RhiBeginFrame, TerraEngine::Renderer::BackendSubmitStage::CommandBufferReset, RenderViewType::Game, 0u, "Reset primary command pool", true, false },
			ElapsedMilliseconds(resetStart));
		// Reset worker pools for this frame once so secondary buffers can be rerecorded safely.
		if (impl->currentFrame < impl->workerCommandPools.size()) {
			auto& framePools = impl->workerCommandPools[impl->currentFrame];
			for (VkCommandPool pool : framePools) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (pool != VK_NULL_HANDLE) {
					const auto workerResetStart = BackendClock::now();
					vkResetCommandPool(impl->device, pool, 0);
					TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
						impl->pendingBackendFrameBreakdown,
						{ TerraEngine::Renderer::BackendFrameCostCategory::RhiBeginFrame, TerraEngine::Renderer::BackendSubmitStage::CommandBufferReset, RenderViewType::Game, 0u, "Reset worker command pool", true, false },
						ElapsedMilliseconds(workerResetStart));
				}
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
	impl->swapchainAcquireWaitConsumed = false;
	impl->skipFrame = false;
	impl->editorRenderSubmitted = false;

	// Acquire the swapchain image once per frame so multiple camera passes can
	// render to the same back buffer and present only on the final camera.
	if (!impl->editorMode) {
		const auto acquireStart = BackendClock::now();
		VkResult result = VK_SUCCESS;
		{
			TERRAENGINE_PROFILE_SCOPE("Vulkan: BeginFrame Acquire Swapchain Image", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			result = vkAcquireNextImageKHR(impl->device, impl->swapchain, UINT64_MAX, impl->imageAvailableSemaphores[impl->currentFrame], VK_NULL_HANDLE, &impl->acquiredImageIndex);
		}
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::FenceWait, TerraEngine::Renderer::BackendSubmitStage::ImageAcquire, RenderViewType::Game, 0u, "Swapchain image acquire", true, false },
			ElapsedMilliseconds(acquireStart),
			0.25f);
		if (impl->MarkDeviceLost(result, "runtime swapchain image acquire")) {
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameCost(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::RhiBeginFrame, TerraEngine::Renderer::BackendSubmitStage::Other, RenderViewType::Game, 0u, "RHI Begin Frame", true, false },
				ElapsedMilliseconds(beginFrameStart));
			return;
		}
		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			const auto recoveryStart = BackendClock::now();
			impl->RecreateSwapChain();
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::ResizeRecreateWait, TerraEngine::Renderer::BackendSubmitStage::SwapchainRecovery, RenderViewType::Game, 0u, "Recreate swapchain after acquire", true, false },
				ElapsedMilliseconds(recoveryStart),
				0.25f,
				"Swapchain acquire requested recovery before rendering this frame.");
			impl->skipFrame = true;
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameCost(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::RhiBeginFrame, TerraEngine::Renderer::BackendSubmitStage::Other, RenderViewType::Game, 0u, "RHI Begin Frame", true, false },
				ElapsedMilliseconds(beginFrameStart));
			return;
		} else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to acquire swap chain image! Result: " << result);
			impl->skipFrame = true;
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameCost(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::RhiBeginFrame, TerraEngine::Renderer::BackendSubmitStage::Other, RenderViewType::Game, 0u, "RHI Begin Frame", true, false },
				ElapsedMilliseconds(beginFrameStart));
			return;
		}
		impl->swapchainImageAcquired = true;
		impl->swapchainAcquireWaitConsumed = false;
		impl->acquiredPresentationImage = TerraEngine::Renderer::Vulkan::AcquiredPresentationImage{
			impl->currentFrame,
			impl->acquiredImageIndex,
			impl->presentationLifecycle.Generation()
		};
		if (!impl->presentationLifecycle.Validate(*impl->acquiredPresentationImage)
			|| impl->acquiredImageIndex >= impl->uiSemaphores.size()) {
			TERRAENGINE_LOG(LogLevel::Error, "Rejected an acquired image whose index or presentation generation is stale.");
			impl->presentationLifecycle.RequestRecreation();
			impl->skipFrame = true;
			return;
		}
	}
	TerraEngine::Renderer::Vulkan::RecordVulkanFrameCost(
		impl->pendingBackendFrameBreakdown,
		{ TerraEngine::Renderer::BackendFrameCostCategory::RhiBeginFrame, TerraEngine::Renderer::BackendSubmitStage::Other, RenderViewType::Game, 0u, "RHI Begin Frame", true, false },
		ElapsedMilliseconds(beginFrameStart));
}

// Create Grid Pipeline for Scene View
// VulkanContext::Impl::CreateSceneViewGridPipeline is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateSceneViewGridPipeline(VkRenderPass renderPass) {
	if (sceneViewGridPipeline != VK_NULL_HANDLE) return;

	// Load SPIR-V
	auto loadShader = [&](const std::string& path) -> VkShaderModule {
		std::filesystem::path shaderPath(path);
		if (shaderPath.is_relative())
		{
			// Editor debug shaders are installed beside the executable, not the
			// current working directory. Resolve through ResourceManager so
			// --project launches from arbitrary folders still find them.
			shaderPath = ResourceManager::Instance().GetEngineRoot() / shaderPath;
		}

		std::ifstream file(shaderPath, std::ios::ate | std::ios::binary);
		if (!file.is_open()) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to open shader file: " << shaderPath);
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
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module for: " << shaderPath);
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
void VulkanContext::Impl::RenderSceneViewGrid(VkCommandBuffer cmd, const FrameRenderData& frameData, VkExtent2D renderExtent) {
	if (sceneViewGridPipeline == VK_NULL_HANDLE) return;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneViewGridPipeline);

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = static_cast<float>(renderExtent.width);
	viewport.height = static_cast<float>(renderExtent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.offset = {0, 0};
	scissor.extent = renderExtent;
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
	glm::vec4 editorTint = glm::vec4(0.0f);
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
void VulkanContext::Impl::RenderSkyBackground(VkCommandBuffer cmd, const FrameRenderData& frameData, VkExtent2D renderExtent)
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
	viewport.width = static_cast<float>(renderExtent.width);
	viewport.height = static_cast<float>(renderExtent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.offset = {0, 0};
	scissor.extent = renderExtent;
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	const glm::mat4 inverseViewMatrix = glm::inverse(frameData.viewMatrix);
	const glm::quat cameraOrientation = glm::normalize(glm::quat_cast(glm::mat3(inverseViewMatrix)));
	const bool orthographicProjection = std::abs(frameData.projectionMatrix[3][3]) > 0.5f;
	// Orthographic projection scale is a zoom value, not a perspective FOV.
	// Keep the procedural sky from stretching as the Scene View ortho size changes.
	const float tanHalfFovX = orthographicProjection ? 1.0f : 1.0f / std::max(std::abs(frameData.projectionMatrix[0][0]), 0.0001f);
	const float tanHalfFovY = orthographicProjection ? 1.0f : 1.0f / std::max(std::abs(frameData.projectionMatrix[1][1]), 0.0001f);

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
	if (frameData.sceneViewOrthographic)
	{
		// Scene View orthographic mode still uses the sky pipeline, but the
		// shader switches to a screen-space editor gradient instead of world sky.
		skyFlags |= 4u;
	}
	push.skyParams = glm::vec4(tanHalfFovX, tanHalfFovY, glm::radians(frameData.sky.rotationOffsetDegrees), static_cast<float>(skyFlags));
	push.editorTint = frameData.editorContextTint;

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
	if (uiStaticVertexBuffers.size() < MAX_FRAMES_IN_FLIGHT) {
		uiStaticVertexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	}
	if (uiSdfVertexBuffers.size() < MAX_FRAMES_IN_FLIGHT) {
		uiSdfVertexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	}
	// BeginFrame has waited this exact slot's fence. Writing only the slot-owned
	// buffers prevents CPU UI generation from racing a different in-flight slot.
	VulkanBuffer& uiStaticVertexBuffer = uiStaticVertexBuffers[currentFrame];
	VulkanBuffer& uiSdfVertexBuffer = uiSdfVertexBuffers[currentFrame];

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
			// CPU_TO_GPU allocations are not guaranteed to be host-coherent on
			// every driver. Flush the exact UI vertex range before Vulkan reads it.
			vmaFlushAllocation(allocator, uiStaticVertexBuffer.allocation, 0, bufferSize);
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
	if (!uiFontAtlasSourceLoadAttempted || uiFontAtlasSourcePath != fontPath) {
		const std::vector<std::uint8_t> fontBytes = ResourceManager::Instance().ReadFileBytes(fontPath);
		uiFontAtlasSourceBytes.assign(fontBytes.begin(), fontBytes.end());
		uiFontAtlasSourcePath = fontPath;
		uiFontAtlasSourceLoadAttempted = true;
		uiFontAtlasMissingWarned = false;
	}
	if (uiFontAtlasSourceBytes.empty()) {
		if (!uiFontAtlasMissingWarned) {
			TERRAENGINE_LOG(LogLevel::Warning, "Runtime UI text font is unavailable. Missing or empty font: " << fontPath);
			uiFontAtlasMissingWarned = true;
		}
		return;
	}
	const UIFontAtlas& fontAtlas = uiTextRenderer.EnsureSignedDistanceFontAtlasFromBytes(fontLogicalPath.generic_string(), uiFontAtlasSourceBytes, uiFontAtlasPixelHeight);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fontAtlas.ready || fontAtlas.width <= 0 || fontAtlas.height <= 0 || fontAtlas.pixels.empty()) {
		if (!uiFontAtlasMissingWarned) {
			TERRAENGINE_LOG(LogLevel::Warning, "Runtime UI text atlas is unavailable. Missing or invalid font: " << fontPath);
			uiFontAtlasMissingWarned = true;
		}
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
		// Font atlas staging can also use non-coherent host-visible memory in
		// optimized builds, so flush before copying into the GPU-only image.
		vmaFlushAllocation(allocator, stagingBuffer.allocation, 0, imageSize);
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
	// SDF text vertices are written by the CPU and consumed by the GPU in the
	// same submitted command buffer, so make the mapped writes explicitly visible.
	vmaFlushAllocation(allocator, uiSdfVertexBuffer.allocation, 0, textBufferSize);
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
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	// Skip rendering if initialization failed so we do not spam validation errors.
	if (!impl->initialized || impl->skipFrame || impl->deviceLost) return;
	if (impl->editorMode && impl->framebufferResized) {
		// Native resize callbacks change both the swapchain and the docked viewport
		// dimensions. EndFrame owns the settled swapchain rebuild; recording here
		// would only resize Scene/Game targets and then discard their command
		// buffers. Pause view recording until that protected boundary completes so
		// one resize drag produces one set of offscreen allocations and descriptors.
		return;
	}

	const auto renderFrameStart = BackendClock::now();
	const uint32_t currentFrame = impl->currentFrame;
	const RenderViewType renderViewType = (view == ViewType::Scene) ? RenderViewType::Scene : RenderViewType::Game;
	const std::uint32_t diagnosticViewId = outputRequest.outputId;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		impl->pendingPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
	}
	if (currentFrame >= impl->pendingPrimaryCommandBuffersSubmitted.size()) {
		impl->pendingPrimaryCommandBuffersSubmitted.resize(impl->MAX_FRAMES_IN_FLIGHT, 0u);
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
		const VkResult submitResult = impl->SubmitGraphicsQueue(1, &pendingSubmitInfo, VK_NULL_HANDLE);
		if (impl->MarkDeviceLost(submitResult, "intermediate editor view submit after snapshot budget exhaustion")) {
			return;
		}
		if (submitResult != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to submit intermediate editor view command buffers after snapshot budget exhaustion: " << submitResult);
			return;
		}
		if (currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[currentFrame] = 1u;
		}
		impl->ConfirmHiZWritesSubmitted(currentFrame);
		const auto forcedIdleStart = BackendClock::now();
		const VkResult forcedIdleResult = impl->WaitGraphicsQueueIdle();
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::QueueIdleWait, TerraEngine::Renderer::BackendSubmitStage::ForcedIdle, renderViewType, diagnosticViewId, "Queue idle after snapshot budget exhaustion", false, true },
			ElapsedMilliseconds(forcedIdleStart),
			0.25f,
			"Snapshot budget exhaustion forced serialization; increase snapshot capacity or reduce required editor outputs.");
		if (forcedIdleResult != VK_SUCCESS)
		{
			if (impl->MarkDeviceLost(forcedIdleResult, "intermediate editor view queue idle after snapshot budget exhaustion")) {
				return;
			}
			TERRAENGINE_LOG(LogLevel::Error, "failed to wait for intermediate editor view command buffers after snapshot budget exhaustion: " << forcedIdleResult);
			return;
		}
		pendingCmdsForSnapshot.clear();
		if (currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[currentFrame] = 0u;
		}
		fallback.result = RenderSnapshotFallbackResult::SerializedSafely;
		impl->pendingRenderSnapshotDiagnostics.fallbackEvents.push_back(fallback);
		snapshotSlotIndex = 0u;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!impl->editorMode && !impl->swapchainImageAcquired) {
		return;
	}

	const bool renderingDedicatedCaptureTarget = impl->editorMode
		&& outputRequest.kind == RenderOutputKind::OffscreenTexture;
	VkExtent2D requestedCaptureExtent = impl->swapchainExtent;
	if (renderingDedicatedCaptureTarget)
	{
		// MCP object/custom captures are not editor viewport snapshots. Allocate
		// their private render target at the requested dimensions so projection,
		// culling, rasterization, and readback all agree on the same pixel grid.
		requestedCaptureExtent.width = outputRequest.targetSize.x > 0u
			? outputRequest.targetSize.x
			: std::max<std::uint32_t>(impl->swapchainExtent.width, 1u);
		requestedCaptureExtent.height = outputRequest.targetSize.y > 0u
			? outputRequest.targetSize.y
			: std::max<std::uint32_t>(impl->swapchainExtent.height, 1u);
	}
	const auto ResolveRequestedEditorExtent = [&]() {
		VkExtent2D extent = impl->swapchainExtent;
		extent.width = outputRequest.targetSize.x > 0u
			? outputRequest.targetSize.x
			: std::max<std::uint32_t>(impl->swapchainExtent.width, 1u);
		extent.height = outputRequest.targetSize.y > 0u
			? outputRequest.targetSize.y
			: std::max<std::uint32_t>(impl->swapchainExtent.height, 1u);
		return extent;
	};
	const VkExtent2D requestedEditorExtent = ResolveRequestedEditorExtent();

	// Handle Editor Mode resizing (lazy).
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->editorMode) {
		const auto ensureOffscreenResources = [&](VulkanContext::Impl::OffscreenResources& resources, VkExtent2D targetExtent) {
			const bool extentMismatch = resources.extent.width != targetExtent.width
				|| resources.extent.height != targetExtent.height;
			if (resources.framebuffer == VK_NULL_HANDLE || extentMismatch)
			{
				// Replacement is transactional. The old target moves to the shared
				// editor submission/readback retirement domain after publication.
				impl->CreateOffscreenResources(resources, targetExtent);
			}
		};
		if (!renderingDedicatedCaptureTarget)
		{
			if (view == ViewType::Scene)
			{
				ensureOffscreenResources(impl->sceneView, requestedEditorExtent);
			}
			else
			{
				ensureOffscreenResources(impl->gameView, requestedEditorExtent);
			}
		}
		if (impl->deviceLost)
		{
			return;
		}
		if (renderingDedicatedCaptureTarget
			&& (impl->captureView.framebuffer == VK_NULL_HANDLE
				|| impl->captureView.extent.width != requestedCaptureExtent.width
				|| impl->captureView.extent.height != requestedCaptureExtent.height)) {
			impl->CreateOffscreenResources(impl->captureView, requestedCaptureExtent);
		}
	}

	// 1. Prepare All Resources (Pipelines, Meshes, Materials) BEFORE starting command recording.
	// This ensures descriptor set updates don't invalidate the recording command buffer.
	const TerraEngine::UUID defaultMeshCollectionId = ResourceManager::Instance().GetDefaultShaderCollectionID();
	const TerraEngine::UUID defaultTaskCollectionId = ResourceManager::Instance().GetDefaultTaskShaderCollectionID();
	const bool defaultTaskPipelineAvailable = impl->taskShaderSupported
		&& (ResourceManager::Instance().GetShaderCollection(defaultTaskCollectionId) != nullptr);

	VkRenderPass currentPass = impl->editorMode ? impl->offscreenRenderPass : impl->renderPass;
	if (currentPass == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Warning, "Skipping Vulkan draw submission because the active render pass is not ready.");
		return;
	}

	VkImage activeDepthImage = impl->depthImage;
	VkImageView activeDepthView = impl->depthImageView;
	VkExtent2D activeRenderExtent = impl->swapchainExtent;
	if (impl->editorMode) {
		const VulkanContext::Impl::OffscreenResources* activeOffscreenView = renderingDedicatedCaptureTarget
			? &impl->captureView
			: ((view == ViewType::Scene) ? &impl->sceneView : &impl->gameView);
		activeDepthImage = activeOffscreenView->depthImage;
		activeDepthView = activeOffscreenView->depthView;
		activeRenderExtent = activeOffscreenView->extent;
		if (activeOffscreenView->image == VK_NULL_HANDLE
			|| activeOffscreenView->framebuffer == VK_NULL_HANDLE
			|| activeRenderExtent.width == 0u
			|| activeRenderExtent.height == 0u) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping Vulkan draw submission because the editor offscreen color target is not ready.");
			return;
		}
		if (activeDepthImage == VK_NULL_HANDLE || activeDepthView == VK_NULL_HANDLE) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping Vulkan draw submission because the editor view depth attachment is not ready.");
			return;
		}
	}

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
	const auto descriptorStart = BackendClock::now();
	for (const auto& command : frameData.commands) {
		PrepareDrawResources(command);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ShadowDrawList& shadowDrawList : frameData.shadowDrawLists) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const DrawCommand& command : shadowDrawList.commands) {
			// Shadow virtual geometry arrives as light-view draw commands with
			// virtualMeshletOffset/Count already selected by RenderSystem. The
			// backend prepares the same mesh-shader resources as the camera path,
			// but shadow visibility never reuses camera Hi-Z decisions.
			PrepareDrawResources(command);
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameData.sky.hasTexture && frameData.sky.textureId != TerraEngine::EMPTY_UUID)
	{
		impl->EnsureTexture(frameData.sky.textureId);
	}
	TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
		impl->pendingBackendFrameBreakdown,
		{ TerraEngine::Renderer::BackendFrameCostCategory::DescriptorUpdate, TerraEngine::Renderer::BackendSubmitStage::DescriptorUpdate, renderViewType, diagnosticViewId, "Prepare descriptors/pipelines/resources", false, true },
		ElapsedMilliseconds(descriptorStart));

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	const bool useMainCommandBuffer = impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()
		|| impl->pendingPrimaryCommandBuffers[impl->currentFrame].empty();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (useMainCommandBuffer) {
		cmd = impl->commandBuffers[impl->currentFrame];
		const auto resetCommandStart = BackendClock::now();
		vkResetCommandBuffer(cmd, 0);
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::RhiBeginFrame, TerraEngine::Renderer::BackendSubmitStage::CommandBufferReset, renderViewType, diagnosticViewId, "Reset primary command buffer", false, true },
			ElapsedMilliseconds(resetCommandStart));
	} else {
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = impl->commandPools[impl->currentFrame];
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		const auto allocateCommandStart = BackendClock::now();
		if (vkAllocateCommandBuffers(impl->device, &allocInfo, &cmd) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to allocate command buffer for additional camera pass");
			return;
		}
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::QueueIdleWait, TerraEngine::Renderer::BackendSubmitStage::CommandBufferLifetime, renderViewType, diagnosticViewId, "Allocate additional camera command buffer", false, true },
			ElapsedMilliseconds(allocateCommandStart),
			0.25f);
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
	const auto commandRecordingStart = BackendClock::now();
	if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
		return;
	}
	impl->RecordDiagnosticCheckpoint(
		cmd,
		renderingDedicatedCaptureTarget
			? "capture.primary.begin"
			: (view == ViewType::Scene ? "scene.primary.begin" : "game.primary.begin"));
	if (impl->validationLayersEnabled && vkSetDebugUtilsObjectNameEXT != nullptr) {
		// Validation messages otherwise print only an opaque command-buffer handle,
		// which makes a cross-submit fault impossible to associate with Scene, Game,
		// capture, or runtime rendering after a temporary buffer has been freed.
		const std::string commandBufferName = std::string(impl->editorMode ? "Editor " : "Runtime ")
			+ (renderingDedicatedCaptureTarget ? "Capture" : (view == ViewType::Scene ? "Scene" : "Game"))
			+ " Primary Render Command Buffer";
		VkDebugUtilsObjectNameInfoEXT nameInfo{};
		nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
		nameInfo.objectType = VK_OBJECT_TYPE_COMMAND_BUFFER;
		nameInfo.objectHandle = reinterpret_cast<uint64_t>(cmd);
		nameInfo.pObjectName = commandBufferName.c_str();
		vkSetDebugUtilsObjectNameEXT(impl->device, &nameInfo);
	}
	impl->MarkGpuTimestampBegin(cmd, impl->currentFrame, view, impl->backendFrameSerial);

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
		renderPassInfo.framebuffer = renderingDedicatedCaptureTarget
			? impl->captureView.framebuffer
			: ((view == ViewType::Scene) ? impl->sceneView.framebuffer : impl->gameView.framebuffer);
	}

	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = activeRenderExtent;

	std::array<VkClearValue, 3> clearValues{};
	if (view == ViewType::Scene && frameData.sceneViewOrthographic)
	{
		clearValues[0].color = {{0.36f, 0.43f, 0.50f, 1.0f}};
	}
	else
	{
		clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
	}
	clearValues[1].depthStencil = {1.0f, 0};
	clearValues[2].color = {{0, 0, 0, 0}};
	renderPassInfo.clearValueCount = impl->UsesVrsAttachment() ? 3 : 2;
	renderPassInfo.pClearValues = clearValues.data();

	// Create Grid Pipeline if needed (lazy init).
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (view == ViewType::Scene && frameData.sceneViewGridVisible) {
		impl->CreateSceneViewGridPipeline(renderPassInfo.renderPass);
	}
	impl->CreateSkyBackgroundPipeline(renderPassInfo.renderPass);

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
	// Publish immutable per-view packet mirrors at the protected Vulkan
	// boundary. RenderSystem owns the value records; Vulkan owns the backing
	// buffers and may reuse them only when the frame slot and view identity match.
	impl->PublishVirtualGeometryGpuPacketResources(frameData, renderViewType, diagnosticViewId, impl->backendFrameSerial);
	const std::uint64_t generatedSubmissionFrameId = frameData.virtualGeometryGpuPackets.empty()
		? impl->backendFrameSerial
		: frameData.virtualGeometryGpuPackets.front().frameId;
	bool generatedVirtualGeometrySubmissionsUsable = !frameData.virtualGeometryGeneratedSubmissions.empty();
	for (const GeneratedVirtualGeometrySubmissionRecord& submission : frameData.virtualGeometryGeneratedSubmissions)
	{
		const bool rangeValid = submission.packetCount > 0u
			&& submission.packetOffset <= frameData.virtualGeometryGpuPackets.size()
			&& static_cast<std::size_t>(submission.packetOffset) + static_cast<std::size_t>(submission.packetCount) <= frameData.virtualGeometryGpuPackets.size();
		generatedVirtualGeometrySubmissionsUsable = generatedVirtualGeometrySubmissionsUsable
			&& submission.frameId == generatedSubmissionFrameId
			&& submission.viewType == renderViewType
			&& submission.viewId == diagnosticViewId
			&& submission.validationState == VirtualGeometryGeneratedSubmissionValidationState::Valid
			&& rangeValid;
	}
	if (!generatedVirtualGeometrySubmissionsUsable)
	{
		// Generated submissions are an optimization authority, not the coverage
		// authority. If they are missing, delayed, or stale, command recording
		// continues with the already expanded CPU-selected draw commands below.
		impl->activeVirtualGeometryGeneratedSubmissionBufferIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
	}
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
			// Shadow images are explicitly initialized and persist across frames.
			// Preserve their real attachment layout instead of discarding it again.
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription depthAttachment{};
			depthAttachment.format = impl->depthFormat;
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			// RecordShadowPass explicitly transitions the reused depth layer back to
			// attachment layout. Keeping both layouts equal prevents an implicit
			// begin-pass transition from racing a write in an earlier queue submit.
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
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
			// Shadow images persist across frames. Order the previous render-pass
			// writes and fragment sampling before clearing the same attachments again;
			// queue submission order alone is not a memory dependency for these accesses.
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
				| VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
				| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT
				| VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
				| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
				| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
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
			else if (lightType == 1u) {
				light.shadowAndPadding.y = static_cast<float>(shadowSlotIndex);
			}
			else if (lightType == 2u) {
				light.shadowAndPadding.y = static_cast<float>(impl->spotShadowTextureIndices[shadowSlotIndex]);
				StoreShadowViewProjection(light, BuildSpotShadowViewProjection(light));
			}
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

		auto CreateComputePipelineWithLayout = [&](VkPipeline& pipeline, VkPipelineLayout layout, const char* shaderPath) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (pipeline != VK_NULL_HANDLE || layout == VK_NULL_HANDLE) {
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
			pipelineInfo.layout = layout;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vkCreateComputePipelines(impl->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
				pipeline = VK_NULL_HANDLE;
			}
			vkDestroyShaderModule(impl->device, module, nullptr);
		};

		auto CreateComputePipeline = [&](VkPipeline& pipeline, const char* shaderPath) {
			CreateComputePipelineWithLayout(pipeline, impl->lightingComputePipelineLayout, shaderPath);
		};

		if (impl->hiZDescriptorSetLayout == VK_NULL_HANDLE) {
			std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
			bindings[0].binding = 0u;
			bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			bindings[0].descriptorCount = 1u;
			bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings[1].binding = 1u;
			bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			bindings[1].descriptorCount = 1u;
			bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkDescriptorSetLayoutCreateInfo hiZLayoutInfo{};
			hiZLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			hiZLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
			hiZLayoutInfo.pBindings = bindings.data();
			// Hi-Z has a tiny private descriptor set so mip generation never
			// rewrites global bindings that later passes still depend on.
			if (vkCreateDescriptorSetLayout(impl->device, &hiZLayoutInfo, nullptr, &impl->hiZDescriptorSetLayout) != VK_SUCCESS) {
				impl->hiZDescriptorSetLayout = VK_NULL_HANDLE;
			}
		}

		if (impl->hiZPipelineLayout == VK_NULL_HANDLE && impl->hiZDescriptorSetLayout != VK_NULL_HANDLE) {
			VkPushConstantRange hiZPushRange{};
			hiZPushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			hiZPushRange.offset = 0u;
			hiZPushRange.size = 32u;

			VkPipelineLayoutCreateInfo hiZPipelineLayoutInfo{};
			hiZPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			hiZPipelineLayoutInfo.setLayoutCount = 1u;
			hiZPipelineLayoutInfo.pSetLayouts = &impl->hiZDescriptorSetLayout;
			hiZPipelineLayoutInfo.pushConstantRangeCount = 1u;
			hiZPipelineLayoutInfo.pPushConstantRanges = &hiZPushRange;
			// The push constants are source width/height and output width/height.
			if (vkCreatePipelineLayout(impl->device, &hiZPipelineLayoutInfo, nullptr, &impl->hiZPipelineLayout) != VK_SUCCESS) {
				impl->hiZPipelineLayout = VK_NULL_HANDLE;
			}
		}

		if (impl->aoDescriptorSetLayout == VK_NULL_HANDLE) {
			std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
			bindings[0].binding = 12u;
			bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			bindings[0].descriptorCount = 1u;
			bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings[1].binding = 21u;
			bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			bindings[1].descriptorCount = 1u;
			bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings[2].binding = 22u;
			bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			bindings[2].descriptorCount = 1u;
			bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings[3].binding = 23u;
			bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			bindings[3].descriptorCount = 1u;
			bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings[4].binding = 24u;
			bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			bindings[4].descriptorCount = 1u;
			bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkDescriptorSetLayoutCreateInfo aoLayoutInfo{};
			aoLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			aoLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
			aoLayoutInfo.pBindings = bindings.data();
			// AO compute uses per-snapshot descriptor sets because editor scene,
			// game, and additional camera passes can be recorded before the GPU
			// executes them. Rewriting the global descriptor set made optimized
			// builds sample whichever AO image was updated last.
			if (vkCreateDescriptorSetLayout(impl->device, &aoLayoutInfo, nullptr, &impl->aoDescriptorSetLayout) != VK_SUCCESS) {
				impl->aoDescriptorSetLayout = VK_NULL_HANDLE;
			}
		}

		if (impl->aoPipelineLayout == VK_NULL_HANDLE && impl->aoDescriptorSetLayout != VK_NULL_HANDLE) {
			VkPushConstantRange aoPushRange{};
			aoPushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			aoPushRange.offset = 0u;
			aoPushRange.size = 16u;

			VkPipelineLayoutCreateInfo aoPipelineLayoutInfo{};
			aoPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			aoPipelineLayoutInfo.setLayoutCount = 1u;
			aoPipelineLayoutInfo.pSetLayouts = &impl->aoDescriptorSetLayout;
			aoPipelineLayoutInfo.pushConstantRangeCount = 1u;
			aoPipelineLayoutInfo.pPushConstantRanges = &aoPushRange;
			if (vkCreatePipelineLayout(impl->device, &aoPipelineLayoutInfo, nullptr, &impl->aoPipelineLayout) != VK_SUCCESS) {
				impl->aoPipelineLayout = VK_NULL_HANDLE;
			}
		}

		CreateComputePipeline(impl->clusterGridComputePipeline, "Shaders/Lighting/ClusterGrid.comp.spv");
		CreateComputePipeline(impl->clusterDepthMaskComputePipeline, "Shaders/Lighting/ClusterDepthMask.comp.spv");
		CreateComputePipeline(impl->clusterLightCountComputePipeline, "Shaders/Lighting/ClusterLightCount.comp.spv");
		CreateComputePipeline(impl->clusterLightPrefixComputePipeline, "Shaders/Lighting/ClusterLightPrefix.comp.spv");
		CreateComputePipeline(impl->clusterLightFillComputePipeline, "Shaders/Lighting/ClusterLightFill.comp.spv");
		CreateComputePipeline(impl->lightAnalysisComputePipeline, "Shaders/Lighting/LightCull.comp.spv");
		CreateComputePipelineWithLayout(impl->hiZComputePipeline, impl->hiZPipelineLayout, "Shaders/PostProcess/HiZGenerate.comp.spv");
		CreateComputePipeline(impl->vrsComputePipeline, "Shaders/PostProcess/VRSGenerate.comp.spv");
		CreateComputePipelineWithLayout(impl->gtaoComputePipeline, impl->aoPipelineLayout, "Shaders/PostProcess/GTAO.comp.spv");
		CreateComputePipelineWithLayout(impl->gtaoBlurComputePipeline, impl->aoPipelineLayout, "Shaders/PostProcess/GTAOBlur.comp.spv");
	};

	EnsureLightingComputePipelines();

	const glm::mat4 currentViewProjection = frameData.projectionMatrix * frameData.viewMatrix;
	VulkanContext::Impl::ViewDepthHistoryState& depthHistory = impl->depthHistoryByView[viewIndex];
	const bool firstPrimaryPassForFrame = useMainCommandBuffer;
	const bool aoDepthHistorySlotReady = impl->currentFrame < depthHistory.ambientOcclusionDepthByFrame.size()
		&& depthHistory.ambientOcclusionDepthByFrame[impl->currentFrame].view != VK_NULL_HANDLE;
	const bool aoDepthHistoryMatchesView = !impl->editorMode
		&& firstPrimaryPassForFrame
		&& aoDepthHistorySlotReady
		&& depthHistory.ambientOcclusionDepthValidByFrame[impl->currentFrame]
		&& MatricesApproximatelyEqual(
			currentViewProjection,
			depthHistory.ambientOcclusionDepthViewProjectionByFrame[impl->currentFrame],
			kDepthHistoryMatrixEpsilon);
	VkImageView aoDepthHistoryView = aoDepthHistoryMatchesView
		? depthHistory.ambientOcclusionDepthByFrame[impl->currentFrame].view
		: VK_NULL_HANDLE;
	const std::uint32_t hiZTargetWidth = std::max<std::uint32_t>(renderPassInfo.renderArea.extent.width, 1u);
	const std::uint32_t hiZTargetHeight = std::max<std::uint32_t>(renderPassInfo.renderArea.extent.height, 1u);
	const uint32_t hiZViewIndex = impl->HiZViewIndexForView(view);
	if (hiZViewIndex < VulkanContext::Impl::kHiZViewCount)
	{
		// Record the true per-view render target size. BeginFrame consumes this
		// at a fence-safe boundary before any new command buffer can bind the
		// global Hi-Z descriptors for the current frame slot.
		impl->desiredHiZExtentByView[hiZViewIndex] = glm::uvec2(hiZTargetWidth, hiZTargetHeight);
	}
	const std::uint64_t hiZProjectionSignature = BuildVulkanHiZProjectionSignature(currentViewProjection);
	const std::uint64_t hiZDepthSceneSignature = BuildVulkanHiZDepthSceneSignature(frameData);
	VulkanHiZ& currentFrameHiZ = impl->CurrentHiZ(hiZViewIndex);
	VulkanHiZ& writableFrameHiZ = impl->WritableHiZForFrame(hiZViewIndex, impl->currentFrame);
	const VulkanHiZHistoryMetadata& readableHiZHistory = impl->readableHiZHistoryByViewFrame[hiZViewIndex][impl->currentFrame];
	const VulkanHiZViewClass hiZViewClass = outputRequest.kind == RenderOutputKind::MainRuntimeView
		? VulkanHiZViewClass::ExportRuntime
		: (renderViewType == RenderViewType::Scene ? VulkanHiZViewClass::Scene : VulkanHiZViewClass::Game);
	const std::uint32_t hiZTargetId = outputRequest.outputId;
	VulkanHiZHistoryMetadata currentHiZHistory{};
	currentHiZHistory.viewId = viewIndex;
	currentHiZHistory.viewClass = hiZViewClass;
	currentHiZHistory.targetId = hiZTargetId;
	currentHiZHistory.width = hiZTargetWidth;
	currentHiZHistory.height = hiZTargetHeight;
	currentHiZHistory.projectionSignature = hiZProjectionSignature;
	currentHiZHistory.depthSceneSignature = hiZDepthSceneSignature;
	currentHiZHistory.frameId = impl->pendingBackendFrameBreakdown.frameId;
	const bool hiZResourcesReady =
		currentFrameHiZ.width == hiZTargetWidth
		&& currentFrameHiZ.height == hiZTargetHeight
		&& currentFrameHiZ.image != VK_NULL_HANDLE
		&& currentFrameHiZ.fullView != VK_NULL_HANDLE
		&& currentFrameHiZ.sampler != VK_NULL_HANDLE
		&& currentFrameHiZ.mipLevels > 0u
		&& impl->hiZComputePipeline != VK_NULL_HANDLE
		&& impl->hiZPipelineLayout != VK_NULL_HANDLE
		&& impl->hiZDescriptorSetLayout != VK_NULL_HANDLE;
	VulkanHiZValidationInput hiZValidationInput{};
	hiZValidationInput.viewId = viewIndex;
	hiZValidationInput.viewClass = hiZViewClass;
	hiZValidationInput.targetId = hiZTargetId;
	hiZValidationInput.width = hiZTargetWidth;
	hiZValidationInput.height = hiZTargetHeight;
	hiZValidationInput.projectionSignature = hiZProjectionSignature;
	hiZValidationInput.depthSceneSignature = hiZDepthSceneSignature;
	hiZValidationInput.pyramidReady = hiZResourcesReady;
	hiZValidationInput.viewSupported = true;
	hiZValidationInput.cameraCut = false;
	const VulkanHiZValidationResult hiZValidation = ValidateVulkanHiZHistory(readableHiZHistory, hiZValidationInput);
	const bool hiZOcclusionActive = hiZValidation.occlusionAllowed;
	currentHiZHistory.warmupFrames = hiZValidation.warmupFrames;
	currentFrameHiZ.latestInvalidReason = hiZValidation.invalidReason;
	currentFrameHiZ.latestValidation = hiZValidation;
	currentFrameHiZ.latestOcclusionAllowed = hiZOcclusionActive;
	writableFrameHiZ.latestInvalidReason = hiZValidation.invalidReason;
	writableFrameHiZ.latestValidation = hiZValidation;
	writableFrameHiZ.latestOcclusionAllowed = hiZOcclusionActive;
	currentFrameHiZ.history.warmupFrames = hiZValidation.warmupFrames;
	writableFrameHiZ.history.warmupFrames = hiZValidation.warmupFrames;
	if (!hiZValidation.historyValid)
	{
		currentFrameHiZ.history.invalidReason = hiZValidation.invalidReason;
		writableFrameHiZ.history.invalidReason = hiZValidation.invalidReason;
	}

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
		static_cast<float>(hiZTargetWidth),
		static_cast<float>(hiZTargetHeight));
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
	// Runtime GTAO samples an owned depth-history image from the same frame slot
	// after that slot's fence has completed. This keeps AO independent from the
	// live depth attachment that the upcoming render pass will clear and write.
	constexpr bool kEnableRuntimeScreenSpaceAo = true;
	const bool screenSpaceAoActive = kEnableRuntimeScreenSpaceAo && aoDepthHistoryMatchesView;
	const float aoQualityForFrame = screenSpaceAoActive
		? static_cast<float>(frameData.settings.ambientOcclusionQuality)
		: 0.0f;
	analysisCullingData.renderSettings2 = glm::vec4(
		frameData.settings.indirectStrength,
		frameData.settings.ambientOcclusionStrength,
		aoQualityForFrame,
		hiZOcclusionActive ? 1.0f : 0.0f);
	analysisCullingData.globalTaskCullingEnabled = (frameData.globalTaskShaderCullingEnabled && impl->globalTaskShaderCullingEnabled) ? 1u : 0u;
	analysisCullingData.taskShaderSupported = impl->taskShaderSupported ? 1u : 0u;

	FrameCullingDataGPU cullingData = analysisCullingData;
	cullingData.clusterGridAndLightCount.w = static_cast<uint32_t>(std::min<size_t>(lightingPayload.size(), VulkanContext::Impl::MAX_GLOBAL_LIGHTS));

	const uint32_t clusterCount = kLightingClusterDimX * kLightingClusterDimY * kLightingClusterDimZ;
	constexpr uint32_t kClusteredLightingMinimumLightCount = 16u;
	const bool clusteredLightingPipelineReady = impl->lightingComputePipelineLayout != VK_NULL_HANDLE
		&& impl->clusterGridComputePipeline != VK_NULL_HANDLE
		&& impl->clusterDepthMaskComputePipeline != VK_NULL_HANDLE
		&& impl->clusterLightCountComputePipeline != VK_NULL_HANDLE
		&& impl->clusterLightPrefixComputePipeline != VK_NULL_HANDLE
		&& impl->clusterLightFillComputePipeline != VK_NULL_HANDLE;
	// Clustered lighting is an acceleration structure. For tiny scenes the full
	// light loop is cheaper and avoids touching the GPU-written cluster buffers,
	// which keeps optimized/export builds deterministic while the scene has only
	// a few authored lights.
	const bool clusteredLightingWorthBuilding = cullingData.clusterGridAndLightCount.w > kClusteredLightingMinimumLightCount;
	const bool clusteredLightingAvailable = clusteredLightingPipelineReady && clusteredLightingWorthBuilding;
	static bool loggedClusteredLightingUnavailable = false;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!clusteredLightingPipelineReady && cullingData.clusterGridAndLightCount.w > kClusteredLightingMinimumLightCount) {
		// Warn once per outage so validation runs clearly explain why fragment
		// shading fell back to the full-light path instead of using cluster bins.
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!loggedClusteredLightingUnavailable) {
			TERRAENGINE_LOG(LogLevel::Warning, "Variable light bins are unavailable because one or more clustered-light compute pipelines failed to initialize. Falling back to full-light shading for this frame.");
			loggedClusteredLightingUnavailable = true;
		}
	}
	else if (clusteredLightingPipelineReady) {
		loggedClusteredLightingUnavailable = false;
	}
	const bool gpuLightAnalysisAvailable = impl->lightingComputePipelineLayout != VK_NULL_HANDLE
		&& impl->lightAnalysisComputePipeline != VK_NULL_HANDLE;
	cullingData.clusteredLightingValid = clusteredLightingAvailable ? 1u : 0u;
	cullingData.clusteredLightingOverflowed = 0u;
	analysisCullingData.clusteredLightingValid = clusteredLightingAvailable ? 1u : 0u;
	analysisCullingData.clusteredLightingOverflowed = 0u;

	// Publish and transition the pre-scene depth input before any compute pass
	// binds the global descriptor set. Updating the depth descriptor after a
	// global-set bind can invalidate optimized/export command buffers even when
	// the draw command itself has not reached submission yet.
	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.precompute.begin" : "game.precompute.begin");
	const bool needsPreSceneDepthCompute = clusteredLightingAvailable;
	const bool depthPreparedForPreSceneCompute = needsPreSceneDepthCompute
		&& impl->PrepareDepthForCompute(cmd, activeDepthImage, activeDepthView, true);
	if (needsPreSceneDepthCompute && !depthPreparedForPreSceneCompute) {
		// Fail open when the shared depth image cannot be borrowed. The draw
		// pass must not sample AO or cluster metadata that depends on a depth
		// transition which did not happen.
		cullingData.renderSettings2.z = 0.0f;
		cullingData.clusteredLightingValid = 0u;
		impl->UpdateFrameCullingData(statsSlotIndex, cullingData);
	}

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

	if (screenSpaceAoActive) {
		const bool generatedAmbientOcclusion = impl->GenerateAmbientOcclusion(cmd, cullingData, snapshotSlotIndex, aoDepthHistoryView);
		if (!generatedAmbientOcclusion) {
			cullingData.renderSettings2.z = 0.0f;
			impl->UpdateFrameCullingData(statsSlotIndex, cullingData);
			impl->ambientOcclusionHistoryValidByFrame[impl->currentFrame] = false;
		}
	} else {
		impl->ambientOcclusionHistoryValidByFrame[impl->currentFrame] = false;
	}

	// Build the variable-length light list before graphics so clustered shading
	// reads a fresh contiguous range for every cluster.
	if (clusteredLightingAvailable && depthPreparedForPreSceneCompute) {
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
	if (depthPreparedForPreSceneCompute) {
		// Pre-scene AO and cluster building sample the previous frame depth.
		// The upcoming scene render pass clears and writes depth again, so hand
		// the shared depth image back to graphics before vkCmdBeginRenderPass.
		impl->RestoreDepthForAttachment(cmd, activeDepthImage);
	}
	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.precompute.end" : "game.precompute.end");

	// Prepared draws keep resource validation and command recording separate:
	// the expensive lookups happen once, then worker command buffers consume a
	// compact list of already-resolved Vulkan handles.
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
	struct PreparedShadowList {
		TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
		std::vector<PreparedDraw> draws;
	};
	std::vector<PreparedShadowList> preparedShadowLists;
	preparedShadowLists.reserve(frameData.shadowDrawLists.size());

	RenderStatsSnapshot pendingStats{};
	pendingStats.viewType = (view == ViewType::Scene) ? RenderViewType::Scene : RenderViewType::Game;
	pendingStats.renderSize = glm::uvec2(hiZTargetWidth, hiZTargetHeight);
	pendingStats.frameTimeMs = SafeFrameTimeMs(frameData.deltaTimeSeconds);
	pendingStats.fps = pendingStats.frameTimeMs > 0.0f ? (1000.0f / pendingStats.frameTimeMs) : 0.0f;
	pendingStats.valid = true;

	const auto ResolvePipeline = [&](const TerraEngine::UUID& shaderCollectionId, VkRenderPass currentPass, uint32_t materialFlags) -> VulkanPipeline* {
		if (currentPass == VK_NULL_HANDLE) {
			// Material edits can invalidate resources while editor offscreen
			// targets are being rebuilt. A missing render pass means this frame
			// has nothing valid to bind against, so fail closed without asking
			// EnsurePipeline to log the lower-level error repeatedly.
			return nullptr;
		}
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
	std::unordered_map<TerraEngine::UUID, Material*> materialLookupCache;
	materialLookupCache.reserve(frameData.commands.size());
	const auto ResolveMaterial = [&](const TerraEngine::UUID& materialId) -> Material* {
		// Material reloads invalidate backend state between frames. Within one
		// RenderFrame call the reflected command list is immutable, so repeated
		// camera and shadow draws can share the same ResourceManager lookup.
		auto existing = materialLookupCache.find(materialId);
		if (existing != materialLookupCache.end()) {
			return existing->second;
		}
		Material* material = ResourceManager::Instance().GetMaterial(materialId);
		materialLookupCache.emplace(materialId, material);
		return material;
	};
	const auto HasMeshletPayload = [](const VulkanMesh& mesh) {
		// Mesh shaders need the packed vertex data, remap table, decode info,
		// primitive indices, and meshlet records to agree. Treat any missing
		// buffer as "not renderable yet" instead of recording a partial draw.
		return mesh.meshShaderVertexBuffer.buffer != VK_NULL_HANDLE
			&& mesh.meshletVertexRemapBuffer.buffer != VK_NULL_HANDLE
			&& mesh.meshDecodeInfoBuffer.buffer != VK_NULL_HANDLE
			&& mesh.indexBuffer.buffer != VK_NULL_HANDLE
			&& mesh.meshletBuffer.buffer != VK_NULL_HANDLE
			&& mesh.meshletCount != 0u;
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& command : frameData.commands) {
		Material* material = ResolveMaterial(command.materialId);
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

		if (!HasMeshletPayload(mesh)) {
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

		// Let task shaders reject meshlets once the mesh payload has CPU-built
		// bounds. Hi-Z visibility is guarded separately, so this flag only
		// controls the ordinary per-meshlet culling path.
		constexpr bool kEnableGpuMeshletCulling = true;
		const bool activeTaskCulling = kEnableGpuMeshletCulling
			&& pipeline->hasTaskStage
			&& frameData.globalTaskShaderCullingEnabled
			&& impl->globalTaskShaderCullingEnabled
			&& command.taskShaderCullingEnabled
			&& mesh.hasMeshletBounds;

		PushConstants push{};
		push.viewProj = frameData.projectionMatrix * frameData.viewMatrix;
		push.model = command.modelMatrix;
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
		// RenderSystem has already selected the packet/cluster meshlet range.
		// Owner-mesh draws use the baked global meshlet offset. Resident page and
		// fallback draws may use a compact page-local stream, but the original
		// owner offset is still pushed so invalid page descriptors can fail open
		// to the correct owner range instead of restarting at meshlet zero.
		const bool requestedPageLocalGeometry =
			command.virtualGeometryPayloadSource == VirtualGeometryPagePayloadSource::ResidentPagePayload
			|| command.virtualGeometryPayloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
		const bool pageLocalGeometry =
			requestedPageLocalGeometry
			&& impl->activeVirtualGeometryPagePayloadBufferIndex != VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
		const uint32_t ownerMeshletOffset = std::min(command.virtualMeshletOffset, mesh.meshletCount);
		const uint32_t ownerMeshletCount = command.virtualMeshletCount == 0u
			? (mesh.meshletCount - ownerMeshletOffset)
			: std::min(command.virtualMeshletCount, mesh.meshletCount - ownerMeshletOffset);
		const uint32_t selectedMeshletCount = pageLocalGeometry
			? command.virtualMeshletCount
			: ownerMeshletCount;
		// Always push the owner-mesh offset. Task/mesh shaders ignore it when a
		// page-local descriptor validates, but they need the original range when
		// a descriptor is missing or rejected and the draw falls back to the
		// owner mesh payload for correctness.
		push.virtualMeshletOffset = ownerMeshletOffset;
		push.virtualMeshletCount = selectedMeshletCount;
		push.selectedVirtualLod = command.selectedVirtualLod;
		push.virtualGeometryFlags = command.virtualGeometryFlags;
		push.selectedVirtualClusterOffset = command.selectedVirtualClusterOffset;
		push.selectedVirtualClusterCount = command.selectedVirtualClusterCount;
		push.selectedVirtualPageNeedOffset = command.selectedVirtualPageNeedOffset;
		push.selectedVirtualPageNeedCount = command.selectedVirtualPageNeedCount;
		push.virtualGeometryPacketBufferIndex = impl->activeVirtualGeometryPacketBufferIndex;
		push.virtualGeometryBatchBufferIndex = impl->activeVirtualGeometryBatchBufferIndex;
		push.virtualGeometryPagePayloadBufferIndex = impl->activeVirtualGeometryPagePayloadBufferIndex;
		push.virtualGeometryPayloadSource = static_cast<std::uint32_t>(command.virtualGeometryPayloadSource);
		push.virtualGeometryEmittedSource = static_cast<std::uint32_t>(command.virtualGeometryEmittedSource);
		push.virtualGeometryPagePayloadByteOffset = static_cast<std::uint32_t>(command.virtualGeometryPagePayloadByteOffset & 0xffffffffull);
		push.virtualGeometryPagePayloadByteSize = static_cast<std::uint32_t>(command.virtualGeometryPagePayloadByteSize & 0xffffffffull);
		push.virtualGeometryPacketDescriptorIndex = command.virtualGeometryPacketDescriptorIndex;
		push.editorTint = command.editorTint;

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
		
		// RenderSystem has already collapsed compatible contiguous virtual
		// geometry cluster packets into this selected meshlet range. Vulkan
		// submits that prepared packet batch as one indirect task draw instead
		// of exploding back into one command per cluster.
		const uint32_t kTaskBatchSize = 32u;
		const uint32_t taskGroupCount = pipeline->hasTaskStage
			? ((selectedMeshletCount + (kTaskBatchSize - 1u)) / kTaskBatchSize)
			: selectedMeshletCount;

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
					// Shadow visibility is evaluated from the light point of
					// view. Keep the original diagnostic flags intact so shadow
					// recording cannot become another path that latches camera
					// Hi-Z off after an editor view or blocker changes.
					shadowDraw.push.virtualGeometryPacketBufferIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
					shadowDraw.push.virtualGeometryBatchBufferIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
					shadowDraw.push.editorTint = glm::vec4(0.0f);
					
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
		pendingStats.triangleCount += EstimateSelectedPrimitiveCount(
			command.meshId,
			mesh.indexCount / 3u,
			ownerMeshletOffset,
			selectedMeshletCount);
		pendingStats.meshletsTotal += selectedMeshletCount;
		pendingStats.taskCullingActive = pendingStats.taskCullingActive || activeTaskCulling;
		pendingStats.hiZActive = pendingStats.hiZActive || hiZOcclusionActive;
		pendingStats.vrsActive = pendingStats.vrsActive || impl->UsesVrsAttachment();

		// Mesh-only and pass-through fallback paths keep conservative CPU estimates.
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!pipeline->hasTaskStage || !activeTaskCulling) {
			pendingStats.meshletsDispatched += selectedMeshletCount;
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
			Material* material = ResolveMaterial(command.materialId);
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
			const uint32_t selectedShadowMeshletOffset = std::min(command.virtualMeshletOffset, mesh.meshletCount);
			const uint32_t selectedShadowMeshletCount = command.virtualMeshletCount == 0u
				? (mesh.meshletCount - selectedShadowMeshletOffset)
				: std::min(command.virtualMeshletCount, mesh.meshletCount - selectedShadowMeshletOffset);
			shadowDraw.push.virtualMeshletOffset = selectedShadowMeshletOffset;
			shadowDraw.push.virtualMeshletCount = selectedShadowMeshletCount;
			shadowDraw.push.selectedVirtualLod = command.selectedVirtualLod;
			shadowDraw.push.virtualGeometryFlags = command.virtualGeometryFlags;
			shadowDraw.push.selectedVirtualClusterOffset = command.selectedVirtualClusterOffset;
			shadowDraw.push.selectedVirtualClusterCount = command.selectedVirtualClusterCount;
			shadowDraw.push.selectedVirtualPageNeedOffset = command.selectedVirtualPageNeedOffset;
			shadowDraw.push.selectedVirtualPageNeedCount = command.selectedVirtualPageNeedCount;
			shadowDraw.push.virtualGeometryPacketBufferIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
			shadowDraw.push.virtualGeometryBatchBufferIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
			shadowDraw.push.virtualGeometryPacketDescriptorIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
			shadowDraw.push.editorTint = glm::vec4(0.0f);
			shadowDraw.materialFlags = materialFlags;

			const uint32_t kTaskBatchSize = 32u;
			const uint32_t taskGroupCount = shadowPipeline->hasTaskStage
				? ((selectedShadowMeshletCount + (kTaskBatchSize - 1u)) / kTaskBatchSize)
				: selectedShadowMeshletCount;
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
		// The indirect command buffer is ordinary mapped CPU_TO_GPU memory, not a
		// staging upload. Release and exported builds can land this allocation in
		// non-coherent memory, so flush the snapshot range before shadow/main
		// passes consume it through vkCmdDrawMeshTasksIndirectEXT.
		const VkDeviceSize indirectByteOffset = static_cast<VkDeviceSize>(renderSnapshot.indirectDrawOffset) * sizeof(VkDrawMeshTasksIndirectCommandEXT);
		const VkDeviceSize indirectByteSize = static_cast<VkDeviceSize>(indirectCommands.size()) * sizeof(VkDrawMeshTasksIndirectCommandEXT);
		vmaFlushAllocation(impl->allocator, impl->indirectDrawBuffers[impl->currentFrame].allocation, indirectByteOffset, indirectByteSize);
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
		else if (lightType == 1u) {
			slotIndex = std::min(shadowPayloadIndex, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS - 1u);
		}
		else if (lightType == 2u) {
			slotIndex = FindShadowSlotFromTextureIndex(impl->spotShadowTextureIndices, shadowPayloadIndex);
		}
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
			shadowPush.taskCullingEnabled = 0u;
			// Keep Hi-Z diagnostic bits intact. Shadow depth uses its own view
			// projection, but clearing these flags here previously acted like a
			// hidden metadata disable path after editor view changes.
			PushDefaultMeshConstants(cmd, *draw.pipeline, shadowPush);

			VkDeviceSize offset = static_cast<VkDeviceSize>(draw.push.indirectDrawOffset + draw.indirectBufferIndex) * sizeof(VkDrawMeshTasksIndirectCommandEXT);
			vkCmdDrawMeshTasksIndirectEXT(cmd, impl->indirectDrawBuffers[impl->currentFrame].buffer, offset, 1, sizeof(VkDrawMeshTasksIndirectCommandEXT));
			recordedDrawCount += 1u;
		}
		return recordedDrawCount;
	};

	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.shadows.begin" : "game.shadows.begin");
	auto RecordShadowPass = [&](VkFramebuffer framebuffer, VkImage colorImage, VkImage depthImage, uint32_t arrayLayer, uint32_t width, uint32_t height, const ShadowLightRecord& record, const glm::mat4& lightViewProjection) -> bool {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (framebuffer == VK_NULL_HANDLE || colorImage == VK_NULL_HANDLE || depthImage == VK_NULL_HANDLE || impl->shadowRenderPass == VK_NULL_HANDLE) {
			return false;
		}

		// A shadow attachment can be written in one queue submission and reused in
		// another. Use explicit per-image barriers because a render-pass external
		// dependency does not provide that cross-submit memory dependency on every
		// driver. The depth transition also orders any cached shader reads before
		// the next clear and write.
		std::array<VkImageMemoryBarrier, 2> attachmentBarriers{};
		attachmentBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		attachmentBarriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		attachmentBarriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		attachmentBarriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachmentBarriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachmentBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		attachmentBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		attachmentBarriers[0].image = colorImage;
		attachmentBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		attachmentBarriers[0].subresourceRange.baseMipLevel = 0u;
		attachmentBarriers[0].subresourceRange.levelCount = 1u;
		attachmentBarriers[0].subresourceRange.baseArrayLayer = arrayLayer;
		attachmentBarriers[0].subresourceRange.layerCount = 1u;

		attachmentBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		attachmentBarriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		attachmentBarriers[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		attachmentBarriers[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		attachmentBarriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachmentBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		attachmentBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		attachmentBarriers[1].image = depthImage;
		attachmentBarriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		attachmentBarriers[1].subresourceRange.baseMipLevel = 0u;
		attachmentBarriers[1].subresourceRange.levelCount = 1u;
		attachmentBarriers[1].subresourceRange.baseArrayLayer = arrayLayer;
		attachmentBarriers[1].subresourceRange.layerCount = 1u;
		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_DEPENDENCY_BY_REGION_BIT,
			0u, nullptr,
			0u, nullptr,
			static_cast<uint32_t>(attachmentBarriers.size()), attachmentBarriers.data());

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
				recordedAnyShadowPass = RecordShadowPass(impl->directionalShadowFramebuffers[record.slot], impl->directionalShadowColors[record.slot].image, impl->directionalShadowDepths[record.slot].image, 0u, record.regionWidth, record.regionHeight, record, lightViewProjection) || recordedAnyShadowPass;
			}
			else if (record.type == 1u) {
				// Point lights render one atlas face per cubemap direction.
				for (uint32_t faceIndex = 0u; faceIndex < 6u; ++faceIndex) {
					const glm::mat4 lightViewProjection = BuildPointShadowViewProjection(*record.light, faceIndex);
					recordedAnyShadowPass = RecordShadowPass(impl->pointShadowFaceFramebuffersBySlot[record.slot][faceIndex], impl->pointShadowCubeColors[record.slot].image, impl->pointShadowCubeDepths[record.slot].image, faceIndex, pointShadowSize, pointShadowSize, record, lightViewProjection) || recordedAnyShadowPass;
				}
			}
			else if (record.type == 2u) {
				const glm::mat4 lightViewProjection = LoadShadowViewProjection(*record.light);
				recordedAnyShadowPass = RecordShadowPass(impl->spotShadowFramebuffers[record.slot], impl->spotShadowColors[record.slot].image, impl->spotShadowDepths[record.slot].image, 0u, record.regionWidth, record.regionHeight, record, lightViewProjection) || recordedAnyShadowPass;
			}
			else if (record.type == 3u) {
				const glm::mat4 lightViewProjection = LoadShadowViewProjection(*record.light);
				recordedAnyShadowPass = RecordShadowPass(impl->areaShadowFramebuffers[record.slot], impl->areaShadowColors[record.slot].image, impl->areaShadowDepths[record.slot].image, 0u, record.regionWidth, record.regionHeight, record, lightViewProjection) || recordedAnyShadowPass;
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
	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.shadows.end" : "game.shadows.end");
	
	std::vector<VkCommandBuffer> secondaryBuffers;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!prepared.empty()) {
		const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
		const size_t workerCount = prepared.size() < kVulkanParallelRecordingThreshold ? 1u : std::min(prepared.size(), hw);
		const size_t chunkSize = (prepared.size() + workerCount - 1) / workerCount;

		impl->EnsureWorkerPools(impl->currentFrame, workerCount);
		std::vector<std::future<VkCommandBuffer>> futures;
		futures.reserve(workerCount);
		auto recordSecondaryRange = [this, &prepared, &HasMeshletPayload](VkCommandBuffer secondary, VkRenderPass renderPass, VkFramebuffer framebuffer, VkExtent2D renderExtent, size_t start, size_t end) -> VkCommandBuffer {
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
				if (!HasMeshletPayload(*draw.mesh)) {
					continue;
				}

				vkCmdBindPipeline(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->pipeline);
				vkCmdBindDescriptorSets(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->layout, 0, static_cast<uint32_t>(draw.descriptorSets.size()), draw.descriptorSets.data(), 0, nullptr);
				PushDefaultMeshConstants(secondary, *draw.pipeline, draw.push);

				VkDeviceSize offset = static_cast<VkDeviceSize>(draw.push.indirectDrawOffset + draw.indirectBufferIndex) * sizeof(VkDrawMeshTasksIndirectCommandEXT);
				vkCmdDrawMeshTasksIndirectEXT(secondary, impl->indirectDrawBuffers[impl->currentFrame].buffer, offset, 1, sizeof(VkDrawMeshTasksIndirectCommandEXT));
			}
			vkEndCommandBuffer(secondary);
			return secondary;
		};

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
			if (workerCount == 1u) {
				if (VkCommandBuffer recorded = recordSecondaryRange(secondary, renderPass, framebuffer, renderExtent, start, end); recorded != VK_NULL_HANDLE) {
					secondaryBuffers.push_back(recorded);
				}
			}
			else {
				auto task = ThreadPool::instance().createTask([secondary, renderPass, framebuffer, renderExtent, start, end, &recordSecondaryRange]() -> VkCommandBuffer {
					return recordSecondaryRange(secondary, renderPass, framebuffer, renderExtent, start, end);
				});
				futures.push_back(task->getFuture());
				ThreadPool::instance().enqueue(task);
			}
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
			impl->RenderSkyBackground(skyCmd, frameData, renderPassInfo.renderArea.extent);
			vkEndCommandBuffer(skyCmd);
			secondaryBuffers.insert(secondaryBuffers.begin(), skyCmd);
		}
	}

	// Render grid (Scene View only).
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (view == ViewType::Scene && frameData.sceneViewGridVisible) {
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
			impl->RenderSceneViewGrid(gridCmd, frameData, renderPassInfo.renderArea.extent);
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
	if (!impl->editorMode && impl->depthImage != VK_NULL_HANDLE) {
		// Every swapchain framebuffer references the same depth image. Order its
		// previous frame write before this frame clears it, even when a different
		// frame-in-flight fence owns the earlier command buffer.
		VkImageMemoryBarrier depthReuseBarrier{};
		depthReuseBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		depthReuseBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		depthReuseBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		depthReuseBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthReuseBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthReuseBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		depthReuseBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		depthReuseBarrier.image = impl->depthImage;
		depthReuseBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		depthReuseBarrier.subresourceRange.baseMipLevel = 0u;
		depthReuseBarrier.subresourceRange.levelCount = 1u;
		depthReuseBarrier.subresourceRange.baseArrayLayer = 0u;
		depthReuseBarrier.subresourceRange.layerCount = 1u;
		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_DEPENDENCY_BY_REGION_BIT,
			0u, nullptr,
			0u, nullptr,
			1u, &depthReuseBarrier);
	}
	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.mainpass.begin" : "game.mainpass.begin");
	vkCmdBeginRenderPass(cmd, &renderPassInfo, contents);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!secondaryBuffers.empty()) {
		vkCmdExecuteCommands(cmd, static_cast<uint32_t>(secondaryBuffers.size()), secondaryBuffers.data());
	}

	vkCmdEndRenderPass(cmd);
	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.mainpass.end" : "game.mainpass.end");
			
	// VRS Generation for next frame
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->UsesVrsAttachment()) {
		const VkImageView vrsColorView = (outputRequest.kind == RenderOutputKind::OffscreenTexture)
			? impl->captureView.view
			: ((view == ViewType::Scene) ? impl->sceneView.view : impl->gameView.view);
		impl->GenerateVRSImage(cmd, vrsColorView);
	}
	if (kEnableRuntimeScreenSpaceAo
		&& !impl->editorMode
		&& firstPrimaryPassForFrame
		&& frameData.settings.ambientOcclusionStrength > 0.001f
		&& frameData.settings.ambientOcclusionQuality > 0u) {
		impl->StoreAmbientOcclusionDepthHistory(cmd, activeDepthImage, viewIndex, currentViewProjection);
	}
	// Generate the pyramid after scene depth is complete. Its task-shader
	// consumer has a fixed four-load proof budget, so enabling Hi-Z cannot create
	// the unbounded startup workload that previously triggered driver watchdogs.
	constexpr bool kEnableHiZMipGeneration = true;
	if (kEnableHiZMipGeneration) {
		impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.hiz.begin" : "game.hiz.begin");
		// Build Hi-Z after the depth attachment contains the final scene depth.
		// The next frame can then use this stable pyramid for conservative occlusion.
		if (impl->GenerateHiZPyramid(writableFrameHiZ, cmd, activeDepthImage, activeDepthView, activeRenderExtent, snapshotSlotIndex, currentHiZHistory)) {
			impl->MarkHiZWritePending(hiZViewIndex, impl->currentFrame);
		}
		impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.hiz.end" : "game.hiz.end");
	}
	impl->MarkGpuTimestampEnd(cmd, impl->currentFrame, view);
	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.primary.end" : "game.primary.end");

	vkEndCommandBuffer(cmd);
	const float commandRecordingMs = ElapsedMilliseconds(commandRecordingStart);
	TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
		impl->pendingBackendFrameBreakdown,
		{ TerraEngine::Renderer::BackendFrameCostCategory::PrimaryCommandRecording, TerraEngine::Renderer::BackendSubmitStage::CommandRecording, renderViewType, diagnosticViewId, "Primary command recording", false, true },
		commandRecordingMs);
	TerraEngine::Renderer::Vulkan::RecordVulkanFrameCost(
		impl->pendingBackendFrameBreakdown,
		{ TerraEngine::Renderer::BackendFrameCostCategory::ViewSubmission, TerraEngine::Renderer::BackendSubmitStage::ViewSubmit, renderViewType, diagnosticViewId, "View Submission", false, true },
		ElapsedMilliseconds(renderFrameStart));
	std::uint64_t passSetHash = 1469598103934665603ull;
	HashCombine(passSetHash, static_cast<std::uint64_t>(frameData.commands.size()));
	HashCombine(passSetHash, static_cast<std::uint64_t>(frameData.shadowDrawLists.size()));
	HashCombine(passSetHash, static_cast<std::uint64_t>(frameData.uiBatch.staticQuads.size()));
	HashCombine(passSetHash, static_cast<std::uint64_t>(frameData.uiBatch.textRuns.size()));

	std::uint64_t cameraHash = 1469598103934665603ull;
	HashMat4(cameraHash, frameData.viewMatrix);
	HashMat4(cameraHash, frameData.projectionMatrix);
	HashVec4(cameraHash, glm::vec4(frameData.cameraWorldPosition, 1.0f));

	std::uint64_t targetHash = 1469598103934665603ull;
	HashCombine(targetHash, static_cast<std::uint64_t>(outputRequest.kind));
	HashCombine(targetHash, static_cast<std::uint64_t>(renderViewType));
	HashCombine(targetHash, static_cast<std::uint64_t>(outputRequest.outputId));
	HashCombine(targetHash, static_cast<std::uint64_t>(outputRequest.targetSize.x));
	HashCombine(targetHash, static_cast<std::uint64_t>(outputRequest.targetSize.y));

	std::uint64_t overlayHash = 1469598103934665603ull;
	HashVec4(overlayHash, frameData.editorContextTint);
	std::string sharedWorkKey = std::to_string(passSetHash) + ":"
		+ std::to_string(cameraHash) + ":"
		+ std::to_string(targetHash);
	TerraEngine::Renderer::ViewSubmitWorkItem submitWork = TerraEngine::Renderer::Vulkan::BuildVulkanViewSubmitWorkItem(
		impl->pendingBackendFrameBreakdown.frameId,
		{ TerraEngine::Renderer::BackendFrameCostCategory::ViewSubmission, TerraEngine::Renderer::BackendSubmitStage::ViewSubmit, renderViewType, diagnosticViewId, "View submit work", false, true },
		passSetHash,
		overlayHash,
		commandRecordingMs,
		0.0f,
		std::move(sharedWorkKey));
	submitWork.cameraId = cameraHash;
	submitWork.targetId = targetHash;
	if (!impl->pendingBackendFrameBreakdown.viewSubmitWork.empty())
	{
		std::string reuseReason;
		submitWork.reuseDecision = TerraEngine::Renderer::CompareViewSubmitWork(impl->pendingBackendFrameBreakdown.viewSubmitWork.front(), submitWork, &reuseReason);
		submitWork.reuseReason = reuseReason;
	}
	else
	{
		submitWork.reuseDecision = TerraEngine::Renderer::ViewSubmitReuseDecision::Separate;
		submitWork.reuseReason = "First submitted view owns the initial backend work for this frame.";
	}
	TerraEngine::Renderer::AddViewSubmitWorkItem(impl->pendingBackendFrameBreakdown, std::move(submitWork));
	// Record the command buffer to be submitted at EndFrame so multiple cameras
	// render sequentially before a single present.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		impl->pendingPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
	}
	if (impl->currentFrame >= impl->pendingPrimaryCommandBuffersSubmitted.size()) {
		impl->pendingPrimaryCommandBuffersSubmitted.resize(impl->MAX_FRAMES_IN_FLIGHT, 0u);
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

// VulkanContext::Impl::PrepareDepthForCompute is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::PrepareDepthForCompute(VkCommandBuffer cmd, VkImage sourceDepthImage, VkImageView depthView, bool updateGlobalDepthDescriptor) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	if (cmd == VK_NULL_HANDLE
		|| sourceDepthImage == VK_NULL_HANDLE
		|| currentFrame >= globalDescriptorSets.size()
		|| depthView == VK_NULL_HANDLE) {
		return false;
	}

	// Depth-dependent compute passes sample the depth image through a descriptor
	// after graphics has populated it. Vulkan may perform depth writes during
	// either early or late fragment tests, so the barrier must wait on both
	// stages; waiting only on late tests lets Hi-Z read the clear value on
	// drivers that keep simple opaque depth writes in the early path.
	VkImageMemoryBarrier depthBarrier{};
	depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	depthBarrier.image = sourceDepthImage;
	depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	depthBarrier.subresourceRange.baseMipLevel = 0u;
	depthBarrier.subresourceRange.levelCount = 1u;
	depthBarrier.subresourceRange.baseArrayLayer = 0u;
	depthBarrier.subresourceRange.layerCount = 1u;
	depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(
		cmd,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &depthBarrier);

	if (!updateGlobalDepthDescriptor) {
		return true;
	}
	if (globalDescriptorSets[currentFrame] == VK_NULL_HANDLE) {
		return false;
	}

	// Binding 12 lives in the persistent global descriptor set. Even though the
	// binding is update-after-bind, exported builds can still have a submitted
	// runtime command buffer tracked for this frame slot, so drain that work
	// before changing which depth view clustered lighting samples.
	WaitForDescriptorMutationIdle();

	VkDescriptorImageInfo sampledDepthInfo{};
	sampledDepthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	sampledDepthInfo.imageView = depthView;
	sampledDepthInfo.sampler = VK_NULL_HANDLE;

	VkWriteDescriptorSet depthWrite{};
	depthWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	depthWrite.dstSet = globalDescriptorSets[currentFrame];
	depthWrite.dstBinding = 12u;
	depthWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	depthWrite.descriptorCount = 1u;
	depthWrite.pImageInfo = &sampledDepthInfo;
	vkUpdateDescriptorSets(device, 1u, &depthWrite, 0u, nullptr);
	return true;
}

// VulkanContext::Impl::RestoreDepthForAttachment is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RestoreDepthForAttachment(VkCommandBuffer cmd, VkImage sourceDepthImage) {
	if (cmd == VK_NULL_HANDLE || sourceDepthImage == VK_NULL_HANDLE) {
		return;
	}

	// Pre-scene compute passes borrow the previous frame depth as a sampled
	// image. The main scene render pass immediately after them owns the same
	// image as a writable depth attachment, so restore the layout explicitly
	// before Vulkan's render-pass load/clear operation sees the attachment.
	VkImageMemoryBarrier depthBarrier{};
	depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	depthBarrier.image = sourceDepthImage;
	depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	depthBarrier.subresourceRange.baseMipLevel = 0u;
	depthBarrier.subresourceRange.levelCount = 1u;
	depthBarrier.subresourceRange.baseArrayLayer = 0u;
	depthBarrier.subresourceRange.layerCount = 1u;
	depthBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	depthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	vkCmdPipelineBarrier(
		cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &depthBarrier);
}

// VulkanContext::Impl::StoreAmbientOcclusionDepthHistory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::StoreAmbientOcclusionDepthHistory(VkCommandBuffer cmd, VkImage sourceDepthImage, uint32_t viewIndex, const glm::mat4& viewProjection) {
	if (cmd == VK_NULL_HANDLE
		|| sourceDepthImage == VK_NULL_HANDLE
		|| viewIndex >= depthHistoryByView.size()
		|| currentFrame >= MAX_FRAMES_IN_FLIGHT
		|| ambientOcclusionWidth == 0u
		|| ambientOcclusionHeight == 0u) {
		return;
	}

	ViewDepthHistoryState& viewHistory = depthHistoryByView[viewIndex];
	VulkanImage& historyDepth = viewHistory.ambientOcclusionDepthByFrame[currentFrame];
	if (historyDepth.image == VK_NULL_HANDLE || historyDepth.view == VK_NULL_HANDLE) {
		viewHistory.ambientOcclusionDepthValidByFrame[currentFrame] = false;
		return;
	}

	auto TransitionDepthImage = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		barrier.subresourceRange.baseMipLevel = 0u;
		barrier.subresourceRange.levelCount = 1u;
		barrier.subresourceRange.baseArrayLayer = 0u;
		barrier.subresourceRange.layerCount = 1u;
		barrier.srcAccessMask = srcAccess;
		barrier.dstAccessMask = dstAccess;
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	};

	// The scene render pass writes the active view depth attachment. Copy that
	// completed depth into an AO-owned history image so next frame's GTAO pass
	// never samples an attachment that is being cleared or rewritten.
	TransitionDepthImage(
		sourceDepthImage,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_TRANSFER_READ_BIT,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT);
	TransitionDepthImage(
		historyDepth.image,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_ACCESS_SHADER_READ_BIT,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageCopy copyRegion{};
	copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	copyRegion.srcSubresource.mipLevel = 0u;
	copyRegion.srcSubresource.baseArrayLayer = 0u;
	copyRegion.srcSubresource.layerCount = 1u;
	copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	copyRegion.dstSubresource.mipLevel = 0u;
	copyRegion.dstSubresource.baseArrayLayer = 0u;
	copyRegion.dstSubresource.layerCount = 1u;
	copyRegion.extent = {
		std::max(ambientOcclusionWidth, 1u),
		std::max(ambientOcclusionHeight, 1u),
		1u
	};
	vkCmdCopyImage(
		cmd,
		sourceDepthImage,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		historyDepth.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1u,
		&copyRegion);

	TransitionDepthImage(
		historyDepth.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	TransitionDepthImage(
		sourceDepthImage,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_ACCESS_TRANSFER_READ_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);

	viewHistory.ambientOcclusionDepthViewProjectionByFrame[currentFrame] = viewProjection;
	viewHistory.ambientOcclusionDepthValidByFrame[currentFrame] = true;
}

// VulkanContext::Impl::GenerateHiZPyramid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::GenerateHiZPyramid(VulkanHiZ& hiZ, VkCommandBuffer cmd, VkImage sourceDepthImage, VkImageView depthView, VkExtent2D sourceDepthExtent, uint32_t snapshotSlotIndex, const VulkanHiZHistoryMetadata& completedHistory) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	// Viewport dimensions can change between RenderFrame discovering the desired
	// size and BeginFrame reaching a fence-safe resource recreation boundary.
	// Never dispatch the old pyramid over the new depth image: Texture2D.Load is
	// not guaranteed to make out-of-range coordinates harmless on every driver.
	// Skipping this one generation keeps all geometry visible, and BeginFrame
	// recreates both ping-pong images at the requested extent before retrying.
	if (sourceDepthExtent.width == 0u
		|| sourceDepthExtent.height == 0u
		|| hiZ.width != sourceDepthExtent.width
		|| hiZ.height != sourceDepthExtent.height) {
		hiZ.latestInvalidReason = VulkanHiZInvalidReason::Resize;
		hiZ.latestOcclusionAllowed = false;
		hiZ.latestValidation = {};
		hiZ.latestValidation.width = sourceDepthExtent.width;
		hiZ.latestValidation.height = sourceDepthExtent.height;
		hiZ.latestValidation.pyramidReady = false;
		hiZ.latestValidation.failOpen = true;
		hiZ.latestValidation.invalidReason = VulkanHiZInvalidReason::Resize;
		return false;
	}

	if (cmd == VK_NULL_HANDLE
		|| sourceDepthImage == VK_NULL_HANDLE
		|| depthView == VK_NULL_HANDLE
		|| hiZ.image == VK_NULL_HANDLE
		|| hiZ.fullView == VK_NULL_HANDLE
		|| hiZ.mipLevels == 0u
		|| hiZ.width == 0u
		|| hiZ.height == 0u
		|| hiZ.mipViews.size() < hiZ.mipLevels
		|| hiZComputePipeline == VK_NULL_HANDLE
		|| hiZPipelineLayout == VK_NULL_HANDLE
		|| hiZDescriptorSetLayout == VK_NULL_HANDLE) {
		hiZ.latestInvalidReason = VulkanHiZInvalidReason::MissingPyramid;
		hiZ.latestOcclusionAllowed = false;
		hiZ.latestValidation = {};
		hiZ.latestValidation.width = hiZ.width;
		hiZ.latestValidation.height = hiZ.height;
		hiZ.latestValidation.pyramidReady = false;
		hiZ.latestValidation.invalidReason = VulkanHiZInvalidReason::MissingPyramid;
		return false;
	}

	for (uint32_t mipIndex = 0u; mipIndex < hiZ.mipLevels; ++mipIndex) {
		// Every dispatch binds a descriptor set that points at one concrete mip.
		// Missing views mean swapchain recreation is incomplete, so skip safely.
		if (hiZ.mipViews[mipIndex] == VK_NULL_HANDLE) {
			hiZ.latestInvalidReason = VulkanHiZInvalidReason::MissingPyramid;
			hiZ.latestOcclusionAllowed = false;
			hiZ.latestValidation = {};
			hiZ.latestValidation.width = hiZ.width;
			hiZ.latestValidation.height = hiZ.height;
			hiZ.latestValidation.pyramidReady = false;
			hiZ.latestValidation.invalidReason = VulkanHiZInvalidReason::MissingPyramid;
			return false;
		}
	}

	const uint32_t clampedSnapshotSlot = std::min(snapshotSlotIndex, MAX_RENDER_SNAPSHOTS_PER_FRAME - 1u);
	if (hiZ.mipLevels > MAX_HIZ_DESCRIPTOR_MIP_LEVELS) {
		TERRAENGINE_LOG(LogLevel::Error, "Skipping Hi-Z generation because the pyramid exceeds descriptor capacity.");
		return false;
	}
	// Allocate a viewport-independent descriptor range. Scene and Game views may
	// have different mip counts while their command buffers wait for one batched
	// submit, so sizing this pool from the active view would destroy descriptor
	// sets already bound by the earlier view.
	const std::size_t descriptorSetCount =
		static_cast<std::size_t>(MAX_FRAMES_IN_FLIGHT)
		* static_cast<std::size_t>(MAX_RENDER_SNAPSHOTS_PER_FRAME)
		* static_cast<std::size_t>(MAX_HIZ_DESCRIPTOR_MIP_LEVELS);
	if (hiZDescriptorPool == VK_NULL_HANDLE || hiZDescriptorSets.size() != descriptorSetCount) {
		if (hiZDescriptorPool != VK_NULL_HANDLE) {
			// This path is normally hit only after a resize. Wait before
			// destroying the old per-frame sets so no pending frame can still
			// reference them.
			const VkResult idleResult = WaitDeviceIdle();
			if (MarkDeviceLost(idleResult, "Hi-Z descriptor pool resize device idle")) {
				return false;
			}
			if (idleResult != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "Skipping Hi-Z descriptor pool resize because the device did not become idle: " << idleResult);
				return false;
			}
			vkDestroyDescriptorPool(device, hiZDescriptorPool, nullptr);
			hiZDescriptorPool = VK_NULL_HANDLE;
			hiZDescriptorSets.clear();
		}

		std::array<VkDescriptorPoolSize, 2> poolSizes{};
		poolSizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		poolSizes[0].descriptorCount = static_cast<uint32_t>(descriptorSetCount);
		poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		poolSizes[1].descriptorCount = static_cast<uint32_t>(descriptorSetCount);

		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.maxSets = static_cast<uint32_t>(descriptorSetCount);
		poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
		poolInfo.pPoolSizes = poolSizes.data();
		if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &hiZDescriptorPool) != VK_SUCCESS) {
			hiZDescriptorPool = VK_NULL_HANDLE;
			hiZDescriptorSets.clear();
			return false;
		}

		std::vector<VkDescriptorSetLayout> layouts(descriptorSetCount, hiZDescriptorSetLayout);
		hiZDescriptorSets.resize(descriptorSetCount, VK_NULL_HANDLE);
		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = hiZDescriptorPool;
		allocInfo.descriptorSetCount = static_cast<uint32_t>(hiZDescriptorSets.size());
		allocInfo.pSetLayouts = layouts.data();
		if (vkAllocateDescriptorSets(device, &allocInfo, hiZDescriptorSets.data()) != VK_SUCCESS) {
			vkDestroyDescriptorPool(device, hiZDescriptorPool, nullptr);
			hiZDescriptorPool = VK_NULL_HANDLE;
			hiZDescriptorSets.clear();
			return false;
		}
	}

	// Hi-Z can be recorded by multiple render snapshots in one frame, such as
	// scene view and game view. Each snapshot owns a descriptor range so a later
	// view never rewrites descriptors already bound into an earlier command
	// buffer that has not reached vkQueueSubmit yet.
	const std::size_t frameDescriptorBase =
		(static_cast<std::size_t>(currentFrame) * static_cast<std::size_t>(MAX_RENDER_SNAPSHOTS_PER_FRAME)
			+ static_cast<std::size_t>(clampedSnapshotSlot))
		* static_cast<std::size_t>(MAX_HIZ_DESCRIPTOR_MIP_LEVELS);
	if (frameDescriptorBase + hiZ.mipLevels > hiZDescriptorSets.size()) {
		return false;
	}

	// Touch the active view depth image only after every Hi-Z resource is known
	// to be valid. Any early return before this point leaves depth in the
	// render-pass layout, while a complete Hi-Z record restores it before the
	// next frame.
	if (!PrepareDepthForCompute(cmd, sourceDepthImage, depthView, false)) {
		return false;
	}

	auto TransitionHiZMip = [&](uint32_t mipIndex,
		VkImageLayout oldLayout,
		VkImageLayout newLayout,
		VkAccessFlags srcAccess,
		VkAccessFlags dstAccess,
		VkPipelineStageFlags srcStage,
		VkPipelineStageFlags dstStage) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = hiZ.image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = mipIndex;
		barrier.subresourceRange.levelCount = 1u;
		barrier.subresourceRange.baseArrayLayer = 0u;
		barrier.subresourceRange.layerCount = 1u;
		barrier.srcAccessMask = srcAccess;
		barrier.dstAccessMask = dstAccess;
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	};

	auto WriteHiZDescriptorSet = [&](uint32_t mipIndex,
		VkImageView inputView,
		VkImageLayout inputLayout,
		VkImageView outputView) -> VkDescriptorSet {
		VkDescriptorSet descriptorSet = hiZDescriptorSets[frameDescriptorBase + mipIndex];

		VkDescriptorImageInfo inputInfo{};
		inputInfo.imageLayout = inputLayout;
		inputInfo.imageView = inputView;
		inputInfo.sampler = VK_NULL_HANDLE;

		VkDescriptorImageInfo outputInfo{};
		outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		outputInfo.imageView = outputView;
		outputInfo.sampler = VK_NULL_HANDLE;

		std::array<VkWriteDescriptorSet, 2> writes{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = descriptorSet;
		writes[0].dstBinding = 0u;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[0].descriptorCount = 1u;
		writes[0].pImageInfo = &inputInfo;
		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = descriptorSet;
		writes[1].dstBinding = 1u;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[1].descriptorCount = 1u;
		writes[1].pImageInfo = &outputInfo;
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0u, nullptr);
		return descriptorSet;
	};

	// struct HiZPushConstants mirrors HiZGenerate.comp.hlsl: source dimensions
	// are used for reads, and output dimensions reject inactive workgroup lanes.
	struct HiZPushConstants
	{
		uint32_t inputWidth = 1u;
		uint32_t inputHeight = 1u;
		uint32_t outputWidth = 1u;
		uint32_t outputHeight = 1u;
		uint32_t occlusionState = 0u;
		uint32_t invalidReason = static_cast<uint32_t>(VulkanHiZInvalidReason::FirstFrame);
		uint32_t reserved0 = 0u;
		uint32_t reserved1 = 0u;
	};

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiZComputePipeline);

	// First use initializes the allocation inside the normal frame submission.
	// Later writes preserve the submitted shader-read layout. A second snapshot
	// recorded into the same queue batch also observes the first recorded write.
	const bool hiZHasPriorRecordedLayout = hiZ.layoutInitialized || hiZ.layoutTransitionRecorded;
	const VkImageLayout hiZOldLayout = hiZHasPriorRecordedLayout
		? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		: VK_IMAGE_LAYOUT_UNDEFINED;
	const VkAccessFlags hiZSourceAccess = hiZHasPriorRecordedLayout ? VK_ACCESS_SHADER_READ_BIT : 0u;
	const VkPipelineStageFlags hiZSourceStage = hiZHasPriorRecordedLayout
		? (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
		: VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

	TransitionHiZMip(
		0u,
		hiZOldLayout,
		VK_IMAGE_LAYOUT_GENERAL,
		hiZSourceAccess,
		VK_ACCESS_SHADER_WRITE_BIT,
		hiZSourceStage,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	VkDescriptorSet levelZeroSet = WriteHiZDescriptorSet(0u, depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, hiZ.mipViews[0]);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiZPipelineLayout, 0u, 1u, &levelZeroSet, 0u, nullptr);
	HiZPushConstants levelZeroPush{};
	levelZeroPush.inputWidth = hiZ.width;
	levelZeroPush.inputHeight = hiZ.height;
	levelZeroPush.outputWidth = hiZ.width;
	levelZeroPush.outputHeight = hiZ.height;
	levelZeroPush.occlusionState = hiZ.latestOcclusionAllowed ? 1u : 0u;
	levelZeroPush.invalidReason = static_cast<uint32_t>(hiZ.latestInvalidReason);
	vkCmdPushConstants(cmd, hiZPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(levelZeroPush), &levelZeroPush);
	vkCmdDispatch(cmd, (hiZ.width + 7u) / 8u, (hiZ.height + 7u) / 8u, 1u);

	TransitionHiZMip(
		0u,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	for (uint32_t mipIndex = 1u; mipIndex < hiZ.mipLevels; ++mipIndex) {
		const uint32_t sourceWidth = std::max(1u, hiZ.width >> (mipIndex - 1u));
		const uint32_t sourceHeight = std::max(1u, hiZ.height >> (mipIndex - 1u));
		const uint32_t outputWidth = std::max(1u, hiZ.width >> mipIndex);
		const uint32_t outputHeight = std::max(1u, hiZ.height >> mipIndex);

		TransitionHiZMip(
			mipIndex,
			hiZOldLayout,
			VK_IMAGE_LAYOUT_GENERAL,
			hiZSourceAccess,
			VK_ACCESS_SHADER_WRITE_BIT,
			hiZSourceStage,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

		VkDescriptorSet mipSet = WriteHiZDescriptorSet(
			mipIndex,
			hiZ.mipViews[mipIndex - 1u],
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			hiZ.mipViews[mipIndex]);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiZPipelineLayout, 0u, 1u, &mipSet, 0u, nullptr);

		HiZPushConstants mipPush{};
		mipPush.inputWidth = sourceWidth;
		mipPush.inputHeight = sourceHeight;
		mipPush.outputWidth = outputWidth;
		mipPush.outputHeight = outputHeight;
		mipPush.occlusionState = hiZ.latestOcclusionAllowed ? 1u : 0u;
		mipPush.invalidReason = static_cast<uint32_t>(hiZ.latestInvalidReason);
		vkCmdPushConstants(cmd, hiZPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(mipPush), &mipPush);
		vkCmdDispatch(cmd, (outputWidth + 7u) / 8u, (outputHeight + 7u) / 8u, 1u);

		TransitionHiZMip(
			mipIndex,
			VK_IMAGE_LAYOUT_GENERAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_SHADER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

	// Restore the active view depth image to the layout expected at the start
	// of the next frame. Keeping this invariant avoids layout mismatches while
	// the independent Hi-Z pyramid remains readable.
	VkImageMemoryBarrier restoreDepthBarrier{};
	restoreDepthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	restoreDepthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	restoreDepthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	restoreDepthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	restoreDepthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	restoreDepthBarrier.image = sourceDepthImage;
	restoreDepthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	restoreDepthBarrier.subresourceRange.baseMipLevel = 0u;
	restoreDepthBarrier.subresourceRange.levelCount = 1u;
	restoreDepthBarrier.subresourceRange.baseArrayLayer = 0u;
	restoreDepthBarrier.subresourceRange.layerCount = 1u;
	restoreDepthBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	restoreDepthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	vkCmdPipelineBarrier(
		cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &restoreDepthBarrier);

	hiZ.history = completedHistory;
	hiZ.history.valid = true;
	hiZ.history.warmupFrames = std::max(hiZ.history.warmupFrames, 1u);
	hiZ.history.invalidReason = VulkanHiZInvalidReason::None;
	// This is only a recording promise. EndFrame promotes it to submitted state;
	// resize cancellation clears it so the next command uses UNDEFINED again.
	hiZ.layoutTransitionRecorded = true;
	return true;
}

// VulkanContext::Impl::GenerateAmbientOcclusion is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::GenerateAmbientOcclusion(VkCommandBuffer cmd, const FrameCullingDataGPU& frameData, uint32_t snapshotSlotIndex, VkImageView sampledDepthView) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	const bool aoEnabled = frameData.renderSettings2.y > 0.001f && frameData.renderSettings2.z > 0.5f;
	if (!aoEnabled
		|| cmd == VK_NULL_HANDLE
		|| sampledDepthView == VK_NULL_HANDLE
		|| aoPipelineLayout == VK_NULL_HANDLE
		|| aoDescriptorSetLayout == VK_NULL_HANDLE
		|| currentFrame >= globalDescriptorSets.size()
		|| globalDescriptorSets[currentFrame] == VK_NULL_HANDLE
		|| gtaoComputePipeline == VK_NULL_HANDLE
		|| gtaoBlurComputePipeline == VK_NULL_HANDLE
		|| currentFrame >= ambientOcclusionRawByFrame.size()
		|| ambientOcclusionRawByFrame[currentFrame].image == VK_NULL_HANDLE
		|| ambientOcclusionBlurredByFrame[currentFrame].image == VK_NULL_HANDLE
		|| ambientOcclusionHistoryByFrame[currentFrame].image == VK_NULL_HANDLE
		|| ambientOcclusionRawByFrame[currentFrame].view == VK_NULL_HANDLE
		|| ambientOcclusionBlurredByFrame[currentFrame].view == VK_NULL_HANDLE
		|| ambientOcclusionHistoryByFrame[currentFrame].view == VK_NULL_HANDLE) {
		return false;
	}

	VulkanImage& ambientOcclusionRaw = ambientOcclusionRawByFrame[currentFrame];
	VulkanImage& ambientOcclusionBlurred = ambientOcclusionBlurredByFrame[currentFrame];
	VulkanImage& ambientOcclusionHistory = ambientOcclusionHistoryByFrame[currentFrame];
	bool& ambientOcclusionHistoryValid = ambientOcclusionHistoryValidByFrame[currentFrame];
	const uint32_t width = std::max(ambientOcclusionWidth, 1u);
	const uint32_t height = std::max(ambientOcclusionHeight, 1u);
	const uint32_t clampedSnapshotSlot = std::min(snapshotSlotIndex, MAX_RENDER_SNAPSHOTS_PER_FRAME - 1u);
	// GTAO and blur run back-to-back inside one command buffer. Give each pass
	// its own descriptor set so the blur descriptor update never mutates the
	// GTAO set after that set has already been bound for dispatch.
	constexpr uint32_t kAoDescriptorSetsPerSnapshot = 2u;
	const std::size_t descriptorSetCount =
		static_cast<std::size_t>(MAX_FRAMES_IN_FLIGHT)
		* static_cast<std::size_t>(MAX_RENDER_SNAPSHOTS_PER_FRAME)
		* static_cast<std::size_t>(kAoDescriptorSetsPerSnapshot);
	if (aoDescriptorPool == VK_NULL_HANDLE || aoDescriptorSets.size() != descriptorSetCount) {
		if (aoDescriptorPool != VK_NULL_HANDLE) {
			// AO descriptors are recorded into primary command buffers. Wait
			// before recreating the pool during resize so no pending frame can
			// still reference sets from the old pool.
			const VkResult idleResult = WaitDeviceIdle();
			if (MarkDeviceLost(idleResult, "ambient occlusion descriptor pool resize device idle")) {
				return false;
			}
			if (idleResult != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "Skipping ambient occlusion descriptor pool resize because the device did not become idle: " << idleResult);
				return false;
			}
			vkDestroyDescriptorPool(device, aoDescriptorPool, nullptr);
			aoDescriptorPool = VK_NULL_HANDLE;
			aoDescriptorSets.clear();
		}

		std::array<VkDescriptorPoolSize, 2> poolSizes{};
		poolSizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		poolSizes[0].descriptorCount = static_cast<uint32_t>(descriptorSetCount * 3u);
		poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		poolSizes[1].descriptorCount = static_cast<uint32_t>(descriptorSetCount * 2u);

		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.maxSets = static_cast<uint32_t>(descriptorSetCount);
		poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
		poolInfo.pPoolSizes = poolSizes.data();
		if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &aoDescriptorPool) != VK_SUCCESS) {
			aoDescriptorPool = VK_NULL_HANDLE;
			aoDescriptorSets.clear();
			return false;
		}

		std::vector<VkDescriptorSetLayout> layouts(descriptorSetCount, aoDescriptorSetLayout);
		aoDescriptorSets.resize(descriptorSetCount, VK_NULL_HANDLE);
		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = aoDescriptorPool;
		allocInfo.descriptorSetCount = static_cast<uint32_t>(aoDescriptorSets.size());
		allocInfo.pSetLayouts = layouts.data();
		if (vkAllocateDescriptorSets(device, &allocInfo, aoDescriptorSets.data()) != VK_SUCCESS) {
			vkDestroyDescriptorPool(device, aoDescriptorPool, nullptr);
			aoDescriptorPool = VK_NULL_HANDLE;
			aoDescriptorSets.clear();
			return false;
		}
	}

	const std::size_t descriptorIndex =
		(static_cast<std::size_t>(currentFrame) * static_cast<std::size_t>(MAX_RENDER_SNAPSHOTS_PER_FRAME)
			+ static_cast<std::size_t>(clampedSnapshotSlot))
		* static_cast<std::size_t>(kAoDescriptorSetsPerSnapshot);
	if (descriptorIndex + 1u >= aoDescriptorSets.size()
		|| aoDescriptorSets[descriptorIndex] == VK_NULL_HANDLE
		|| aoDescriptorSets[descriptorIndex + 1u] == VK_NULL_HANDLE) {
		return false;
	}
	VkDescriptorSet gtaoDescriptorSet = aoDescriptorSets[descriptorIndex];
	VkDescriptorSet blurDescriptorSet = aoDescriptorSets[descriptorIndex + 1u];

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
	depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	depthInfo.imageView = sampledDepthView;
	depthInfo.sampler = VK_NULL_HANDLE;

	VkDescriptorImageInfo rawOutputInfo{};
	rawOutputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	rawOutputInfo.imageView = ambientOcclusionRaw.view;
	rawOutputInfo.sampler = VK_NULL_HANDLE;

	std::array<VkWriteDescriptorSet, 2> gtaoWrites{};
	gtaoWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	gtaoWrites[0].dstSet = gtaoDescriptorSet;
	gtaoWrites[0].dstBinding = 12;
	gtaoWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	gtaoWrites[0].descriptorCount = 1;
	gtaoWrites[0].pImageInfo = &depthInfo;

	gtaoWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	gtaoWrites[1].dstSet = gtaoDescriptorSet;
	gtaoWrites[1].dstBinding = 23;
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

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoPipelineLayout, 0, 1, &gtaoDescriptorSet, 0, nullptr);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoComputePipeline);
	vkCmdPushConstants(cmd, aoPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(gtaoPush), &gtaoPush);
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
		ambientOcclusionBlurred.image,
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
	blurredOutputInfo.imageView = ambientOcclusionBlurred.view;
	blurredOutputInfo.sampler = VK_NULL_HANDLE;

	VkDescriptorImageInfo historyInputInfo{};
	historyInputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	historyInputInfo.imageView = ambientOcclusionHistory.view;
	historyInputInfo.sampler = VK_NULL_HANDLE;

	// The blur shader also samples depth for bilateral edge preservation. The
	// descriptor set is separate from GTAO, so it needs its own binding 12 write
	// even though both passes sample the same depth-history image.
	std::array<VkWriteDescriptorSet, 4> blurWrites{};
	blurWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	blurWrites[0].dstSet = blurDescriptorSet;
	blurWrites[0].dstBinding = 12;
	blurWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	blurWrites[0].descriptorCount = 1;
	blurWrites[0].pImageInfo = &depthInfo;

	blurWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	blurWrites[1].dstSet = blurDescriptorSet;
	blurWrites[1].dstBinding = 22;
	blurWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	blurWrites[1].descriptorCount = 1;
	blurWrites[1].pImageInfo = &rawInputInfo;

	blurWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	blurWrites[2].dstSet = blurDescriptorSet;
	blurWrites[2].dstBinding = 24;
	blurWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	blurWrites[2].descriptorCount = 1;
	blurWrites[2].pImageInfo = &blurredOutputInfo;

	blurWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	blurWrites[3].dstSet = blurDescriptorSet;
	blurWrites[3].dstBinding = 21;
	blurWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	blurWrites[3].descriptorCount = 1;
	blurWrites[3].pImageInfo = &historyInputInfo;
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
	// Keep temporal AO blending disabled until the resolved AO history is also
	// split per view/camera. The depth input is now owned, but reusing one AO
	// history image across multiple views can still smear between cameras.
	blurPush.historyBlend = 0.0f;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoBlurComputePipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoPipelineLayout, 0, 1, &blurDescriptorSet, 0, nullptr);
	vkCmdPushConstants(cmd, aoPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(blurPush), &blurPush);
	vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1u);

	TransitionAoImage(
		ambientOcclusionBlurred.image,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_ACCESS_TRANSFER_READ_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT);
	TransitionAoImage(
		ambientOcclusionHistory.image,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_ACCESS_SHADER_READ_BIT,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageCopy copyRegion{};
	copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyRegion.srcSubresource.mipLevel = 0u;
	copyRegion.srcSubresource.baseArrayLayer = 0u;
	copyRegion.srcSubresource.layerCount = 1u;
	copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyRegion.dstSubresource.mipLevel = 0u;
	copyRegion.dstSubresource.baseArrayLayer = 0u;
	copyRegion.dstSubresource.layerCount = 1u;
	copyRegion.extent = { width, height, 1u };
	// Keep the AO material descriptor stable for the lifetime of the frame.
	// The blur pass writes the frame's final AO image, then this copy seeds the
	// next frame's history image without swapping Vulkan handles behind any
	// already-recorded descriptor sets.
	vkCmdCopyImage(
		cmd,
		ambientOcclusionBlurred.image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		ambientOcclusionHistory.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1u,
		&copyRegion);

	TransitionAoImage(
		ambientOcclusionBlurred.image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_TRANSFER_READ_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	TransitionAoImage(
		ambientOcclusionHistory.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	ambientOcclusionHistoryValid = true;
	return true;
}

// VulkanContext::Impl::GenerateVRSImage is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::GenerateVRSImage(VkCommandBuffer cmd, VkImageView colorView) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
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
	colorInfo.sampler = CurrentHiZ(kHiZGameViewIndex).sampler;

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
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	if (!impl->initialized || impl->skipFrame || impl->deviceLost) return;

	const auto endFrameStart = BackendClock::now();
	// Submit all recorded camera command buffers together so the swapchain is
	// presented once using the final camera's output.
	if (impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		return;
	}

	auto& cmds = impl->pendingPrimaryCommandBuffers[impl->currentFrame];
	if (impl->editorMode && impl->framebufferResized) {
		// A GLFW resize can arrive after scene/game command buffers have been
		// recorded but before they are submitted. Those command buffers may
		// reference old offscreen framebuffers, depth resources, or swapchain-
		// sized attachments, so submitting them can device-lost the queue on
		// some drivers. Drop this frame and rebuild the resized targets before
		// the editor UI pass acquires the next swapchain image.
		impl->DiscardRecordedHiZWrites(impl->currentFrame);
		cmds.clear();
		if (impl->currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] = 0u;
		}
		impl->editorRenderSubmitted = false;
		// Rebuild promptly on the leading edge, then cap repeated device-idle work
		// while the OS continues sending resize notifications during a drag.
		if (!impl->ClaimFramebufferResizeRebuild()) {
			return;
		}
		impl->framebufferResized = false;
		const auto recoveryStart = BackendClock::now();
		impl->RecreateSwapChain();
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::ResizeRecreateWait, TerraEngine::Renderer::BackendSubmitStage::SwapchainRecovery, RenderViewType::Game, 0u, "Recreate editor swapchain before submit", true, false },
			ElapsedMilliseconds(recoveryStart),
			0.25f,
			"Editor resize was processed before submitting stale scene/game command buffers.");
		return;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (cmds.empty()) {
		if (!impl->editorMode && impl->swapchainImageAcquired) {
			// Standalone runtime acquires the swapchain image in BeginFrame.
			// If render graph rejection or startup state leaves no recorded
			// command buffers, still consume the acquire semaphore and present
			// once so the binary semaphore and frame fence cannot be reused
			// while the presentation engine still owns pending work.
			VkSemaphore waitSemaphores[] = {impl->imageAvailableSemaphores[impl->currentFrame]};
			VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
			VkSemaphore signalSemaphore = impl->uiSemaphores[impl->acquiredImageIndex];

			VkSubmitInfo emptySubmitInfo{};
			emptySubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			if (!impl->swapchainAcquireWaitConsumed) {
				emptySubmitInfo.waitSemaphoreCount = 1;
				emptySubmitInfo.pWaitSemaphores = waitSemaphores;
				emptySubmitInfo.pWaitDstStageMask = waitStages;
			}
			emptySubmitInfo.signalSemaphoreCount = 1;
			emptySubmitInfo.pSignalSemaphores = &signalSemaphore;

			const auto resetFenceStart = BackendClock::now();
			const VkResult resetResult = vkResetFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame]);
			if (resetResult != VK_SUCCESS) {
				if (impl->MarkDeviceLost(resetResult, "empty runtime frame fence reset")) return;
				TERRAENGINE_LOG(LogLevel::Error, "Empty runtime frame fence reset failed: " << resetResult);
				impl->RecreateSwapChain();
				return;
			}
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::RhiEndFrame, TerraEngine::Renderer::BackendSubmitStage::FrameAllocatorReset, RenderViewType::Game, 0u, "Reset frame fence for empty runtime submit", true, false },
				ElapsedMilliseconds(resetFenceStart));

			const auto submitStart = BackendClock::now();
			const VkResult submitResult = impl->SubmitGraphicsQueue(1, &emptySubmitInfo, impl->inFlightFences[impl->currentFrame]);
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::QueueSubmission, TerraEngine::Renderer::BackendSubmitStage::QueueSubmit, RenderViewType::Game, 0u, "Empty runtime queue submit", true, false },
				ElapsedMilliseconds(submitStart),
				0.25f);
			if (submitResult != VK_SUCCESS) {
				if (impl->MarkDeviceLost(submitResult, "empty runtime queue submit")) {
					return;
				}
				TERRAENGINE_LOG(LogLevel::Error, "[Vulkan] Empty QueueSubmit failed! Result: " << submitResult << ". Recreating swapchain to recover state.");
				impl->RecreateSwapChain();
				return;
			}

			VkPresentInfoKHR presentInfo{};
			presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			presentInfo.waitSemaphoreCount = 1;
			presentInfo.pWaitSemaphores = &signalSemaphore;

			VkSwapchainKHR swapChains[] = {impl->swapchain};
			presentInfo.swapchainCount = 1;
			presentInfo.pSwapchains = swapChains;
			presentInfo.pImageIndices = &impl->acquiredImageIndex;

			const auto presentStart = BackendClock::now();
			const VkResult presentResult = impl->PresentSwapchain(&presentInfo);
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::Presentation, TerraEngine::Renderer::BackendSubmitStage::Present, RenderViewType::Game, 0u, "Empty runtime queue present", true, false },
				ElapsedMilliseconds(presentStart),
				0.25f);
			if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || impl->framebufferResized) {
				impl->framebufferResized = false;
				impl->RecreateSwapChain();
			} else if (presentResult != VK_SUCCESS) {
				if (impl->MarkDeviceLost(presentResult, "empty runtime queue present")) {
					return;
				}
				TERRAENGINE_LOG(LogLevel::Error, "failed to present empty swap chain image! Result: " << presentResult);
			}

			impl->swapchainImageAcquired = false;
			impl->swapchainAcquireWaitConsumed = false;
			impl->currentFrame = (impl->currentFrame + 1) % impl->MAX_FRAMES_IN_FLIGHT;
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameCost(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::RhiEndFrame, TerraEngine::Renderer::BackendSubmitStage::Other, RenderViewType::Game, 0u, "RHI End Frame", true, false },
				ElapsedMilliseconds(endFrameStart));
			return;
		}
		return;
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	VkSemaphore waitSemaphores[] = {impl->imageAvailableSemaphores[impl->currentFrame]};
	VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!impl->editorMode && impl->swapchainImageAcquired && !impl->swapchainAcquireWaitConsumed) {
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
	}

	submitInfo.commandBufferCount = static_cast<uint32_t>(cmds.size());
	submitInfo.pCommandBuffers = cmds.data();

	VkSemaphore signalSemaphore = impl->editorMode
		? impl->renderFinishedSemaphores[impl->currentFrame]
		: impl->uiSemaphores[impl->acquiredImageIndex];

	VkSemaphore signalSemaphores[] = {signalSemaphore};
	// Signal render finished only when command buffers are submitted; PresentEditor
	// checks editorRenderSubmitted before waiting on this binary semaphore.
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->editorMode) {
		// In Editor Mode, we defer the frame fence signal to the UI pass (PresentEditor)
		// so the frame is considered "in flight" until the UI is also done.
		const auto submitStart = BackendClock::now();
		VkResult submitResult = impl->SubmitGraphicsQueue(1, &submitInfo, VK_NULL_HANDLE);
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::QueueSubmission, TerraEngine::Renderer::BackendSubmitStage::QueueSubmit, RenderViewType::Game, 0u, "Editor render queue submit", true, false },
			ElapsedMilliseconds(submitStart),
			0.25f);
		if (submitResult != VK_SUCCESS) {
			if (impl->MarkDeviceLost(submitResult, "editor render queue submit")) {
				cmds.clear();
				return;
			}
			TERRAENGINE_LOG(LogLevel::Error, "failed to submit draw command buffers! Result: " << submitResult);
			const VkResult idleResult = impl->WaitGraphicsQueueIdle();
			if (idleResult != VK_SUCCESS) {
				if (impl->MarkDeviceLost(idleResult, "editor render recovery queue idle")) {
					return;
				}
				TERRAENGINE_LOG(LogLevel::Error, "Skipping editor render recovery because the graphics queue did not prove idle: " << idleResult);
				return;
			}
			impl->DiscardRecordedHiZWrites(impl->currentFrame);
			cmds.clear();
			if (impl->currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
				impl->pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] = 0u;
			}
			impl->editorRenderSubmitted = false;
			const auto recoveryStart = BackendClock::now();
			impl->RecreateSwapChain();
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::ResizeRecreateWait, TerraEngine::Renderer::BackendSubmitStage::SwapchainRecovery, RenderViewType::Game, 0u, "Recreate swapchain after editor submit failure", true, false },
				ElapsedMilliseconds(recoveryStart),
				0.25f,
				"Queue submit failed during editor rendering; swapchain recovery was required.");
			return;
		}
		impl->editorRenderSubmitted = true;
		impl->ConfirmHiZWritesSubmitted(impl->currentFrame);
		if (impl->currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] = 1u;
		}
	} else {
		const auto resetFenceStart = BackendClock::now();
		const VkResult resetResult = vkResetFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame]);
		if (resetResult != VK_SUCCESS) {
			if (impl->MarkDeviceLost(resetResult, "runtime frame fence reset")) return;
			TERRAENGINE_LOG(LogLevel::Error, "Runtime frame fence reset failed: " << resetResult);
			impl->RecreateSwapChain();
			return;
		}
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::RhiEndFrame, TerraEngine::Renderer::BackendSubmitStage::FrameAllocatorReset, RenderViewType::Game, 0u, "Reset frame fence", true, false },
			ElapsedMilliseconds(resetFenceStart));
		const auto submitStart = BackendClock::now();
		VkResult submitResult = impl->SubmitGraphicsQueue(1, &submitInfo, impl->inFlightFences[impl->currentFrame]);
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::QueueSubmission, TerraEngine::Renderer::BackendSubmitStage::QueueSubmit, RenderViewType::Game, 0u, "Runtime render queue submit", true, false },
			ElapsedMilliseconds(submitStart),
			0.25f);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (submitResult != VK_SUCCESS) {
			if (impl->MarkDeviceLost(submitResult, "runtime render queue submit")) {
				return;
			}
			impl->DiscardRecordedHiZWrites(impl->currentFrame);
			TERRAENGINE_LOG(LogLevel::Error, "[Vulkan] QueueSubmit failed! Result: " << submitResult << ". Recreating swapchain to recover state.");
			const auto recoveryStart = BackendClock::now();
			impl->RecreateSwapChain();
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::ResizeRecreateWait, TerraEngine::Renderer::BackendSubmitStage::SwapchainRecovery, RenderViewType::Game, 0u, "Recreate swapchain after runtime submit failure", true, false },
				ElapsedMilliseconds(recoveryStart),
				0.25f,
				"Queue submit failed during runtime rendering; swapchain recovery was required.");
			return;
		}
		impl->ConfirmHiZWritesSubmitted(impl->currentFrame);
		if (impl->currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] = 1u;
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

		const auto presentStart = BackendClock::now();
		VkResult result = impl->PresentSwapchain(&presentInfo);
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::Presentation, TerraEngine::Renderer::BackendSubmitStage::Present, RenderViewType::Game, 0u, "Queue present", true, false },
			ElapsedMilliseconds(presentStart),
			0.25f);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || impl->framebufferResized) {
			impl->framebufferResized = false;
			const auto recoveryStart = BackendClock::now();
			impl->RecreateSwapChain();
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::ResizeRecreateWait, TerraEngine::Renderer::BackendSubmitStage::SwapchainRecovery, RenderViewType::Game, 0u, "Recreate swapchain after present", true, false },
				ElapsedMilliseconds(recoveryStart),
				0.25f,
				"Present reported resize or suboptimal swapchain state.");
		} else if (result != VK_SUCCESS) {
			if (impl->MarkDeviceLost(result, "runtime queue present")) {
				return;
			}
			TERRAENGINE_LOG(LogLevel::Error, "failed to present swap chain image! Result: " << result);
		}

		impl->swapchainImageAcquired = false;
		impl->swapchainAcquireWaitConsumed = false;
		impl->currentFrame = (impl->currentFrame + 1) % impl->MAX_FRAMES_IN_FLIGHT;
	}
	TerraEngine::Renderer::Vulkan::RecordVulkanFrameCost(
		impl->pendingBackendFrameBreakdown,
		{ TerraEngine::Renderer::BackendFrameCostCategory::RhiEndFrame, TerraEngine::Renderer::BackendSubmitStage::Other, RenderViewType::Game, 0u, "RHI End Frame", true, false },
		ElapsedMilliseconds(endFrameStart));
	{
		std::lock_guard<std::mutex> lock(impl->statsMutex);
		impl->latestBackendFrameBreakdown = impl->pendingBackendFrameBreakdown;
	}
}

// VulkanContext::SetGlobalTaskShaderCullingEnabled is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::SetGlobalTaskShaderCullingEnabled(bool enabled) {
	if (impl) {
		std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
		impl->globalTaskShaderCullingEnabled = enabled;
	}
}

// VulkanContext::IsTaskShaderSupported is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::IsTaskShaderSupported() const {
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	return impl != nullptr && impl->taskShaderSupported;
}

bool VulkanContext::SupportsVirtualGeometryMeshShaderRoute() const
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	return impl != nullptr && impl->meshShaderSupported;
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

TerraEngine::Renderer::BackendCapabilityProfile VulkanContext::GetBackendCapabilityProfile() const
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	TerraEngine::Renderer::BackendCapabilityProfile profile;
	profile.backendId = "vulkan";
	// Vulkan supports recording independent command preparation on workers, but
	// TerraEngine still keeps swapchain acquire, final queue submission, present,
	// and resource retirement under the single coordinator to avoid lifetime
	// races with editor view resize and frame cleanup.
	profile.supportsParallelRecording = true;
	profile.supportsAsyncUploadPreparation = true;
	profile.supportsTransientResources = true;
	profile.supportsSecondaryRecording = true;
	profile.supportsParallelShadowRecording = true;
	profile.supportsParallelPerViewRecording = true;
	profile.supportsParallelPipelineStatePreparation = true;
	profile.supportsParallelDescriptorPreparation = true;
	profile.supportsParallelUploadPlanning = true;
	profile.supportsParallelTransitionPlanning = true;
	profile.supportsParallelCulling = true;
	profile.supportsParallelLightPreparation = true;
	profile.supportsParallelUiPreparation = true;
	profile.supportsParallelPacketValidation = true;
	profile.supportsPacketLocalDiagnostics = true;
	profile.supportsRenderTaskGroups = true;
	profile.supportsFrameOverlap = false;
	profile.supportsRenderWorkBudgets = true;
	profile.supportsMultiQueueReadiness = true;
	profile.supportsWorkerOwnedPreparation = true;
	profile.supportsImageReadback = true;
	const bool virtualGeometryMeshShaderReady = SupportsVirtualGeometryMeshShaderRoute();
	profile.supportsVirtualGeometryGpuPacketBuffers = virtualGeometryMeshShaderReady;
	profile.supportsVirtualGeometryGpuCompaction = virtualGeometryMeshShaderReady;
	profile.supportsVirtualGeometryGeneratedSubmissions = virtualGeometryMeshShaderReady;
	profile.supportsVirtualGeometryCounterReadback = virtualGeometryMeshShaderReady && profile.supportsImageReadback;
	profile.maxRecordingWorkers = static_cast<std::uint32_t>(std::max<std::size_t>(1u, std::thread::hardware_concurrency()));
	profile.workerSchedulingThreshold = 128u;
	profile.maxPreparedFrames = 2u;
	profile.requiredSerialBoundaries = { "acquire", "final-submit", "present", "resource-retire", "swapchain-resize" };
	profile.queueCapabilities = {
		{ "transfer", TerraEngine::Renderer::RenderBackendQueueMode::Fallback, true, "Upload planning may run on workers, but queue submission remains coordinator-owned." },
		{ "compute", TerraEngine::Renderer::RenderBackendQueueMode::Fallback, false, "Compute overlap is represented by readiness diagnostics until GPU-driven passes need it." },
		{ "graphics", TerraEngine::Renderer::RenderBackendQueueMode::Serial, false, "Graphics final submission stays on the coordinator." },
		{ "presentation", TerraEngine::Renderer::RenderBackendQueueMode::Serial, false, "Presentation stays on the coordinator." }
	};
	profile.diagnosticCapabilities = { "packet-counts", "serialized-reasons", "pass-labels", "resource-warnings", "backend-fallbacks", "backend-frame-breakdown", "worker-recording-batches", "queue-readiness", "work-budgets", "performance-gates", "capture-fallbacks", "capture-readback", "virtual-geometry-gpu-packet-capabilities" };
	profile.diagnosticCapabilities.push_back(virtualGeometryMeshShaderReady
		? "virtual-geometry-mesh-shader-route"
		: "virtual-geometry-unsupported-missing-mesh-shader");
	return profile;
}

bool VulkanContext::ConsumeRenderSubmissionPlan(const TerraEngine::Renderer::RenderSubmissionPlan& plan)
{
	// The current renderer still records concrete Vulkan work from FrameRenderData
	// in RenderFrame. Consuming the validated graph here gives diagnostics and
	// future backends a stable contract without moving presentation ownership
	// out of EndFrame.
	return plan.valid;
}

TerraEngine::Renderer::RenderCaptureResult VulkanContext::ReadbackCapture(const TerraEngine::Renderer::RenderCaptureRequest& request)
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	TerraEngine::Renderer::RenderCaptureResult result;
	result.requestId = request.requestId;
	result.sourceKind = request.sourceKind;
	result.sourceName = request.sourceName;
	result.size = request.targetSize;
	result.frameId = request.submittedFrameId;
	result.freshness = request.freshness;
	result.overlaysIncluded = false;
	result.errorCategory = "backend_readback_unavailable";
	if (!impl || impl->device == VK_NULL_HANDLE || impl->allocator == VK_NULL_HANDLE)
	{
		result.message = "Vulkan capture readback cannot run before the backend is initialized.";
		return result;
	}
	if (impl->deviceLost)
	{
		result.errorCategory = "device_lost";
		result.message = "Vulkan capture readback cannot run while renderer recovery is pending.";
		return result;
	}
	if (impl->inFlightReadbacks.size() >= kMaxVulkanCaptureReadbacks)
	{
		result.errorCategory = "queue_full";
		result.message = "Vulkan capture readback has reached its bounded in-flight limit.";
		return result;
	}

	if (!impl->editorMode)
	{
		result.message = "Vulkan capture readback currently targets editor offscreen render targets.";
		return result;
	}

	const bool captureSceneView = request.sourceKind == TerraEngine::Renderer::RenderCaptureSourceKind::GameView
		&& request.viewType == RenderViewType::Scene;
	const bool captureDedicatedView = request.sourceKind != TerraEngine::Renderer::RenderCaptureSourceKind::GameView;
	const Impl::OffscreenResources& target = captureDedicatedView
		? impl->captureView
		: (captureSceneView ? impl->sceneView : impl->gameView);
	if (target.image == VK_NULL_HANDLE || target.framebuffer == VK_NULL_HANDLE)
	{
		result.message = captureDedicatedView
			? "Dedicated capture render target is not ready for Vulkan readback."
			: (captureSceneView
			? "Scene View render target is not ready for Vulkan readback."
			: "Game View render target is not ready for Vulkan readback.");
		return result;
	}

	const std::uint32_t sourceWidth = target.extent.width;
	const std::uint32_t sourceHeight = target.extent.height;
	if (sourceWidth == 0u || sourceHeight == 0u)
	{
		result.message = "Vulkan readback skipped because the source render target has zero size.";
		return result;
	}

	const std::uint32_t bytesPerPixel = TerraEngine::Renderer::VulkanReadback::BytesPerPixel(impl->offscreenColorFormat);
	if (bytesPerPixel == 0u)
	{
		result.errorCategory = "unsupported_readback";
		result.message = "Vulkan readback cannot encode the active render target format.";
		return result;
	}

	const std::uint64_t pixelCount64 = static_cast<std::uint64_t>(sourceWidth) * static_cast<std::uint64_t>(sourceHeight);
	if (pixelCount64 > std::numeric_limits<std::uint64_t>::max() / bytesPerPixel)
	{
		result.message = "Vulkan readback skipped because the source render target byte count overflowed.";
		return result;
	}
	const std::uint64_t copyBytes64 = pixelCount64 * bytesPerPixel;
	if (copyBytes64 > std::numeric_limits<VkDeviceSize>::max() || copyBytes64 > std::numeric_limits<std::size_t>::max())
	{
		result.message = "Vulkan readback skipped because the source render target is too large to stage.";
		return result;
	}

	VulkanBuffer stagingBuffer{};
	try
	{
		stagingBuffer = impl->CreateBuffer(
			static_cast<VkDeviceSize>(copyBytes64),
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VMA_MEMORY_USAGE_GPU_TO_CPU,
			"Capture Readback Staging Buffer");
	}
	catch (const std::exception& exception)
	{
		result.message = std::string("Vulkan capture readback could not allocate staging memory: ") + exception.what();
		return result;
	}

	auto destroyStagingBuffer = [&]() {
		if (stagingBuffer.buffer != VK_NULL_HANDLE && stagingBuffer.allocation != VK_NULL_HANDLE)
		{
			impl->DestroyBufferImmediate(stagingBuffer);
		}
	};

	VkCommandBuffer commandBuffer = impl->BeginSingleTimeCommands();
	if (commandBuffer == VK_NULL_HANDLE)
	{
		destroyStagingBuffer();
		result.message = "Vulkan capture readback could not allocate an immediate command buffer.";
		return result;
	}
	const VkCommandPool commandPool = impl->immediateCommandPool != VK_NULL_HANDLE
		? impl->immediateCommandPool
		: impl->commandPools[impl->currentFrame];
	auto freeCommandBuffer = [&]() {
		if (commandBuffer != VK_NULL_HANDLE)
		{
			std::lock_guard<std::mutex> lock(impl->immediateCommandMutex);
			vkFreeCommandBuffers(impl->device, commandPool, 1u, &commandBuffer);
			commandBuffer = VK_NULL_HANDLE;
		}
	};
	RecordCaptureImageBarrier(commandBuffer, target.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy copyRegion{};
	copyRegion.bufferOffset = 0u;
	copyRegion.bufferRowLength = 0u;
	copyRegion.bufferImageHeight = 0u;
	copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyRegion.imageSubresource.mipLevel = 0u;
	copyRegion.imageSubresource.baseArrayLayer = 0u;
	copyRegion.imageSubresource.layerCount = 1u;
	copyRegion.imageOffset = { 0, 0, 0 };
	copyRegion.imageExtent = { sourceWidth, sourceHeight, 1u };
	vkCmdCopyImageToBuffer(
		commandBuffer,
		target.image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		stagingBuffer.buffer,
		1u,
		&copyRegion);
	RecordCaptureImageBarrier(commandBuffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	const VkResult endResult = vkEndCommandBuffer(commandBuffer);
	if (endResult != VK_SUCCESS)
	{
		freeCommandBuffer();
		destroyStagingBuffer();
		result.message = "Vulkan capture readback could not finish recording its copy command.";
		return result;
	}

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence readbackFence = VK_NULL_HANDLE;
	const VkResult fenceResult = vkCreateFence(impl->device, &fenceInfo, nullptr, &readbackFence);
	if (fenceResult != VK_SUCCESS)
	{
		destroyStagingBuffer();
		freeCommandBuffer();
		result.message = "Vulkan capture readback could not create a fence.";
		return result;
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;

	VkResult submitResult = VK_SUCCESS;
	{
		std::lock_guard<std::mutex> lock(impl->immediateCommandMutex);
		submitResult = impl->SubmitGraphicsQueue(1u, &submitInfo, readbackFence);
	}
	if (submitResult != VK_SUCCESS)
	{
		const bool lost = impl->MarkDeviceLost(submitResult, "asynchronous capture readback submit");
		vkDestroyFence(impl->device, readbackFence, nullptr);
		freeCommandBuffer();
		destroyStagingBuffer();
		result.errorCategory = lost ? "device_lost" : "backend_readback_unavailable";
		result.message = lost
			? "Vulkan device was lost while submitting capture readback."
			: "Vulkan capture readback could not submit its copy command.";
		return result;
	}

	Impl::VulkanReadbackRequest readbackRequest{};
	readbackRequest.completion = result;
	readbackRequest.completion.size = glm::uvec2(sourceWidth, sourceHeight);
	readbackRequest.completion.mimeType = "image/png";
	readbackRequest.completion.backendReadback = true;
	readbackRequest.completion.deterministicFallback = false;
	readbackRequest.completion.pendingAsync = false;
	readbackRequest.completion.overlaysIncluded = false;
	readbackRequest.viewType = request.viewType;
	readbackRequest.sourceImage = target.image;
	readbackRequest.stagingBuffer = stagingBuffer.buffer;
	readbackRequest.stagingAllocation = stagingBuffer.allocation;
	readbackRequest.fence = readbackFence;
	readbackRequest.commandBuffer = commandBuffer;
	readbackRequest.commandPool = commandPool;
	readbackRequest.width = sourceWidth;
	readbackRequest.height = sourceHeight;
	readbackRequest.sourceFormat = impl->offscreenColorFormat;
	readbackRequest.submittedAt = std::chrono::steady_clock::now();
	readbackRequest.timeout = std::chrono::milliseconds(request.timeoutMs > 0u ? request.timeoutMs : 2000u);
	if (request.targetSize.x > 0u
		&& request.targetSize.y > 0u
		&& (request.targetSize.x != sourceWidth || request.targetSize.y != sourceHeight))
	{
		readbackRequest.completion.warnings.push_back("Capture returned the native Vulkan render target dimensions without post-readback resizing.");
	}
	if (impl->offscreenColorFormat == VK_FORMAT_R16G16B16A16_SFLOAT)
	{
		readbackRequest.completion.warnings.push_back("HDR render target pixels were tone-clamped and gamma encoded into an 8-bit PNG.");
	}
	if (request.includeOverlays)
	{
		readbackRequest.completion.warnings.push_back("Vulkan render-target readback captures the renderer color target before editor overlay composition.");
	}

	impl->inFlightReadbacks.push_back(std::move(readbackRequest));

	result.ok = true;
	result.pendingAsync = true;
	result.errorCategory.clear();
	result.mimeType = "image/png";
	result.backendReadback = true;
	result.deterministicFallback = false;
	result.message = captureDedicatedView
		? "Queued live Vulkan dedicated capture render target for async readback."
		: (captureSceneView
		? "Queued live Vulkan Scene View render target for async readback."
		: "Queued live Vulkan Game View render target for async readback.");

	return result;
}

void VulkanContext::Impl::DestroyReadbackRequestResources(VulkanReadbackRequest& request)
{
	// This helper is called only after fence completion, failed submission, or
	// device teardown. Command-pool operations need the same external lock used
	// by immediate allocation and submission.
	if (request.fence != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
	{
		vkDestroyFence(device, request.fence, nullptr);
		request.fence = VK_NULL_HANDLE;
	}
	if (request.commandBuffer != VK_NULL_HANDLE && request.commandPool != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
	{
		std::lock_guard<std::mutex> lock(immediateCommandMutex);
		vkFreeCommandBuffers(device, request.commandPool, 1u, &request.commandBuffer);
		request.commandBuffer = VK_NULL_HANDLE;
	}
	if (request.stagingBuffer != VK_NULL_HANDLE && request.stagingAllocation != VK_NULL_HANDLE && allocator != VK_NULL_HANDLE)
	{
		VulkanBuffer stagingBuffer{ request.stagingBuffer, request.stagingAllocation, 0u };
		DestroyBufferImmediate(stagingBuffer);
		request.stagingBuffer = VK_NULL_HANDLE;
		request.stagingAllocation = VK_NULL_HANDLE;
	}
}

bool VulkanContext::Impl::HasInFlightReadbackForImage(VkImage image) const
{
	if (image == VK_NULL_HANDLE)
	{
		return false;
	}
	return std::any_of(inFlightReadbacks.begin(), inFlightReadbacks.end(), [image](const VulkanReadbackRequest& request) {
		return request.sourceImage == image;
	});
}

void VulkanContext::Impl::CancelReadbackRequests(std::string_view errorCategory, std::string_view message, bool publishResults)
{
	for (VulkanReadbackRequest& request : inFlightReadbacks)
	{
		if (publishResults && !request.completionPublished)
		{
			request.completion.ok = false;
			request.completion.pendingAsync = false;
			request.completion.base64Data.clear();
			request.completion.errorCategory = std::string(errorCategory);
			request.completion.message = std::string(message);
			completedReadbacks.push_back(std::move(request.completion));
			request.completionPublished = true;
		}
		DestroyReadbackRequestResources(request);
	}
	inFlightReadbacks.clear();
}

void VulkanContext::Impl::ProcessReadbackRequests()
{
	const auto now = std::chrono::steady_clock::now();
	for (auto iterator = inFlightReadbacks.begin(); iterator != inFlightReadbacks.end();)
	{
		VulkanReadbackRequest& request = *iterator;
		VkResult status = vkGetFenceStatus(device, request.fence);
		if (status == VK_NOT_READY)
		{
			if (!request.completionPublished && now - request.submittedAt >= request.timeout)
			{
				request.completion.ok = false;
				request.completion.pendingAsync = false;
				request.completion.base64Data.clear();
				request.completion.errorCategory = "timeout";
				request.completion.message = "Vulkan async capture readback timed out waiting for the GPU.";
				completedReadbacks.push_back(request.completion);
				request.completionPublished = true;
			}
			// A timeout is a client-visible cancellation, not proof that Vulkan
			// released the submission. Keep all GPU objects until a later poll sees
			// the fence signal or device loss.
			++iterator;
			continue;
		}
		if (status == VK_ERROR_DEVICE_LOST)
		{
			MarkDeviceLost(status, "async readback fence polling");
			if (!request.completionPublished)
			{
				request.completion.ok = false;
				request.completion.errorCategory = "device_lost";
				request.completion.message = "Vulkan device was lost during async capture readback.";
				completedReadbacks.push_back(std::move(request.completion));
			}
			DestroyReadbackRequestResources(request);
			iterator = inFlightReadbacks.erase(iterator);
			continue;
		}
		if (status != VK_SUCCESS)
		{
			if (!request.completionPublished)
			{
				request.completion.ok = false;
				request.completion.errorCategory = "backend_readback_unavailable";
				request.completion.message = "Vulkan async capture readback fence polling failed.";
				completedReadbacks.push_back(request.completion);
				request.completionPublished = true;
			}
			// Query errors such as host/device memory exhaustion do not prove the
			// submitted command stopped using its resources. Retain ownership just
			// like the timeout path and retry on a later protected poll.
			++iterator;
			continue;
		}

		// A previously published timeout only needs deferred resource cleanup once
		// the fence signals; it must never publish a second terminal result.
		if (request.completionPublished)
		{
			DestroyReadbackRequestResources(request);
			iterator = inFlightReadbacks.erase(iterator);
			continue;
		}

		const std::uint32_t bytesPerPixel = TerraEngine::Renderer::VulkanReadback::BytesPerPixel(request.sourceFormat);
		const std::uint64_t copyBytes64 = static_cast<std::uint64_t>(request.width) * static_cast<std::uint64_t>(request.height) * bytesPerPixel;

		const bool captureSceneView = request.completion.sourceKind == TerraEngine::Renderer::RenderCaptureSourceKind::GameView
			&& request.viewType == RenderViewType::Scene;
		const bool captureDedicatedView = request.completion.sourceKind != TerraEngine::Renderer::RenderCaptureSourceKind::GameView;
		const OffscreenResources& target = captureDedicatedView
			? captureView
			: (captureSceneView ? sceneView : gameView);

		if (request.sourceImage != target.image
			|| request.width != target.extent.width
			|| request.height != target.extent.height)
		{
			request.completion.ok = false;
			request.completion.errorCategory = "resize_aborted";
			request.completion.message = "Vulkan capture readback aborted because its source viewport was recreated before completion.";
			completedReadbacks.push_back(std::move(request.completion));
			DestroyReadbackRequestResources(request);
			iterator = inFlightReadbacks.erase(iterator);
			continue;
		}

		std::vector<std::uint8_t> readbackBytes(static_cast<std::size_t>(copyBytes64));
		void* mapped = nullptr;
		const VkResult mapResult = vmaMapMemory(allocator, request.stagingAllocation, &mapped);
		if (mapResult == VK_ERROR_DEVICE_LOST)
		{
			MarkDeviceLost(mapResult, "async readback memory mapping");
			request.completion.ok = false;
			request.completion.errorCategory = "device_lost";
			request.completion.message = "Vulkan device was lost while mapping capture staging memory.";
			completedReadbacks.push_back(std::move(request.completion));
		}
		else if (mapResult != VK_SUCCESS || mapped == nullptr)
		{
			request.completion.ok = false;
			request.completion.errorCategory = "backend_readback_unavailable";
			request.completion.message = "Vulkan capture readback could not map staging memory.";
			completedReadbacks.push_back(std::move(request.completion));
		}
		else
		{
			const VkResult invalidateResult = vmaInvalidateAllocation(allocator, request.stagingAllocation, 0u, static_cast<VkDeviceSize>(copyBytes64));
			if (invalidateResult == VK_SUCCESS)
			{
				std::memcpy(readbackBytes.data(), mapped, readbackBytes.size());
			}
			vmaUnmapMemory(allocator, request.stagingAllocation);

			if (invalidateResult != VK_SUCCESS)
			{
				if (invalidateResult == VK_ERROR_DEVICE_LOST)
				{
					MarkDeviceLost(invalidateResult, "async readback memory invalidation");
				}
				request.completion.ok = false;
				request.completion.errorCategory = invalidateResult == VK_ERROR_DEVICE_LOST ? "device_lost" : "backend_readback_unavailable";
				request.completion.message = "Vulkan capture readback could not invalidate staging memory for host access.";
				completedReadbacks.push_back(std::move(request.completion));
			}
			else
			{
				try
				{
					std::vector<std::uint8_t> rgbaPixels = TerraEngine::Renderer::VulkanReadback::ConvertToRgba8(
						readbackBytes,
						request.width,
						request.height,
						request.sourceFormat);
					const std::vector<std::uint8_t> pngBytes = TerraEngine::Renderer::EncodeRgba8ToPng(rgbaPixels, request.width, request.height);
					request.completion.base64Data = TerraEngine::Renderer::EncodeBase64(pngBytes);
					request.completion.ok = true;
					request.completion.errorCategory.clear();
					request.completion.message = captureDedicatedView
						? "Captured live Vulkan dedicated capture render target."
						: (captureSceneView
						? "Captured live Vulkan Scene View render target."
						: "Captured live Vulkan Game View render target.");
				}
				catch (const std::exception& exception)
				{
					request.completion.ok = false;
					request.completion.errorCategory = "backend_readback_unavailable";
					request.completion.base64Data.clear();
					request.completion.message = std::string("Vulkan capture readback failed while encoding PNG data: ") + exception.what();
				}
				completedReadbacks.push_back(std::move(request.completion));
			}
		}

		DestroyReadbackRequestResources(request);
		iterator = inFlightReadbacks.erase(iterator);
	}
}

std::vector<TerraEngine::Renderer::RenderCaptureResult> VulkanContext::PollCaptureReadbacks()
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	std::vector<TerraEngine::Renderer::RenderCaptureResult> results;
	if (!impl || !impl->initialized || impl->device == VK_NULL_HANDLE)
	{
		return results;
	}
	impl->ProcessReadbackRequests();
	results.reserve(impl->completedReadbacks.size());
	while (!impl->completedReadbacks.empty())
	{
		results.push_back(std::move(impl->completedReadbacks.front()));
		impl->completedReadbacks.pop_front();
	}
	return results;
}

TerraEngine::Renderer::BackendFrameBreakdown VulkanContext::GetLatestBackendFrameBreakdown() const
{
	if (!impl)
	{
		return {};
	}
	std::lock_guard<std::mutex> lock(impl->statsMutex);
	return impl->latestBackendFrameBreakdown;
}

// VulkanContext::GetLatestRenderSnapshotDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderSnapshotDiagnosticsSnapshot VulkanContext::GetLatestRenderSnapshotDiagnostics() const {
	if (!impl) {
		return {};
	}
	std::lock_guard<std::mutex> lock(impl->statsMutex);
	return impl->latestRenderSnapshotDiagnostics;
}

TerraEngine::Renderer::RendererRecoverySnapshot VulkanContext::GetLatestRendererRecoverySnapshot() const
{
	if (!impl)
	{
		return TerraEngine::Renderer::MakeActiveRecoverySnapshot();
	}
	std::lock_guard<std::mutex> lock(impl->statsMutex);
	return impl->latestRendererRecoverySnapshot;
}

void VulkanContext::PublishRendererRecoverySnapshot(TerraEngine::Renderer::RendererRecoverySnapshot snapshot)
{
	if (!impl)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(impl->statsMutex);
	impl->latestRendererRecoverySnapshot = std::move(snapshot);
}

VulkanContext::LiveGpuSubmissionBoundary VulkanContext::GetLiveGpuSubmissionBoundary() const
{
	return {
		"partially-worker-safe",
		"Render snapshots, resource readiness, secondary command recording, and submit packet assembly can be prepared away from the protected path.",
		"Swapchain acquire, queue submit, present, fence/semaphore ownership, editor UI present, and swapchain recovery stay protected."
	};
}

// VulkanContext::InvalidateMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::InvalidateMaterial(const TerraEngine::UUID& materialId) {
	if (impl) {
		std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
		impl->InvalidateMaterial(materialId);
	}
}

// VulkanContext::InvalidateMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::InvalidateMesh(const TerraEngine::UUID& meshId) {
	if (impl) {
		std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
		impl->InvalidateMesh(meshId);
	}
}

// VulkanContext::InvalidateTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::InvalidateTexture(const TerraEngine::UUID& textureId) {
	if (impl) {
		std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
		impl->InvalidateTexture(textureId);
	}
}

// VulkanContext::InvalidateResource is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::InvalidateResource(const TerraEngine::UUID& id) {
	if (impl) {
		std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
		impl->InvalidateResource(id, false);
	}
}

void VulkanContext::RetireLoadedResource(const TerraEngine::UUID& id, std::uint64_t frameId, std::string_view debugName) {
	(void)frameId;
	(void)debugName;
	if (impl) {
		std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
		// Vulkan resource residency currently shares the descriptor-safe
		// invalidation path. Keeping this behind the RHI hook means the engine
		// can swap in true fence-delayed destruction without changing callers.
		impl->InvalidateResource(id, false);
	}
}

// VulkanContext::WaitIdle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::WaitIdle() {
	if (impl && impl->device) {
		std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
		const auto waitStart = BackendClock::now();
		const VkResult idleResult = impl->WaitDeviceIdle();
		if (impl->MarkDeviceLost(idleResult, "explicit Vulkan WaitIdle")) {
			return;
		}
		if (idleResult != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "Explicit Vulkan WaitIdle failed: " << idleResult);
			return;
		}
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::DeviceIdleWait, TerraEngine::Renderer::BackendSubmitStage::ForcedIdle, RenderViewType::Game, 0u, "Explicit Vulkan WaitIdle", true, false },
			ElapsedMilliseconds(waitStart),
			0.25f,
			"Explicit WaitIdle blocks all GPU progress; keep it out of normal frame submission paths.");
	}
}

// VulkanContext::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Shutdown() {
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	impl->Cleanup();
}

// VulkanContext::ClearResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::ClearResources() {
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	impl->ClearResources();
}

// VulkanContext::GetEditorHandles is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::GetEditorHandles(EditorHandles& h) {
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	h.instance = impl->instance;
	h.physicalDevice = impl->physicalDevice;
	h.device = impl->device;
	h.queue = impl->graphicsQueue;
	h.queueFamily = impl->FindQueueFamilies(impl->physicalDevice).graphicsFamily.value();
	// The editor backend must not allocate from the engine's mesh descriptor
	// pool because asset loading and viewport refresh can occur in one frame.
	h.descriptorPool = impl->editorDescriptorPool;
	h.renderPass = impl->renderPass;
	h.minImageCount = impl->requestedSwapchainImageCount;
	h.imageCount = static_cast<uint32_t>(impl->swapchainImages.size());
	h.sceneImageView = (void*)impl->sceneView.view;
	h.sceneSampler = (void*)impl->sceneView.sampler;
	h.sceneImageGeneration = impl->sceneView.generation;
	h.gameImageView = (void*)impl->gameView.view;
	h.gameSampler = (void*)impl->gameView.sampler;
	h.gameImageGeneration = impl->gameView.generation;
	h.vkGetInstanceProcAddr = (void*)vkGetInstanceProcAddr;
}

void VulkanContext::RunWithQueueAccess(const std::function<void()>& operation)
{
	if (!impl || !operation)
	{
		return;
	}

	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	// ImGui's Vulkan backend performs font/texture upload submits internally,
	// outside VulkanContext's normal wrappers. Share the exact same mutex so
	// those submits cannot race worker-driven mesh or texture uploads.
	std::lock_guard<std::mutex> lock(impl->queueAccessMutex);
	operation();
}

// VulkanContext::EnableEditorMode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::EnableEditorMode() {
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	impl->InitEditorMode();
}

// VulkanContext::PresentEditor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::Renderer::Vulkan::EditorPresentResult VulkanContext::PresentEditor(
	std::function<void()> prepareUI,
	std::function<void(void*)> renderUI) {
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	using namespace TerraEngine::Renderer::Vulkan;
	EditorPresentResult result{};
	if (!impl->editorMode || !impl->initialized || impl->deviceLost) {
		result.outcome = impl->deviceLost ? EditorPresentOutcome::DeviceLost : EditorPresentOutcome::Skipped;
		return result;
	}

	auto fail = [&](EditorUiFrameStage stage, VkResult nativeResult, std::string_view operation) {
		result.stage = stage;
		result.nativeResult = static_cast<std::int32_t>(nativeResult);
		if (impl->MarkDeviceLost(nativeResult, operation)) {
			impl->presentationLifecycle.MarkDeviceLost();
			impl->editorCompletionTracker.MarkDeviceLost();
			result.outcome = EditorPresentOutcome::DeviceLost;
		} else {
			result.outcome = EditorPresentOutcome::RecoverableFailure;
			TERRAENGINE_LOG(LogLevel::Error, "Editor UI frame stopped at "
				<< EditorUiFrameStageName(stage) << ": " << nativeResult);
		}
		return result;
	};

	auto consumeOutstandingSemaphores = [&](bool acquired) -> VkResult {
		std::array<VkSemaphore, 2> waits{};
		std::array<VkPipelineStageFlags, 2> stages{
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
		};
		std::uint32_t waitCount = 0;
		if (acquired) waits[waitCount++] = impl->imageAvailableSemaphores[impl->currentFrame];
		if (impl->editorRenderSubmitted) waits[waitCount++] = impl->renderFinishedSemaphores[impl->currentFrame];
		if (waitCount == 0) return VK_SUCCESS;

		VkSubmitInfo drain{};
		drain.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		drain.waitSemaphoreCount = waitCount;
		drain.pWaitSemaphores = waits.data();
		drain.pWaitDstStageMask = stages.data();
		VkResult drainResult = impl->SubmitGraphicsQueue(1, &drain, VK_NULL_HANDLE);
		if (drainResult == VK_SUCCESS) drainResult = impl->WaitGraphicsQueueIdle();
		if (drainResult == VK_SUCCESS) impl->editorRenderSubmitted = false;
		return drainResult;
	};

	// BeginFrame waited this slot's previous UI fence before recording began.
	// Do not reset or free the pool here: EndFrame may have just submitted
	// Scene/Game command buffers from it, and the UI submission below extends
	// their semaphore chain to the fence that the next BeginFrame will wait.

	if (impl->framebufferResized) {
		if (!impl->ClaimFramebufferResizeRebuild()) {
			result.outcome = EditorPresentOutcome::Skipped;
			return result;
		}
		const VkResult drainResult = consumeOutstandingSemaphores(false);
		if (drainResult != VK_SUCCESS) {
			return fail(EditorUiFrameStage::DrainSemaphore, drainResult, "editor resize semaphore drain");
		}
		impl->RecreateSwapChain();
		result.outcome = EditorPresentOutcome::RecreationRequested;
		return result;
	}

	std::uint32_t imageIndex = 0;
	const VkResult acquireResult = vkAcquireNextImageKHR(
		impl->device, impl->swapchain, UINT64_MAX,
		impl->imageAvailableSemaphores[impl->currentFrame], VK_NULL_HANDLE, &imageIndex);
	if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
		const VkResult drainResult = consumeOutstandingSemaphores(false);
		if (drainResult != VK_SUCCESS) {
			return fail(EditorUiFrameStage::DrainSemaphore, drainResult, "editor outdated acquire semaphore drain");
		}
		impl->RecreateSwapChain();
		result.outcome = EditorPresentOutcome::RecreationRequested;
		result.stage = EditorUiFrameStage::AcquireImage;
		result.nativeResult = static_cast<std::int32_t>(acquireResult);
		return result;
	}
	if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
		return fail(EditorUiFrameStage::AcquireImage, acquireResult, "editor swapchain image acquire");
	}

	AcquiredPresentationImage acquired{
		impl->currentFrame,
		imageIndex,
		impl->presentationLifecycle.Generation()
	};
	impl->acquiredPresentationImage = acquired;
	result.acquiredImage = acquired;
	if (!impl->presentationLifecycle.Validate(acquired)
		|| imageIndex >= impl->framebuffers.size()
		|| imageIndex >= impl->uiSemaphores.size()) {
		const VkResult drainResult = consumeOutstandingSemaphores(true);
		if (drainResult != VK_SUCCESS) {
			return fail(EditorUiFrameStage::DrainSemaphore, drainResult, "invalid editor image semaphore drain");
		}
		impl->presentationLifecycle.RequestRecreation();
		result.outcome = EditorPresentOutcome::RecreationRequested;
		result.stage = EditorUiFrameStage::AcquireImage;
		return result;
	}

	VkCommandBuffer uiCommandBuffer = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo allocation{};
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = impl->commandPools[impl->currentFrame];
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	const VkResult allocationResult = vkAllocateCommandBuffers(impl->device, &allocation, &uiCommandBuffer);
	if (allocationResult != VK_SUCCESS) {
		consumeOutstandingSemaphores(true);
		return fail(EditorUiFrameStage::AllocateCommandBuffer, allocationResult, "editor UI command buffer allocation");
	}
	impl->extraPrimaryCommandBuffers[impl->currentFrame].push_back(uiCommandBuffer);

	try {
		// Dynamic ImGui font textures use a backend-owned transfer command pool
		// and hidden queue submit/wait calls. Process those operations only after
		// this frame slot's fence completed, and before beginning the swapchain UI
		// command buffer, so mouse-triggered glyph uploads cannot overlap an active
		// render pass or retire descriptors still referenced by this frame slot.
		std::lock_guard<std::mutex> queueLock(impl->queueAccessMutex);
		if (prepareUI) prepareUI();
	} catch (...) {
		consumeOutstandingSemaphores(true);
		return fail(EditorUiFrameStage::RecordDrawData, VK_ERROR_INITIALIZATION_FAILED,
			"editor UI backend texture preparation");
	}

	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	const VkResult beginResult = vkBeginCommandBuffer(uiCommandBuffer, &begin);
	if (beginResult != VK_SUCCESS) {
		consumeOutstandingSemaphores(true);
		return fail(EditorUiFrameStage::BeginCommandBuffer, beginResult, "editor UI command buffer begin");
	}
	impl->RecordDiagnosticCheckpoint(uiCommandBuffer, "editor.ui.begin");
	if (impl->validationLayersEnabled && vkSetDebugUtilsObjectNameEXT != nullptr) {
		VkDebugUtilsObjectNameInfoEXT name{};
		name.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
		name.objectType = VK_OBJECT_TYPE_COMMAND_BUFFER;
		name.objectHandle = reinterpret_cast<std::uint64_t>(uiCommandBuffer);
		name.pObjectName = "Editor UI Present Command Buffer";
		const VkResult namingResult = vkSetDebugUtilsObjectNameEXT(impl->device, &name);
		if (namingResult != VK_SUCCESS) {
			consumeOutstandingSemaphores(true);
			return fail(EditorUiFrameStage::DiagnosticNaming, namingResult, "editor UI diagnostic naming");
		}
	}

	VkRenderPassBeginInfo renderPass{};
	renderPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPass.renderPass = impl->renderPass;
	renderPass.framebuffer = impl->framebuffers[imageIndex];
	renderPass.renderArea.extent = impl->swapchainExtent;
	VkClearValue clear{};
	clear.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
	renderPass.clearValueCount = 1;
	renderPass.pClearValues = &clear;

	try {
		vkCmdBeginRenderPass(uiCommandBuffer, &renderPass, VK_SUBPASS_CONTENTS_INLINE);
		{
			// Dear ImGui records descriptor-backed draw calls here. The queue
			// mutex is also the editor's protected descriptor/presentation boundary.
			std::lock_guard<std::mutex> queueLock(impl->queueAccessMutex);
			if (renderUI) renderUI(uiCommandBuffer);
		}
		vkCmdEndRenderPass(uiCommandBuffer);
	} catch (...) {
		consumeOutstandingSemaphores(true);
		return fail(EditorUiFrameStage::RecordDrawData, VK_ERROR_INITIALIZATION_FAILED,
			"editor UI backend draw recording");
	}
	impl->RecordDiagnosticCheckpoint(uiCommandBuffer, "editor.ui.end");
	const VkResult endResult = vkEndCommandBuffer(uiCommandBuffer);
	if (endResult != VK_SUCCESS) {
		consumeOutstandingSemaphores(true);
		return fail(EditorUiFrameStage::EndCommandBuffer, endResult, "editor UI command buffer end");
	}

	std::array<VkSemaphore, 2> waits{
		impl->imageAvailableSemaphores[impl->currentFrame],
		VK_NULL_HANDLE
	};
	std::array<VkPipelineStageFlags, 2> waitStages{
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
	};
	std::uint32_t waitCount = 1;
	if (impl->editorRenderSubmitted) {
		waits[waitCount++] = impl->renderFinishedSemaphores[impl->currentFrame];
	}
	VkSemaphore presentReady = impl->uiSemaphores[imageIndex];
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = waitCount;
	submit.pWaitSemaphores = waits.data();
	submit.pWaitDstStageMask = waitStages.data();
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &uiCommandBuffer;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &presentReady;

	const VkResult resetFenceResult = vkResetFences(
		impl->device, 1, &impl->inFlightFences[impl->currentFrame]);
	if (resetFenceResult != VK_SUCCESS) {
		consumeOutstandingSemaphores(true);
		return fail(EditorUiFrameStage::ResetFrameFence, resetFenceResult, "editor UI frame fence reset");
	}
	const VkResult submitResult = impl->SubmitGraphicsQueue(
		1, &submit, impl->inFlightFences[impl->currentFrame]);
	if (submitResult != VK_SUCCESS) {
		// Failed submission consumes no waits. Drain them, then replace the now
		// unsignaled fence so the frame slot remains reusable.
		const VkResult drainResult = consumeOutstandingSemaphores(true);
		if (drainResult == VK_SUCCESS) {
			VkFenceCreateInfo fenceInfo{};
			fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			VkFence replacement = VK_NULL_HANDLE;
			const VkResult repairResult = vkCreateFence(impl->device, &fenceInfo, nullptr, &replacement);
			if (repairResult == VK_SUCCESS) {
				vkDestroyFence(impl->device, impl->inFlightFences[impl->currentFrame], nullptr);
				impl->inFlightFences[impl->currentFrame] = replacement;
			} else {
				return fail(EditorUiFrameStage::RepairFence, repairResult, "editor UI frame fence repair");
			}
		}
		return fail(EditorUiFrameStage::Submit, submitResult, "editor UI queue submit");
	}

	result.submissionSerial = impl->editorCompletionTracker.RecordSubmission(impl->currentFrame);
	result.outcome = EditorPresentOutcome::Submitted;
	result.stage = EditorUiFrameStage::Submit;
	impl->editorRenderSubmitted = false;

	VkPresentInfoKHR present{};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &presentReady;
	present.swapchainCount = 1;
	present.pSwapchains = &impl->swapchain;
	present.pImageIndices = &imageIndex;
	const VkResult presentResult = impl->PresentSwapchain(&present);
	result.stage = EditorUiFrameStage::Present;
	result.nativeResult = static_cast<std::int32_t>(presentResult);
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || impl->framebufferResized) {
		impl->RecreateSwapChain();
		result.outcome = EditorPresentOutcome::RecreationRequested;
	} else if (presentResult != VK_SUCCESS) {
		return fail(EditorUiFrameStage::Present, presentResult, "editor queue present");
	}

	impl->currentFrame = (impl->currentFrame + 1) % impl->MAX_FRAMES_IN_FLIGHT;
	return result;
}

TerraEngine::Renderer::Vulkan::EditorSubmissionProgress VulkanContext::GetEditorSubmissionProgress() const {
	if (!impl) return {};
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	return impl->editorCompletionTracker.Snapshot();
}

std::int32_t VulkanContext::FreeEditorDescriptorSet(void* descriptorSet) {
	if (!impl || descriptorSet == nullptr) return static_cast<std::int32_t>(VK_SUCCESS);
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	if (impl->deviceLost) return static_cast<std::int32_t>(VK_ERROR_DEVICE_LOST);
	std::lock_guard<std::recursive_mutex> descriptorLock(impl->descriptorHostAccessMutex);
	VkDescriptorSet descriptor = reinterpret_cast<VkDescriptorSet>(descriptorSet);
	return static_cast<std::int32_t>(vkFreeDescriptorSets(
		impl->device, impl->editorDescriptorPool, 1, &descriptor));
}

void VulkanContext::AcknowledgeViewportTextureGeneration(
	ViewType view, std::uint64_t generation, std::uint64_t finalUseSerial) {
	if (!impl) return;
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	for (auto& retired : impl->retiredOffscreenResources) {
		if (retired.editorView == view && retired.replacementGeneration == generation) {
			retired.editorDescriptorAcknowledged = true;
			retired.retireAfterSerial = std::max(retired.retireAfterSerial, finalUseSerial);
		}
	}
	impl->DrainRetiredOffscreenResources();
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
void VulkanContext::Impl::DestroyOffscreenResourceSet(OffscreenResources& resources)
{
	if (resources.sampler != VK_NULL_HANDLE) vkDestroySampler(device, resources.sampler, nullptr);
	if (resources.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, resources.framebuffer, nullptr);
	if (resources.view != VK_NULL_HANDLE) vkDestroyImageView(device, resources.view, nullptr);
	if (resources.depthView != VK_NULL_HANDLE) vkDestroyImageView(device, resources.depthView, nullptr);
	if (resources.image != VK_NULL_HANDLE && resources.allocation != VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, resources.image, resources.allocation);
	}
	if (resources.depthImage != VK_NULL_HANDLE && resources.depthAllocation != VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, resources.depthImage, resources.depthAllocation);
	}
	resources = {};
}

void VulkanContext::Impl::DrainRetiredOffscreenResources()
{
	const std::uint64_t completedSerial = editorCompletionTracker.Snapshot().completedSerial;
	auto iterator = retiredOffscreenResources.begin();
	while (iterator != retiredOffscreenResources.end()) {
		const bool readbackComplete = !HasInFlightReadbackForImage(iterator->resources.image);
		if (!iterator->editorDescriptorAcknowledged
			|| iterator->retireAfterSerial > completedSerial || !readbackComplete) {
			++iterator;
			continue;
		}
		DestroyOffscreenResourceSet(iterator->resources);
		iterator = retiredOffscreenResources.erase(iterator);
	}
}

// VulkanContext::Impl::CreateOffscreenResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CreateOffscreenResources(OffscreenResources& res, VkExtent2D requestedExtent) {
	const std::uint64_t replacementGeneration = res.generation + 1u;
	OffscreenResources candidate{};
	candidate.generation = replacementGeneration;
	auto DestroyOffscreenResourceSet = [&]() {
		this->DestroyOffscreenResourceSet(candidate);
		// Preserve the logical identity only in the temporary candidate. The
		// previously published target remains untouched on creation failure.
		candidate.generation = replacementGeneration;
	};
	VkExtent2D targetExtent = requestedExtent;
	if (targetExtent.width == 0u)
	{
		targetExtent.width = swapchainExtent.width;
	}
	if (targetExtent.height == 0u)
	{
		targetExtent.height = swapchainExtent.height;
	}
	if (targetExtent.width == 0u || targetExtent.height == 0u)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Skipping editor offscreen resource creation because the target extent is zero.");
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (offscreenColorFormat == VK_FORMAT_UNDEFINED) {
			// Scene and game views are sampled by the editor after rendering and may
			// be copied into MCP capture staging buffers. The selected format must
			// support all three operations or readback would rely on undefined driver
			// behavior.
			const VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
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
				TERRAENGINE_LOG(LogLevel::Warning, "No sampled, copyable color-attachment format reported for editor offscreen rendering; falling back to R8G8B8A8_UNORM.");
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
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
			// Hi-Z is generated after the editor render pass, so depth must be
			// stored even though the presented viewport only shows color.
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			// Offscreen depth is explicitly initialized and restored to attachment
			// layout after Hi-Z/AO reads. Preserve that real layout here so a new
			// frame does not introduce an unnecessary UNDEFINED layout transition.
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
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
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
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
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkAttachmentReference colorAttachmentRef{};
			colorAttachmentRef.attachment = 0;
			colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription depthAttachment{};
			depthAttachment.format = FindDepthFormat();
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			// Hi-Z is generated after the editor render pass, so depth must be
			// stored even though the presented viewport only shows color.
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			// The editor restores this persistent image after compute reads. Match
			// that layout so loadOp=CLEAR is ordered without an implicit discard
			// transition racing the prior frame's depth write.
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
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
			// Scene/Game targets are sampled by ImGui and their depth images are read
			// by compute before being restored. Cover both prior consumers and the
			// previous depth restore before this frame clears either attachment.
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
				| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
				| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
				| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
				| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			// Publish color for ImGui and depth for Hi-Z/AO/capture operations that
			// immediately follow the render pass in the recorded command buffer.
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
				| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
				| VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
				| VK_PIPELINE_STAGE_TRANSFER_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT
				| VK_ACCESS_TRANSFER_READ_BIT;
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
	imageInfo.extent.width = targetExtent.width;
	imageInfo.extent.height = targetExtent.height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = offscreenColorFormat;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &candidate.image, &candidate.allocation, nullptr) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor offscreen color image!");
		DestroyOffscreenResourceSet();
		return;
	}
	vmaSetAllocationName(allocator, candidate.allocation, "Offscreen Render Target Image");
	// Editor scene/game views are registered with ImGui as shader-readable
	// textures immediately after creation. Transition the image up front so a
	// skipped or delayed first scene render cannot leave ImGui sampling an
	// image that still has Vulkan's UNDEFINED layout.
	TransitionImageLayout(candidate.image, offscreenColorFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	// Create View
	candidate.view = CreateImageView(candidate.image, offscreenColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
	if (candidate.view == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor offscreen color view!");
		DestroyOffscreenResourceSet();
		return;
	}

	// Create a depth attachment owned by this editor view. Scene View and Game
	// View command buffers are recorded separately and submitted together, so
	// sharing one depth image lets one view clear/write the depth that another
	// view later uses for Hi-Z. Keeping depth per view makes the offscreen
	// framebuffer match the per-view Hi-Z history.
	const VkFormat offscreenDepthFormat = (depthFormat != VK_FORMAT_UNDEFINED) ? depthFormat : FindDepthFormat();
	VkImageCreateInfo depthImageInfo{};
	depthImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	depthImageInfo.imageType = VK_IMAGE_TYPE_2D;
	depthImageInfo.extent.width = targetExtent.width;
	depthImageInfo.extent.height = targetExtent.height;
	depthImageInfo.extent.depth = 1u;
	depthImageInfo.mipLevels = 1u;
	depthImageInfo.arrayLayers = 1u;
	depthImageInfo.format = offscreenDepthFormat;
	depthImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	depthImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	depthImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	depthImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo depthAllocInfo{};
	depthAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	if (vmaCreateImage(allocator, &depthImageInfo, &depthAllocInfo, &candidate.depthImage, &candidate.depthAllocation, nullptr) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor offscreen depth image!");
		DestroyOffscreenResourceSet();
		return;
	}
	vmaSetAllocationName(allocator, candidate.depthAllocation, "Editor Offscreen Depth Image");
	candidate.depthView = CreateImageView(candidate.depthImage, offscreenDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
	if (candidate.depthView == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor offscreen depth view!");
		DestroyOffscreenResourceSet();
		return;
	}
	TransitionImageLayout(candidate.depthImage, offscreenDepthFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	// Create Framebuffer
	std::vector<VkImageView> fbAttachments = {
		candidate.view,
		candidate.depthView
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
	framebufferInfo.width = targetExtent.width;
	framebufferInfo.height = targetExtent.height;
	framebufferInfo.layers = 1;

	if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &candidate.framebuffer) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor offscreen framebuffer!");
		DestroyOffscreenResourceSet();
		return;
	}

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

	if (vkCreateSampler(device, &samplerInfo, nullptr, &candidate.sampler) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor offscreen sampler!");
		DestroyOffscreenResourceSet();
		return;
	}
	candidate.extent = targetExtent;
	OffscreenResources previous = std::move(res);
	res = std::move(candidate);
	const bool previousWasAllocated = previous.image != VK_NULL_HANDLE
		|| previous.depthImage != VK_NULL_HANDLE || previous.framebuffer != VK_NULL_HANDLE;
	if (previousWasAllocated) {
		RetiredOffscreenResourceSet retired{};
		retired.resources = std::move(previous);
		retired.replacementGeneration = replacementGeneration;
		retired.retireAfterSerial = editorCompletionTracker.Snapshot().lastSubmittedSerial;
		if (&res == &sceneView) retired.editorView = VulkanContext::ViewType::Scene;
		else if (&res == &gameView) retired.editorView = VulkanContext::ViewType::Game;
		retired.editorDescriptorAcknowledged = !retired.editorView.has_value();
		retiredOffscreenResources.push_back(std::move(retired));
	}
	DrainRetiredOffscreenResources();
}

// VulkanContext::Impl::RecreateOffscreenResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecreateOffscreenResources() {
	if (deviceLost) {
		return;
	}
	// Each target publishes transactionally and retires through completed UI
	// serials/readback fences, so ordinary viewport recreation never idles the device.
	CreateOffscreenResources(sceneView);
	CreateOffscreenResources(gameView);
	if (captureView.image != VK_NULL_HANDLE || captureView.framebuffer != VK_NULL_HANDLE)
	{
		// Keep the dedicated MCP capture target at its requested extent across
		// editor resizes instead of snapping it back to the swapchain extent.
		CreateOffscreenResources(captureView, captureView.extent);
	}
}

// VulkanContext::Impl::ClearResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::ClearResources() {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	if (!device) return;
	const VkResult idleResult = WaitDeviceIdle();
	if (MarkDeviceLost(idleResult, "clear Vulkan resources device idle")) {
		return;
	}
	if (idleResult != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Skipping Vulkan resource clear because the device did not become idle: " << idleResult);
		return;
	}

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
		if (mesh.meshletSectionBuffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, mesh.meshletSectionBuffer.buffer, mesh.meshletSectionBuffer.allocation);
		}
		vmaDestroyBuffer(allocator, mesh.meshletBoundsBuffer.buffer, mesh.meshletBoundsBuffer.allocation);
	}
	meshCache.clear();

	// View-owned virtual geometry packet buffers are renderer-side scratch
	// resources. Clear them while the device is idle so old packet, batch,
	// compacted, generated-submission, and counter buffers cannot survive a
	// swapchain/device resource rebuild.
	for (std::vector<VirtualGeometryGpuPacketFrameResources>& frameResources : virtualGeometryGpuPacketResourcesByFrame) {
		for (VirtualGeometryGpuPacketFrameResources& resources : frameResources) {
			DestroyVirtualGeometryGpuPacketResources(resources);
		}
		frameResources.clear();
	}
	for (VirtualGeometryGpuPacketFrameResources& resources : retiredVirtualGeometryGpuPacketResources) {
		DestroyVirtualGeometryGpuPacketResources(resources);
	}
	retiredVirtualGeometryGpuPacketResources.clear();

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
	for (VulkanImage& image : ambientOcclusionRawByFrame) {
		DestroyVulkanImage(image);
	}
	for (VulkanImage& image : ambientOcclusionBlurredByFrame) {
		DestroyVulkanImage(image);
	}
	for (VulkanImage& image : ambientOcclusionHistoryByFrame) {
		DestroyVulkanImage(image);
	}
	for (ViewDepthHistoryState& viewHistory : depthHistoryByView) {
		for (VulkanImage& image : viewHistory.ambientOcclusionDepthByFrame) {
			DestroyVulkanImage(image);
		}
		viewHistory.ambientOcclusionDepthValidByFrame.fill(false);
	}
	ambientOcclusionWidth = 0u;
	ambientOcclusionHeight = 0u;
	ambientOcclusionHistoryValidByFrame.fill(false);

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
	// Material slot zero is the permanent fallback material, so rebuilt caches
	// must resume authored material allocation at slot one.
	nextMaterialIndex = 1u;
}
