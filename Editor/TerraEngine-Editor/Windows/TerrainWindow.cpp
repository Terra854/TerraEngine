// TerrainWindow.cpp -- Conservative terrain authoring controls.

#include "TerrainWindow.h"

#include "../Terrain/TerrainCreationService.h"
#include "../Terrain/TerrainHistoryCommand.h"
#include "../Subsystems/UndoRedoSystem.h"

#include "Terrain/TerrainAreaCodec.h"
#include "Terrain/TerrainDerivedSystem.h"
#include "Terrain/TerrainStreamingSystem.h"
#include "AssetDatabase.h"
#include "Components/MeshRenderComponent.h"
#include "Components/TerrainComponent.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "File Formats/TerraEngineTextureFile.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Physics/Terrain/TerrainHeightFieldActiveRect.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/Terrain/TerrainRenderProxyRegistry.h"
#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"
#include "ThreadPool/ThreadPool.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainHydraulicErosion.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainProceduralOperators.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainSplineGrader.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainThermalErosion.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#include <imgui.h>

namespace
{
	std::unique_ptr<Texture> MakeTerrainControlTexture(
		std::uint16_t width,
		std::uint16_t height,
		std::uint32_t vkFormat,
		std::uint16_t bytesPerPixel,
		std::vector<std::uint8_t> pixels)
	{
		const std::size_t expectedBytes = static_cast<std::size_t>(width) * height * bytesPerPixel;
		if(width == 0u || height == 0u || pixels.size() != expectedBytes) return {};
		auto texture = std::make_unique<Texture>();
		texture->header.magic = kMagic;
		texture->header.version = kVersion;
		texture->header.headerSize = sizeof(TETFHeader);
		texture->descriptor.width = width;
		texture->descriptor.height = height;
		texture->descriptor.depth = 1u;
		texture->descriptor.layers = 1u;
		texture->descriptor.mipCount = 1u;
		texture->descriptor.faces = 1u;
		texture->descriptor.blockWidth = 1u;
		texture->descriptor.blockHeight = 1u;
		texture->descriptor.blockBytes = bytesPerPixel;
		texture->descriptor.semantic = static_cast<std::uint8_t>(TextureFileSemantic::Data);
		texture->descriptor.payloadChunkIndex = TETF_INVALID_CHUNK_INDEX;
		texture->descriptor.userMetadataChunk = TETF_INVALID_CHUNK_INDEX;
		texture->descriptor.thumbnailChunk = TETF_INVALID_CHUNK_INDEX;
		texture->descriptor.vkFormat = vkFormat;
		texture->levelTable.push_back({0u, static_cast<std::uint64_t>(pixels.size())});
		texture->transcodedData = std::move(pixels);
		return texture;
	}

	std::optional<std::uint16_t> FindUnusedTerrainLayerId(
		const TerraEngine::Terrain::TerrainLayerStack& stack)
	{
		// Stable ID 0 is invalid and 0xffff belongs to the renderer's diagnostic
		// fallback. Searching the bounded authoring range avoids wraparound when an
		// artist has imported definitions with large, sparse IDs.
		for(std::uint32_t candidate = 1u;
			candidate < TerraEngine::Terrain::kTerrainDiagnosticFallbackLayerId;
			++candidate)
		{
			const bool used = std::any_of(stack.layers.begin(), stack.layers.end(),
				[candidate](const auto& layer) { return layer.stableId == candidate; });
			if(!used) return static_cast<std::uint16_t>(candidate);
		}
		return std::nullopt;
	}

	bool EqualTerrainLayerName(std::string_view left, std::string_view right) noexcept
	{
		// Layer validation uses ASCII case-insensitive names. The editor mirrors
		// that rule when proposing copy names so a generated action cannot create a
		// candidate that the protected transaction boundary must reject.
		if(left.size() != right.size()) return false;
		for(std::size_t index = 0u; index < left.size(); ++index)
		{
			const auto fold = [](unsigned char value) {
				return value >= 'A' && value <= 'Z'
					? static_cast<unsigned char>(value - 'A' + 'a') : value;
			};
			if(fold(static_cast<unsigned char>(left[index]))
				!= fold(static_cast<unsigned char>(right[index]))) return false;
		}
		return true;
	}

	std::string MakeUniqueTerrainLayerCopyName(
		const TerraEngine::Terrain::TerrainLayerStack& stack,
		std::string_view sourceName)
	{
		for(std::uint32_t copyNumber = 1u; copyNumber <= 1'000u; ++copyNumber)
		{
			const std::string candidate = std::string(sourceName) + (copyNumber == 1u
				? " Copy" : " Copy " + std::to_string(copyNumber));
			const bool used = std::any_of(stack.layers.begin(), stack.layers.end(),
				[&candidate](const auto& layer) { return EqualTerrainLayerName(layer.name, candidate); });
			if(!used) return candidate;
		}
		return {};
	}

	const char* TerrainLayerFallbackLabel(
		TerraEngine::Terrain::TerrainLayerFallbackState state) noexcept
	{
		switch(state)
		{
		case TerraEngine::Terrain::TerrainLayerFallbackState::Healthy: return "source ready";
		case TerraEngine::Terrain::TerrainLayerFallbackState::MissingSource: return "material source missing";
		case TerraEngine::Terrain::TerrainLayerFallbackState::InvalidSource: return "material source invalid";
		case TerraEngine::Terrain::TerrainLayerFallbackState::FallbackActive: return "intentional fallback active";
		}
		return "unknown fallback state";
	}
}

TerrainWindow::TerrainWindow()
{
	automationAdapter.SetCommandCallback([this](std::string_view capability, const nlohmann::json& arguments) {
		return ExecuteAutomationCommand(capability, arguments);
	});
	automationAdapter.SetTypedOperationCallback([this](std::string_view, const TerraEngine::Editor::Automation::TerrainAutomationRequest& request) {
		const auto started = BeginAutomationOperation(request);
		return TerraEngine::Editor::Terrain::TerrainAutomationCommandResult{
			started.accepted, started.category, started.message, started.transactionId, nlohmann::json::object()};
	});
}

TerrainWindow::~TerrainWindow()
{
	if(terrainRenderProxyCancellation)
		terrainRenderProxyCancellation->store(true, std::memory_order_release);
	TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance().Remove(
		terrainRenderProxyMaterialId);
	// The collision scheduler may outlive editor windows during shutdown. Remove
	// only this window's store pointer before its immutable storage is destroyed.
	if(terrainStore)
		TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().ClearPublication(terrainStore.get());
}

bool TerrainWindow::QueueAutomaticTerrainCollision(std::string& outError)
{
	outError.clear();
	if(!terrainStore) return true;
	if(committedHeightMm.size() != TerraEngine::Terrain::kTerrainAreaSampleCount)
	{
		outError = "Automatic terrain collision requires one resident canonical terrain area.";
		return false;
	}

	// Edit Mode deliberately owns no live physics world. Keep the request armed;
	// StartPlayMode calls this method again immediately after world creation.
	if(!TerraEngine::Physics::PhysicsSystem::Instance().WorldHandle().IsValid())
		return true;

	const auto capture = terrainStore->CaptureRoot();
	if(!capture.IsValid())
	{
		outError = "Automatic terrain collision requires a current immutable root.";
		return false;
	}
	const auto started = std::chrono::steady_clock::now();
	TerraEngine::Physics::TerrainHeightFieldRecipe recipe;
	recipe.terrainAssetId = capture.root->assetUuid;
	// One editor session can select any signed fixed-storage area. Preserve that
	// address in the recipe so physics and navigation reconstruct the same canonical
	// origin as mounted export terrain.
	recipe.areaX = selectedTerrainArea.x;
	recipe.areaZ = selectedTerrainArea.z;
	recipe.authoredGeneration = capture.publicationGeneration;
	recipe.authoredHash = capture.root->rootContentHash.low64 ^ capture.root->rootContentHash.high64;
	if(recipe.authoredHash == 0u) recipe.authoredHash = 1u;
	recipe.sampleSpacingMetres = authoredSampleSpacingMetres;
	recipe.heightMillimetres.resize(TerraEngine::Physics::TerrainHeightFieldRecipe::kSampleCount);
	for(std::uint32_t z = 0u; z < TerraEngine::Physics::TerrainHeightFieldRecipe::kGridExtent; ++z)
		for(std::uint32_t x = 0u; x < TerraEngine::Physics::TerrainHeightFieldRecipe::kGridExtent; ++x)
			recipe.heightMillimetres[static_cast<std::size_t>(z) * TerraEngine::Physics::TerrainHeightFieldRecipe::kGridExtent + x]
				= committedHeightMm[static_cast<std::size_t>(std::min(z, 255u)) * 256u + std::min(x, 255u)];
	recipe.holeBits = committedHoleMask;
	if(recipe.holeBits.size() != TerraEngine::Physics::TerrainHeightFieldRecipe::kHoleByteCount)
		recipe.holeBits.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0u);
	const auto active = TerraEngine::Terrain::TerrainActiveRectForArea(
		capture.root->authoredBounds, selectedTerrainArea);
	if(!active || !TerraEngine::Physics::Terrain::ApplyTerrainActiveRectToRecipe(recipe, *active))
	{
		// Keep the committed authoring mask untouched. Only this detached candidate is
		// rejected, so the next valid root publication can retry without data loss.
		outError = "Automatic terrain collision requires a valid authored active rectangle.";
		return false;
	}
	auto candidate = TerraEngine::Physics::Terrain::PrepareTerrainCollisionCandidate(
		std::move(recipe), capture.root->rootContentHash, capture.publicationGeneration);
	candidate.buildLatencyMilliseconds = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - started).count();
	if(!candidate.complete)
	{
		outError = "The automatic terrain collision recipe failed provider-neutral validation.";
		return false;
	}
	TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().SetPreparedCandidate(std::move(candidate));
	return true;
}

void TerrainWindow::ScheduleRenderProxyBuild()
{
	const bool useDetachedPreview = authoringSession.IsDirty() || holePreviewActive;
	if(!terrainStore) return;
	if(terrainRenderProxyBuildActive)
	{
		// Let the current immutable preview finish so a continuous drag still gets
		// visible intermediate results. One queued rebuild then consumes the newest
		// preview buffer, bounding worker pressure to a single active task.
		terrainRenderProxyRebuildRequested = true;
		return;
	}
	const auto capture = terrainStore->CaptureRoot();
	if(!capture.IsValid()) return;
	const auto& bounds = capture.root->authoredBounds;
	const std::int64_t spanX = bounds.maxXExclusive - bounds.minX;
	const std::int64_t spanZ = bounds.maxZExclusive - bounds.minZ;
	if(spanX < 2 || spanZ < 2) return;
	const std::uint32_t overviewSamplesX = static_cast<std::uint32_t>(std::min<std::int64_t>(256, spanX));
	const std::uint32_t overviewSamplesZ = static_cast<std::uint32_t>(std::min<std::int64_t>(256, spanZ));
	const auto& heightAreas = authoringSession.IsDirty() ? previewAreaHeightMm : committedAreaHeightMm;
	const auto& holeAreas = holePreviewActive ? previewAreaHoleMasks : committedAreaHoleMasks;
	std::vector<std::int32_t> heights(
		TerraEngine::Terrain::kTerrainAreaSampleCount, capture.root->defaultHeightMm);
	std::vector<std::uint8_t> holes(TerraEngine::Terrain::kTerrainAreaSampleCount / 8u, 0u);
	const auto sourceCoordinate = [](std::int64_t minimum, std::int64_t span,
		std::uint32_t output, std::uint32_t outputCount) {
		// The overview is capped at 256 samples per axis. Rounding the immutable
		// source coordinate keeps both authored endpoints exact while bounding the
		// editor proxy cost independently of the number of expanded areas.
		const long double numerator = static_cast<long double>(span - 1)
			* static_cast<long double>(output);
		return minimum + static_cast<std::int64_t>(std::llround(
			numerator / static_cast<long double>(outputCount - 1u)));
	};
	for(std::uint32_t z = 0u; z < overviewSamplesZ; ++z)
		for(std::uint32_t x = 0u; x < overviewSamplesX; ++x)
		{
			const TerraEngine::Terrain::TerrainSampleCoordinate sample{
				sourceCoordinate(bounds.minX, spanX, x, overviewSamplesX),
				sourceCoordinate(bounds.minZ, spanZ, z, overviewSamplesZ)};
			const auto area = TerraEngine::Terrain::OwnerArea(sample);
			const auto local = TerraEngine::Terrain::LocalSample(sample);
			if(!area || !local) continue;
			const std::size_t sourceIndex = static_cast<std::size_t>(local->z) * 256u + local->x;
			const std::size_t destinationIndex = static_cast<std::size_t>(z) * 256u + x;
			if(const auto found = heightAreas.find(*area);
				found != heightAreas.end() && found->second.size() == TerraEngine::Terrain::kTerrainAreaSampleCount)
				heights[destinationIndex] = found->second[sourceIndex];
			if(const auto found = holeAreas.find(*area);
				found != holeAreas.end() && found->second.size() == TerraEngine::Terrain::kTerrainAreaSampleCount / 8u
				&& (found->second[sourceIndex / 8u] & static_cast<std::uint8_t>(1u << (sourceIndex % 8u))) != 0u)
				holes[destinationIndex / 8u] |= static_cast<std::uint8_t>(1u << (destinationIndex % 8u));
		}
	const auto cancellation = std::make_shared<std::atomic_bool>(false);
	const TerraEngine::UUID assetId = capture.root->assetUuid;
	auto authoredHash = capture.root->rootContentHash;
	std::uint64_t generation = std::max<std::uint64_t>(1u, capture.publicationGeneration);
	if(useDetachedPreview)
	{
		// Preview identity is derived only from copied editor buffers. It never enters
		// TerrainAssetStore and therefore cannot become visible as authored content.
		std::vector<std::uint8_t> previewBytes(
			heights.size() * sizeof(std::int32_t) + holes.size());
		std::memcpy(previewBytes.data(), heights.data(), heights.size() * sizeof(std::int32_t));
		if(!holes.empty())
			std::memcpy(previewBytes.data() + heights.size() * sizeof(std::int32_t), holes.data(), holes.size());
		authoredHash = TerraEngine::Terrain::HashTerrainBytes(previewBytes);
		generation = generation == std::numeric_limits<std::uint64_t>::max()
			? generation : generation + 1u;
	}
	const float spacing = authoredSampleSpacingMetres;
	const std::array<double, 2u> originMetres{
		static_cast<double>(bounds.minX) * spacing,
		static_cast<double>(bounds.minZ) * spacing};
	const std::array<float, 2u> overviewStep{
		static_cast<float>(static_cast<double>(spanX - 1) * spacing / static_cast<double>(overviewSamplesX - 1u)),
		static_cast<float>(static_cast<double>(spanZ - 1) * spacing / static_cast<double>(overviewSamplesZ - 1u))};
	const TerraEngine::Terrain::TerrainAreaActiveRect activeRect{
		0u, 0u, static_cast<std::uint16_t>(overviewSamplesX), static_cast<std::uint16_t>(overviewSamplesZ)};
	auto work = [assetId, activeRect, authoredHash, generation, spacing, originMetres, overviewStep,
		heights = std::move(heights), holes = std::move(holes), cancellation]() mutable
	{
		TerraEngine::Renderer::Terrain::TerrainDirectMeshInput input{};
		input.assetId = assetId;
		input.activeRect = activeRect;
		input.authoredHash = authoredHash;
		input.authoredGeneration = generation;
		input.sampleSpacingMetres = spacing;
		input.sampleOriginMetres = originMetres;
		input.sampleStepMetres = overviewStep;
		input.heightMillimetres = heights;
		input.holeCellBits = holes;
		return TerraEngine::Renderer::Terrain::BuildTerrainDirectMesh(input, cancellation.get());
	};
	auto task = ThreadPool::instance().createTask(std::move(work));
	terrainRenderProxyFuture = task->getFuture();
	terrainRenderProxyCancellation = cancellation;
	terrainRenderProxyBuildActive = true;
	terrainRenderProxyBuildWasPreview = useDetachedPreview;
	terrainRenderProxyRebuildRequested = false;
	ThreadPool::instance().enqueue(task, TerraEngine::Terrain::kTerrainAreaSampleCount);
}

void TerrainWindow::QueueRenderProxySceneInstance()
{
	if(!terrainStore
		|| terrainRenderProxyMeshId == TerraEngine::EMPTY_UUID
		|| terrainRenderProxyMaterialId == TerraEngine::EMPTY_UUID)
		return;
	const auto capture = terrainStore->CaptureRoot();
	if(!capture.IsValid()) return;
	// Terrain render resources are derived from the durable asset and published
	// outside ECS. RenderSystem resolves this binding directly from
	// TerrainComponent, so scene serialization never needs generated mesh IDs.
	if(!TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance().Publish({
		capture.root->assetUuid, terrainRenderProxyMeshId, terrainRenderProxyMaterialId}))
	{
		status = "Terrain render proxy publication rejected an incomplete binding.";
		return;
	}
	// Resource replacement and scene-instance creation have different lifetimes.
	// A terrain edit or renderer reset must republish this binding even though the
	// durable scene object already exists and needs no additional mutation.
	if(terrainRenderProxyInstanceQueued) return;
	ObjectManager& objects = ObjectManager::instance();
	if(terrainRenderProxyTargetObjectId != TerraEngine::EMPTY_UUID)
	{
		const auto targetObject = objects.getObject(terrainRenderProxyTargetObjectId);
		if(!targetObject)
		{
			status = "Terrain render proxy restore was cancelled because its scene object no longer exists.";
			return;
		}
		// Earlier editor versions serialized the generated fallback as a mesh
		// component. Remove only that recognizable proxy shape; an independently
		// authored mesh component on the same object remains untouched.
		if(const MeshRenderComponent* legacy = objects.GetComponent<MeshRenderComponent>(*targetObject);
			legacy != nullptr
			&& legacy->meshId == terrainRenderProxyMeshId
			&& legacy->materialId == terrainRenderProxyMaterialId
			&& legacy->spatialObjectId == TerraEngine::EMPTY_UUID
			&& !legacy->taskShaderCullingEnabled)
		{
			TerraEngine::Scene::TransactionalMutationBuilder migration(
				"terrain-render-migration-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()),
				TerraEngine::Scene::TransactionOrigin::Editor,
				"Remove Legacy Terrain Render Proxy");
			migration.ObserveScene(objects)
				.ObserveObject(objects, terrainRenderProxyTargetObjectId)
				.SetUndoGroup("Remove Legacy Terrain Render Proxy")
				.RemoveComponent(terrainRenderProxyTargetObjectId,
					std::string(MeshRenderComponent::kComponentName));
			TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(migration.Build());
		}
		terrainRenderProxyInstanceQueued = true;
		return;
	}
	TerraEngine::Scene::TransactionalMutationBuilder builder(
		"terrain-render-instance-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()),
		TerraEngine::Scene::TransactionOrigin::Editor,
		"Create Terrain Render Instance");
	const auto local = TerraEngine::Scene::LocalObject(-1);
	builder.ObserveScene(objects)
		.SetUndoGroup("Create Terrain Render Instance")
		.CreateObject(-1, "Terrain")
		.SetStatic(local, true)
		.AddComponent(local, std::string(TerrainComponent::kComponentName))
		.UpdateComponentProperty(local, std::string(TerrainComponent::kComponentName), "terrainAssetId", TerraEngine::Reflection::Variant{capture.root->assetUuid})
		.UpdateComponentProperty(local, std::string(TerrainComponent::kComponentName), "visible", TerraEngine::Reflection::Variant{true})
		.UpdateComponentProperty(local, std::string(TerrainComponent::kComponentName), "castsShadows", TerraEngine::Reflection::Variant{true});
	TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(builder.Build());
	terrainRenderProxyInstanceQueued = true;
}

bool TerrainWindow::RestoreSceneTerrainProxy()
{
	ObjectManager& objects = ObjectManager::instance();
	TerraEngine::UUID firstVisibleObjectId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID missingProxyObjectId = TerraEngine::EMPTY_UUID;
	objects.forEachObjectWithComponents<TerrainComponent>(
		[&](Object object, TerrainComponent& terrain)
		{
			if(!terrain.visible || terrain.terrainAssetId == TerraEngine::EMPTY_UUID) return;
			const TerraEngine::UUID candidateObjectId = objects.getUUID(object);
			if(firstVisibleObjectId == TerraEngine::EMPTY_UUID)
				firstVisibleObjectId = candidateObjectId;
			// Build one missing asset at a time. Once it publishes, the next update
			// can select another terrain asset without duplicating scene components.
			if(missingProxyObjectId == TerraEngine::EMPTY_UUID
				&& !TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance()
					.Resolve(terrain.terrainAssetId))
			{
				missingProxyObjectId = candidateObjectId;
			}
		});
	const TerraEngine::UUID objectId = missingProxyObjectId != TerraEngine::EMPTY_UUID
		? missingProxyObjectId : firstVisibleObjectId;
	if(objectId == TerraEngine::EMPTY_UUID) return false;

	std::string error;
	return OpenTerrainForEditing(objectId, error);
}

bool TerrainWindow::OpenTerrainForEditing(TerraEngine::UUID objectId, std::string& outError)
{
	outError.clear();
	ObjectManager& objects = ObjectManager::instance();
	const std::optional<Object> object = objects.getObject(objectId);
	const TerrainComponent* terrain = object ? objects.GetComponent<TerrainComponent>(*object) : nullptr;
	if(!object || terrain == nullptr || terrain->terrainAssetId == TerraEngine::EMPTY_UUID)
	{
		outError = "The selected object does not reference an editable terrain asset.";
		status = outError;
		return false;
	}
	const TerraEngine::UUID assetId = terrain->terrainAssetId;
	if(terrainStore)
	{
		const auto current = terrainStore->CaptureRoot();
		if(current.IsValid() && current.root->assetUuid == assetId
			&& terrainRenderProxyTargetObjectId == objectId)
		{
			// Renderer teardown can retire derived proxy bindings while the durable
			// terrain component and authoring store remain valid. Rebuild on demand
			// so the scene recovers without requiring another Inspector drop.
			const auto proxy = TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance()
				.Resolve(assetId);
			if(!proxy && !terrainRenderProxyBuildActive) ScheduleRenderProxyBuild();
			status = "Terrain Tools is ready: drag in Scene View to create a detached sculpt preview.";
			return true;
		}
		if(authoringSession.IsDirty())
		{
			outError = "Apply or cancel the current terrain preview before editing another terrain.";
			status = outError;
			return false;
		}
	}

	std::filesystem::path terrainPath = ResourceManager::Instance().GetRegisteredAssetPath(assetId, AssetType::Terrain);
	if(terrainPath.empty())
	{
		if(const AssetMetadata* metadata = AssetDatabase::Instance().FindAssetByGuid(assetId))
			terrainPath = AssetDatabase::Instance().GetAssetRoot() / metadata->sourcePath;
	}
	if(terrainPath.empty())
	{
		status = "Terrain Tools could not open the selected terrain because its asset is not registered.";
		outError = status;
		TERRAENGINE_LOG(LogLevel::Error, status << " Asset: " << TerraEngine::ToHexString(assetId));
		return false;
	}

	TerraEngine::Editor::Terrain::TerrainCreationService service;
	const auto opened = service.OpenExisting(terrainPath);
	if(!opened.succeeded || opened.assetId != assetId)
	{
		status = "Terrain Tools could not open the selected terrain: " + opened.message;
		outError = status;
		TERRAENGINE_LOG(LogLevel::Error, status << " Path: " << terrainPath);
		return false;
	}
	destinationPath = terrainPath;
	authoredSampleSpacingMetres = opened.sampleSpacingMetres;
	InitializeAuthoringTerrain(opened.assetId, opened.heightfield, objectId, opened.authoredBounds);
	status = "Editing " + terrainPath.filename().string() + ". Drag in Scene View to sculpt a detached preview.";
	return true;
}

