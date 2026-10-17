#include "Core/Logger/Logger.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include "Resource/VirtualGeometryStreamingSystem.h"
#include "RenderFrameCapture.h"
#include "RenderPreparationTasks.h"
#include "Renderer/Graph/RenderFrameScratchValidation.h"
#include "Components/LightComponent.h"
#include <glm/gtc/quaternion.hpp>
// Typed light snapshot and sky preparation, preserving authored collection order.
#include "RenderFrameState.h"

void RenderSystem::AppendSnapshotLightRows(const std::pmr::vector<LightEntry>& lights,
	TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot)
{
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
}

namespace TerraEngine::Renderer::Frame {
	std::uint64_t BuildLightCandidateSignature(
		const TerraEngine::Scene::WorldPose& transform,
		const LightComponent& light)
	{
		// The packed light cache key covers transform and every authoring field
		// used to produce the GPULight payload. The renderer can reuse the
		// previous candidate until one of those inputs changes.
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(
			TerraEngine::Scene::WorldPositionHash{}(transform.position)));
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
	void PruneStaleLightCandidateCache(LightCandidateCacheState& cache, const std::unordered_set<TerraEngine::UUID>& aliveLightIds)
	{
		for (auto iterator = cache.recordsByLightId.begin(); iterator != cache.recordsByLightId.end();)
		{
			iterator = aliveLightIds.contains(iterator->first)
				? std::next(iterator)
				: cache.recordsByLightId.erase(iterator);
		}
	}
}

// Candidate hashing borrows copied scratch lights; all admitted ranges settle before returning the owned vector.
std::vector<FrameLightCandidate> RenderSystem::PrepareFrameLightCandidates(
	std::span<const LightEntry> lights, TerraEngine::Renderer::Frame::LightCandidateCacheState& cache,
	const FrameWorldTransforms& worldCapture, std::size_t renderGraphWorkerCount,
	TerraEngine::Renderer::RenderDiagnosticsSummary& renderGraphDiagnostics,
	const std::shared_ptr<const TerraEngine::Renderer::RenderFrameScratchValidationCallbacks>& scratchValidation,
	const TerraEngine::Renderer::RenderFrameScratchDiagnostics& capturedScratchDiagnostics)
{
	// Pack reflected lights in parallel so large scenes keep the CPU upload path
	// scalable while preserving deterministic ordering and stable light IDs.
	std::vector<FrameLightCandidate> packedLights;
	if (!lights.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Pack Lights", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		LightCandidateCacheState& lightCandidateCache = cache;
		lightCandidateCache.hitCount = 0u;
		lightCandidateCache.missCount = 0u;
		std::vector<std::uint64_t> lightCandidateSignatures(lights.size(), 0u);
		std::unordered_set<TerraEngine::UUID> aliveLightIds;
		aliveLightIds.reserve(lights.size());

		auto makeLightCandidate = [&](const LightEntry& entry,
			const TerraEngine::Scene::WorldPose& transform) {
			const LightComponent& light = entry.light;
			const glm::quat normalizedRotation = glm::normalize(transform.rotation);
			glm::vec3 direction = normalizedRotation * glm::vec3(0.0f, 0.0f, -1.0f);

			if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z) || glm::length(direction) < 1e-5f)
			{
				direction = glm::vec3(0.0f, -1.0f, 0.0f);
			}
			else
			{
				direction = glm::normalize(direction);
			}

			glm::vec3 finalColor = light.color;

			if (light.appearance == LightAppearance::FilterAndTemperature)
			{
				finalColor *= LightComponent::KelvinToRGB(light.temperature);
			}

			float clampedIntensity = std::max(light.intensity, 0.0f);

			if (light.type != LightType::Directional && light.unit == LightUnit::Lumens)
			{
				clampedIntensity /= (4.0f * 3.14159265f); // Convert Lumens to Candela
			}

			const float clampedRange = std::max(light.range, 0.001f);
			const float clampedInnerAngle = std::clamp(light.innerConeAngle, 0.0f, 1.56f);
			const float clampedOuterAngle = std::clamp(std::max(light.outerConeAngle, clampedInnerAngle + 0.001f), 0.0f, 1.57f);

			GPULight gpuLight{};
			// Translation is localized per view after light selection. Shared light
			// candidates never store an absolute float position.
			gpuLight.positionAndType = glm::vec4(
				glm::vec3(0.0F),
				static_cast<float>(static_cast<std::uint32_t>(light.type)));
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
			candidate.canonicalPosition = transform.position;
			candidate.gpuLight = gpuLight;
			candidate.stabilityPreference = std::max(light.stabilityPreference, 0.0f);
			candidate.candidateFlags = 0u;

			if (light.castsShadows)
			{
				candidate.candidateFlags |= kFrameLightCandidateFlagShadowCapable;
			}

			if (light.type == LightType::Area)
			{
				candidate.candidateFlags |= kFrameLightCandidateFlagSpecialCost;
			}

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
				const auto observeRange = [&](TerraEngine::Renderer::RenderFrameScratchValidationCheckpoint checkpoint) {
					if(!scratchValidation || !scratchValidation->onEvent) return;
					TerraEngine::Renderer::RenderFrameScratchValidationEvent event;
					event.checkpoint = checkpoint;
					event.episode = capturedScratchDiagnostics;
					event.start = start; event.end = end;
					event.workerLimit = renderGraphWorkerCount;
					event.rangeDigest = 14695981039346656037ULL;
					TerraEngine::Renderer::AppendScratchValidationWord(event.rangeDigest, start, 8U);
					TerraEngine::Renderer::AppendScratchValidationWord(event.rangeDigest, end, 8U);
					event.computedSignatureDigest = 14695981039346656037ULL;
					for(std::size_t i = start; i < end; ++i) {
						TerraEngine::Renderer::AppendScratchLightValidationDigest(event.rangeDigest, lights[i].lightId, lights[i].light);
						if(checkpoint == TerraEngine::Renderer::RenderFrameScratchValidationCheckpoint::LightReadEnd)
							TerraEngine::Renderer::AppendScratchValidationWord(event.computedSignatureDigest, lightCandidateSignatures[i], 8U);
					}
					scratchValidation->onEvent(event);
				};
				observeRange(TerraEngine::Renderer::RenderFrameScratchValidationCheckpoint::LightReadBegin);

				for (std::size_t i = start; i < end; ++i)
				{
					const LightEntry& entry = lights[i];
					lightCandidateSignatures[i] = BuildLightCandidateSignature(worldCapture.Get(entry.entity), entry.light);
				}
				observeRange(TerraEngine::Renderer::RenderFrameScratchValidationCheckpoint::LightReadEnd);
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
			record.candidate = makeLightCandidate(entry, worldCapture.Get(entry.entity));
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

	return packedLights;
}

