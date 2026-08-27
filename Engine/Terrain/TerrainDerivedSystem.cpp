// TerrainDerivedSystem.cpp — explicit terrain derived-data scheduler contract.

#include "TerrainDerivedSystem.h"

#include "Terrain/TerrainMaterial.h"

#include "Core/Logger/Logger.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/PhysicsTransactions.h"
#include "Renderer/Terrain/TerrainRenderProxyRegistry.h"
#include "Resource/ResourceManager.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ContentMount/TerrainRuntimeBootstrap.h"
#include "Runtime/ContentMount/TerrainRuntimeProxyLoader.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Terrain
{
	namespace
	{
		struct PendingRuntimeTerrainProxy
		{
			TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
			std::future<TerraEngine::Runtime::MountedTerrainRuntimeResult> preparedRuntime;
			TerrainMaterialSettings materialSettings{};
		};
	}

	TerrainDerivedPublicationCheck ValidateTerrainDerivedCandidate(const TerrainDerivedCandidate& candidate, const TerrainAssetStore& store)
	{
		// This is deliberately repeated at the protected boundary. A worker may
		// finish after an edit/undo and must never replace last-known-good output.
		if (candidate.cancellation != nullptr && candidate.cancellation->load(std::memory_order_acquire))
			return {TerrainResultCode::Cancelled, "Terrain derived candidate was cancelled before publication."};
		if (!candidate.complete || candidate.assetUuid == TerraEngine::EMPTY_UUID || !candidate.sourceRootHash.IsValid() || candidate.sourcePublicationGeneration == 0u)
			return {TerrainResultCode::InvalidArgument, "Terrain derived candidate is incomplete."};
		const TerrainRootCapture capture = store.CaptureRoot();
		if (!capture.IsValid() || capture.root->assetUuid != candidate.assetUuid || capture.root->rootContentHash != candidate.sourceRootHash || capture.publicationGeneration != candidate.sourcePublicationGeneration)
			return {TerrainResultCode::Conflict, "Terrain derived candidate observed a stale authored generation."};
		return {};
	}

	TerrainDerivedPublicationCheck ValidateTerrainMaterialMappingCandidate(
		const TerrainMaterialMappingCandidate& candidate, const TerrainAssetStore& store)
	{
		if(const TerrainDerivedPublicationCheck source = ValidateTerrainDerivedCandidate(candidate.source, store);
			!source.Succeeded()) return source;
		if(!candidate.invalidation.Contains(TerrainInvalidationKind::MaterialBindings)) return {};
		if(!candidate.mappingComplete || candidate.layerPhases.empty())
			return {TerrainResultCode::InvalidArgument,
				"Terrain material invalidation requires a complete immutable mapping candidate."};
		return {};
	}

	TerrainDerivedPublicationCheck ValidateTerrainDerivedSurfaceBatch(const TerrainDerivedSurfaceBatch& batch, const TerrainAssetStore& store)
	{
		if (const auto source = ValidateTerrainDerivedCandidate(batch.source, store); !source.Succeeded()) return source;
		if (batch.derivedGeneration == 0u || !batch.geometryIdentity.IsValid() || !batch.holeIdentity.IsValid()
			|| !batch.surfaceControlIdentity.IsValid() || !batch.geometryComplete || !batch.holesComplete || !batch.surfaceControlsComplete)
			return {TerrainResultCode::InvalidArgument, "Terrain geometry, holes, and surface controls must form one complete derived generation."};
		return {};
	}

	TerrainDerivedSystem& TerrainDerivedSystem::Instance()
	{
		static TerrainDerivedSystem instance;
		return instance;
	}

	void TerrainDerivedSystem::Initialize()
	{
		cancelRequested.store(false, std::memory_order_release);
		initialized.store(true, std::memory_order_release);

		ResourceManager& resources = ResourceManager::Instance();
		if(!resources.IsExportRuntime() || !ContentMount::Instance().IsMounted()) return;

		// A dedicated protected bootstrap seam copies durable scene values before
		// this worker-facing system dispatches package decode and terrain cooking.
		std::vector<PendingRuntimeTerrainProxy> pending;
		for(const TerraEngine::Runtime::MountedTerrainBootstrapRequest& request
			: TerraEngine::Runtime::CaptureMountedTerrainBootstrapRequests())
		{
				auto task = ThreadPool::instance().createTask(
					[assetId = request.assetId, path = request.logicalPath]()
					{
						return TerraEngine::Runtime::BuildMountedTerrainRuntime(
							ContentMount::Instance(), assetId, path);
					});
				pending.push_back({request.assetId, task->getFuture(), request.materialSettings});
				ThreadPool::instance().enqueue(task, TerraEngine::Terrain::kTerrainAreaSampleCount);
		}

		for(PendingRuntimeTerrainProxy& item : pending)
		{
			// Registration and provider transactions are protected operations. Worker
			// results stay detached until every decoded area and derivative is valid.
			auto completed = item.preparedRuntime.get();
			if(!completed.Succeeded())
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"Failed to prepare exported terrain " << TerraEngine::ToHexString(item.assetId)
					<< ": " << completed.renderProxy.diagnostic);
				continue;
			}
			TerraEngine::Physics::TerrainCollisionReplacementPacket collisionPacket;
			collisionPacket.id = "runtime-terrain-collision-" + TerraEngine::ToHexString(item.assetId);
			collisionPacket.sourceRootHash = completed.sourceRootHash;
			collisionPacket.sourcePublicationGeneration = completed.sourcePublicationGeneration;
			collisionPacket.recipe = std::move(completed.collisionRecipe);
			const auto collisionReceipt = TerraEngine::Physics::PhysicsSystem::Instance()
				.CommitTerrainCollisionReplacement(std::move(collisionPacket));
			if(!collisionReceipt.committed)
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"Failed to publish exported terrain collision "
					<< TerraEngine::ToHexString(item.assetId) << ": " << collisionReceipt.message);
				continue;
			}
			const TerraEngine::UUID materialId = resources.RegisterMaterial(
				"terrain/runtime/" + TerraEngine::ToHexString(item.assetId),
				CreateTerrainMaterial(resources, item.materialSettings));
			const TerraEngine::UUID meshId = resources.RegisterMesh(
				"terrain/runtime/" + TerraEngine::ToHexString(item.assetId),
				std::move(completed.renderProxy.mesh));
			const bool published = TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance().Publish(
				{item.assetId, meshId, materialId, 0U});
			if(!published)
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"Failed to publish exported terrain render proxy: "
					<< TerraEngine::ToHexString(item.assetId));
				continue;
			}
			TERRAENGINE_LOG(LogLevel::Info,
				"Published exported terrain render proxy: " << TerraEngine::ToHexString(item.assetId));
		}
	}

	bool TerrainDerivedSystem::EnqueueInvalidation(
		TerrainDerivedInvalidation invalidation, TerrainAssetStore& store)
	{
		const TerrainRootCapture capture = store.CaptureRoot();
		if(!capture.IsValid() || invalidation.assetUuid == TerraEngine::EMPTY_UUID
			|| invalidation.kinds.Empty() || capture.root->assetUuid != invalidation.assetUuid
			|| capture.publicationGeneration != invalidation.publicationGeneration) return false;
		std::scoped_lock lock(queueMutex);
		pendingInvalidations.push_back({std::move(invalidation), capture, &store});
		return true;
	}

	void TerrainDerivedSystem::PrepareInvalidationUnion()
	{
		std::vector<PendingInvalidation> pending;
		{
			std::scoped_lock lock(queueMutex);
			pending.swap(pendingInvalidations);
		}
		if(pending.empty() || cancelRequested.load(std::memory_order_acquire)) return;

		// All receipts for one asset become one latest-root work item. Older roots
		// contribute only their invalidation kinds/areas; the newest immutable root
		// is sufficient to derive the complete current result.
		std::vector<PendingInvalidation> unioned;
		for(PendingInvalidation& request : pending)
		{
			auto existing = std::find_if(unioned.begin(), unioned.end(), [&](const PendingInvalidation& item) {
				return item.invalidation.assetUuid == request.invalidation.assetUuid;
			});
			if(existing == unioned.end())
			{
				unioned.push_back(std::move(request));
				continue;
			}
			existing->invalidation.kinds.UnionWith(request.invalidation.kinds);
			existing->invalidation.areas.insert(existing->invalidation.areas.end(),
				request.invalidation.areas.begin(), request.invalidation.areas.end());
			existing->invalidation.layerIds.insert(existing->invalidation.layerIds.end(),
				request.invalidation.layerIds.begin(), request.invalidation.layerIds.end());
			if(request.capture.publicationGeneration > existing->capture.publicationGeneration)
			{
				existing->capture = std::move(request.capture);
				existing->store = request.store;
				existing->invalidation.publicationGeneration = existing->capture.publicationGeneration;
			}
		}

		std::vector<PreparedCandidate> built;
		built.reserve(unioned.size());
		for(PendingInvalidation& request : unioned)
		{
			auto& areas = request.invalidation.areas;
			std::sort(areas.begin(), areas.end());
			areas.erase(std::unique(areas.begin(), areas.end()), areas.end());
			auto& layerIds = request.invalidation.layerIds;
			std::sort(layerIds.begin(), layerIds.end());
			layerIds.erase(std::unique(layerIds.begin(), layerIds.end()), layerIds.end());

			TerrainMaterialMappingCandidate candidate{};
			candidate.source.assetUuid = request.capture.root->assetUuid;
			candidate.source.sourceRootHash = request.capture.root->rootContentHash;
			candidate.source.sourcePublicationGeneration = request.capture.publicationGeneration;
			candidate.source.cancellation = &cancelRequested;
			candidate.invalidation = request.invalidation;
			candidate.mappingComplete = !candidate.invalidation.Contains(TerrainInvalidationKind::MaterialBindings);

			if(candidate.invalidation.Contains(TerrainInvalidationKind::MaterialBindings)
				&& request.capture.root->layerStack)
			{
				std::vector<TerrainAreaCoordinate> phaseAreas = areas;
				for(const TerrainDirectoryShard& shard : request.capture.root->directoryShards)
					for(const auto& [area, descriptor] : shard.descriptors)
					{
						(void)descriptor;
						phaseAreas.push_back(area);
					}
				if(phaseAreas.empty())
				{
					const auto owner = OwnerArea({request.capture.root->authoredBounds.minX,
						request.capture.root->authoredBounds.minZ});
					if(owner.has_value()) phaseAreas.push_back(*owner);
				}
				std::sort(phaseAreas.begin(), phaseAreas.end());
				phaseAreas.erase(std::unique(phaseAreas.begin(), phaseAreas.end()), phaseAreas.end());

				candidate.mappingComplete = !phaseAreas.empty();
				for(const TerrainAreaCoordinate area : phaseAreas)
				{
					for(const TerrainLayer& layer : request.capture.root->layerStack->layers)
					{
						if(!layer.enabled) continue;
						TerrainAreaMaterialPhase phase{};
						if(!PrepareTerrainAreaMaterialPhase(area, request.capture.root->sampleSpacingMetres,
							layer.mapping, phase).Succeeded())
						{
							candidate.mappingComplete = false;
							candidate.layerPhases.clear();
							break;
						}
						candidate.layerPhases.push_back({layer.stableId, phase});
					}
					if(!candidate.mappingComplete) break;
				}
			}
			candidate.source.complete = !cancelRequested.load(std::memory_order_acquire)
				&& candidate.mappingComplete;
			built.push_back({std::move(candidate), request.store});
		}

		std::scoped_lock lock(queueMutex);
		preparedCandidates.insert(preparedCandidates.end(),
			std::make_move_iterator(built.begin()), std::make_move_iterator(built.end()));
	}

	void TerrainDerivedSystem::PublishPreparedAtProtectedBoundary()
	{
		std::vector<PreparedCandidate> prepared;
		{
			std::scoped_lock lock(queueMutex);
			prepared.swap(preparedCandidates);
		}
		for(PreparedCandidate& item : prepared)
		{
			TerrainDerivedPublicationReceipt receipt{};
			receipt.invalidation = item.candidate.invalidation;
			const TerrainDerivedPublicationCheck validation = item.store == nullptr
				? TerrainDerivedPublicationCheck{TerrainResultCode::InvalidArgument, "Terrain derived candidate lost its asset-store owner."}
				: ValidateTerrainMaterialMappingCandidate(item.candidate, *item.store);
			receipt.code = validation.code;
			receipt.message = validation.message;
			receipt.committed = validation.Succeeded();
			receipt.materialBindingsPublished = receipt.committed
				&& item.candidate.invalidation.Contains(TerrainInvalidationKind::MaterialBindings);

			std::scoped_lock lock(queueMutex);
			if(receipt.materialBindingsPublished)
				publishedMaterialCandidates[item.candidate.source.assetUuid] = item.candidate;
			publicationReceipts.push_back(std::move(receipt));
		}
	}

	std::optional<TerrainMaterialMappingCandidate> TerrainDerivedSystem::ResolvePublishedMaterialCandidate(
		TerraEngine::UUID assetUuid) const
	{
		std::scoped_lock lock(queueMutex);
		const auto found = publishedMaterialCandidates.find(assetUuid);
		return found == publishedMaterialCandidates.end()
			? std::nullopt : std::optional<TerrainMaterialMappingCandidate>{found->second};
	}

	std::vector<TerrainDerivedPublicationReceipt> TerrainDerivedSystem::DrainPublicationReceipts()
	{
		std::scoped_lock lock(queueMutex);
		std::vector<TerrainDerivedPublicationReceipt> drained;
		drained.swap(publicationReceipts);
		return drained;
	}

	void TerrainDerivedSystem::Update(float)
	{
		// All future work is declared below; this avoids a hidden sequential path.
	}

	void TerrainDerivedSystem::Shutdown()
	{
		cancelRequested.store(true, std::memory_order_release);
		std::scoped_lock lock(queueMutex);
		pendingInvalidations.clear();
		preparedCandidates.clear();
		publicationReceipts.clear();
		publishedMaterialCandidates.clear();
		TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance().Clear();
		initialized.store(false, std::memory_order_release);
	}

	std::string TerrainDerivedSystem::SystemName()
	{
		return "TerrainDerivedSystem";
	}

	TerraEngine::Scheduling::SystemDeclaration TerrainDerivedSystem::SchedulerDeclaration()
	{
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration;
		declaration.name = SystemName();
		declaration.phase = "Render";
		declaration.after = { "TerrainStreamingSystem" };
		declaration.before = { "VirtualGeometryStreamingSystem", "RenderSystem" };
		declaration.reads = { kResourceTerrainAssetRoots, kResourceTerrainPublishedResidency, kResourceTerrainDerivedInvalidations };
		declaration.writes = { kResourceTerrainPreparedDerived, kResourceTerrainPublishedDerived, kResourceTerrainDiagnostics };
		declaration.mainThread = false;
		declaration.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		declaration.boundaryClassification = HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Builds immutable terrain render, control, and collision candidates from committed authored snapshots on workers.";
		declaration.protectedCommit = "Validates source generations and publishes a coherent derived terrain generation before render consumers capture their inputs.";
		declaration.protectedReason = declaration.protectedCommit;
		declaration.profilingLabel = "Terrain Derived";
		declaration.usesDefaultMetadata = false;
		return declaration;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> TerrainDerivedSystem::DeclareFrameWorkloads(float)
	{
		using namespace TerraEngine::Scheduling;
		const bool active = initialized.load(std::memory_order_acquire);
		FrameWorkloadDeclaration prepare = MakeWorkerPreparePhase(
			"TerrainDerivedPrepare", SystemName(),
			{ kResourceTerrainAssetRoots, kResourceTerrainPublishedResidency, kResourceTerrainDerivedInvalidations },
			{ kResourceTerrainPreparedDerived }, "TerrainDerived.Prepare");
		prepare.stableDescriptorId = "TerrainDerivedSystem/Render/TerrainDerivedPrepare";
		prepare.phase = "Render";
		prepare.activationPredicateKey = "terrain-derived-lifecycle";
		prepare.boundaryClassification = HardBoundaryClassification::WorkerSafe;
		prepare.safeSubwork = "Regenerates value-only terrain products without mutating renderer, physics, or authored storage.";
		// WorkerPrepare workloads are lowered by JobGraph onto the engine ThreadPool;
		// this callback therefore sees only immutable root captures and value output.
		prepare.work = [this](TerraEngine::Scheduling::JobGraphContext&) { PrepareInvalidationUnion(); };

		FrameWorkloadDeclaration publish = MakeProtectedCommitPhase(
			"TerrainDerivedPublish", SystemName(),
			{ kResourceTerrainPreparedDerived, kResourceTerrainPublishedDerived },
			{ kResourceTerrainPublishedDerived, kResourceTerrainDiagnostics },
			"Terrain derived publication requires a complete, generation-matched candidate set.",
			"TerrainDerived.Publish");
		publish.stableDescriptorId = "TerrainDerivedSystem/Render/TerrainDerivedPublish";
		publish.phase = "Render";
		publish.activationPredicateKey = prepare.activationPredicateKey;
		publish.explicitDependencies = { prepare.stableDescriptorId };
		publish.boundaryClassification = HardBoundaryClassification::Protected;
		publish.hardBoundary = true;
		publish.protectedCommit = publish.protectedReason;
		publish.sameFrameVisibilityRequired = true;
		publish.work = [this](TerraEngine::Scheduling::JobGraphContext&) { PublishPreparedAtProtectedBoundary(); };

		for(FrameWorkloadDeclaration* workload : { &prepare, &publish })
		{
			workload->active = active;
			workload->ready = active;
			workload->readinessReason = active ? std::string{} : "terrain derived system is not initialized";
		}
		return { std::move(prepare), std::move(publish) };
	}
}