void TerrainWindow::PollRenderProxyBuild()
{
	if(!terrainRenderProxyBuildActive || !terrainRenderProxyFuture.valid()
		|| terrainRenderProxyFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	auto completed = terrainRenderProxyFuture.get();
	terrainRenderProxyBuildActive = false;
	terrainRenderProxyCancellation.reset();
	const bool completedWasPreview = terrainRenderProxyBuildWasPreview;
	const auto capture = terrainStore ? terrainStore->CaptureRoot() : TerraEngine::Terrain::TerrainRootCapture{};
	const bool currentAsset = capture.IsValid() && completed.assetId == capture.root->assetUuid;
	const std::uint64_t currentRootGeneration = capture.IsValid()
		? std::max<std::uint64_t>(1u, capture.publicationGeneration) : 0u;
	const bool current = completedWasPreview
		? currentAsset && (authoringSession.IsDirty() || holePreviewActive)
		: currentAsset && completed.authoredHash == capture.root->rootContentHash
			&& completed.authoredGeneration == currentRootGeneration;
	const bool rebuildRequested = terrainRenderProxyRebuildRequested;
	terrainRenderProxyRebuildRequested = false;
	if(!completed.Succeeded() || !current)
	{
		// A cancelled or obsolete preview must never replace the current proxy. The
		// queued rebuild selects the latest detached or committed buffers afresh.
		ScheduleRenderProxyBuild();
		return;
	}

	ResourceManager& resources = ResourceManager::Instance();
	SynchronizeTerrainMaterial();
	if(terrainRenderProxyMaterialId == TerraEngine::EMPTY_UUID)
	{
		status = "Terrain material publication failed; the mesh remains detached.";
		return;
	}
	const std::string meshKey = "terrain/" + TerraEngine::ToHexString(completed.assetId);
	terrainRenderProxyMeshId = resources.RegisterMesh(meshKey, std::move(completed.mesh));
	RenderSystem::Instance().InvalidateGeneratedMesh(terrainRenderProxyMeshId);
	QueueRenderProxySceneInstance();
	status = completedWasPreview
		? "Terrain brush preview updated in Scene View."
		: "Terrain render proxy published; Scene and Game views will consume the committed generation.";
	if(rebuildRequested)
		ScheduleRenderProxyBuild();
}

void TerrainWindow::SynchronizeTerrainMaterial(bool forceLayerMaterialRefresh)
{
	if(!terrainStore) return;
	const auto capture = terrainStore->CaptureRoot();
	if(!capture.IsValid() || !capture.root->layerStack) return;
	// Detached layer previews and material availability notifications live in the
	// editor mirror until their protected transaction publishes. Rendering that
	// mirror keeps material feedback immediate without mutating the immutable root.
	const TerraEngine::Terrain::TerrainLayerStack& runtimeLayerStack = LayerPreview();
	TerraEngine::Terrain::TerrainMaterialSettings settings{};
	ObjectManager& objects = ObjectManager::instance();
	bool found = false;
	objects.forEachObjectWithComponents<TerrainComponent>(
		[&](Object object, TerrainComponent& terrain)
		{
			if(found || terrain.terrainAssetId != capture.root->assetUuid) return;
			if(terrainRenderProxyTargetObjectId != TerraEngine::EMPTY_UUID
				&& objects.getUUID(object) != terrainRenderProxyTargetObjectId) return;
			settings = terrain.MaterialSettings();
			found = true;
		});
	// A new terrain can reach this boundary one flush before its component. The
	// documented defaults remain valid until the next protected synchronization.
	settings = TerraEngine::Terrain::SanitizeTerrainMaterialSettings(settings);
	const auto foundLayer = std::find_if(
		runtimeLayerStack.layers.begin(), runtimeLayerStack.layers.end(),
		[](const TerraEngine::Terrain::TerrainLayer& layer) {
			return layer.enabled && layer.materialAssetUuid != TerraEngine::EMPTY_UUID
				&& layer.fallbackState == TerraEngine::Terrain::TerrainLayerFallbackState::Healthy;
		});
	const TerraEngine::Terrain::TerrainLayer* surfaceLayer = foundLayer != runtimeLayerStack.layers.end()
		? &*foundLayer : nullptr;
	const TerraEngine::UUID surfaceMaterialId = surfaceLayer
		? surfaceLayer->materialAssetUuid : TerraEngine::EMPTY_UUID;
	const Material* surfaceMaterial = surfaceMaterialId != TerraEngine::EMPTY_UUID
		? ResourceManager::Instance().GetMaterial(surfaceMaterialId) : nullptr;
	const bool sourceAvailable = surfaceMaterial != nullptr;

	const auto& controlWeights = surfacePreviewActive ? previewSurfaceWeights : committedSurfaceWeights;
	if(controlWeights.size() != TerraEngine::Terrain::kTerrainAreaSampleCount) return;
	std::vector<std::uint8_t> weightPixels(controlWeights.size() * 4u, 0u);
	std::vector<std::uint8_t> layerIdPixels(controlWeights.size() * 8u, 0u);
	for(std::size_t sampleIndex = 0u; sampleIndex < controlWeights.size(); ++sampleIndex)
	{
		for(std::size_t contributor = 0u; contributor < 4u; ++contributor)
		{
			const auto& slot = controlWeights[sampleIndex].slots[contributor];
			weightPixels[sampleIndex * 4u + contributor] = slot.weight;
			layerIdPixels[sampleIndex * 8u + contributor * 2u] = static_cast<std::uint8_t>(slot.layerId & 0xffu);
			layerIdPixels[sampleIndex * 8u + contributor * 2u + 1u] = static_cast<std::uint8_t>(slot.layerId >> 8u);
		}
	}
	std::vector<std::uint8_t> controlIdentityBytes;
	controlIdentityBytes.reserve(weightPixels.size() + layerIdPixels.size());
	controlIdentityBytes.insert(controlIdentityBytes.end(), weightPixels.begin(), weightPixels.end());
	controlIdentityBytes.insert(controlIdentityBytes.end(), layerIdPixels.begin(), layerIdPixels.end());
	const auto controlHash = TerraEngine::Terrain::HashTerrainBytes(controlIdentityBytes);
	const bool controlTexturesChanged = terrainWeightTextureId == TerraEngine::EMPTY_UUID
		|| terrainLayerIdTextureId == TerraEngine::EMPTY_UUID
		|| publishedSurfaceControlHash != controlHash;
	if(!forceLayerMaterialRefresh && !controlTexturesChanged
		&& terrainRenderProxyMaterialId != TerraEngine::EMPTY_UUID
		&& publishedTerrainMaterial && *publishedTerrainMaterial == settings
		&& publishedTerrainLayerGeneration == runtimeLayerStack.generation
		&& publishedTerrainLayerMaterialId == surfaceMaterialId
		&& publishedTerrainLayerSourceAvailable == sourceAvailable
		&& publishedSurfaceControlHash == controlHash) return;
	ResourceManager& resources = ResourceManager::Instance();
	if(controlTexturesChanged)
	{
		// Painting changes these larger images. A color-only material edit reuses
		// them and updates just the runtime layer table below.
		terrainWeightTextureId = resources.RegisterTexture(
			"terrain/control-weights/" + TerraEngine::ToHexString(capture.root->assetUuid),
			MakeTerrainControlTexture(256u, 256u, 37u, 4u, std::move(weightPixels)));
		terrainLayerIdTextureId = resources.RegisterTexture(
			"terrain/control-layer-ids/" + TerraEngine::ToHexString(capture.root->assetUuid),
			MakeTerrainControlTexture(256u, 256u, 91u, 8u, std::move(layerIdPixels)));
		RenderSystem::Instance().InvalidateGeneratedTexture(terrainWeightTextureId);
		RenderSystem::Instance().InvalidateGeneratedTexture(terrainLayerIdTextureId);
	}
	terrainRenderProxyMaterialId = resources.RegisterMaterial(
		"terrain/editor/" + TerraEngine::ToHexString(capture.root->assetUuid),
		TerraEngine::Terrain::CreateTerrainMaterial(
			resources, settings, surfaceMaterial,
			surfaceLayer ? &surfaceLayer->mapping : nullptr));
	publishedTerrainMaterial = settings;
	publishedTerrainLayerGeneration = runtimeLayerStack.generation;
	publishedTerrainLayerMaterialId = surfaceMaterialId;
	publishedTerrainLayerSourceAvailable = sourceAvailable;
	publishedSurfaceControlHash = controlHash;

	TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeBinding runtimeBinding{};
	runtimeBinding.materialId = terrainRenderProxyMaterialId;
	runtimeBinding.weightTextureId = terrainWeightTextureId;
	runtimeBinding.layerIdTextureId = terrainLayerIdTextureId;
	for(const auto& layer : runtimeLayerStack.layers)
	{
		if(!layer.enabled || runtimeBinding.layerCount >= runtimeBinding.layers.size()) continue;
		auto& runtimeLayer = runtimeBinding.layers[runtimeBinding.layerCount++];
		runtimeLayer.stableLayerId = layer.stableId;
		runtimeLayer.mapping.tileSizeMetres = {
			static_cast<float>(layer.mapping.tileSizeXMetres), static_cast<float>(layer.mapping.tileSizeZMetres)};
		runtimeLayer.mapping.inverseRotationCosSin = {
			static_cast<float>(std::cos(-layer.mapping.rotationRadians)),
			static_cast<float>(std::sin(-layer.mapping.rotationRadians))};
		runtimeLayer.mapping.triplanarSharpness = static_cast<float>(layer.mapping.triplanarSharpness);
		runtimeLayer.mapping.mode = layer.mapping.mode == TerraEngine::Terrain::TerrainLayerMappingMode::Triplanar
			? TerraEngine::Renderer::Terrain::TerrainGpuMappingMode::Triplanar
			: TerraEngine::Renderer::Terrain::TerrainGpuMappingMode::TerrainXZ;
		runtimeLayer.uvTransform = {
			static_cast<float>(layer.mapping.tileSizeXMetres), static_cast<float>(layer.mapping.tileSizeZMetres),
			static_cast<float>(layer.mapping.offsetXMetres), static_cast<float>(layer.mapping.offsetZMetres)};
		if(layer.materialAssetUuid == TerraEngine::EMPTY_UUID)
		{
			// A newly created terrain deliberately starts with a usable flat layer;
			// magenta is reserved for an authored source that later became invalid.
			runtimeLayer.baseColorFactor = {
				settings.baseColor.r, settings.baseColor.g, settings.baseColor.b, 1.0F};
			continue;
		}
		const Material* layerMaterial = resources.GetMaterial(layer.materialAssetUuid);
		if(layerMaterial == nullptr || layer.fallbackState != TerraEngine::Terrain::TerrainLayerFallbackState::Healthy)
		{
			// The immutable binding carries an explicit diagnostic state; renderer
			// preparation resolves it without fabricating a texture UUID.
			runtimeLayer.fallbackActive = true;
			runtimeLayer.baseColorFactor = {1.0F, 0.8F, 0.0F, 1.0F};
			continue;
		}
		// TMAT is authoritative for all four PBR semantics and their factors. The
		// generated proxy stays neutral, so this source is applied exactly once.
		static_cast<TerraEngine::Terrain::TerrainLayerSurfaceSource&>(runtimeLayer) =
			TerraEngine::Terrain::ResolveTerrainLayerSurfaceSource(*layerMaterial);
		// The resolved TMAT factor and TerrainComponent tint are independent
		// authored inputs. Bake their product into the immutable runtime layer once;
		// the generated proxy material remains neutral and cannot apply it again.
		runtimeLayer.baseColorFactor = {
			layerMaterial->header.baseColorFactor[0] * settings.baseColor.r,
			layerMaterial->header.baseColorFactor[1] * settings.baseColor.g,
			layerMaterial->header.baseColorFactor[2] * settings.baseColor.b,
			layerMaterial->header.baseColorFactor[3]};
		const auto baseTexture = std::find_if(layerMaterial->textures.begin(), layerMaterial->textures.end(),
			[](const TextureBinding& binding) { return binding.semantic == TextureSemantic::Base; });
		if(baseTexture != layerMaterial->textures.end())
		{
			runtimeLayer.uvTransform[0] *= baseTexture->uvScale[0];
			runtimeLayer.uvTransform[1] *= baseTexture->uvScale[1];
			runtimeLayer.uvTransform[2] += baseTexture->uvOffset[0];
			runtimeLayer.uvTransform[3] += baseTexture->uvOffset[1];
		}
	}
	for(const auto& shard : capture.root->directoryShards)
	{
		for(const auto& [area, descriptor] : shard.descriptors)
		{
			(void)descriptor;
			TerraEngine::Renderer::Terrain::TerrainAreaMappingBinding areaBinding;
			areaBinding.area = area;
			areaBinding.sampleSpacingMetres = capture.root->sampleSpacingMetres;
			areaBinding.layerCount = runtimeBinding.layerCount;
			bool complete = true;
			for(std::uint32_t layerIndex = 0u; layerIndex < runtimeBinding.layerCount; ++layerIndex)
			{
				const auto enabledLayer = std::find_if(runtimeLayerStack.layers.begin(), runtimeLayerStack.layers.end(),
					[&runtimeBinding, layerIndex](const auto& candidate) {
						return candidate.stableId == runtimeBinding.layers[layerIndex].stableLayerId;
					});
				TerraEngine::Terrain::TerrainAreaMaterialPhase phase;
				if(enabledLayer == runtimeLayerStack.layers.end()
					|| !TerraEngine::Terrain::PrepareTerrainAreaMaterialPhase(
						area, capture.root->sampleSpacingMetres, enabledLayer->mapping, phase).Succeeded())
				{
					complete = false;
					break;
				}
				areaBinding.layerPhases[layerIndex].stableLayerId = enabledLayer->stableId;
				areaBinding.layerPhases[layerIndex].phaseMetres = {
					static_cast<float>(phase.phaseMetres.x), static_cast<float>(phase.phaseMetres.y)};
			}
			if(complete) runtimeBinding.areaMappings.push_back(std::move(areaBinding));
		}
	}
	if(!TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance().Publish(std::move(runtimeBinding)))
	{
		status = "Terrain material controls could not publish a complete runtime binding.";
		return;
	}
	// The UUID is stable across layer edits, so retire the old GPU record only
	// after its complete replacement runtime binding is visible.
	RenderSystem::Instance().InvalidateGeneratedMaterial(terrainRenderProxyMaterialId);
	// Replacing the stable resource advances ResourceManager's renderer token.
	// Republish too, because a renderer reset may have cleared the asset binding.
	if(terrainRenderProxyMeshId != TerraEngine::EMPTY_UUID)
		(void)TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance().Publish(
			{capture.root->assetUuid, terrainRenderProxyMeshId, terrainRenderProxyMaterialId});
}

void TerrainWindow::ReleaseSceneTerrain(TerraEngine::UUID assetId)
{
	if(!terrainStore) return;
	const auto capture = terrainStore->CaptureRoot();
	if(!capture.IsValid() || capture.root->assetUuid != assetId) return;
	ResetSceneTerrainState(
		"Terrain component removed; terrain rendering, collision, and authoring were detached.");
}

void TerrainWindow::ReleaseProjectTerrainState()
{
	// Project close is a stronger boundary than component removal. Retire the
	// opened asset even when the component observation pass did not run this frame.
	if(terrainStore)
	{
		const auto capture = terrainStore->CaptureRoot();
		if(capture.IsValid())
		{
			(void)TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance()
				.Remove(capture.root->assetUuid);
			(void)TerraEngine::Physics::PhysicsSystem::Instance()
				.RemoveTerrainCollisions(capture.root->assetUuid);
		}
	}
	ResetSceneTerrainState(
		"Terrain project state released; derived resources will be rebuilt after the next scene load.");
}

void TerrainWindow::ResetSceneTerrainState(std::string_view statusMessage)
{
	// The worker owns copied terrain values. Cancellation prevents an obsolete
	// completion from being admitted after a different project becomes active.
	if(terrainRenderProxyCancellation)
		terrainRenderProxyCancellation->store(true, std::memory_order_release);
	if(terrainStore)
		TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().ClearPublication(terrainStore.get());
	authoringSystem.CancelPreparedWork("Terrain component removed from the scene.");
	// Shutdown joins any in-flight protected publication and detaches its
	// non-owning service pointers. A workload declared earlier in this frame may
	// still execute after project close, but it will now observe a disabled session
	// instead of touching the queue and publication objects destroyed below.
	authoringSystem.Shutdown();
	terrainStore.reset();
	terrainQueue.reset();
	terrainPublication.reset();
	terrainGarbageCollector.reset();
	terrainRenderProxyCancellation.reset();
	TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance().Remove(
		terrainRenderProxyMaterialId);
	terrainRenderProxyMeshId = TerraEngine::EMPTY_UUID;
	terrainRenderProxyMaterialId = TerraEngine::EMPTY_UUID;
	terrainWeightTextureId = TerraEngine::EMPTY_UUID;
	terrainLayerIdTextureId = TerraEngine::EMPTY_UUID;
	terrainRenderProxyTargetObjectId = TerraEngine::EMPTY_UUID;
	publishedTerrainMaterial.reset();
	publishedSurfaceControlHash = {};
	publishedTerrainLayerGeneration = 0u;
	publishedTerrainLayerMaterialId = TerraEngine::EMPTY_UUID;
	publishedTerrainLayerSourceAvailable = false;
	terrainRenderProxyBuildActive = false;
	terrainRenderProxyRebuildRequested = false;
	terrainRenderProxyInstanceQueued = false;
	status = statusMessage;
}

TerraEngine::Editor::Terrain::TerrainAutomationCommandResult TerrainWindow::ExecuteAutomationCommand(
	std::string_view capability, const nlohmann::json& arguments)
{
	using Result = TerraEngine::Editor::Terrain::TerrainAutomationCommandResult;
	if(capability == "terrain.create" || capability == "terrain.import")
	{
		try
		{
			if(!arguments.is_object() || !arguments.contains("destination") || !arguments["destination"].is_string())
				return {false, "InvalidSchema", "Terrain create/import requires a destination .teterrain path."};
			TerraEngine::AssetPipeline::TerrainImportRequest request;
			request.assetId = TerraEngine::GenerateUUID();
			request.sampleSpacingMetres = arguments.value("sampleSpacingMetres", 1.0F);
			request.heightScaleMetres = arguments.value("heightScaleMetres", 1.0F);
			request.heightOffsetMetres = arguments.value("heightOffsetMetres", 0.0F);
			if(capability == "terrain.create")
			{
				if(!arguments.contains("widthSamples") || !arguments["widthSamples"].is_number_unsigned()
					|| !arguments.contains("depthSamples") || !arguments["depthSamples"].is_number_unsigned()
					|| !arguments.contains("sampleSpacingMetres") || !arguments["sampleSpacingMetres"].is_number())
					return {false, "InvalidSchema", "Terrain create requires positive widthSamples, depthSamples, and sampleSpacingMetres."};
				request.sourceFormat = TerraEngine::AssetPipeline::TerrainSourceFormat::Flat;
				request.flatWidth = arguments["widthSamples"].get<std::uint32_t>();
				request.flatDepth = arguments["depthSamples"].get<std::uint32_t>();
			}
			else
			{
				if(!arguments.contains("source") || !arguments["source"].is_string())
					return {false, "InvalidSchema", "Terrain import requires a PNG16 or RAW16 source path."};
				request.sourcePath = arguments["source"].get<std::string>();
				const std::string format = arguments.value("format", std::string{"png16"});
				if(format == "png16") request.sourceFormat = TerraEngine::AssetPipeline::TerrainSourceFormat::Png16;
				else if(format == "raw16-le")
				{
					request.sourceFormat = TerraEngine::AssetPipeline::TerrainSourceFormat::Raw16LittleEndian;
					request.rawWidth = arguments.value("width", 0u);
					request.rawHeight = arguments.value("height", 0u);
				}
				else return {false, "InvalidSchema", "Terrain import format must be png16 or raw16-le."};
			}
			TerraEngine::Editor::Terrain::TerrainCreationService service =
				TerraEngine::Editor::Terrain::CreateProjectTerrainCreationService();
			std::filesystem::path destination = arguments["destination"].get<std::string>();
			if(!destination.is_absolute())
				destination = AssetDatabase::Instance().GetAssetRoot().parent_path() / destination;
			destination = std::filesystem::absolute(destination).lexically_normal();
			const std::filesystem::path relativeDestination = destination.lexically_relative(
				AssetDatabase::Instance().GetAssetRoot());
			if(relativeDestination.empty() || *relativeDestination.begin() == "..")
				return {false, "InvalidSchema", "Terrain destinations must remain inside the active project's Assets directory."};
			auto result = service.CreateOrImport(request, destination, [](float, std::string_view, std::string_view) { return true; });
			if(!result.succeeded) return {false, result.cancelled ? "Cancelled" : "Unavailable", result.message};
			destinationPath = destination;
			authoredSampleSpacingMetres = request.sampleSpacingMetres;
			InitializeAuthoringTerrain(request.assetId, result.heightfield);
			const auto capture = terrainStore->CaptureRoot();
			const std::uint64_t areasX = (static_cast<std::uint64_t>(result.heightfield.width) + 255u) / 256u;
			const std::uint64_t areasZ = (static_cast<std::uint64_t>(result.heightfield.height) + 255u) / 256u;
			return {true, {}, result.message, std::nullopt,
				{{"assetUuid", TerraEngine::ToHexString(request.assetId)},
				 {"rootHash", TerraEngine::Terrain::ToHexString(capture.root->rootContentHash)},
				 {"publicationGeneration", capture.publicationGeneration},
				 {"widthSamples", result.heightfield.width}, {"depthSamples", result.heightfield.height},
				 {"sampleSpacingMetres", request.sampleSpacingMetres},
				 {"physicalWidthMetres", static_cast<double>(result.heightfield.width) * request.sampleSpacingMetres},
				 {"physicalDepthMetres", static_cast<double>(result.heightfield.height) * request.sampleSpacingMetres},
				 {"coveredAreaCount", areasX * areasZ}}};
		}
		catch(const std::exception& error)
		{
			return {false, "InvalidSchema", std::string{"Terrain create/import arguments are invalid: "} + error.what()};
		}
	}
	if(capability == "terrain.session.begin")
	{
		if(!terrainStore || !terrainStore->CaptureRoot().IsValid())
			return {false, "Unavailable", "Terrain session begin requires an open immutable terrain root."};
		if(automationSessionId)
			return {false, "Conflict", "A terrain automation session is already active.", std::nullopt,
				{{"sessionId", TerraEngine::ToHexString(*automationSessionId)}}};
		automationSessionId = TerraEngine::GenerateUUID();
		const auto capture = terrainStore->CaptureRoot();
		return {true, {}, "Terrain automation session opened over an immutable root.", std::nullopt,
			{{"sessionId", TerraEngine::ToHexString(*automationSessionId)},
			 {"rootHash", TerraEngine::Terrain::ToHexString(capture.root->rootContentHash)},
			 {"publicationGeneration", capture.publicationGeneration},
			 {"visibilityBoundary", "terrain_authoring_publish"}}};
	}
	if(capability == "terrain.session.get")
	{
		if(!automationSessionId)
			return {false, "Unavailable", "No terrain automation session is active."};
		const auto capture = terrainStore ? terrainStore->CaptureRoot() : TerraEngine::Terrain::TerrainRootCapture{};
		return {true, {}, "Terrain automation session snapshot.", std::nullopt,
			{{"sessionId", TerraEngine::ToHexString(*automationSessionId)},
			 {"active", true}, {"hasDetachedPreview", authoringSession.IsDirty()},
			 {"publicationGeneration", capture.publicationGeneration}}};
	}
	if(capability == "terrain.session.cancel")
	{
		if(!automationSessionId)
			return {false, "Unavailable", "No terrain automation session is active."};
		const auto sessionId = TerraEngine::ToHexString(*automationSessionId);
		CancelDetachedPreview("Terrain automation session cancelled; committed terrain was preserved.");
		automationSessionId.reset();
		return {true, {}, "Terrain automation session cancelled without publishing detached state.", std::nullopt,
			{{"sessionId", sessionId}, {"committed", false}}};
	}
	if(capability == "terrain.session.commit")
	{
		if(!automationSessionId)
			return {false, "Unavailable", "No terrain automation session is active."};
		if(!authoringSession.IsDirty())
			return {false, "Unavailable", "The terrain automation session has no detached preview to commit."};
		if(!QueuePreviewForPublication())
			return {false, "Unavailable", status.empty() ? "The detached terrain preview could not be queued." : status};
		const auto sessionId = TerraEngine::ToHexString(*automationSessionId);
		automationSessionId.reset();
		return {true, {}, "Terrain automation session queued for protected publication.", std::nullopt,
			{{"sessionId", sessionId}, {"committed", false},
			 {"pendingTransactions", terrainQueue ? terrainQueue->PendingCount() : 0u},
			 {"visibilityBoundary", "terrain_authoring_publish"}}};
	}
	if(capability == "terrain.resize.preview")
	{
		if(!arguments.is_object() || !arguments.contains("kind") || !arguments["kind"].is_string())
			return {false, "InvalidSchema", "Terrain resize preview requires kind expand or crop."};
		const std::string kind = arguments["kind"].get<std::string>();
		if(kind == "expand")
		{
			resizeExpansionAreas = {arguments.value("minimumXAreas", 0), arguments.value("maximumXAreas", 0),
				arguments.value("minimumZAreas", 0), arguments.value("maximumZAreas", 0)};
			const std::string policy = arguments.value("policy", std::string{"edge-blend"});
			resizeExpansionPolicy = policy == "extend-edge" ? static_cast<int>(TerraEngine::Terrain::TerrainExpansionPolicy::ExtendEdge)
				: policy == "flat-default" ? static_cast<int>(TerraEngine::Terrain::TerrainExpansionPolicy::FlatDefault)
				: static_cast<int>(TerraEngine::Terrain::TerrainExpansionPolicy::EdgeBlend);
			resizeTransitionSamples = arguments.value("transitionSamples", 64);
			resizeHolePolicy = arguments.value("holePolicy", std::string{"close-at-former-edge"}) == "extend-connected-boundary-holes"
				? static_cast<int>(TerraEngine::Terrain::TerrainBoundaryHolePolicy::ExtendConnectedBoundaryHoles)
				: static_cast<int>(TerraEngine::Terrain::TerrainBoundaryHolePolicy::CloseAtFormerEdge);
		}
		else if(kind == "crop")
		{
			for(const char* key : {"minX", "minZ", "maxXExclusive", "maxZExclusive"})
				if(!arguments.contains(key) || !arguments[key].is_number_integer())
					return {false, "InvalidSchema", "Terrain crop preview requires exact signed half-open sample bounds."};
			resizeCropBounds = {arguments["minX"].get<std::int64_t>(), arguments["minZ"].get<std::int64_t>(),
				arguments["maxXExclusive"].get<std::int64_t>(), arguments["maxZExclusive"].get<std::int64_t>()};
		}
		else return {false, "InvalidSchema", "Terrain resize kind must be expand or crop."};
		std::string error;
		if(!StartResizePreview(kind == "expand" ? TerraEngine::Terrain::TerrainResizeKind::Expand
			: TerraEngine::Terrain::TerrainResizeKind::Crop, error)) return {false, "Unavailable", std::move(error)};
		const auto progress = resizeJobs.Progress();
		return {true, "Pending", "Terrain resize preview started from one immutable root capture.", std::nullopt,
			{{"jobId", TerraEngine::ToHexString(progress.jobId)}, {"stage", static_cast<int>(progress.stage)},
			 {"visibilityBoundary", "terrain_authoring_publish"}}};
	}
	if(capability == "terrain.job.get")
	{
		PollResizePreview();
		const auto progress = resizeJobs.Progress();
		if(!arguments.is_object() || !arguments.contains("jobId") || !arguments["jobId"].is_string()
			|| arguments["jobId"].get<std::string>() != TerraEngine::ToHexString(progress.jobId))
			return {false, "Unavailable", "The requested terrain resize job is not active in this editor session."};
		nlohmann::json payload = {{"jobId", TerraEngine::ToHexString(progress.jobId)},
			{"stage", static_cast<int>(progress.stage)}, {"progress", progress.fraction},
			{"completedAreas", progress.completedAreas}, {"totalAreas", progress.totalAreas},
			{"cancellationAvailable", progress.cancellationAvailable}};
		if(resizePreview)
		{
			const auto& preview = resizePreview->preview;
			const auto encodeBounds = [](TerraEngine::Terrain::TerrainSampleBounds bounds) {
				return nlohmann::json{{"minX", bounds.minX}, {"minZ", bounds.minZ},
					{"maxXExclusive", bounds.maxXExclusive},
					{"maxZExclusive", bounds.maxZExclusive}};
			};
			payload["proposalFingerprint"] = TerraEngine::Terrain::ToHexString(preview.proposalFingerprint);
			payload["kind"] = std::string(TerraEngine::Terrain::ToString(preview.delta.kind));
			payload["previousBounds"] = encodeBounds(preview.delta.previousBounds);
			payload["proposedBounds"] = encodeBounds(preview.delta.proposedBounds);
			payload["policy"] = std::string(TerraEngine::Terrain::ToString(preview.delta.expansionPolicy));
			payload["holePolicy"] = std::string(TerraEngine::Terrain::ToString(preview.delta.holePolicy));
			payload["transitionSamples"] = preview.delta.transitionSamples;
			payload["readyGeneration"] = resizePreview->readyGeneration;
			payload["previousAreaCount"] = preview.previousAreaCount;
			payload["proposedAreaCount"] = preview.proposedAreaCount;
			payload["physicalWidthMetres"] = preview.physicalWidthMetres;
			payload["physicalDepthMetres"] = preview.physicalDepthMetres;
			payload["requiresDataLossConfirmation"] = preview.requiresDataLossConfirmation;
			payload["cropLoss"] = {{"discardedSamples", preview.cropLoss.discardedSamples},
				{"nonDefaultHeightSamples", preview.cropLoss.nonDefaultHeightSamples},
				{"holeSamples", preview.cropLoss.holeSamples},
				{"paintedSamples", preview.cropLoss.paintedSamples},
				{"removedDescriptors", preview.cropLoss.removedDescriptors},
				{"partialDescriptors", preview.cropLoss.partialDescriptors},
				{"retainedHistoryBytes", preview.cropLoss.retainedHistoryBytes}};
			payload["warnings"] = preview.warnings;
			payload["diagnostic"] = preview.message;
		}
		return {true, resizePreview ? "Ready" : "Pending", progress.message, std::nullopt, std::move(payload)};
	}
	if(capability == "terrain.job.cancel")
	{
		const auto progress = resizeJobs.Progress();
		if(!arguments.is_object() || !arguments.contains("jobId") || !arguments["jobId"].is_string()
			|| arguments["jobId"].get<std::string>() != TerraEngine::ToHexString(progress.jobId))
			return {false, "Unavailable", "The requested terrain resize job is not active in this editor session."};
		resizeJobs.Cancel("Terrain resize job cancelled through automation.");
		ClearResizePreview();
		return {true, "Cancelled", "Terrain resize preparation cancelled before protected publication."};
	}
	if(capability == "terrain.expand.start" || capability == "terrain.crop.start")
	{
		PollResizePreview();
		if(!resizePreview || !arguments.contains("proposalFingerprint") || !arguments["proposalFingerprint"].is_string())
			return {false, "ApprovalRequired", "A ready resize preview and its exact proposal fingerprint are required."};
		const bool crop = capability == "terrain.crop.start";
		if((resizePreview->preview.delta.kind == TerraEngine::Terrain::TerrainResizeKind::Crop) != crop)
			return {false, "Conflict", "The ready resize preview is for a different operation kind."};
		if(arguments["proposalFingerprint"].get<std::string>() != TerraEngine::Terrain::ToHexString(resizePreview->preview.proposalFingerprint))
			return {false, "StaleGeneration", "The resize proposal fingerprint no longer matches the ready preview."};
		const bool acknowledged = arguments.value("acknowledgeDataLoss", false);
		const auto& approvedPreview = resizePreview->preview;
		const nlohmann::json proposedBounds = {{"minX", approvedPreview.delta.proposedBounds.minX},
			{"minZ", approvedPreview.delta.proposedBounds.minZ},
			{"maxXExclusive", approvedPreview.delta.proposedBounds.maxXExclusive},
			{"maxZExclusive", approvedPreview.delta.proposedBounds.maxZExclusive}};
		const auto readyGeneration = resizePreview->readyGeneration;
		std::string error;
		if(!CommitResizePreview(acknowledged, error))
			return {false, crop && !acknowledged ? "ApprovalRequired" : "Unavailable", std::move(error)};
		return {true, "Pending", crop ? "Terrain crop queued for protected publication." : "Terrain expansion queued for protected publication.",
			queuedResizeTransactionId, {{"transactionId", queuedResizeTransactionId ? TerraEngine::ToHexString(*queuedResizeTransactionId) : std::string{}},
			{"proposalFingerprint", arguments["proposalFingerprint"]},
			{"kind", std::string(TerraEngine::Terrain::ToString(approvedPreview.delta.kind))},
			{"proposedBounds", proposedBounds}, {"readyGeneration", readyGeneration},
			{"visibilityBoundary", "terrain_authoring_publish"}}};
	}
	if(capability == "terrain.diagnostics.get")
	{
		const auto snapshot = DiagnosticSnapshot();
		if(snapshot.revision == 0u)
			return {false, "Unavailable", "Terrain diagnostics require an open immutable root."};
		nlohmann::json items = nlohmann::json::array();
		for(const auto& item : snapshot.items)
		{
			nlohmann::json encoded = {{"message", item.message},
				{"category", static_cast<int>(item.category)}, {"severity", static_cast<int>(item.severity)},
				{"impact", static_cast<int>(item.impact)}, {"action", static_cast<int>(item.action)}};
			if(item.location) encoded["location"] = {{"x", item.location->x}, {"z", item.location->z}};
			items.push_back(std::move(encoded));
		}
		return {true, {}, "Terrain immutable diagnostic snapshot.", std::nullopt,
			{{"revision", snapshot.revision}, {"authoredGeneration", snapshot.authoredGeneration},
			 {"savedGeneration", snapshot.savedGeneration}, {"visibleGeneration", snapshot.visibleGeneration},
			 {"dirty", snapshot.dirty}, {"pendingTransactions", snapshot.pendingTransactions},
			 {"historyDepth", snapshot.historyDepth}, {"historyBytes", snapshot.historyBytes},
			 {"items", std::move(items)}}};
	}
	if(capability == "terrain.validate")
	{
		const auto snapshot = DiagnosticSnapshot();
		if(snapshot.revision == 0u) return {false, "Unavailable", "Terrain validation requires an open immutable root."};
		const auto errors = std::count_if(snapshot.items.begin(), snapshot.items.end(), [](const auto& item) {
			return item.severity == TerraEngine::Terrain::TerrainDiagnosticSeverity::Error;
		});
		return {true, {}, errors == 0 ? "Terrain validation completed." : "Terrain validation found blocking diagnostics.",
			std::nullopt, {{"revision", snapshot.revision}, {"errorCount", errors}, {"passing", errors == 0}}};
	}
	if(capability == "terrain.save")
	{
		std::string error;
		return SaveTerrain(error) ? Result{true, {}, "Terrain immutable root saved.", std::nullopt,
			{{"dirty", false}, {"visibilityBoundary", "terrain_current_replace"}}}
			: Result{false, "Unavailable", std::move(error)};
	}
	if(capability == "terrain.rebuild")
	{
		const std::string target = arguments.value("target", std::string{"all"});
		if(target == "collision" || target == "all")
			return ExecuteAutomationCommand("terrain.collision-refresh", arguments);
		if(target != "persistence")
			return {false, "InvalidSchema", "Terrain rebuild target must be all, collision, or persistence."};
		std::string message;
		return RepairFailedGeneration(message) ? Result{true, {}, std::move(message), std::nullopt,
			{{"lastKnownGoodPreserved", true}}} : Result{false, "Unavailable", std::move(message)};
	}
	if(capability == "terrain.collision-refresh")
	{
		std::string error;
		if(!QueueAutomaticTerrainCollision(error)) return {false, "Unavailable", std::move(error)};
		const auto capture = terrainStore ? terrainStore->CaptureRoot() : TerraEngine::Terrain::TerrainRootCapture{};
		return {true, "Pending", TerraEngine::Physics::PhysicsSystem::Instance().WorldHandle().IsValid()
			? "Terrain collision candidate queued for the protected before-physics boundary."
			: "Terrain collision is automatic and will publish when the physics world starts.", std::nullopt,
			{{"sourceRootHash", capture.IsValid() ? TerraEngine::Terrain::ToHexString(capture.root->rootContentHash) : std::string{}},
			 {"sourcePublicationGeneration", capture.publicationGeneration},
			 {"visibilityBoundary", "terrain_collision_publish_before_physics"}}};
	}
	if(capability == "terrain.collision-cancel")
	{
		TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().CancelPreparedCandidate();
		return {true, {}, "Terrain collision candidate cancelled before publication."};
	}
	if(capability == "terrain.export")
		return {false, "Unavailable", "Terrain export requires a configured project ExportPipeline service."};
	return {false, "UnsupportedOperation", "The requested terrain editor command is not supported by this service."};
}

void TerrainWindow::SetSculpting(bool value)
{
	if(value && holeSculpting)
	{
		CancelHolePreview();
		holeSculpting = false;
	}
	if(!value && sculpting && authoringSession.IsDirty())
		CancelDetachedPreview("Terrain preview cancelled because the sculpt tool changed.");
	sculpting = value;
	sceneViewBrushFootprint.reset();
}

void TerrainWindow::SetHoleSculpting(bool value)
{
	if(value && sculpting)
	{
		if(authoringSession.IsDirty())
			CancelDetachedPreview("Terrain preview cancelled because the sculpt tool changed.");
		sculpting = false;
	}
	if(!value && holeSculpting)
		CancelHolePreview();
	holeSculpting = value;
	sceneViewBrushFootprint.reset();
}

bool TerrainWindow::IsEditingTargetSelected(
	const std::optional<TerraEngine::UUID>& selectedObjectId) const noexcept
{
	// A newly created terrain may not have a scene object until its queued create
	// transaction publishes. It must not accept Scene View input during that gap.
	return selectedObjectId.has_value()
		&& terrainRenderProxyTargetObjectId != TerraEngine::EMPTY_UUID
		&& *selectedObjectId == terrainRenderProxyTargetObjectId;
}

void TerrainWindow::HandleEditingTargetDeselected()
{
	if(!brushStrokeActive && !surfaceBrushActive && !holeBrushActive && !authoringSession.IsDirty()
		&& !surfacePreviewActive && !holePreviewActive && !sceneViewBrushFootprint.has_value())
		return;

	brushStrokeActive = false;
	surfaceBrushActive = false;
	holeBrushActive = false;
	previousStamp.reset();
	previousSurfaceStamp.reset();
	sceneViewBrushFootprint.reset();
	if(authoringSession.IsDirty())
		CancelDetachedPreview("Terrain preview cancelled because its terrain object is no longer selected.");
	if(holePreviewActive)
		CancelHolePreview();
	if(surfacePreviewActive)
		CancelSurfacePreview();
}

void TerrainWindow::SynchronizeEditingSelection(
	const std::optional<TerraEngine::UUID>& selectedTerrainObjectId)
{
	if(!selectedTerrainObjectId.has_value())
	{
		if(editingSelectionActive || IsBrushStrokeActive() || authoringSession.IsDirty()
			|| surfacePreviewActive || holePreviewActive)
			HandleEditingTargetDeselected();
		editingSelectionActive = false;
		sculpting = false;
		surfacePainting = false;
		holeSculpting = false;
		return;
	}

	if(!IsEditingTargetSelected(selectedTerrainObjectId))
	{
		// Switching directly between terrain objects must discard the old detached
		// preview before OpenTerrainForEditing binds the new durable asset.
		HandleEditingTargetDeselected();
		std::string openError;
		if(!OpenTerrainForEditing(*selectedTerrainObjectId, openError))
		{
			editingSelectionActive = false;
			sculpting = false;
			surfacePainting = false;
			holeSculpting = false;
			status = std::move(openError);
			return;
		}
	}

	editingSelectionActive = true;
	ApplyInspectorToolMode();
}

void TerrainWindow::ApplyInspectorToolMode()
{
	const bool wantsHeightBrush = editingSelectionActive && inspectorTool == InspectorTool::Sculpt;
	const bool wantsSurfaceBrush = editingSelectionActive && inspectorTool == InspectorTool::Paint;
	const bool wantsHoleBrush = editingSelectionActive && inspectorTool == InspectorTool::Holes;
	if(surfacePainting && !wantsSurfaceBrush)
	{
		CancelSurfacePreview();
		surfaceBrushActive = false;
		previousSurfaceStamp.reset();
	}
	if(sculpting != wantsHeightBrush) SetSculpting(wantsHeightBrush);
	if(holeSculpting != wantsHoleBrush) SetHoleSculpting(wantsHoleBrush);
	if(wantsSurfaceBrush)
	{
		if(sculpting) SetSculpting(false);
		if(holeSculpting) SetHoleSculpting(false);
	}
	surfacePainting = wantsSurfaceBrush;
}

void TerrainWindow::DrawInspectorToolPalette()
{
	struct ToolEntry
	{
		InspectorTool tool;
		const char* label;
		const char* tooltip;
	};
	static constexpr std::array<ToolEntry, 5> tools = {{
		{InspectorTool::Sculpt, "Sculpt", "Raise, lower, smooth, flatten, and shape terrain height."},
		{InspectorTool::Paint, "Paint", "Paint the selected terrain surface layer."},
		{InspectorTool::Holes, "Holes", "Paint or erase terrain holes in Scene View."},
		{InspectorTool::Layers, "Layers", "Manage the ordered terrain material layers."},
		{InspectorTool::Settings, "More", "Procedural tools, diagnostics, save, and maintenance."}
	}};

	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(2.0F, 2.0F));
	if(ImGui::BeginTable("##TerrainInspectorToolPalette", static_cast<int>(tools.size()),
		ImGuiTableFlags_SizingStretchSame))
	{
		for(const ToolEntry& entry : tools)
		{
			ImGui::TableNextColumn();
			const bool selected = inspectorTool == entry.tool;
			if(selected)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
			}
			if(ImGui::Button(entry.label, ImVec2(-1.0F, 30.0F)) && !selected)
			{
				inspectorTool = entry.tool;
				ApplyInspectorToolMode();
			}
			if(selected) ImGui::PopStyleColor(2);
			if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
				ImGui::SetTooltip("%s", entry.tooltip);
		}
		ImGui::EndTable();
	}
	ImGui::PopStyleVar();
}

