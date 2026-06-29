// RenderSystem.cpp — Render System Implementation
//
// Implements the RenderSystem class, coordinating the high-level rendering flow for each frame.
// It handles the transition between different rendering states, manages the active camera, and integrates UI and scene drawing.
// The implementation ensures that scene data is correctly prepared and submitted to the RHI for GPU execution.

#include "RenderSystem.h"
#include "UI/UISystem.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "RHI/Vulkan/VulkanRendererWorkCache.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/Graph/RenderSubmissionPlan.h"
#include "Core/Logger/Logger.h"
#include "Core/Scheduling/EcsQueryTaskGraph.h"
#include "Window/WindowSystem.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyComponent.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scene/SceneManager.h"
#include "Core/Object/ObjectManager.h"
#include "Components/Transform.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"
#include "Profiling/Profiler.h"
#include "Profiling/ProfilerScope.h"
#include "Core/Memory/FrameArena.h"
#include "Core/Memory/FlatMap.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "UI/UISystem.h"
#include <entt/entt.hpp>
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

namespace
{
	const glm::vec3 kSkyReferenceSunDirection = glm::normalize(glm::vec3(0.0f, 0.35f, -1.0f));
	constexpr float kDirectionalShadowNear = 0.1f;
	constexpr float kDirectionalShadowFar = 180.0f;
	constexpr float kDirectionalShadowExtent = 40.0f;
	constexpr std::size_t kRenderParallelThreshold = 64u;
	constexpr std::uint64_t kRenderHashOffset = 1469598103934665603ull;
	constexpr std::uint64_t kRenderHashPrime = 1099511628211ull;

	struct CachedDrawCommandRecord
	{
		std::uint64_t signature = 0u;
		std::uint64_t lastTouchedFrameId = 0u;
		DrawCommand command;
	};

	struct DrawCommandCacheState
	{
		std::unordered_map<TerraEngine::UUID, CachedDrawCommandRecord> recordsByObjectId;
		std::size_t hitCount = 0u;
		std::size_t missCount = 0u;
	};

	struct CachedLightCandidateRecord
	{
		std::uint64_t signature = 0u;
		std::uint64_t lastTouchedFrameId = 0u;
		FrameLightCandidate candidate;
	};

	struct LightCandidateCacheState
	{
		std::unordered_map<TerraEngine::UUID, CachedLightCandidateRecord> recordsByLightId;
		std::size_t hitCount = 0u;
		std::size_t missCount = 0u;
	};

	struct VisibleCommandCacheRecord
	{
		std::uint64_t signature = 0u;
		std::uint64_t lastTouchedFrameId = 0u;
		std::vector<DrawCommand> commands;
		TerraEngine::Scene::SpatialDiagnosticsSnapshot diagnostics;
	};

	struct VisibleCommandCacheState
	{
		std::unordered_map<std::uint64_t, VisibleCommandCacheRecord> recordsBySignature;
		std::size_t hitCount = 0u;
		std::size_t missCount = 0u;
	};

	struct ShadowDrawListCacheRecord
	{
		std::uint64_t signature = 0u;
		std::uint64_t lastTouchedFrameId = 0u;
		std::vector<ShadowDrawList> shadowDrawLists;
	};

	struct ShadowDrawListCacheState
	{
		std::unordered_map<std::uint64_t, ShadowDrawListCacheRecord> recordsBySignature;
		std::size_t hitCount = 0u;
		std::size_t missCount = 0u;
	};

	struct VisibleCommandBuildResult
	{
		std::vector<DrawCommand> commands;
		TerraEngine::Scene::SpatialDiagnosticsSnapshot diagnostics;
		bool cacheHit = false;
	};

	VirtualGeometryAssetRuntime BuildVirtualGeometryRuntimeFromMesh(const TerraEngine::UUID& meshId, const Mesh& mesh)
	{
		VirtualGeometryAssetRuntime runtime{};
		runtime.assetId = meshId;
		runtime.cookStatus = mesh.hasVirtualGeometry ? VirtualGeometryCookStatus::Cooked : VirtualGeometryCookStatus::NeedsRebuild;
		runtime.sourceTriangleCount = mesh.virtualGeometryHeader.sourceTriangleCount;
		runtime.sourceVertexCount = mesh.virtualGeometryHeader.sourceVertexCount;

		// Convert the loader-owned TEMF tables into a compact runtime mirror.
		// Render preparation copies this mirror into immutable frame snapshots,
		// so workers can choose LOD/page ranges without touching ResourceManager
		// caches or live scene/component storage.
		runtime.detailLevels.reserve(mesh.virtualGeometryDetailLevels.size());
		for (const VirtualGeometryDetailLevelRecord& detail : mesh.virtualGeometryDetailLevels)
		{
			VirtualGeometryDetailRuntime record{};
			record.lodIndex = detail.lodIndex;
			record.meshletOffset = detail.meshletOffset;
			record.meshletCount = detail.meshletCount;
			record.triangleCount = detail.primitiveCount;
			record.geometricError = detail.geometricError;
			record.targetRatio = detail.targetRatio;
			record.boundsMin = glm::vec3(detail.boundsMin[0], detail.boundsMin[1], detail.boundsMin[2]);
			record.boundsMax = glm::vec3(detail.boundsMax[0], detail.boundsMax[1], detail.boundsMax[2]);
			runtime.detailLevels.push_back(record);
		}
		runtime.cullableGroups.reserve(mesh.virtualGeometryCullableGroups.size());
		for (const VirtualGeometryCullableGroupRecord& group : mesh.virtualGeometryCullableGroups)
		{
			VirtualGeometryCullableGroupRuntime record{};
			record.groupIndex = group.groupIndex;
			record.lodIndex = group.lodIndex;
			record.meshletIndex = group.vertexOffset;
			record.sectionId = group.sectionId;
			record.pageId = group.pageId;
			record.boundsCenter = glm::vec3(group.boundsCenter[0], group.boundsCenter[1], group.boundsCenter[2]);
			record.boundsRadius = group.boundsRadius;
			runtime.cullableGroups.push_back(record);
		}
		runtime.pages.reserve(mesh.virtualGeometryPages.size());
		for (const VirtualGeometryPageRecord& page : mesh.virtualGeometryPages)
		{
			GeometryPageRuntime record{};
			record.pageId = page.pageId;
			record.firstLodIndex = page.firstLodIndex;
			record.lodCount = page.lodCount;
			record.groupOffset = page.groupOffset;
			record.groupCount = page.groupCount;
			record.byteSize = page.byteSize;
			record.priority = static_cast<float>(page.priority);
			record.fallbackPageId = page.fallbackPageId;
			record.fallbackPage = page.pageId == page.fallbackPageId || page.firstLodIndex == 0u;
			// TEMF v1 keeps the cooked virtual-geometry pages inside the loaded
			// mesh payload. Until page streaming starts replacing these embedded
			// ranges with demand-loaded storage, every published page is resident
			// and can participate in LOD selection immediately.
			record.state = GeometryPageState::Resident;
			runtime.pages.push_back(record);
		}
		return runtime;
	}

	bool GeometryPageCoversLod(const GeometryPageRuntime& page, std::uint32_t lodIndex)
	{
		if (page.lodCount == 0u)
		{
			return false;
		}
		const std::uint64_t first = page.firstLodIndex;
		const std::uint64_t lastExclusive = first + page.lodCount;
		return static_cast<std::uint64_t>(lodIndex) >= first && static_cast<std::uint64_t>(lodIndex) < lastExclusive;
	}

	bool GeometryPageIsResident(const GeometryPageRuntime& page)
	{
		return page.state == GeometryPageState::Resident;
	}

