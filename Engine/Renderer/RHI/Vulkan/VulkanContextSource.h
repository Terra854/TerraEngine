#pragma once

// Shared implementation-only dependencies and helpers for the Vulkan context source files.
// Keeping these helpers internal preserves their original translation-unit ownership while
// allowing each rendering responsibility to live in a focused source file.

#include "VulkanContext.h"
#include "Core/Logger/Logger.h"
#include "Resource/ResourceManager.h"
#include "Resource/VirtualGeometryStreamingSystem.h"
#include "VulkanCommonIncludes.h"
#include "VulkanContextImpl.h"
#include "VulkanFrameDiagnostics.h"
#include "VulkanReadbackConversion.h"
#include "VulkanRendererWorkCache.h"
#include "Renderer/RenderCaptureEncoding.h"
#include "Profiling/ProfilerScope.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <thread>
#include <unordered_set>
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
	// A worker needs enough repeated draw commands to amortize task scheduling,
	// command-pool ownership, and command-buffer begin/end calls. This also bounds
	// retained worker pools in very large crowds.
	constexpr std::size_t kVulkanDrawsPerRecordingWorker = 256u;
	constexpr std::size_t kMaxVulkanCaptureReadbacks = 4u;
	constexpr std::size_t kMaxPreparedPushConstantRanges = 8u;

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

	std::uint64_t BuildVulkanHiZDepthSceneSignature(
		const TerraEngine::Renderer::RenderViewCommandResolver& commands)
	{
		std::uint64_t signature = 1469598103934665603ull;
		HashVulkanHiZDepthSignatureValue(signature, static_cast<std::uint64_t>(commands.Count()));
		std::unordered_map<TerraEngine::UUID, std::uint64_t> materialSignatures;
		materialSignatures.reserve(std::min<std::size_t>(
			commands.Count(), 64U));

		for(std::size_t commandIndex = 0u; commandIndex < commands.Count(); ++commandIndex)
		{
			RenderItemRecord command{};
			if(!commands.Resolve(commandIndex, command)) continue;
			// Hi-Z is a previous-frame acceleration structure. Record the exact
			// depth-producing draw identity that created it for diagnostics, but
			// do not use this signature as a hard gate: interactive movement
			// must keep Hi-Z active and let the shader fail open per meshlet.
			HashVulkanHiZDepthSignatureValue(signature,
				static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(command.objectId)));
			HashVulkanHiZDepthSignatureValue(signature,
				static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(command.meshId)));
			HashVulkanHiZDepthSignatureValue(signature,
				static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(command.materialId)));
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

			const auto cachedMaterial = materialSignatures.find(command.materialId);
			if(cachedMaterial != materialSignatures.end()) {
				HashVulkanHiZDepthSignatureValue(signature, cachedMaterial->second);
				continue;
			}
			// Material depth behavior is shared by every prefab instance. Hash one
			// exact publication per UUID, then fold that compact value into each draw
			// instead of locking ResourceManager and revisiting texture bindings thousands of
			// times on an otherwise stable crowd frame.
			std::uint64_t materialSignature = 1469598103934665603ull;
			const Material* material =
				ResourceManager::Instance().GetMaterial(command.materialId);
			if(material == nullptr) {
				HashVulkanHiZDepthSignatureValue(materialSignature, 0U);
			} else {
				HashVulkanHiZDepthSignatureValue(materialSignature, 1U);
				HashVulkanHiZDepthSignatureValue(materialSignature, material->header.flags);
				HashVulkanHiZDepthSignatureValue(materialSignature, material->header.shadingModel);
				HashVulkanHiZDepthSignatureValue(materialSignature, material->header.alphaMode);
				HashVulkanHiZDepthSignatureUuid(materialSignature,
					material->header.shaderCollectionId);
				HashVulkanHiZDepthSignatureFloat(materialSignature,
					material->header.alphaCutoff);
				HashVulkanHiZDepthSignatureValue(materialSignature,
					static_cast<std::uint64_t>(material->shadowBehavior));
				HashVulkanHiZDepthSignatureValue(materialSignature,
					material->header.textureCount);
				for(const TextureBinding& binding : material->textures) {
					HashVulkanHiZDepthSignatureValue(materialSignature,
						static_cast<std::uint64_t>(binding.semantic));
					HashVulkanHiZDepthSignatureUuid(materialSignature,
						binding.textureUuid);
					HashVulkanHiZDepthSignatureValue(materialSignature,
						static_cast<std::uint64_t>(binding.uvSetIndex));
					HashVulkanHiZDepthSignatureFloat(materialSignature, binding.uvScale[0]);
					HashVulkanHiZDepthSignatureFloat(materialSignature, binding.uvScale[1]);
					HashVulkanHiZDepthSignatureFloat(materialSignature, binding.uvOffset[0]);
					HashVulkanHiZDepthSignatureFloat(materialSignature, binding.uvOffset[1]);
				}
			}
			materialSignatures.emplace(command.materialId, materialSignature);
			HashVulkanHiZDepthSignatureValue(signature, materialSignature);
		}

		return signature;
	}

	bool PushDefaultMeshConstants(
		VkCommandBuffer commandBuffer,
		VkPipelineLayout pipelineLayout,
		const VkPushConstantRange* pushConstantRanges,
		std::size_t pushConstantRangeCount,
		const PushConstants& push)
	{
		if (commandBuffer == VK_NULL_HANDLE
			|| pipelineLayout == VK_NULL_HANDLE
			|| pushConstantRanges == nullptr)
		{
			return false;
		}

		const auto* pushBytes = reinterpret_cast<const std::uint8_t*>(&push);
		bool pushedAnyRange = false;
		for (std::size_t rangeIndex = 0u;
			rangeIndex < pushConstantRangeCount;
			++rangeIndex)
		{
			const VkPushConstantRange& range = pushConstantRanges[rangeIndex];
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
				pipelineLayout,
				range.stageFlags,
				range.offset,
				range.size,
				pushBytes + range.offset);
			pushedAnyRange = true;
		}
		return pushedAnyRange;
	}

	bool PushDefaultMeshConstants(
		VkCommandBuffer commandBuffer,
		const VulkanPipeline& pipeline,
		const PushConstants& push)
	{
		return PushDefaultMeshConstants(
			commandBuffer,
			pipeline.layout,
			pipeline.pushConstants.data(),
			pipeline.pushConstants.size(),
			push);
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
		const glm::vec3& cameraLocalPosition,
		const std::array<glm::vec4, 6>& frustumPlanes)
	{
		const GPULight& light = candidate.gpuLight;
		const std::uint32_t lightType = GetLightType(light);
		const float authoredPriority = std::max(light.extraParams.w, 0.0f);
		const float luminousIntensity = std::max(light.colorAndIntensity.w, 0.0f);
		const float colorWeight = std::max({ light.colorAndIntensity.x, light.colorAndIntensity.y, light.colorAndIntensity.z, 0.0f });
		const float effectiveRange = GetEffectiveLightInfluenceRadius(candidate);
		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const float distanceToCamera = glm::length(lightPosition - cameraLocalPosition);
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
	bool ShouldResetTemporalViewState(bool stateValid,
		const glm::vec3& lastCameraLocalPosition,
		const glm::mat4& lastProjectionMatrix,
		const TerraEngine::Renderer::RenderViewPresentationInput& presentation)
	{
		if (!stateValid)
		{
			return false;
		}

		const float cameraDistance = glm::length(
			presentation.cameraLocalPosition - lastCameraLocalPosition);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (cameraDistance > kTemporalResetDistance)
		{
			return true;
		}

		return !MatricesApproximatelyEqual(presentation.projectionMatrix,
			lastProjectionMatrix, kTemporalResetMatrixEpsilon);
	}

	// ComputeDirectionalShadowCenter is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 ComputeDirectionalShadowCenter(const GPULight& light, const glm::vec3& cameraLocalPosition)
	{
		const glm::vec3 lightDirection = SafeNormalize(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float texelWorldSize = (kDirectionalShadowExtent * 2.0f) / 1024.0f;

		// Directional shadows follow the camera, but snapping the light-space
		// center to whole shadow texels keeps the shadow map from crawling as the
		// camera moves by sub-texel amounts.
		const glm::vec3 provisionalLightPosition = cameraLocalPosition - lightDirection * 60.0f;
		const glm::mat4 provisionalView = glm::lookAtRH(provisionalLightPosition, cameraLocalPosition, worldUp);
		glm::vec4 snappedCenterLightSpace = provisionalView * glm::vec4(cameraLocalPosition, 1.0f);
		snappedCenterLightSpace.x = std::floor(snappedCenterLightSpace.x / texelWorldSize) * texelWorldSize;
		snappedCenterLightSpace.y = std::floor(snappedCenterLightSpace.y / texelWorldSize) * texelWorldSize;
		return glm::vec3(glm::inverse(provisionalView) * snappedCenterLightSpace);
	}

	// BuildDirectionalShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 BuildDirectionalShadowViewProjection(const GPULight& light, const glm::vec3& cameraLocalPosition)
	{
		const glm::vec3 lightDirection = SafeNormalize(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float shadowExtent = kDirectionalShadowExtent;
		const glm::vec3 snappedCenterWorld = ComputeDirectionalShadowCenter(light, cameraLocalPosition);

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
