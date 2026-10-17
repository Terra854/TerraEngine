// Actual per-view cache reconstruction, spatial admission and resource demand.
#include "RenderViewPreparation.h"
#include "RenderPreparationTasks.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/ResourceStreaming.h"
namespace TerraEngine::Renderer::Frame {
	std::uint64_t BuildVisibleCommandCacheKey(
		std::uint64_t sceneRevision,
		std::uint64_t settingsSignature,
		std::uint64_t renderableVisibilitySignature,
		const glm::mat4& viewProjection,
		const glm::uvec2& targetSize,
		RenderOutputKind outputKind,
		RenderViewType viewType,
		std::uint32_t outputId,
		std::uint64_t transitionStateSignature,
		std::uint64_t virtualGeometryResidencyGeneration,
		std::uint64_t rendererResourceGeneration,
		std::uint64_t rendererGeneration)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, sceneRevision);
		HashCombine(seed, settingsSignature);
		// Candidate admission depends on stable geometry plus the small set of
		// animation states that change coarse visibility or owner fallback. Do not
		// include the pose publication generation here: it advances every animated
		// frame even though the same objects, LODs, and geometry routes stay visible.
		HashCombine(seed, renderableVisibilitySignature);
		HashMat4(seed, viewProjection);
		HashUVec2(seed, targetSize);
		// Output kind separates Scene, Game, runtime, shadow, and capture frame
		// identities even when a caller happens to reuse a view type and output id.
		HashCombine(seed, static_cast<std::uint64_t>(outputKind));
		HashCombine(seed, static_cast<std::uint64_t>(viewType));
		HashCombine(seed, outputId);
		HashCombine(seed, transitionStateSignature);
		// Cached page needs and selected fallback clusters are valid only for the
		// physical residency snapshot that produced them. Publication changes the
		// generation at a protected boundary and forces one deterministic rebuild.
		HashCombine(seed, virtualGeometryResidencyGeneration);
		// Conventional and direct draws also retain renderer-owned resources. A
		// successful hot reload must therefore miss this cache even when virtual
		// geometry page residency did not change.
		HashCombine(seed, rendererResourceGeneration);
		// Device recovery publishes a new render-session generation. Cached packets
		// and selection evidence must never cross that protected lifetime boundary.
		HashCombine(seed, rendererGeneration);
		// Selection depends on the camera, output target, topology, resources, and
		// residency above, not on the caller's capture request string. Readback keeps
		// its exact request/frame/renderer/target generations separately, so repeated
		// captures may safely retain this immutable geometry evidence.
		return seed;
	}
}
// Consume only the declared borrowed view; returned arrays retain their original owning storage.
std::vector<std::size_t> RenderSystem::CollectFrameVisibleCandidates(const FrameVisibilityInputs& inputs, const TerraEngine::Scene::SpatialQueryResult& query)
{
	const auto renderables = inputs.renderables;
	const auto& renderFrameSnapshot = inputs.renderFrameSnapshot;
	const auto& renderableLookup = inputs.spatialCache.renderableLookup;
	const auto& conservativeAnimationBounds = inputs.spatialCache.conservativeAnimationBounds;
	const auto& renderableDrawCommands = inputs.renderableDrawCommands;
	const auto& retainedRenderTopology = inputs.retainedRenderTopology;
	const auto& conventionalVisibilityIdentityIndices = retainedRenderTopology.conventionalVisibilityIdentityIndices;
	const auto& renderableCommandIndices = inputs.identityCache.indicesByObjectId;

		std::vector<std::size_t> candidateIndices;
		candidateIndices.reserve(std::min<std::size_t>(
			renderables.size(),
			query.hits.size() + renderables.size()));
		std::vector<bool> spatiallyVisible(renderables.size(), false);
		// Convert the spatial result to a stable renderable-order mask. Properly
		// cooked deformable meshes already publish a whole-animation envelope;
		// only legacy/unproven animated bounds need the fail-open append below.
		for (const TerraEngine::Scene::SpatialQueryHit& hit : query.hits)
		{
			if (const auto lookup = renderableLookup.find(hit.entity); lookup != renderableLookup.end())
			{
				spatiallyVisible[lookup->second] = true;
			}
		}
		for (std::size_t index = 0u; index < renderables.size(); ++index)
		{
			const Mesh* mesh = renderables[index].runtimeMesh;
			const bool ownerFallbackNeedsVisibility =
				renderFrameSnapshot.renderables[index].animationBinding
					.RequiresVirtualGeometryOwnerFallbackVisibility(
						mesh != nullptr && mesh->hasVirtualGeometry);
			if (spatiallyVisible[index]
				|| renderFrameSnapshot.renderables[index].animationBinding
					.RequiresUnboundedCoarseVisibility(
						conservativeAnimationBounds[index])
				|| ownerFallbackNeedsVisibility)
			{
				// The later virtual-geometry selector expands this exact case into a
				// complete owner draw. Appending it here is deliberately narrow: healthy
				// page-local animation still benefits from ordinary CPU broadphase culling.
				candidateIndices.push_back(index);
			}
		}
		return candidateIndices;
	}