	const GeometryPageRuntime* FindGeometryPageForLod(const VirtualGeometryAssetRuntime& asset, std::uint32_t lodIndex)
	{
		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (GeometryPageCoversLod(page, lodIndex))
			{
				return &page;
			}
		}
		return nullptr;
	}

	const GeometryPageRuntime* FindGeometryPageById(const VirtualGeometryAssetRuntime& asset, std::uint32_t pageId)
	{
		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (page.pageId == pageId)
			{
				return &page;
			}
		}
		return nullptr;
	}

	std::size_t DetailIndexForLod(const VirtualGeometryAssetRuntime& asset, std::uint32_t lodIndex)
	{
		for (std::size_t index = 0u; index < asset.detailLevels.size(); ++index)
		{
			if (asset.detailLevels[index].lodIndex == lodIndex)
			{
				return index;
			}
		}
		return 0u;
	}

	std::size_t ResolveResidentVirtualGeometryDetailIndex(
		const VirtualGeometryAssetRuntime& asset,
		std::size_t desiredIndex,
		std::string& outFallbackReason)
	{
		if (asset.pages.empty() || desiredIndex >= asset.detailLevels.size())
		{
			return desiredIndex;
		}

		const std::uint32_t desiredLod = asset.detailLevels[desiredIndex].lodIndex;
		const GeometryPageRuntime* desiredPage = FindGeometryPageForLod(asset, desiredLod);
		if (desiredPage != nullptr && GeometryPageIsResident(*desiredPage))
		{
			return desiredIndex;
		}

		const std::uint32_t fallbackPageId = desiredPage != nullptr ? desiredPage->fallbackPageId : 0u;
		if (const GeometryPageRuntime* fallbackPage = FindGeometryPageById(asset, fallbackPageId);
			fallbackPage != nullptr && GeometryPageIsResident(*fallbackPage))
		{
			// Missing high-detail pages must fail open to a resident fallback.
			// Returning the fallback detail here preserves visibility while the
			// streaming system catches up or reports a page failure.
			outFallbackReason = "desired virtual geometry page is not resident; using resident fallback page";
			return DetailIndexForLod(asset, fallbackPage->firstLodIndex);
		}

		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (page.fallbackPage && GeometryPageIsResident(page))
			{
				outFallbackReason = "desired virtual geometry page is not resident; using first resident fallback page";
				return DetailIndexForLod(asset, page.firstLodIndex);
			}
		}

		outFallbackReason = "desired virtual geometry page is missing and no resident fallback page is published";
		return 0u;
	}

	std::size_t ResolveBestResidentVirtualGeometryDetailIndex(
		const VirtualGeometryAssetRuntime& asset,
		std::string& outFallbackReason)
	{
		if (asset.detailLevels.empty() || asset.pages.empty())
		{
			return 0u;
		}

		// Walk details in cooked order so the highest usable detail wins when
		// virtual geometry is disabled for debugging but resident page metadata
		// is still available. This avoids selecting a non-resident high-detail
		// range that would immediately need another fallback later.
		for (std::size_t detailIndex = 0u; detailIndex < asset.detailLevels.size(); ++detailIndex)
		{
			const std::uint32_t lodIndex = asset.detailLevels[detailIndex].lodIndex;
			if (const GeometryPageRuntime* page = FindGeometryPageForLod(asset, lodIndex);
				page != nullptr && GeometryPageIsResident(*page))
			{
				outFallbackReason = "virtual geometry disabled by quality profile; using best resident detail";
				return detailIndex;
			}
		}

		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (page.fallbackPage && GeometryPageIsResident(page))
			{
				outFallbackReason = "virtual geometry disabled by quality profile; using resident fallback page";
				return DetailIndexForLod(asset, page.firstLodIndex);
			}
		}

		outFallbackReason = "virtual geometry disabled by quality profile; no resident fallback page is published";
		return 0u;
	}

	float EstimateProjectedMeshRadiusPixels(const Mesh& mesh, const glm::mat4& modelMatrix, const glm::vec3& cameraPosition, const glm::uvec2& targetSize)
	{
		if (!mesh.hasLocalBounds || targetSize.x == 0u || targetSize.y == 0u)
		{
			return 512.0f;
		}

		const glm::vec3 localMin(mesh.localBoundsMin[0], mesh.localBoundsMin[1], mesh.localBoundsMin[2]);
		const glm::vec3 localMax(mesh.localBoundsMax[0], mesh.localBoundsMax[1], mesh.localBoundsMax[2]);
		const glm::vec3 localCenter = (localMin + localMax) * 0.5f;
		const float localRadius = glm::length(localMax - localMin) * 0.5f;
		const glm::vec3 worldCenter = glm::vec3(modelMatrix * glm::vec4(localCenter, 1.0f));
		const float worldScale = std::max(
			glm::length(glm::vec3(modelMatrix[0])),
			std::max(glm::length(glm::vec3(modelMatrix[1])), glm::length(glm::vec3(modelMatrix[2]))));
		const float distance = std::max(glm::length(worldCenter - cameraPosition), 1e-3f);
		const float viewportMax = static_cast<float>(std::max(targetSize.x, targetSize.y));
		const float projectedRadius = (localRadius * std::max(worldScale, 1e-6f) / distance) * viewportMax;
		return std::isfinite(projectedRadius) ? projectedRadius : 512.0f;
	}

	std::uint64_t HashFloat(float value)
	{
		std::uint32_t bits = 0u;
		std::memcpy(&bits, &value, sizeof(bits));
		return static_cast<std::uint64_t>(bits);
	}

	void HashCombine(std::uint64_t& seed, std::uint64_t value)
	{
		seed ^= value;
		seed *= kRenderHashPrime;
	}

	void HashVec3(std::uint64_t& seed, const glm::vec3& value)
	{
		HashCombine(seed, HashFloat(value.x));
		HashCombine(seed, HashFloat(value.y));
		HashCombine(seed, HashFloat(value.z));
	}

	void HashVec4(std::uint64_t& seed, const glm::vec4& value)
	{
		HashCombine(seed, HashFloat(value.x));
		HashCombine(seed, HashFloat(value.y));
		HashCombine(seed, HashFloat(value.z));
		HashCombine(seed, HashFloat(value.w));
	}

	void HashMat4(std::uint64_t& seed, const glm::mat4& value)
	{
		for (int column = 0; column < 4; ++column)
		{
			HashVec4(seed, value[column]);
		}
	}

	void HashUVec2(std::uint64_t& seed, const glm::uvec2& value)
	{
		HashCombine(seed, static_cast<std::uint64_t>(value.x));
		HashCombine(seed, static_cast<std::uint64_t>(value.y));
	}

	void HashQuat(std::uint64_t& seed, const glm::quat& value)
	{
		HashCombine(seed, HashFloat(value.x));
		HashCombine(seed, HashFloat(value.y));
		HashCombine(seed, HashFloat(value.z));
		HashCombine(seed, HashFloat(value.w));
	}

	std::uint64_t BuildDrawCommandSignature(
		const TerraEngine::UUID& meshId,
		const TerraEngine::UUID& materialId,
		const Transform& transform,
		bool taskShaderCullingEnabled)
	{
		// The cache key covers every DrawCommand input that affects model-space
		// or resource binding. Scene View tint is applied later to the visible
		// command list, so it intentionally does not invalidate the base cache.
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(meshId)));
		HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(materialId)));
		HashVec3(seed, transform.position);
		HashQuat(seed, transform.rotation);
		HashVec3(seed, transform.scale);
		HashCombine(seed, taskShaderCullingEnabled ? 1u : 0u);
		return seed;
	}

	std::uint64_t BuildLightCandidateSignature(const Transform& transform, const LightComponent& light)
	{
		// The packed light cache key covers transform and every authoring field
		// used to produce the GPULight payload. The renderer can reuse the
		// previous candidate until one of those inputs changes.
		std::uint64_t seed = kRenderHashOffset;
		HashVec3(seed, transform.position);
		HashQuat(seed, transform.rotation);
		HashCombine(seed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(light.type)));
		HashCombine(seed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(light.unit)));
		HashCombine(seed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(light.appearance)));
		HashVec3(seed, light.color);
		HashCombine(seed, HashFloat(light.temperature));
		HashCombine(seed, HashFloat(light.intensity));
		HashCombine(seed, HashFloat(light.range));
		HashCombine(seed, HashFloat(light.innerConeAngle));
		HashCombine(seed, HashFloat(light.outerConeAngle));
		HashCombine(seed, HashFloat(light.width));
		HashCombine(seed, HashFloat(light.height));
		HashCombine(seed, light.castsShadows ? 1u : 0u);
		HashCombine(seed, HashFloat(light.shadowStrength));
		HashCombine(seed, HashFloat(light.shadowBias));
		HashCombine(seed, HashFloat(light.indirectMultiplier));
		HashCombine(seed, HashFloat(light.authoredPriority));
		HashCombine(seed, HashFloat(light.stabilityPreference));
		return seed;
	}

	std::uint64_t BuildRenderSettingsSignature(const RenderSettings& settings, QualityPreset preset, bool taskShaderCullingEnabled)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(preset));
		HashCombine(seed, taskShaderCullingEnabled ? 1u : 0u);
		HashCombine(seed, settings.shadowResolution);
		HashCombine(seed, settings.shadowFilteringSize);
		HashCombine(seed, settings.maxUploadedLights);
		HashCombine(seed, settings.maxDynamicLights);
		HashCombine(seed, settings.maxShadowLights);
		HashCombine(seed, settings.maxAreaLights);
		HashCombine(seed, settings.enableLTC);
		HashCombine(seed, settings.enableIBL);
		HashCombine(seed, HashFloat(settings.indirectStrength));
		HashCombine(seed, HashFloat(settings.ambientOcclusionStrength));
		HashCombine(seed, settings.ambientOcclusionQuality);
		HashCombine(seed, settings.anisotropicFiltering);
		HashCombine(seed, settings.variableRateShading);
		const VirtualGeometryQualityProfile virtualGeometry = SanitizeVirtualGeometryQualityProfile(settings.virtualGeometry);
		// Virtual-geometry debug and quality settings affect the selected meshlet
		// range stored in cached draw commands. Include them in the cache key so
		// editor dropdown changes are visible immediately, without waiting for a
		// camera move to invalidate the view-projection portion of the key.
		HashCombine(seed, HashFloat(virtualGeometry.lodErrorScale));
		HashCombine(seed, virtualGeometry.maxLodDrop);
		HashCombine(seed, virtualGeometry.hiZOcclusionEnabled ? 1u : 0u);
		HashCombine(seed, virtualGeometry.hiZSelfOcclusionGuardEnabled ? 1u : 0u);
		HashCombine(seed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(virtualGeometry.debugVisualizationMode)));
		HashCombine(seed, virtualGeometry.geometryBudgetBytes);
		HashCombine(seed, HashFloat(virtualGeometry.preloadBias));
		return seed;
	}

	std::uint64_t BuildVisibleCommandCacheKey(
		std::uint64_t sceneRevision,
		std::uint64_t settingsSignature,
		std::uint64_t renderableWorldSignature,
		const glm::mat4& viewProjection,
		const glm::uvec2& targetSize,
		RenderViewType viewType,
		std::uint32_t outputId)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, sceneRevision);
		HashCombine(seed, settingsSignature);
		HashCombine(seed, renderableWorldSignature);
		HashMat4(seed, viewProjection);
		HashUVec2(seed, targetSize);
		HashCombine(seed, static_cast<std::uint64_t>(viewType));
		HashCombine(seed, outputId);
		return seed;
	}

	std::uint64_t BuildLightSetSignature(const std::vector<FrameLightCandidate>& lights)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(lights.size()));
		for (const FrameLightCandidate& light : lights)
		{
			HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(light.lightId)));
			HashVec4(seed, light.gpuLight.positionAndType);
			HashVec4(seed, light.gpuLight.directionAndRange);
			HashVec4(seed, light.gpuLight.colorAndIntensity);
			HashVec4(seed, light.gpuLight.coneAnglesAndArea);
			HashVec4(seed, light.gpuLight.extraParams);
			HashCombine(seed, light.candidateFlags);
		}
		return seed;
	}

	std::uint64_t BuildShadowDrawListCacheKey(
		std::uint64_t sceneRevision,
		std::uint64_t settingsSignature,
		std::uint64_t renderableWorldSignature,
		std::uint64_t lightSetSignature,
		const glm::vec3& cameraWorldPosition)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, sceneRevision);
		HashCombine(seed, settingsSignature);
		HashCombine(seed, renderableWorldSignature);
		HashCombine(seed, lightSetSignature);
		HashVec3(seed, cameraWorldPosition);
		return seed;
	}

	void PruneStaleDrawCommandCache(DrawCommandCacheState& cache, const std::unordered_set<TerraEngine::UUID>& aliveObjectIds)
	{
		for (auto iterator = cache.recordsByObjectId.begin(); iterator != cache.recordsByObjectId.end();)
		{
			iterator = aliveObjectIds.contains(iterator->first)
				? std::next(iterator)
				: cache.recordsByObjectId.erase(iterator);
		}
	}

	void PruneStaleLightCandidateCache(LightCandidateCacheState& cache, const std::unordered_set<TerraEngine::UUID>& aliveLightIds)
	{
		for (auto iterator = cache.recordsByLightId.begin(); iterator != cache.recordsByLightId.end();)
		{
			iterator = aliveLightIds.contains(iterator->first)
				? std::next(iterator)
				: cache.recordsByLightId.erase(iterator);
		}
	}

	template<typename Cache>
	void TrimSignatureCache(Cache& cache, std::size_t limit)
	{
		while (cache.recordsBySignature.size() > limit)
		{
			cache.recordsBySignature.erase(cache.recordsBySignature.begin());
		}
	}

	template<typename Fn>
	void RunRenderChunkedWork(std::size_t itemCount, std::size_t workerLimit, std::size_t parallelThreshold, const char* taskLabel, Fn&& work)
	{
		if (itemCount == 0u)
		{
			return;
		}
		const std::size_t workerCount = std::min(itemCount, std::max<std::size_t>(1u, workerLimit));
		if (workerCount <= 1u || itemCount < parallelThreshold)
		{
			work(0u, itemCount);
			return;
		}

		const std::size_t chunkSize = (itemCount + workerCount - 1u) / workerCount;
		std::vector<std::future<void>> futures;
		futures.reserve(workerCount);
		for (std::size_t taskIndex = 0u; taskIndex < workerCount; ++taskIndex)
		{
			const std::size_t begin = taskIndex * chunkSize;
			if (begin >= itemCount)
			{
				break;
			}
			const std::size_t end = std::min(itemCount, begin + chunkSize);
			auto task = ThreadPool::instance().createTask([begin, end, taskLabel, &work]() {
				TERRAENGINE_PROFILE_SCOPE(taskLabel, TerraEngine::Profiling::ProfilerModuleId::TaskExecution);
				work(begin, end);
			});
			futures.push_back(task->getFuture());
			ThreadPool::instance().enqueue(task);
		}
		for (auto& future : futures)
		{
			future.get();
		}
	}

	// ResolveRuntimeSettingsDirectory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::filesystem::path ResolveRuntimeSettingsDirectory()
	{
		auto& resourceManager = ResourceManager::Instance();
		// Exported builds keep writable settings beside the packaged runtime,
		// while editor/project sessions must write next to Assets so authored
		// ProjectSettings stay in <project>/ProjectSettings.
		if (resourceManager.IsExportRuntime())
		{
			return resourceManager.GetAssetRoot() / "Settings";
		}

		return resourceManager.GetProjectRoot() / "ProjectSettings";
	}

	// ResolveLegacyAssetSettingsPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::filesystem::path ResolveLegacyAssetSettingsPath()
	{
		auto& resourceManager = ResourceManager::Instance();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (resourceManager.IsExportRuntime())
		{
			return {};
		}

		// Older builds incorrectly wrote render settings inside Assets. Loading
		// that file as a fallback preserves a user's settings once, and the next
		// save writes them back to the corrected project-level directory.
		return resourceManager.GetAssetRoot() / "ProjectSettings" / "RenderSettings.json";
	}

	// SanitizeRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	RenderSettings SanitizeRenderSettings(const RenderSettings& input)
	{
		RenderSettings sanitized = input;
		sanitized.maxUploadedLights = std::max(sanitized.maxUploadedLights, 1u);
		sanitized.maxDynamicLights = std::min(sanitized.maxDynamicLights, sanitized.maxUploadedLights);
		sanitized.maxAreaLights = std::min(sanitized.maxAreaLights, sanitized.maxDynamicLights);
		sanitized.maxShadowLights = std::min(sanitized.maxShadowLights, sanitized.maxDynamicLights);
		// Shadow maps are allocated lazily by the Vulkan backend, so authoring
		// budgets may exceed four lights without paying for unused slots.
		sanitized.maxShadowLights = std::min(sanitized.maxShadowLights, kMaxShadowResourceSlots);
		sanitized.indirectStrength = std::clamp(sanitized.indirectStrength, 0.0f, 8.0f);
		sanitized.ambientOcclusionStrength = std::clamp(sanitized.ambientOcclusionStrength, 0.0f, 4.0f);
		sanitized.ambientOcclusionQuality = std::min(sanitized.ambientOcclusionQuality, 3u);
		sanitized.enableIBL = sanitized.enableIBL > 0u ? 1u : 0u;
		sanitized.variableRateShading = sanitized.variableRateShading > 0u ? 1u : 0u;
		sanitized.virtualGeometry = SanitizeVirtualGeometryQualityProfile(sanitized.virtualGeometry);
		return sanitized;
	}

	TerraEngine::Scheduling::EcsComponentQueryDeclaration MakeProtectedRenderQuery(
		std::string queryName,
		bool includeDiagnosticsMetadata,
		std::initializer_list<const char*> reads,
		std::initializer_list<const char*> readResources,
		std::initializer_list<const char*> writeResources,
		TerraEngine::Scheduling::EcsQueryChunkWork work)
	{
		TerraEngine::Scheduling::EcsComponentQueryDeclaration query;
		query.queryName = std::move(queryName);
		if (includeDiagnosticsMetadata)
		{
			query.access.ownerSystem = "RenderSystem";
			query.access.queryName = query.queryName;
			query.access.readComponents.assign(reads.begin(), reads.end());
			query.access.readResources.assign(readResources.begin(), readResources.end());
			query.access.writeResources.assign(writeResources.begin(), writeResources.end());
			query.access.workerEligible = false;
			query.access.mainThreadReason = "Renderer collection currently reads live EnTT component storage and frame allocator containers.";
		}
		query.preserveMatchingObjectOrder = true;
		query.copyObjectIdsIntoChunks = false;
		query.runOnceWithoutObjectSnapshot = true;
		query.work = std::move(work);
		return query;
	}

	Transform ReflectTransformComponent(const Transform& transform)
	{
		Transform reflectedTransform{};
		// Render collection runs as protected main-thread work after scripting,
		// so it can read the concrete component fields directly. Avoiding the
		// reflection getter here removes repeated property lookup, variant
		// wrapping, and shared-lock overhead from every visible object each
		// frame.
		reflectedTransform.position = transform.position;
		reflectedTransform.rotation = transform.rotation;
		reflectedTransform.scale = transform.scale;
		return reflectedTransform;
	}

	// SafeNormalizeDirection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 SafeNormalizeDirection(const glm::vec3& direction, const glm::vec3& fallback)
	{
		if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z))
		{
			return fallback;
		}

		const float length = glm::length(direction);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (length <= 0.0001f)
		{
			return fallback;
		}

		return direction / length;
	}

	// ApplySkyYawOffset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 ApplySkyYawOffset(const glm::vec3& direction, float degrees)
	{
		const glm::quat yawRotation = glm::angleAxis(glm::radians(degrees), glm::vec3(0.0f, 1.0f, 0.0f));
		return SafeNormalizeDirection(yawRotation * direction, kSkyReferenceSunDirection);
	}

	// SafeNormalizeShadowDirection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 SafeNormalizeShadowDirection(const glm::vec3& value, const glm::vec3& fallback)
	{
		const float length = glm::length(value);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!std::isfinite(length) || length <= 0.00001f)
		{
			return fallback;
		}

		return value / length;
	}

	// ComputeDirectionalShadowCenter is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 ComputeDirectionalShadowCenter(const GPULight& light, const glm::vec3& cameraWorldPosition)
	{
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float texelWorldSize = (kDirectionalShadowExtent * 2.0f) / 1024.0f;

		// Matching the Vulkan light-space snap keeps CPU culling aligned with the
		// exact shadow map coverage that the GPU will render.
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
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const glm::vec3 snappedCenterWorld = ComputeDirectionalShadowCenter(light, cameraWorldPosition);
		const glm::vec3 lightPosition = snappedCenterWorld - lightDirection * 60.0f;
		glm::mat4 view = glm::lookAtRH(lightPosition, snappedCenterWorld, worldUp);
		glm::mat4 projection = glm::orthoRH_ZO(-kDirectionalShadowExtent, kDirectionalShadowExtent, -kDirectionalShadowExtent, kDirectionalShadowExtent, kDirectionalShadowNear, kDirectionalShadowFar);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

	// BuildSpotShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 BuildSpotShadowViewProjection(const GPULight& light)
	{
		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
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
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
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

}

// RenderSystem::Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderSystem& RenderSystem::Instance()
{
	// Provide a singleton instance so the main engine loop can register the
	// system without managing heap allocations or ownership semantics.
	static RenderSystem instance;
	return instance;
}

RenderSystem::RenderSystem()
	// objManager is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: objManager(ObjectManager::instance())
{
	latestGameStats.viewType = RenderViewType::Game;
	latestSceneStats.viewType = RenderViewType::Scene;
	latestGameBudgetDiagnostics.viewType = RenderViewType::Game;
	latestSceneBudgetDiagnostics.viewType = RenderViewType::Scene;
	latestGameLightingDiagnostics.viewType = RenderViewType::Game;
	latestSceneLightingDiagnostics.viewType = RenderViewType::Scene;
}

void RenderSystem::SetSceneViewContextTint(const std::unordered_set<TerraEngine::UUID>& focusedObjectIds)
{
	// The editor calls this once per Scene View frame while prefab context mode
	// is active. An empty set disables the cue so stale selections do not tint
	// unrelated views on later frames.
	sceneViewContextFocusedObjectIds = focusedObjectIds;
	sceneViewContextTintEnabled = !sceneViewContextFocusedObjectIds.empty();
}

// RenderSystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::Initialize()
{
	TERRAENGINE_LOG(LogLevel::Info, "Initializing RenderSystem...");
	LoadSettings();
	
	auto vulkanContext = std::make_unique<VulkanContext>();
	// VRS is a render-pass attachment decision in Vulkan. Apply the startup
	// render profile before backend initialization so exported builds that
	// declare VRS disabled never create or load a shading-rate attachment.
	vulkanContext->SetVariableRateShadingEnabled(SanitizeRenderSettings(currentSettings).variableRateShading > 0u);
	rhi = std::move(vulkanContext);
	rhi->Initialize(WindowSystem::Instance());

	WindowSystem::Instance().SetResizeCallback([](int width, int height) {
		try {
			auto& renderSystem = RenderSystem::Instance();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (renderSystem.GetContext()) {
				static_cast<VulkanContext*>(renderSystem.GetContext())->Resize(width, height);
			}
		} catch (const std::exception& e) {
			TERRAENGINE_LOG(LogLevel::Error, "Exception in resize callback: " << e.what());
		} catch (...) {
			TERRAENGINE_LOG(LogLevel::Error, "Unknown exception in resize callback");
		}
	});

	initialized = true;
}

template<typename T>
using FrameVector = std::vector<T, FrameAllocator<T>>;

template<typename K, typename V>
using FrameMap = FlatMap<K, V, FrameAllocator<std::pair<K, V>>>;

// RenderSystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::Update(float deltaTime)
{
	if (!initialized) {
		return;
	}
	TERRAENGINE_PROFILE_SCOPE("RenderSystem::Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);

	// Standalone runtime UI should track the real framebuffer size. In the
	// editor, however, the Game View is letterboxed inside an ImGui window and
	// owns its own viewport size, so forcing the full GLFW framebuffer here
	// would make UI layout and hit testing react to the wrong rectangle.
	if (!editorModeEnabled)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Sync Runtime Viewport", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto* windowHandle = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle()))
		{
			int framebufferWidth = 0;
			int framebufferHeight = 0;
			glfwGetFramebufferSize(windowHandle, &framebufferWidth, &framebufferHeight);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (framebufferWidth > 0 && framebufferHeight > 0)
			{
				UISystem::Instance().SetGameViewportSize(glm::vec2(static_cast<float>(framebufferWidth), static_cast<float>(framebufferHeight)));
			}
		}
	}

	// Reset the frame allocator at the start of the frame logic
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reset Frame Arena", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		FrameArena::Instance().Reset();
	}

	// Snapshot camera components so view/projection data is sourced from
	// authored entities instead of the legacy hardcoded values.
	FrameVector<CameraEntry> cameras;
	cameras.reserve(10);
	// Snapshot renderable components to avoid locking the registry during parallel work.
	FrameVector<RenderableEntry> renderables;
	renderables.reserve(1000);
	// Snapshot scene light components so packing can run in worker tasks without
	// touching the entity registry after this point.
	FrameVector<LightEntry> lights;
	lights.reserve(256);
	std::size_t runtimeLayoutRejectedRecordCount = 0u;
	std::vector<std::string> runtimeLayoutRejectionReasons;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: ECS Collection Graph", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const bool diagnosticsEnabled = TerraEngine::Scheduling::EcsGraphFrameDiagnosticsEnabled();
		TerraEngine::Scheduling::EcsQueryTaskGraph renderCollectionGraph({ "render-system-collection", "Render System Collection", false, diagnosticsEnabled });

		renderCollectionGraph.AddQuery(MakeProtectedRenderQuery(
			"Render.CollectCameras",
			diagnosticsEnabled,
			{ "Transform", "CameraComponent" },
			{},
			{ "Render.CameraCollection" },
			[this, &cameras](const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&)
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Cameras", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				objManager.forEachObjectWithComponents<Transform, CameraComponent>(
					[&](Object object, Transform&, CameraComponent& cameraComp)
					{
						CameraComponent reflectedCamera{};
						reflectedCamera.verticalFovDegrees = cameraComp.verticalFovDegrees;
						reflectedCamera.aspectRatio = cameraComp.aspectRatio;
						reflectedCamera.nearPlane = cameraComp.nearPlane;
						reflectedCamera.farPlane = cameraComp.farPlane;
						reflectedCamera.up = cameraComp.up;
						cameras.push_back({ object.EntityId(), reflectedCamera });
					}
				);
			}));

		renderCollectionGraph.AddQuery(MakeProtectedRenderQuery(
			"Render.CollectRenderables",
			diagnosticsEnabled,
			{ "Transform", "MeshRenderComponent" },
			{ "Render.CameraCollection" },
			{ "Render.RenderableCollection" },
			[&cameras, &renderables, &runtimeLayoutRejectedRecordCount, &runtimeLayoutRejectionReasons, this](const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&)
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Renderables", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				if (cameras.empty() && !hasSceneViewCamera)
				{
					return;
				}
				objManager.forEachObjectWithComponents<Transform, MeshRenderComponent>(
					[&](Object object, Transform&, MeshRenderComponent& meshComp)
					{
						const TerraEngine::UUID meshId = meshComp.meshId;
						const TerraEngine::UUID materialId = meshComp.materialId;
						const TerraEngine::UUID spatialObjectId = meshComp.spatialObjectId;
						const bool objectTaskCullingEnabled = meshComp.taskShaderCullingEnabled;
						if (meshId == TerraEngine::EMPTY_UUID || materialId == TerraEngine::EMPTY_UUID)
						{
							++runtimeLayoutRejectedRecordCount;
							runtimeLayoutRejectionReasons.push_back("missing-render-resource-id");
							return;
						}

						auto& rm = ResourceManager::Instance();
						if (!rm.HasResource(meshId) || !rm.HasResource(materialId))
						{
							++runtimeLayoutRejectedRecordCount;
							runtimeLayoutRejectionReasons.push_back("resource-not-loaded");
							return;
						}

						MeshRenderComponent reflectedMesh{};
						reflectedMesh.meshId = meshId;
						reflectedMesh.materialId = materialId;
						reflectedMesh.spatialObjectId = spatialObjectId;
						reflectedMesh.taskShaderCullingEnabled = objectTaskCullingEnabled;
						const TerraEngine::UUID objectId = spatialObjectId != TerraEngine::EMPTY_UUID ? spatialObjectId : objManager.getUUID(object);
						renderables.push_back({ object.EntityId(), objectId, objManager.isObjectStatic(object), reflectedMesh });
					}
				);
			}));

		renderCollectionGraph.AddQuery(MakeProtectedRenderQuery(
			"Render.CollectLights",
			diagnosticsEnabled,
			{ "Transform", "LightComponent" },
			{},
			{ "Render.LightCollection" },
			[this, &lights](const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&)
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Lights", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				objManager.forEachObjectWithComponents<Transform, LightComponent>(
					[&](Object object, Transform&, LightComponent& lightComp)
					{
					LightComponent reflectedLight{};
					const std::uint32_t encodedType = static_cast<std::uint32_t>(lightComp.type);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (encodedType > static_cast<std::uint32_t>(LightType::Area))
					{
						// Unsupported legacy light types are intentionally skipped so they
						// are not silently remapped to another runtime behavior.
						return;
					}
					reflectedLight.type = static_cast<LightType>(encodedType);

					const std::uint32_t encodedUnit = static_cast<std::uint32_t>(lightComp.unit);
					reflectedLight.unit = static_cast<LightUnit>(encodedUnit);

					const std::uint32_t encodedAppearance = static_cast<std::uint32_t>(lightComp.appearance);
					reflectedLight.appearance = static_cast<LightAppearance>(encodedAppearance);

					reflectedLight.color = lightComp.color;
					reflectedLight.temperature = lightComp.temperature;
					reflectedLight.intensity = lightComp.intensity;
					reflectedLight.range = lightComp.range;
					reflectedLight.innerConeAngle = lightComp.innerConeAngle;
					reflectedLight.outerConeAngle = lightComp.outerConeAngle;
					reflectedLight.width = lightComp.width;
					reflectedLight.height = lightComp.height;
					reflectedLight.castsShadows = lightComp.castsShadows;
					reflectedLight.shadowStrength = lightComp.shadowStrength;
					reflectedLight.shadowBias = lightComp.shadowBias;
					reflectedLight.indirectMultiplier = lightComp.indirectMultiplier;
					reflectedLight.authoredPriority = lightComp.authoredPriority;
					reflectedLight.stabilityPreference = lightComp.stabilityPreference;

					lights.push_back({ object.EntityId(), objManager.getUUID(object), reflectedLight });
					}
				);
			}));

		const TerraEngine::Scheduling::EcsSystemGraphFrameSummary& graphSummary = renderCollectionGraph.Execute();
		if (diagnosticsEnabled)
		{
			TerraEngine::Scheduling::PublishEcsGraphFrameSummary(graphSummary);
		}
		if (!graphSummary.valid)
		{
			TERRAENGINE_LOG(LogLevel::Error, TerraEngine::Scheduling::FormatEcsSystemGraphFrameSummary(graphSummary));
		}
	}

	// Throttle warning spam so missing cameras do not flood the console every frame.
	static bool loggedMissingCamera = false;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (cameras.empty() && !hasSceneViewCamera)
	{
		if (!loggedMissingCamera)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "RenderSystem skipped frame because no CameraComponent was found in the scene.");
			loggedMissingCamera = true;
		}
	}
	else
	{
		loggedMissingCamera = false;
	}

	TerraEngine::Scene::SceneTransformPropagationResult transformPropagation;
	FrameMap<entt::entity, Transform> worldTransforms;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Resolve World Transforms", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		TerraEngine::Scene::SceneGraph sceneGraph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objManager, false);
		TerraEngine::Scene::SceneDirtyTransformState dirtyTransforms = TerraEngine::Scene::BuildDirtyTransformState(sceneGraph);
		transformPropagation = TerraEngine::Scene::PropagateWorldTransforms(sceneGraph, dirtyTransforms);
		worldTransforms.reserve(transformPropagation.worldTransformsByEntity.size());
		for (const auto& [entity, transform] : transformPropagation.worldTransformsByEntity)
		{
			worldTransforms.emplace(entity, transform);
		}
		if (!transformPropagation.valid)
		{
			TERRAENGINE_LOG(LogLevel::Error, "RenderSystem received an invalid scene transform propagation result.");
		}
	}

	auto makeModelMatrix = [](const Transform& transform) {
		glm::mat4 model = glm::mat4(1.0f);
		model = glm::translate(model, transform.position);
		model = model * glm::mat4_cast(transform.rotation);
		model = glm::scale(model, transform.scale);
		return model;
	};
	const Transform fallbackWorldTransform{};
	auto getWorldTransform = [&](entt::entity entity) -> const Transform& {
		if (const Transform* transform = worldTransforms.find(entity))
		{
			return *transform;
		}
		return fallbackWorldTransform;
	};

	activeRenderFrameId = nextRenderFrameId++;
	const TerraEngine::Renderer::BackendCapabilityProfile backendCapabilities = rhi
		? rhi->GetBackendCapabilityProfile()
		: TerraEngine::Renderer::BackendCapabilityProfile{};
	TerraEngine::Renderer::RenderFrameSnapshot renderFrameSnapshot;
	renderFrameSnapshot.frameId = activeRenderFrameId;
	renderFrameSnapshot.renderSettings = SanitizeRenderSettings(currentSettings);
	renderFrameSnapshot.sceneRevision = objManager.GetSpatialRevision();
	renderFrameSnapshot.viewRequests.reserve(2u + gameViewCameras.size() + cameras.size());
	if (hasSceneViewCamera && (!editorModeEnabled || (sceneViewTargetSize.x > 0u && sceneViewTargetSize.y > 0u)))
	{
		renderFrameSnapshot.viewRequests.push_back({ RenderOutputKind::SceneView, RenderViewType::Scene, sceneViewTargetSize, 1u });
	}
	if (!gameViewSuppressed && (!editorModeEnabled || (gameViewTargetSize.x > 0u && gameViewTargetSize.y > 0u)))
	{
		if (!gameViewCameras.empty())
		{
			renderFrameSnapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, gameViewTargetSize, 2u });
		}
		else if (!cameras.empty())
		{
			renderFrameSnapshot.viewRequests.push_back({ RenderOutputKind::MainRuntimeView, RenderViewType::Game, gameViewTargetSize, 2u });
		}
	}
	if (renderFrameSnapshot.viewRequests.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Skip No Render Outputs", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		TerraEngine::Renderer::RenderDiagnosticsSummary noViewDiagnostics{};
		noViewDiagnostics.frameId = activeRenderFrameId;
		if (TerraEngine::Renderer::RenderDiagnosticsRequested())
		{
			noViewDiagnostics.delayedWork.push_back("Render frame skipped because no Scene, Game, or runtime output target was requested.");
		}
		TerraEngine::Renderer::PublishRenderDiagnostics(noViewDiagnostics);
		{
			std::lock_guard<std::mutex> lock(statsMutex);
			latestRenderFrameSnapshot = renderFrameSnapshot;
			latestRenderGraphDiagnostics = noViewDiagnostics;
		}
		// Even when no view submits GPU work, streaming visibility needs still
		// have to age out. Without this cleanup pass, assets that were visible
		// on a previous Scene/Game View frame stay marked as active-view after
		// the user hides those views or disables every camera.
		ResourceManager::Instance().RunResidencyCleanup(activeRenderFrameId);
		ClearOverrideCameras();
		return;
	}
	renderFrameSnapshot.renderables.reserve(renderables.size());
	renderFrameSnapshot.meshes.reserve(renderables.size());
	renderFrameSnapshot.materials.reserve(renderables.size());
	for (const RenderableEntry& entry : renderables)
	{
		const Transform* transform = worldTransforms.find(entry.entity);
		TerraEngine::Renderer::VirtualGeometryDrawRange virtualRange{};
		if (const Mesh* mesh = ResourceManager::Instance().GetMesh(entry.mesh.meshId); mesh != nullptr && mesh->hasVirtualGeometry)
		{
			const VirtualGeometryAssetRuntime runtime = BuildVirtualGeometryRuntimeFromMesh(entry.mesh.meshId, *mesh);
			virtualRange = SelectVirtualGeometryResidentLodRange(
				runtime,
				renderFrameSnapshot.renderSettings.virtualGeometry,
				512.0f,
				entry.mesh.taskShaderCullingEnabled,
				!mesh->meshletBounds.empty());
		}
		renderFrameSnapshot.renderables.push_back({
			entry.objectId,
			entry.mesh.meshId,
			entry.mesh.materialId,
			transform ? makeModelMatrix(*transform) : glm::mat4(1.0f),
			virtualRange,
			entry.mesh.taskShaderCullingEnabled,
			false
		});
		// Keep mesh and material records aligned with renderables. Several
		// backend preparation and export diagnostics paths treat this snapshot
		// as a per-renderable contract rather than a unique resource table.
		renderFrameSnapshot.meshes.push_back({ entry.mesh.meshId, "mesh", true, false });
		renderFrameSnapshot.materials.push_back({ entry.mesh.materialId, "material", true, false });
	}
	renderFrameSnapshot.lights.reserve(lights.size());
	for (const LightEntry& light : lights)
	{
		renderFrameSnapshot.lights.push_back({
			light.lightId,
			static_cast<std::uint32_t>(light.light.type),
			light.light.castsShadows,
			false
		});
	}
	TerraEngine::Renderer::SortSnapshotDeterministically(renderFrameSnapshot);
	const std::vector<TerraEngine::Renderer::RenderPassNode> renderGraphPasses = TerraEngine::Renderer::BuildDefaultRenderPasses(renderFrameSnapshot, backendCapabilities);
	const TerraEngine::Renderer::RenderWorkPacketPlan renderPacketPlan = TerraEngine::Renderer::BuildRenderWorkPacketPlan(renderFrameSnapshot, renderGraphPasses, backendCapabilities);
	static TerraEngine::Renderer::Vulkan::VulkanRendererPreparationCache rendererPreparationCache;
	TerraEngine::Renderer::Vulkan::VulkanRendererPreparationSummary rendererPreparationSummary;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Prepare Backend Work Caches", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		rendererPreparationSummary = rendererPreparationCache.BuildFramePreparation(renderFrameSnapshot, renderGraphPasses, backendCapabilities);
	}
	TerraEngine::Renderer::RenderSubmissionPlan renderSubmissionPlan = TerraEngine::Renderer::BuildRenderSubmissionPlan(renderFrameSnapshot, renderGraphPasses, renderPacketPlan, activeRenderFrameId);
	if (!rhi->ConsumeRenderSubmissionPlan(renderSubmissionPlan))
	{
		TERRAENGINE_LOG(LogLevel::Error, "Render graph rejected frame " << activeRenderFrameId << " before backend submission.");
	}
	TerraEngine::Renderer::RenderDiagnosticsSummary renderGraphDiagnostics = TerraEngine::Renderer::BuildRenderDiagnosticsSummary(renderFrameSnapshot, renderPacketPlan, renderSubmissionPlan, backendCapabilities);
	renderGraphDiagnostics.runtimeLayoutRenderRecordCount = cameras.size() + renderables.size() + lights.size();
	renderGraphDiagnostics.runtimeLayoutRejectedRecordCount = runtimeLayoutRejectedRecordCount;
	renderGraphDiagnostics.runtimeLayoutRejectionReasons = runtimeLayoutRejectionReasons;
	if (TerraEngine::Renderer::RenderDiagnosticsRequested() && rhi)
	{
		TerraEngine::Renderer::BackendFrameBreakdown backendBreakdown = rhi->GetLatestBackendFrameBreakdown();
		if (backendBreakdown.frameId == 0u)
		{
			backendBreakdown = TerraEngine::Renderer::BuildUnavailableBackendFrameBreakdown(renderFrameSnapshot);
		}
		TerraEngine::Renderer::ApplyBackendBreakdownToRenderDiagnostics(renderGraphDiagnostics, backendBreakdown);
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Vulkan preparation cache hits: "
			+ std::to_string(rendererPreparationSummary.cacheHitCount)
			+ "/"
			+ std::to_string(rendererPreparationSummary.cacheLookupCount));
		if (rendererPreparationSummary.uploadPlan.queuedForUploadWorker)
		{
			renderGraphDiagnostics.backendFallbacks.push_back("Immutable upload packets are queued for backend worker preparation.");
		}
	}
	const size_t renderGraphWorkerCount = std::max<size_t>(1u, backendCapabilities.maxRecordingWorkers);
	static VisibleCommandCacheState visibleCommandCache;
	static ShadowDrawListCacheState shadowDrawListCache;
	visibleCommandCache.hitCount = 0u;
	visibleCommandCache.missCount = 0u;
	shadowDrawListCache.hitCount = 0u;
	shadowDrawListCache.missCount = 0u;
	const std::uint64_t renderPreparationSettingsSignature = BuildRenderSettingsSignature(renderFrameSnapshot.renderSettings, currentPreset, taskShaderCullingEnabled);

	std::vector<TerraEngine::Scene::SpatialObjectRecord> spatialRecords;
	std::unordered_map<entt::entity, std::size_t> renderableLookup;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Refresh Spatial Index", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		spatialRecords.reserve(renderables.size());
		renderableLookup.reserve(renderables.size());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::size_t index = 0; index < renderables.size(); ++index)
		{
			const RenderableEntry& entry = renderables[index];
			renderableLookup.emplace(entry.entity, index);
			const Transform& transform = getWorldTransform(entry.entity);
			const glm::mat4 model = makeModelMatrix(transform);
			TerraEngine::Scene::SpatialAABB localBounds = ResourceManager::Instance().GetMeshLocalBounds(entry.mesh.meshId);
			TerraEngine::Scene::SpatialAABB worldBounds = localBounds.IsUsable()
				? TerraEngine::Scene::TransformAABB(localBounds, model)
				: TerraEngine::Scene::MakeFallbackBounds(transform.position, 1.0f);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!worldBounds.IsUsable())
			{
				worldBounds = TerraEngine::Scene::MakeFallbackBounds(transform.position, 1.0f);
			}

			TerraEngine::Scene::SpatialObjectRecord record{};
			record.objectId = entry.objectId;
			record.entity = entry.entity;
			record.active = true;
			record.isStatic = entry.isStatic;
			record.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable | TerraEngine::Scene::SpatialCategoryEditorSelectable;
			record.worldBounds = worldBounds;
			record.revision = objManager.GetSpatialRevision();
			spatialRecords.push_back(record);
		}
		spatialIndex.Refresh(spatialRecords);
	}
	TerraEngine::Scene::SpatialDiagnosticsSnapshot latestSpatialRefreshDiagnostics = spatialIndex.GetDiagnosticsSnapshot();
	latestSpatialRefreshDiagnostics.transformDirtyRootCount = transformPropagation.dirtyRootCount;
	latestSpatialRefreshDiagnostics.transformExpandedObjectCount = transformPropagation.expandedDirtyObjectCount;
	latestSpatialRefreshDiagnostics.transformProcessedObjectCount = transformPropagation.processedObjectCount;
	latestSpatialRefreshDiagnostics.transformSkippedCleanObjectCount = transformPropagation.skippedCleanObjectCount;
	latestSpatialRefreshDiagnostics.transformHierarchyLevelCount = transformPropagation.hierarchyLevelCount;
	latestSpatialRefreshDiagnostics.boundsRecomputedObjectCount = static_cast<std::uint32_t>(spatialRecords.size());

	std::vector<DrawCommand> renderableDrawCommands(renderables.size());
	std::vector<std::uint64_t> drawCommandSignatures(renderables.size(), 0u);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!renderables.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		static DrawCommandCacheState drawCommandCache;
		drawCommandCache.hitCount = 0u;
		drawCommandCache.missCount = 0u;
		std::unordered_set<TerraEngine::UUID> aliveDrawCommandObjects;
		aliveDrawCommandObjects.reserve(renderables.size());

		// Workers compute cheap signatures from immutable reflected inputs.
		// The cache map itself is updated later on the coordinator thread so
		// frame-to-frame reuse does not add a lock to every renderable.
		RunRenderChunkedWork(
			renderables.size(),
			renderGraphWorkerCount,
			kRenderParallelThreshold,
			"RenderSystem Task: Hash Draw Command Chunk",
			[&](std::size_t start, std::size_t end)
			{
				for (std::size_t index = start; index < end; ++index)
				{
					const RenderableEntry& entry = renderables[index];
					const MeshRenderComponent& meshComp = entry.mesh;
					drawCommandSignatures[index] = BuildDrawCommandSignature(
						meshComp.meshId,
						meshComp.materialId,
						getWorldTransform(entry.entity),
						meshComp.taskShaderCullingEnabled);
				}
			});
		for (std::size_t index = 0u; index < renderables.size(); ++index)
		{
			const RenderableEntry& entry = renderables[index];
			const MeshRenderComponent& meshComp = entry.mesh;
			aliveDrawCommandObjects.insert(entry.objectId);
			const std::uint64_t signature = drawCommandSignatures[index];
			auto cacheEntry = drawCommandCache.recordsByObjectId.find(entry.objectId);
			if (cacheEntry != drawCommandCache.recordsByObjectId.end() && cacheEntry->second.signature == signature)
			{
				cacheEntry->second.lastTouchedFrameId = activeRenderFrameId;
				renderableDrawCommands[index] = cacheEntry->second.command;
				++drawCommandCache.hitCount;
				continue;
			}

			DrawCommand command{};
			command.meshId = meshComp.meshId;
			command.materialId = meshComp.materialId;
			command.objectId = entry.objectId;
			command.taskShaderCullingEnabled = meshComp.taskShaderCullingEnabled;
			command.modelMatrix = makeModelMatrix(getWorldTransform(entry.entity));
			renderableDrawCommands[index] = command;

			CachedDrawCommandRecord record{};
			record.signature = signature;
			record.lastTouchedFrameId = activeRenderFrameId;
			record.command = command;
			drawCommandCache.recordsByObjectId[entry.objectId] = std::move(record);
			++drawCommandCache.missCount;
		}
		PruneStaleDrawCommandCache(drawCommandCache, aliveDrawCommandObjects);
		if (TerraEngine::Renderer::RenderDiagnosticsRequested())
		{
			renderGraphDiagnostics.backendFallbacks.push_back(
				"Draw command cache hits: "
				+ std::to_string(drawCommandCache.hitCount)
				+ "/"
				+ std::to_string(drawCommandCache.hitCount + drawCommandCache.missCount));
		}
	}
	// The visible and shadow caches store draw commands with model matrices.
	// Include every renderable's current draw signature so Play-mode transform
	// changes invalidate cached view work even when the editor camera is still.
	std::uint64_t renderableWorldSignature = kRenderHashOffset;
	HashCombine(renderableWorldSignature, static_cast<std::uint64_t>(drawCommandSignatures.size()));
	for (std::size_t index = 0u; index < drawCommandSignatures.size(); ++index)
	{
		HashCombine(renderableWorldSignature, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(renderables[index].objectId)));
		HashCombine(renderableWorldSignature, drawCommandSignatures[index]);
	}

	auto collectCandidateIndices = [&](const TerraEngine::Scene::SpatialQueryResult& query) {
		std::vector<std::size_t> candidateIndices;
		candidateIndices.reserve(query.hits.size());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const TerraEngine::Scene::SpatialQueryHit& hit : query.hits)
		{
			if (const auto lookup = renderableLookup.find(hit.entity); lookup != renderableLookup.end())
			{
				candidateIndices.push_back(lookup->second);
			}
		}
		return candidateIndices;
	};

	auto buildDrawCommandsFromCandidateIndices = [&](const std::vector<std::size_t>& candidateIndices) {
		std::vector<DrawCommand> commands;
		commands.reserve(candidateIndices.size());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const std::size_t candidateIndex : candidateIndices)
		{
			commands.push_back(renderableDrawCommands[candidateIndex]);
		}
		return commands;
	};

	std::vector<VirtualGeometryAssetRuntime> gameVirtualGeometryDiagnosticsAssets;
	std::vector<VirtualGeometryAssetRuntime> sceneVirtualGeometryDiagnosticsAssets;
	std::vector<VirtualGeometryAssetRuntime> shadowVirtualGeometryDiagnosticsAssets;
	auto collectVirtualGeometryDiagnosticsAssets = [&](const std::vector<DrawCommand>& commands, RenderViewType viewType) {
		std::vector<VirtualGeometryAssetRuntime>& target = viewType == RenderViewType::Scene
			? sceneVirtualGeometryDiagnosticsAssets
			: (viewType == RenderViewType::Shadow ? shadowVirtualGeometryDiagnosticsAssets : gameVirtualGeometryDiagnosticsAssets);
		for (const DrawCommand& command : commands)
		{
			if (const Mesh* mesh = ResourceManager::Instance().GetMesh(command.meshId); mesh != nullptr && mesh->hasVirtualGeometry)
			{
				target.push_back(BuildVirtualGeometryRuntimeFromMesh(command.meshId, *mesh));
			}
		}
	};

	auto recordVirtualGeometryViewSelection = [&](
		const std::vector<DrawCommand>& commands,
		RenderOutputKind outputKind,
		RenderViewType viewType,
		const glm::uvec2& targetSize,
		std::uint32_t outputId,
		bool shadowPass,
		bool exportRuntime,
		bool hiZOcclusionAllowed) {
		TerraEngine::Renderer::VirtualGeometryViewSelectionState state;
		state.outputKind = outputKind;
		state.viewType = viewType;
		state.targetSize = targetSize;
		state.outputId = outputId;
		state.shadowPass = shadowPass;
		state.exportRuntime = exportRuntime;
		state.hiZOcclusionAllowed = hiZOcclusionAllowed;
		for (const DrawCommand& command : commands)
		{
			TerraEngine::Renderer::VirtualGeometryDrawRange range{};
			range.selectedLodIndex = command.selectedVirtualLod;
			range.meshletOffset = command.virtualMeshletOffset;
			range.meshletCount = command.virtualMeshletCount;
			range.virtualGeometryActive = (command.virtualGeometryFlags & 1u) != 0u;
			range.failOpen = (command.virtualGeometryFlags & 2u) != 0u;
			range.hiZSelfOcclusionGuardPassed = (command.virtualGeometryFlags & 4u) != 0u;
			if (range.failOpen)
			{
				range.fallbackReason = shadowPass
					? "shadow virtual geometry kept conservative visibility"
					: "view virtual geometry failed open; see fallback diagnostics";
			}
			if (!range.virtualGeometryActive && range.meshletCount == 0u && !range.failOpen)
			{
				continue;
			}
			state.draws.push_back({
				command.objectId,
				command.meshId,
				std::move(range),
				command.taskShaderCullingEnabled
			});
		}
		renderFrameSnapshot.virtualGeometryViewSelections.push_back(std::move(state));
	};

	auto applyVirtualGeometrySelection = [&](std::vector<DrawCommand>& commands, RenderViewType viewType, const glm::vec3& cameraPosition, const glm::uvec2& targetSize) {
		for (DrawCommand& command : commands)
		{
			Mesh* mesh = ResourceManager::Instance().GetMesh(command.meshId);
			if (mesh == nullptr || !mesh->hasVirtualGeometry)
			{
				continue;
			}

			const VirtualGeometryAssetRuntime runtime = BuildVirtualGeometryRuntimeFromMesh(command.meshId, *mesh);
			const float projectedRadiusPixels = EstimateProjectedMeshRadiusPixels(*mesh, command.modelMatrix, cameraPosition, targetSize);
			const TerraEngine::Renderer::VirtualGeometryDrawRange range = SelectVirtualGeometryResidentLodRange(
				runtime,
				renderFrameSnapshot.renderSettings.virtualGeometry,
				projectedRadiusPixels,
				command.taskShaderCullingEnabled && taskShaderCullingEnabled,
				!mesh->meshletBounds.empty());

			command.virtualMeshletOffset = range.meshletOffset;
			command.virtualMeshletCount = range.meshletCount;
			command.selectedVirtualLod = range.selectedLodIndex;
			const std::uint32_t debugMode = static_cast<std::uint32_t>(renderFrameSnapshot.renderSettings.virtualGeometry.debugVisualizationMode) & 0xFFu;
			command.virtualGeometryFlags = (range.virtualGeometryActive ? 1u : 0u)
				| (range.failOpen ? 2u : 0u)
				| (range.hiZSelfOcclusionGuardPassed ? 4u : 0u)
				| (debugMode << 8u);
			if (range.failOpen && TerraEngine::Renderer::RenderDiagnosticsRequested())
			{
				TerraEngine::Renderer::AddVirtualGeometryFailOpenDiagnostic(renderGraphDiagnostics, command.meshId, range);
			}
		}
		collectVirtualGeometryDiagnosticsAssets(commands, viewType);
	};

	auto applyShadowVirtualGeometrySelection = [&](std::vector<DrawCommand>& commands) {
		for (DrawCommand& command : commands)
		{
			Mesh* mesh = ResourceManager::Instance().GetMesh(command.meshId);
			if (mesh == nullptr || !mesh->hasVirtualGeometry)
			{
				continue;
			}

			const VirtualGeometryAssetRuntime runtime = BuildVirtualGeometryRuntimeFromMesh(command.meshId, *mesh);
			const TerraEngine::Renderer::VirtualGeometryDrawRange range = SelectVirtualGeometryShadowLodRange(
				runtime,
				renderFrameSnapshot.renderSettings.virtualGeometry,
				!mesh->meshletBounds.empty());

			command.virtualMeshletOffset = range.meshletOffset;
			command.virtualMeshletCount = range.meshletCount;
			command.selectedVirtualLod = range.selectedLodIndex;
			command.virtualGeometryFlags = (range.virtualGeometryActive ? 1u : 0u)
				| (range.failOpen ? 2u : 0u);
		}
		collectVirtualGeometryDiagnosticsAssets(commands, RenderViewType::Shadow);
	};

	auto publishVisibleStreamingNeeds = [&](const std::vector<DrawCommand>& commands, RenderViewType viewType, const glm::vec3& cameraPosition) {
		const char* viewReason = viewType == RenderViewType::Scene ? "visible scene view renderable" : "visible game view renderable";
		for (const DrawCommand& command : commands)
		{
			// Only draw commands that survived the active view frustum query
			// should refresh streaming visibility. Snapshot-wide renderable
			// metadata is not enough, because disabled cameras or offscreen
			// editor views must not keep every scene asset resident.
			ResourceManager::Instance().TouchResource(command.meshId, AssetType::Mesh, activeRenderFrameId, TerraEngine::Resource::ResidencyPinReason::None, viewReason);
			ResourceManager::Instance().TouchResource(command.materialId, AssetType::Material, activeRenderFrameId, TerraEngine::Resource::ResidencyPinReason::None, viewReason);
			ResourceManager::Instance().MarkStreamingNeed(command.meshId, AssetType::Mesh, TerraEngine::Resource::StreamingNeedType::Visible, activeRenderFrameId, viewReason, 100);
			ResourceManager::Instance().MarkStreamingNeed(command.materialId, AssetType::Material, TerraEngine::Resource::StreamingNeedType::Visible, activeRenderFrameId, viewReason, 100);
			if (Material* material = ResourceManager::Instance().GetMaterial(command.materialId))
			{
				const glm::vec3 objectPosition = glm::vec3(command.modelMatrix[3]);
				const double viewDistance = static_cast<double>(glm::length(objectPosition - cameraPosition));
				for (const TextureBinding& binding : material->textures)
				{
					if (binding.textureUuid == TerraEngine::EMPTY_UUID)
					{
						continue;
					}
					ResourceManager::Instance().MarkStreamingNeed(binding.textureUuid, AssetType::Texture, TerraEngine::Resource::StreamingNeedType::Visible, activeRenderFrameId, viewReason, 90);
					ResourceManager::Instance().MarkTextureStreamingDistance(binding.textureUuid, viewDistance, activeRenderFrameId, viewReason);
				}
			}
		}
	};

	auto buildVisibleDrawCommands = [&](const glm::mat4& viewProjection, const glm::uvec2& targetSize, RenderOutputKind outputKind, RenderViewType viewType, std::uint32_t outputId, const glm::vec3& cameraPosition) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Visible Draw Commands Cache Lookup", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const std::uint64_t signature = BuildVisibleCommandCacheKey(
			renderFrameSnapshot.sceneRevision,
			renderPreparationSettingsSignature,
			renderableWorldSignature,
			viewProjection,
			targetSize,
			viewType,
			outputId);
		if (const auto existing = visibleCommandCache.recordsBySignature.find(signature); existing != visibleCommandCache.recordsBySignature.end())
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Visible Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			++visibleCommandCache.hitCount;
			existing->second.lastTouchedFrameId = activeRenderFrameId;
			collectVirtualGeometryDiagnosticsAssets(existing->second.commands, viewType);
			const RenderViewType diagnosticsViewType = outputKind == RenderOutputKind::MainRuntimeView ? RenderViewType::ExportRuntime : viewType;
			recordVirtualGeometryViewSelection(existing->second.commands, outputKind, diagnosticsViewType, targetSize, outputId, false, outputKind == RenderOutputKind::MainRuntimeView, true);
			publishVisibleStreamingNeeds(existing->second.commands, viewType, cameraPosition);
			return VisibleCommandBuildResult{ existing->second.commands, existing->second.diagnostics, true };
		}

		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Rebuild Visible Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const TerraEngine::Scene::SpatialFrustum frustum = TerraEngine::Scene::ExtractFrustum(viewProjection);
		TerraEngine::Scene::SpatialQueryFilter filter{};
		filter.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;
		filter.collectDiagnostics = TerraEngine::Renderer::RenderDiagnosticsRequested();
		const TerraEngine::Scene::SpatialQueryResult query = spatialIndex.QueryFrustum(frustum, filter);
		VisibleCommandBuildResult result;
		result.commands = buildDrawCommandsFromCandidateIndices(collectCandidateIndices(query));
		applyVirtualGeometrySelection(result.commands, viewType, cameraPosition, targetSize);
		const RenderViewType diagnosticsViewType = outputKind == RenderOutputKind::MainRuntimeView ? RenderViewType::ExportRuntime : viewType;
		recordVirtualGeometryViewSelection(result.commands, outputKind, diagnosticsViewType, targetSize, outputId, false, outputKind == RenderOutputKind::MainRuntimeView, true);
		result.diagnostics = latestSpatialRefreshDiagnostics;
		const TerraEngine::Scene::SpatialDiagnosticsSnapshot queryDiagnostics = spatialIndex.GetDiagnosticsSnapshot();
		result.diagnostics.visibleCandidateCount = queryDiagnostics.visibleCandidateCount;
		result.diagnostics.rejectedObjectCount = queryDiagnostics.rejectedObjectCount;
		result.diagnostics.fallbackObjectCount = queryDiagnostics.fallbackObjectCount;
		result.diagnostics.lastQueryDurationMs = queryDiagnostics.lastQueryDurationMs;
		publishVisibleStreamingNeeds(result.commands, viewType, cameraPosition);
		VisibleCommandCacheRecord record;
		record.signature = signature;
		record.lastTouchedFrameId = activeRenderFrameId;
		record.commands = result.commands;
		record.diagnostics = result.diagnostics;
		visibleCommandCache.recordsBySignature[signature] = std::move(record);
		TrimSignatureCache(visibleCommandCache, 32u);
		++visibleCommandCache.missCount;
		return result;
	};

	// Pack reflected lights in parallel so large scenes keep the CPU upload path
	// scalable while preserving deterministic ordering and stable light IDs.
	std::vector<FrameLightCandidate> packedLights;
	if (!lights.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Pack Lights", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		static LightCandidateCacheState lightCandidateCache;
		lightCandidateCache.hitCount = 0u;
		lightCandidateCache.missCount = 0u;
		std::vector<std::uint64_t> lightCandidateSignatures(lights.size(), 0u);
		std::unordered_set<TerraEngine::UUID> aliveLightIds;
		aliveLightIds.reserve(lights.size());

		auto makeLightCandidate = [&](const LightEntry& entry, const Transform& transform) {
			const LightComponent& light = entry.light;
			const glm::quat normalizedRotation = glm::normalize(transform.rotation);
			glm::vec3 direction = normalizedRotation * glm::vec3(0.0f, 0.0f, -1.0f);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z) || glm::length(direction) < 1e-5f)
			{
				direction = glm::vec3(0.0f, -1.0f, 0.0f);
			}
			else
			{
				direction = glm::normalize(direction);
			}

			glm::vec3 finalColor = light.color;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (light.appearance == LightAppearance::FilterAndTemperature)
			{
				finalColor *= LightComponent::KelvinToRGB(light.temperature);
			}

			float clampedIntensity = std::max(light.intensity, 0.0f);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (light.type != LightType::Directional && light.unit == LightUnit::Lumens)
			{
				clampedIntensity /= (4.0f * 3.14159265f); // Convert Lumens to Candela
			}

			const float clampedRange = std::max(light.range, 0.001f);
			const float clampedInnerAngle = std::clamp(light.innerConeAngle, 0.0f, 1.56f);
			const float clampedOuterAngle = std::clamp(std::max(light.outerConeAngle, clampedInnerAngle + 0.001f), 0.0f, 1.57f);

			GPULight gpuLight{};
			gpuLight.positionAndType = glm::vec4(transform.position, static_cast<float>(static_cast<std::uint32_t>(light.type)));
			gpuLight.directionAndRange = glm::vec4(direction, clampedRange);
			gpuLight.colorAndIntensity = glm::vec4(
				std::max(finalColor.x, 0.0f),
				std::max(finalColor.y, 0.0f),
				std::max(finalColor.z, 0.0f),
				clampedIntensity);
			gpuLight.coneAnglesAndArea = glm::vec4(
				std::cos(clampedInnerAngle),
				std::cos(clampedOuterAngle),
				std::max(light.width, 0.001f),
				std::max(light.height, 0.001f));
			gpuLight.shadowAndPadding = glm::vec4(light.castsShadows ? 1.0f : 0.0f, -1.0f, 0.0f, 0.0f);
			gpuLight.extraParams = glm::vec4(
				std::clamp(light.shadowStrength, 0.0f, 1.0f),
				std::max(light.shadowBias, 0.0f),
				std::max(light.indirectMultiplier, 0.0f),
				light.authoredPriority);

			FrameLightCandidate candidate{};
			candidate.lightId = entry.lightId;
			candidate.gpuLight = gpuLight;
			candidate.stabilityPreference = std::max(light.stabilityPreference, 0.0f);
			candidate.candidateFlags = 0u;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (light.castsShadows)
			{
				candidate.candidateFlags |= kFrameLightCandidateFlagShadowCapable;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (light.type == LightType::Area)
			{
				candidate.candidateFlags |= kFrameLightCandidateFlagSpecialCost;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (light.type == LightType::Directional)
			{
				candidate.candidateFlags |= kFrameLightCandidateFlagDirectional;
			}
			return candidate;
		};

		RunRenderChunkedWork(
			lights.size(),
			renderGraphWorkerCount,
			kRenderParallelThreshold,
			"RenderSystem Task: Hash Light Chunk",
			[&](std::size_t start, std::size_t end)
			{
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (std::size_t i = start; i < end; ++i)
				{
					const LightEntry& entry = lights[i];
					lightCandidateSignatures[i] = BuildLightCandidateSignature(getWorldTransform(entry.entity), entry.light);
				}
			});
		packedLights.reserve(lights.size());
		for (std::size_t i = 0u; i < lights.size(); ++i)
		{
			const LightEntry& entry = lights[i];
			aliveLightIds.insert(entry.lightId);
			const std::uint64_t signature = lightCandidateSignatures[i];
			auto cacheEntry = lightCandidateCache.recordsByLightId.find(entry.lightId);
			if (cacheEntry != lightCandidateCache.recordsByLightId.end() && cacheEntry->second.signature == signature)
			{
				cacheEntry->second.lastTouchedFrameId = activeRenderFrameId;
				packedLights.push_back(cacheEntry->second.candidate);
				++lightCandidateCache.hitCount;
				continue;
			}

			CachedLightCandidateRecord record{};
			record.signature = signature;
			record.lastTouchedFrameId = activeRenderFrameId;
			record.candidate = makeLightCandidate(entry, getWorldTransform(entry.entity));
			packedLights.push_back(record.candidate);
			lightCandidateCache.recordsByLightId[entry.lightId] = std::move(record);
			++lightCandidateCache.missCount;
		}
		PruneStaleLightCandidateCache(lightCandidateCache, aliveLightIds);
		if (TerraEngine::Renderer::RenderDiagnosticsRequested())
		{
			renderGraphDiagnostics.backendFallbacks.push_back(
				"Light candidate cache hits: "
				+ std::to_string(lightCandidateCache.hitCount)
				+ "/"
				+ std::to_string(lightCandidateCache.hitCount + lightCandidateCache.missCount));
		}
	}

	auto buildShadowDrawLists = [&](const glm::vec3& cameraWorldPosition) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Shadow Draw Lists Cache Lookup", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const std::uint64_t signature = BuildShadowDrawListCacheKey(
			renderFrameSnapshot.sceneRevision,
			renderPreparationSettingsSignature,
			renderableWorldSignature,
			BuildLightSetSignature(packedLights),
			cameraWorldPosition);
		if (const auto existing = shadowDrawListCache.recordsBySignature.find(signature); existing != shadowDrawListCache.recordsBySignature.end())
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			++shadowDrawListCache.hitCount;
			existing->second.lastTouchedFrameId = activeRenderFrameId;
			std::vector<ShadowDrawList> cachedShadowDrawLists = existing->second.shadowDrawLists;
			for (std::size_t shadowIndex = 0u; shadowIndex < cachedShadowDrawLists.size(); ++shadowIndex)
			{
				collectVirtualGeometryDiagnosticsAssets(cachedShadowDrawLists[shadowIndex].commands, RenderViewType::Shadow);
				recordVirtualGeometryViewSelection(
					cachedShadowDrawLists[shadowIndex].commands,
					RenderOutputKind::OffscreenTexture,
					RenderViewType::Shadow,
					glm::uvec2(renderFrameSnapshot.renderSettings.shadowResolution, renderFrameSnapshot.renderSettings.shadowResolution),
					static_cast<std::uint32_t>(1000u + shadowIndex),
					true,
					false,
					false);
			}
			return cachedShadowDrawLists;
		}

		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Rebuild Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		std::vector<std::optional<ShadowDrawList>> shadowDrawListsByLight(packedLights.size());
		TerraEngine::Scene::SpatialQueryFilter filter{};
		filter.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;

		RunRenderChunkedWork(
			packedLights.size(),
			renderGraphWorkerCount,
			kRenderParallelThreshold,
			"RenderSystem Task: Build Shadow Draw Lists",
			[&](std::size_t start, std::size_t end)
			{
				for (std::size_t lightIndex = start; lightIndex < end; ++lightIndex)
				{
					const FrameLightCandidate& candidate = packedLights[lightIndex];
					if ((candidate.candidateFlags & kFrameLightCandidateFlagShadowCapable) == 0u)
					{
						continue;
					}

					ShadowDrawList shadowDrawList{};
					shadowDrawList.lightId = candidate.lightId;

					const std::uint32_t lightType = static_cast<std::uint32_t>(std::max(candidate.gpuLight.positionAndType.w, 0.0f) + 0.5f);
					TerraEngine::Scene::SpatialQueryResult query{};
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (lightType == static_cast<std::uint32_t>(LightType::Directional))
					{
						const glm::mat4 lightViewProjection = BuildDirectionalShadowViewProjection(candidate.gpuLight, cameraWorldPosition);
						query = spatialIndex.QueryFrustum(TerraEngine::Scene::ExtractFrustum(lightViewProjection), filter);
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (lightType == static_cast<std::uint32_t>(LightType::Point))
					{
						TerraEngine::Scene::SpatialSphere lightBounds{};
						lightBounds.center = glm::vec3(candidate.gpuLight.positionAndType);
						lightBounds.radius = std::max(candidate.gpuLight.directionAndRange.w, 0.5f);
						query = spatialIndex.QuerySphere(lightBounds, filter);
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (lightType == static_cast<std::uint32_t>(LightType::Spot))
					{
						const glm::mat4 lightViewProjection = BuildSpotShadowViewProjection(candidate.gpuLight);
						query = spatialIndex.QueryFrustum(TerraEngine::Scene::ExtractFrustum(lightViewProjection), filter);
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (lightType == static_cast<std::uint32_t>(LightType::Area))
					{
						const glm::mat4 lightViewProjection = BuildAreaShadowViewProjection(candidate.gpuLight);
						query = spatialIndex.QueryFrustum(TerraEngine::Scene::ExtractFrustum(lightViewProjection), filter);
					}

					shadowDrawList.commands = buildDrawCommandsFromCandidateIndices(collectCandidateIndices(query));
					shadowDrawListsByLight[lightIndex] = std::move(shadowDrawList);
				}
			});

		std::vector<ShadowDrawList> shadowDrawLists;
		shadowDrawLists.reserve(shadowDrawListsByLight.size());
		for (std::optional<ShadowDrawList>& shadowDrawList : shadowDrawListsByLight)
		{
			if (shadowDrawList.has_value())
			{
				shadowDrawLists.push_back(std::move(*shadowDrawList));
			}
		}
		for (std::size_t shadowIndex = 0u; shadowIndex < shadowDrawLists.size(); ++shadowIndex)
		{
			applyShadowVirtualGeometrySelection(shadowDrawLists[shadowIndex].commands);
			recordVirtualGeometryViewSelection(
				shadowDrawLists[shadowIndex].commands,
				RenderOutputKind::OffscreenTexture,
				RenderViewType::Shadow,
				glm::uvec2(renderFrameSnapshot.renderSettings.shadowResolution, renderFrameSnapshot.renderSettings.shadowResolution),
				static_cast<std::uint32_t>(1000u + shadowIndex),
				true,
				false,
				false);
		}

		ShadowDrawListCacheRecord record;
		record.signature = signature;
		record.lastTouchedFrameId = activeRenderFrameId;
		record.shadowDrawLists = shadowDrawLists;
		shadowDrawListCache.recordsBySignature[signature] = std::move(record);
		TrimSignatureCache(shadowDrawListCache, 32u);
		++shadowDrawListCache.missCount;
		return shadowDrawLists;
	};

	SkyRuntimeState skyRuntimeState{};
	const SkyComponent* activeSkyComponent = nullptr;
	const LightEntry* activeSkyDirectionalLight = nullptr;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Resolve Sky State", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		ObjectManager& objectManager = ObjectManager::instance();
		std::optional<entt::entity> activeSkyObject = SceneManager::Instance().GetActiveSkyObject();
		std::optional<Object> activeSkyObjectHandle = activeSkyObject.has_value() ? objectManager.getObject(*activeSkyObject) : std::nullopt;
		activeSkyComponent = activeSkyObjectHandle ? objectManager.GetComponent<SkyComponent>(*activeSkyObjectHandle) : nullptr;
		const bool hasActiveSkyComponent = activeSkyComponent != nullptr;
		skyRuntimeState.isValid = true;
		skyRuntimeState.usingFallback = true;
		skyRuntimeState.hasProblem = false;
		skyRuntimeState.dirty = skyStateDirty;
		skyRuntimeState.forceBlackFallback = !hasActiveSkyComponent;
		std::string skyIssueMessage;

		const TerraEngine::UUID skyObjectId = activeSkyObjectHandle ? objectManager.getUUID(*activeSkyObjectHandle) : TerraEngine::EMPTY_UUID;
		const LightEntry* skyObjectDirectionalLight = nullptr;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const LightEntry& lightEntry : lights)
		{
			if (lightEntry.lightId != skyObjectId)
			{
				continue;
			}
			if (lightEntry.light.type == LightType::Directional)
			{
				skyObjectDirectionalLight = &lightEntry;
			}
			break;
		}

		glm::vec3 baseSunDirection = kSkyReferenceSunDirection;
		const LightEntry* resolvedDirectionalLight = nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (hasActiveSkyComponent)
		{
			resolvedDirectionalLight = skyObjectDirectionalLight;
			activeSkyDirectionalLight = resolvedDirectionalLight;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (resolvedDirectionalLight == nullptr)
			{
				skyRuntimeState.forceBlackFallback = true;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (resolvedDirectionalLight != nullptr)
		{
			if (const Transform* lightTransform = worldTransforms.find(resolvedDirectionalLight->entity))
			{
				const glm::quat lightRotation = glm::normalize(lightTransform->rotation);
				const glm::vec3 lightForward = lightRotation * glm::vec3(0.0f, 0.0f, -1.0f);
				baseSunDirection = SafeNormalizeDirection(-lightForward, baseSunDirection);
			}
		}

		const float skyRotationOffset = activeSkyComponent != nullptr ? ClampSkyRotationOffset(activeSkyComponent->rotationOffsetDegrees) : 0.0f;
		skyRuntimeState.resolvedSunDirection = ApplySkyYawOffset(baseSunDirection, skyRotationOffset);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (hasActiveSkyComponent && !skyRuntimeState.forceBlackFallback && !activeSkyComponent->sourceAsset.empty())
		{
			const TerraEngine::UUID resolvedTextureId = ResourceManager::Instance().ResolveAssetReference(activeSkyComponent->sourceAsset, AssetType::Texture);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (resolvedTextureId == TerraEngine::EMPTY_UUID)
			{
				skyRuntimeState.hasProblem = true;
				skyIssueMessage = "Scene sky source could not be resolved. Rendering the fallback background.";
			}
			else
			{
				skyRuntimeState.resolvedSkyHandle = resolvedTextureId;
				skyRuntimeState.usingFallback = false;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skyRuntimeState.hasProblem)
		{
			const std::string sourceAsset = activeSkyComponent != nullptr ? activeSkyComponent->sourceAsset : std::string();
			const std::string issueKey = sourceAsset + "|" + skyIssueMessage;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (issueKey != lastLoggedSkyIssueKey)
			{
				TERRAENGINE_LOG(LogLevel::Warning, "[Sky] " << skyIssueMessage << " Source: " << sourceAsset);
				lastLoggedSkyIssueKey = issueKey;
			}
		}
		else
		{
			lastLoggedSkyIssueKey.clear();
		}
	}

	FrameRenderData frameData;
	frameData.deltaTimeSeconds = deltaTime;
	frameData.globalTaskShaderCullingEnabled = taskShaderCullingEnabled;
	frameData.cullingEpsilon = 0.001f;
	frameData.activePreset = currentPreset;
	frameData.customOverrideActive = (currentPreset == QualityPreset::Custom);
	frameData.settings = SanitizeRenderSettings(currentSettings);
	frameData.sky.renderBackground = true;
	frameData.sky.hasTexture = skyRuntimeState.resolvedSkyHandle != TerraEngine::EMPTY_UUID && !skyRuntimeState.usingFallback;
	frameData.sky.usingFallback = skyRuntimeState.usingFallback;
	frameData.sky.forceBlack = skyRuntimeState.forceBlackFallback;
	frameData.sky.intensity = activeSkyDirectionalLight != nullptr ? std::max(activeSkyDirectionalLight->light.intensity, 0.0f) : 1.0f;
	frameData.sky.exposure = activeSkyComponent != nullptr ? ClampSkyExposure(activeSkyComponent->exposure) : 1.0f;
	frameData.sky.rotationOffsetDegrees = activeSkyComponent != nullptr ? ClampSkyRotationOffset(activeSkyComponent->rotationOffsetDegrees) : 0.0f;
	frameData.sky.horizonTint = activeSkyComponent != nullptr ? ClampSkyTint(activeSkyComponent->horizonTint) : frameData.sky.horizonTint;
	frameData.sky.zenithTint = activeSkyComponent != nullptr ? ClampSkyTint(activeSkyComponent->zenithTint) : frameData.sky.zenithTint;
	frameData.sky.groundTint = activeSkyComponent != nullptr ? ClampSkyTint(activeSkyComponent->groundTint) : frameData.sky.groundTint;
	frameData.sky.sunDirection = skyRuntimeState.resolvedSunDirection;
	frameData.sky.textureId = skyRuntimeState.resolvedSkyHandle;
	frameData.lightCandidates = std::move(packedLights);
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Fetch UI Batch", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		frameData.uiBatch = UISystem::Instance().GetCombinedFrameBatch();
	}

	rhi->SetGlobalTaskShaderCullingEnabled(taskShaderCullingEnabled);
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: RHI Begin Frame", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		rhi->BeginFrame();
	}

	TerraEngine::Scene::SpatialDiagnosticsSnapshot sceneSpatialDiagnostics{};
	TerraEngine::Scene::SpatialDiagnosticsSnapshot gameSpatialDiagnostics{};
	// Convert each camera submission into an explicit output owner so the RHI
	// can give Scene, Game, and future offscreen cameras independent snapshot
	// ranges instead of inferring ownership from call order alone.
	auto makeOutputRequest = [&](RenderOutputKind kind, RenderViewType diagnosticsView, std::uint32_t outputId) {
		RenderOutputRequest request{};
		request.kind = kind;
		request.priority = RenderOutputPriority::Required;
		request.diagnosticsView = diagnosticsView;
		// Editor windows supply panel-sized targets so Vulkan records offscreen
		// passes against the exact texture rectangle ImGui will present.
		if (kind == RenderOutputKind::SceneView)
		{
			request.targetSize = sceneViewTargetSize;
		}
		else if (kind == RenderOutputKind::GameView)
		{
			request.targetSize = gameViewTargetSize;
		}
		request.outputId = outputId;
		return request;
	};
	auto applySceneViewContextTint = [&]() {
		if (!sceneViewContextTintEnabled || sceneViewContextFocusedObjectIds.empty())
		{
			return;
		}

		// Prefab context mode keeps the opened prefab branch in normal material
		// color and gently washes every surrounding scene command toward white.
		// This runs only on the Scene View command list before submission.
		const glm::vec4 contextTint(1.0f, 1.0f, 1.0f, 0.25f);
		frameData.editorContextTint = contextTint;
		for (DrawCommand& command : frameData.commands)
		{
			command.editorTint = sceneViewContextFocusedObjectIds.contains(command.objectId)
				? glm::vec4(0.0f)
				: contextTint;
		}
	};
	const bool sceneViewTargetReady = !editorModeEnabled || (sceneViewTargetSize.x > 0u && sceneViewTargetSize.y > 0u);
	const bool gameViewTargetReady = !editorModeEnabled || (gameViewTargetSize.x > 0u && gameViewTargetSize.y > 0u);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hasSceneViewCamera && sceneViewTargetReady)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Scene View", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		frameData.viewMatrix = sceneViewCamera.view;
		frameData.projectionMatrix = sceneViewCamera.proj;
		frameData.cameraWorldPosition = sceneViewCamera.pos;
		const VisibleCommandBuildResult visibleCommands = buildVisibleDrawCommands(frameData.projectionMatrix * frameData.viewMatrix, sceneViewTargetSize, RenderOutputKind::SceneView, RenderViewType::Scene, 1u, frameData.cameraWorldPosition);
		frameData.commands = visibleCommands.commands;
		sceneSpatialDiagnostics = visibleCommands.diagnostics;
		applySceneViewContextTint();
		frameData.shadowDrawLists = buildShadowDrawLists(frameData.cameraWorldPosition);
		rhi->RenderFrame(frameData, makeOutputRequest(RenderOutputKind::SceneView, RenderViewType::Scene, 1u));
	}
	else if (TerraEngine::Renderer::RenderDiagnosticsRequested())
	{
		renderGraphDiagnostics.delayedWork.push_back(
			hasSceneViewCamera
				? "Scene View skipped because its render target is not ready."
				: "Scene View skipped because the editor did not provide a scene camera this frame.");
	}

	// Asset-style prefab editing has no active runtime scene. When the editor
	// requests suppression, skip every Game View submission path so opening a
	// prefab asset cannot burn a full offscreen game render every frame.
	if (!gameViewSuppressed && gameViewTargetReady && !gameViewCameras.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Game View Cameras", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& cam : gameViewCameras)
		{
			frameData.editorContextTint = glm::vec4(0.0f);
			frameData.viewMatrix = cam.view;
			frameData.projectionMatrix = cam.proj;
			frameData.cameraWorldPosition = cam.pos;
			const VisibleCommandBuildResult visibleCommands = buildVisibleDrawCommands(frameData.projectionMatrix * frameData.viewMatrix, gameViewTargetSize, RenderOutputKind::GameView, RenderViewType::Game, 2u, frameData.cameraWorldPosition);
			frameData.commands = visibleCommands.commands;
			gameSpatialDiagnostics = visibleCommands.diagnostics;
			frameData.shadowDrawLists = buildShadowDrawLists(frameData.cameraWorldPosition);
			rhi->RenderFrame(frameData, makeOutputRequest(RenderOutputKind::GameView, RenderViewType::Game, 2u));
		}
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (!gameViewSuppressed && gameViewTargetReady && cameras.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Blank Game View", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		FrameRenderData blankData{};
		blankData.viewMatrix = glm::mat4(1.0f);
		blankData.projectionMatrix = glm::mat4(1.0f);
		blankData.cameraWorldPosition = glm::vec3(0.0f);
		blankData.deltaTimeSeconds = deltaTime;
		blankData.globalTaskShaderCullingEnabled = taskShaderCullingEnabled;
			blankData.cullingEpsilon = 0.001f;
			blankData.activePreset = currentPreset;
			blankData.customOverrideActive = (currentPreset == QualityPreset::Custom);
			blankData.settings = SanitizeRenderSettings(currentSettings);
			blankData.sky = frameData.sky;
			blankData.editorContextTint = glm::vec4(0.0f);
			blankData.commands.clear();
			blankData.shadowDrawLists.clear();
			rhi->RenderFrame(blankData, makeOutputRequest(RenderOutputKind::GameView, RenderViewType::Game, 2u));
	}
	else if (!gameViewSuppressed && gameViewTargetReady)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Runtime Cameras", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t cameraIndex = 0; cameraIndex < cameras.size(); ++cameraIndex)
		{
			const entt::entity cameraEntity = cameras[cameraIndex].entity;
			const auto& camera = cameras[cameraIndex].camera;
			const Transform* propagatedCameraTransform = worldTransforms.find(cameraEntity);
			Transform cameraTransform = propagatedCameraTransform != nullptr ? *propagatedCameraTransform : Transform{};

			// Normalize the rotation so derived direction vectors remain unit length
			// even if authoring data drifts slightly off the identity quaternion.
			const glm::quat cameraRotation = glm::normalize(cameraTransform.rotation);
			const glm::vec3 defaultForward(0.0f, 0.0f, -1.0f);
			const glm::vec3 forward = cameraRotation * defaultForward;
			glm::vec3 up = cameraRotation * camera.up;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (glm::length(up) < 0.0001f)
			{
				up = glm::vec3(0.0f, 1.0f, 0.0f);
			}

			// Clamp projection inputs to keep glm::perspective from receiving invalid values.
			const float clampedFov = std::max(0.001f, camera.verticalFovDegrees);
			const float safeAspect = camera.aspectRatio > 0.0f ? camera.aspectRatio : 1.0f;
			const float nearPlane = std::max(0.0001f, camera.nearPlane);
			const float farPlane = std::max(nearPlane + 0.001f, camera.farPlane);
			frameData.viewMatrix = glm::lookAt(cameraTransform.position, cameraTransform.position + forward, glm::normalize(up));
			frameData.projectionMatrix = glm::perspective(glm::radians(clampedFov), safeAspect, nearPlane, farPlane);
			frameData.projectionMatrix[1][1] *= -1;
			frameData.cameraWorldPosition = cameraTransform.position;
			frameData.editorContextTint = glm::vec4(0.0f);
			const VisibleCommandBuildResult visibleCommands = buildVisibleDrawCommands(frameData.projectionMatrix * frameData.viewMatrix, gameViewTargetSize, RenderOutputKind::MainRuntimeView, RenderViewType::Game, 2u, frameData.cameraWorldPosition);
			frameData.commands = visibleCommands.commands;
			gameSpatialDiagnostics = visibleCommands.diagnostics;
			frameData.shadowDrawLists = buildShadowDrawLists(frameData.cameraWorldPosition);

			rhi->RenderFrame(frameData, makeOutputRequest(RenderOutputKind::MainRuntimeView, RenderViewType::Game, 2u));
		}
	}
	else if (TerraEngine::Renderer::RenderDiagnosticsRequested() && gameViewSuppressed)
	{
		renderGraphDiagnostics.delayedWork.push_back("Game View skipped because the editor suppressed it for prefab asset editing.");
	}
	else if (TerraEngine::Renderer::RenderDiagnosticsRequested() && !gameViewTargetReady)
	{
		renderGraphDiagnostics.delayedWork.push_back("Game View skipped because its render target is not ready.");
	}
	if (TerraEngine::Renderer::RenderDiagnosticsRequested())
	{
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Visible draw command cache hits: "
			+ std::to_string(visibleCommandCache.hitCount)
			+ "/"
			+ std::to_string(visibleCommandCache.hitCount + visibleCommandCache.missCount));
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Shadow draw list cache hits: "
			+ std::to_string(shadowDrawListCache.hitCount)
			+ "/"
			+ std::to_string(shadowDrawListCache.hitCount + shadowDrawListCache.missCount));
		renderGraphDiagnostics.backendFallbacks.push_back(
			"Persistent render packet cache hits: "
			+ std::to_string(rendererPreparationSummary.renderPacketCache.cacheHits)
			+ "/"
			+ std::to_string(rendererPreparationSummary.renderPacketCache.packetCount));
	}
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: RHI End Frame", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		rhi->EndFrame();
	}

	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Publish Render Stats", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> virtualGeometryViewSnapshots;
		const VirtualGeometryQualityProfile virtualGeometryQuality = renderFrameSnapshot.renderSettings.virtualGeometry;
		virtualGeometryViewSnapshots.push_back(TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Game, activeRenderFrameId, gameVirtualGeometryDiagnosticsAssets, virtualGeometryQuality));
		virtualGeometryViewSnapshots.push_back(TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Scene, activeRenderFrameId, sceneVirtualGeometryDiagnosticsAssets, virtualGeometryQuality));
		bool shadowDiagnosticsPublished = false;
		{
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot shadowSnapshot =
				TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Shadow, activeRenderFrameId, shadowVirtualGeometryDiagnosticsAssets, virtualGeometryQuality);
			shadowSnapshot.hiZActive = false;
			shadowSnapshot.hiZHistoryValid = false;
			shadowSnapshot.hiZInvalidReason = "shadow views never consume camera Hi-Z history";
			shadowSnapshot.warnings.push_back("Shadow virtual geometry uses conservative visibility and disables camera Hi-Z occlusion.");
			virtualGeometryViewSnapshots.push_back(std::move(shadowSnapshot));
			shadowDiagnosticsPublished = true;
		}
		std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> selectionSnapshots =
			TerraEngine::Renderer::BuildVirtualGeometryViewDiagnosticsFromSnapshot(renderFrameSnapshot, virtualGeometryQuality, GetVirtualGeometryInactiveReason());
		for (TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& selectionSnapshot : selectionSnapshots)
		{
			if (selectionSnapshot.viewType == RenderViewType::ExportRuntime
				|| (selectionSnapshot.viewType == RenderViewType::Shadow && !shadowDiagnosticsPublished))
			{
				virtualGeometryViewSnapshots.push_back(std::move(selectionSnapshot));
			}
		}
		const TerraEngine::Scheduling::SystemDeclaration schedulerDeclaration = SchedulerDeclaration();
		std::vector<TerraEngine::Renderer::VirtualGeometrySchedulerDiagnostics> virtualGeometrySchedulerSnapshots;
		virtualGeometrySchedulerSnapshots.push_back(TerraEngine::Renderer::BuildVirtualGeometrySchedulerDiagnostics(
			schedulerDeclaration.name,
			schedulerDeclaration.phase,
			schedulerDeclaration.reads,
			schedulerDeclaration.writes,
			schedulerDeclaration.safeSubwork,
			schedulerDeclaration.protectedCommit,
			schedulerDeclaration.requiresFlushedSceneMutations
				? "SceneMutationQueue flush before render snapshot reads; virtual geometry diagnostics publish after protected RHI work."
				: "Virtual geometry diagnostics publish after protected RHI work."));
		TerraEngine::Renderer::ApplyVirtualGeometryDiagnostics(renderGraphDiagnostics, std::move(virtualGeometryViewSnapshots), std::move(virtualGeometrySchedulerSnapshots));
		TerraEngine::Renderer::PublishRenderDiagnostics(renderGraphDiagnostics);
		std::lock_guard<std::mutex> lock(statsMutex);
		latestGameStats = rhi->GetLatestRenderStats(RenderViewType::Game);
		latestSceneStats = rhi->GetLatestRenderStats(RenderViewType::Scene);
		latestGameStats.spatialIndexedObjectCount = gameSpatialDiagnostics.indexedObjectCount;
		latestGameStats.spatialVisibleCandidateCount = gameSpatialDiagnostics.visibleCandidateCount;
		latestGameStats.spatialRejectedObjectCount = gameSpatialDiagnostics.rejectedObjectCount;
		latestGameStats.spatialFallbackObjectCount = gameSpatialDiagnostics.fallbackObjectCount;
		latestGameStats.spatialRefreshMode = gameSpatialDiagnostics.refreshMode;
		latestGameStats.spatialAddedObjectCount = gameSpatialDiagnostics.addedObjectCount;
		latestGameStats.spatialUpdatedObjectCount = gameSpatialDiagnostics.updatedObjectCount;
		latestGameStats.spatialRemovedObjectCount = gameSpatialDiagnostics.removedObjectCount;
		latestGameStats.spatialUnchangedObjectCount = gameSpatialDiagnostics.unchangedObjectCount;
		latestGameStats.spatialFullRefreshObjectCount = gameSpatialDiagnostics.fullRefreshObjectCount;
		latestGameStats.transformDirtyRootCount = gameSpatialDiagnostics.transformDirtyRootCount;
		latestGameStats.transformProcessedObjectCount = gameSpatialDiagnostics.transformProcessedObjectCount;
		latestGameStats.boundsRecomputedObjectCount = gameSpatialDiagnostics.boundsRecomputedObjectCount;
		latestSceneStats.spatialIndexedObjectCount = sceneSpatialDiagnostics.indexedObjectCount;
		latestSceneStats.spatialVisibleCandidateCount = sceneSpatialDiagnostics.visibleCandidateCount;
		latestSceneStats.spatialRejectedObjectCount = sceneSpatialDiagnostics.rejectedObjectCount;
		latestSceneStats.spatialFallbackObjectCount = sceneSpatialDiagnostics.fallbackObjectCount;
		latestSceneStats.spatialRefreshMode = sceneSpatialDiagnostics.refreshMode;
		latestSceneStats.spatialAddedObjectCount = sceneSpatialDiagnostics.addedObjectCount;
		latestSceneStats.spatialUpdatedObjectCount = sceneSpatialDiagnostics.updatedObjectCount;
		latestSceneStats.spatialRemovedObjectCount = sceneSpatialDiagnostics.removedObjectCount;
		latestSceneStats.spatialUnchangedObjectCount = sceneSpatialDiagnostics.unchangedObjectCount;
		latestSceneStats.spatialFullRefreshObjectCount = sceneSpatialDiagnostics.fullRefreshObjectCount;
		latestSceneStats.transformDirtyRootCount = sceneSpatialDiagnostics.transformDirtyRootCount;
		latestSceneStats.transformProcessedObjectCount = sceneSpatialDiagnostics.transformProcessedObjectCount;
		latestSceneStats.boundsRecomputedObjectCount = sceneSpatialDiagnostics.boundsRecomputedObjectCount;
		latestGameSpatialDiagnostics = gameSpatialDiagnostics;
		latestSceneSpatialDiagnostics = sceneSpatialDiagnostics;
		latestGameBudgetDiagnostics = rhi->GetLatestFrameBudgetDiagnostics(RenderViewType::Game);
		latestSceneBudgetDiagnostics = rhi->GetLatestFrameBudgetDiagnostics(RenderViewType::Scene);
		latestGameLightingDiagnostics = rhi->GetLatestLightingDiagnostics(RenderViewType::Game);
		latestSceneLightingDiagnostics = rhi->GetLatestLightingDiagnostics(RenderViewType::Scene);
		latestRenderSnapshotDiagnostics = rhi->GetLatestRenderSnapshotDiagnostics();
		latestRenderGraphDiagnostics = renderGraphDiagnostics;
		latestRenderFrameSnapshot = renderFrameSnapshot;
		latestSkyRuntimeState = skyRuntimeState;
		TerraEngine::Profiling::Profiler::Instance().RecordRenderStats(latestGameStats, latestSceneStats);
	}

	skyStateDirty = false;

	ResourceManager::Instance().RunResidencyCleanup(activeRenderFrameId);

	ClearOverrideCameras();
}

// RenderSystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::Shutdown()
{
	if (rhi) {
		rhi->Shutdown();
		rhi.reset();
	}
}

// RenderSystem::SystemName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string RenderSystem::SystemName()
{
	return "RenderSystem";
}

TerraEngine::Scheduling::SystemDeclaration RenderSystem::SchedulerDeclaration()
{
	TerraEngine::Scheduling::SystemDeclaration declaration;
	declaration.name = "RenderSystem";
	declaration.phase = "Render";
	declaration.reads = { "Window", "SceneGraph", "ObjectManager", "UI", "Resources", "VirtualGeometryQualityProfile", "VirtualGeometryResidencySnapshots" };
	declaration.writes = { "Renderer", "VirtualGeometryDiagnostics" };
	// Live ECS collection still runs protected inside RenderSystem, while draw,
	// light, upload, and backend packet preparation consume immutable snapshots
	// virtual geometry LOD/page intent, and visibility diagnostics consume
	// immutable snapshots and are safe for the frame workload migration layer to
	// classify as worker eligible system work. Live GPU submission is not
	// transactional because it owns swapchain/fence side effects; scene-owned
	// render metadata edits must be prepared as SceneMutationQueue transactions
	// before this boundary.
	declaration.mainThread = false;
	declaration.requiresFlushedSceneMutations = true;
	declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare;
	declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe;
	declaration.hardBoundary = true;
	declaration.safeSubwork = "Render snapshots, virtual geometry LOD/page preparation, resource readiness, draw packets, secondary command buffers, and submit descriptions.";
	declaration.protectedCommit = "Swapchain acquire, queue submit, present, frame fence/semaphore ownership, virtual geometry diagnostics publication, editor present, and recovery.";
	declaration.protectedReason = "Live GPU queue and presentation ownership remain protected after worker preparation.";
	declaration.profilingLabel = declaration.name;
	declaration.usesDefaultMetadata = false;
	return declaration;
}

