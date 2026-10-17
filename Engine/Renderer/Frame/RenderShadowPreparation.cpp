#include "RenderViewPreparation.h"
#include "RenderPreparationTasks.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include <glm/gtc/matrix_transform.hpp>
// Immutable caster evidence is copied from captured owners without reopening ECS/resources.
#include "RenderFrameState.h"
#include "RenderFrameCapture.h"
#include "RenderSnapshotPreparation.h"
#include "Resource/ResourceManager.h"
#include "Resource/ShaderCollection.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"

void RenderSystem::CaptureSnapshotShadowEvidence(const RenderableEntry& entry,
	std::size_t renderableIndex, const Mesh* mesh,
	const TerraEngine::Renderer::Frame::CanonicalGeometryRouteFacts& routeFacts,
	TerraEngine::Renderer::RenderFrameRenderableRecord& snapshotRenderable,
	const FrameSnapshotShadowView& inputs)
{
	const auto& binding = entry.animationBinding;
	const auto& capturedShadowObjectLifetimes = inputs.capturedShadowObjectLifetimes;
	const auto& frameMaterialResources = inputs.frameMaterialResources;
	const auto& capturedDefaultShaderCollectionId = inputs.resourceCohort.capturedDefaultShaderCollectionId;
	const auto& capturedDefaultTaskShaderCollectionId = inputs.resourceCohort.capturedDefaultTaskShaderCollectionId;
	const auto& capturedShadowMeshCollection = inputs.resourceCohort.capturedShadowMeshCollection;
	const auto& capturedShadowTaskCollection = inputs.resourceCohort.capturedShadowTaskCollection;
	const auto capturedRendererResourceRevision = inputs.resourceCohort.capturedRendererResourceRevision;
	const auto rendererResourceOwnerCohortStable = inputs.resourceCohort.rendererResourceOwnerCohortStable;
	// Worker assembly consumes only identity values and shared shader owners
	// sealed above. It must never reopen live ECS or loader state after suspend.
	try
	{
		auto shadowInputs = std::make_shared<TerraEngine::Renderer::ShadowCasterStamp>();
		shadowInputs->objectId = entry.objectId;
		shadowInputs->lifetimeGeneration = capturedShadowObjectLifetimes[renderableIndex];
		shadowInputs->meshId = entry.mesh->meshId;
		shadowInputs->assetId = entry.terrainAssetId != TerraEngine::EMPTY_UUID
			? entry.terrainAssetId : entry.mesh->meshId;
		shadowInputs->materialId = entry.mesh->materialId;
		const auto& shadowPose = snapshotRenderable.canonicalWorldPose;
		shadowInputs->worldPosition.cell = {shadowPose.position.cell.x, shadowPose.position.cell.y, shadowPose.position.cell.z};
		shadowInputs->worldPosition.offsetMetres = {shadowPose.position.offsetMetres.x, shadowPose.position.offsetMetres.y, shadowPose.position.offsetMetres.z};
		shadowInputs->rotation = {shadowPose.rotation.w, shadowPose.rotation.x, shadowPose.rotation.y, shadowPose.rotation.z};
		shadowInputs->scale = {shadowPose.scale.x, shadowPose.scale.y, shadowPose.scale.z};
		shadowInputs->staticEligible = entry.isStatic;
		// Authoring is one input to static admission. Backend preparation must
		// independently prove the resources and shader actually used by each draw.
		shadowInputs->staticSupport.authoredStatic = entry.isStatic;
		using ShadowProof = TerraEngine::Renderer::ShadowCasterEvidence;
		const auto addShadowProof = [&](ShadowProof proof) { shadowInputs->evidenceMask |= static_cast<std::uint64_t>(proof); };
		if (shadowInputs->lifetimeGeneration != 0U) addShadowProof(ShadowProof::Identity);
		addShadowProof(ShadowProof::Transform);
		// This is the actual resource publication counter, not an identity hash.
		// One broad counter intentionally over-invalidates when an unrelated asset
		// changes; it safely covers payload, material, shader and texture commits.
		const auto shadowResourceRevision = rendererResourceOwnerCohortStable ? inputs.frameResourceRevision : 0;
		if (mesh != nullptr && shadowResourceRevision != 0U)
		{
			shadowInputs->meshRevision = shadowResourceRevision;
			shadowInputs->assetRevision = shadowResourceRevision;
			addShadowProof(ShadowProof::Geometry);
			if (routeFacts.directUsable)
			{
				// Canonical complete owner geometry is retained even when visibility
				// chose a VG route. Current shadows read that whole owner, not streamed
				// pages; backend recording still must prove its actual buffers ready.
				// Sparse VG page packets require their own selected publication proof.
				shadowInputs->pageContentRevision = shadowResourceRevision;
				shadowInputs->payloadContentRevision = shadowResourceRevision;
				shadowInputs->residencyGeneration = shadowResourceRevision;
				addShadowProof(ShadowProof::Residency);
			}
		}
		const auto shadowMaterialEntry = frameMaterialResources.find(entry.mesh->materialId);
		const Material* shadowMaterial = shadowMaterialEntry != frameMaterialResources.end() ? shadowMaterialEntry->second : nullptr;
		if (shadowMaterial != nullptr && shadowResourceRevision != 0U)
		{
			shadowInputs->materialRevision = shadowResourceRevision;
			shadowInputs->shadowBehavior = static_cast<std::uint64_t>(shadowMaterial->shadowBehavior)
				| (static_cast<std::uint64_t>(shadowMaterial->header.alphaMode) << 32U)
				| (static_cast<std::uint64_t>(shadowMaterial->header.flags) << 40U);
			shadowInputs->materialShadowParameters = {shadowMaterial->header.alphaCutoff,
				shadowMaterial->header.baseColorFactor[3], 0.0, 0.0};
			addShadowProof(ShadowProof::Material);
			const auto authoredShader = shadowMaterial->header.shaderCollectionId;
			const bool knownDefault = authoredShader == TerraEngine::EMPTY_UUID
				|| authoredShader == capturedDefaultShaderCollectionId || authoredShader == capturedDefaultTaskShaderCollectionId;
			shadowInputs->shaderId = authoredShader == TerraEngine::EMPTY_UUID ? capturedDefaultTaskShaderCollectionId : authoredShader;
			shadowInputs->shaderRevision = shadowResourceRevision;
			// An authored surface can discard or displace with hidden/time-varying
			// inputs. Its UUID/revision alone does not prove an unchanged silhouette.
			const auto& shadowCollection = shadowInputs->shaderId == capturedDefaultTaskShaderCollectionId
				? capturedShadowTaskCollection : capturedShadowMeshCollection;
			if (knownDefault && shadowInputs->shaderId != TerraEngine::EMPTY_UUID
				&& shadowCollection != nullptr && shadowCollection->collectionGeneration != 0)
			{
				shadowInputs->shaderRevision = shadowCollection->collectionGeneration;
				addShadowProof(ShadowProof::Shader);
				shadowInputs->staticSupport.knownDefaultShadowAbi = true;
			}
			// Default opaque shadow texture samples cannot clip the depth silhouette.
			// Cutout and other texture-dependent silhouettes remain unknown until exact sampling
			// inputs are explicitly supported, forcing the correct full refresh.
			shadowInputs->textureRevision = shadowResourceRevision;
			if (knownDefault && shadowMaterial->header.alphaMode == static_cast<std::uint32_t>(AlphaMode::Opaque))
				addShadowProof(ShadowProof::Texture);
		}
		const bool knownNondeformable = mesh != nullptr && !mesh->hasDeformablePayload;
		// Bind pose on a deformable mesh remains dynamic: a later animation
		// callback can change it without changing the author's static flag.
		shadowInputs->staticSupport.deformationAbsent = knownNondeformable
			&& !binding.HasCpuDeformation() && !binding.gpu.IsComplete();
		const bool knownBindPose = mesh != nullptr && binding.meshId == entry.mesh->meshId
			&& binding.deformablePayloadGeneration == mesh->deformablePayloadGeneration
			&& binding.source == TerraEngine::Renderer::AnimationRenderPoseSource::BindPose
			&& binding.status == TerraEngine::Renderer::AnimationRenderBindingStatus::Ready;
		if (knownNondeformable || knownBindPose)
			addShadowProof(ShadowProof::Deformation); // Explicit known bind-pose zero.
		else if (!binding.gpu.IsComplete() && binding.HasCpuDeformation()
			&& binding.cpuDeformation->jointCount == binding.cpuDeformation->paletteMatrices.size())
		{
			const auto& cpu = *binding.cpuDeformation;
			shadowInputs->animationInstanceId = BuildStableAnimationInstanceId(binding, entry.objectId);
			shadowInputs->deformationGeneration = binding.deformablePayloadGeneration;
			shadowInputs->deformationContentIdLow = cpu.contentIdLow;
			shadowInputs->deformationContentIdHigh = cpu.contentIdHigh;
			shadowInputs->deformationPalette.reserve(cpu.paletteMatrices.size());
			for (const auto& palette : cpu.paletteMatrices)
			{
				std::array<double, 16> matrix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
				for (std::size_t component = 0; component < 4; ++component)
				{
					matrix[component] = palette.row0[component];
					matrix[4 + component] = palette.row1[component];
					matrix[8 + component] = palette.row2[component];
				}
				shadowInputs->deformationPalette.push_back(matrix);
			}
			shadowInputs->morphWeights.assign(cpu.morphWeights.begin(), cpu.morphWeights.end());
			addShadowProof(ShadowProof::Deformation);
		}
		// Pure GPU work/palette publication generations are transient selectors;
		// their output cannot be proven exact without a CPU-owned immutable value.
		snapshotRenderable.shadowCasterInputs = std::move(shadowInputs);
	}
	catch (const std::bad_alloc&)
	{
		// Missing copied evidence conservatively refreshes; allocation failure
		// must never publish a partially built known-content proof.
		snapshotRenderable.shadowCasterInputs.reset();
	}
}