// Consume only the declared borrowed view; returned arrays retain their original owning storage.
std::vector<RenderItemRecord> RenderSystem::MaterializeFrameVisibleCandidates(const FrameVisibilityInputs& inputs, const std::vector<std::size_t>& candidateIndices)
{
	const auto renderables = inputs.renderables;
	const auto& renderFrameSnapshot = inputs.renderFrameSnapshot;
	const auto& renderableLookup = inputs.spatialCache.renderableLookup;
	const auto& conservativeAnimationBounds = inputs.spatialCache.conservativeAnimationBounds;
	const auto& renderableDrawCommands = inputs.renderableDrawCommands;
	const auto& retainedRenderTopology = inputs.retainedRenderTopology;
	const auto& conventionalVisibilityIdentityIndices = retainedRenderTopology.conventionalVisibilityIdentityIndices;
	const auto& renderableCommandIndices = inputs.identityCache.indicesByObjectId;

		std::vector<RenderItemRecord> commands;
		commands.reserve(candidateIndices.size());

		for (const std::size_t candidateIndex : candidateIndices)
		{
			commands.push_back(renderableDrawCommands[candidateIndex]);
			commands.back().animation = BuildPublishedAnimationGeneration(
				renderFrameSnapshot.renderables[candidateIndex].animation);
			commands.back().animationBinding =
					TerraEngine::Renderer::PublishedAnimationRenderBinding(
						renderFrameSnapshot.renderables[candidateIndex].animationBinding);
			const auto identity = conventionalVisibilityIdentityIndices.find(
				commands.back().objectId);
			commands.back().conventionalVisibilityIdentityIndex =
				identity != conventionalVisibilityIdentityIndices.end()
					? identity->second : 0u;
		}
		return commands;
	}