TerraEngine::Scheduling::HardBoundary RenderSystem::GetLiveGpuSubmissionBoundary() const
{
	for (const TerraEngine::Scheduling::HardBoundary& boundary : TerraEngine::Scheduling::InitialWorkerHardBoundaries())
	{
		if (boundary.boundaryName == "LiveGpuSubmission")
		{
			return boundary;
		}
	}
	return {};
}

std::vector<TerraEngine::Scene::RenderRuntimeRecord> RenderSystem::BuildCompactRenderRuntimeRecords()
{
	std::vector<TerraEngine::Scene::RenderRuntimeRecord> records;
	objManager.forEachObjectWithComponents<Transform, CameraComponent>(
		[&](Object object, Transform&, CameraComponent&)
		{
			TerraEngine::Scene::RenderRuntimeRecord record;
			record.recordKind = TerraEngine::Scene::RuntimeLayoutRecordKind::Camera;
			record.entity = object.EntityId();
			record.objectId = objManager.getUUID(object);
			record.sourceRevision = objManager.GetObjectSpatialRevision(object);
			records.push_back(std::move(record));
		});

	objManager.forEachObjectWithComponents<Transform, MeshRenderComponent>(
		[&](Object object, Transform&, MeshRenderComponent& mesh)
		{
			TerraEngine::Scene::RenderRuntimeRecord record;
			record.recordKind = TerraEngine::Scene::RuntimeLayoutRecordKind::Renderable;
			record.entity = object.EntityId();
			record.objectId = mesh.spatialObjectId != TerraEngine::EMPTY_UUID ? mesh.spatialObjectId : objManager.getUUID(object);
			record.resourceIds = { mesh.meshId, mesh.materialId };
			record.renderFlags = mesh.taskShaderCullingEnabled ? 1u : 0u;
			record.sourceRevision = objManager.GetObjectSpatialRevision(object);
			if (mesh.meshId == TerraEngine::EMPTY_UUID || mesh.materialId == TerraEngine::EMPTY_UUID)
			{
				record.rejectionReason = "missing-render-resource-id";
			}
			records.push_back(std::move(record));
		});

	objManager.forEachObjectWithComponents<Transform, LightComponent>(
		[&](Object object, Transform&, LightComponent& light)
		{
			TerraEngine::Scene::RenderRuntimeRecord record;
			record.recordKind = TerraEngine::Scene::RuntimeLayoutRecordKind::Light;
			record.entity = object.EntityId();
			record.objectId = objManager.getUUID(object);
			record.renderFlags = light.castsShadows ? 1u : 0u;
			record.sourceRevision = objManager.GetObjectSpatialRevision(object);
			records.push_back(std::move(record));
		});

	return records;
}