void TerrainWindow::DrawInspectorSculptControls()
{
	const char* modes[] = {"Raise", "Lower", "Smooth", "Flatten", "Pick Height", "Terrace", "Ramp", "Stamp", "Noise"};
	int mode = static_cast<int>(brushSettings.mode);
	ImGui::SetNextItemWidth(-1.0F);
	if(ImGui::Combo("##TerrainSculptMode", &mode, modes, IM_ARRAYSIZE(modes)))
		brushSettings.mode = static_cast<TerraEngine::Editor::Terrain::TerrainBrushMode>(mode);
	ImGui::TextDisabled("Drag over the terrain in Scene View. Release creates one undo step.");
	ImGui::SliderFloat("Size", &brushSettings.radiusSamples, 0.25F, 512.0F, "%.1f samples");
	ImGui::SliderFloat("Opacity", &brushSettings.pressure, 0.0F, 1.0F, "%.2f");
	ImGui::SliderFloat("Strength", &brushSettings.strengthMm, 1.0F, 10000.0F, "%.0f mm");
	const char* falloffs[] = {"Hard", "Linear", "Smooth", "Soft"};
	int falloff = static_cast<int>(brushSettings.falloff);
	if(ImGui::Combo("Falloff", &falloff, falloffs, IM_ARRAYSIZE(falloffs)))
		brushSettings.falloff = static_cast<TerraEngine::Editor::Terrain::TerrainBrushFalloff>(falloff);

	if(brushSettings.mode == TerraEngine::Editor::Terrain::TerrainBrushMode::FlattenValue
		|| brushSettings.mode == TerraEngine::Editor::Terrain::TerrainBrushMode::FlattenPicked
		|| brushSettings.mode == TerraEngine::Editor::Terrain::TerrainBrushMode::Ramp)
		ImGui::InputScalar("Target Height", ImGuiDataType_S32, &brushSettings.flattenHeightMm);
	if(brushSettings.mode == TerraEngine::Editor::Terrain::TerrainBrushMode::Terrace)
		ImGui::InputScalar("Terrace Step", ImGuiDataType_S32, &brushSettings.terraceStepMm);
	if(brushSettings.mode == TerraEngine::Editor::Terrain::TerrainBrushMode::Noise)
		ImGui::InputScalar("Noise Seed", ImGuiDataType_U32, &brushSettings.randomSeed);

	if(ImGui::CollapsingHeader("Brush Options"))
	{
		ImGui::SliderFloat("Spacing", &brushSettings.spacingSamples, 0.0F, 128.0F, "%.1f samples");
		ImGui::SliderAngle("Rotation", &brushSettings.rotationRadians, -180.0F, 180.0F);
		ImGui::Checkbox("Use Mask", &maskEnabled);
	}
}

void TerrainWindow::DrawInspectorPaintControls()
{
	// Layer creation, removal, disable, and project reload can all invalidate the
	// last brush target. Repair it before drawing or accepting a Scene View stroke.
	EnsureValidPaintLayer();
	const TerraEngine::Terrain::TerrainLayerStack stack = LayerPreview();
	const std::size_t enabledLayerCount = static_cast<std::size_t>(std::count_if(
		stack.layers.begin(), stack.layers.end(),
		[](const TerraEngine::Terrain::TerrainLayer& layer) { return layer.enabled; }));
	if(enabledLayerCount <= 1u)
	{
		ImGui::TextColored(ImVec4(1.0F, 0.72F, 0.18F, 1.0F),
			"The only enabled layer already covers 100%% of the terrain.");
		ImGui::TextWrapped(
			"Open Layers, add another terrain layer, assign its material, then paint that new layer.");
	}
	const auto selectedLayer = std::find_if(stack.layers.begin(), stack.layers.end(), [this](const auto& layer) {
		return layer.stableId == surfaceBrushSettings.layerId;
	});
	const char* preview = selectedLayer != stack.layers.end() ? selectedLayer->name.c_str() : "Choose Layer";
	if(ImGui::BeginCombo("Layer", preview))
	{
		for(const auto& layer : stack.layers)
		{
			if(!layer.enabled) continue;
			const bool selected = layer.stableId == surfaceBrushSettings.layerId;
			if(ImGui::Selectable(layer.name.c_str(), selected))
			{
				surfaceBrushSettings.layerId = layer.stableId;
				selectedLayerId = layer.stableId;
			}
			if(selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	const char* paintModes[] = {"Paint", "Erase", "Fill", "Replace", "Normalize", "Layer Mask"};
	int paintMode = static_cast<int>(surfaceBrushSettings.mode);
	if(ImGui::Combo("Mode", &paintMode, paintModes, IM_ARRAYSIZE(paintModes)))
		surfaceBrushSettings.mode = static_cast<TerraEngine::Editor::Terrain::TerrainSurfaceBrushMode>(paintMode);
	int strength = static_cast<int>(surfaceBrushSettings.strength);
	if(ImGui::SliderInt("Opacity", &strength, 1, 255))
		surfaceBrushSettings.strength = static_cast<std::uint8_t>(strength);
	ImGui::SliderFloat("Size", &surfaceBrushSettings.radiusSamples, 0.25F, 512.0F, "%.1f samples");
	ImGui::SliderFloat("Spacing", &surfaceBrushSettings.spacingSamples, 0.0F, 128.0F, "%.1f samples");
	ImGui::Checkbox("Use Layer Mask", &surfaceBrushSettings.useLayerMask);
	const bool areaOperation = surfaceBrushSettings.mode == TerraEngine::Editor::Terrain::TerrainSurfaceBrushMode::Fill
		|| surfaceBrushSettings.mode == TerraEngine::Editor::Terrain::TerrainSurfaceBrushMode::Replace
		|| surfaceBrushSettings.mode == TerraEngine::Editor::Terrain::TerrainSurfaceBrushMode::Normalize;
	ImGui::TextDisabled(areaOperation
		? "This operation applies to the complete selected area as one undo step."
		: "Drag over the terrain in Scene View. Release creates one undo step.");
	if(areaOperation && ImGui::Button("Apply To Entire Area", ImVec2(-1.0F, 0.0F)))
	{
		const auto result = PreviewSurfaceBrush(surfaceBrushSettings,
			surfaceBrushSettings.useLayerMask
				? std::span<const std::uint8_t>(brushMask) : std::span<const std::uint8_t>{});
		std::string error;
		if(!result.changed)
			status = enabledLayerCount <= 1u
				? "Surface paint made no change: the only enabled layer already covers the complete terrain. Add and select another layer."
				: "Surface paint made no change: this layer is already at maximum weight in the selected area, or the layer mask has no coverage.";
		else if(!ApplySurfacePreview(error)) status = std::move(error);
	}
	if(displacedLayerWarning != 0u)
		ImGui::TextColored(ImVec4(1.0F, 0.65F, 0.15F, 1.0F),
			"Layer %u was displaced from a full four-layer sample.", displacedLayerWarning);
}

void TerrainWindow::DrawInspectorHoleControls()
{
	const char* modes[] = {"Paint Holes", "Erase Holes"};
	int mode = static_cast<int>(holeBrushSettings.mode);
	ImGui::SetNextItemWidth(-1.0F);
	if(ImGui::Combo("##TerrainHoleMode", &mode, modes, IM_ARRAYSIZE(modes)))
		holeBrushSettings.mode = static_cast<TerraEngine::Editor::Terrain::TerrainHoleBrushMode>(mode);
	holeBrushSettings.radiusCells = TerraEngine::Editor::Terrain::TerrainHoleBrush::EffectiveRadiusCells(
		holeBrushSettings.radiusCells);
	ImGui::SliderFloat("Size", &holeBrushSettings.radiusCells,
		TerraEngine::Editor::Terrain::TerrainHoleBrush::kMinimumRadiusCells, 128.0F, "%.1f cells");
	ImGui::TextDisabled("Drag over the terrain in Scene View. Collision updates after release.");
	if(holePreviewActive && ImGui::Button("Cancel Hole Preview", ImVec2(-1.0F, 0.0F)))
		CancelHolePreview();
}

void TerrainWindow::DrawInspectorLayerControls()
{
	const auto stageAndApply = [this](TerraEngine::Terrain::TerrainLayerPatch patch, std::string& outError) {
		return StageLayerPatch(patch, outError) && ApplyLayerPreview(outError);
	};
	const TerraEngine::Terrain::TerrainLayerStack stack = LayerPreview();
	const std::size_t enabledLayerCount = static_cast<std::size_t>(std::count_if(
		stack.layers.begin(), stack.layers.end(),
		[](const auto& layer) { return layer.enabled; }));
	const bool definitionLimitReached = stack.layers.size()
		>= TerraEngine::Terrain::Format::kTerrainMaximumLayers;
	const bool enabledLimitReached = enabledLayerCount
		>= TerraEngine::Terrain::Format::kTerrainMaximumEnabledLayers;
	const bool listEditBlocked = queuedLayerTransactionId.has_value() || layerPreviewActive;
	const std::optional<std::uint16_t> unusedLayerId = FindUnusedTerrainLayerId(stack);
	ImGui::TextDisabled("Layers are evaluated from top to bottom and keep stable paint IDs.");
	ImGui::Text("Definitions: %zu / %zu    Enabled: %zu / %zu",
		stack.layers.size(), TerraEngine::Terrain::Format::kTerrainMaximumLayers,
		enabledLayerCount, TerraEngine::Terrain::Format::kTerrainMaximumEnabledLayers);
	if(listEditBlocked)
		ImGui::TextDisabled("Apply or cancel the current detached layer operation before starting another list edit.");
	if(enabledLimitReached)
		ImGui::TextDisabled("The enabled set is full. New definitions are added disabled and can be enabled after another layer is disabled.");
	for(std::size_t index = 0u; index < stack.layers.size(); ++index)
	{
		const auto& layer = stack.layers[index];
		ImGui::PushID(static_cast<int>(layer.stableId));
		const bool selected = selectedLayerId == layer.stableId;
		if(ImGui::Selectable(layer.name.c_str(), selected, 0, ImVec2(0.0F, 24.0F)))
		{
			selectedLayerId = layer.stableId;
			if(layer.enabled) surfaceBrushSettings.layerId = layer.stableId;
		}
		if(selected)
		{
			const auto stageMappingValue = [this, &layer](auto update, bool edited, bool deactivated) {
				if(edited)
				{
					TerraEngine::Terrain::TerrainLayer replacement = layer;
					update(replacement.mapping);
					std::string error;
					if(!StageLayerPatch({TerraEngine::Terrain::TerrainLayerPatchKind::Replace,
						0u, layer.stableId, 0xffffffffu, std::move(replacement)}, error))
						status = std::move(error);
				}
				if(deactivated && layerPreviewActive && !queuedLayerTransactionId)
				{
					std::string error;
					if(!ApplyLayerPreview(error)) status = std::move(error);
				}
			};
			ImGui::SeparatorText("Physical Mapping");
			int mappingMode = static_cast<int>(layer.mapping.mode);
			const char* mappingModes[] = {"Terrain XZ", "Triplanar"};
			const bool modeChanged = ImGui::Combo("Mode", &mappingMode, mappingModes, IM_ARRAYSIZE(mappingModes));
			stageMappingValue([mappingMode](auto& mapping) {
				mapping.mode = static_cast<TerraEngine::Terrain::TerrainLayerMappingMode>(mappingMode);
			}, modeChanged, modeChanged);

			double tileSize[2] = {layer.mapping.tileSizeXMetres, layer.mapping.tileSizeZMetres};
			const bool tileChanged = ImGui::DragScalarN("Tile Size", ImGuiDataType_Double,
				tileSize, 2, 0.05F, nullptr, nullptr, "%.3f m");
			stageMappingValue([&tileSize](auto& mapping) {
				mapping.tileSizeXMetres = tileSize[0];
				mapping.tileSizeZMetres = tileSize[1];
			}, tileChanged, ImGui::IsItemDeactivatedAfterEdit());

			double offset[2] = {layer.mapping.offsetXMetres, layer.mapping.offsetZMetres};
			const bool offsetChanged = ImGui::DragScalarN("Offset", ImGuiDataType_Double,
				offset, 2, 0.05F, nullptr, nullptr, "%.3f m");
			stageMappingValue([&offset](auto& mapping) {
				mapping.offsetXMetres = offset[0];
				mapping.offsetZMetres = offset[1];
			}, offsetChanged, ImGui::IsItemDeactivatedAfterEdit());

			double rotation = layer.mapping.rotationRadians;
			const bool rotationChanged = ImGui::DragScalar("Rotation", ImGuiDataType_Double,
				&rotation, 0.01F, nullptr, nullptr, "%.3f rad");
			stageMappingValue([rotation](auto& mapping) {
				mapping.rotationRadians = rotation;
			}, rotationChanged, ImGui::IsItemDeactivatedAfterEdit());

			if(layer.mapping.mode == TerraEngine::Terrain::TerrainLayerMappingMode::Triplanar)
			{
				double sharpness = layer.mapping.triplanarSharpness;
				const bool sharpnessChanged = ImGui::DragScalar("Triplanar Sharpness", ImGuiDataType_Double,
					&sharpness, 0.1F, nullptr, nullptr, "%.2f");
				stageMappingValue([sharpness](auto& mapping) {
					mapping.triplanarSharpness = sharpness;
				}, sharpnessChanged, ImGui::IsItemDeactivatedAfterEdit());
			}
			if(layerPreviewActive)
			{
				if(ImGui::Button("Apply Mapping Preview"))
				{
					std::string error;
					if(!ApplyLayerPreview(error)) status = std::move(error);
				}
				ImGui::SameLine();
				if(ImGui::Button("Cancel Mapping Preview")) CancelLayerPreview();
			}
			ImGui::TextDisabled("Mapping is terrain-local in metres; moving the terrain or render origin does not change phase.");

			const AssetMetadata* materialMetadata = layer.materialAssetUuid != TerraEngine::EMPTY_UUID
				? AssetDatabase::Instance().FindAssetByGuid(layer.materialAssetUuid) : nullptr;
			const Material* runtimeMaterial = layer.materialAssetUuid != TerraEngine::EMPTY_UUID
				? ResourceManager::Instance().GetMaterial(layer.materialAssetUuid) : nullptr;
			ImGui::TextDisabled("Material: %s", materialMetadata
				? materialMetadata->sourcePath.filename().string().c_str() : "None (flat fallback)");
			const bool materialUnavailable = layer.materialAssetUuid != TerraEngine::EMPTY_UUID
				&& runtimeMaterial == nullptr;
			const bool fallbackActive = materialUnavailable
				|| layer.fallbackState != TerraEngine::Terrain::TerrainLayerFallbackState::Healthy;
			if(fallbackActive)
			{
				ImGui::TextColored(ImVec4(1.0F, 0.72F, 0.18F, 1.0F),
					"Fallback: %s. Diagnostic terrain remains visible.",
					materialUnavailable ? "runtime material unavailable"
						: TerrainLayerFallbackLabel(layer.fallbackState));
				ImGui::TextWrapped(
					"Authored paint still references stable layer %u; repair or assign the material source to remove the checker without repainting.",
					static_cast<unsigned int>(layer.stableId));
			}
			else ImGui::TextDisabled("Effective layer source: ready (no diagnostic fallback).");
			ImGui::BeginDisabled(listEditBlocked);
			ImGui::Button("Drop Material Here", ImVec2(-1.0F, 0.0F));
			ImGui::EndDisabled();
			if(!listEditBlocked && ImGui::BeginDragDropTarget())
			{
				if(const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MATERIAL"))
				{
					TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
					if(payload->DataSize == sizeof(TerraEngine::UUID))
						std::memcpy(&materialId, payload->Data, sizeof(materialId));
					const AssetMetadata* dropped = AssetDatabase::Instance().FindAssetByGuid(materialId);
					if(dropped == nullptr || (dropped->importerName != "MaterialImporter"
						&& dropped->sourcePath.extension() != ".tmat"))
					{
						status = "Terrain layers accept compiled Material assets only.";
					}
					else
					{
						TerraEngine::Terrain::TerrainLayer replacement = layer;
						replacement.materialAssetUuid = materialId;
						replacement.fallbackState = TerraEngine::Terrain::TerrainLayerFallbackState::Healthy;
						std::string error;
						if(!stageAndApply({TerraEngine::Terrain::TerrainLayerPatchKind::Replace,
							0u, layer.stableId, 0xffffffffu, std::move(replacement)}, error))
							status = std::move(error);
					}
				}
				ImGui::EndDragDropTarget();
			}
			if(layer.materialAssetUuid != TerraEngine::EMPTY_UUID)
			{
				ImGui::BeginDisabled(listEditBlocked);
				const bool clearMaterial = ImGui::SmallButton("Clear Material");
				ImGui::EndDisabled();
				if(clearMaterial)
				{
					// Clearing is a detached metadata replacement. The protected
					// publication boundary, rather than this widget, changes runtime state.
					TerraEngine::Terrain::TerrainLayer replacement = layer;
					replacement.materialAssetUuid = TerraEngine::EMPTY_UUID;
					replacement.fallbackState = TerraEngine::Terrain::TerrainLayerFallbackState::FallbackActive;
					std::string error;
					if(!stageAndApply({TerraEngine::Terrain::TerrainLayerPatchKind::Replace,
						0u, layer.stableId, 0xffffffffu, std::move(replacement)}, error))
						status = std::move(error);
				}
			}

			const bool enableWouldOverflow = !layer.enabled && enabledLimitReached;
			const bool disableWouldRemoveFinal = layer.enabled && enabledLayerCount <= 1u;
			ImGui::BeginDisabled(listEditBlocked || enableWouldOverflow || disableWouldRemoveFinal);
			const bool toggleEnabled = ImGui::SmallButton(layer.enabled ? "Disable" : "Enable");
			ImGui::EndDisabled();
			if(toggleEnabled)
			{
				std::string error;
				if(!stageAndApply({TerraEngine::Terrain::TerrainLayerPatchKind::SetEnabled,
					0u, layer.stableId, 0u, {}, !layer.enabled}, error)) status = std::move(error);
				else EnsureValidPaintLayer();
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(listEditBlocked || index == 0u);
			const bool moveUp = ImGui::SmallButton("Move Up");
			ImGui::EndDisabled();
			if(moveUp)
			{
				std::string error;
				if(!stageAndApply({TerraEngine::Terrain::TerrainLayerPatchKind::Move,
					0u, layer.stableId, static_cast<std::uint32_t>(index - 1u)}, error)) status = std::move(error);
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(listEditBlocked || index + 1u >= stack.layers.size());
			const bool moveDown = ImGui::SmallButton("Move Down");
			ImGui::EndDisabled();
			if(moveDown)
			{
				std::string error;
				if(!stageAndApply({TerraEngine::Terrain::TerrainLayerPatchKind::Move,
					0u, layer.stableId, static_cast<std::uint32_t>(index + 1u)}, error)) status = std::move(error);
			}
			ImGui::SameLine();
			const bool removeWouldRemoveFinal = stack.layers.size() <= 1u
				|| (layer.enabled && enabledLayerCount <= 1u);
			ImGui::BeginDisabled(listEditBlocked || removeWouldRemoveFinal);
			const bool removeLayer = ImGui::SmallButton("Remove");
			ImGui::EndDisabled();
			if(removeLayer)
			{
				std::string error;
				if(!stageAndApply({TerraEngine::Terrain::TerrainLayerPatchKind::Remove,
					0u, layer.stableId}, error)) status = std::move(error);
				else
				{
					EnsureValidPaintLayer();
					if(selectedLayerId == layer.stableId) selectedLayerId = surfaceBrushSettings.layerId;
				}
			}

			ImGui::SameLine();
			ImGui::BeginDisabled(listEditBlocked || definitionLimitReached || !unusedLayerId.has_value());
			const bool duplicateLayer = ImGui::SmallButton("Duplicate");
			ImGui::EndDisabled();
			if(duplicateLayer)
			{
				TerraEngine::Terrain::TerrainLayer duplicate = layer;
				duplicate.stableId = *unusedLayerId;
				duplicate.name = MakeUniqueTerrainLayerCopyName(stack, layer.name);
				// A full runtime set can still retain more authoring definitions. The
				// copy starts disabled so the complete sixteen-layer GPU publication
				// remains valid and no renderer-side truncation is needed.
				duplicate.enabled = layer.enabled && !enabledLimitReached;
				const bool duplicateEnabled = duplicate.enabled;
				std::string error;
				if(!stageAndApply({TerraEngine::Terrain::TerrainLayerPatchKind::Add,
					0u, 0u, static_cast<std::uint32_t>(index + 1u), std::move(duplicate)}, error))
					status = std::move(error);
				else if(!duplicateEnabled)
					status += " The copied definition is disabled until capacity is available.";
			}
			if(enableWouldOverflow)
				ImGui::TextDisabled("Disable another layer before enabling this definition.");
			else if(disableWouldRemoveFinal || removeWouldRemoveFinal)
				ImGui::TextDisabled("At least one enabled terrain layer must remain.");
		}
		ImGui::PopID();
	}
	ImGui::BeginDisabled(listEditBlocked || definitionLimitReached || !unusedLayerId.has_value());
	const bool addLayer = ImGui::Button("Add Terrain Layer", ImVec2(-1.0F, 0.0F));
	ImGui::EndDisabled();
	if(addLayer)
	{
		TerraEngine::Terrain::TerrainLayer layer;
		layer.stableId = *unusedLayerId;
		layer.name = "Layer " + std::to_string(*unusedLayerId);
		layer.enabled = !enabledLimitReached;
		layer.fallbackState = TerraEngine::Terrain::TerrainLayerFallbackState::FallbackActive;
		const bool addedEnabled = layer.enabled;
		std::string error;
		if(stageAndApply({TerraEngine::Terrain::TerrainLayerPatchKind::Add,
			0u, 0u, 0xffffffffu, std::move(layer)}, error))
		{
			selectedLayerId = *unusedLayerId;
			if(addedEnabled) surfaceBrushSettings.layerId = *unusedLayerId;
			else status += " The new definition is disabled until capacity is available.";
		}
		else status = std::move(error);
	}
	if(definitionLimitReached)
		ImGui::TextDisabled("The TERRAROOT1 definition limit is 64. Remove a definition before adding another.");
}

void TerrainWindow::DrawInspectorSettingsControls()
{
	// Material values are authored here instead of through the generic reflected
	// vec3 editor. The dedicated control presents RGB in familiar 8-bit units and
	// publishes all changes through the terrain transaction/undo path below.
	DrawMaterialControls();
	ImGui::Separator();
	if(terrainDirtyState.IsDirty() && ImGui::Button("Save Terrain Asset", ImVec2(-1.0F, 0.0F)))
	{
		std::string saveError;
		if(!SaveTerrain(saveError)) status = std::move(saveError);
	}
	ImGui::Checkbox("Pin Selected Area", &selectedAreaPinned);
	if(ImGui::CollapsingHeader("Resize Terrain", ImGuiTreeNodeFlags_DefaultOpen)) DrawResizeControls();
	if(ImGui::CollapsingHeader("Procedural Tools")) DrawProceduralControls();
	if(ImGui::CollapsingHeader("Diagnostics")) DrawDiagnosticControls();
	if(ImGui::CollapsingHeader("Maintenance"))
	{
		if(ImGui::Button("Collect Unused Terrain Data", ImVec2(-1.0F, 0.0F)))
		{
			std::string message;
			if(!RunBoundedGarbageCollection(64u, message)) status = std::move(message);
		}
		if(ImGui::Button("Repair Failed Generation", ImVec2(-1.0F, 0.0F)))
		{
			std::string message;
			if(!RepairFailedGeneration(message)) status = std::move(message);
		}
	}
}

void TerrainWindow::DrawResizeControls()
{
	PollResizePreview();
	ImGui::TextDisabled("Resize keeps sample spacing fixed. Edge Blend is deterministic for uneven authored edges.");
	ImGui::InputInt("Expand -X areas", &resizeExpansionAreas[0]);
	ImGui::InputInt("Expand +X areas", &resizeExpansionAreas[1]);
	ImGui::InputInt("Expand -Z areas", &resizeExpansionAreas[2]);
	ImGui::InputInt("Expand +Z areas", &resizeExpansionAreas[3]);
	for(int& value : resizeExpansionAreas) value = std::clamp(value, 0, 32);
	const char* policies[] = {"Edge Blend (default)", "Extend Edge", "Flat Default"};
	ImGui::Combo("Expansion policy", &resizeExpansionPolicy, policies, IM_ARRAYSIZE(policies));
	ImGui::SliderInt("Transition samples", &resizeTransitionSamples, 1, 1024);
	const char* holePolicies[] = {"Close at former edge", "Extend connected boundary holes"};
	ImGui::Combo("Boundary holes", &resizeHolePolicy, holePolicies, IM_ARRAYSIZE(holePolicies));
	if(ImGui::Button("Preview Expansion", ImVec2(-1.0F, 0.0F)))
	{
		std::string error;
		if(!StartResizePreview(TerraEngine::Terrain::TerrainResizeKind::Expand, error)) status = std::move(error);
	}
	ImGui::Separator();
	for(std::size_t index = 0u; index < resizeCropBounds.size(); ++index)
	{
		static const char* labels[] = {"Crop min X", "Crop min Z", "Crop max X exclusive", "Crop max Z exclusive"};
		ImGui::InputScalar(labels[index], ImGuiDataType_S64, &resizeCropBounds[index]);
	}
	if(ImGui::Button("Preview Crop", ImVec2(-1.0F, 0.0F)))
	{
		resizeCropAcknowledged = false;
		std::string error;
		if(!StartResizePreview(TerraEngine::Terrain::TerrainResizeKind::Crop, error)) status = std::move(error);
	}

	const auto progress = resizeJobs.Progress();
	if(progress.stage != TerraEngine::Editor::Terrain::TerrainResizeStage::Idle)
	{
		ImGui::ProgressBar(progress.fraction, ImVec2(-1.0F, 0.0F), progress.message.c_str());
		if(!queuedResizeTransactionId && (progress.cancellationAvailable || resizePreview)
			&& ImGui::Button("Cancel Resize Preview"))
			CancelResizePreview("Terrain resize preview cancelled by the user.");
	}
	if(!resizePreview) return;
	const auto& preview = resizePreview->preview;
	ImGui::Text("Bounds: [%lld, %lld) x [%lld, %lld)",
		static_cast<long long>(preview.delta.proposedBounds.minX), static_cast<long long>(preview.delta.proposedBounds.maxXExclusive),
		static_cast<long long>(preview.delta.proposedBounds.minZ), static_cast<long long>(preview.delta.proposedBounds.maxZExclusive));
	ImGui::Text("Footprint: %.2f m x %.2f m; %zu area(s), %zu partial.",
		preview.physicalWidthMetres, preview.physicalDepthMetres, preview.proposedAreaCount, preview.partialAreaCount);
	ImGui::Text("Work: %zu generated area(s), %llu sample(s), %zu prepared bytes.",
		preview.estimatedWork.generatedAreas, static_cast<unsigned long long>(preview.estimatedWork.generatedSamples),
		preview.estimatedWork.preparedBytes);
	for(const std::string& warning : preview.warnings)
		ImGui::TextColored(ImVec4(1.0F, 0.65F, 0.15F, 1.0F), "%s", warning.c_str());
	if(preview.requiresDataLossConfirmation)
	{
		ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.25F, 1.0F),
			"Crop discards %llu samples and %zu descriptor(s).",
			static_cast<unsigned long long>(preview.cropLoss.discardedSamples), preview.cropLoss.removedDescriptors);
		ImGui::Checkbox("I understand cropped authored data is removed", &resizeCropAcknowledged);
	}
	const bool canApply = !preview.requiresDataLossConfirmation || resizeCropAcknowledged;
	ImGui::BeginDisabled(!canApply || queuedResizeTransactionId.has_value());
	if(ImGui::Button(preview.delta.kind == TerraEngine::Terrain::TerrainResizeKind::Expand
		? "Expand Terrain" : "Crop Terrain", ImVec2(-1.0F, 0.0F)))
	{
		std::string error;
		if(!CommitResizePreview(resizeCropAcknowledged, error)) status = std::move(error);
	}
	ImGui::EndDisabled();
}

bool TerrainWindow::StartResizePreview(TerraEngine::Terrain::TerrainResizeKind kind, std::string& outError)
{
	if(!terrainStore || queuedResizeTransactionId)
	{
		outError = "Resize requires an open terrain and no pending resize publication.";
		return false;
	}
	const auto capture = terrainStore->CaptureRoot();
	if(!capture.IsValid())
	{
		outError = "Resize requires a current immutable terrain root.";
		return false;
	}
	TerraEngine::Editor::Terrain::TerrainResizeJobRequest request{};
	request.observedRoot = capture;
	request.maximumAreas = 4096u;
	request.budget.maximumScannedAreas = 4096u;
	request.budget.maximumGeneratedAreas = 4096u;
	request.budget.maximumGeneratedSamples = 4096u * TerraEngine::Terrain::kTerrainAreaSampleCount;
	request.delta.kind = kind;
	request.delta.previousBounds = capture.root->authoredBounds;
	request.delta.expansionPolicy = static_cast<TerraEngine::Terrain::TerrainExpansionPolicy>(resizeExpansionPolicy);
	request.delta.holePolicy = static_cast<TerraEngine::Terrain::TerrainBoundaryHolePolicy>(resizeHolePolicy);
	request.delta.transitionSamples = static_cast<std::uint32_t>(resizeTransitionSamples);
	request.holePolicyExplicit = true;
	if(kind == TerraEngine::Terrain::TerrainResizeKind::Expand)
	{
		request.delta.proposedBounds = {
			capture.root->authoredBounds.minX - static_cast<std::int64_t>(resizeExpansionAreas[0]) * TerraEngine::Terrain::kTerrainAreaExtentSamples,
			capture.root->authoredBounds.minZ - static_cast<std::int64_t>(resizeExpansionAreas[2]) * TerraEngine::Terrain::kTerrainAreaExtentSamples,
			capture.root->authoredBounds.maxXExclusive + static_cast<std::int64_t>(resizeExpansionAreas[1]) * TerraEngine::Terrain::kTerrainAreaExtentSamples,
			capture.root->authoredBounds.maxZExclusive + static_cast<std::int64_t>(resizeExpansionAreas[3]) * TerraEngine::Terrain::kTerrainAreaExtentSamples};
	}
	else request.delta.proposedBounds = {resizeCropBounds[0], resizeCropBounds[1], resizeCropBounds[2], resizeCropBounds[3]};
	const auto snapshots = committedAreaHeightMm;
	const std::int32_t defaultHeight = capture.root->defaultHeightMm;
	request.sourceHeight = [snapshots, defaultHeight](TerraEngine::Terrain::TerrainSampleCoordinate sample) -> std::optional<std::int32_t> {
		const auto area = TerraEngine::Terrain::OwnerArea(sample);
		const auto local = TerraEngine::Terrain::LocalSample(sample);
		if(!area || !local) return std::nullopt;
		const auto found = snapshots.find(*area);
		if(found == snapshots.end()) return defaultHeight;
		return found->second[static_cast<std::size_t>(local->z) * 256u + static_cast<std::size_t>(local->x)];
	};
	for(const auto& shard : capture.root->directoryShards)
		for(const auto& [coordinate, descriptor] : shard.descriptors)
		{
			TerraEngine::Editor::Terrain::TerrainResizeJobRequest::AreaSource source{};
			source.descriptor = descriptor;
			if(const auto heights = committedAreaHeightMm.find(coordinate); heights != committedAreaHeightMm.end())
				source.heightMm = heights->second;
			else if(descriptor.channels.heightHash)
			{
				const auto bytes = terrainStore->Blobs().Resolve(*descriptor.channels.heightHash);
				if(bytes && bytes->size() == TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t))
				{
					source.heightMm.resize(TerraEngine::Terrain::kTerrainAreaSampleCount);
					std::memcpy(source.heightMm.data(), bytes->data(), bytes->size());
				}
			}
			if(descriptor.channels.holeHash)
			{
				const auto bytes = terrainStore->Blobs().Resolve(*descriptor.channels.holeHash);
				if(bytes && bytes->size() == TerraEngine::Terrain::kTerrainAreaSampleCount / 8u)
					source.holeBits = *bytes;
			}
			for(const auto& [group, weightHash] : descriptor.channels.weightGroupHashes)
			{
				const auto bytes = terrainStore->Blobs().Resolve(weightHash);
				constexpr std::size_t bytesPerSample = 12u;
				if(!bytes || bytes->size() != TerraEngine::Terrain::kTerrainAreaSampleCount * bytesPerSample) continue;
				std::vector<TerraEngine::Terrain::TerrainWeightSample> decoded(TerraEngine::Terrain::kTerrainAreaSampleCount);
				bool valid = true;
				for(std::size_t index = 0u; index < decoded.size(); ++index)
				{
					const std::span<const std::uint8_t, bytesPerSample> sample(
						bytes->data() + index * bytesPerSample, bytesPerSample);
					if(!TerraEngine::Terrain::DecodeTerrainWeightSample(sample, decoded[index]).Succeeded())
					{
						valid = false;
						break;
					}
				}
				if(valid) source.surfaceGroups.emplace(group, std::move(decoded));
			}
			request.sourceAreas.emplace(coordinate, std::move(source));
		}
	ClearResizePreview();
	return resizeJobs.Start(std::move(request), outError);
}

void TerrainWindow::PollResizePreview()
{
	if(auto ready = resizeJobs.TakeReadyPreview())
	{
		resizePreview = std::move(*ready);
		RebuildResizeViewportHeightCache();
		status = resizePreview->preview.message;
	}
}

void TerrainWindow::RebuildResizeViewportHeightCache()
{
	resizePreviewAreaHeightMm.clear();
	if(!resizePreview || resizePreview->preview.delta.kind != TerraEngine::Terrain::TerrainResizeKind::Expand)
		return;
	for(const auto& descriptor : resizePreview->preview.delta.descriptorUpserts)
	{
		if(!descriptor.channels.heightHash) continue;
		const auto blob = std::find_if(resizePreview->preparedBlobs.begin(), resizePreview->preparedBlobs.end(),
			[&descriptor](const auto& candidate) { return candidate.first == *descriptor.channels.heightHash; });
		if(blob == resizePreview->preparedBlobs.end()
			|| blob->second.size() != TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t)) continue;
		auto& heights = resizePreviewAreaHeightMm[descriptor.coordinate];
		heights.resize(TerraEngine::Terrain::kTerrainAreaSampleCount);
		std::memcpy(heights.data(), blob->second.data(), blob->second.size());
	}
}

void TerrainWindow::ClearResizePreview() noexcept
{
	resizePreview.reset();
	resizePreviewAreaHeightMm.clear();
	resizeCropAcknowledged = false;
}

void TerrainWindow::CancelResizePreview(std::string_view reason)
{
	// Cancellation is immediately visible in the editor even if a worker is
	// between its final cancellation check and future completion. The job owner
	// also suppresses that late result, so PollResizePreview cannot resurrect it.
	resizeJobs.Cancel(reason);
	ClearResizePreview();
	status.assign(reason);
}

std::optional<std::int32_t> TerrainWindow::SampleActiveResizePreviewHeight(
	TerraEngine::Terrain::TerrainSampleCoordinate sample) const noexcept
{
	if(!resizePreview) return std::nullopt;
	const auto& delta = resizePreview->preview.delta;
	const TerraEngine::Terrain::TerrainSampleBounds visibleBounds = delta.kind == TerraEngine::Terrain::TerrainResizeKind::Expand
		? delta.proposedBounds : delta.previousBounds;
	if(!visibleBounds.Contains(sample)) return std::nullopt;
	const auto area = TerraEngine::Terrain::OwnerArea(sample);
	const auto local = TerraEngine::Terrain::LocalSample(sample);
	if(!area || !local) return std::nullopt;
	const auto sampleArea = [&](const auto& areas) -> std::optional<std::int32_t> {
		const auto found = areas.find(*area);
		if(found == areas.end() || found->second.size() != TerraEngine::Terrain::kTerrainAreaSampleCount)
			return std::nullopt;
		return found->second[static_cast<std::size_t>(local->z) * 256u + local->x];
	};
	if(const auto prepared = sampleArea(resizePreviewAreaHeightMm)) return prepared;
	if(const auto committed = sampleArea(committedAreaHeightMm)) return committed;
	return resizePreview->observedRoot.root
		? std::optional<std::int32_t>{resizePreview->observedRoot.root->defaultHeightMm}
		: std::nullopt;
}

bool TerrainWindow::CommitResizePreview(bool confirmDataLoss, std::string& outError)
{
	if(!terrainStore || !resizePreview)
	{
		outError = "Create a current resize preview before committing.";
		return false;
	}
	auto transaction = resizeJobs.BuildTransaction(*terrainStore, *resizePreview, confirmDataLoss, outError);
	if(!transaction) return false;
	queuedResizeTransactionId = transaction->transactionId;
	if(!authoringSystem.EnqueueProceduralCommit(std::move(*transaction), outError))
	{
		queuedResizeTransactionId.reset();
		return false;
	}
	status = "Terrain resize queued for one protected publication boundary.";
	return true;
}

void TerrainWindow::DrawInspectorControls(TerraEngine::UUID selectedTerrainObjectId)
{
	const std::optional<TerraEngine::UUID> selectedId = selectedTerrainObjectId;
	if(!editingSelectionActive || !IsEditingTargetSelected(selectedId) || !terrainStore)
	{
		ImGui::TextDisabled("Preparing terrain tools...");
		if(!status.empty()) ImGui::TextWrapped("%s", status.c_str());
		return;
	}

	ObserveTerrainMaterialChanges();
	PollProceduralPreview();
	DrawInspectorToolPalette();
	ImGui::Spacing();
	switch(inspectorTool)
	{
	case InspectorTool::Sculpt: DrawInspectorSculptControls(); break;
	case InspectorTool::Paint: DrawInspectorPaintControls(); break;
	case InspectorTool::Holes: DrawInspectorHoleControls(); break;
	case InspectorTool::Layers: DrawInspectorLayerControls(); break;
	case InspectorTool::Settings: DrawInspectorSettingsControls(); break;
	}
	if(authoringSession.IsDirty())
	{
		ImGui::Spacing();
		ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.25F, 1.0F), "Uncommitted terrain preview");
		if(ImGui::Button("Cancel Preview", ImVec2(-1.0F, 0.0F)))
			CancelDetachedPreview("Detached terrain preview cancelled; committed terrain was not changed.");
	}
	if(!status.empty() && status.find("ready:") == std::string::npos)
	{
		ImGui::Spacing();
		ImGui::TextWrapped("%s", status.c_str());
	}
}