// Consume only the declared borrowed view; returned arrays retain their original owning storage.
TerraEngine::Renderer::VisibleStreamingTouchSet RenderSystem::BuildFrameVisibleStreamingTouches(
	const FrameVisibleStreamingInputs& inputs, const std::vector<RenderItemRecord>& commands,
	RenderViewType viewType, std::uint32_t outputId, const glm::vec3& cameraPosition,
	TerraEngine::Resource::TerrainVisibleTextureTouchSnapshot* terrainTouches)
{
	const auto& frameMaterialResources = inputs.frameMaterialResources;
	const auto& frameTerrainObjects = inputs.terrain.frameTerrainObjects;
	const auto& frameTerrainMaterialBindings = inputs.terrain.frameTerrainMaterialBindings;

		TerraEngine::Renderer::VisibleStreamingTouchSet touchSet;
		touchSet.viewType = viewType;
		touchSet.outputId = outputId;
		touchSet.sourceCommandCount = static_cast<std::uint32_t>(std::min<std::size_t>(commands.size(), std::numeric_limits<std::uint32_t>::max()));
		touchSet.valid = true;

		std::unordered_set<TerraEngine::UUID> uniqueMeshes;
		std::unordered_set<TerraEngine::UUID> uniqueMaterials;
		std::unordered_map<TerraEngine::UUID, std::size_t> textureTouchIndices;
		uniqueMeshes.reserve(commands.size());
		uniqueMaterials.reserve(commands.size());
		textureTouchIndices.reserve(commands.size());
		std::vector<TerraEngine::Resource::TerrainVisibleMaterialArea> visibleTerrainAreas;
		visibleTerrainAreas.reserve(commands.size());

		std::uint32_t sourceTouchCount = 0u;
		for (const RenderItemRecord& command : commands)
		{
			if (command.meshId != TerraEngine::EMPTY_UUID)
			{
				++sourceTouchCount;
				if (uniqueMeshes.insert(command.meshId).second)
				{
					touchSet.meshes.push_back({ command.meshId, ::VisibleStreamingResourceKind::Mesh, 100 });
				}
			}
			if (command.materialId != TerraEngine::EMPTY_UUID)
			{
				++sourceTouchCount;
				if (uniqueMaterials.insert(command.materialId).second)
				{
					touchSet.materials.push_back({ command.materialId, ::VisibleStreamingResourceKind::Material, 100 });
				}
			}
			const glm::vec3 objectPosition = glm::vec3(command.modelMatrix[3]);
			const double viewDistance = static_cast<double>(
				glm::length(objectPosition - cameraPosition));
			if(const auto terrainObject = frameTerrainObjects.find(command.objectId);
				terrainObject != frameTerrainObjects.end())
			{
				const auto terrainMaterial = frameTerrainMaterialBindings.find(
					terrainObject->second.second);
				if(terrainMaterial != frameTerrainMaterialBindings.end())
				{
					const auto appendArea = [&](TerraEngine::Terrain::TerrainAreaCoordinate area,
						double sampleSpacingMetres) {
						TerraEngine::Resource::TerrainVisibleMaterialArea visible{};
						visible.assetId = terrainObject->second.first;
						visible.area = area;
						visible.materialId = terrainMaterial->second.materialId;
						visible.weightTextureId = terrainMaterial->second.weightTextureId;
						visible.layerIdTextureId = terrainMaterial->second.layerIdTextureId;
						visible.layerCount = std::min<std::uint32_t>(
							terrainMaterial->second.layerCount,
							static_cast<std::uint32_t>(visible.layers.size()));
						for(std::uint32_t layerIndex = 0u;
							layerIndex < visible.layerCount; ++layerIndex)
						{
							const auto& source = terrainMaterial->second.layers[layerIndex];
							visible.layers[layerIndex] = {source.stableLayerId,
								source.baseColorTextureId, source.normalTextureId,
								source.ormTextureId, source.emissiveTextureId};
						}
						// The command translation is already localized to this view. Add the
						// canonical area offset in double precision so distant signed area
						// indices do not collapse before nearest-distance aggregation.
						const glm::dvec3 objectDelta = glm::dvec3(objectPosition - cameraPosition);
						const glm::dvec3 areaOffset{
							static_cast<double>(area.x) * 256.0 * sampleSpacingMetres,
							0.0,
							static_cast<double>(area.z) * 256.0 * sampleSpacingMetres};
						visible.nearestViewDistance = glm::length(objectDelta + areaOffset);
						visibleTerrainAreas.push_back(std::move(visible));
					};
					if(terrainMaterial->second.areaMappings.empty())
					{
						// Temporary direct origin proxies predate per-area phase tables.
						// Capture their real sources as area zero until their next publication.
						appendArea({}, 1.0);
					}
					else
					{
						for(const auto& area : terrainMaterial->second.areaMappings)
							appendArea(area.area, area.sampleSpacingMetres);
					}
				}
			}

			const auto capturedMaterial = frameMaterialResources.find(command.materialId);
			const Material* material = capturedMaterial != frameMaterialResources.end()
				? capturedMaterial->second : nullptr;
			if (material == nullptr)
			{
				continue;
			}
			for (const TextureBinding& binding : material->textures)
			{
				if (binding.textureUuid == TerraEngine::EMPTY_UUID)
				{
					continue;
				}
				++sourceTouchCount;
				auto [existingTexture, inserted] = textureTouchIndices.emplace(binding.textureUuid, touchSet.textures.size());
				if (inserted)
				{
					touchSet.textures.push_back({ binding.textureUuid, viewDistance, 90 });
				}
				else
				{
					TerraEngine::Renderer::VisibleTextureTouch& touch = touchSet.textures[existingTexture->second];
					touch.nearestViewDistance = std::min(touch.nearestViewDistance, viewDistance);
				}
			}
		}
		const auto preparedTerrainTouches = TerraEngine::Resource::
			PrepareTerrainVisibleTextureTouchSnapshot(activeRenderFrameId, visibleTerrainAreas);
		if(terrainTouches != nullptr) *terrainTouches = preparedTerrainTouches;
		if(preparedTerrainTouches.complete)
		{
			sourceTouchCount += static_cast<std::uint32_t>(std::min<std::size_t>(
				preparedTerrainTouches.touches.size()
					+ preparedTerrainTouches.duplicateEliminationCount,
				std::numeric_limits<std::uint32_t>::max() - sourceTouchCount));
			for(const TerraEngine::Resource::TerrainVisibleTextureTouch& terrainTouch
				: preparedTerrainTouches.touches)
			{
				auto [existingTexture, inserted] = textureTouchIndices.emplace(
					terrainTouch.textureId, touchSet.textures.size());
				if(inserted)
				{
					touchSet.textures.push_back({terrainTouch.textureId,
						terrainTouch.nearestViewDistance, terrainTouch.priority});
				}
				else
				{
					auto& touch = touchSet.textures[existingTexture->second];
					touch.nearestViewDistance = std::min(
						touch.nearestViewDistance, terrainTouch.nearestViewDistance);
					touch.priority = std::max(touch.priority, terrainTouch.priority);
				}
			}
		}

		const std::uint32_t uniqueTouchCount =
			static_cast<std::uint32_t>(std::min<std::size_t>(
				touchSet.meshes.size() + touchSet.materials.size() + touchSet.textures.size(),
				std::numeric_limits<std::uint32_t>::max()));
		touchSet.duplicateEliminationCount = sourceTouchCount > uniqueTouchCount ? sourceTouchCount - uniqueTouchCount : 0u;
		return touchSet;
	}