bool RenderSystem::CompactRenderRecordsMatchSnapshot(const std::vector<TerraEngine::Scene::RenderRuntimeRecord>& records, const TerraEngine::Renderer::RenderFrameSnapshot& snapshot)
{
	std::size_t renderableCount = 0u;
	std::size_t lightCount = 0u;
	for (const TerraEngine::Scene::RenderRuntimeRecord& record : records)
	{
		if (!record.rejectionReason.empty())
		{
			continue;
		}
		if (record.recordKind == TerraEngine::Scene::RuntimeLayoutRecordKind::Renderable)
		{
			++renderableCount;
		}
		else if (record.recordKind == TerraEngine::Scene::RuntimeLayoutRecordKind::Light)
		{
			++lightCount;
		}
	}
	return renderableCount == snapshot.renderables.size() && lightCount == snapshot.lights.size();
}

TerraEngine::Renderer::VirtualGeometryDrawRange RenderSystem::SelectVirtualGeometryLodRange(
	const VirtualGeometryAssetRuntime& asset,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels,
	bool taskShaderCullingEnabled,
	bool boundsValid)
{
	TerraEngine::Renderer::VirtualGeometryDrawRange range{};
	if (asset.detailLevels.empty())
	{
		range.failOpen = true;
		range.fallbackReason = "missing virtual geometry detail metadata";
		return range;
	}

	const VirtualGeometryQualityProfile sanitized = SanitizeVirtualGeometryQualityProfile(quality);
	const auto ApplyDetail = [&asset, &range](std::size_t detailIndex) {
		const VirtualGeometryDetailRuntime& detail = asset.detailLevels[detailIndex];
		range.selectedLodIndex = detail.lodIndex;
		range.meshletOffset = detail.meshletOffset;
		range.meshletCount = detail.meshletCount;
	};

	ApplyDetail(0u);
	if (!taskShaderCullingEnabled)
	{
		range.failOpen = true;
		range.fallbackReason = "task shader culling disabled; using highest-detail fallback";
		return range;
	}
	if (!boundsValid)
	{
		range.failOpen = true;
		range.fallbackReason = "invalid meshlet bounds; using highest-detail fallback";
		return range;
	}

	range.virtualGeometryActive = true;
	range.hiZSelfOcclusionGuardPassed = sanitized.hiZSelfOcclusionGuardEnabled;
	std::size_t desiredIndex = 0u;
	const float finiteProjectedRadiusPixels = std::isfinite(projectedRadiusPixels)
		? std::max(projectedRadiusPixels, 0.0f)
		: 512.0f;
	constexpr float kFirstVirtualGeometryLodTransitionPixels = 480.0f;
	constexpr float kVirtualGeometryLodTransitionScale = 0.5f;
	// Walk every cooked detail level instead of using two fixed buckets. Each
	// lower LOD becomes valid when the mesh covers roughly half as much screen
	// radius as the previous transition, which makes LOD visualizer colors move
	// at ordinary editor camera distances while still keeping nearby meshes sharp.
	for (std::size_t detailIndex = 1u; detailIndex < asset.detailLevels.size(); ++detailIndex)
	{
		const float transitionPixels = kFirstVirtualGeometryLodTransitionPixels
			* std::pow(kVirtualGeometryLodTransitionScale, static_cast<float>(detailIndex - 1u));
		if (finiteProjectedRadiusPixels >= transitionPixels)
		{
			break;
		}
		desiredIndex = detailIndex;
	}

	if (sanitized.maxLodDrop > 0u)
	{
		desiredIndex = std::min<std::size_t>(desiredIndex, sanitized.maxLodDrop);
	}

	while (desiredIndex > 0u)
	{
		const VirtualGeometryDetailRuntime& detail = asset.detailLevels[desiredIndex];
		const float scaledError = detail.geometricError * sanitized.lodErrorScale;
		if (std::isfinite(scaledError) && scaledError <= 0.1f)
		{
			break;
		}
		--desiredIndex;
	}

	ApplyDetail(desiredIndex);
	return range;
}