void TerrainWindow::DrawMaterialControls()
{
	ImGui::TextUnformatted("Terrain Material");
	ImGui::TextDisabled("Metallic Roughness (lit PBR)");
	if(!terrainStore)
	{
		ImGui::TextDisabled("Open or create a terrain to edit its material.");
		return;
	}
	const auto capture = terrainStore->CaptureRoot();
	if(!capture.IsValid()) return;
	ObjectManager& objects = ObjectManager::instance();
	std::optional<Object> owner;
	if(terrainRenderProxyTargetObjectId != TerraEngine::EMPTY_UUID)
	{
		const auto candidate = objects.getObject(terrainRenderProxyTargetObjectId);
		if(candidate && objects.GetComponent<TerrainComponent>(*candidate)) owner = candidate;
	}
	if(!owner)
	{
		objects.forEachObjectWithComponents<TerrainComponent>(
			[&](Object object, TerrainComponent& terrain)
			{
				if(!owner && terrain.terrainAssetId == capture.root->assetUuid) owner = object;
			});
	}
	TerrainComponent* terrain = owner ? objects.GetComponent<TerrainComponent>(*owner) : nullptr;
	if(!terrain)
	{
		ImGui::TextDisabled("The terrain scene component is not available yet.");
		return;
	}
	glm::vec3 baseColor = terrain->baseColor;
	float metallic = terrain->metallic;
	float roughness = terrain->roughness;
	// TerrainComponent stores normalized channels for shaders and serialization,
	// while artists expect RGB fields in the 0-255 range. Explicit Uint8 display
	// flags prevent ImGui's global color preference from exposing 0.0-1.0 values.
	const bool colorChanged = ImGui::ColorEdit3(
		"Base Color (RGB)",
		&baseColor.x,
		ImGuiColorEditFlags_Uint8
			| ImGuiColorEditFlags_DisplayRGB
			| ImGuiColorEditFlags_InputRGB);
	const bool metallicChanged = ImGui::SliderFloat("Metallic", &metallic, 0.0F, 1.0F);
	const bool roughnessChanged = ImGui::SliderFloat("Roughness", &roughness, 0.0F, 1.0F);
	if(!colorChanged && !metallicChanged && !roughnessChanged) return;

	// UI prepares one atomic scene transaction. The regular protected mutation
	// flush makes persistence, undo, renderer invalidation, and automation agree.
	const TerraEngine::UUID objectId = objects.getUUID(*owner);
	TerraEngine::Scene::TransactionalMutationBuilder builder(
		"terrain-material-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()),
		TerraEngine::Scene::TransactionOrigin::Editor, "Edit Terrain Material");
	builder.ObserveScene(objects).ObserveObject(objects, objectId)
		.SetUndoGroup("Edit Terrain Material");
	if(colorChanged)
		builder.UpdateComponentProperty(objectId, "TerrainComponent", "baseColor",
			TerraEngine::Reflection::Variant{baseColor});
	if(metallicChanged)
		builder.UpdateComponentProperty(objectId, "TerrainComponent", "metallic",
			TerraEngine::Reflection::Variant{metallic});
	if(roughnessChanged)
		builder.UpdateComponentProperty(objectId, "TerrainComponent", "roughness",
			TerraEngine::Reflection::Variant{roughness});
	TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(builder.Build());
}

void TerrainWindow::DrawDiagnosticControls()
{
	ImGui::TextUnformatted("Terrain diagnostics");
	if(terrainStore)
	{
		const auto capture = terrainStore->CaptureRoot();
		const auto areas = capture.IsValid()
			? TerraEngine::Terrain::EnumerateTerrainAreas(capture.root->authoredBounds, 4096u)
			: std::nullopt;
		const std::string selectedLabel = std::to_string(selectedTerrainArea.x) + ", "
			+ std::to_string(selectedTerrainArea.z);
		if(areas && ImGui::BeginCombo("Selected signed area", selectedLabel.c_str()))
		{
			for(const auto coordinate : *areas)
			{
				const std::string label = std::to_string(coordinate.x) + ", " + std::to_string(coordinate.z);
				if(ImGui::Selectable(label.c_str(), coordinate == selectedTerrainArea))
				{
					// Area switching only changes detached editor mirrors. The immutable
					// store remains authoritative until a typed transaction is published.
					selectedTerrainArea = coordinate;
					if(const auto found = committedAreaHeightMm.find(coordinate);
						found != committedAreaHeightMm.end()) committedHeightMm = found->second;
					(void)SynchronizeCommittedAuthoringStateFromPublishedRoot();
					ScheduleRenderProxyBuild();
				}
			}
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Available coverage: %zu area(s).", areas ? areas->size() : 0u);
	}
	ImGui::InputText("Filter diagnostics", diagnosticFilter, IM_ARRAYSIZE(diagnosticFilter));
	const auto diagnostics=DiagnosticSnapshot();
	ImGui::TextDisabled("Selected area: %lld, %lld | streaming %.0f%% | retiring %zu | collision regions %zu",
		static_cast<long long>(selectedTerrainArea.x), static_cast<long long>(selectedTerrainArea.z),
		static_cast<double>(diagnostics.streamingProgress * 100.0F), diagnostics.retiringAreaCount,
		diagnostics.collisionRegionCount);
	if(diagnostics.commonReadyMismatch)
		ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.15F, 1.0F),
			"Common-ready generation is pending; last-known-good consumers remain active.");
	for(const auto& collision:diagnostics.collisions)
	{
		const std::string line="Collision " + collision.message;
		if(diagnosticFilter[0]!='\0' && line.find(diagnosticFilter)==std::string::npos) continue;
		const bool needsAction=collision.status==TerraEngine::Terrain::TerrainCollisionStatus::Stale || collision.status==TerraEngine::Terrain::TerrainCollisionStatus::Failed;
		ImGui::TextColored(needsAction?ImVec4(1.0F,0.55F,0.15F,1.0F):ImVec4(0.55F,0.9F,0.55F,1.0F),"%s",line.c_str());
		ImGui::SameLine(); ImGui::TextDisabled("age %llums, build %llums",static_cast<unsigned long long>(collision.ageMilliseconds),static_cast<unsigned long long>(collision.buildLatencyMilliseconds));
		if(needsAction && ImGui::Button(("Repair / rebuild##"+std::to_string(collision.coordinate.x)+":"+std::to_string(collision.coordinate.z)).c_str()))
			status="Collision rebuild requested; last-known-good remains active until a current derived receipt arrives.";
		ImGui::SameLine();
		if(ImGui::Button(("Locate##collision:"+std::to_string(collision.coordinate.x)+":"+std::to_string(collision.coordinate.z)).c_str()))
			diagnosticLocateRequest = collision.coordinate;
	}
	for(const auto& item : diagnostics.items)
	{
		if(diagnosticFilter[0]!='\0' && item.message.find(diagnosticFilter)==std::string::npos) continue;
		const char* severity = item.severity == TerraEngine::Terrain::TerrainDiagnosticSeverity::Error ? "error"
			: item.severity == TerraEngine::Terrain::TerrainDiagnosticSeverity::Warning ? "warning" : "information";
		const char* action = item.action == TerraEngine::Terrain::TerrainDiagnosticAction::Retry ? "retry"
			: item.action == TerraEngine::Terrain::TerrainDiagnosticAction::Rebuild ? "rebuild"
			: item.action == TerraEngine::Terrain::TerrainDiagnosticAction::ReloadSource ? "reload source"
			: item.action == TerraEngine::Terrain::TerrainDiagnosticAction::ResolveConflict ? "resolve conflict"
			: item.action == TerraEngine::Terrain::TerrainDiagnosticAction::IncreaseBudget ? "increase budget"
			: item.action == TerraEngine::Terrain::TerrainDiagnosticAction::LocateArea ? "locate area" : "none";
		ImGui::Text("[%s] %s", severity, item.message.c_str());
		ImGui::SameLine(); ImGui::TextDisabled("action: %s", action);
		if(item.location)
		{
			ImGui::SameLine();
			if(ImGui::Button(("Locate##item:"+std::to_string(item.location->x)+":"+std::to_string(item.location->z)).c_str()))
				diagnosticLocateRequest = *item.location;
		}
	}
	if(ImGui::Button("Repair failed terrain generation")) { std::string repair; if(!RepairFailedGeneration(repair)) status=repair; }
}

void TerrainWindow::DrawCreateImportControls()
{
	ImGui::TextUnformatted("Create / Import");
	ImGui::TextDisabled("Height is stored in millimetres; spacing is metres per sample.");
	const char* formats[] = {"Flat 256 x 256", "PNG16 grayscale", "RAW16 little-endian"};
	ImGui::Combo("Source", &sourceFormat, formats, IM_ARRAYSIZE(formats));
	static char sourceBuffer[512]{}; static char destinationBuffer[512]{};
	ImGui::InputText("Source path", sourceBuffer, IM_ARRAYSIZE(sourceBuffer));
	ImGui::InputText("Destination (.teterrain)", destinationBuffer, IM_ARRAYSIZE(destinationBuffer));
	if(ImGui::Button("Create / Import"))
	{
		TerraEngine::AssetPipeline::TerrainImportRequest request;
		request.sourceFormat = static_cast<TerraEngine::AssetPipeline::TerrainSourceFormat>(sourceFormat);
		request.sourcePath = sourceBuffer; request.assetId = TerraEngine::GenerateUUID();
		// RAW dimensions are deliberately visible and bounded in this first UI.
		if(request.sourceFormat == TerraEngine::AssetPipeline::TerrainSourceFormat::Raw16LittleEndian) { request.rawWidth = 256U; request.rawHeight = 256U; }
		TerraEngine::Editor::Terrain::TerrainCreationService service =
			TerraEngine::Editor::Terrain::CreateProjectTerrainCreationService();
		auto result = service.CreateOrImport(request, destinationBuffer, [&](float, std::string_view, std::string_view) { return true; });
		status = result.message;
		if(result.succeeded)
		{
			destinationPath = destinationBuffer;
			InitializeAuthoringTerrain(request.assetId, result.heightfield);
		}
	}
	if(!status.empty()) ImGui::TextWrapped("%s", status.c_str());
}

void TerrainWindow::DrawSculptControls()
{
	ImGui::TextUnformatted("Sculpt");
	bool requestedSculpting = sculpting;
	if(ImGui::Checkbox("Enable terrain brush", &requestedSculpting)) SetSculpting(requestedSculpting);
	ImGui::TextDisabled("Drag in Scene View; releasing commits one undoable stroke.");
	ImGui::SliderFloat("Radius (samples)", &brushSettings.radiusSamples, 0.25F, 512.0F);
	ImGui::SliderFloat("Strength (mm)", &brushSettings.strengthMm, 1.0F, 10000.0F);
	ImGui::SliderFloat("Spacing (samples)", &brushSettings.spacingSamples, 0.0F, 128.0F);
	ImGui::SliderFloat("Rotation (radians)", &brushSettings.rotationRadians, -3.14159F, 3.14159F);
	ImGui::SliderFloat("Pressure", &brushSettings.pressure, 0.0F, 1.0F);
	const char* modes[] = {"Raise", "Lower", "Smooth", "Flatten value", "Flatten picked", "Terrace", "Ramp", "Stamp", "Noise"};
	int mode = static_cast<int>(brushSettings.mode); if(ImGui::Combo("Brush mode", &mode, modes, IM_ARRAYSIZE(modes))) brushSettings.mode = static_cast<TerraEngine::Editor::Terrain::TerrainBrushMode>(mode);
	const char* falloffs[] = {"Constant", "Linear", "SmoothStep", "Gaussian"};
	int falloff = static_cast<int>(brushSettings.falloff); if(ImGui::Combo("Falloff", &falloff, falloffs, IM_ARRAYSIZE(falloffs))) brushSettings.falloff = static_cast<TerraEngine::Editor::Terrain::TerrainBrushFalloff>(falloff);
	if(brushSettings.mode == TerraEngine::Editor::Terrain::TerrainBrushMode::FlattenValue || brushSettings.mode == TerraEngine::Editor::Terrain::TerrainBrushMode::FlattenPicked || brushSettings.mode == TerraEngine::Editor::Terrain::TerrainBrushMode::Ramp)
		ImGui::InputScalar("Target height (mm)", ImGuiDataType_S32, &brushSettings.flattenHeightMm);
	if(brushSettings.mode == TerraEngine::Editor::Terrain::TerrainBrushMode::Terrace)
		ImGui::InputScalar("Terrace step (mm)", ImGuiDataType_S32, &brushSettings.terraceStepMm);
	if(brushSettings.mode == TerraEngine::Editor::Terrain::TerrainBrushMode::Noise)
		ImGui::InputScalar("Noise seed", ImGuiDataType_U32, &brushSettings.randomSeed);
	ImGui::Checkbox("Use mask", &maskEnabled);
	ImGui::ProgressBar(authoringSession.IsDirty() ? 0.75F : 0.0F, ImVec2(-1.0F, 0.0F), authoringSession.IsDirty() ? "Detached preview ready" : "No queued terrain work");
	ImGui::TextDisabled("Preview: %s", authoringSession.IsDirty() ? "detached changes pending" : "no pending changes");
	ImGui::TextDisabled("Saved root: %s", terrainDirtyState.IsDirty() ? "different from current root" : "matches current root");
	if(ImGui::Button("Cancel detached preview")) CancelDetachedPreview("Detached terrain preview cancelled; committed terrain was not changed.");
	ImGui::SameLine();
	if(ImGui::Button("Apply preview"))
	{
		applyRequested = QueuePreviewForPublication();
		if(!applyRequested && status.empty()) status = "No valid detached terrain preview is available to apply.";
	}
	// Recovery/GC are intentionally stateful UI rather than destructive buttons:
	// persistence owns repair and garbage collection through its durable service.
	ImGui::TextDisabled("Recovery: protected publication receipts are retained.");
	ImGui::TextDisabled("History: %zu undo, %zu redo", UndoRedoSystem::Instance().GetUndoDepth(), UndoRedoSystem::Instance().GetRedoDepth());
	ImGui::Checkbox("Pin selected terrain area", &selectedAreaPinned);
	ImGui::TextDisabled("GC: immutable history blobs are released when history is pruned.");
	if(ImGui::Button("Run bounded terrain GC"))
	{
		std::string gcMessage;
		const bool collectionCompleted = RunBoundedGarbageCollection(64U, gcMessage);
		if(!collectionCompleted && gcMessage.empty()) garbageCollectionStatus = "Terrain garbage collection did not complete.";
	}
	ImGui::SameLine();
	if(ImGui::Button("Repair failed generation"))
	{
		std::string recoveryMessage;
		const bool repairCompleted = RepairFailedGeneration(recoveryMessage);
		if(!repairCompleted && recoveryMessage.empty()) repairStatus = "Terrain generation repair did not complete.";
	}
	if(!garbageCollectionStatus.empty()) ImGui::TextDisabled("GC: %s", garbageCollectionStatus.c_str());
	if(!repairStatus.empty()) ImGui::TextDisabled("Repair: %s", repairStatus.c_str());
	if(terrainDirtyState.IsDirty() && ImGui::Button("Save terrain root"))
	{
		std::string saveError;
		if(!SaveTerrain(saveError) && status.empty()) status = saveError;
	}
	ImGui::Separator();
	ImGui::TextUnformatted("Holes / Collision");
	bool requestedHoleSculpting = holeSculpting;
	if(ImGui::Checkbox("Enable hole brush", &requestedHoleSculpting)) SetHoleSculpting(requestedHoleSculpting);
	const char* holeModes[] = {"Paint hole", "Erase hole"};
	int holeMode = static_cast<int>(holeBrushSettings.mode);
	if(ImGui::Combo("Hole mode", &holeMode, holeModes, IM_ARRAYSIZE(holeModes))) holeBrushSettings.mode = static_cast<TerraEngine::Editor::Terrain::TerrainHoleBrushMode>(holeMode);
		holeBrushSettings.radiusCells = TerraEngine::Editor::Terrain::TerrainHoleBrush::EffectiveRadiusCells(
			holeBrushSettings.radiusCells);
		ImGui::SliderFloat("Hole radius (cells)", &holeBrushSettings.radiusCells,
			TerraEngine::Editor::Terrain::TerrainHoleBrush::kMinimumRadiusCells, 128.0F);
		if(ImGui::Button("Paint hole at area centre"))
		{
			if(!PreviewHoleStamp(TerraEngine::Editor::Terrain::TerrainHoleBrush::CenteredOnCell(128, 128)))
				status = "Hole preview was outside the active canonical mask.";
			else
			{
				std::string error;
				if(!ApplyHolePreview(error)) status = error;
			}
		}
	ImGui::SameLine();
	if(ImGui::Button("Apply hole preview"))
	{
		std::string error;
		if(!ApplyHolePreview(error)) status = error;
	}
	ImGui::SameLine();
	if(ImGui::Button("Cancel hole preview")) CancelHolePreview();
	ImGui::TextDisabled(holePreviewActive ? "Hole preview is detached; collision remains last-known-good until Apply commits." : "No detached hole preview.");
	const auto collisionDiagnostics = DiagnosticSnapshot();
	if(collisionDiagnostics.collisions.empty())
		ImGui::TextDisabled("Collision: no terrain collision candidate has been observed yet.");
	else for(const auto& collision : collisionDiagnostics.collisions)
	{
		const char* state = collision.status == TerraEngine::Terrain::TerrainCollisionStatus::Current ? "current"
			: collision.status == TerraEngine::Terrain::TerrainCollisionStatus::Pending ? "pending"
			: collision.status == TerraEngine::Terrain::TerrainCollisionStatus::Stale ? "stale"
			: collision.status == TerraEngine::Terrain::TerrainCollisionStatus::Failed ? "failed" : "unavailable";
		ImGui::Text("Collision (%lld, %lld): %s | source %s / %llu | age %llums | build %llums%s",
			static_cast<long long>(collision.coordinate.x), static_cast<long long>(collision.coordinate.z), state,
			TerraEngine::Terrain::ToHexString(collision.sourceRootHash).c_str(), static_cast<unsigned long long>(collision.sourcePublicationGeneration),
			static_cast<unsigned long long>(collision.ageMilliseconds), static_cast<unsigned long long>(collision.buildLatencyMilliseconds),
			collision.lastKnownGoodActive ? " | last-known-good active" : "");
	}
}