// Consume only the declared borrowed view; returned arrays retain their original owning storage.
TerraEngine::Renderer::VisibleCommandReuseTelemetry RenderSystem::BuildFrameVisibleCommandTelemetry(
	RenderViewType viewType, std::uint32_t outputId, bool cacheHit, bool copiedCommands,
	const TerraEngine::Renderer::VisibleStreamingTouchSet& touchSet)
{
		TerraEngine::Renderer::VisibleCommandReuseTelemetry telemetry;
		telemetry.viewType = viewType;
		telemetry.outputId = outputId;
		telemetry.cacheHit = cacheHit;
		telemetry.commandCount = touchSet.sourceCommandCount;
		telemetry.copiedCommandCount = copiedCommands ? touchSet.sourceCommandCount : 0u;
		telemetry.uniqueMeshTouches = static_cast<std::uint32_t>(std::min<std::size_t>(touchSet.meshes.size(), std::numeric_limits<std::uint32_t>::max()));
		telemetry.uniqueMaterialTouches = static_cast<std::uint32_t>(std::min<std::size_t>(touchSet.materials.size(), std::numeric_limits<std::uint32_t>::max()));
		telemetry.uniqueTextureTouches = static_cast<std::uint32_t>(std::min<std::size_t>(touchSet.textures.size(), std::numeric_limits<std::uint32_t>::max()));
		telemetry.duplicateResourceTouchesEliminated = touchSet.duplicateEliminationCount;
		telemetry.detailedDiagnosticsRequested = TerraEngine::Renderer::RenderDiagnosticsRequested();
		return telemetry;
	}