TerraEngine::Renderer::VirtualGeometryDrawRange RenderSystem::SelectVirtualGeometryResidentLodRange(
	const VirtualGeometryAssetRuntime& asset,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels,
	bool taskShaderCullingEnabled,
	bool boundsValid)
{
	TerraEngine::Renderer::VirtualGeometryDrawRange range = SelectVirtualGeometryLodRange(
		asset,
		quality,
		projectedRadiusPixels,
		taskShaderCullingEnabled,
		boundsValid);
	const auto applyDetail = [&asset, &range](std::size_t detailIndex) {
		const VirtualGeometryDetailRuntime& detail = asset.detailLevels[detailIndex];
		range.selectedLodIndex = detail.lodIndex;
		range.meshletOffset = detail.meshletOffset;
		range.meshletCount = detail.meshletCount;
	};

	if (range.failOpen || !range.virtualGeometryActive || asset.pages.empty() || asset.detailLevels.empty())
	{
		return range;
	}

	const std::size_t desiredIndex = DetailIndexForLod(asset, range.selectedLodIndex);
	std::string fallbackReason;
	const std::size_t residentIndex = ResolveResidentVirtualGeometryDetailIndex(asset, desiredIndex, fallbackReason);
	if (residentIndex != desiredIndex)
	{
		applyDetail(residentIndex);
		range.failOpen = true;
		range.fallbackReason = fallbackReason;
	}
	return range;
}