namespace {
	constexpr float kDirectionalShadowNear = 0.1f;
	constexpr float kDirectionalShadowFar = 180.0f;
	constexpr float kDirectionalShadowExtent = 40.0f;
	glm::vec3 SafeNormalizeShadowDirection(const glm::vec3& value, const glm::vec3& fallback)
	{
		const float length = glm::length(value);

		if (!std::isfinite(length) || length <= 0.00001f)
		{
			return fallback;
		}

		return value / length;
	}
	glm::vec3 ComputeDirectionalShadowCenter(const GPULight& light, const glm::vec3& cameraLocalPosition)
	{
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float texelWorldSize = (kDirectionalShadowExtent * 2.0f) / 1024.0f;

		// Matching the Vulkan light-space snap keeps CPU culling aligned with the
		// exact shadow map coverage that the GPU will render.
		const glm::vec3 provisionalLightPosition = cameraLocalPosition - lightDirection * 60.0f;
		const glm::mat4 provisionalView = glm::lookAtRH(provisionalLightPosition, cameraLocalPosition, worldUp);
		glm::vec4 snappedCenterLightSpace = provisionalView * glm::vec4(cameraLocalPosition, 1.0f);
		snappedCenterLightSpace.x = std::floor(snappedCenterLightSpace.x / texelWorldSize) * texelWorldSize;
		snappedCenterLightSpace.y = std::floor(snappedCenterLightSpace.y / texelWorldSize) * texelWorldSize;
		return glm::vec3(glm::inverse(provisionalView) * snappedCenterLightSpace);
	}
	std::uint64_t BuildLightSetSignature(std::span<const FrameLightCandidate> lights)
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
		const glm::vec3& cameraLocalPosition)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, sceneRevision);
		HashCombine(seed, settingsSignature);
		HashCombine(seed, renderableWorldSignature);
		HashCombine(seed, lightSetSignature);
		HashVec3(seed, cameraLocalPosition);
		return seed;
	}
	glm::mat4 BuildDirectionalShadowViewProjection(const GPULight& light, const glm::vec3& cameraLocalPosition)
	{
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const glm::vec3 snappedCenterWorld = ComputeDirectionalShadowCenter(light, cameraLocalPosition);
		const glm::vec3 lightPosition = snappedCenterWorld - lightDirection * 60.0f;
		glm::mat4 view = glm::lookAtRH(lightPosition, snappedCenterWorld, worldUp);
		glm::mat4 projection = glm::orthoRH_ZO(-kDirectionalShadowExtent, kDirectionalShadowExtent, -kDirectionalShadowExtent, kDirectionalShadowExtent, kDirectionalShadowNear, kDirectionalShadowFar);
		projection[1][1] *= -1.0f;
		return projection * view;
	}
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