namespace {
	const glm::vec3 kSkyReferenceSunDirection = glm::normalize(glm::vec3(0.0f, 0.35f, -1.0f));
	glm::vec3 SafeNormalizeDirection(const glm::vec3& direction, const glm::vec3& fallback)
	{
		if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z))
		{
			return fallback;
		}

		const float length = glm::length(direction);

		if (length <= 0.0001f)
		{
			return fallback;
		}

		return direction / length;
	}
	glm::vec3 ApplySkyYawOffset(const glm::vec3& direction, float degrees)
	{
		const glm::quat yawRotation = glm::angleAxis(glm::radians(degrees), glm::vec3(0.0f, 1.0f, 0.0f));
		return SafeNormalizeDirection(yawRotation * direction, kSkyReferenceSunDirection);
	}
}

// Resolve copied sky/light owners and frame upload values before the existing view boundary.
RenderViewBuildState RenderSystem::PrepareFrameHeader(std::span<const LightEntry> lights,
	std::vector<FrameLightCandidate>& packedLights, TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	const FrameSkyCapture& skyCapture, const FrameWorldTransforms& worldCapture,
	const FrameBackendObservations& observations, float deltaTime, SkyRuntimeState& skyRuntimeState)
{
	const auto& capturedSkyComponent = skyCapture.capturedSkyComponent;
	const auto& capturedSkyObjectId = skyCapture.capturedSkyObjectId;
	const auto& capturedSkyTextureId = skyCapture.capturedSkyTextureId;
	const auto* worldTransforms = worldCapture.worldTransforms;
	const auto& capturedUiBatch = observations.capturedUiBatch;
	const auto& capturedVirtualGeometryStreamingPlan = observations.capturedVirtualGeometryStreamingPlan;
	const SkyComponent* activeSkyComponent = capturedSkyComponent.has_value()
		? std::addressof(*capturedSkyComponent) : nullptr;
	const LightEntry* activeSkyDirectionalLight = nullptr;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Resolve Sky State", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const bool hasActiveSkyComponent = activeSkyComponent != nullptr;
		skyRuntimeState.isValid = true;
		skyRuntimeState.usingFallback = true;
		skyRuntimeState.hasProblem = false;
		skyRuntimeState.dirty = skyStateDirty;
		skyRuntimeState.forceBlackFallback = !hasActiveSkyComponent;
		std::string skyIssueMessage;

		const TerraEngine::UUID skyObjectId = capturedSkyObjectId;
		const LightEntry* skyObjectDirectionalLight = nullptr;

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

		if (hasActiveSkyComponent)
		{
			resolvedDirectionalLight = skyObjectDirectionalLight;
			activeSkyDirectionalLight = resolvedDirectionalLight;

			if (resolvedDirectionalLight == nullptr)
			{
				skyRuntimeState.forceBlackFallback = true;
			}
		}


		if (resolvedDirectionalLight != nullptr)
		{
			if (const auto lightTransform = worldTransforms->find(resolvedDirectionalLight->entity); lightTransform != worldTransforms->end())
			{
				const glm::quat lightRotation = glm::normalize(lightTransform->second.rotation);
				const glm::vec3 lightForward = lightRotation * glm::vec3(0.0f, 0.0f, -1.0f);
				baseSunDirection = SafeNormalizeDirection(-lightForward, baseSunDirection);
			}
		}

		const float skyRotationOffset = activeSkyComponent != nullptr ? ClampSkyRotationOffset(activeSkyComponent->rotationOffsetDegrees) : 0.0f;
		skyRuntimeState.resolvedSunDirection = ApplySkyYawOffset(baseSunDirection, skyRotationOffset);


		if (hasActiveSkyComponent && !skyRuntimeState.forceBlackFallback && !activeSkyComponent->sourceAsset.empty())
		{
			const TerraEngine::UUID resolvedTextureId = capturedSkyTextureId;

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


		if (skyRuntimeState.hasProblem)
		{
			const std::string sourceAsset = activeSkyComponent != nullptr ? activeSkyComponent->sourceAsset : std::string();
			const std::string issueKey = sourceAsset + "|" + skyIssueMessage;

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

	RenderViewBuildState frameData;
	frameData.frameId = activeRenderFrameId;
	frameData.deltaTimeSeconds = deltaTime;
	frameData.globalTaskShaderCullingEnabled = taskShaderCullingEnabled;
	frameData.cullingEpsilon = 0.001f;
	frameData.activePreset = currentPreset;
	frameData.customOverrideActive = (currentPreset == QualityPreset::Custom);
	frameData.settings = SanitizeRenderSettings(currentSettings);
	// Reorder only owned captured candidates and copied settings. The backend's
	// ordinary ApplyFrameLightBudgets consumes these genuine inputs for each view.
	if (renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied
		&& !TerraEngine::Renderer::ApplyRendererEfficiencyFixtureLightingControls(renderFrameSnapshot.efficiencyFixtureIntent,
			packedLights, frameData.settings, renderFrameSnapshot.efficiencyFixtureIntent.controlReason))
		renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied = false;
	frameData.efficiencyOptions = renderFrameSnapshot.efficiencyOptions;
	frameData.efficiencyFixtureIntent = renderFrameSnapshot.efficiencyFixtureIntent;
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
		frameData.uiBatch = capturedUiBatch;
	}
	{
		// StreamingPrepare completed before this render workload. Copy only
		// integrity-checked decoded values into the immutable frame payload;
		// Vulkan still must upload them and observe its frame fence before the
		// protected streaming system may publish Resident state.
		for (const TerraEngine::Resource::VirtualGeometryStreamingDecodedPage& decoded
			: capturedVirtualGeometryStreamingPlan.decodedPages)
		{
			if (decoded.status != TerraEngine::Resource::VirtualGeometryStreamingDecodeStatus::PendingUpload
				|| decoded.decodedBytes.empty())
			{
				continue;
			}

			VirtualGeometryDecodedPageUploadRecord upload{};
			upload.assetId = decoded.request.assetId;
			upload.pageId = decoded.request.pageId;
			upload.fallbackPageId = decoded.request.fallbackPageId;
			upload.clusterId = decoded.request.clusterId;
			upload.dependencyPageIds = decoded.request.dependencyPageIds;
			upload.publicationGeneration = std::max<std::uint64_t>(activeRenderFrameId, 1u);
			// Exact canonical page totals are mandatory before PendingUpload, so
			// the immutable backend record never substitutes decoded bytes for a
			// missing stored-byte count.
			upload.storedByteCount = decoded.request.expectedStoredByteCount;
			upload.decodedByteCount = decoded.decodedBytes.size();
			upload.integrityValue = decoded.request.expectedIntegrityValue;
			upload.decodedBytes = decoded.decodedBytes;
			frameData.virtualGeometryDecodedPageUploads.push_back(std::move(upload));
		}
	}

	return frameData;
}