TerraEngine::Renderer::VirtualGeometryDrawRange RenderSystem::SelectVirtualGeometryShadowLodRange(
	const VirtualGeometryAssetRuntime& asset,
	const VirtualGeometryQualityProfile& quality,
	bool boundsValid)
{
	VirtualGeometryQualityProfile shadowQuality = SanitizeVirtualGeometryQualityProfile(quality);
	shadowQuality.maxLodDrop = 0u;
	shadowQuality.hiZOcclusionEnabled = false;
	shadowQuality.hiZSelfOcclusionGuardEnabled = false;

	TerraEngine::Renderer::VirtualGeometryDrawRange range = SelectVirtualGeometryResidentLodRange(
		asset,
		shadowQuality,
		1024.0f,
		true,
		boundsValid);
	range.hiZSelfOcclusionGuardPassed = false;
	if (range.virtualGeometryActive)
	{
		range.selectedLodIndex = asset.detailLevels.empty() ? range.selectedLodIndex : asset.detailLevels.front().lodIndex;
		range.meshletOffset = asset.detailLevels.empty() ? range.meshletOffset : asset.detailLevels.front().meshletOffset;
		range.meshletCount = asset.detailLevels.empty() ? range.meshletCount : asset.detailLevels.front().meshletCount;
	}
	if (!range.failOpen)
	{
		range.fallbackReason = "shadow pass uses conservative virtual geometry visibility without camera Hi-Z";
	}
	return range;
}