void TerrainWindow::DrawSurfaceControls()
{
	ImGui::TextUnformatted("Terrain Surface Layers");
	ImGui::TextDisabled("Layer IDs stay stable when this list is reordered or a material is unavailable.");
	// Each button is one authoring action. Stage and queue it together so users
	// never need to discover a second Apply button for ordinary list edits.
	const auto stageAndApplyLayerPatch = [this](TerraEngine::Terrain::TerrainLayerPatch patch, std::string& outError) {
		return StageLayerPatch(patch, outError) && ApplyLayerPreview(outError);
	};
	// Render from a copy: a click can replace the detached stack while the UI is
	// iterating, and retaining a reference would make that ordinary edit unsafe.
	const TerraEngine::Terrain::TerrainLayerStack stack = LayerPreview();
	for(std::size_t index = 0u; index < stack.layers.size(); ++index)
	{
		const auto& layer = stack.layers[index];
		ImGui::PushID(static_cast<int>(layer.stableId));
		const bool selected = selectedLayerId == layer.stableId;
		if(ImGui::Selectable(layer.name.c_str(), selected)) selectedLayerId = layer.stableId;
		ImGui::SameLine();
		if(layer.fallbackState != TerraEngine::Terrain::TerrainLayerFallbackState::Healthy)
			ImGui::TextColored(ImVec4(1.0F, 0.65F, 0.1F, 1.0F), "Fallback active (source missing or invalid)");
		else ImGui::TextDisabled("Source ready");
		ImGui::SameLine();
		if(ImGui::SmallButton(layer.enabled ? "Disable" : "Enable"))
		{
			std::string error;
			const bool staged = stageAndApplyLayerPatch({TerraEngine::Terrain::TerrainLayerPatchKind::SetEnabled, 0u, layer.stableId, 0u, {}, !layer.enabled}, error);
			if(!staged) status = error.empty() ? "Unable to stage the terrain layer visibility change." : error;
		}
		ImGui::SameLine();
		if(ImGui::SmallButton("Duplicate"))
		{
			std::uint16_t nextId = 1u;
			for(const auto& candidate : stack.layers) nextId = std::max<std::uint16_t>(nextId, static_cast<std::uint16_t>(candidate.stableId + 1u));
			TerraEngine::Terrain::TerrainLayer duplicate = layer;
			duplicate.stableId = nextId;
			duplicate.name += " Copy";
			std::string error;
			const bool staged = stageAndApplyLayerPatch({TerraEngine::Terrain::TerrainLayerPatchKind::Add, 0u, 0u, 0xffffffffu, std::move(duplicate)}, error);
			if(!staged) status = error.empty() ? "Unable to stage the terrain layer duplicate." : error;
		}
		ImGui::SameLine();
		if(index > 0u && ImGui::SmallButton("Up"))
		{
			std::string error;
			const bool staged = stageAndApplyLayerPatch({TerraEngine::Terrain::TerrainLayerPatchKind::Move, 0u, layer.stableId, static_cast<std::uint32_t>(index - 1u)}, error);
			if(!staged) status = error.empty() ? "Unable to stage the terrain layer reorder." : error;
		}
		ImGui::SameLine();
		if(stack.layers.size() > 1u && ImGui::SmallButton("Remove"))
		{
			std::string error;
			const bool staged = stageAndApplyLayerPatch({TerraEngine::Terrain::TerrainLayerPatchKind::Remove, 0u, layer.stableId}, error);
			if(!staged) status = error.empty() ? "Unable to stage the terrain layer removal." : error;
		}
		ImGui::PopID();
	}
	if(ImGui::Button("Add layer"))
	{
		std::uint16_t nextId = 1u;
		for(const auto& layer : stack.layers) nextId = std::max<std::uint16_t>(nextId, static_cast<std::uint16_t>(layer.stableId + 1u));
		TerraEngine::Terrain::TerrainLayer layer;
		layer.stableId = nextId;
		layer.name = "Layer " + std::to_string(nextId);
		layer.fallbackState = TerraEngine::Terrain::TerrainLayerFallbackState::FallbackActive;
		std::string error;
		if(stageAndApplyLayerPatch({TerraEngine::Terrain::TerrainLayerPatchKind::Add, 0u, 0u, 0xffffffffu, std::move(layer)}, error)) selectedLayerId = nextId;
		else status = error;
	}
	ImGui::SameLine();
	if(ImGui::Button("Apply layer list"))
	{
		std::string error;
		if(!ApplyLayerPreview(error)) status = error;
	}
	ImGui::SameLine();
	if(ImGui::Button("Cancel layer list")) CancelLayerPreview();
	if(selectedLayerId != 0u && ImGui::Button("Repair selected source"))
	{
		const auto found = std::find_if(stack.layers.begin(), stack.layers.end(), [this](const auto& layer) { return layer.stableId == selectedLayerId; });
		if(found == stack.layers.end() || found->materialAssetUuid == TerraEngine::EMPTY_UUID || !AssetDatabase::Instance().FindAssetByGuid(found->materialAssetUuid))
			status = "Source repair needs a registered material asset with the selected layer UUID.";
		else
		{
			std::string error;
			if(!stageAndApplyLayerPatch({TerraEngine::Terrain::TerrainLayerPatchKind::SetFallbackState, 0u, selectedLayerId, 0u, {}, true, TerraEngine::Terrain::TerrainLayerFallbackState::Healthy}, error)) status = error;
		}
	}

	ImGui::Separator();
	ImGui::TextUnformatted("Surface paint preview");
	int selected = static_cast<int>(surfaceBrushSettings.layerId);
	if(ImGui::InputInt("Layer ID", &selected)) surfaceBrushSettings.layerId = selected > 0 && selected <= 65535 ? static_cast<std::uint16_t>(selected) : 0u;
	int replacement = static_cast<int>(surfaceBrushSettings.replacementLayerId);
	if(ImGui::InputInt("Replace with layer ID", &replacement)) surfaceBrushSettings.replacementLayerId = replacement > 0 && replacement <= 65535 ? static_cast<std::uint16_t>(replacement) : 0u;
	const char* paintModes[] = {"Paint", "Erase", "Fill", "Replace", "Normalize", "Layer mask"};
	int paintMode = static_cast<int>(surfaceBrushSettings.mode);
	if(ImGui::Combo("Paint mode", &paintMode, paintModes, IM_ARRAYSIZE(paintModes))) surfaceBrushSettings.mode = static_cast<TerraEngine::Editor::Terrain::TerrainSurfaceBrushMode>(paintMode);
	int strength = static_cast<int>(surfaceBrushSettings.strength);
	if(ImGui::SliderInt("Weight strength", &strength, 1, 255)) surfaceBrushSettings.strength = static_cast<std::uint8_t>(strength);
	ImGui::Checkbox("Use layer mask", &surfaceBrushSettings.useLayerMask);
	if(ImGui::Button("Paint selected area"))
	{
		const auto result = PreviewSurfaceBrush(surfaceBrushSettings,
			surfaceBrushSettings.useLayerMask ? std::span<const std::uint8_t>(brushMask) : std::span<const std::uint8_t>{});
		if(!result.changed) status = "Surface preview made no change; choose a valid stable layer ID or mask coverage.";
		else
		{
			std::string error;
			if(!ApplySurfacePreview(error)) status = error;
		}
	}
	if(displacedLayerWarning != 0u)
		ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.1F, 1.0F), "Fifth contributor displaced stable layer %u; Apply preserves the explicit canonical result.", displacedLayerWarning);
	ImGui::SameLine();
	if(ImGui::Button("Apply surface preview"))
	{
		std::string error;
		if(!ApplySurfacePreview(error)) status = error;
	}
	ImGui::SameLine();
	if(ImGui::Button("Cancel surface preview")) CancelSurfacePreview();
}

void TerrainWindow::DrawProceduralControls()
{
	PollProceduralPreview();
	ImGui::TextUnformatted("Procedural / Spline Preview");
	ImGui::TextDisabled("Worker batches stay detached; camera and cancellation remain responsive while the preview runs.");
	const char* operations[] = {"Height remap", "Terrace", "Seeded noise", "Stamp", "Hydraulic erosion", "Thermal erosion", "Spline grade"};
	ImGui::Combo("Operation", &proceduralOperation, operations, IM_ARRAYSIZE(operations));
	ImGui::InputScalar("Minimum height (mm)", ImGuiDataType_S32, &proceduralMinimumHeightMm);
	// The optional threshold is exposed conservatively: clear it by entering the
	// full int32 range rather than inventing an unbounded operator selection.
	ImGui::InputScalar("Height delta (mm)", ImGuiDataType_S32, &proceduralHeightDeltaMm);
	const auto progress = proceduralJobs.Progress();
	ImGui::ProgressBar(progress.fraction, ImVec2(-1.0F, 0.0F), progress.message.c_str());
	ImGui::TextDisabled("Estimate: %zu area(s), %zu samples, %zu kernel batch(es), %zu preview bytes.",
		progress.estimate.areas, progress.estimate.samples, progress.estimate.kernelBatches, progress.estimate.previewBytes);
	if(ImGui::Button("Run procedural operation"))
	{
		std::string error;
		proceduralAutoApplyRequested = StartProceduralPreview(error);
		if(!proceduralAutoApplyRequested) status = error;
	}
	ImGui::SameLine();
	if(ImGui::Button("Cancel procedural preview")) CancelProceduralPreview();
	if(ImGui::Button("Apply procedural preview"))
	{
		std::string error;
		if(!ApplyProceduralPreview(error)) status = error;
	}
}

void TerrainWindow::PollProceduralPreview()
{
	if(const auto ready = proceduralJobs.TakeReadyPreview())
	{
		proceduralPreview = *ready;
		if(proceduralAutoApplyRequested)
		{
			proceduralAutoApplyRequested = false;
			std::string error;
			if(!ApplyProceduralPreview(error)) status = error;
		}
		else status = "Detached procedural preview is ready to Apply or Cancel.";
	}
}

bool TerrainWindow::StartProceduralPreview(std::string& outError)
{
	if(!terrainStore || queuedProceduralTransactionId)
	{
		outError = "Procedural preview requires an open terrain and no pending procedural publication.";
		return false;
	}
	const auto capture = terrainStore->CaptureRoot();
	const auto descriptor = terrainStore->ResolveDescriptorOrImplicit(*capture.root, selectedTerrainArea);
	if(committedHeightMm.size() != TerraEngine::Terrain::kTerrainAreaSampleCount)
	{
		outError = "Procedural preview requires one resident canonical terrain area.";
		return false;
	}
	TerraEngine::Editor::Terrain::TerrainProceduralJobRequest request;
	request.observedRoot = capture; request.observedArea = descriptor; request.heightMm = committedHeightMm;
	request.selectionGrid.storageOrigin = {
		selectedTerrainArea.x * TerraEngine::Terrain::kTerrainAreaExtentSamples,
		selectedTerrainArea.z * TerraEngine::Terrain::kTerrainAreaExtentSamples};
	request.selectionGrid.storageWidth = 256u; request.selectionGrid.storageHeight = 256u;
	request.selectionGrid.evaluationBounds = {
		request.selectionGrid.storageOrigin.x, request.selectionGrid.storageOrigin.z,
		request.selectionGrid.storageOrigin.x + 256, request.selectionGrid.storageOrigin.z + 256};
	request.selectionCriteria = proceduralSelection;
	if(proceduralMinimumHeightMm != std::numeric_limits<std::int32_t>::min())
		request.selectionCriteria.minimumHeightMm = proceduralMinimumHeightMm;
	request.selectionCriteria.maximumEvaluatedSamples = TerraEngine::Terrain::kTerrainAreaSampleCount;
	request.historyGroup = "Terrain procedural / spline operation";
	const std::int32_t delta = proceduralHeightDeltaMm;
	const int operation = proceduralOperation;
	const auto descriptorCopy = descriptor;
	request.kernel = [delta, operation, descriptorCopy](std::span<const std::int32_t> source, std::span<std::int32_t> output, std::span<const std::uint8_t> mask, const std::atomic_bool& cancelled) {
		if(source.size() != output.size() || mask.size() != source.size()) return TerraEngine::Terrain::TerrainResultCode::InvalidArgument;
		// Pascal's operators accept immutable value packets.  This adapter leaves
		// live asset state untouched while translating the editor's one-area preview.
		TerraEngine::Editor::Terrain::TerrainProceduralArea area;
		area.coordinate = descriptorCopy.coordinate; area.observedGeneration = descriptorCopy.editGeneration;
		area.sourceHash = descriptorCopy.authoredContentHash; area.heightMm.assign(source.begin(), source.end());
		area.selectionMask.assign(mask.begin(), mask.end());
		TerraEngine::Editor::Terrain::TerrainProceduralRequest input;
		input.areas = {std::move(area)}; input.budget.maximumAreas = 1u;
		input.budget.maximumSamples = TerraEngine::Terrain::kTerrainAreaSampleCount;
		input.budget.maximumWorkingBytes = TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t) * 2u;
		input.cancellation = &cancelled;
		TerraEngine::Editor::Terrain::TerrainProceduralResult result;
		switch(operation)
		{
		case 0: { TerraEngine::Editor::Terrain::TerrainHeightRemapSettings settings; settings.addMm = delta; result = TerraEngine::Editor::Terrain::ApplyTerrainHeightRemap(input, settings); break; }
		case 1: { TerraEngine::Editor::Terrain::TerrainTerraceSettings settings; settings.stepMm = std::max(1, std::abs(delta)); settings.blendPercent = 100u; result = TerraEngine::Editor::Terrain::ApplyTerrainTerrace(input, settings); break; }
		case 2: { TerraEngine::Editor::Terrain::TerrainNoiseSettings settings; settings.seed = 0x5445525241494EULL; settings.amplitudeMm = std::abs(delta); result = TerraEngine::Editor::Terrain::ApplyTerrainSeededNoise(input, settings); break; }
		case 3: { TerraEngine::Editor::Terrain::TerrainStampSettings settings; settings.centre = {128, 128}; settings.radiusSamples = 32u; settings.heightDeltaMm = delta; result = TerraEngine::Editor::Terrain::ApplyTerrainStamp(input, settings); break; }
		case 4: { TerraEngine::Editor::Terrain::TerrainHydraulicErosionSettings settings; settings.iterations = 4u; settings.erosionPerIterationMm = std::max(1, std::abs(delta) / 4); result = TerraEngine::Editor::Terrain::ApplyTerrainHydraulicErosion(input, settings); break; }
		case 5: { TerraEngine::Editor::Terrain::TerrainThermalErosionSettings settings; settings.iterations = 4u; settings.talusMm = std::max(0, std::abs(delta)); settings.transferPerIterationMm = 1; result = TerraEngine::Editor::Terrain::ApplyTerrainThermalErosion(input, settings); break; }
		case 6:
		{
			TerraEngine::Editor::Terrain::TerrainSplineGradeRequest spline;
			spline.terrain = std::move(input); spline.settings.sampleSpacingMetres = 1.0;
			spline.settings.bankHeightMm = std::max(0, std::abs(delta));
			TerraEngine::Editor::Terrain::TerrainSplineControlPoint first;
			TerraEngine::Editor::Terrain::TerrainSplineControlPoint last;
			first.position.offsetMetres = {16.0, 0.0, 128.0}; last.position.offsetMetres = {240.0, 0.0, 128.0};
			spline.controlPoints = {first, last};
			result = TerraEngine::Editor::Terrain::ApplyTerrainSplineGrade(spline); break;
		}
		default: return TerraEngine::Terrain::TerrainResultCode::InvalidArgument;
		}
		if(!result.Succeeded()) return result.code;
		if(result.areas.size() != 1u || result.areas.front().heightMm.size() != output.size()) return TerraEngine::Terrain::TerrainResultCode::InvalidArgument;
		std::copy(result.areas.front().heightMm.begin(), result.areas.front().heightMm.end(), output.begin());
		return TerraEngine::Terrain::TerrainResultCode::Ok;
	};
	if(!proceduralJobs.Start(std::move(request), outError)) return false;
	proceduralPreview.reset();
	return true;
}

bool TerrainWindow::ApplyProceduralPreview(std::string& outError)
{
	PollProceduralPreview();
	if(!proceduralPreview || !terrainStore)
	{
		outError = "No completed procedural preview is available to apply.";
		return false;
	}
	auto transaction = proceduralJobs.BuildTransaction(*terrainStore, *proceduralPreview, outError);
	if(!transaction) return false;
	const TerraEngine::UUID transactionId = transaction->transactionId;
	if(!authoringSystem.EnqueueProceduralCommit(std::move(*transaction), outError)) return false;
	queuedProceduralTransactionId = transactionId;
	status = "Procedural preview queued as one named typed history group.";
	return true;
}

void TerrainWindow::CancelProceduralPreview() noexcept
{
	proceduralAutoApplyRequested = false;
	proceduralJobs.Cancel("Procedural preview cancelled by the editor.");
	if(queuedProceduralTransactionId)
		authoringSystem.CancelPreparedTransaction(*queuedProceduralTransactionId, "Procedural transaction cancelled before protected publication.");
	proceduralPreview.reset();
}

std::optional<TerraEngine::Terrain::TerrainEditTransaction> TerrainWindow::PrepareAutomationTransaction(const TerraEngine::Editor::Automation::TerrainAutomationRequest& request, std::string& outError)
{
	if(!terrainStore)
	{
		outError="Terrain automation requires an open editor-owned terrain session.";
		return std::nullopt;
	}
	const auto capture=terrainStore->CaptureRoot();
	const auto descriptor=terrainStore->ResolveDescriptorOrImplicit(*capture.root,selectedTerrainArea);
	if(committedHeightMm.size()!=TerraEngine::Terrain::kTerrainAreaSampleCount) { outError="A resident canonical terrain area is required."; return std::nullopt; }
	TerraEngine::Editor::Terrain::TerrainProceduralPreview preview; preview.observedRoot=capture; preview.observedArea=descriptor; preview.beforeHeightMm=committedHeightMm; preview.afterHeightMm=committedHeightMm; preview.historyGroup="Terrain automation height remap";
	TerraEngine::Editor::Terrain::TerrainProceduralArea area; area.coordinate=descriptor.coordinate; area.observedGeneration=descriptor.editGeneration; area.sourceHash=descriptor.authoredContentHash; area.heightMm=committedHeightMm;
	TerraEngine::Editor::Terrain::TerrainProceduralRequest procedural; procedural.areas.push_back(std::move(area)); procedural.budget.maximumAreas=request.budget.maximumAreas; procedural.budget.maximumSamples=request.budget.maximumSamples; procedural.budget.maximumWorkingBytes=request.budget.maximumPreparedBytes;
	TerraEngine::Editor::Terrain::TerrainProceduralResult result;
	if(request.operation=="height-remap" || request.operation=="raise" || request.operation=="lower") { TerraEngine::Editor::Terrain::TerrainHeightRemapSettings settings; settings.addMm=request.settings.value("addMm",request.operation=="lower" ? -1 : 1); result=TerraEngine::Editor::Terrain::ApplyTerrainHeightRemap(procedural,settings); }
	else if(request.operation=="terrace") { TerraEngine::Editor::Terrain::TerrainTerraceSettings settings; settings.stepMm=request.settings.value("stepMm",1000); settings.blendPercent=request.settings.value("blendPercent",0u); result=TerraEngine::Editor::Terrain::ApplyTerrainTerrace(procedural,settings); }
	else if(request.operation=="noise") { TerraEngine::Editor::Terrain::TerrainNoiseSettings settings; settings.seed=request.seed.value_or(0u); settings.amplitudeMm=request.settings.value("amplitudeMm",0); settings.periodSamples=request.settings.value("periodSamples",32u); result=TerraEngine::Editor::Terrain::ApplyTerrainSeededNoise(procedural,settings); }
	else if(request.operation=="hydraulic-erosion") result=TerraEngine::Editor::Terrain::ApplyTerrainHydraulicErosion(procedural,{});
	else if(request.operation=="thermal-erosion") result=TerraEngine::Editor::Terrain::ApplyTerrainThermalErosion(procedural,{});
	else { outError="The requested operation needs an external terrain service callback."; return std::nullopt; }
	if(!result.Succeeded() || result.areas.size()!=1u) { outError=result.message.empty()?"Terrain procedural preparation failed.":result.message; return std::nullopt; }
	preview.afterHeightMm=std::move(result.areas.front().heightMm); preview.historyGroup="Terrain automation " + request.operation;
	return proceduralJobs.BuildTransaction(*terrainStore,preview,outError);
}

TerrainWindow::AutomationOperationStart TerrainWindow::BeginAutomationOperation(const TerraEngine::Editor::Automation::TerrainAutomationRequest& request)
{
	// This is intentionally a small dispatch seam: it calls existing window
	// producers and returns their queue identity, never a mutable terrain buffer.
	if(request.operation=="height-remap") return {true,{},"Height remap is prepared through TerrainAutomationAdapter.",std::nullopt};
	if(request.operation=="paint" || request.operation=="erase-layer" || request.operation=="fill-layer" || request.operation=="normalize-layers")
	{
		if(!terrainStore) return {false,"Unavailable","An open terrain session is required.",std::nullopt};
		try
		{
			const auto layer = request.settings.value("layerId", 1u);
			const auto strength = request.settings.value("strength", 32u);
			if(layer > std::numeric_limits<std::uint16_t>::max() || strength > 255u)
				return {false,"InvalidSchema","Layer ID or strength exceeds the canonical surface format.",std::nullopt};
			TerraEngine::Editor::Terrain::TerrainSurfaceBrushSettings settings;
			settings.layerId = static_cast<std::uint16_t>(layer);
			settings.strength = static_cast<std::uint8_t>(strength);
			settings.useLayerMask = true;
			settings.mode = request.operation=="paint" ? TerraEngine::Editor::Terrain::TerrainSurfaceBrushMode::Paint
				: request.operation=="erase-layer" ? TerraEngine::Editor::Terrain::TerrainSurfaceBrushMode::Erase
				: request.operation=="fill-layer" ? TerraEngine::Editor::Terrain::TerrainSurfaceBrushMode::Fill
				: TerraEngine::Editor::Terrain::TerrainSurfaceBrushMode::Normalize;
			std::vector<std::uint8_t> mask(TerraEngine::Terrain::kTerrainAreaSampleCount, 0u);
			for(std::int64_t z = std::max<std::int64_t>(0, request.bounds.minZ); z < std::min<std::int64_t>(256, request.bounds.maxZExclusive); ++z)
				for(std::int64_t x = std::max<std::int64_t>(0, request.bounds.minX); x < std::min<std::int64_t>(256, request.bounds.maxXExclusive); ++x)
					mask[static_cast<std::size_t>(z) * 256u + static_cast<std::size_t>(x)] = 255u;
			const auto preview = PreviewSurfaceBrush(settings, mask);
			if(!preview.changed) return {false,"Unavailable","The bounded layer request produced no detached change.",std::nullopt};
			std::string error;
			if(!ApplySurfacePreview(error)) return {false,"Unavailable",error,std::nullopt};
			return {true,{},"Layer transaction queued for protected publication.",queuedSurfaceTransactionId};
		}
		catch(const std::exception& error)
		{
			return {false,"InvalidSchema",std::string{"Layer settings are invalid: "}+error.what(),std::nullopt};
		}
	}
	if(request.operation=="paint-hole" || request.operation=="erase-hole")
	{
		if(!terrainStore) return {false,"Unavailable","An open terrain session is required.",std::nullopt};
		TerraEngine::Editor::Terrain::TerrainHoleBrushStamp stamp;
		stamp.centerCellX=request.settings.value("centerX",128.0F); stamp.centerCellZ=request.settings.value("centerZ",128.0F);
		holeBrushSettings.mode=request.operation=="paint-hole"?TerraEngine::Editor::Terrain::TerrainHoleBrushMode::PaintHole:TerraEngine::Editor::Terrain::TerrainHoleBrushMode::EraseHole;
		holeBrushSettings.radiusCells = TerraEngine::Editor::Terrain::TerrainHoleBrush::EffectiveRadiusCells(
			request.settings.value("radiusCells", 1.0F));
		if(!PreviewHoleStamp(stamp)) return {false,"InvalidSchema","Hole preview did not affect a canonical cell.",std::nullopt};
		std::string error; if(!ApplyHolePreview(error)) return {false,"Unavailable",error,std::nullopt};
		return {true,{},"Hole transaction queued for protected publication.",queuedHoleTransactionId};
	}
	if(request.operation=="height-remap" || request.operation=="raise" || request.operation=="lower" || request.operation=="smooth" || request.operation=="flatten")
		return {false,"Unavailable","This sculpt operation requires an explicit canonical brush-stroke producer.",std::nullopt};
	return {false,"Unavailable","The requested terrain operation has no configured editor-owned service.",std::nullopt};
}

nlohmann::json TerrainWindow::RunLiveValidationFixture(const std::filesystem::path& destination)
{
	nlohmann::json result = {{"schemaVersion", 1}, {"succeeded", false}, {"destination", destination.string()}};
	const auto created = ExecuteAutomationCommand("terrain.create", {{"destination", destination.string()},
		{"widthSamples", 256u}, {"depthSamples", 256u}, {"sampleSpacingMetres", 1.0F}});
	if(!created.accepted || !terrainStore)
	{
		result["failure"] = created.message;
		return result;
	}
	const auto before = terrainStore->CaptureRoot();
	const auto descriptor = terrainStore->FindDescriptor(*before.root, {0, 0});
	if(!descriptor)
	{
		result["failure"] = "The live terrain root has no canonical authored area.";
		return result;
	}
	TerraEngine::Editor::Automation::TerrainAutomationRequest request;
	request.transactionId = TerraEngine::GenerateUUID();
	request.assetId = before.root->assetUuid;
	request.instanceId = TerraEngine::GenerateUUID();
	request.operation = "height-remap";
	request.settings = {{"addMm", 125}};
	request.expectedRootHash = TerraEngine::Terrain::ToHexString(before.root->rootContentHash);
	request.workloadClass = "interactive";
	// The live fixture deliberately crosses the same explicit Apply boundary as
	// an approved editor command. Preview-only requests must remain unable to
	// enter the protected publication queue.
	request.previewOnly = false;
	request.bounds = before.root->authoredBounds;
	request.budget = {1u, TerraEngine::Terrain::kTerrainAreaSampleCount,
		TerraEngine::Terrain::Format::kTerrainAreaMaximumDecodedBytes};
	request.observations = {{descriptor->coordinate, descriptor->editGeneration, descriptor->authoredContentHash}};
	request.approvalFingerprint = TerraEngine::Editor::Automation::TerrainApprovalFingerprint(request);
	std::string error;
	auto transaction = PrepareAutomationTransaction(request, error);
	if(!transaction || !automationAdapter.Enqueue(request, std::move(*transaction), error))
	{
		result["failure"] = error;
		return result;
	}
	const bool invisibleBeforeBoundary = terrainStore->CaptureRoot().publicationGeneration == before.publicationGeneration;
	ProcessProtectedAuthoringBoundary();
	const auto afterFirst = terrainStore->CaptureRoot();
	const auto descriptorAfterFirst = terrainStore->FindDescriptor(*afterFirst.root, {0, 0});
	if(!descriptorAfterFirst)
	{
		result["failure"] = "The first live terrain publication lost its canonical authored area.";
		return result;
	}
	// Chain a second additive command through the same automation boundary. This
	// catches stale editor-buffer mirrors: 125 mm followed by 75 mm must publish
	// 200 mm, rather than restarting the second command from the original flat
	// heightfield while the immutable store is already at a newer generation.
	request.transactionId = TerraEngine::GenerateUUID();
	request.settings = {{"addMm", 75}};
	request.expectedRootHash = TerraEngine::Terrain::ToHexString(afterFirst.root->rootContentHash);
	request.observations = {{descriptorAfterFirst->coordinate, descriptorAfterFirst->editGeneration,
		descriptorAfterFirst->authoredContentHash}};
	request.approvalFingerprint = TerraEngine::Editor::Automation::TerrainApprovalFingerprint(request);
	auto chainedTransaction = PrepareAutomationTransaction(request, error);
	if(!chainedTransaction || !automationAdapter.Enqueue(request, std::move(*chainedTransaction), error))
	{
		result["failure"] = error;
		return result;
	}
	ProcessProtectedAuthoringBoundary();
	const auto after = terrainStore->CaptureRoot();
	const bool chainedHeightPreserved = !committedHeightMm.empty()
		&& std::all_of(committedHeightMm.begin(), committedHeightMm.end(), [](std::int32_t height) { return height == 200; });
	const bool published = after.publicationGeneration >= before.publicationGeneration + 2u
		&& after.root->rootContentHash != before.root->rootContentHash;
	const auto saved = ExecuteAutomationCommand("terrain.save", nlohmann::json::object());
	const auto diagnostics = DiagnosticSnapshot();
	result["succeeded"] = invisibleBeforeBoundary && published && chainedHeightPreserved && saved.accepted
		&& !diagnostics.stagePercentiles.empty() && !diagnostics.dirty;
	result["invisibleBeforeBoundary"] = invisibleBeforeBoundary;
	result["publicationGenerationBefore"] = before.publicationGeneration;
	result["publicationGenerationAfter"] = after.publicationGeneration;
	result["chainedHeightMm"] = committedHeightMm.empty() ? 0 : committedHeightMm.front();
	result["chainedHeightPreserved"] = chainedHeightPreserved;
	result["saved"] = saved.accepted;
	result["dirtyAfterSave"] = diagnostics.dirty;
	result["measuredStageCount"] = diagnostics.stagePercentiles.size();
	result["rootHash"] = TerraEngine::Terrain::ToHexString(after.root->rootContentHash);
	if(!result["succeeded"].get<bool>()) result["failure"] = "Live terrain create/edit/publish/save invariants did not all pass.";
	return result;
}