// Consume only the declared borrowed view; returned arrays retain their original owning storage.
TerraEngine::Renderer::Frame::VisibleCommandBuildResult RenderSystem::PrepareFrameVisibleCommands(
	const FrameVisibilityInputs& inputs, const FrameVirtualGeometryInputs& virtualGeometryInputs,
	FrameVirtualGeometryPreparation& virtualGeometryPreparation, const FrameVisibleStreamingInputs& streamingInputs,
	std::vector<std::pair<RenderViewType, TerraEngine::Renderer::VisibleStreamingTouchSet>>& pendingStreamingTouchSets,
	const glm::mat4& viewProjection, const glm::uvec2& targetSize, RenderOutputKind outputKind,
	RenderViewType viewType, std::uint32_t outputId, const glm::vec3& cameraPosition,
	const TerraEngine::Renderer::RenderReferenceFrame& referenceFrame)
{
	const auto renderables = inputs.renderables;
	const auto& renderFrameSnapshot = inputs.renderFrameSnapshot;
	const auto& renderableLookup = inputs.spatialCache.renderableLookup;
	const auto& conservativeAnimationBounds = inputs.spatialCache.conservativeAnimationBounds;
	const auto& renderableDrawCommands = inputs.renderableDrawCommands;
	const auto& retainedRenderTopology = inputs.retainedRenderTopology;
	const auto& conventionalVisibilityIdentityIndices = retainedRenderTopology.conventionalVisibilityIdentityIndices;
	const auto& renderableCommandIndices = inputs.identityCache.indicesByObjectId;

	const auto& latestSpatialRefreshDiagnostics = inputs.latestSpatialRefreshDiagnostics;
	const auto renderableVisibilitySignature = inputs.renderableVisibilitySignature;
	const auto snapshotRendererGeneration = inputs.snapshotRendererGeneration;
	const auto virtualGeometryResidencyGeneration = inputs.virtualGeometryResidencyGeneration;
	auto& visibleCommandCache = lifetimeState->visibleCommandCache;

		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Visible Draw Commands Cache Lookup", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const auto localizeViewCommands = [&](std::vector<RenderItemRecord>& commands)
		{
			commands.erase(std::remove_if(commands.begin(), commands.end(),
				[&](RenderItemRecord& command)
				{
					const TerraEngine::Renderer::RenderLocalizedPoseMatrices localized =
						TerraEngine::Renderer::BuildLocalizedRenderPoseMatrices(
							command.canonicalWorldPose, referenceFrame);
					if (!localized.valid)
					{
						return true;
					}
					command.modelMatrix = localized.currentMatrix;
					return false;
				}), commands.end());
		};
		VirtualGeometryQualityProfile viewVirtualGeometryQuality = renderFrameSnapshot.renderSettings.virtualGeometry;
		viewVirtualGeometryQuality.debugVisualizationMode = GetVirtualGeometryDebugVisualizationMode(viewType, outputId);
		viewVirtualGeometryQuality = SanitizeVirtualGeometryQualityProfile(viewVirtualGeometryQuality);
		RenderSettings viewRenderSettings = renderFrameSnapshot.renderSettings;
		viewRenderSettings.virtualGeometry = viewVirtualGeometryQuality;
		std::uint64_t signature = BuildVisibleCommandCacheKey(
			renderFrameSnapshot.sceneRevision,
			BuildRenderSettingsSignature(viewRenderSettings, currentPreset, taskShaderCullingEnabled),
			renderableVisibilitySignature,
			viewProjection,
			targetSize,
			outputKind,
			viewType,
			outputId,
			BuildVirtualGeometryTransitionStateSignature(virtualGeometryTransitionCache, outputKind, viewType, outputId),
			virtualGeometryResidencyGeneration,
			renderFrameSnapshot.resourceRevision,
			snapshotRendererGeneration);
		// Slot lifetime keys and packet-template identities are topology-owned. A
		// rebuilt allocator root must never reuse a retained view-selection payload.
		HashCombine(signature, retainedRenderTopology.topology.generation);
		HashCombine(signature, static_cast<std::uint64_t>(
			TerraEngine::Scene::WorldPositionHash{}(referenceFrame.canonicalOrigin)));
		HashCombine(signature, referenceFrame.originGeneration);
		HashCombine(signature, referenceFrame.spatialGeneration);
		HashCombine(signature, referenceFrame.residencyGeneration);
		const auto materializeCachedCommands = [&](const VisibleCommandCacheRecord& record)
		{
			std::vector<RenderItemRecord> commands;
			const std::size_t count = record.stableOrdinals.size();
			commands.reserve(count);
			for(std::size_t index = 0u; index < count; ++index)
			{
				const std::uint32_t ordinal = record.stableOrdinals[index];
				if(ordinal >= renderableDrawCommands.size()
					|| ordinal >= renderables.size()
					|| index >= record.selectedLods.size()
					|| index >= record.routeSelectors.size()
					|| index >= record.editorTints.size()
					|| record.commandLanes == nullptr
					|| index >= record.commandLanes->size()) {
					continue;
				}
				RenderItemRecord command = renderableDrawCommands[ordinal];
				command.animation = BuildPublishedAnimationGeneration(
					renderFrameSnapshot.renderables[ordinal].animation);
				command.animationBinding =
					TerraEngine::Renderer::PublishedAnimationRenderBinding(
						renderFrameSnapshot.renderables[ordinal].animationBinding);
				ApplyRenderViewCommandLane((*record.commandLanes)[index],
					record.selectedLods[index],
					static_cast<TerraEngine::Renderer::GeometryRoute>(
						record.routeSelectors[index]),
					record.editorTints[index], command);
				commands.push_back(std::move(command));
			}
			localizeViewCommands(commands);
			return commands;
		};
		if (const auto existing = visibleCommandCache.recordsBySignature.find(signature); existing != visibleCommandCache.recordsBySignature.end())
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Visible Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			++visibleCommandCache.hitCount;
			existing->second.lastTouchedFrameId = activeRenderFrameId;
			std::vector<RenderItemRecord> cachedCommands;
			if(!existing->second.hasViewDrawEvidence
				|| !existing->second.viewDraws)
			{
				// Legacy/incomplete cache records still fail open through complete command
				// reconstruction. Current records retain compact view draws and do not need
				// a crowd-sized command array merely to republish the same selection.
				cachedCommands = materializeCachedCommands(existing->second);
			}
			if (TerraEngine::Renderer::RenderDiagnosticsRequested())
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Visible Diagnostics", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				if(!cachedCommands.empty())
				{
					CollectFrameVirtualGeometryDiagnostics(virtualGeometryInputs, virtualGeometryPreparation, cachedCommands, viewType);
				}
				else
				{
					// Diagnostics need one runtime entry per mesh, not one complete draw
					// record per instance. Resolve unique cached ordinals directly from the
					// shared base working set and keep command materialization deferred.
					std::vector<RenderItemRecord> uniqueMeshCommands;
					std::unordered_set<TerraEngine::UUID> seenMeshes;
					for(const std::uint32_t ordinal : existing->second.stableOrdinals)
					{
						if(ordinal >= renderableDrawCommands.size()) continue;
						const RenderItemRecord& command = renderableDrawCommands[ordinal];
						if(seenMeshes.insert(command.meshId).second)
							uniqueMeshCommands.push_back(command);
					}
					CollectFrameVirtualGeometryDiagnostics(virtualGeometryInputs, virtualGeometryPreparation, uniqueMeshCommands, viewType);
				}
			}
			const RenderViewType diagnosticsViewType = outputKind == RenderOutputKind::MainRuntimeView ? RenderViewType::ExportRuntime : viewType;
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Visible VG Selection Stats", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				const auto& retained = *existing->second.retainedSelectionEvidence;
				RecordFrameVirtualGeometrySelection(virtualGeometryInputs, virtualGeometryPreparation, cachedCommands,
					retained.selectedClusters, retained.pageNeeds,
					retained.clusterPackets, retained.clusterBatches,
					retained.compactionResults, retained.fallbackCoverage,
					retained.lodTransitionStates,
					viewVirtualGeometryQuality.debugVisualizationMode, outputKind,
					diagnosticsViewType, targetSize, outputId, false,
					outputKind == RenderOutputKind::MainRuntimeView,
					&existing->second.virtualGeometryCommandIndices, true, nullptr,
					&existing->second);
			}
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Visible Streaming Touch Set", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				if (!existing->second.streamingTouchSet.valid)
				{
					existing->second.reuseTelemetry.conservativeRefresh = true;
					existing->second.reuseTelemetry.fallbackReason = "cached visible streaming touch set was unavailable";
					existing->second.streamingTouchSet = BuildFrameVisibleStreamingTouches(streamingInputs,
						cachedCommands, viewType, outputId, cameraPosition,
						&existing->second.terrainTextureTouches);
				}
				pendingStreamingTouchSets.emplace_back(
					viewType, existing->second.streamingTouchSet);
			}
			VisibleCommandBuildResult cachedResult;
			{
				TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reference Cached Visible Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				// Binding refresh is fused into the mandatory command materialization below.
				// The cache's revision-validated indices avoid both this former full loop
				// and a second crowd-sized overlay allocation on every hit.
				cachedResult.cachedRecord = &existing->second;
				cachedResult.commands = std::move(cachedCommands);
				cachedResult.packetEvidenceSignature = existing->second.signature;
				cachedResult.streamingTouchSet = existing->second.streamingTouchSet;
				cachedResult.terrainTextureTouches = existing->second.terrainTextureTouches;
				cachedResult.reuseTelemetry = BuildFrameVisibleCommandTelemetry(viewType, outputId, true, true, existing->second.streamingTouchSet);
				if (existing->second.reuseTelemetry.conservativeRefresh)
				{
					cachedResult.reuseTelemetry.conservativeRefresh = true;
					cachedResult.reuseTelemetry.fallbackReason = existing->second.reuseTelemetry.fallbackReason;
				}
				cachedResult.diagnostics = existing->second.diagnostics;
				cachedResult.cacheHit = true;
			}
			return cachedResult;
		}

		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Rebuild Visible Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const TerraEngine::Scene::SpatialFrustum frustum = TerraEngine::Scene::ExtractFrustum(viewProjection);
		TerraEngine::Scene::SpatialQueryFilter filter{};
		filter.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;
		filter.collectDiagnostics = TerraEngine::Renderer::RenderDiagnosticsRequested();
		const TerraEngine::Scene::SpatialReferenceFrame indexedFrame =
			spatialIndex.GetReferenceFrame();
		VisibleCommandBuildResult result;
		result.packetEvidenceSignature = signature;
		if (indexedFrame.canonicalOrigin == referenceFrame.canonicalOrigin
			&& indexedFrame.generation == referenceFrame.spatialGeneration)
		{
			const TerraEngine::Scene::SpatialQueryResult query =
				spatialIndex.QueryFrustum(frustum, filter);
			result.commands = MaterializeFrameVisibleCandidates(inputs,
				CollectFrameVisibleCandidates(inputs, query));
		}
		else
		{
			// A view outside the currently materialized region must not reinterpret
			// that region's float index. Until its resident region index is published,
			// localize the immutable command snapshot and fail closed on out-of-range
			// records; no live scene storage is scanned here.
			result.commands = renderableDrawCommands;
		}
		localizeViewCommands(result.commands);
		VirtualGeometryViewSelectionPacket selectionPacket = ApplyFrameVirtualGeometrySelection(virtualGeometryInputs, virtualGeometryPreparation, result.commands, viewProjection, outputKind, viewType, outputId, cameraPosition, targetSize, viewVirtualGeometryQuality);
		const RenderViewType diagnosticsViewType = outputKind == RenderOutputKind::MainRuntimeView ? RenderViewType::ExportRuntime : viewType;
		RecordFrameVirtualGeometrySelection(virtualGeometryInputs, virtualGeometryPreparation, result.commands, selectionPacket.selectedClusters, selectionPacket.pageNeeds, selectionPacket.clusterPackets, selectionPacket.clusterBatches, selectionPacket.compactionResults, selectionPacket.fallbackCoverage, selectionPacket.lodTransitionStates, viewVirtualGeometryQuality.debugVisualizationMode, outputKind, diagnosticsViewType, targetSize, outputId, false, outputKind == RenderOutputKind::MainRuntimeView, nullptr, true, &selectionPacket);
		result.selectedClusters = std::move(selectionPacket.selectedClusters);
		result.pageNeeds = std::move(selectionPacket.pageNeeds);
		result.clusterPackets = std::move(selectionPacket.clusterPackets);
		result.clusterBatches = std::move(selectionPacket.clusterBatches);
		result.gpuPacketRecords = std::move(selectionPacket.gpuPacketRecords);
		result.gpuBatchDescriptors = std::move(selectionPacket.gpuBatchDescriptors);
		result.gpuPacketFrameSets = std::move(selectionPacket.gpuPacketFrameSets);
		result.gpuCompactionDispatches = std::move(selectionPacket.gpuCompactionDispatches);
		result.generatedSubmissions = std::move(selectionPacket.generatedSubmissions);
		result.gpuCounterReadbacks = std::move(selectionPacket.gpuCounterReadbacks);
		result.gpuFallbackRecords = std::move(selectionPacket.gpuFallbackRecords);
		result.compactionResults = std::move(selectionPacket.compactionResults);
		result.fallbackCoverage = std::move(selectionPacket.fallbackCoverage);
		result.lodTransitionStates = std::move(selectionPacket.lodTransitionStates);
		result.diagnostics = latestSpatialRefreshDiagnostics;
		const TerraEngine::Scene::SpatialDiagnosticsSnapshot queryDiagnostics = spatialIndex.GetDiagnosticsSnapshot();
		result.diagnostics.visibleCandidateCount = queryDiagnostics.visibleCandidateCount;
		result.diagnostics.rejectedObjectCount = queryDiagnostics.rejectedObjectCount;
		result.diagnostics.fallbackObjectCount = queryDiagnostics.fallbackObjectCount;
		result.diagnostics.lastQueryDurationMs = queryDiagnostics.lastQueryDurationMs;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Visible Streaming Touch Set", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			result.streamingTouchSet = BuildFrameVisibleStreamingTouches(streamingInputs,
				result.commands, viewType, outputId, cameraPosition,
				&result.terrainTextureTouches);
			result.reuseTelemetry = BuildFrameVisibleCommandTelemetry(viewType, outputId, false, false, result.streamingTouchSet);
		}
		pendingStreamingTouchSets.emplace_back(viewType, result.streamingTouchSet);
		VisibleCommandCacheRecord record;
		record.signature = signature;
		record.lastTouchedFrameId = activeRenderFrameId;
		record.stableOrdinals.reserve(result.commands.size());
		record.selectedLods.reserve(result.commands.size());
		record.routeSelectors.reserve(result.commands.size());
		record.editorTints.reserve(result.commands.size());
		std::vector<RenderViewCommandLane> recordCommandLanes;
		recordCommandLanes.reserve(result.commands.size());
		for(std::size_t commandIndex = 0U;
			commandIndex < result.commands.size(); ++commandIndex)
		{
			const RenderItemRecord& command = result.commands[commandIndex];
			const auto current = renderableCommandIndices.find(command.objectId);
			const bool indexRepresentable = current != renderableCommandIndices.end()
				&& current->second <= std::numeric_limits<std::uint32_t>::max();
			if(!indexRepresentable) continue;
			record.stableOrdinals.push_back(
				static_cast<std::uint32_t>(current->second));
			record.selectedLods.push_back(command.selectedVirtualLod);
			record.routeSelectors.push_back(
				static_cast<std::uint32_t>(command.geometryRoute));
			record.editorTints.push_back(command.editorTint);
			recordCommandLanes.push_back(MakeRenderViewCommandLane(command));
			const bool contributesVirtualGeometry =
				(command.virtualGeometryFlags & (kVirtualGeometryFlagActive
					| kVirtualGeometryFlagFailOpen)) != 0U
				|| command.virtualMeshletCount != 0U;
			if(contributesVirtualGeometry
				&& record.stableOrdinals.size() - 1u
					<= std::numeric_limits<std::uint32_t>::max()) {
				record.virtualGeometryCommandIndices.push_back(
					static_cast<std::uint32_t>(
						record.stableOrdinals.size() - 1u));
			}
		}
		auto retainedSelectionEvidence = std::make_shared<TerraEngine::Renderer::
			VirtualGeometryRetainedSelectionEvidence>();
		retainedSelectionEvidence->selectedClusters = result.selectedClusters;
		retainedSelectionEvidence->pageNeeds = result.pageNeeds;
		retainedSelectionEvidence->clusterPackets = result.clusterPackets;
		retainedSelectionEvidence->clusterBatches = result.clusterBatches;
		auto retainedGpuEvidence =
			std::make_shared<VirtualGeometryRetainedGpuEvidence>();
		retainedGpuEvidence->packetRecords = result.gpuPacketRecords;
		retainedGpuEvidence->batchDescriptors = result.gpuBatchDescriptors;
		retainedGpuEvidence->packetFrameSets = result.gpuPacketFrameSets;
		retainedGpuEvidence->compactionDispatches = result.gpuCompactionDispatches;
		retainedGpuEvidence->generatedSubmissions = result.generatedSubmissions;
		retainedGpuEvidence->counterReadbacks = result.gpuCounterReadbacks;
		retainedGpuEvidence->fallbackRecords = result.gpuFallbackRecords;
		record.retainedGpuEvidence = std::move(retainedGpuEvidence);
		retainedSelectionEvidence->compactionResults =
			result.compactionResults;
		retainedSelectionEvidence->fallbackCoverage =
			result.fallbackCoverage;
		retainedSelectionEvidence->lodTransitionStates =
			result.lodTransitionStates;
		if(!renderFrameSnapshot.virtualGeometryViewSelections.empty())
		{
			const TerraEngine::Renderer::VirtualGeometryViewSelectionState& viewEvidence =
				renderFrameSnapshot.virtualGeometryViewSelections.back();
			retainedSelectionEvidence->pageRequests = viewEvidence.PageRequests();
			retainedSelectionEvidence->residentPagePayloads =
				viewEvidence.ResidentPagePayloads();
			retainedSelectionEvidence->emittedPageSources =
				viewEvidence.EmittedPageSources();
			record.viewDraws = std::make_shared<const std::vector<
				TerraEngine::Renderer::VirtualGeometryViewDrawRecord>>(
					viewEvidence.DrawRecords());
			record.hasViewDrawEvidence = true;
		}
		record.commandLanes = std::make_shared<const std::vector<
			RenderViewCommandLane>>(std::move(recordCommandLanes));
		record.retainedSelectionEvidence = std::move(retainedSelectionEvidence);
		record.streamingTouchSet = result.streamingTouchSet;
		record.terrainTextureTouches = result.terrainTextureTouches;
		record.reuseTelemetry = result.reuseTelemetry;
		record.diagnostics = result.diagnostics;
		visibleCommandCache.recordsBySignature[signature] = std::move(record);
		TrimSignatureCache(visibleCommandCache, 32u);
		++visibleCommandCache.missCount;
		return result;
	}
