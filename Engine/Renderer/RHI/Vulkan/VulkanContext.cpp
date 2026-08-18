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
#include "Resource/VirtualGeometryStreamingSystem.h"
#define VMA_LEAK_LOG_FORMAT(format, ...) do { \
		char _vma_buf[1024]; \
		std::snprintf(_vma_buf, sizeof(_vma_buf), (format), __VA_ARGS__); \
		TERRAENGINE_LOG(LogLevel::Error, "[VMA] " << _vma_buf); \
	} while(false)

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
#include <fstream>
#include <optional>
#include <sstream>

#include "VulkanContextSource.h"

// VulkanContext::Impl::ApplyFrameLightBudgets is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VulkanContext::Impl::LightBudgetApplicationResult
VulkanContext::Impl::ApplyFrameLightBudgets(
	const TerraEngine::Renderer::RenderViewPresentationInput& presentation,
	RenderViewType viewType)
{
	LightBudgetApplicationResult result{};
	result.diagnostics.viewType = viewType;
	result.diagnostics.activePreset = presentation.activePreset;
	result.diagnostics.customOverrideActive = presentation.customOverrideActive;
	result.diagnostics.candidateLightCount = static_cast<std::uint32_t>(std::min<std::size_t>(presentation.lightCandidates.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
	result.diagnostics.uploadedLightBudget = std::min(presentation.settings.maxUploadedLights, MAX_GLOBAL_LIGHTS);
	result.diagnostics.retainedLightBudget = std::min(presentation.settings.maxDynamicLights, result.diagnostics.uploadedLightBudget);
	result.diagnostics.shadowLightBudget = std::min(presentation.settings.maxShadowLights, std::min(result.diagnostics.retainedLightBudget, MAX_SHADOW_RESOURCE_SLOTS));
	result.diagnostics.specialLightBudget = std::min(presentation.settings.maxAreaLights, result.diagnostics.retainedLightBudget);
	result.diagnostics.gpuAssistActive = (lightAnalysisComputePipeline != VK_NULL_HANDLE);

	const std::uint32_t viewIndex = (viewType == RenderViewType::Scene) ? 1u : 0u;
	ViewLightingTemporalState& viewState = lightingTemporalStateByView[viewIndex];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ShouldResetTemporalViewState(viewState.valid, viewState.lastCameraLocalPosition, viewState.lastProjectionMatrix, presentation))
	{
		viewState.lights.clear();
		viewState.previousShadowOwners.fill(TerraEngine::EMPTY_UUID);
		viewState.valid = false;
	}
	viewState.frameSerial += 1u;
	const std::uint64_t frameSerial = viewState.frameSerial;

	const GpuLightAnalysisViewSnapshot previousGpuAnalysis = GetLatestGpuLightAnalysis(viewType);
	const std::array<glm::vec4, 6> frustumPlanes = BuildFrustumPlanes(presentation.projectionMatrix * presentation.viewMatrix);
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
	uploadScores.reserve(presentation.lightCandidates.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t candidateIndex = 0; candidateIndex < presentation.lightCandidates.size(); ++candidateIndex)
	{
		UploadScoredCandidate entry{};
		entry.score = ComputeSceneInfluenceScore(presentation.lightCandidates[candidateIndex], presentation.cameraLocalPosition, frustumPlanes);
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
		result.uploadedCandidates.push_back(presentation.lightCandidates[uploadScore.originalIndex]);
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
		scored.screenInfluence = ComputeProjectedScreenInfluence(candidate, presentation.viewMatrix, presentation.projectionMatrix, frustumPlanes);
		scored.gpuVisibilityScore = 0.0f;
		scored.baseScore = ComputeSceneInfluenceScore(candidate, presentation.cameraLocalPosition, frustumPlanes);
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
	viewState.lastCameraLocalPosition = presentation.cameraLocalPosition;
	viewState.lastViewMatrix = presentation.viewMatrix;
	viewState.lastProjectionMatrix = presentation.projectionMatrix;
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
	// A lost device cannot finish a pending generation. Drop the immutable
	// publication and its graphics bridge atomically so later callers select the
	// stateless bind-pose fallback instead of a stale bindless record.
	activeAnimationGraphicsBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeCpuAnimationGraphicsBufferIndex =
		INVALID_BINDLESS_BUFFER_INDEX;
	pendingAnimationCommandPlan = {};
	pendingAnimationImmutablePlan.reset();
	pendingCpuAnimationPlan.reset();
	pendingAnimationFrameId = 0U;
	animationComputeRecordedForCurrentFrame = false;
	animationComputeSubmissionMarkedForCurrentFrame = false;
	recordedAnimationDispatchCountForCurrentFrame = 0U;
	pendingDeformationCommandPlan = {};
	pendingDeformationImmutablePlan.reset();
	deformationComputeRecordedForCurrentFrame = false;
	recordedDeformationDispatchCountForCurrentFrame = 0u;
	activeDeformationGraphicsBufferIndex =
		INVALID_BINDLESS_BUFFER_INDEX;
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestAnimationGpuBackendExecution = {};
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
		// Coalesce callbacks carrying the same framebuffer extent. Native window
		// managers can report repeated dimensions while a resize drag is settling.
		impl->RecordFramebufferResize(width, height);
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
	// A decoded page was uploaded while this frame slot last recorded. The fence
	// is the first point at which both its bytes and bindless descriptor are
	// proven complete. Queue complete value candidates now; the streaming
	// system's next protected PagePublish boundary performs the atomic swap.
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: BeginFrame Complete Virtual Geometry Resources", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if (impl->currentFrame < impl->pendingVirtualGeometryPublicationsByFrame.size())
		{
			auto& completedCandidates =
				impl->pendingVirtualGeometryPublicationsByFrame[impl->currentFrame];
			if (!completedCandidates.empty())
			{
				for (auto& candidate : completedCandidates)
				{
					candidate.descriptor.uploadCompleted = true;
				}
				TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance()
					.SubmitCompletedPublicationCandidates(
						std::move(completedCandidates));
				completedCandidates.clear();
			}
		}
		// The slot fence has completed above. Consume the decoder's host-visible
		// receipt here without a second wait, retaining a prior proven generation on
		// any mismatch or mapping failure.
		impl->CompleteVulkanVirtualGeometryProductionDecode(impl->currentFrame);
	}
	// Reusing a frame slot is the sole completion boundary for its animation
	// generation. No pose data is read back: the completed receipt only records
	// that the existing graphics submission and its compute prefix are finished.
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: BeginFrame Complete Animation Resources", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		impl->CompleteVulkanAnimationFrameResources(impl->currentFrame);
	}
	impl->animationComputeRecordedForCurrentFrame = false;
	impl->animationComputeSubmissionMarkedForCurrentFrame = false;
	impl->recordedAnimationDispatchCountForCurrentFrame = 0U;
	impl->pendingAnimationCommandPlan = {};
	impl->pendingAnimationImmutablePlan.reset();
	impl->pendingAnimationFullImmutablePlanCoverage = false;
	impl->pendingCpuAnimationPlan.reset();
	impl->pendingAnimationFrameId = 0U;
	impl->activeAnimationGraphicsBufferIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
	impl->activeCpuAnimationGraphicsBufferIndex =
		VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
	impl->pendingDeformationCommandPlan = {};
	impl->pendingDeformationImmutablePlan.reset();
	impl->deformationComputeRecordedForCurrentFrame = false;
	impl->recordedDeformationDispatchCountForCurrentFrame = 0u;
	impl->activeDeformationGraphicsBufferIndex =
		VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
	if (impl->editorMode) {
		TERRAENGINE_PROFILE_SCOPE("Vulkan: BeginFrame Complete Editor Resources", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
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
		TERRAENGINE_PROFILE_SCOPE("Vulkan: BeginFrame Drain Stranded Editor Render", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
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
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: BeginFrame Promote HiZ Resources", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		bool promotedHiZ = false;
		for (uint32_t hiZViewIndex = 0u; hiZViewIndex < VulkanContext::Impl::kHiZViewCount; ++hiZViewIndex)
		{
			promotedHiZ = impl->PromoteCompletedHiZWrite(hiZViewIndex, impl->currentFrame) || promotedHiZ;
		}
		if (promotedHiZ)
		{
			impl->UpdateGlobalHiZ(impl->currentFrame);
		}
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
		if (impl->currentFrame < impl->indirectDrawCommandCounts.size()) {
			// The waited fence proves every indirect range recorded by this slot is
			// idle. Reset only now so other in-flight slots retain their commands.
			impl->indirectDrawCommandCounts[impl->currentFrame] = 0u;
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

	// Reset the primary pool only after this frame slot's fence proves every main
	// and extra primary command buffer is idle. Some drivers retain pool-level
	// bookkeeping beyond an individual reset, so this protected reset remains the
	// correctness boundary for multi-view and dedicated-capture command buffers.
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: BeginFrame Reset Command Pools",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const auto resetStart = BackendClock::now();
		const VkResult poolResetResult = vkResetCommandPool(
			impl->device, impl->commandPools[impl->currentFrame], 0);
		if (impl->MarkDeviceLost(poolResetResult, "begin frame command pool reset"))
		{
			return;
		}
		if (poolResetResult != VK_SUCCESS)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"BeginFrame could not reset the completed frame command pool: "
					<< poolResetResult);
			impl->skipFrame = true;
			return;
		}
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::RhiBeginFrame,
				TerraEngine::Renderer::BackendSubmitStage::CommandBufferReset,
				RenderViewType::Game, 0u, "Reset primary command pool", true, false },
			ElapsedMilliseconds(resetStart));
		// Reset only pools that supplied command buffers during the completed use of
		// this frame slot. An individual command-buffer reset is sufficient for Vulkan
		// validity, but some drivers retain native recording resources until the pool
		// is reset. Skipping inactive retained pools preserves the original CPU saving
		// without allowing those native resources to accumulate every frame.
		if (impl->currentFrame < impl->workerCommandPools.size()
			&& impl->currentFrame < impl->workerBufferUsage.size())
		{
			auto& framePools = impl->workerCommandPools[impl->currentFrame];
			const auto& frameUsage = impl->workerBufferUsage[impl->currentFrame];
			const auto workerResetStart = BackendClock::now();
			const std::size_t activePoolLimit = std::min(
				framePools.size(), frameUsage.size());
			std::size_t resetWorkerPoolCount = 0u;
			for (std::size_t poolIndex = 0u;
				poolIndex < activePoolLimit;
				++poolIndex)
			{
				if (frameUsage[poolIndex] == 0u
					|| framePools[poolIndex] == VK_NULL_HANDLE)
				{
					continue;
				}
				bool ownsReusableSecondary = false;
				if(poolIndex < impl->workerCommandBuffers[impl->currentFrame].size())
				{
					for(const VkCommandBuffer secondary :
						impl->workerCommandBuffers[impl->currentFrame][poolIndex])
					{
						if(impl->reusableSecondaryCommandSignatures.contains(secondary))
						{
							ownsReusableSecondary = true;
							break;
						}
					}
				}
				if(ownsReusableSecondary)
				{
					// The frame fence already proved this pool idle. Preserve cached
					// recordings; non-cached buffers in the same pool are reset
					// individually immediately before their next recording.
					continue;
				}
				const VkResult workerResetResult = vkResetCommandPool(
					impl->device, framePools[poolIndex], 0);
				if (impl->MarkDeviceLost(
					workerResetResult, "begin frame active worker command pool reset"))
				{
					return;
				}
				if (workerResetResult != VK_SUCCESS)
				{
					TERRAENGINE_LOG(LogLevel::Error,
						"BeginFrame could not reset an active worker command pool: "
							<< workerResetResult);
					impl->skipFrame = true;
					return;
				}
				++resetWorkerPoolCount;
			}
			if (resetWorkerPoolCount != 0u)
			{
				TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
					impl->pendingBackendFrameBreakdown,
					{ TerraEngine::Renderer::BackendFrameCostCategory::RhiBeginFrame,
						TerraEngine::Renderer::BackendSubmitStage::CommandBufferReset,
						RenderViewType::Game, 0u,
						"Reset active worker command pools", true, false },
					ElapsedMilliseconds(workerResetStart));
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
		const VkFence unextendedAcquireProof = impl->PrepareUnextendedAcquireProof();
		{
			TERRAENGINE_PROFILE_SCOPE("Vulkan: BeginFrame Acquire Swapchain Image", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			result = vkAcquireNextImageKHR(impl->device, impl->swapchain, UINT64_MAX,
				impl->imageAvailableSemaphores[impl->currentFrame], unextendedAcquireProof,
				&impl->acquiredImageIndex);
		}
		impl->ResolveUnextendedAcquireProof(unextendedAcquireProof, result, impl->acquiredImageIndex);
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
			impl->RequestOutOfDateRecovery();
			const auto recoveryStart = BackendClock::now();
			if (impl->ClaimFramebufferResizeRebuild()) impl->RecreateSwapChain();
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
	// Packet storage and compute execution require a live Vulkan device, not
	// mesh-shader support. Keeping this bootstrap capability independent lets a
	// first virtual-geometry frame create and execute the pipelines whose
	// fence-delayed receipts promote the stronger stage-evidence flags below.
	// Hardware raster availability remains available separately through
	// SupportsVirtualGeometryMeshShaderRoute().
	profile.supportsVirtualGeometryGpuPacketBuffers =
		impl != nullptr
		&& impl->device != VK_NULL_HANDLE
		&& impl->descriptorPool != VK_NULL_HANDLE;
	// A compaction capability becomes available only for the current view after
	// all resources, descriptor bindings, and the real compute pipeline are
	// ready. Completion remains a separate, delayed receipt after the slot fence.
	const auto hasCompletedStageEvidence = [impl = impl.get()](TerraEngine::Renderer::VirtualGeometryBackendStage stage) {
		const std::uint32_t index = static_cast<std::uint32_t>(stage);
		return index < impl->completedVirtualGeometryStageEvidence.size()
			&& impl->completedVirtualGeometryStageEvidence[index];
	};
	profile.supportsVirtualGeometryGpuCompaction = hasCompletedStageEvidence(TerraEngine::Renderer::VirtualGeometryBackendStage::Compaction);
	profile.supportsVirtualGeometryGeneratedSubmissions = hasCompletedStageEvidence(TerraEngine::Renderer::VirtualGeometryBackendStage::IndirectGeneration);
	profile.supportsVirtualGeometryCounterReadback = false;
	profile.supportsVirtualGeometryComputeRaster =
		hasCompletedStageEvidence(TerraEngine::Renderer::VirtualGeometryBackendStage::ComputeRaster);
	profile.supportsVirtualGeometryVisibilityResolve =
		hasCompletedStageEvidence(TerraEngine::Renderer::VirtualGeometryBackendStage::VisibilityResolve);
	profile.supportsVirtualGeometryShadingBins =
		hasCompletedStageEvidence(TerraEngine::Renderer::VirtualGeometryBackendStage::Shading);
	profile.virtualGeometryGeneratedWork.supportsDeviceAuthoredWork = profile.supportsVirtualGeometryGeneratedSubmissions;
	profile.virtualGeometryGeneratedWork.supportsZeroCommandSubmission = profile.supportsVirtualGeometryGeneratedSubmissions;
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
		? "virtual-geometry-mesh-shader-packet-rendering"
		: "virtual-geometry-unsupported-missing-mesh-shader");
	profile.diagnosticCapabilities.push_back("virtual-geometry-cpu-expanded-draw-fallback-authority");
	if(profile.supportsVirtualGeometryGpuCompaction)
	{
		profile.diagnosticCapabilities.push_back("virtual-geometry-gpu-compaction-recording-ready");
	}
	if(!impl->latestVirtualGeometryExecutorPlan.unavailableReason.empty())
	{
		profile.diagnosticCapabilities.push_back(
			"virtual-geometry-executor-unavailable: "
				+ impl->latestVirtualGeometryExecutorPlan.unavailableReason);
	}
	profile.diagnosticCapabilities.push_back(
		impl->completedVirtualGeometryProductionDecodeEvidence
			? "virtual-geometry-expanded-production-device-decode-parity-ready: generation="
				+ std::to_string(impl->publishedVirtualGeometryProductionDecodeGeneration)
			: "virtual-geometry-expanded-production-device-decode-cpu-fallback");
	if(!impl->virtualGeometryProductionDecodeUnavailableReason.empty()) {
		profile.diagnosticCapabilities.push_back(
			"virtual-geometry-expanded-production-device-decode-unavailable: "
				+ impl->virtualGeometryProductionDecodeUnavailableReason);
	}
	return profile;
}

bool VulkanContext::ConsumeRenderFramePublication(
	std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication> publication)
{
	TERRAENGINE_PROFILE_SCOPE("Vulkan: Consume Render Frame Publication",
		TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	if(publication == nullptr
		|| publication->frameId == 0u || publication->generation == 0u
		|| publication->rendererSessionGeneration == 0u
		|| publication->backendResourceGeneration == 0u
		|| publication->persistent == nullptr || publication->dynamic == nullptr
		|| publication->views.size() != publication->presentationInputs.size()
		|| publication->backendWork == nullptr)
	{
		TERRAENGINE_LOG(LogLevel::Error,
			"Vulkan rejected an incomplete immutable render frame publication.");
		return false;
	}
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	if(!impl->initialized || impl->deviceLost
		|| publication->rendererSessionGeneration != impl->renderSessionGeneration
		|| publication->backendResourceGeneration != impl->backendResourceGeneration)
	{
		// Never let stale roots reach descriptor/work-plan preparation: that path
		// can allocate against a render pass or frame slot retired by recovery.
		TERRAENGINE_LOG(LogLevel::Warning,
			"Vulkan rejected a stale render publication. frame=" << publication->frameId
				<< ", session=" << publication->rendererSessionGeneration
				<< ", backendResources=" << publication->backendResourceGeneration);
		return false;
	}
	if(!ConsumePublishedWorkPlans(*publication->backendWork))
	{
		return false;
	}
	// Retain the root before descriptor and command preparation.  This is the
	// sole backend-visible owner for view selections and presentation inputs.
	impl->activeRenderFramePublication = std::move(publication);
	return true;
}

bool VulkanContext::ConsumePublishedWorkPlans(
	const TerraEngine::Renderer::RenderBackendWorkPublication& plan)
{
	TERRAENGINE_PROFILE_SCOPE("Vulkan: Consume Published Work Plans",
		TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	std::unique_lock<std::recursive_mutex> stateLock(impl->deviceStateMutex, std::defer_lock);
	{
		// Expose lock contention separately from resource publication. A long wait
		// here points at synchronization ownership, not animation or deformation.
		TERRAENGINE_PROFILE_SCOPE("Vulkan: Submission Plan State Lock Wait", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		stateLock.lock();
	}
	// The renderer graph has already validated packet identity. Recheck the
	// immutable animation hand-off at Vulkan's protected boundary because this
	// code owns descriptor writes and must never follow live animator state.
	if (!plan.valid || !plan.animationWork.valid
		|| (plan.deformationWork != nullptr
			&& !plan.deformationWork->IsComplete()))
	{
		TERRAENGINE_LOG(
			LogLevel::Error,
			"Vulkan rejected an incomplete render submission plan. frame="
				<< plan.frameId << ", renderValid=" << plan.valid
				<< ", animationValid=" << plan.animationWork.valid
				<< ", deformationComplete="
				<< (plan.deformationWork == nullptr
					|| plan.deformationWork->IsComplete()));
		return false;
	}
	impl->activeAnimationGraphicsBufferIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
	impl->activeCpuAnimationGraphicsBufferIndex =
		VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
	impl->pendingAnimationCommandPlan = {};
	impl->pendingAnimationImmutablePlan.reset();
	impl->pendingCpuAnimationPlan.reset();
	impl->pendingAnimationFrameId = 0U;
	impl->animationComputeRecordedForCurrentFrame = false;
	impl->animationComputeSubmissionMarkedForCurrentFrame = false;
	impl->recordedAnimationDispatchCountForCurrentFrame = 0U;
	impl->pendingDeformationCommandPlan = {};
	impl->pendingDeformationImmutablePlan.reset();
	impl->deformationComputeRecordedForCurrentFrame = false;
	impl->recordedDeformationDispatchCountForCurrentFrame = 0u;
	impl->activeDeformationGraphicsBufferIndex =
		VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
	const auto PublishDeformation = [&]()
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: Publish Deformation Plan", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(plan.deformationWork == nullptr
			|| plan.deformationWork->work.empty())
		{
			return true;
		}
		const TerraEngine::Renderer::Vulkan::
			VulkanDeformationCommandPlan commandPlan =
				TerraEngine::Renderer::Vulkan::
					BuildVulkanDeformationCommandPlan(
						*plan.deformationWork);
		if(!commandPlan.valid || !commandPlan.hasWork
			|| !impl->PublishVulkanDeformationResources(
				*plan.deformationWork)
			|| !impl->EnsureVulkanDeformationComputePipelines())
		{
			TERRAENGINE_LOG(
				LogLevel::Error,
				"Vulkan rejected canonical deformation setup. frame="
					<< plan.frameId << ", commandValid="
					<< commandPlan.valid << ", commandHasWork="
					<< commandPlan.hasWork);
			return false;
		}
		impl->pendingDeformationCommandPlan = commandPlan;
		impl->pendingDeformationImmutablePlan =
			plan.deformationWork;
		return true;
	};
	const bool hasGpuAnimationWork =
		plan.animationWork.fullImmutablePlanCoverage
		|| !plan.animationWork.ranges.empty();
	const bool hasCpuAnimationWork =
		plan.animationWork.cpuDeformationPlan != nullptr;
	if(!hasGpuAnimationWork && !hasCpuAnimationWork)
	{
		return PublishDeformation();
	}

	if(hasCpuAnimationWork) {
		TERRAENGINE_PROFILE_SCOPE("Vulkan: Publish CPU Animation Plan", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const std::shared_ptr<const TerraEngine::Renderer::CpuAnimationDeformationPlan>&
			cpuPlan = plan.animationWork.cpuDeformationPlan;
		if(cpuPlan == nullptr || !cpuPlan->IsComplete()
			|| cpuPlan->frameId != plan.frameId
			|| !impl->PublishVulkanCpuAnimationResources(*cpuPlan)) {
			TERRAENGINE_LOG(
				LogLevel::Error,
				"Vulkan rejected CPU animation publication. frame="
					<< plan.frameId << ", planPresent="
					<< (cpuPlan != nullptr) << ", planComplete="
					<< (cpuPlan != nullptr && cpuPlan->IsComplete())
					<< ", matchingFrame="
					<< (cpuPlan != nullptr
						&& cpuPlan->frameId == plan.frameId));
			impl->activeCpuAnimationGraphicsBufferIndex =
				VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
			return false;
		}
		impl->pendingCpuAnimationPlan = cpuPlan;
	}
	if(!hasGpuAnimationWork)
	{
		return PublishDeformation();
	}

	const std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan>& immutablePlan = plan.animationWork.immutablePlan;
	std::uint32_t rendererGeneration = 0u;
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: Validate GPU Animation Plan", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if (!immutablePlan || immutablePlan->generation == 0U || immutablePlan->WorkPackets().empty()) return false;
		rendererGeneration = plan.animationWork.rendererGeneration;
		if (rendererGeneration == 0U) return false;
		if(plan.animationWork.fullImmutablePlanCoverage)
		{
			if(plan.animationWork.gpuAnimatorCount
				!= immutablePlan->WorkPackets().size()) return false;
		}
		for (const TerraEngine::Renderer::AnimationBackendWorkRange& range
			: plan.animationWork.ranges)
		{
			const std::size_t firstPacket = range.firstGpuWorkPacket;
			const std::size_t packetCount = range.gpuWorkPacketCount;
			if (range.animationPlanGeneration != immutablePlan->generation
				|| range.rendererGeneration != rendererGeneration
				|| range.paletteGeneration == 0U
				|| packetCount == 0U
				|| firstPacket > immutablePlan->WorkPackets().size()
				|| packetCount > immutablePlan->WorkPackets().size() - firstPacket)
			{
				return false;
			}
		}
	}

	TerraEngine::Renderer::Vulkan::VulkanAnimationCommandPlan commandPlan;
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: Publish GPU Animation Plan", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		commandPlan = TerraEngine::Renderer::Vulkan::BuildVulkanAnimationCommandPlan(*immutablePlan, rendererGeneration);
		if (!commandPlan.valid || !commandPlan.hasWork) return false;
		if (!impl->PublishVulkanAnimationResources(*immutablePlan, rendererGeneration)
			|| !impl->EnsureVulkanAnimationComputePipelines())
		{
			// PublishVulkanAnimationResources clears the active bridge before it
			// begins. Preserve that invariant on every setup failure and report the
			// rejected handoff truthfully so renderer diagnostics cannot claim that
			// animation work was accepted when no descriptors or pipeline exist.
			impl->activeAnimationGraphicsBufferIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
			impl->activeCpuAnimationGraphicsBufferIndex =
				VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
			impl->pendingCpuAnimationPlan.reset();
			return false;
		}
	}

	impl->pendingAnimationCommandPlan = commandPlan;
	impl->pendingAnimationImmutablePlan = immutablePlan;
	impl->pendingAnimationFullImmutablePlanCoverage =
		plan.animationWork.fullImmutablePlanCoverage
		&& plan.animationWork.cpuOrFallbackAnimatorCount == 0U;
	impl->pendingAnimationFrameId = plan.animationWork.frameId;
	return PublishDeformation();
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

TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot
VulkanContext::GetLatestAnimationGpuBackendExecution() const
{
	if (!impl)
	{
		return {};
	}
	std::lock_guard<std::mutex> lock(impl->statsMutex);
	return impl->latestAnimationGpuBackendExecution;
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

std::uint64_t VulkanContext::GetBackendResourceGeneration() const
{
	if (!impl) return 1u;
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	return impl->backendResourceGeneration;
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