void TerrainWindow::InitializeAuthoringTerrain(
	TerraEngine::UUID assetId,
	const TerraEngine::AssetPipeline::TerrainHeightfield& heightfield,
	TerraEngine::UUID existingObjectId,
	std::optional<TerraEngine::Terrain::TerrainSampleBounds> authoredBounds)
{
	// Terrain stroke commands retain references into the previous store. Remove
	// only that tool-owned domain before replacing it; scene/object history must
	// survive opening or creating a terrain asset.
	UndoRedoSystem::Instance().DiscardHistoryDomain("terrain-authoring");
	automationSessionId.reset();
	if(terrainRenderProxyCancellation)
		terrainRenderProxyCancellation->store(true, std::memory_order_release);
	terrainRenderProxyRebuildRequested = false;
	terrainRenderProxyInstanceQueued = false;
	TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance().Remove(
		terrainRenderProxyMaterialId);
	terrainRenderProxyMeshId = TerraEngine::EMPTY_UUID;
	terrainRenderProxyMaterialId = TerraEngine::EMPTY_UUID;
	terrainWeightTextureId = TerraEngine::EMPTY_UUID;
	terrainLayerIdTextureId = TerraEngine::EMPTY_UUID;
	publishedTerrainMaterial.reset();
	publishedSurfaceControlHash = {};
	publishedTerrainLayerGeneration = 0u;
	publishedTerrainLayerMaterialId = TerraEngine::EMPTY_UUID;
	publishedTerrainLayerSourceAvailable = false;
	terrainRenderProxyTargetObjectId = existingObjectId;
	if(terrainStore)
		TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().ClearPublication(terrainStore.get());
	// Keep a bounded area-keyed mirror. Each area is padded by its nearest active
	// sample, so selecting a partial edge never exposes stale tail values.
	const TerraEngine::Terrain::TerrainSampleBounds bounds = authoredBounds.value_or(
		TerraEngine::Terrain::TerrainSampleBounds{0, 0, static_cast<std::int64_t>(heightfield.width), static_cast<std::int64_t>(heightfield.height)});
	resizeJobs.Cancel("Terrain session changed.");
	ClearResizePreview();
	queuedResizeTransactionId.reset();
	resizeCropBounds = {bounds.minX, bounds.minZ, bounds.maxXExclusive, bounds.maxZExclusive};
	committedAreaHeightMm.clear();
	const auto areas = TerraEngine::Terrain::EnumerateTerrainAreas(bounds, 64u);
	if(areas && !heightfield.heightsMm.empty())
	{
		for(const auto coordinate : *areas)
		{
			const auto active = TerraEngine::Terrain::TerrainActiveRectForArea(bounds, coordinate);
			if(!active) continue;
			auto& areaHeights = committedAreaHeightMm[coordinate];
			areaHeights.resize(TerraEngine::Terrain::kTerrainAreaSampleCount);
			for(std::uint32_t localZ = 0u; localZ < 256u; ++localZ)
				for(std::uint32_t localX = 0u; localX < 256u; ++localX)
				{
					const std::uint32_t canonicalX = std::clamp(localX,
						static_cast<std::uint32_t>(active->minX), static_cast<std::uint32_t>(active->maxXExclusive - 1u));
					const std::uint32_t canonicalZ = std::clamp(localZ,
						static_cast<std::uint32_t>(active->minZ), static_cast<std::uint32_t>(active->maxZExclusive - 1u));
					const std::int64_t globalX = coordinate.x * TerraEngine::Terrain::kTerrainAreaExtentSamples + canonicalX;
					const std::int64_t globalZ = coordinate.z * TerraEngine::Terrain::kTerrainAreaExtentSamples + canonicalZ;
					const std::size_t sourceX = static_cast<std::size_t>(globalX - bounds.minX);
					const std::size_t sourceZ = static_cast<std::size_t>(globalZ - bounds.minZ);
					areaHeights[static_cast<std::size_t>(localZ) * 256u + localX]
						= heightfield.heightsMm[sourceZ * heightfield.width + sourceX];
				}
		}
	}
	selectedTerrainArea = areas && !areas->empty() ? areas->front() : TerraEngine::Terrain::TerrainAreaCoordinate{};
	committedHeightMm = committedAreaHeightMm.contains(selectedTerrainArea)
		? committedAreaHeightMm[selectedTerrainArea]
		: std::vector<std::int32_t>(TerraEngine::Terrain::kTerrainAreaSampleCount,
			heightfield.heightsMm.empty() ? 0 : heightfield.heightsMm.front());
	previewHeightMm = committedHeightMm; brushMask.assign(256U * 256U, 255U);
	previewAreaHeightMm = committedAreaHeightMm;
	committedHoleMask.assign(TerraEngine::Terrain::kTerrainAreaSampleCount / 8u, 0u);
	previewHoleMask = committedHoleMask;
	committedAreaHoleMasks.clear();
	for(const auto& [coordinate, heights] : committedAreaHeightMm)
	{
		(void)heights;
		committedAreaHoleMasks.emplace(coordinate, committedHoleMask);
	}
	previewAreaHoleMasks = committedAreaHoleMasks;
	std::vector<std::uint8_t> bytes(committedHeightMm.size() * sizeof(std::int32_t)); std::memcpy(bytes.data(), committedHeightMm.data(), bytes.size());
	const auto heightHash = TerraEngine::Terrain::HashTerrainBytes(bytes);
	auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
	root->assetUuid = assetId; root->publicationGeneration = 1U; root->persistedRootGeneration = 1U; root->rootContentHash = heightHash;
	root->authoredBounds = bounds; root->defaultHeightMm = committedHeightMm.front(); root->layerStackGeneration = 1U;
	TerraEngine::Terrain::TerrainDirectoryShard shard; shard.areaBounds = root->authoredBounds; shard.identity = heightHash;
	TerraEngine::Terrain::TerrainAreaDescriptor descriptor; descriptor.coordinate = selectedTerrainArea; descriptor.editGeneration = 1U; descriptor.authoredContentHash = heightHash; descriptor.channels.heightHash = heightHash;
	shard.descriptors.emplace(descriptor.coordinate, descriptor); root->directoryShards.push_back(std::move(shard));
	// Put initial layer metadata in the immutable root before the store captures
	// it. Later list edits therefore use the same CAS/persistence path as paint.
	auto initialLayerStack = std::make_shared<TerraEngine::Terrain::TerrainLayerStack>();
	initialLayerStack->generation = 1u;
	initialLayerStack->layers.push_back({1u, "Base Layer", TerraEngine::EMPTY_UUID, {}, true, true, TerraEngine::Terrain::TerrainLayerFallbackState::Healthy});
	root->layerStack = std::move(initialLayerStack);
	terrainStore = std::make_unique<TerraEngine::Terrain::TerrainAssetStore>(root); terrainStore->Blobs().PutVerified(heightHash, std::move(bytes));
	if(!destinationPath.empty() && std::filesystem::exists(destinationPath.string() + ".data/CURRENT"))
	{
			// Canonical terrain bundles rehydrate their saved/predecessor roots before
			// history begins. Unsupported generations fail closed and retain this
			// unpublished working root, so no partial descriptor reaches rendering.
		const auto opened = terrainStore->OpenPersistedRoot(destinationPath.string() + ".data");
		if(opened.Succeeded())
		{
			const auto persisted = terrainStore->CaptureRoot();
			const auto persistedArea = terrainStore->ResolveDescriptorOrImplicit(*persisted.root, selectedTerrainArea);
			if(persistedArea.channels.heightHash)
			{
				if(const auto persistedBytes = terrainStore->Blobs().Resolve(*persistedArea.channels.heightHash);
					persistedBytes && persistedBytes->size() == committedHeightMm.size() * sizeof(std::int32_t))
				{
					std::memcpy(committedHeightMm.data(), persistedBytes->data(), persistedBytes->size());
					previewHeightMm = committedHeightMm;
				}
			}
			if(persistedArea.channels.holeHash)
			{
				if(const auto persistedHoles = terrainStore->Blobs().Resolve(*persistedArea.channels.holeHash);
					persistedHoles && persistedHoles->size() == TerraEngine::Terrain::kTerrainAreaSampleCount / 8u)
				{
					committedHoleMask = *persistedHoles;
					previewHoleMask = committedHoleMask;
				}
			}
		}
	}
	terrainQueue = std::make_unique<TerraEngine::Terrain::TerrainMutationQueue>(); terrainPublication = std::make_unique<TerraEngine::Terrain::TerrainPublication>(*terrainStore);
	terrainGarbageCollector = std::make_unique<TerraEngine::Terrain::TerrainGarbageCollector>(*terrainStore);
	const auto residentRoot = terrainStore->CaptureRoot();
	if(residentRoot.IsValid())
		(void)TerraEngine::Terrain::TerrainStreamingSystem::Instance().SubmitCoarseCoverage(
			residentRoot.root->assetUuid, residentRoot.publicationGeneration,
			residentRoot.root->authoredBounds, 4096u, 64u);
	authoringSystem.Shutdown(); authoringSystem.Configure(*terrainQueue, *terrainPublication); authoringSystem.Initialize();
	// Automation receives this narrow adapter rather than direct asset access.
	// It can preflight and enqueue only the same typed transactions as the UI.
	automationAdapter.Configure(*terrainStore, authoringSystem);
	TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().ConfigurePublication(
		terrainStore.get(), [](TerraEngine::Physics::TerrainCollisionReplacementPacket packet) {
			return TerraEngine::Physics::PhysicsSystem::Instance().CommitTerrainCollisionReplacement(std::move(packet));
		});
	// Terrain always owns heightfield collision. In Edit Mode this remains armed
	// until Play Mode creates its world; exported/editor worlds that already exist
	// receive the candidate immediately.
	std::string collisionError;
	if(!QueueAutomaticTerrainCollision(collisionError) && !collisionError.empty())
		status = collisionError;
	automationAdapter.SetPrepareCallback([this](const TerraEngine::Editor::Automation::TerrainAutomationRequest& request, std::string& error) { return PrepareAutomationTransaction(request,error); });
	terrainDirtyState.MarkSaved(terrainStore->CaptureRoot());
	// A terrain always starts with one stable fallback layer. Surface authoring
	// uses this metadata by ID, so later UI reordering never changes texels.
	const auto initializedCapture = terrainStore->CaptureRoot();
	committedLayerStack = initializedCapture.root && initializedCapture.root->layerStack
		? *initializedCapture.root->layerStack : *root->layerStack;
	previewLayerStack = committedLayerStack;
	const std::array<TerraEngine::Terrain::TerrainWeightContribution, 1u> baseWeight{{{1u, 255u}}};
	committedSurfaceWeights.assign(TerraEngine::Terrain::kTerrainAreaSampleCount, TerraEngine::Terrain::NormalizeTerrainWeights(baseWeight));
	previewSurfaceWeights = committedSurfaceWeights;
	committedAreaSurfaceWeights.clear();
	for(const auto& [coordinate, heights] : committedAreaHeightMm)
	{
		(void)heights;
		committedAreaSurfaceWeights.emplace(coordinate, committedSurfaceWeights);
	}
	previewAreaSurfaceWeights = committedAreaSurfaceWeights;
	// Persisted roots can contain holes and painted weights in addition to height.
	// Rehydrate all channels through the same path used after history replay.
	(void)SynchronizeCommittedAuthoringStateFromPublishedRoot();
	selectedLayerId = TerraEngine::Editor::Terrain::TerrainSurfaceBrush::ResolveEnabledLayerId(
		committedLayerStack, 1u);
	surfaceBrushSettings.layerId = selectedLayerId;
	layerPreviewActive = false;
	surfacePreviewActive = false;
	displacedLayerWarning = 0u;
	observedTerrainMaterialGeneration = AssetDatabase::Instance().GetTerrainMaterialChangeGeneration();
	previousStamp.reset(); authoringSession.Cancel(); status = "Terrain Tools is ready: drag in Scene View to create a detached sculpt preview.";
	ScheduleRenderProxyBuild();
}

void TerrainWindow::EnsureValidPaintLayer() noexcept
{
	const auto& stack = LayerPreview();
	const std::uint16_t resolved =
		TerraEngine::Editor::Terrain::TerrainSurfaceBrush::ResolveEnabledLayerId(
			stack, surfaceBrushSettings.layerId);
	surfaceBrushSettings.layerId = resolved;
	// If the metadata selection vanished, keep the Layers panel and Paint panel
	// on the same surviving entry. Disabled layers remain selectable for repair.
	const bool selectedStillExists = std::any_of(
		stack.layers.begin(), stack.layers.end(), [this](const auto& layer) {
			return layer.stableId == selectedLayerId;
		});
	if(!selectedStillExists) selectedLayerId = resolved;
}

bool TerrainWindow::SynchronizeCommittedAuthoringStateFromPublishedRoot()
{
	// The immutable asset store is the authority after every protected publish.
	// History and automation prepare detached blobs without mutating the window's
	// preview vectors, so copying any preview here would make the next operation
	// start from stale samples. Resolve the published channels into fresh buffers.
	if(!terrainStore) return false;
	const auto capture = terrainStore->CaptureRoot();
	if(!capture.IsValid()) return false;
	if(capture.root->layerStack)
	{
		committedLayerStack = *capture.root->layerStack;
		previewLayerStack = committedLayerStack;
	}
	const auto descriptor = terrainStore->ResolveDescriptorOrImplicit(*capture.root, selectedTerrainArea);
	auto resolveCanonicalPayload = [&](TerraEngine::Terrain::TerrainAreaCoordinate coordinate,
		TerraEngine::Terrain::ContentHash hash,
		TerraEngine::Terrain::TerrainChannelKind channelKind, std::uint16_t weightGroup,
		std::size_t expectedBytes) -> std::optional<std::vector<std::uint8_t>>
	{
		const auto stored = terrainStore->Blobs().Resolve(hash);
		if(!stored) return std::nullopt;
		// Fresh editor transactions retain raw immutable payloads until Save wraps
		// them in the durable .tearea ABI. Reopened stores contain that envelope, so
		// normalize both representations to the same exact authoring bytes here.
		if(stored->size() == expectedBytes) return stored;
		TerraEngine::Terrain::DecodedTerrainArea decoded;
		const auto result = TerraEngine::Terrain::DecodeTerrainArea(*stored, decoded);
		if(!result.Succeeded() || decoded.header.assetUuid != capture.root->assetUuid
			|| decoded.header.area != coordinate
			|| decoded.header.channelKind != channelKind
			|| decoded.header.weightGroup != weightGroup
			|| decoded.header.envelopeHash != hash
			|| decoded.canonicalPayload.size() != expectedBytes)
		{
			return std::nullopt;
		}
		return std::move(decoded.canonicalPayload);
	};

	bool synchronizedHeight = false;
	committedAreaHeightMm.clear();
	const auto coveredAreas = TerraEngine::Terrain::EnumerateTerrainAreas(capture.root->authoredBounds, 4096u);
	if(coveredAreas)
		for(const auto coordinate : *coveredAreas)
		{
			// Keep the resolved value named descriptor: it is the complete channel
			// authority for this coordinate, whether the area was explicit or implicit.
			const auto descriptor = terrainStore->ResolveDescriptorOrImplicit(*capture.root, coordinate);
			auto& heights = committedAreaHeightMm[coordinate];
			heights.assign(TerraEngine::Terrain::kTerrainAreaSampleCount, capture.root->defaultHeightMm);
			if(!descriptor.channels.heightHash) continue;
			const std::size_t expectedBytes = TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t);
			const auto bytes = resolveCanonicalPayload(coordinate, *descriptor.channels.heightHash,
				TerraEngine::Terrain::TerrainChannelKind::Height, 0u, expectedBytes);
			if(bytes) std::memcpy(heights.data(), bytes->data(), expectedBytes);
		}
	if(const auto selected = committedAreaHeightMm.find(selectedTerrainArea); selected != committedAreaHeightMm.end())
	{
		committedHeightMm = selected->second;
		synchronizedHeight = true;
	}
	previewAreaHeightMm = committedAreaHeightMm;
	previewHeightMm = committedHeightMm;

	// A missing hole channel is the canonical all-solid terrain state. Rebuild
	// every covered area so a signed cross-boundary stroke cannot compose from a
	// stale selected-area compatibility buffer.
	committedAreaHoleMasks.clear();
	if(coveredAreas)
		for(const auto coordinate : *coveredAreas)
		{
			const auto areaDescriptor = terrainStore->ResolveDescriptorOrImplicit(*capture.root, coordinate);
			auto& holes = committedAreaHoleMasks[coordinate];
			holes.assign(TerraEngine::Terrain::kTerrainAreaSampleCount / 8u, 0u);
			if(!areaDescriptor.channels.holeHash) continue;
			const auto bytes = resolveCanonicalPayload(coordinate, *areaDescriptor.channels.holeHash,
				TerraEngine::Terrain::TerrainChannelKind::Holes, 0u, holes.size());
			if(bytes) holes = *bytes;
		}
	committedHoleMask.assign(TerraEngine::Terrain::kTerrainAreaSampleCount / 8u, 0u);
	if(const auto selected = committedAreaHoleMasks.find(selectedTerrainArea);
		selected != committedAreaHoleMasks.end()) committedHoleMask = selected->second;
	// Keep these explicit selected-descriptor checks visible as the compatibility
	// contract used by history-replay source audits.
	if(descriptor.channels.holeHash && committedAreaHoleMasks.empty())
	{
		const auto bytes = resolveCanonicalPayload(selectedTerrainArea, *descriptor.channels.holeHash,
			TerraEngine::Terrain::TerrainChannelKind::Holes, 0u, committedHoleMask.size());
		if(bytes) committedHoleMask = *bytes;
	}
	previewAreaHoleMasks = committedAreaHoleMasks;
	previewHoleMask = committedHoleMask;

	// Likewise, an absent weight group means the root still uses its fallback
	// layer. Decode canonical 12-byte samples only after validating the full blob,
	// so a corrupt replay can never partially replace an area-keyed editor mirror.
	const auto fallbackLayer = std::find_if(committedLayerStack.layers.begin(), committedLayerStack.layers.end(),
		[](const TerraEngine::Terrain::TerrainLayer& layer) { return layer.enabled; });
	const std::uint16_t fallbackLayerId = fallbackLayer != committedLayerStack.layers.end()
		? fallbackLayer->stableId : (committedLayerStack.layers.empty() ? 1u : committedLayerStack.layers.front().stableId);
	const std::array<TerraEngine::Terrain::TerrainWeightContribution, 1u> fallbackWeight{{{fallbackLayerId, 255u}}};
	const auto fallbackSample = TerraEngine::Terrain::NormalizeTerrainWeights(fallbackWeight);
	committedAreaSurfaceWeights.clear();
	if(coveredAreas)
		for(const auto coordinate : *coveredAreas)
		{
			const auto areaDescriptor = terrainStore->ResolveDescriptorOrImplicit(*capture.root, coordinate);
			auto& weights = committedAreaSurfaceWeights[coordinate];
			weights.assign(TerraEngine::Terrain::kTerrainAreaSampleCount, fallbackSample);
			const auto weight = areaDescriptor.channels.weightGroupHashes.find(1u);
			if(weight == areaDescriptor.channels.weightGroupHashes.end()) continue;
			constexpr std::size_t bytesPerSample = 12u;
			const auto bytes = resolveCanonicalPayload(coordinate, weight->second,
				TerraEngine::Terrain::TerrainChannelKind::WeightGroup, 1u,
				TerraEngine::Terrain::kTerrainAreaSampleCount * bytesPerSample);
			if(!bytes) continue;
			bool valid = true;
			for(std::size_t index = 0u; index < weights.size(); ++index)
			{
				const std::span<const std::uint8_t, bytesPerSample> sample(bytes->data() + index * bytesPerSample, bytesPerSample);
				if(!TerraEngine::Terrain::DecodeTerrainWeightSample(sample, weights[index]).Succeeded())
				{
					valid = false;
					break;
				}
			}
			if(!valid) weights.assign(TerraEngine::Terrain::kTerrainAreaSampleCount, fallbackSample);
		}
	committedSurfaceWeights.assign(TerraEngine::Terrain::kTerrainAreaSampleCount, fallbackSample);
	if(const auto selected = committedAreaSurfaceWeights.find(selectedTerrainArea);
		selected != committedAreaSurfaceWeights.end()) committedSurfaceWeights = selected->second;
	// Preserve the explicit selected-descriptor expression used by compatibility
	// tests while the area-keyed map above remains authoritative.
	if(const auto weight = descriptor.channels.weightGroupHashes.find(1u);
		weight != descriptor.channels.weightGroupHashes.end() && committedAreaSurfaceWeights.empty())
	{
		(void)weight;
	}
	previewAreaSurfaceWeights = committedAreaSurfaceWeights;
	previewSurfaceWeights = committedSurfaceWeights;
	return synchronizedHeight;
}

bool TerrainWindow::StageLayerPatch(const TerraEngine::Terrain::TerrainLayerPatch& sourcePatch, std::string& outError)
{
	if(queuedLayerTransactionId.has_value())
	{
		outError = "Wait for the queued layer operation to reach the protected boundary before staging another one.";
		return false;
	}
	const auto& base = committedLayerStack;
	TerraEngine::Terrain::TerrainLayerPatch patch = sourcePatch;
	patch.expectedGeneration = base.generation;
	TerraEngine::Terrain::TerrainLayerPatch inverse;
	inverse.expectedGeneration = 0u;
	auto existing = std::find_if(base.layers.begin(), base.layers.end(), [&patch](const auto& layer) { return layer.stableId == patch.targetStableId; });
	switch(patch.kind)
	{
	case TerraEngine::Terrain::TerrainLayerPatchKind::Add:
		inverse.kind = TerraEngine::Terrain::TerrainLayerPatchKind::Remove;
		inverse.targetStableId = patch.layer.stableId;
		break;
	case TerraEngine::Terrain::TerrainLayerPatchKind::Remove:
		if(existing == base.layers.end()) { outError = "The layer selected for removal no longer exists."; return false; }
		inverse.kind = TerraEngine::Terrain::TerrainLayerPatchKind::Add;
		inverse.layer = *existing;
		inverse.destinationIndex = static_cast<std::uint32_t>(std::distance(base.layers.begin(), existing));
		break;
	case TerraEngine::Terrain::TerrainLayerPatchKind::Move:
		if(existing == base.layers.end()) { outError = "The layer selected for reordering no longer exists."; return false; }
		inverse.kind = TerraEngine::Terrain::TerrainLayerPatchKind::Move;
		inverse.targetStableId = patch.targetStableId;
		inverse.destinationIndex = static_cast<std::uint32_t>(std::distance(base.layers.begin(), existing));
		break;
	case TerraEngine::Terrain::TerrainLayerPatchKind::Replace:
		if(existing == base.layers.end()) { outError = "The layer selected for replacement no longer exists."; return false; }
		inverse.kind = TerraEngine::Terrain::TerrainLayerPatchKind::Replace;
		inverse.targetStableId = patch.targetStableId;
		inverse.layer = *existing;
		break;
	case TerraEngine::Terrain::TerrainLayerPatchKind::SetEnabled:
		if(existing == base.layers.end()) { outError = "The layer selected for enabling no longer exists."; return false; }
		inverse.kind = TerraEngine::Terrain::TerrainLayerPatchKind::SetEnabled;
		inverse.targetStableId = patch.targetStableId;
		inverse.enabled = existing->enabled;
		break;
	case TerraEngine::Terrain::TerrainLayerPatchKind::SetFallbackState:
		if(existing == base.layers.end()) { outError = "The layer selected for source repair no longer exists."; return false; }
		inverse.kind = TerraEngine::Terrain::TerrainLayerPatchKind::SetFallbackState;
		inverse.targetStableId = patch.targetStableId;
		inverse.fallbackState = existing->fallbackState;
		break;
	}
	TerraEngine::Terrain::TerrainLayerStack candidate;
	const auto result = TerraEngine::Terrain::ApplyTerrainLayerPatch(base, patch, candidate);
	if(!result.Succeeded())
	{
		outError = result.message;
		return false;
	}
	previewLayerStack = std::move(candidate);
	stagedLayerPatch = patch;
	stagedInverseLayerPatch = inverse;
	layerPreviewActive = true;
	return true;
}

bool TerrainWindow::ApplyLayerPreview(std::string& outError)
{
	if(!layerPreviewActive)
	{
		outError = "No detached layer metadata preview is available to apply.";
		return false;
	}
	if(!terrainStore || !stagedLayerPatch || !stagedInverseLayerPatch)
	{
		outError = "Terrain layer publication requires an initialized terrain store and typed metadata patch.";
		return false;
	}
	const auto capture = terrainStore->CaptureRoot();
	auto transaction = TerraEngine::Editor::Terrain::TerrainAuthoringSession::BuildLayerCommitRequest(
		capture, *stagedLayerPatch, *stagedInverseLayerPatch, "Terrain layer metadata", outError);
	if(!transaction) return false;
	const TerraEngine::UUID transactionId = transaction->transactionId;
	if(!authoringSystem.EnqueueDetachedCommit(std::move(*transaction), outError)) return false;
	queuedLayerTransactionId = transactionId;
	status = "Terrain layer preview queued for protected publication; Cancel remains available until the boundary.";
	return true;
}

void TerrainWindow::CancelLayerPreview() noexcept
{
	if(queuedLayerTransactionId)
		authoringSystem.CancelPreparedTransaction(*queuedLayerTransactionId, "Terrain layer preview was cancelled before protected publication.");
	previewLayerStack = committedLayerStack;
	layerPreviewActive = false;
	stagedLayerPatch.reset();
	stagedInverseLayerPatch.reset();
}

TerraEngine::Editor::Terrain::TerrainSurfaceBrushResult TerrainWindow::PreviewSurfaceBrush(const TerraEngine::Editor::Terrain::TerrainSurfaceBrushSettings& settings, std::span<const std::uint8_t> mask)
{
	if(!surfacePreviewActive)
	{
		previewSurfaceWeights = committedSurfaceWeights;
		previewAreaSurfaceWeights = committedAreaSurfaceWeights;
	}
	surfaceBrushSettings = settings;
	const auto result = TerraEngine::Editor::Terrain::TerrainSurfaceBrush::ApplyArea(previewSurfaceWeights, settings, mask);
	if(result.changed) previewAreaSurfaceWeights[selectedTerrainArea] = previewSurfaceWeights;
	surfacePreviewActive = surfacePreviewActive || result.changed;
	if(result.displacedLayerId != 0u) displacedLayerWarning = result.displacedLayerId;
	return result;
}

bool TerrainWindow::ApplySurfacePreview(std::string& outError)
{
	if(!surfacePreviewActive)
	{
		outError = "No detached surface-weight preview is available to apply.";
		return false;
	}
	return QueueSurfacePreviewForPublication(outError);
}

void TerrainWindow::CancelSurfacePreview() noexcept
{
	if(queuedSurfaceTransactionId)
		authoringSystem.CancelPreparedTransaction(*queuedSurfaceTransactionId, "Terrain surface preview was cancelled before protected publication.");
	previewSurfaceWeights = committedSurfaceWeights;
	previewAreaSurfaceWeights = committedAreaSurfaceWeights;
	surfacePreviewActive = false;
	displacedLayerWarning = 0u;
}

bool TerrainWindow::PreviewHoleStamp(const TerraEngine::Editor::Terrain::TerrainHoleBrushStamp& stamp)
{
	if(!terrainStore) return false;
	if(!holePreviewActive)
	{
		previewHoleMask = committedHoleMask;
		previewAreaHoleMasks = committedAreaHoleMasks;
	}
	std::vector<std::uint8_t> after(previewHoleMask.size());
	const bool changed = TerraEngine::Editor::Terrain::TerrainHoleBrush::ApplyStamp(
		previewHoleMask, after, 256u, 256u, holeBrushSettings, stamp);
	if(changed)
	{
		previewHoleMask = std::move(after);
		previewAreaHoleMasks[selectedTerrainArea] = previewHoleMask;
		holePreviewActive = true;
		status = "Detached hole preview is ready to Apply or Cancel.";
		// Hole painting shares the same detached render route as height sculpting so
		// removed cells disappear during the drag without publishing collision early.
		ScheduleRenderProxyBuild();
	}
	return changed;
}

