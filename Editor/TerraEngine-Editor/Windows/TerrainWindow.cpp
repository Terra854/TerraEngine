// TerrainWindow.cpp -- Conservative terrain authoring controls.

#include "TerrainWindow.h"

#include "../Terrain/TerrainCreationService.h"
#include "../Terrain/TerrainHistoryCommand.h"
#include "../Subsystems/UndoRedoSystem.h"

#include "Terrain/TerrainAreaCodec.h"
#include "AssetDatabase.h"
#include "Components/MeshRenderComponent.h"
#include "Components/TerrainComponent.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/Terrain/TerrainRenderProxyRegistry.h"
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

TerrainWindow::TerrainWindow() : EditorWindow("Terrain Tools", false, "Tools")
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
	// The collision scheduler may outlive editor windows during shutdown. Remove
	// only this window's store pointer before its immutable storage is destroyed.
	if(terrainStore)
		TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().ClearPublication(terrainStore.get());
}

void TerrainWindow::ScheduleRenderProxyBuild()
{
	const bool useDetachedPreview = authoringSession.IsDirty() || holePreviewActive;
	const std::vector<std::int32_t>& sourceHeights = useDetachedPreview
		? previewHeightMm : committedHeightMm;
	const std::vector<std::uint8_t>& sourceHoles = useDetachedPreview && holePreviewActive
		? previewHoleMask : committedHoleMask;
	if(!terrainStore || sourceHeights.size() != TerraEngine::Terrain::kTerrainAreaSampleCount)
		return;
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
	const auto descriptor = terrainStore->FindDescriptor(*capture.root, {0, 0});
	if(!descriptor) return;
	const auto cancellation = std::make_shared<std::atomic_bool>(false);
	auto heights = sourceHeights;
	auto holes = sourceHoles;
	const TerraEngine::UUID assetId = capture.root->assetUuid;
	auto authoredHash = descriptor->authoredContentHash;
	std::uint64_t generation = descriptor->editGeneration;
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
		generation = descriptor->editGeneration == std::numeric_limits<std::uint64_t>::max()
			? descriptor->editGeneration : descriptor->editGeneration + 1u;
	}
	const float spacing = authoredSampleSpacingMetres;
	auto work = [assetId, authoredHash, generation, spacing, heights = std::move(heights), holes = std::move(holes), cancellation]() mutable
	{
		TerraEngine::Renderer::Terrain::TerrainDirectMeshInput input{};
		input.assetId = assetId;
		input.authoredHash = authoredHash;
		input.authoredGeneration = generation;
		input.sampleSpacingMetres = spacing;
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
	InitializeAuthoringTerrain(opened.assetId, opened.heightfield, objectId);
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
	const auto descriptor = capture.IsValid() ? terrainStore->FindDescriptor(*capture.root, {0, 0}) : std::nullopt;
	const bool currentAsset = descriptor && completed.assetId == capture.root->assetUuid;
	const bool current = completedWasPreview
		? currentAsset && (authoringSession.IsDirty() || holePreviewActive)
		: currentAsset && completed.authoredHash == descriptor->authoredContentHash
			&& completed.authoredGeneration == descriptor->editGeneration;
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
	if(terrainRenderProxyMaterialId == TerraEngine::EMPTY_UUID)
	{
		auto material = std::make_unique<Material>();
		std::memcpy(material->header.magic, "TMAT", 4u);
		material->header.version = kMaterialCurrentVersion;
		material->header.headerSize = static_cast<std::uint16_t>(sizeof(MaterialHeader));
		material->header.shadingModel = static_cast<std::uint32_t>(ShadingModel::Unlit);
		material->header.alphaMode = static_cast<std::uint32_t>(AlphaMode::Opaque);
		// Terrain is normally viewed from above, but an editor authoring proxy must
		// remain visible while orbiting below it or while a flat tile has no vertical
		// extent. The production terrain surface path can restore one-sided culling.
		material->header.flags = DoubleSided;
		material->header.shaderCollectionId = resources.GetDefaultShaderCollectionID();
		material->header.baseColorFactor[0] = 0.18F;
		material->header.baseColorFactor[1] = 0.55F;
		material->header.baseColorFactor[2] = 0.16F;
		material->header.baseColorFactor[3] = 1.0F;
		material->header.roughnessFactor = 0.92F;
		material->header.normalScale = 1.0F;
		material->header.occlusionStrength = 1.0F;
		material->shaderCollectionId = material->header.shaderCollectionId;
		material->shadowBehavior = DeriveMaterialShadowBehavior(
			material->header.alphaMode, material->header.emissiveFactor);
		terrainRenderProxyMaterialId = resources.RegisterMaterial("terrain-direct-fallback", std::move(material));
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
			if(capability == "terrain.create") request.sourceFormat = TerraEngine::AssetPipeline::TerrainSourceFormat::Flat;
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
			return {true, {}, result.message, std::nullopt,
				{{"assetUuid", TerraEngine::ToHexString(request.assetId)},
				 {"rootHash", TerraEngine::Terrain::ToHexString(capture.root->rootContentHash)},
				 {"publicationGeneration", capture.publicationGeneration}}};
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
		if(!terrainStore || committedHeightMm.size() != TerraEngine::Terrain::kTerrainAreaSampleCount)
			return {false, "Unavailable", "Collision refresh requires one resident canonical terrain area."};
		const auto capture = terrainStore->CaptureRoot();
		if(!capture.IsValid()) return {false, "Unavailable", "Collision refresh requires a current immutable root."};
		const auto started = std::chrono::steady_clock::now();
		TerraEngine::Physics::TerrainHeightFieldRecipe recipe;
		recipe.terrainAssetId = capture.root->assetUuid;
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
		auto candidate = TerraEngine::Physics::Terrain::PrepareTerrainCollisionCandidate(
			std::move(recipe), capture.root->rootContentHash, capture.publicationGeneration);
		candidate.buildLatencyMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();
		if(!candidate.complete) return {false, "InvalidSchema", "The generated terrain collision recipe failed provider-neutral validation."};
		TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().SetPreparedCandidate(std::move(candidate));
		return {true, "Pending", "Terrain collision candidate queued for the protected before-physics boundary.", std::nullopt,
			{{"sourceRootHash", TerraEngine::Terrain::ToHexString(capture.root->rootContentHash)},
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

void TerrainWindow::OnGui()
{
	ObserveTerrainMaterialChanges();
	DrawCreateImportControls();
	ImGui::Separator();
	DrawSculptControls();
	ImGui::Separator();
	DrawSurfaceControls();
	ImGui::Separator();
	DrawProceduralControls();
	ImGui::Separator();
	DrawDiagnosticControls();
}

void TerrainWindow::DrawDiagnosticControls()
{
	ImGui::TextUnformatted("Terrain diagnostics");
	ImGui::InputText("Filter diagnostics", diagnosticFilter, IM_ARRAYSIZE(diagnosticFilter));
	const auto diagnostics=DiagnosticSnapshot();
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
	const auto descriptor = terrainStore->FindDescriptor(*capture.root, {0, 0});
	if(!descriptor || committedHeightMm.size() != TerraEngine::Terrain::kTerrainAreaSampleCount)
	{
		outError = "Procedural preview requires one resident canonical terrain area.";
		return false;
	}
	TerraEngine::Editor::Terrain::TerrainProceduralJobRequest request;
	request.observedRoot = capture; request.observedArea = *descriptor; request.heightMm = committedHeightMm;
	request.selectionGrid.storageOrigin = {0, 0}; request.selectionGrid.storageWidth = 256u; request.selectionGrid.storageHeight = 256u;
	request.selectionGrid.evaluationBounds = {0, 0, 256, 256};
	request.selectionCriteria = proceduralSelection;
	if(proceduralMinimumHeightMm != std::numeric_limits<std::int32_t>::min())
		request.selectionCriteria.minimumHeightMm = proceduralMinimumHeightMm;
	request.selectionCriteria.maximumEvaluatedSamples = TerraEngine::Terrain::kTerrainAreaSampleCount;
	request.historyGroup = "Terrain procedural / spline operation";
	const std::int32_t delta = proceduralHeightDeltaMm;
	const int operation = proceduralOperation;
	const auto descriptorCopy = *descriptor;
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
	const auto capture=terrainStore->CaptureRoot(); const auto descriptor=terrainStore->FindDescriptor(*capture.root,{0,0});
	if(!descriptor || committedHeightMm.size()!=TerraEngine::Terrain::kTerrainAreaSampleCount) { outError="A resident canonical terrain area is required."; return std::nullopt; }
	TerraEngine::Editor::Terrain::TerrainProceduralPreview preview; preview.observedRoot=capture; preview.observedArea=*descriptor; preview.beforeHeightMm=committedHeightMm; preview.afterHeightMm=committedHeightMm; preview.historyGroup="Terrain automation height remap";
	TerraEngine::Editor::Terrain::TerrainProceduralArea area; area.coordinate=descriptor->coordinate; area.observedGeneration=descriptor->editGeneration; area.sourceHash=descriptor->authoredContentHash; area.heightMm=committedHeightMm;
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
	const auto created = ExecuteAutomationCommand("terrain.create", {{"destination", destination.string()}, {"sampleSpacingMetres", 1.0F}});
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
	TerraEngine::UUID existingObjectId)
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
	terrainRenderProxyMeshId = TerraEngine::EMPTY_UUID;
	terrainRenderProxyTargetObjectId = existingObjectId;
	if(terrainStore)
		TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().ClearPublication(terrainStore.get());
	// Keep a small, editor-owned in-memory authority for the visible workflow.
	// Runtime persistence remains the creation service's immutable private bundle.
	committedHeightMm.assign(256U * 256U, heightfield.heightsMm.empty() ? 0 : heightfield.heightsMm.front());
	for(std::uint32_t z = 0U; z < 256U; ++z) for(std::uint32_t x = 0U; x < 256U; ++x)
		committedHeightMm[static_cast<std::size_t>(z) * 256U + x] = heightfield.heightsMm[std::min<std::size_t>(heightfield.heightsMm.size() - 1U, static_cast<std::size_t>(std::min(z, heightfield.height - 1U)) * heightfield.width + std::min(x, heightfield.width - 1U))];
	previewHeightMm = committedHeightMm; brushMask.assign(256U * 256U, 255U);
	committedHoleMask.assign(TerraEngine::Terrain::kTerrainAreaSampleCount / 8u, 0u);
	previewHoleMask = committedHoleMask;
	std::vector<std::uint8_t> bytes(committedHeightMm.size() * sizeof(std::int32_t)); std::memcpy(bytes.data(), committedHeightMm.data(), bytes.size());
	const auto heightHash = TerraEngine::Terrain::HashTerrainBytes(bytes);
	auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
	root->assetUuid = assetId; root->publicationGeneration = 1U; root->persistedRootGeneration = 1U; root->rootContentHash = heightHash;
	root->authoredBounds = {0, 0, 256, 256}; root->layerStackGeneration = 1U;
	TerraEngine::Terrain::TerrainDirectoryShard shard; shard.areaBounds = root->authoredBounds; shard.identity = heightHash;
	TerraEngine::Terrain::TerrainAreaDescriptor descriptor; descriptor.coordinate = {0, 0}; descriptor.editGeneration = 1U; descriptor.authoredContentHash = heightHash; descriptor.channels.heightHash = heightHash;
	shard.descriptors.emplace(descriptor.coordinate, descriptor); root->directoryShards.push_back(std::move(shard));
	// Put initial layer metadata in the immutable root before the store captures
	// it. Later list edits therefore use the same CAS/persistence path as paint.
	auto initialLayerStack = std::make_shared<TerraEngine::Terrain::TerrainLayerStack>();
	initialLayerStack->generation = 1u;
	initialLayerStack->layers.push_back({1u, "Base Layer", TerraEngine::EMPTY_UUID, {}, true, TerraEngine::Terrain::TerrainLayerFallbackState::Healthy});
	root->layerStack = std::move(initialLayerStack);
	terrainStore = std::make_unique<TerraEngine::Terrain::TerrainAssetStore>(root); terrainStore->Blobs().PutVerified(heightHash, std::move(bytes));
	if(!destinationPath.empty() && std::filesystem::exists(destinationPath.string() + ".data/CURRENT"))
	{
		// New-format terrain bundles rehydrate their saved/predecessor roots before
		// history begins. Legacy creation bundles deliberately fall back to the
		// just-created working root until their loader/migration path is available.
		const auto opened = terrainStore->OpenPersistedRoot(destinationPath.string() + ".data");
		if(opened.Succeeded())
		{
			const auto persisted = terrainStore->CaptureRoot();
			const auto persistedArea = terrainStore->FindDescriptor(*persisted.root, {0, 0});
			if(persistedArea && persistedArea->channels.heightHash)
			{
				if(const auto persistedBytes = terrainStore->Blobs().Resolve(*persistedArea->channels.heightHash);
					persistedBytes && persistedBytes->size() == committedHeightMm.size() * sizeof(std::int32_t))
				{
					std::memcpy(committedHeightMm.data(), persistedBytes->data(), persistedBytes->size());
					previewHeightMm = committedHeightMm;
				}
			}
			if(persistedArea && persistedArea->channels.holeHash)
			{
				if(const auto persistedHoles = terrainStore->Blobs().Resolve(*persistedArea->channels.holeHash);
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
	authoringSystem.Shutdown(); authoringSystem.Configure(*terrainQueue, *terrainPublication); authoringSystem.Initialize();
	// Automation receives this narrow adapter rather than direct asset access.
	// It can preflight and enqueue only the same typed transactions as the UI.
	automationAdapter.Configure(*terrainStore, authoringSystem);
	TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().ConfigurePublication(
		terrainStore.get(), [](TerraEngine::Physics::TerrainCollisionReplacementPacket packet) {
			return TerraEngine::Physics::PhysicsSystem::Instance().CommitTerrainCollisionReplacement(std::move(packet));
		});
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
	selectedLayerId = 1u;
	layerPreviewActive = false;
	surfacePreviewActive = false;
	displacedLayerWarning = 0u;
	observedTerrainMaterialGeneration = AssetDatabase::Instance().GetTerrainMaterialChangeGeneration();
	previousStamp.reset(); authoringSession.Cancel(); status = "Terrain Tools is ready: drag in Scene View to create a detached sculpt preview.";
	ScheduleRenderProxyBuild();
}

bool TerrainWindow::SynchronizeCommittedHeightFromPublishedRoot()
{
	// The immutable asset store is the authority after every protected publish.
	// Automation prepares a detached blob without mutating the window's preview
	// vector, so copying previewHeightMm here would make the next operation start
	// from stale samples. Resolve the just-published channel and mirror its exact
	// canonical int32 payload into both editor buffers instead.
	if(!terrainStore) return false;
	const auto capture = terrainStore->CaptureRoot();
	if(!capture.IsValid()) return false;
	const auto descriptor = terrainStore->FindDescriptor(*capture.root, {0, 0});
	if(!descriptor || !descriptor->channels.heightHash) return false;
	const auto bytes = terrainStore->Blobs().Resolve(*descriptor->channels.heightHash);
	const std::size_t expectedBytes = TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t);
	if(!bytes || bytes->size() != expectedBytes) return false;
	committedHeightMm.resize(TerraEngine::Terrain::kTerrainAreaSampleCount);
	std::memcpy(committedHeightMm.data(), bytes->data(), expectedBytes);
	previewHeightMm = committedHeightMm;
	return true;
}

bool TerrainWindow::StageLayerPatch(const TerraEngine::Terrain::TerrainLayerPatch& sourcePatch, std::string& outError)
{
	if(layerPreviewActive || queuedLayerTransactionId.has_value())
	{
		outError = "Apply or Cancel the current detached layer operation before staging another one.";
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
	TerraEngine::Terrain::TerrainEditTransaction transaction;
	transaction.transactionId = TerraEngine::GenerateUUID();
	transaction.origin = TerraEngine::Terrain::TerrainEditOrigin::VisibleEditor;
	transaction.targetAssetUuid = capture.root->assetUuid;
	transaction.historyGroup = "Terrain layer metadata";
	transaction.inputRootHash = capture.root->rootContentHash;
	transaction.expectedLayerStackGeneration = capture.root->layerStackGeneration;
	transaction.affectedBounds = capture.root->authoredBounds;
	transaction.budget.maximumAreas = 1u;
	transaction.layerStackPatch = *stagedLayerPatch;
	transaction.inverseLayerStackPatch = *stagedInverseLayerPatch;
	const TerraEngine::UUID transactionId = transaction.transactionId;
	if(!authoringSystem.EnqueueDetachedCommit(std::move(transaction), outError)) return false;
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
	if(!surfacePreviewActive) previewSurfaceWeights = committedSurfaceWeights;
	surfaceBrushSettings = settings;
	const auto result = TerraEngine::Editor::Terrain::TerrainSurfaceBrush::ApplyArea(previewSurfaceWeights, settings, mask);
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
	surfacePreviewActive = false;
	displacedLayerWarning = 0u;
}

bool TerrainWindow::PreviewHoleStamp(const TerraEngine::Editor::Terrain::TerrainHoleBrushStamp& stamp)
{
	if(!terrainStore) return false;
	if(!holePreviewActive) previewHoleMask = committedHoleMask;
	std::vector<std::uint8_t> after(previewHoleMask.size());
	const bool changed = TerraEngine::Editor::Terrain::TerrainHoleBrush::ApplyStamp(
		previewHoleMask, after, 256u, 256u, holeBrushSettings, stamp);
	if(changed)
	{
		previewHoleMask = std::move(after);
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
	if(!terrainStore || previewHoleMask.size() != TerraEngine::Terrain::kTerrainAreaSampleCount / 8u || committedHoleMask.size() != previewHoleMask.size())
	{
		outError = "Hole painting requires one complete canonical 256 by 256 cell mask.";
		return false;
	}
	const auto afterHash = TerraEngine::Terrain::HashTerrainBytes(previewHoleMask);
	const auto beforeHash = TerraEngine::Terrain::HashTerrainBytes(committedHoleMask);
	if(!terrainStore->Blobs().PutVerified(afterHash, previewHoleMask) || !terrainStore->Blobs().PutVerified(beforeHash, committedHoleMask))
	{
		outError = "Hole preview could not retain its immutable before/after masks.";
		return false;
	}
	const auto capture = terrainStore->CaptureRoot();
	const auto current = terrainStore->FindDescriptor(*capture.root, {0, 0});
	if(!current) { outError = "Hole preview cannot publish because the selected area is unavailable."; return false; }
	auto makeSpan = [](TerraEngine::Terrain::ContentHash hash, const std::vector<std::uint8_t>& bytes) {
		TerraEngine::Terrain::TerrainExactSpan span;
		span.channel = TerraEngine::Terrain::TerrainChannelKind::Holes;
		span.uncompressedByteCount = static_cast<std::uint32_t>(bytes.size());
		span.compressedContentHash = hash;
		span.immutableBlobHash = hash;
		span.compressedBytes = bytes;
		return span;
	};
	TerraEngine::Terrain::TerrainPatch patch;
	patch.kind = TerraEngine::Terrain::TerrainPatchKind::ReplaceHoles;
	patch.preparedBlobHash = afterHash;
	patch.replacement = *current;
	patch.replacement.editGeneration++;
	patch.replacement.channels.holeHash = afterHash;
	patch.replacement.authoredContentHash = afterHash;
	patch.beforeSpans = {makeSpan(beforeHash, committedHoleMask)};
	patch.afterSpans = {makeSpan(afterHash, previewHoleMask)};
	patch.retainedBlobHashes = {beforeHash, afterHash};
	TerraEngine::Terrain::TerrainPatch inverse = patch;
	inverse.preparedBlobHash = beforeHash;
	inverse.replacement = *current;
	inverse.replacement.editGeneration++;
	inverse.replacement.channels.holeHash = beforeHash;
	inverse.replacement.authoredContentHash = beforeHash;
	inverse.beforeSpans = patch.afterSpans;
	inverse.afterSpans = patch.beforeSpans;
	TerraEngine::Terrain::TerrainEditTransaction transaction;
	transaction.transactionId = TerraEngine::GenerateUUID();
	transaction.origin = TerraEngine::Terrain::TerrainEditOrigin::VisibleEditor;
	transaction.targetAssetUuid = capture.root->assetUuid;
	transaction.historyGroup = "Terrain hole stroke";
	transaction.inputRootHash = capture.root->rootContentHash;
	transaction.expectedLayerStackGeneration = capture.root->layerStackGeneration;
	transaction.affectedBounds = capture.root->authoredBounds;
	transaction.budget.maximumAreas = 1u;
	transaction.readSet = {{current->coordinate, current->editGeneration, current->authoredContentHash}};
	transaction.patches = {std::move(patch)};
	transaction.inversePatches = {std::move(inverse)};
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
	if(!terrainStore || previewSurfaceWeights.size() != TerraEngine::Terrain::kTerrainAreaSampleCount || committedSurfaceWeights.size() != previewSurfaceWeights.size())
	{
		outError = "Surface painting requires one complete canonical terrain area preview.";
		return false;
	}
	std::vector<std::uint8_t> afterBytes, beforeBytes;
	if(!TerraEngine::Terrain::MaterializeTerrainWeightGroup(previewSurfaceWeights, afterBytes).Succeeded()
		|| !TerraEngine::Terrain::MaterializeTerrainWeightGroup(committedSurfaceWeights, beforeBytes).Succeeded())
	{
		outError = "Surface preview contains non-canonical layer weights.";
		return false;
	}
	const auto afterHash = TerraEngine::Terrain::HashTerrainBytes(afterBytes);
	const auto beforeHash = TerraEngine::Terrain::HashTerrainBytes(beforeBytes);
	if(!terrainStore->Blobs().PutVerified(afterHash, std::move(afterBytes)) || !terrainStore->Blobs().PutVerified(beforeHash, std::move(beforeBytes)))
	{
		outError = "Surface preview could not retain its immutable weight blobs.";
		return false;
	}
	const auto capture = terrainStore->CaptureRoot();
	const auto current = terrainStore->FindDescriptor(*capture.root, {0, 0});
	if(!current)
	{
		outError = "Surface preview cannot publish because the selected terrain area is unavailable.";
		return false;
	}
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
	// Resolve returns immutable copies after PutVerified; exact spans retain those
	// bytes so undo has no dependency on a material source or preview buffer.
	const auto afterBlob = terrainStore->Blobs().Resolve(afterHash);
	const auto beforeBlob = terrainStore->Blobs().Resolve(beforeHash);
	if(!afterBlob || !beforeBlob) { outError = "Surface preview blobs disappeared before transaction preparation."; return false; }
	TerraEngine::Terrain::TerrainPatch patch;
	patch.kind = TerraEngine::Terrain::TerrainPatchKind::ReplaceWeightGroup;
	patch.preparedBlobHash = afterHash;
	patch.replacement = *current;
	patch.replacement.editGeneration++;
	patch.replacement.channels.weightGroupHashes[1u] = afterHash;
	patch.replacement.authoredContentHash = afterHash;
	patch.beforeSpans = {makeSpan(beforeHash, *beforeBlob)};
	patch.afterSpans = {makeSpan(afterHash, *afterBlob)};
	patch.retainedBlobHashes = {beforeHash, afterHash};
	TerraEngine::Terrain::TerrainPatch inverse = patch;
	inverse.preparedBlobHash = beforeHash;
	inverse.replacement = *current;
	inverse.replacement.editGeneration++;
	inverse.replacement.channels.weightGroupHashes[1u] = beforeHash;
	inverse.replacement.authoredContentHash = beforeHash;
	inverse.beforeSpans = patch.afterSpans;
	inverse.afterSpans = patch.beforeSpans;
	TerraEngine::Terrain::TerrainEditTransaction transaction;
	transaction.transactionId = TerraEngine::GenerateUUID();
	transaction.origin = TerraEngine::Terrain::TerrainEditOrigin::VisibleEditor;
	transaction.targetAssetUuid = capture.root->assetUuid;
	transaction.historyGroup = "Terrain surface stroke";
	transaction.inputRootHash = capture.root->rootContentHash;
	transaction.expectedLayerStackGeneration = capture.root->layerStackGeneration;
	transaction.affectedBounds = capture.root->authoredBounds;
	transaction.budget.maximumAreas = 1u;
	transaction.readSet = {{current->coordinate, current->editGeneration, current->authoredContentHash}};
	transaction.patches = {std::move(patch)};
	transaction.inversePatches = {std::move(inverse)};
	const TerraEngine::UUID transactionId = transaction.transactionId;
	if(!authoringSystem.EnqueueDetachedCommit(std::move(transaction), outError)) return false;
	queuedSurfaceTransactionId = transactionId;
	status = "Terrain surface preview queued for protected publication.";
	return true;
}

void TerrainWindow::ObserveTerrainMaterialChanges()
{
	const TerrainMaterialChangeSnapshot changes = AssetDatabase::Instance().GetTerrainMaterialChangesSince(observedTerrainMaterialGeneration);
	if(changes.generation == observedTerrainMaterialGeneration && !changes.requiresFullRefresh) return;
	auto applyFallback = [](TerraEngine::Terrain::TerrainLayerStack& stack, TerraEngine::UUID materialId, TerraEngine::Terrain::TerrainLayerFallbackState state) {
		for(auto& layer : stack.layers)
			if(layer.materialAssetUuid == materialId) layer.fallbackState = state;
	};
	if(changes.requiresFullRefresh)
	{
		for(auto* stack : {&committedLayerStack, &previewLayerStack})
			for(auto& layer : stack->layers)
				if(layer.materialAssetUuid != TerraEngine::EMPTY_UUID)
					layer.fallbackState = AssetDatabase::Instance().FindAssetByGuid(layer.materialAssetUuid)
						? TerraEngine::Terrain::TerrainLayerFallbackState::Healthy
						: TerraEngine::Terrain::TerrainLayerFallbackState::MissingSource;
	}
	for(const TerrainMaterialChange& change : changes.changes)
	{
		const auto state = change.kind == TerrainMaterialChangeKind::Deleted
			? TerraEngine::Terrain::TerrainLayerFallbackState::MissingSource
			: TerraEngine::Terrain::TerrainLayerFallbackState::Healthy;
		applyFallback(committedLayerStack, change.materialAssetId, state);
		applyFallback(previewLayerStack, change.materialAssetId, state);
	}
	observedTerrainMaterialGeneration = changes.generation;
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
	snapshot.dirty = terrainDirtyState.IsDirty();
	snapshot.pendingTransactions = terrainQueue ? terrainQueue->PendingCount() : 0U;
	snapshot.historyDepth = UndoRedoSystem::Instance().GetUndoDepth();
	snapshot.historyBytes = UndoRedoSystem::Instance().GetHistoryBytes();
	const auto collision = TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().Snapshot();
	if(collision.status != TerraEngine::Physics::Terrain::TerrainCollisionStatus::Absent)
	{
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
			snapshot.residency.push_back({coordinate, bytes, selectedAreaPinned && coordinate == TerraEngine::Terrain::TerrainAreaCoordinate{0, 0}});
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
		const auto recoveredRoot = terrainStore->CaptureRoot();
		if(const auto area = terrainStore->FindDescriptor(*recoveredRoot.root, {0, 0}); area && area->channels.heightHash)
		{
			if(const auto bytes = terrainStore->Blobs().Resolve(*area->channels.heightHash);
				bytes && bytes->size() == committedHeightMm.size() * sizeof(std::int32_t))
			{
				std::memcpy(committedHeightMm.data(), bytes->data(), bytes->size());
				previewHeightMm = committedHeightMm;
			}
		}
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
	if((!sculpting && !holeSculpting) || !terrainStore) return;
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
		if(x < 0 || z < 0 || x >= 256 || z >= 256) return std::nullopt;
		return static_cast<double>(previewHeightMm[static_cast<std::size_t>(z) * 256U + x]) / 1000.0;
	}, 20000.0, 0.5);
	if(hit)
	{
		// Height brushes stamp vertices at integer samples, while hole brushes stamp
		// the centre of the picked cell at a half-sample offset. Convert the active
		// tool's exact centre and radius into metres for Scene View projection.
		const double surfaceHeight = static_cast<double>(
			previewHeightMm[static_cast<std::size_t>(hit->sample.z) * 256U
				+ static_cast<std::size_t>(hit->sample.x)]) / 1000.0;
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
				: brushSettings.radiusSamples;
			sceneViewBrushFootprint = SceneViewBrushFootprint{
				worldCenter.position,
				std::max(0.001F, radiusUnits) * authoredSampleSpacingMetres,
				holeSculpting};
		}
	}
	// Hovering updates only the projected footprint. Mutation remains gated by
	// the pointer lifecycle below so moving the mouse never creates terrain work.
	if(!pressed && !brushStrokeActive && !holeBrushActive) return;
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
		authoringSession.SetResidentAreas({{0, 0}}); brushStrokeActive = true; previousStamp.reset(); ApplyBrushAt(*hit);
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
	// Pointer routing already owns interaction feedback; a rejected detached
	// sample simply leaves the current preview unchanged. Snap to the selected
	// cell centre so radii below one cell cannot fall between every cell centre.
	(void)PreviewHoleStamp(TerraEngine::Editor::Terrain::TerrainHoleBrush::CenteredOnCell(
		hit.sample.x, hit.sample.z));
}

void TerrainWindow::ApplyBrushAt(const TerraEngine::Editor::Terrain::TerrainPickHit& hit)
{
	const TerraEngine::Editor::Terrain::TerrainBrushStamp stamp{static_cast<float>(hit.sample.x), static_cast<float>(hit.sample.z)};
	if(previousStamp && !TerraEngine::Editor::Terrain::TerrainBrushEngine::IsStrokeSampleDue(*previousStamp, stamp, brushSettings.spacingSamples)) return;
	std::vector<std::int32_t> after(previewHeightMm.size());
	TerraEngine::Editor::Terrain::TerrainBrushEngine::ApplyStamp(previewHeightMm, after, 256U, 256U, brushSettings, stamp, maskEnabled ? std::span<const std::uint8_t>(brushMask) : std::span<const std::uint8_t>{});
	std::vector<std::int32_t> delta(after.size()); for(std::size_t index = 0U; index < after.size(); ++index) delta[index] = after[index] - previewHeightMm[index];
	std::string error; if(!authoringSession.AddHeightDelta({0, 0}, std::move(delta), error)) { status = error; brushStrokeActive = false; return; }
	previewHeightMm = std::move(after); previousStamp = stamp;
	// Render the detached value immediately. The worker owns a copy and publishes
	// only at the protected boundary; mouse release still creates the sole durable
	// transaction and undo-history entry for the complete stroke.
	ScheduleRenderProxyBuild();
}

bool TerrainWindow::QueuePreviewForPublication()
{
	if(!terrainStore || !authoringSession.IsDirty()) return false;
	auto capture = terrainStore->CaptureRoot(); const auto current = terrainStore->FindDescriptor(*capture.root, {0, 0});
	if(!current) { status = "Terrain preview rejected: the selected area descriptor is unavailable."; return false; }
	if(!current->channels.heightHash || !current->channels.heightHash->IsValid())
	{
		status = "Terrain preview rejected: the selected area has no immutable height blob for exact undo.";
		return false;
	}
	std::vector<std::uint8_t> bytes(previewHeightMm.size() * sizeof(std::int32_t)); std::memcpy(bytes.data(), previewHeightMm.data(), bytes.size());
	const auto hash = TerraEngine::Terrain::HashTerrainBytes(bytes); terrainStore->Blobs().PutVerified(hash, std::move(bytes));
	TerraEngine::Terrain::TerrainPatch patch; patch.kind = TerraEngine::Terrain::TerrainPatchKind::ReplaceHeight; patch.preparedBlobHash = hash; patch.replacement = *current; patch.replacement.editGeneration++; patch.replacement.authoredContentHash = hash; patch.replacement.channels.heightHash = hash;
	// Keep the full immutable before/after blobs alive for exact history. The
	// inverse still travels through TerrainPublication and gets fresh CAS
	// observations at replay time; it never writes the live root directly.
	patch.retainedBlobHashes = {*current->channels.heightHash, hash};
	TerraEngine::Terrain::TerrainPatch inverse = patch;
	inverse.preparedBlobHash = *current->channels.heightHash;
	inverse.replacement = *current;
	inverse.replacement.channels.heightHash = *current->channels.heightHash;
	inverse.replacement.authoredContentHash = current->authoredContentHash;
	inverse.retainedBlobHashes = patch.retainedBlobHashes;
	std::string error; const auto transaction = authoringSession.BuildCommitRequest({patch}, "Terrain sculpt stroke", error);
	if(!transaction) { status = error; return false; }
	TerraEngine::Terrain::TerrainEditTransaction prepared = *transaction;
	prepared.inversePatches = {std::move(inverse)};
	if(!authoringSystem.EnqueueDetachedCommit(std::move(prepared), error)) { status = error; return false; }
	status = "Terrain preview queued for the protected authoring boundary."; return true;
}

void TerrainWindow::ProcessProtectedAuthoringBoundary()
{
	// Resource publication belongs to the same protected editor boundary as the
	// terrain receipt. A completed worker mesh cannot appear mid-render-frame.
	PollRenderProxyBuild();
	const auto publicationStart = std::chrono::steady_clock::now();
	authoringSystem.PublishAtProtectedBoundary();
	bool committedRenderableChange = false;
	for(const auto& receipt : authoringSystem.DrainReceipts())
	{
		// Automation jobs become terminal only after this protected-boundary receipt,
		// never merely because a worker prepared a detached transaction.
		automationAdapter.ObserveReceipt(receipt);
		const bool isLayerReceipt = queuedLayerTransactionId && receipt.transactionId == *queuedLayerTransactionId;
		const bool isSurfaceReceipt = queuedSurfaceTransactionId && receipt.transactionId == *queuedSurfaceTransactionId;
		const bool isHoleReceipt = queuedHoleTransactionId && receipt.transactionId == *queuedHoleTransactionId;
		const bool isProceduralReceipt = queuedProceduralTransactionId && receipt.transactionId == *queuedProceduralTransactionId;
		// Undo/redo receipts move the existing cursor. They must not create a
		// second history command simply because their replay committed.
		const bool wasHistoryReplay = UndoRedoSystem::Instance().CompleteTerrainReplay(receipt);
		if(receipt.outcome == TerraEngine::Terrain::TerrainEditOutcome::Committed)
		{
			committedRenderableChange = true;
			if(!wasHistoryReplay && receipt.historyPayload && terrainStore && terrainQueue)
			{
				const auto capture = terrainStore->CaptureRoot();
				TerraEngine::Terrain::TerrainEditTransaction forward;
				forward.targetAssetUuid = capture.root->assetUuid; forward.inputRootHash = capture.root->rootContentHash;
				forward.expectedLayerStackGeneration = capture.root->layerStackGeneration; forward.affectedBounds = capture.root->authoredBounds;
				forward.budget.maximumAreas = std::max<std::size_t>(1u, receipt.historyPayload->forwardPatches.size()); forward.budget.maximumSamples = forward.budget.maximumAreas * TerraEngine::Terrain::kTerrainAreaSampleCount; forward.patches = receipt.historyPayload->forwardPatches;
				forward.layerStackPatch = receipt.historyPayload->forwardLayerStackPatch;
				TerraEngine::Terrain::TerrainEditTransaction inverse = forward;
				inverse.patches = receipt.historyPayload->inversePatches;
				inverse.layerStackPatch = receipt.historyPayload->inverseLayerStackPatch;
				// The command refreshes observations/generations immediately before each
				// deferred replay, keeping stale history atomic and rejectable.
				UndoRedoSystem::Instance().PushCommand(std::make_unique<TerraEngine::Editor::Terrain::TerrainHistoryCommand>(
					*terrainStore, *terrainQueue, std::move(forward), std::move(inverse), receipt.historyPayload->retainedByteCount));
			}
			terrainDirtyState.Observe(terrainStore ? terrainStore->CaptureRoot() : TerraEngine::Terrain::TerrainRootCapture{});
			if(isLayerReceipt)
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
				surfacePreviewActive = false; displacedLayerWarning = 0u; queuedSurfaceTransactionId.reset();
				status = "Terrain surface weights applied at the protected boundary.";
			}
			else if(isHoleReceipt)
			{
				committedHoleMask = previewHoleMask;
				holePreviewActive = false; holeBrushActive = false; queuedHoleTransactionId.reset();
				status = "Terrain holes applied at the protected boundary; collision regeneration was invalidated.";
			}
			else if(isProceduralReceipt)
			{
				// Only the immutable result associated with this receipt becomes
				// authoritative.  A later preview cannot leak through this boundary.
				if(!SynchronizeCommittedHeightFromPublishedRoot() && proceduralPreview)
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
				if(!SynchronizeCommittedHeightFromPublishedRoot()) committedHeightMm = previewHeightMm;
				previewHeightMm = committedHeightMm; authoringSession.Cancel(); applyRequested = false;
				status = "Terrain sculpt applied at the protected boundary (generation " + std::to_string(receipt.publicationGeneration) + ").";
			}
		}
		else
		{
			if(isLayerReceipt) { queuedLayerTransactionId.reset(); layerPreviewActive = false; previewLayerStack = committedLayerStack; }
			if(isSurfaceReceipt) { queuedSurfaceTransactionId.reset(); surfacePreviewActive = false; previewSurfaceWeights = committedSurfaceWeights; }
			if(isHoleReceipt) { queuedHoleTransactionId.reset(); holePreviewActive = false; holeBrushActive = false; previewHoleMask = committedHoleMask; }
			if(isProceduralReceipt) { queuedProceduralTransactionId.reset(); proceduralPreview.reset(); previewHeightMm = committedHeightMm; }
			applyRequested = false;
			status = std::string(isLayerReceipt ? "Terrain layer preview was not published: " : (isSurfaceReceipt ? "Terrain surface preview was not published: " : (isHoleReceipt ? "Terrain hole preview was not published: " : (isProceduralReceipt ? "Terrain procedural preview was not published: " : "Terrain sculpt rejected: ")))) + receipt.message;
		}
	}
	if(committedRenderableChange)
		ScheduleRenderProxyBuild();
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