// RenderSystem::ReloadMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::ReloadMaterial(const TerraEngine::UUID& materialId)
{
	ResourceManager::Instance().ReloadMaterial(materialId);
	// Surface runtime-contract fallbacks during hot reload so artists can see
	// when a material is rendering with authored defaults instead of textures.
	if (const Material* material = ResourceManager::Instance().GetMaterial(materialId))
	{
		for (const std::string& warning : material->fallbackWarnings)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Material " << TerraEngine::ToHexString(materialId) << ": " << warning);
		}
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (rhi)
	{
		rhi->InvalidateMaterial(materialId);
	}
}

// RenderSystem::ReloadMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::ReloadMesh(const TerraEngine::UUID& meshId)
{
	ResourceManager::Instance().ReloadMesh(meshId);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (rhi)
	{
		rhi->InvalidateMesh(meshId);
	}
}

// RenderSystem::ReloadTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::ReloadTexture(const TerraEngine::UUID& textureId)
{
	auto future = ResourceManager::Instance().ReloadTexture(textureId);
	// Wait for the background transcode to finish before we swap the GPU resource
	future.get();

	if (rhi)
	{
		rhi->InvalidateTexture(textureId);
	}
}

// RenderSystem::DeleteResource is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::DeleteResource(const TerraEngine::UUID& id)
{
	if (rhi)
	{
		rhi->WaitIdle();
		rhi->InvalidateResource(id);
	}
}

void RenderSystem::RetireLoadedResource(const TerraEngine::UUID& id, std::uint64_t frameId, std::string_view debugName)
{
	if (rhi)
	{
		// Residency cleanup is a cache-pressure path, not an editor delete.
		// Backends can defer physical destruction until submitted frames no
		// longer reference the resource instead of forcing a device-wide idle.
		rhi->RetireLoadedResource(id, frameId, debugName);
	}
}

// RenderSystem::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::Clear()
{
	if (rhi)
	{
		rhi->ClearResources();
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestGameStats = {};
		latestSceneStats = {};
		latestGameBudgetDiagnostics = {};
		latestSceneBudgetDiagnostics = {};
		latestGameLightingDiagnostics = {};
		latestSceneLightingDiagnostics = {};
		latestRenderSnapshotDiagnostics = {};
		latestRenderGraphDiagnostics = {};
		latestRenderFrameSnapshot = {};
		latestGameStats.viewType = RenderViewType::Game;
		latestSceneStats.viewType = RenderViewType::Scene;
		latestGameBudgetDiagnostics.viewType = RenderViewType::Game;
		latestSceneBudgetDiagnostics.viewType = RenderViewType::Scene;
		latestGameLightingDiagnostics.viewType = RenderViewType::Game;
		latestSceneLightingDiagnostics.viewType = RenderViewType::Scene;
		latestSkyRuntimeState = {};
	}
	skyStateDirty = true;
	lastLoggedSkyIssueKey.clear();
	ClearOverrideCameras();
}

// RenderSystem::SetTaskShaderCullingEnabled is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::SetTaskShaderCullingEnabled(bool enabled)
{
	taskShaderCullingEnabled = enabled;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (rhi)
	{
		rhi->SetGlobalTaskShaderCullingEnabled(enabled);
	}
}

// RenderSystem::GetLatestRenderStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderStatsSnapshot RenderSystem::GetLatestRenderStats(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneStats;
	}
	return latestGameStats;
}

// RenderSystem::GetLatestSpatialDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::Scene::SpatialDiagnosticsSnapshot RenderSystem::GetLatestSpatialDiagnostics(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneSpatialDiagnostics;
	}
	return latestGameSpatialDiagnostics;
}

// RenderSystem::GetLatestFrameBudgetDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
FrameBudgetDiagnosticsSnapshot RenderSystem::GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneBudgetDiagnostics;
	}
	return latestGameBudgetDiagnostics;
}

// RenderSystem::GetLatestLightingDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
LightingDiagnosticsSnapshot RenderSystem::GetLatestLightingDiagnostics(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneLightingDiagnostics;
	}
	return latestGameLightingDiagnostics;
}

// RenderSystem::GetLatestRenderSnapshotDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderSnapshotDiagnosticsSnapshot RenderSystem::GetLatestRenderSnapshotDiagnostics() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestRenderSnapshotDiagnostics;
}

TerraEngine::Renderer::RenderDiagnosticsSummary RenderSystem::GetLatestRenderGraphDiagnostics() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestRenderGraphDiagnostics;
}

void RenderSystem::SetRenderGraphDiagnosticsRequested(bool requested)
{
	TerraEngine::Renderer::SetRenderDiagnosticsRequested(requested);
	if (!requested)
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestRenderGraphDiagnostics = {};
	}
}

// RenderSystem::GetLatestSkyRuntimeState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
SkyRuntimeState RenderSystem::GetLatestSkyRuntimeState() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestSkyRuntimeState;
}

// RenderSystem::MarkSkyStateDirty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::MarkSkyStateDirty()
{
	skyStateDirty = true;
	lastLoggedSkyIssueKey.clear();
}

// RenderSystem::SetQualityPreset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::SetQualityPreset(QualityPreset preset)
{
	if (preset == QualityPreset::Custom)
	{
		currentPreset = QualityPreset::Custom;
		return;
	}

	currentPreset = preset;
	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (preset)
	{
	case QualityPreset::Low:
		currentSettings.shadowResolution = 512u;
		currentSettings.shadowFilteringSize = 1u;
		currentSettings.maxUploadedLights = 64u;
		currentSettings.maxDynamicLights = 32u;
		currentSettings.maxShadowLights = 1u;
		currentSettings.maxAreaLights = 0u;
		currentSettings.enableLTC = 0u;
		currentSettings.enableIBL = 0u;
		currentSettings.indirectStrength = 0.35f;
		currentSettings.ambientOcclusionStrength = 0.5f;
		currentSettings.ambientOcclusionQuality = 0u;
		currentSettings.anisotropicFiltering = 1u;
		currentSettings.variableRateShading = 0u;
		break;
	case QualityPreset::Medium:
		currentSettings.shadowResolution = 1024u;
		currentSettings.shadowFilteringSize = 3u;
		currentSettings.maxUploadedLights = 128u;
		currentSettings.maxDynamicLights = 64u;
		currentSettings.maxShadowLights = 2u;
		currentSettings.maxAreaLights = 2u;
		currentSettings.enableLTC = 1u;
		currentSettings.enableIBL = 1u;
		currentSettings.indirectStrength = 0.75f;
		currentSettings.ambientOcclusionStrength = 0.75f;
		currentSettings.ambientOcclusionQuality = 1u;
		currentSettings.anisotropicFiltering = 1u;
		currentSettings.variableRateShading = 0u;
		break;
	case QualityPreset::High:
		currentSettings.shadowResolution = 2048u;
		currentSettings.shadowFilteringSize = 5u;
		currentSettings.maxUploadedLights = 256u;
		currentSettings.maxDynamicLights = 128u;
		currentSettings.maxShadowLights = 3u;
		currentSettings.maxAreaLights = 4u;
		currentSettings.enableLTC = 1u;
		currentSettings.enableIBL = 1u;
		currentSettings.indirectStrength = 1.0f;
		currentSettings.ambientOcclusionStrength = 1.0f;
		currentSettings.ambientOcclusionQuality = 2u;
		currentSettings.anisotropicFiltering = 4u;
		currentSettings.variableRateShading = 0u;
		break;
	case QualityPreset::Ultra:
		currentSettings.shadowResolution = 4096u;
		currentSettings.shadowFilteringSize = 7u;
		currentSettings.maxUploadedLights = 512u;
		currentSettings.maxDynamicLights = 256u;
		currentSettings.maxShadowLights = 8u;
		currentSettings.maxAreaLights = 8u;
		currentSettings.enableLTC = 1u;
		currentSettings.enableIBL = 1u;
		currentSettings.indirectStrength = 1.25f;
		currentSettings.ambientOcclusionStrength = 1.25f;
		currentSettings.ambientOcclusionQuality = 3u;
		currentSettings.anisotropicFiltering = 16u;
		currentSettings.variableRateShading = 0u;
		break;
	}

	currentSettings = SanitizeRenderSettings(currentSettings);
	SaveSettings();
}

// RenderSystem::SetRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::SetRenderSettings(const RenderSettings& settings)
{
	currentSettings = SanitizeRenderSettings(settings);
	currentPreset = QualityPreset::Custom;
	if (auto* vulkanContext = dynamic_cast<VulkanContext*>(rhi.get()))
	{
		vulkanContext->SetVariableRateShadingEnabled(currentSettings.variableRateShading > 0u);
	}
	SaveSettings();
}

void RenderSystem::SetVirtualGeometryQualityProfile(const VirtualGeometryQualityProfile& profile)
{
	currentSettings.virtualGeometry = SanitizeVirtualGeometryQualityProfile(profile);
	currentPreset = QualityPreset::Custom;
	SaveSettings();
}

VirtualGeometryQualityProfile RenderSystem::GetVirtualGeometryQualityProfile() const
{
	return SanitizeVirtualGeometryQualityProfile(currentSettings.virtualGeometry);
}

VirtualGeometryInactiveReason RenderSystem::GetVirtualGeometryInactiveReason() const
{
	const bool meshShaderRouteSupported = rhi != nullptr && rhi->SupportsVirtualGeometryMeshShaderRoute();
	return ResolveVirtualGeometryInactiveReason(currentSettings.virtualGeometry, meshShaderRouteSupported);
}

// RenderSystem::LoadSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::LoadSettings()
{
	namespace fs = std::filesystem;
	fs::path settingsPath = ResolveRuntimeSettingsDirectory() / "RenderSettings.json";

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fs::exists(settingsPath))
	{
		const fs::path legacySettingsPath = ResolveLegacyAssetSettingsPath();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!legacySettingsPath.empty() && fs::exists(legacySettingsPath))
		{
			settingsPath = legacySettingsPath;
		}
		else
		{
			SetQualityPreset(QualityPreset::High);
			return;
		}
	}

	try
	{
		std::ifstream file(settingsPath);
		nlohmann::json j;
		file >> j;

		currentPreset = static_cast<QualityPreset>(j.value("preset", 2u));
		
		auto s = j["settings"];
		currentSettings.shadowResolution = s.value("shadowResolution", 2048u);
		currentSettings.shadowFilteringSize = s.value("shadowFilteringSize", 5u);
		currentSettings.maxUploadedLights = s.value("maxUploadedLights", 256u);
		currentSettings.maxDynamicLights = s.value("maxDynamicLights", 128u);
		currentSettings.maxShadowLights = s.value("maxShadowLights", 8u);
		currentSettings.maxAreaLights = s.value("maxAreaLights", 4u);
		currentSettings.enableLTC = s.value("enableLTC", 1u);
		currentSettings.enableIBL = s.value("enableIBL", 1u);
		currentSettings.indirectStrength = s.value("indirectStrength", 1.0f);
		currentSettings.ambientOcclusionStrength = s.value("ambientOcclusionStrength", 1.0f);
		currentSettings.ambientOcclusionQuality = s.value("ambientOcclusionQuality", 2u);
		currentSettings.anisotropicFiltering = s.value("anisotropicFiltering", 4u);
		currentSettings.variableRateShading = s.value("variableRateShading", 0u);
		if (s.contains("virtualGeometry") && s["virtualGeometry"].is_object())
		{
			const auto& virtualGeometry = s["virtualGeometry"];
			currentSettings.virtualGeometry.lodErrorScale = virtualGeometry.value("lodErrorScale", 1.0f);
			currentSettings.virtualGeometry.maxLodDrop = virtualGeometry.value("maxLodDrop", 0u);
			currentSettings.virtualGeometry.hiZOcclusionEnabled = virtualGeometry.value("hiZOcclusionEnabled", false);
			currentSettings.virtualGeometry.hiZSelfOcclusionGuardEnabled = virtualGeometry.value("hiZSelfOcclusionGuardEnabled", true);
			currentSettings.virtualGeometry.debugVisualizationMode = static_cast<VirtualGeometryDebugVisualizationMode>(
				virtualGeometry.value("debugVisualizationMode", static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::None)));
			currentSettings.virtualGeometry.geometryBudgetBytes = virtualGeometry.value("geometryBudgetBytes", static_cast<std::uint64_t>(0u));
			currentSettings.virtualGeometry.preloadBias = virtualGeometry.value("preloadBias", 1.0f);
		}
		currentSettings = SanitizeRenderSettings(currentSettings);
	}
	catch (const std::exception& e)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to load RenderSettings: " << e.what());
		SetQualityPreset(QualityPreset::High);
	}
}

// RenderSystem::SaveSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::SaveSettings()
{
	namespace fs = std::filesystem;
	const fs::path settingsDir = ResolveRuntimeSettingsDirectory();
	const fs::path settingsPath = settingsDir / "RenderSettings.json";

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fs::exists(settingsDir))
	{
		fs::create_directories(settingsDir);
	}

	try
	{
		nlohmann::json j;
		j["preset"] = static_cast<uint32_t>(currentPreset);
		
		nlohmann::json s;
		s["shadowResolution"] = currentSettings.shadowResolution;
		s["shadowFilteringSize"] = currentSettings.shadowFilteringSize;
		s["maxUploadedLights"] = currentSettings.maxUploadedLights;
		s["maxDynamicLights"] = currentSettings.maxDynamicLights;
		s["maxShadowLights"] = currentSettings.maxShadowLights;
		s["maxAreaLights"] = currentSettings.maxAreaLights;
		s["enableLTC"] = currentSettings.enableLTC;
		s["enableIBL"] = currentSettings.enableIBL;
		s["indirectStrength"] = currentSettings.indirectStrength;
		s["ambientOcclusionStrength"] = currentSettings.ambientOcclusionStrength;
		s["ambientOcclusionQuality"] = currentSettings.ambientOcclusionQuality;
		s["anisotropicFiltering"] = currentSettings.anisotropicFiltering;
		s["variableRateShading"] = currentSettings.variableRateShading;
		nlohmann::json virtualGeometry;
		const VirtualGeometryQualityProfile virtualGeometryProfile = SanitizeVirtualGeometryQualityProfile(currentSettings.virtualGeometry);
		virtualGeometry["lodErrorScale"] = virtualGeometryProfile.lodErrorScale;
		virtualGeometry["maxLodDrop"] = virtualGeometryProfile.maxLodDrop;
		virtualGeometry["hiZOcclusionEnabled"] = virtualGeometryProfile.hiZOcclusionEnabled;
		virtualGeometry["hiZSelfOcclusionGuardEnabled"] = virtualGeometryProfile.hiZSelfOcclusionGuardEnabled;
		virtualGeometry["debugVisualizationMode"] = static_cast<std::uint32_t>(virtualGeometryProfile.debugVisualizationMode);
		virtualGeometry["geometryBudgetBytes"] = virtualGeometryProfile.geometryBudgetBytes;
		virtualGeometry["preloadBias"] = virtualGeometryProfile.preloadBias;
		s["virtualGeometry"] = virtualGeometry;
		
		j["settings"] = s;

		std::ofstream file(settingsPath);
		file << j.dump(4);
	}
	catch (const std::exception& e)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to save RenderSettings: " << e.what());
	}
}