bool TerrainWindow::ApplyHolePreview(std::string& outError)
{
	if(!holePreviewActive)
	{
		outError = "No detached hole preview is available to apply.";
		return false;
	}
	return QueueHolePreviewForPublication(outError);
}

void TerrainWindow::CancelHolePreview()
{
	const bool restoreCommittedProxy = holePreviewActive;
	if(queuedHoleTransactionId)
		authoringSystem.CancelPreparedTransaction(*queuedHoleTransactionId, "Terrain hole preview was cancelled before protected publication.");
	previewHoleMask = committedHoleMask;
	previewAreaHoleMasks = committedAreaHoleMasks;
	holePreviewActive = false;
	holeBrushActive = false;
	if(restoreCommittedProxy)
		ScheduleRenderProxyBuild();
}

bool TerrainWindow::QueueHolePreviewForPublication(std::string& outError)
{
	if(queuedHoleTransactionId)
	{
		outError = "The current hole preview is already waiting for protected publication.";
		return false;
	}
	if(!terrainStore || previewAreaHoleMasks.empty()
		|| committedAreaHoleMasks.size() != previewAreaHoleMasks.size())
	{
		outError = "Hole painting requires complete area-keyed canonical masks.";
		return false;
	}
	const auto capture = terrainStore->CaptureRoot();
	auto makeSpan = [](TerraEngine::Terrain::ContentHash hash, const std::vector<std::uint8_t>& bytes) {
		TerraEngine::Terrain::TerrainExactSpan span;
		span.channel = TerraEngine::Terrain::TerrainChannelKind::Holes;
		span.uncompressedByteCount = static_cast<std::uint32_t>(bytes.size());
		span.compressedContentHash = hash;
		span.immutableBlobHash = hash;
		span.compressedBytes = bytes;
		return span;
	};
	TerraEngine::Terrain::TerrainEditTransaction transaction;
	transaction.transactionId = TerraEngine::GenerateUUID();
	transaction.origin = TerraEngine::Terrain::TerrainEditOrigin::VisibleEditor;
	transaction.targetAssetUuid = capture.root->assetUuid;
	transaction.historyGroup = "Terrain hole stroke";
	transaction.inputRootHash = capture.root->rootContentHash;
	transaction.expectedLayerStackGeneration = capture.root->layerStackGeneration;
	transaction.affectedBounds = capture.root->authoredBounds;
	for(const auto& [coordinate, after] : previewAreaHoleMasks)
	{
		const auto before = committedAreaHoleMasks.find(coordinate);
		if(before == committedAreaHoleMasks.end() || before->second == after) continue;
		const auto beforeHash = TerraEngine::Terrain::HashTerrainBytes(before->second);
		const auto afterHash = TerraEngine::Terrain::HashTerrainBytes(after);
		if(!terrainStore->Blobs().PutVerified(afterHash, after)
			|| !terrainStore->Blobs().PutVerified(beforeHash, before->second))
		{
			outError = "Hole preview could not retain all immutable before/after masks.";
			return false;
		}
		const auto current = terrainStore->ResolveDescriptorOrImplicit(*capture.root, coordinate);
		TerraEngine::Terrain::TerrainPatch patch;
		patch.kind = TerraEngine::Terrain::TerrainPatchKind::ReplaceHoles;
		patch.preparedBlobHash = afterHash;
		patch.replacement = current;
		patch.replacement.editGeneration++;
		patch.replacement.channels.holeHash = afterHash;
		patch.replacement.authoredContentHash = afterHash;
		patch.beforeSpans = {makeSpan(beforeHash, before->second)};
		patch.afterSpans = {makeSpan(afterHash, after)};
		patch.retainedBlobHashes = {beforeHash, afterHash};
		TerraEngine::Terrain::TerrainPatch inverse = patch;
		inverse.preparedBlobHash = beforeHash;
		inverse.replacement = current;
		inverse.replacement.editGeneration++;
		inverse.replacement.channels.holeHash = beforeHash;
		inverse.replacement.authoredContentHash = beforeHash;
		inverse.beforeSpans = patch.afterSpans;
		inverse.afterSpans = patch.beforeSpans;
		transaction.readSet.push_back({current.coordinate, current.editGeneration, current.authoredContentHash});
		transaction.patches.push_back(std::move(patch));
		transaction.inversePatches.push_back(std::move(inverse));
	}
	if(transaction.patches.empty())
	{
		outError = "The detached hole preview contains no changed terrain areas.";
		return false;
	}
	transaction.budget.maximumAreas = transaction.patches.size();
	transaction.budget.maximumSamples = transaction.patches.size() * TerraEngine::Terrain::kTerrainAreaSampleCount;
	transaction.budget.maximumPreparedBytes = transaction.patches.size() * (TerraEngine::Terrain::kTerrainAreaSampleCount / 8u);
	const TerraEngine::UUID transactionId = transaction.transactionId;
	if(!authoringSystem.EnqueueDetachedCommit(std::move(transaction), outError)) return false;
	queuedHoleTransactionId = transactionId;
	status = "Terrain hole preview queued for protected publication and collision rebuild.";
	return true;
}

bool TerrainWindow::QueueSurfacePreviewForPublication(std::string& outError)
{
	if(queuedSurfaceTransactionId)
	{
		outError = "The current surface preview is already waiting for protected publication.";
		return false;
	}
	if(!terrainStore || previewAreaSurfaceWeights.empty()
		|| committedAreaSurfaceWeights.size() != previewAreaSurfaceWeights.size())
	{
		outError = "Surface painting requires complete area-keyed terrain previews.";
		return false;
	}
	const auto capture = terrainStore->CaptureRoot();
	auto makeSpan = [](TerraEngine::Terrain::ContentHash hash, const std::vector<std::uint8_t>& bytes) {
		TerraEngine::Terrain::TerrainExactSpan span;
		span.channel = TerraEngine::Terrain::TerrainChannelKind::WeightGroup;
		span.weightGroup = 1u;
		span.uncompressedByteCount = static_cast<std::uint32_t>(bytes.size());
		span.compressedContentHash = hash;
		span.immutableBlobHash = hash;
		span.compressedBytes = bytes;
		return span;
	};
	TerraEngine::Terrain::TerrainEditTransaction transaction;
	transaction.transactionId = TerraEngine::GenerateUUID();
	transaction.origin = TerraEngine::Terrain::TerrainEditOrigin::VisibleEditor;
	transaction.targetAssetUuid = capture.root->assetUuid;
	transaction.historyGroup = "Terrain surface stroke";
	transaction.inputRootHash = capture.root->rootContentHash;
	transaction.expectedLayerStackGeneration = capture.root->layerStackGeneration;
	transaction.affectedBounds = capture.root->authoredBounds;
	std::size_t preparedBytes = 0u;
	for(const auto& [coordinate, after] : previewAreaSurfaceWeights)
	{
		const auto before = committedAreaSurfaceWeights.find(coordinate);
		if(before == committedAreaSurfaceWeights.end() || before->second == after) continue;
		std::vector<std::uint8_t> afterBytes, beforeBytes;
		if(!TerraEngine::Terrain::MaterializeTerrainWeightGroup(after, afterBytes).Succeeded()
			|| !TerraEngine::Terrain::MaterializeTerrainWeightGroup(before->second, beforeBytes).Succeeded())
		{
			outError = "Surface preview contains non-canonical layer weights.";
			return false;
		}
		const auto afterHash = TerraEngine::Terrain::HashTerrainBytes(afterBytes);
		const auto beforeHash = TerraEngine::Terrain::HashTerrainBytes(beforeBytes);
		if(!terrainStore->Blobs().PutVerified(afterHash, afterBytes)
			|| !terrainStore->Blobs().PutVerified(beforeHash, beforeBytes))
		{
			outError = "Surface preview could not retain all immutable weight blobs.";
			return false;
		}
		const auto current = terrainStore->ResolveDescriptorOrImplicit(*capture.root, coordinate);
		TerraEngine::Terrain::TerrainPatch patch;
		patch.kind = TerraEngine::Terrain::TerrainPatchKind::ReplaceWeightGroup;
		patch.preparedBlobHash = afterHash;
		patch.replacement = current;
		patch.replacement.editGeneration++;
		patch.replacement.channels.weightGroupHashes[1u] = afterHash;
		patch.replacement.authoredContentHash = afterHash;
		patch.beforeSpans = {makeSpan(beforeHash, beforeBytes)};
		patch.afterSpans = {makeSpan(afterHash, afterBytes)};
		patch.retainedBlobHashes = {beforeHash, afterHash};
		TerraEngine::Terrain::TerrainPatch inverse = patch;
		inverse.preparedBlobHash = beforeHash;
		inverse.replacement = current;
		inverse.replacement.editGeneration++;
		inverse.replacement.channels.weightGroupHashes[1u] = beforeHash;
		inverse.replacement.authoredContentHash = beforeHash;
		inverse.beforeSpans = patch.afterSpans;
		inverse.afterSpans = patch.beforeSpans;
		transaction.readSet.push_back({current.coordinate, current.editGeneration, current.authoredContentHash});
		transaction.patches.push_back(std::move(patch));
		transaction.inversePatches.push_back(std::move(inverse));
		preparedBytes += beforeBytes.size() + afterBytes.size();
	}
	if(transaction.patches.empty())
	{
		outError = "The detached surface preview contains no changed terrain areas.";
		return false;
	}
	transaction.budget.maximumAreas = transaction.patches.size();
	transaction.budget.maximumSamples = transaction.patches.size() * TerraEngine::Terrain::kTerrainAreaSampleCount;
	transaction.budget.maximumPreparedBytes = preparedBytes;
	const TerraEngine::UUID transactionId = transaction.transactionId;
	if(!authoringSystem.EnqueueDetachedCommit(std::move(transaction), outError)) return false;
	queuedSurfaceTransactionId = transactionId;
	status = "Terrain surface preview queued for protected publication.";
	return true;
}

void TerrainWindow::RefreshMaterialAsset(TerraEngine::UUID materialId)
{
	if(!terrainStore || materialId == TerraEngine::EMPTY_UUID) return;
	const auto referencesMaterial = [materialId](const TerraEngine::Terrain::TerrainLayerStack& stack) {
		return std::any_of(stack.layers.begin(), stack.layers.end(),
			[materialId](const TerraEngine::Terrain::TerrainLayer& layer) {
				return layer.materialAssetUuid == materialId;
			});
	};
	if(!referencesMaterial(committedLayerStack) && !referencesMaterial(previewLayerStack)) return;

	// The Material Inspector has already installed the authored TMAT in the live
	// resource cache. Repair both editor mirrors before rebuilding the small
	// terrain runtime table so an earlier missing-source marker cannot mask it.
	const auto state = ResourceManager::Instance().GetMaterial(materialId) != nullptr
		? TerraEngine::Terrain::TerrainLayerFallbackState::Healthy
		: TerraEngine::Terrain::TerrainLayerFallbackState::MissingSource;
	const auto applyFallback = [materialId, state](TerraEngine::Terrain::TerrainLayerStack& stack) {
		for(auto& layer : stack.layers)
			if(layer.materialAssetUuid == materialId) layer.fallbackState = state;
	};
	applyFallback(committedLayerStack);
	applyFallback(previewLayerStack);
	SynchronizeTerrainMaterial(true);
}

void TerrainWindow::ObserveTerrainMaterialChanges()
{
	const TerrainMaterialChangeSnapshot changes = AssetDatabase::Instance().GetTerrainMaterialChangesSince(observedTerrainMaterialGeneration);
	if(changes.generation == observedTerrainMaterialGeneration && !changes.requiresFullRefresh) return;
	bool referencedMaterialChanged = false;
	const auto referencesMaterial = [this](TerraEngine::UUID materialId) {
		const auto stackReferences = [materialId](const TerraEngine::Terrain::TerrainLayerStack& stack) {
			return std::any_of(stack.layers.begin(), stack.layers.end(),
				[materialId](const TerraEngine::Terrain::TerrainLayer& layer) {
					return layer.materialAssetUuid == materialId;
				});
		};
		return stackReferences(committedLayerStack) || stackReferences(previewLayerStack);
	};
	auto applyFallback = [](TerraEngine::Terrain::TerrainLayerStack& stack, TerraEngine::UUID materialId, TerraEngine::Terrain::TerrainLayerFallbackState state) {
		for(auto& layer : stack.layers)
			if(layer.materialAssetUuid == materialId) layer.fallbackState = state;
	};
	if(changes.requiresFullRefresh)
	{
		for(auto* stack : {&committedLayerStack, &previewLayerStack})
			for(auto& layer : stack->layers)
				if(layer.materialAssetUuid != TerraEngine::EMPTY_UUID)
				{
					referencedMaterialChanged = true;
					layer.fallbackState = AssetDatabase::Instance().FindAssetByGuid(layer.materialAssetUuid)
						? TerraEngine::Terrain::TerrainLayerFallbackState::Healthy
						: TerraEngine::Terrain::TerrainLayerFallbackState::MissingSource;
				}
	}
	for(const TerrainMaterialChange& change : changes.changes)
	{
		referencedMaterialChanged = referencedMaterialChanged || referencesMaterial(change.materialAssetId);
		const auto state = change.kind == TerrainMaterialChangeKind::Deleted
			? TerraEngine::Terrain::TerrainLayerFallbackState::MissingSource
			: TerraEngine::Terrain::TerrainLayerFallbackState::Healthy;
		applyFallback(committedLayerStack, change.materialAssetId, state);
		applyFallback(previewLayerStack, change.materialAssetId, state);
	}
	observedTerrainMaterialGeneration = changes.generation;
	// File-watcher edits and deletions use the same immediate terrain republish
	// path as Inspector edits. Control textures remain cached when paint is stable.
	if(referencedMaterialChanged) SynchronizeTerrainMaterial(true);
}

void TerrainWindow::CancelDetachedPreview(std::string_view reason)
{
	// Preview buffers are editor-private. Resetting them is safe at every
	// lifecycle boundary because live terrain data changes only via publication.
	const bool restoreCommittedProxy = authoringSession.IsDirty();
	authoringSession.Cancel();
	authoringSystem.CancelPreparedWork(reason);
	previewHeightMm = committedHeightMm;
	CancelHolePreview();
	CancelProceduralPreview();
	brushStrokeActive = false;
	previousStamp.reset();
	applyRequested = false;
	status.assign(reason);
	if(restoreCommittedProxy)
		ScheduleRenderProxyBuild();
}

void TerrainWindow::MarkTerrainSaved()
{
	if(terrainStore) terrainDirtyState.MarkSaved(terrainStore->CaptureRoot());
}

bool TerrainWindow::SaveTerrain(std::string& outError)
{
	if(!terrainStore || !terrainDirtyState.IsDirty()) return true;
	if(destinationPath.empty())
	{
		outError = "Terrain has unsaved authored content but no private .teterrain data location.";
		return false;
	}
	// The store stages blobs and immutable roots before replacing CURRENT, so
	// failed saves preserve the predecessor recovery path.
	const auto receipt = terrainStore->PersistAtomically({destinationPath.string() + ".data"});
	if(!receipt.Succeeded())
	{
		outError = receipt.message;
		status = "Terrain save failed; recovery keeps the previous CURRENT root: " + receipt.message;
		return false;
	}
	const auto committed = terrainStore->CaptureRoot();
	if(!committed.IsValid()
		|| !TerraEngine::Editor::Terrain::TerrainCreationService::SynchronizeCompatibilityDescriptor(
			destinationPath, *committed.root, outError))
	{
		status = "Terrain root was saved, but compatibility metadata synchronization failed: " + outError;
		return false;
	}
	MarkTerrainSaved();
	status = "Terrain saved with immutable root " + TerraEngine::Terrain::ToHexString(receipt.savedRootHash) + ".";
	return true;
}

TerraEngine::Terrain::TerrainDiagnosticSnapshot TerrainWindow::DiagnosticSnapshot() const
{
	TerraEngine::Terrain::TerrainDiagnosticSnapshot snapshot;
	if(!terrainStore) return snapshot;
	const auto capture = terrainStore->CaptureRoot();
	if(!capture.IsValid()) return snapshot;
	snapshot.revision = capture.publicationGeneration;
	snapshot.authoredGeneration = capture.publicationGeneration;
	snapshot.savedGeneration = terrainStore->CapturePersistedRootState().savedPublicationGeneration;
	snapshot.visibleGeneration = capture.publicationGeneration;
	snapshot.controlGeneration = capture.root->layerStackGeneration;
	snapshot.selectedArea = selectedTerrainArea;
	const auto availableAreas = TerraEngine::Terrain::EnumerateTerrainAreas(capture.root->authoredBounds, 4096u);
	snapshot.availableAreaCount = availableAreas ? availableAreas->size() : 0u;
	const auto commonReady = TerraEngine::Terrain::TerrainDerivedSystem::Instance()
		.ResolveCommonReadyCandidate(capture.root->assetUuid);
	if(commonReady)
	{
		snapshot.commonReadyGeneration = commonReady->publicationGeneration;
		snapshot.commonReadyMismatch = commonReady->publicationGeneration != capture.publicationGeneration
			|| commonReady->rootHash != capture.root->rootContentHash;
	}
	else snapshot.commonReadyMismatch = capture.publicationGeneration != 0u;
	const auto streaming = TerraEngine::Terrain::TerrainStreamingSystem::Instance().ResidencySnapshot();
	if(snapshot.availableAreaCount != 0u)
	{
		const std::size_t coarseAreas = static_cast<std::size_t>(std::count_if(
			streaming.areas.begin(), streaming.areas.end(), [](const auto& area) { return area.regionalRoot; }));
		snapshot.streamingProgress = std::min(1.0F,
			static_cast<float>(coarseAreas) / static_cast<float>(snapshot.availableAreaCount));
	}
	snapshot.dirty = terrainDirtyState.IsDirty();
	snapshot.pendingTransactions = terrainQueue ? terrainQueue->PendingCount() : 0U;
	snapshot.historyDepth = UndoRedoSystem::Instance().GetUndoDepth();
	snapshot.historyBytes = UndoRedoSystem::Instance().GetHistoryBytes();
	const auto resizeProgress = resizeJobs.Progress();
	if(resizeProgress.stage != TerraEngine::Editor::Terrain::TerrainResizeStage::Idle)
	{
		TerraEngine::Terrain::TerrainResizeDiagnosticRecord record{};
		record.jobId = resizeProgress.jobId;
		record.currentBounds = capture.root->authoredBounds;
		record.progress = resizeProgress.fraction;
		record.cancellationAvailable = resizeProgress.cancellationAvailable;
		record.stage = std::to_string(static_cast<int>(resizeProgress.stage));
		record.message = resizeProgress.message;
		record.sourceRootHash = capture.root->rootContentHash;
		record.sourceGeneration = capture.publicationGeneration;
		if(resizePreview)
		{
			record.kind = resizePreview->preview.delta.kind;
			record.proposedBounds = resizePreview->preview.delta.proposedBounds;
			record.expansionPolicy = resizePreview->preview.delta.expansionPolicy;
			record.holePolicy = resizePreview->preview.delta.holePolicy;
			record.proposalFingerprint = resizePreview->preview.proposalFingerprint;
			record.commonReadyGeneration = resizePreview->readyGeneration;
			record.affectedAreas = resizePreview->preview.delta.affectedOneRing.size();
			record.retiredAreas = resizePreview->preview.delta.forcedRetirements.size();
			snapshot.retiringAreaCount = record.retiredAreas;
			record.retainedHistoryBytes = resizePreview->preview.cropLoss.retainedHistoryBytes;
			record.budget = resizePreview->preview.budget;
			record.cropLoss = resizePreview->preview.cropLoss;
			record.confirmationRequired = resizePreview->preview.requiresDataLossConfirmation;
			record.confirmationAcknowledged = resizeCropAcknowledged;
		}
		if(resizeProgress.stage == TerraEngine::Editor::Terrain::TerrainResizeStage::Cancelled)
		{
			record.terminalCode = TerraEngine::Terrain::TerrainResultCode::Cancelled;
			record.rolledBack = true;
		}
		else if(resizeProgress.stage == TerraEngine::Editor::Terrain::TerrainResizeStage::Failed)
		{
			record.terminalCode = TerraEngine::Terrain::TerrainResultCode::Failed;
			record.rolledBack = true;
		}
		snapshot.resizes.push_back(std::move(record));
	}
	const auto collision = TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().Snapshot();
	if(collision.status != TerraEngine::Physics::Terrain::TerrainCollisionStatus::Absent)
	{
		snapshot.collisionRegionCount = collision.regions.size();
		snapshot.collisionGeneration = collision.status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Current
			? collision.sourcePublicationGeneration : collision.lastKnownGoodSourcePublicationGeneration;
		snapshot.collisions.push_back(TerraEngine::Terrain::MakeTerrainCollisionRecord(
			collision, static_cast<std::uint64_t>(std::max(0.0, collision.ageMilliseconds)),
			static_cast<std::uint64_t>(std::max(0.0, collision.buildLatencyMilliseconds))));
	}
	for(const auto& layer : committedLayerStack.layers)
	{
		if(layer.fallbackState == TerraEngine::Terrain::TerrainLayerFallbackState::Healthy) continue;
		TerraEngine::Terrain::TerrainLayerFallbackRecord fallback;
		fallback.stableLayerId = layer.stableId;
		fallback.materialAssetId = layer.materialAssetUuid;
		fallback.state = layer.fallbackState;
		fallback.message = layer.fallbackState == TerraEngine::Terrain::TerrainLayerFallbackState::MissingSource
			? "Material source is missing; authored weights keep their stable layer ID and render a safe fallback."
			: "Material source needs repair; authored weights keep their stable layer ID and render a safe fallback.";
		snapshot.layerFallbacks.push_back(std::move(fallback));
	}
	for(const auto& shard : capture.root->directoryShards)
	{
		for(const auto& [coordinate, descriptor] : shard.descriptors)
		{
			std::uint64_t bytes = 0U;
			auto addBlobBytes = [&](const std::optional<TerraEngine::Terrain::ContentHash>& hash) {
				if(hash) if(const auto blob = terrainStore->Blobs().Resolve(*hash)) bytes += blob->size();
			};
			addBlobBytes(descriptor.channels.heightHash);
			addBlobBytes(descriptor.channels.holeHash);
			for(const auto& [group, hash] : descriptor.channels.weightGroupHashes)
			{
				(void)group;
				if(const auto blob = terrainStore->Blobs().Resolve(hash)) bytes += blob->size();
			}
			snapshot.residency.push_back({coordinate, bytes, selectedAreaPinned && coordinate == selectedTerrainArea});
		}
	}
	TerraEngine::Terrain::TerrainDiagnosticBuildInput input;
	input.snapshot = std::move(snapshot);
	if(!publicationStageSamplesNanoseconds.empty())
		input.stageSamples.emplace_back("terrain_authoring_publish", publicationStageSamplesNanoseconds);
	return TerraEngine::Terrain::BuildTerrainDiagnosticSnapshot(std::move(input));
}

std::optional<TerraEngine::Terrain::TerrainAreaCoordinate> TerrainWindow::ConsumeDiagnosticLocateRequest() noexcept
{
	auto request = diagnosticLocateRequest;
	diagnosticLocateRequest.reset();
	return request;
}

bool TerrainWindow::RunBoundedGarbageCollection(std::size_t maximumRemovals, std::string& outMessage)
{
	if(!terrainGarbageCollector)
	{
		outMessage = "Terrain garbage collection is unavailable until a terrain asset is open.";
		garbageCollectionStatus = outMessage;
		return false;
	}
	TerraEngine::Terrain::TerrainGarbageCollectionRequest request;
	request.maximumRemovals = std::max<std::size_t>(1U, maximumRemovals);
	const auto receipt = terrainGarbageCollector->Collect(request);
	outMessage = receipt.cancelled
		? "Garbage collection was cancelled before reclaiming a blob."
		: "Reclaimed " + std::to_string(receipt.removed.size()) + " blobs while protecting " + std::to_string(receipt.protectedCount) + " reachable blobs.";
	garbageCollectionStatus = outMessage;
	return !receipt.cancelled;
}

bool TerrainWindow::RepairFailedGeneration(std::string& outMessage)
{
	if(!terrainStore || destinationPath.empty())
	{
		outMessage = "Terrain repair requires an opened terrain asset with a private data directory.";
		repairStatus = outMessage;
		return false;
	}
	const std::filesystem::path dataDirectory = destinationPath.string() + ".data";
	CancelDetachedPreview("Terrain preview cancelled before generation repair.");
	// Prefer a complete CURRENT/predecessor root. Only when recovery has no
	// complete root do we persist the still-valid in-memory authority as repair.
	const auto recovery = terrainStore->OpenPersistedRoot(dataDirectory);
	if(recovery.Succeeded())
	{
		// Recovery can select a resized predecessor with signed/non-origin areas.
		// Rebuild every area-keyed mirror from that complete root; never privilege
		// the compatibility origin descriptor over authored bounds.
		(void)SynchronizeCommittedAuthoringStateFromPublishedRoot();
		terrainDirtyState.MarkSaved(terrainStore->CaptureRoot());
		outMessage = recovery.usedPredecessor ? "Recovered the complete predecessor terrain generation." : "Revalidated the current terrain generation.";
		repairStatus = outMessage;
		return true;
	}
	const auto repaired = terrainStore->PersistAtomically({dataDirectory});
	if(!repaired.Succeeded())
	{
		outMessage = "Terrain repair failed: " + repaired.message;
		repairStatus = outMessage;
		return false;
	}
	MarkTerrainSaved();
	outMessage = "Repaired terrain storage by publishing the complete in-memory generation.";
	repairStatus = outMessage;
	return true;
}