// Shadow range jobs settle while their view-owned light span and retained spatial inputs remain alive.
std::vector<ShadowDrawList> RenderSystem::PrepareFrameShadowLists(
	const FrameVisibilityInputs& visibilityInputs, const FrameVirtualGeometryInputs& virtualGeometryInputs,
	FrameVirtualGeometryPreparation& virtualGeometryPreparation,
	std::span<const RenderItemRecord* const> renderableDrawCommandsByOrdinal,
	std::uint64_t renderPreparationSettingsSignature, std::uint64_t renderableSubmissionSignature,
	std::size_t renderGraphWorkerCount, std::span<const FrameLightCandidate> ownedViewLights,
	const glm::vec3& viewCameraLocalPosition, const TerraEngine::Renderer::RenderReferenceFrame& viewReferenceFrame,
	RenderViewType viewType, std::uint32_t outputId)
{
	const auto& renderFrameSnapshot = visibilityInputs.renderFrameSnapshot;
	const auto& retainedRenderTopology = visibilityInputs.retainedRenderTopology;
	const auto& renderableDrawCommands = visibilityInputs.renderableDrawCommands;
	const auto& fixtureTargets = virtualGeometryInputs.fixtureTargets;
	auto& fixtureShadowSelectionApplied = virtualGeometryPreparation.fixtureShadowSelectionApplied;
	auto& shadowDrawListCache = lifetimeState->shadowDrawListCache;
	const auto compactShadowDrawList = [&](TerraEngine::UUID lightId,
		const std::vector<RenderItemRecord>& commands)
	{
		ShadowDrawList result{};
		result.lightId = lightId;
		result.topologyGeneration = std::max<std::uint64_t>(
			1u, retainedRenderTopology.topology.generation);
		result.stableOrdinals.reserve(commands.size());
		result.selectedLods.reserve(commands.size());
		result.routeSelectors.reserve(commands.size());
		result.editorTints.reserve(commands.size());
		result.commandLanes.reserve(commands.size());
		for(const RenderItemRecord& command : commands)
		{
			if(command.conventionalVisibilityIdentityIndex == 0u)
				continue;
			const std::uint32_t ordinal =
				command.conventionalVisibilityIdentityIndex - 1u;
			if(ordinal >= retainedRenderTopology.topology.records.size()
				|| !retainedRenderTopology.topology.records[ordinal].groupKey.IsComplete())
				continue;
			result.stableOrdinals.push_back(ordinal);
			result.selectedLods.push_back(command.selectedVirtualLod);
			result.routeSelectors.push_back(
				static_cast<std::uint32_t>(command.geometryRoute));
			result.editorTints.push_back(command.editorTint);
			result.commandLanes.push_back(MakeRenderViewCommandLane(command));
		}
		return result;
	};
	const auto materializeShadowDrawList = [&](const ShadowDrawList& shadow)
	{
		std::vector<RenderItemRecord> commands;
		if(!shadow.HasParallelLengths()) return commands;
		commands.reserve(shadow.stableOrdinals.size());
		for(std::size_t itemIndex = 0u;
			itemIndex < shadow.stableOrdinals.size(); ++itemIndex)
		{
			const std::uint32_t ordinal = shadow.stableOrdinals[itemIndex];
			if(ordinal >= renderableDrawCommandsByOrdinal.size()
				|| renderableDrawCommandsByOrdinal[ordinal] == nullptr) continue;
			RenderItemRecord command = *renderableDrawCommandsByOrdinal[ordinal];
			ApplyRenderViewCommandLane(shadow.commandLanes[itemIndex],
				shadow.selectedLods[itemIndex],
				static_cast<TerraEngine::Renderer::GeometryRoute>(
					shadow.routeSelectors[itemIndex]),
				shadow.editorTints[itemIndex], command);
			commands.push_back(std::move(command));
		}
		return commands;
	};


		// Per-view frame storage owns these candidates after the producer vector moves.
		// Chunk jobs settle before this borrowed span leaves the protected builder.
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Shadow Draw Lists Cache Lookup", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Spatial queries use the index's own frame, regardless of a view rebase.
		const auto indexedFrame = spatialIndex.GetReferenceFrame();
		TerraEngine::Scene::WorldDelta cameraDelta;
		cameraDelta.offsetMetres = glm::dvec3(viewCameraLocalPosition);
		const auto canonicalCamera = TerraEngine::Scene::ApplyWorldDelta(viewReferenceFrame.canonicalOrigin, cameraDelta);
		const auto localizedCamera = canonicalCamera.Succeeded()
			? TerraEngine::Scene::LocalizeWorldPosition(canonicalCamera.position, indexedFrame.canonicalOrigin, indexedFrame.maximumLocalDistanceMetres)
			: TerraEngine::Scene::WorldLocalizationResult{{}, canonicalCamera.error};
		const glm::vec3 cameraLocalPosition = localizedCamera.Succeeded() ? glm::vec3(localizedCamera.offsetMetres) : glm::vec3(0);
		std::vector<FrameLightCandidate> queryLights(ownedViewLights.begin(), ownedViewLights.end());
		std::vector<bool> queryLightLocalized(queryLights.size(), false);
		for (std::size_t i = 0; i < queryLights.size(); ++i) {
			const auto localized = TerraEngine::Scene::LocalizeWorldPosition(queryLights[i].canonicalPosition,
				indexedFrame.canonicalOrigin, indexedFrame.maximumLocalDistanceMetres);
			queryLightLocalized[i] = localized.Succeeded();
			queryLights[i].gpuLight.positionAndType = glm::vec4(localized.Succeeded() ? glm::vec3(localized.offsetMetres) : glm::vec3(0),
				queryLights[i].gpuLight.positionAndType.w);
		}
		std::uint64_t signature = BuildShadowDrawListCacheKey(
			renderFrameSnapshot.sceneRevision,
			renderPreparationSettingsSignature,
			renderableSubmissionSignature,
			BuildLightSetSignature(queryLights),
			cameraLocalPosition);
		// Fixture controls change actual selector bytes, so old compact lists must
		// not bypass their application. The generation is stable across step frames.
		HashCombine(signature, renderFrameSnapshot.efficiencyFixtureIntent.controlGeneration);
		HashCombine(signature, indexedFrame.generation);
		HashCombine(signature, std::hash<double>{}(indexedFrame.maximumLocalDistanceMetres));
		HashCombine(signature, TerraEngine::Scene::WorldPositionHash{}(indexedFrame.canonicalOrigin));
		HashCombine(signature, localizedCamera.Succeeded() ? 1U : 0U);
		for (const bool known : queryLightLocalized) HashCombine(signature, known ? 1U : 0U);
		const auto* casterSelection = TerraEngine::Renderer::ResolveRendererEfficiencyFixtureCasterSelection(
			renderFrameSnapshot.efficiencyFixtureIntent, viewType, outputId);
		// View-qualified membership is stable through this captured job batch.
		// Hash every UUID byte so equal camera positions cannot share different
		// controlled caster lists, or accidentally reuse an uncontrolled list.
		HashCombine(signature, static_cast<std::uint32_t>(viewType));
		HashCombine(signature, outputId);
		HashCombine(signature, casterSelection ? casterSelection->objectIds.size() : 0U);
		if (casterSelection) for (const auto& id : casterSelection->objectIds)
			for (const auto byte : id) HashCombine(signature, byte);
		if (const auto existing = shadowDrawListCache.recordsBySignature.find(signature); existing != shadowDrawListCache.recordsBySignature.end()
			&& existing->second.controlledViewType == viewType && existing->second.controlledOutputId == outputId
			&& existing->second.fixtureControlGeneration == renderFrameSnapshot.efficiencyFixtureIntent.controlGeneration
			&& existing->second.controlledCasterSelection == (casterSelection != nullptr)
			&& (casterSelection ? existing->second.controlledCasterIds == casterSelection->objectIds : existing->second.controlledCasterIds.empty()))
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			++shadowDrawListCache.hitCount;
			existing->second.lastTouchedFrameId = activeRenderFrameId;
			std::vector<ShadowDrawList> cachedShadowDrawLists;
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Copy Cached Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			cachedShadowDrawLists = existing->second.shadowDrawLists;
			}
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Shadow VG Selection Stats", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			for (std::size_t shadowIndex = 0u; shadowIndex < cachedShadowDrawLists.size(); ++shadowIndex)
			{
				const std::vector<RenderItemRecord> diagnosticCommands =
					materializeShadowDrawList(cachedShadowDrawLists[shadowIndex]);
				const auto& intent = renderFrameSnapshot.efficiencyFixtureIntent;
				if (intent.controlsApplied && intent.shadowSelection)
				{
					const auto proof = std::find_if(fixtureTargets.begin(), fixtureTargets.end(), [&](const auto& row) { return row.objectId == intent.shadowSelection->objectId; });
					if (proof != fixtureTargets.end()) for (const auto& actual : diagnosticCommands)
					{
						auto expected = actual;
						if (TerraEngine::Renderer::ApplyRendererEfficiencyFixtureSelection(expected, *intent.shadowSelection, *proof)
							&& expected.selectedVirtualLod == actual.selectedVirtualLod
							&& expected.virtualMeshletOffset == actual.virtualMeshletOffset
							&& expected.virtualMeshletCount == actual.virtualMeshletCount)
							fixtureShadowSelectionApplied.store(true, std::memory_order_relaxed);
					}
				}
				CollectFrameVirtualGeometryDiagnostics(virtualGeometryInputs, virtualGeometryPreparation,
					diagnosticCommands, RenderViewType::Shadow);
				RecordFrameVirtualGeometrySelection(virtualGeometryInputs, virtualGeometryPreparation,
					diagnosticCommands,
					{},
					{},
					{},
					{},
					{},
					{},
					{},
					VirtualGeometryDebugVisualizationMode::None,
					RenderOutputKind::OffscreenTexture,
					RenderViewType::Shadow,
					glm::uvec2(renderFrameSnapshot.renderSettings.shadowResolution, renderFrameSnapshot.renderSettings.shadowResolution),
					static_cast<std::uint32_t>(1000u + shadowIndex),
					true,
					false);
			}
			return cachedShadowDrawLists;
		}

		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Rebuild Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		std::vector<std::optional<ShadowDrawList>> shadowDrawListsByLight(ownedViewLights.size());
		TerraEngine::Scene::SpatialQueryFilter filter{};
		filter.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;

		RunRenderChunkedWork(
			ownedViewLights.size(),
			renderGraphWorkerCount,
			kRenderParallelThreshold,
			"RenderSystem Task: Build Shadow Draw Lists",
			[&](std::size_t start, std::size_t end)
			{
				for (std::size_t lightIndex = start; lightIndex < end; ++lightIndex)
				{
					const FrameLightCandidate& candidate = queryLights[lightIndex];
					if ((candidate.candidateFlags & kFrameLightCandidateFlagShadowCapable) == 0u)
					{
						continue;
					}

					ShadowDrawList shadowDrawList{};
					shadowDrawList.lightId = candidate.lightId;

					const std::uint32_t lightType = static_cast<std::uint32_t>(std::max(candidate.gpuLight.positionAndType.w, 0.0f) + 0.5f);
					TerraEngine::Scene::SpatialQueryResult query{};

					const bool queryReady = lightType == static_cast<std::uint32_t>(LightType::Directional)
					? localizedCamera.Succeeded() : queryLightLocalized[lightIndex];
					if (!queryReady) {
						// An unrepresentable query must retain complete immutable caster coverage.
					}
					else if (lightType == static_cast<std::uint32_t>(LightType::Directional))
					{
						const glm::mat4 lightViewProjection = BuildDirectionalShadowViewProjection(candidate.gpuLight, cameraLocalPosition);
						query = spatialIndex.QueryFrustum(TerraEngine::Scene::ExtractFrustum(lightViewProjection), filter);
					}

					else if (lightType == static_cast<std::uint32_t>(LightType::Point))
					{
						TerraEngine::Scene::SpatialSphere lightBounds{};
						lightBounds.center = glm::vec3(candidate.gpuLight.positionAndType);
						lightBounds.radius = std::max(candidate.gpuLight.directionAndRange.w, 0.5f);
						query = spatialIndex.QuerySphere(lightBounds, filter);
					}

					else if (lightType == static_cast<std::uint32_t>(LightType::Spot))
					{
						const glm::mat4 lightViewProjection = BuildSpotShadowViewProjection(candidate.gpuLight);
						query = spatialIndex.QueryFrustum(TerraEngine::Scene::ExtractFrustum(lightViewProjection), filter);
					}

					else if (lightType == static_cast<std::uint32_t>(LightType::Area))
					{
						const glm::mat4 lightViewProjection = BuildAreaShadowViewProjection(candidate.gpuLight);
						query = spatialIndex.QueryFrustum(TerraEngine::Scene::ExtractFrustum(lightViewProjection), filter);
					}

					std::vector<RenderItemRecord> shadowCommands =
						queryReady ? MaterializeFrameVisibleCandidates(visibilityInputs, CollectFrameVisibleCandidates(visibilityInputs, query)) : renderableDrawCommands;
					ApplyFrameShadowVirtualGeometrySelection(virtualGeometryInputs, virtualGeometryPreparation, shadowCommands);
					TerraEngine::Renderer::FilterRendererEfficiencyFixtureShadowCasters(shadowCommands, casterSelection);
					shadowDrawList = compactShadowDrawList(
						candidate.lightId, shadowCommands);
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
			const std::vector<RenderItemRecord> diagnosticCommands =
				materializeShadowDrawList(shadowDrawLists[shadowIndex]);
			RecordFrameVirtualGeometrySelection(virtualGeometryInputs, virtualGeometryPreparation,
				diagnosticCommands,
				{},
				{},
				{},
				{},
				{},
				{},
				{},
				VirtualGeometryDebugVisualizationMode::None,
				RenderOutputKind::OffscreenTexture,
				RenderViewType::Shadow,
				glm::uvec2(renderFrameSnapshot.renderSettings.shadowResolution, renderFrameSnapshot.renderSettings.shadowResolution),
				static_cast<std::uint32_t>(1000u + shadowIndex),
				true,
				false);
		}

		ShadowDrawListCacheRecord record;
		record.signature = signature;
		// The digest narrows lookup; exact qualified membership still authorizes
		// cache reuse, so collisions cannot substitute another view's caster set.
		record.controlledViewType = viewType; record.controlledOutputId = outputId;
		record.fixtureControlGeneration = renderFrameSnapshot.efficiencyFixtureIntent.controlGeneration;
		record.controlledCasterSelection = casterSelection != nullptr;
		if (casterSelection) record.controlledCasterIds = casterSelection->objectIds;
		record.lastTouchedFrameId = activeRenderFrameId;
		record.shadowDrawLists = shadowDrawLists;
		shadowDrawListCache.recordsBySignature[signature] = std::move(record);
		TrimSignatureCache(shadowDrawListCache, 32u);
		++shadowDrawListCache.missCount;
		return shadowDrawLists;
	}