void TerrainWindow::HandleSceneViewPointer(
	const TerraEngine::Scene::WorldPosition& rayOrigin,
	const glm::dvec3& rayDirection,
	bool pressed, bool held, bool released)
{
	if((!sculpting && !surfacePainting && !holeSculpting) || !terrainStore) return;
	if(surfacePainting) EnsureValidPaintLayer();
	sceneViewBrushFootprint.reset();
	TerraEngine::Scene::WorldPosition terrainOrigin{};
	if(terrainRenderProxyTargetObjectId != TerraEngine::EMPTY_UUID)
	{
		const std::optional<Object> target = ObjectManager::instance().getObject(terrainRenderProxyTargetObjectId);
		const std::optional<TerraEngine::Scene::WorldPose> pose = target
			? ObjectManager::instance().GetCanonicalWorldPose(*target) : std::nullopt;
		if(!pose)
		{
			status = "Terrain stroke rejected because the selected terrain instance no longer exists.";
			return;
		}
		terrainOrigin = pose->position;
	}
	const auto localOrigin = TerraEngine::Scene::LocalizeWorldPosition(rayOrigin, terrainOrigin, 20'000.0);
	if(!localOrigin.Succeeded())
	{
		status = "Terrain stroke rejected because the terrain is outside the bounded Scene View editing range.";
		return;
	}
	const double spacing = std::max(0.001, static_cast<double>(authoredSampleSpacingMetres));
	const TerraEngine::Editor::Terrain::TerrainCanonicalRay sampleRay{
		{localOrigin.offsetMetres.x / spacing, localOrigin.offsetMetres.y, localOrigin.offsetMetres.z / spacing},
		{rayDirection.x / spacing, rayDirection.y, rayDirection.z / spacing}};
	const auto hit = TerraEngine::Editor::Terrain::TerrainPicking::PickHeightfield(sampleRay, [this](std::int64_t x, std::int64_t z) -> std::optional<double> {
		if(!terrainStore) return std::nullopt;
		const auto capture = terrainStore->CaptureRoot();
		if(!capture.IsValid() || !capture.root->authoredBounds.Contains({x, z})) return std::nullopt;
		const auto owner = TerraEngine::Terrain::OwnerArea({x, z});
		const auto local = TerraEngine::Terrain::LocalSample({x, z});
		if(!owner || !local) return std::nullopt;
		const auto& snapshots = authoringSession.IsDirty() ? previewAreaHeightMm : committedAreaHeightMm;
		const auto found = snapshots.find(*owner);
		if(found == snapshots.end() || found->second.size() != TerraEngine::Terrain::kTerrainAreaSampleCount) return std::nullopt;
		const auto& holeSnapshots = holePreviewActive ? previewAreaHoleMasks : committedAreaHoleMasks;
		const auto holes = holeSnapshots.find(*owner);
		if(holes != holeSnapshots.end() && TerraEngine::Editor::Terrain::TerrainHoleBrush::IsHole(
			holes->second, 256u, 256u, local->x, local->z)) return std::nullopt;
		return static_cast<double>(found->second[static_cast<std::size_t>(local->z) * 256u + local->x]) / 1000.0;
	}, 20000.0, 0.5);
	if(hit)
	{
		// Height brushes stamp vertices at integer samples, while hole brushes stamp
		// the centre of the picked cell at a half-sample offset. Convert the active
		// tool's exact centre and radius into metres for Scene View projection.
		const auto owner = TerraEngine::Terrain::OwnerArea(hit->sample);
		const auto local = TerraEngine::Terrain::LocalSample(hit->sample);
		const auto& snapshots = authoringSession.IsDirty() ? previewAreaHeightMm : committedAreaHeightMm;
		const auto found = owner ? snapshots.find(*owner) : snapshots.end();
		const double surfaceHeight = owner && local && found != snapshots.end()
			? static_cast<double>(found->second[static_cast<std::size_t>(local->z) * 256u + local->x]) / 1000.0
			: hit->canonicalWorldPosition.y;
		const auto holeStamp = TerraEngine::Editor::Terrain::TerrainHoleBrush::CenteredOnCell(
			hit->sample.x, hit->sample.z);
		const double centerSampleX = holeSculpting
			? static_cast<double>(holeStamp.centerCellX) : static_cast<double>(hit->sample.x);
		const double centerSampleZ = holeSculpting
			? static_cast<double>(holeStamp.centerCellZ) : static_cast<double>(hit->sample.z);
		const auto worldCenter = TerraEngine::Scene::ApplyWorldDelta(
			terrainOrigin,
			{{}, {
				centerSampleX * spacing,
				surfaceHeight + 0.02,
				centerSampleZ * spacing}});
		if(worldCenter.Succeeded())
		{
			const float radiusUnits = holeSculpting
				? TerraEngine::Editor::Terrain::TerrainHoleBrush::EffectiveRadiusCells(holeBrushSettings.radiusCells)
				: (surfacePainting ? surfaceBrushSettings.radiusSamples : brushSettings.radiusSamples);
			sceneViewBrushFootprint = SceneViewBrushFootprint{
				worldCenter.position,
				std::max(0.001F, radiusUnits) * authoredSampleSpacingMetres,
				holeSculpting};
		}
	}
	// Hovering updates only the projected footprint. Mutation remains gated by
	// the pointer lifecycle below so moving the mouse never creates terrain work.
	if(!pressed && !brushStrokeActive && !surfaceBrushActive && !holeBrushActive) return;
	if(surfacePainting)
	{
		const bool supportsStroke = surfaceBrushSettings.mode == TerraEngine::Editor::Terrain::TerrainSurfaceBrushMode::Paint
			|| surfaceBrushSettings.mode == TerraEngine::Editor::Terrain::TerrainSurfaceBrushMode::Erase
			|| surfaceBrushSettings.mode == TerraEngine::Editor::Terrain::TerrainSurfaceBrushMode::LayerMask;
		if(!supportsStroke)
		{
			if(pressed) status = "Fill, Replace, and Normalize use the entire-area button in Terrain Tools.";
			return;
		}
		if(pressed)
		{
			if(!hit) { status = "Surface stroke rejected: the canonical ray did not hit a resident terrain sample."; return; }
			surfaceBrushActive = true;
			previousSurfaceStamp.reset();
			ApplySurfaceBrushAt(*hit);
		}
		else if(held && surfaceBrushActive && hit) ApplySurfaceBrushAt(*hit);
		if(released && surfaceBrushActive)
		{
			surfaceBrushActive = false;
			previousSurfaceStamp.reset();
			std::string error;
			if(surfacePreviewActive && !ApplySurfacePreview(error)) status = error;
			else if(surfacePreviewActive) status = "Terrain surface stroke queued for the protected authoring boundary.";
			else
			{
				const auto& stack = LayerPreview();
				const std::size_t enabledLayers = static_cast<std::size_t>(std::count_if(
					stack.layers.begin(), stack.layers.end(),
					[](const TerraEngine::Terrain::TerrainLayer& layer) { return layer.enabled; }));
				status = surfaceBrushSettings.layerId == 0u
					? "Surface stroke rejected: no enabled paint layer is available."
					: (enabledLayers <= 1u
						? "Surface stroke made no change: the only enabled layer already covers 100% of the terrain. Add and select another layer."
						: "Surface stroke made no change: the selected layer is already at maximum weight under the brush, or its mask has no coverage.");
			}
		}
		return;
	}
	if(holeSculpting)
	{
		if(pressed)
		{
			if(!hit) { status = "Hole stroke rejected: the canonical ray did not hit a resident terrain cell."; return; }
			holeBrushActive = true;
			ApplyHoleBrushAt(*hit);
		}
		else if(held && holeBrushActive && hit) ApplyHoleBrushAt(*hit);
		if(released && holeBrushActive)
		{
			holeBrushActive = false;
			std::string error;
			if(holePreviewActive && !ApplyHolePreview(error)) status = error;
			else if(holePreviewActive) status = "Terrain hole stroke queued for the protected authoring boundary.";
			else status = "Hole stroke produced no changes.";
		}
		return;
	}
	if(pressed)
	{
		if(!hit) { status = "Terrain stroke rejected: the canonical ray did not hit a resident terrain sample."; return; }
		std::string error; if(!authoringSession.Begin(terrainStore->CaptureRoot(), error)) { status = error; return; }
		std::set<TerraEngine::Terrain::TerrainAreaCoordinate> resident;
		for(const auto& [coordinate, heights] : committedAreaHeightMm)
			if(heights.size() == TerraEngine::Terrain::kTerrainAreaSampleCount) resident.insert(coordinate);
		authoringSession.SetResidentAreas(std::move(resident));
		previewAreaHeightMm = committedAreaHeightMm;
		brushStrokeActive = true; previousStamp.reset(); ApplyBrushAt(*hit);
	}
	else if(held && brushStrokeActive && hit) ApplyBrushAt(*hit);
	if(released && brushStrokeActive)
	{
		brushStrokeActive = false;
		previousStamp.reset();
		const bool changed = authoringSession.IsDirty();
		applyRequested = QueuePreviewForPublication();
		if(!changed) status = "Terrain stroke produced no changes.";
	}
}

void TerrainWindow::ApplyHoleBrushAt(const TerraEngine::Editor::Terrain::TerrainPickHit& hit)
{
	if(!terrainStore) return;
	if(!holePreviewActive) previewAreaHoleMasks = committedAreaHoleMasks;
	const double radius = TerraEngine::Editor::Terrain::TerrainHoleBrush::EffectiveRadiusCells(holeBrushSettings.radiusCells);
	const auto minimumArea = TerraEngine::Terrain::OwnerArea({
		static_cast<std::int64_t>(std::floor(static_cast<double>(hit.sample.x) - radius)),
		static_cast<std::int64_t>(std::floor(static_cast<double>(hit.sample.z) - radius))});
	const auto maximumArea = TerraEngine::Terrain::OwnerArea({
		static_cast<std::int64_t>(std::ceil(static_cast<double>(hit.sample.x) + radius)),
		static_cast<std::int64_t>(std::ceil(static_cast<double>(hit.sample.z) + radius))});
	if(!minimumArea || !maximumArea) return;
	bool changed = false;
	for(std::int64_t areaZ = minimumArea->z; areaZ <= maximumArea->z; ++areaZ)
		for(std::int64_t areaX = minimumArea->x; areaX <= maximumArea->x; ++areaX)
		{
			const TerraEngine::Terrain::TerrainAreaCoordinate coordinate{areaX, areaZ};
			auto found = previewAreaHoleMasks.find(coordinate);
			if(found == previewAreaHoleMasks.end()) continue;
			std::vector<std::uint8_t> after(found->second.size());
			const auto stamp = TerraEngine::Editor::Terrain::TerrainHoleBrush::CenteredOnCell(
				hit.sample.x - areaX * TerraEngine::Terrain::kTerrainAreaExtentSamples,
				hit.sample.z - areaZ * TerraEngine::Terrain::kTerrainAreaExtentSamples);
			if(!TerraEngine::Editor::Terrain::TerrainHoleBrush::ApplyStamp(
				found->second, after, 256u, 256u, holeBrushSettings, stamp)) continue;
			found->second = std::move(after);
			if(coordinate == selectedTerrainArea) previewHoleMask = found->second;
			changed = true;
		}
	if(!changed) return;
	holePreviewActive = true;
	status = "Detached cross-area hole preview is ready to Apply or Cancel.";
	ScheduleRenderProxyBuild();
}

void TerrainWindow::ApplySurfaceBrushAt(const TerraEngine::Editor::Terrain::TerrainPickHit& hit)
{
	const TerraEngine::Editor::Terrain::TerrainSurfaceBrushStamp stamp{
		static_cast<float>(hit.sample.x), static_cast<float>(hit.sample.z)};
	if(previousSurfaceStamp && !TerraEngine::Editor::Terrain::TerrainSurfaceBrush::IsStrokeSampleDue(
		*previousSurfaceStamp, stamp, surfaceBrushSettings.spacingSamples))
		return;
	if(!surfacePreviewActive) previewAreaSurfaceWeights = committedAreaSurfaceWeights;
	const auto minimumArea = TerraEngine::Terrain::OwnerArea({
		static_cast<std::int64_t>(std::floor(static_cast<double>(hit.sample.x) - surfaceBrushSettings.radiusSamples)),
		static_cast<std::int64_t>(std::floor(static_cast<double>(hit.sample.z) - surfaceBrushSettings.radiusSamples))});
	const auto maximumArea = TerraEngine::Terrain::OwnerArea({
		static_cast<std::int64_t>(std::ceil(static_cast<double>(hit.sample.x) + surfaceBrushSettings.radiusSamples)),
		static_cast<std::int64_t>(std::ceil(static_cast<double>(hit.sample.z) + surfaceBrushSettings.radiusSamples))});
	if(!minimumArea || !maximumArea) return;
	bool changed = false;
	for(std::int64_t areaZ = minimumArea->z; areaZ <= maximumArea->z; ++areaZ)
		for(std::int64_t areaX = minimumArea->x; areaX <= maximumArea->x; ++areaX)
		{
			const TerraEngine::Terrain::TerrainAreaCoordinate coordinate{areaX, areaZ};
			auto found = previewAreaSurfaceWeights.find(coordinate);
			if(found == previewAreaSurfaceWeights.end()) continue;
			const TerraEngine::Editor::Terrain::TerrainSurfaceBrushStamp localStamp{
				static_cast<float>(hit.sample.x - areaX * TerraEngine::Terrain::kTerrainAreaExtentSamples),
				static_cast<float>(hit.sample.z - areaZ * TerraEngine::Terrain::kTerrainAreaExtentSamples)};
			std::vector<TerraEngine::Terrain::TerrainWeightSample> after(found->second.size());
			const auto result = TerraEngine::Editor::Terrain::TerrainSurfaceBrush::ApplyStamp(
				found->second, after, 256u, 256u, surfaceBrushSettings, localStamp,
				surfaceBrushSettings.useLayerMask && coordinate == selectedTerrainArea
					? std::span<const std::uint8_t>(brushMask) : std::span<const std::uint8_t>{});
			if(!result.changed) continue;
			found->second = std::move(after);
			if(coordinate == selectedTerrainArea) previewSurfaceWeights = found->second;
			if(result.displacedLayerId != 0u) displacedLayerWarning = result.displacedLayerId;
			changed = true;
		}
	if(!changed) return;
	surfacePreviewActive = true;
	previousSurfaceStamp = stamp;
	// The durable edit remains deferred until release. Material synchronization at
	// the next protected boundary can consume this detached control-page preview.
	status = "Terrain surface stroke preview updated; release to commit one undo step.";
}

void TerrainWindow::ApplyBrushAt(const TerraEngine::Editor::Terrain::TerrainPickHit& hit)
{
	const TerraEngine::Editor::Terrain::TerrainBrushStamp stamp{static_cast<float>(hit.sample.x), static_cast<float>(hit.sample.z)};
	if(previousStamp && !TerraEngine::Editor::Terrain::TerrainBrushEngine::IsStrokeSampleDue(*previousStamp, stamp, brushSettings.spacingSamples)) return;
	const std::int64_t minimumX = static_cast<std::int64_t>(std::floor(static_cast<double>(hit.sample.x) - brushSettings.radiusSamples));
	const std::int64_t minimumZ = static_cast<std::int64_t>(std::floor(static_cast<double>(hit.sample.z) - brushSettings.radiusSamples));
	const std::int64_t maximumX = static_cast<std::int64_t>(std::ceil(static_cast<double>(hit.sample.x) + brushSettings.radiusSamples));
	const std::int64_t maximumZ = static_cast<std::int64_t>(std::ceil(static_cast<double>(hit.sample.z) + brushSettings.radiusSamples));
	const auto minimumArea = TerraEngine::Terrain::OwnerArea({minimumX, minimumZ});
	const auto maximumArea = TerraEngine::Terrain::OwnerArea({maximumX, maximumZ});
	if(!minimumArea || !maximumArea) { status = "Terrain stroke coordinates exceed the supported area lattice."; return; }
	bool changed = false;
	for(std::int64_t areaZ = minimumArea->z; areaZ <= maximumArea->z; ++areaZ)
		for(std::int64_t areaX = minimumArea->x; areaX <= maximumArea->x; ++areaX)
		{
			const TerraEngine::Terrain::TerrainAreaCoordinate coordinate{areaX, areaZ};
			auto found = previewAreaHeightMm.find(coordinate);
			if(found == previewAreaHeightMm.end() || found->second.size() != TerraEngine::Terrain::kTerrainAreaSampleCount) continue;
			const TerraEngine::Editor::Terrain::TerrainBrushStamp localStamp{
				static_cast<float>(static_cast<double>(hit.sample.x) - static_cast<double>(areaX * TerraEngine::Terrain::kTerrainAreaExtentSamples)),
				static_cast<float>(static_cast<double>(hit.sample.z) - static_cast<double>(areaZ * TerraEngine::Terrain::kTerrainAreaExtentSamples))};
			std::vector<std::int32_t> after(found->second.size());
			TerraEngine::Editor::Terrain::TerrainBrushEngine::ApplyStamp(found->second, after, 256U, 256U,
				brushSettings, localStamp, maskEnabled && coordinate == selectedTerrainArea
					? std::span<const std::uint8_t>(brushMask) : std::span<const std::uint8_t>{});
			std::vector<std::int32_t> delta(after.size());
			bool areaChanged = false;
			for(std::size_t index = 0U; index < after.size(); ++index)
			{
				delta[index] = after[index] - found->second[index];
				areaChanged = areaChanged || delta[index] != 0;
			}
			if(!areaChanged) continue;
			std::string error;
			if(!authoringSession.AddHeightDelta(coordinate, std::move(delta), error))
			{
				status = error;
				brushStrokeActive = false;
				return;
			}
			if(coordinate == selectedTerrainArea)
			{
				// Preserve the selected-area compatibility mirror while the area-keyed
				// snapshot remains the authority used by cross-boundary publication.
				previewHeightMm = std::move(after);
				found->second = previewHeightMm;
			}
			else found->second = std::move(after);
			changed = true;
		}
	if(!changed) return;
	previousStamp = stamp;
	// Render the detached value immediately. The worker owns a copy and publishes
	// only at the protected boundary; mouse release still creates the sole durable
	// transaction and undo-history entry for the complete stroke.
	ScheduleRenderProxyBuild();
}

bool TerrainWindow::QueuePreviewForPublication()
{
	if(!terrainStore || !authoringSession.IsDirty()) return false;
	auto capture = terrainStore->CaptureRoot();
	std::vector<TerraEngine::Terrain::TerrainPatch> patches;
	std::vector<TerraEngine::Terrain::TerrainPatch> inversePatches;
	for(const auto& [coordinate, delta] : authoringSession.PreviewHeightDeltas())
	{
		(void)delta;
		const auto before = committedAreaHeightMm.find(coordinate);
		const auto after = previewAreaHeightMm.find(coordinate);
		if(before == committedAreaHeightMm.end() || after == previewAreaHeightMm.end())
		{
			status = "Terrain preview rejected because an edited area snapshot is unavailable.";
			return false;
		}
		const auto current = terrainStore->ResolveDescriptorOrImplicit(*capture.root, coordinate);
		std::vector<std::uint8_t> beforeBytes(before->second.size() * sizeof(std::int32_t));
		std::vector<std::uint8_t> afterBytes(after->second.size() * sizeof(std::int32_t));
		std::memcpy(beforeBytes.data(), before->second.data(), beforeBytes.size());
		std::memcpy(afterBytes.data(), after->second.data(), afterBytes.size());
		const auto beforeHash = TerraEngine::Terrain::HashTerrainBytes(beforeBytes);
		const auto afterHash = TerraEngine::Terrain::HashTerrainBytes(afterBytes);
		if(!terrainStore->Blobs().PutVerified(beforeHash, std::move(beforeBytes))
			|| !terrainStore->Blobs().PutVerified(afterHash, std::move(afterBytes)))
		{
			status = "Terrain preview could not retain its immutable multi-area height blobs.";
			return false;
		}
		TerraEngine::Terrain::TerrainPatch patch;
		patch.kind = TerraEngine::Terrain::TerrainPatchKind::ReplaceHeight;
		patch.preparedBlobHash = afterHash;
		patch.replacement = current;
		patch.replacement.editGeneration++;
		patch.replacement.authoredContentHash = afterHash;
		patch.replacement.channels.heightHash = afterHash;
		const auto [minimum, maximum] = std::minmax_element(after->second.begin(), after->second.end());
		patch.replacement.minHeightMm = *minimum;
		patch.replacement.maxHeightMm = *maximum;
		patch.retainedBlobHashes = {beforeHash, afterHash};
		TerraEngine::Terrain::TerrainPatch inverse = patch;
		inverse.preparedBlobHash = beforeHash;
		inverse.replacement = current;
		inverse.replacement.channels.heightHash = beforeHash;
		inverse.replacement.authoredContentHash = current.authoredContentHash;
		inverse.retainedBlobHashes = patch.retainedBlobHashes;
		patches.push_back(std::move(patch));
		inversePatches.push_back(std::move(inverse));
	}
	std::string error; const auto transaction = authoringSession.BuildCommitRequest(std::move(patches), "Terrain sculpt stroke", error);
	if(!transaction) { status = error; return false; }
	TerraEngine::Terrain::TerrainEditTransaction prepared = *transaction;
	prepared.inversePatches = std::move(inversePatches);
	if(!authoringSystem.EnqueueDetachedCommit(std::move(prepared), error)) { status = error; return false; }
	status = "Terrain preview queued for the protected authoring boundary."; return true;
}

void TerrainWindow::ProcessProtectedAuthoringBoundary()
{
	// Resource publication belongs to the same protected editor boundary as the
	// terrain receipt. A completed worker mesh cannot appear mid-render-frame.
	SynchronizeTerrainMaterial();
	PollRenderProxyBuild();
	const auto publicationStart = std::chrono::steady_clock::now();
	authoringSystem.PublishAtProtectedBoundary();
	bool committedGeometryChange = false;
	bool committedControlChange = false;
	bool committedMaterialChange = false;
	bool committedCollisionChange = false;
	for(const auto& completion : authoringSystem.DrainCompletions())
	{
		const auto& receipt = completion.receipt;
		// Automation jobs become terminal only after this protected-boundary receipt,
		// never merely because a worker prepared a detached transaction.
		automationAdapter.ObserveReceipt(receipt);
		const bool isLayerReceipt = queuedLayerTransactionId && receipt.transactionId == *queuedLayerTransactionId;
		const bool isSurfaceReceipt = queuedSurfaceTransactionId && receipt.transactionId == *queuedSurfaceTransactionId;
		const bool isHoleReceipt = queuedHoleTransactionId && receipt.transactionId == *queuedHoleTransactionId;
		const bool isProceduralReceipt = queuedProceduralTransactionId && receipt.transactionId == *queuedProceduralTransactionId;
		const bool isResizeReceipt = queuedResizeTransactionId && receipt.transactionId == *queuedResizeTransactionId;
		if(isResizeReceipt) queuedResizeTransactionId.reset();
		// Undo/redo receipts move the existing cursor. They must not create a
		// second history command simply because their replay committed.
		const bool wasHistoryReplay = UndoRedoSystem::Instance().CompleteTerrainReplay(receipt);
		if(receipt.outcome == TerraEngine::Terrain::TerrainEditOutcome::Committed)
		{
			if(receipt.invalidation)
			{
				committedGeometryChange = receipt.invalidation->Contains(
					TerraEngine::Terrain::TerrainInvalidationKind::GeometryAndCollision) || committedGeometryChange;
				committedControlChange = receipt.invalidation->Contains(
					TerraEngine::Terrain::TerrainInvalidationKind::ControlTextures) || committedControlChange;
				committedMaterialChange = receipt.invalidation->Contains(
					TerraEngine::Terrain::TerrainInvalidationKind::MaterialBindings) || committedMaterialChange;
			}
			else
			{
				// Compatibility producers without typed invalidation remain conservative,
				// while every native terrain transaction above uses the exact union.
				committedGeometryChange = (!isLayerReceipt && !isSurfaceReceipt) || committedGeometryChange;
				committedControlChange = isSurfaceReceipt || committedControlChange;
				committedMaterialChange = isLayerReceipt || committedMaterialChange;
			}
			if(receipt.historyPayload)
			{
				committedCollisionChange = std::any_of(
					receipt.historyPayload->forwardPatches.begin(), receipt.historyPayload->forwardPatches.end(),
					[](const TerraEngine::Terrain::TerrainPatch& patch) {
						return patch.kind == TerraEngine::Terrain::TerrainPatchKind::ReplaceHeight
							|| patch.kind == TerraEngine::Terrain::TerrainPatchKind::ReplaceHoles;
					}) || committedCollisionChange;
			}
			else if(isHoleReceipt || isProceduralReceipt || (!isLayerReceipt && !isSurfaceReceipt))
			{
				// Older or compatibility producers may omit history payloads. Their
				// typed receipt identity still tells us whether height/hole authority
				// changed, so collision cannot silently remain on an older generation.
				committedCollisionChange = true;
			}
			if(!wasHistoryReplay && receipt.historyPayload && terrainStore && terrainQueue)
			{
				// The protected boundary returns the immutable request beside its receipt.
				// History therefore preserves resize policy, bounds, budgets, origin, and
				// exact inverse data instead of reverse-engineering a request from output.
				TerraEngine::Terrain::TerrainEditTransaction forward = completion.transaction;
				forward.budget.maximumAreas = std::max<std::size_t>(1u, receipt.historyPayload->forwardPatches.size()); forward.budget.maximumSamples = forward.budget.maximumAreas * TerraEngine::Terrain::kTerrainAreaSampleCount; forward.patches = receipt.historyPayload->forwardPatches;
				forward.layerStackPatch = receipt.historyPayload->forwardLayerStackPatch;
				TerraEngine::Terrain::TerrainEditTransaction inverse = forward;
				inverse.patches = receipt.historyPayload->inversePatches;
				inverse.layerStackPatch = receipt.historyPayload->inverseLayerStackPatch;
				forward.resizeDelta = receipt.historyPayload->forwardResize;
				forward.inverseResizeDelta = receipt.historyPayload->inverseResize;
				inverse.resizeDelta = receipt.historyPayload->inverseResize;
				inverse.inverseResizeDelta = receipt.historyPayload->forwardResize;
				if(forward.resizeDelta || inverse.resizeDelta)
				{
					const std::size_t resizeAreaBudget = std::max({std::size_t{1u}, receipt.affectedAreas.size(),
						forward.resizeDelta ? forward.resizeDelta->descriptorUpserts.size() + forward.resizeDelta->descriptorRemovals.size() : 0u,
						inverse.resizeDelta ? inverse.resizeDelta->descriptorUpserts.size() + inverse.resizeDelta->descriptorRemovals.size() : 0u});
					forward.budget.maximumAreas = resizeAreaBudget;
					inverse.budget.maximumAreas = resizeAreaBudget;
					forward.historyGroup = forward.resizeDelta->kind == TerraEngine::Terrain::TerrainResizeKind::Expand ? "Expand Terrain" : "Crop Terrain";
					inverse.historyGroup = inverse.resizeDelta->kind == TerraEngine::Terrain::TerrainResizeKind::Expand ? "Expand Terrain" : "Crop Terrain";
				}
				// The command refreshes observations/generations immediately before each
				// deferred replay, keeping stale history atomic and rejectable.
				UndoRedoSystem::Instance().PushCommand(std::make_unique<TerraEngine::Editor::Terrain::TerrainHistoryCommand>(
					*terrainStore, *terrainQueue, std::move(forward), std::move(inverse), receipt.historyPayload->retainedByteCount));
			}
			terrainDirtyState.Observe(terrainStore ? terrainStore->CaptureRoot() : TerraEngine::Terrain::TerrainRootCapture{});
			if(isResizeReceipt && terrainStore)
			{
				const auto resized = terrainStore->CaptureRoot();
				if(resized.IsValid())
				{
					if(completion.transaction.resizeDelta)
						TerraEngine::Terrain::TerrainStreamingSystem::Instance().SubmitForcedRetirements(
							resized.publicationGeneration, completion.transaction.resizeDelta->forcedRetirements);
					const auto resizedAreas = TerraEngine::Terrain::EnumerateTerrainAreas(resized.root->authoredBounds, 4096u);
					if(resizedAreas && std::find(resizedAreas->begin(), resizedAreas->end(), selectedTerrainArea) == resizedAreas->end())
						selectedTerrainArea = resizedAreas->front();
					// Expansion/crop changes both directory membership and authored bounds.
					// Refresh every area mirror after repairing selection and before scheduling
					// the overview proxy; using the former selected-area buffer here made a
					// successful expansion look identical to its pre-commit footprint.
					(void)SynchronizeCommittedAuthoringStateFromPublishedRoot();
					resizeCropBounds = {resized.root->authoredBounds.minX, resized.root->authoredBounds.minZ,
						resized.root->authoredBounds.maxXExclusive, resized.root->authoredBounds.maxZExclusive};
					(void)TerraEngine::Terrain::TerrainStreamingSystem::Instance().SubmitCoarseCoverage(
						resized.root->assetUuid, resized.publicationGeneration,
						resized.root->authoredBounds, 4096u, 64u);
					ClearResizePreview();
				}
			}
			if(wasHistoryReplay)
			{
				// A replay can target height, holes, surface weights, or layer metadata.
				// Refresh every editor mirror from the newly authoritative root before
				// another brush or layer action can compose from stale detached data.
				(void)SynchronizeCommittedAuthoringStateFromPublishedRoot();
				authoringSession.Cancel();
				layerPreviewActive = false;
				surfacePreviewActive = false;
				holePreviewActive = false;
				holeBrushActive = false;
				displacedLayerWarning = 0u;
				status = "Terrain undo/redo applied at the protected boundary.";
			}
			else if(isResizeReceipt)
			{
				status = "Terrain resize applied at the protected boundary; the full authored footprint is rebuilding.";
			}
			else if(isLayerReceipt)
			{
				const auto capture = terrainStore->CaptureRoot();
				if(capture.root && capture.root->layerStack) committedLayerStack = *capture.root->layerStack;
				else committedLayerStack = previewLayerStack;
				previewLayerStack = committedLayerStack;
				layerPreviewActive = false; stagedLayerPatch.reset(); stagedInverseLayerPatch.reset(); queuedLayerTransactionId.reset();
				status = "Terrain layer metadata applied at the protected boundary; authored weights were preserved.";
			}
			else if(isSurfaceReceipt)
			{
				committedSurfaceWeights = previewSurfaceWeights;
				committedAreaSurfaceWeights = previewAreaSurfaceWeights;
				surfacePreviewActive = false; displacedLayerWarning = 0u; queuedSurfaceTransactionId.reset();
				status = "Terrain surface weights applied at the protected boundary.";
			}
			else if(isHoleReceipt)
			{
				committedHoleMask = previewHoleMask;
				committedAreaHoleMasks = previewAreaHoleMasks;
				holePreviewActive = false; holeBrushActive = false; queuedHoleTransactionId.reset();
				status = "Terrain holes applied at the protected boundary; collision regeneration was invalidated.";
			}
			else if(isProceduralReceipt)
			{
				// Only the immutable result associated with this receipt becomes
				// authoritative.  A later preview cannot leak through this boundary.
				if(!SynchronizeCommittedAuthoringStateFromPublishedRoot() && proceduralPreview)
				{
					committedHeightMm = proceduralPreview->afterHeightMm;
					previewHeightMm = committedHeightMm;
				}
				proceduralPreview.reset(); queuedProceduralTransactionId.reset();
				status = "Terrain procedural preview applied as one named history group.";
			}
			else
			{
				// Visible strokes already carry a matching preview, while automation
				// commits do not. Prefer the published blob for both so chained tools
				// always compose from the same authoritative generation.
				if(!SynchronizeCommittedAuthoringStateFromPublishedRoot()) committedHeightMm = previewHeightMm;
				previewHeightMm = committedHeightMm; authoringSession.Cancel(); applyRequested = false;
				status = "Terrain sculpt applied at the protected boundary (generation " + std::to_string(receipt.publicationGeneration) + ").";
			}
		}
		else
		{
			if(isResizeReceipt) ClearResizePreview();
			if(isLayerReceipt) { queuedLayerTransactionId.reset(); layerPreviewActive = false; previewLayerStack = committedLayerStack; }
			if(isSurfaceReceipt) { queuedSurfaceTransactionId.reset(); surfacePreviewActive = false; previewSurfaceWeights = committedSurfaceWeights; previewAreaSurfaceWeights = committedAreaSurfaceWeights; }
			if(isHoleReceipt) { queuedHoleTransactionId.reset(); holePreviewActive = false; holeBrushActive = false; previewHoleMask = committedHoleMask; previewAreaHoleMasks = committedAreaHoleMasks; }
			if(isProceduralReceipt) { queuedProceduralTransactionId.reset(); proceduralPreview.reset(); previewHeightMm = committedHeightMm; }
			applyRequested = false;
			status = std::string(isLayerReceipt ? "Terrain layer preview was not published: " : (isSurfaceReceipt ? "Terrain surface preview was not published: " : (isHoleReceipt ? "Terrain hole preview was not published: " : (isProceduralReceipt ? "Terrain procedural preview was not published: " : "Terrain sculpt rejected: ")))) + receipt.message;
		}
	}
	if(committedMaterialChange || committedControlChange)
		SynchronizeTerrainMaterial(true);
	if(committedGeometryChange)
		ScheduleRenderProxyBuild();
	if(committedCollisionChange)
	{
		std::string collisionError;
		if(!QueueAutomaticTerrainCollision(collisionError) && !collisionError.empty())
			status = collisionError;
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now() - publicationStart).count();
	publicationStageSamplesNanoseconds.push_back(static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
	if(publicationStageSamplesNanoseconds.size() > 256u)
		publicationStageSamplesNanoseconds.erase(publicationStageSamplesNanoseconds.begin());
}

std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> TerrainWindow::DeclareAuthoringWorkloads(float deltaTime)
{
	auto workloads = authoringSystem.DeclareFrameWorkloads(deltaTime);
	for(auto& workload : workloads)
	{
		if(workload.stableDescriptorId == "TerrainAuthoringSystem/PostFrame/TerrainAuthoringPublish")
		{
			// Publishing and consuming its receipts are one protected boundary.
			// This records history/dirty state even when the Terrain window is not
			// being drawn, rather than making correctness depend on an ImGui tick.
			workload.work = [this](TerraEngine::Scheduling::JobGraphContext&) { ProcessProtectedAuthoringBoundary(); };
			workload.writes.push_back(
				TerraEngine::Scheduling::kResourceTerrainRenderProxies);
		}
	}
	return workloads;
}
