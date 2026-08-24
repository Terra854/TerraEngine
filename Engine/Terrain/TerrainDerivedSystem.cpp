// TerrainDerivedSystem.cpp — explicit terrain derived-data scheduler contract.

#include "TerrainDerivedSystem.h"

#include "Components/TerrainComponent.h"
#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/PhysicsTransactions.h"
#include "Renderer/Terrain/TerrainRenderProxyRegistry.h"
#include "Resource/ResourceManager.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ContentMount/TerrainRuntimeProxyLoader.h"
#include "ThreadPool/ThreadPool.h"

#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <unordered_set>
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
		};

		std::unique_ptr<Material> CreateRuntimeTerrainMaterial(ResourceManager& resources)
		{
			// The direct-resident terrain mesh uses the ordinary material contract so
			// it can render immediately while the paged terrain surface path warms up.
			auto material = std::make_unique<Material>();
			std::memcpy(material->header.magic, "TMAT", 4U);
			material->header.version = kMaterialCurrentVersion;
			material->header.headerSize = static_cast<std::uint16_t>(sizeof(MaterialHeader));
			// The generated proxy includes terrain slope normals. Selecting the shared
			// PBR model makes exported terrain consume the same lights and shadows as
			// every other opaque scene surface.
			material->header.shadingModel = static_cast<std::uint32_t>(ShadingModel::MetallicRoughness);
			material->header.alphaMode = static_cast<std::uint32_t>(AlphaMode::Opaque);
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
			return material;
		}
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
		initialized.store(true, std::memory_order_release);

		ResourceManager& resources = ResourceManager::Instance();
		if(!resources.IsExportRuntime() || !ContentMount::Instance().IsMounted()) return;

		// SceneManager has already loaded the startup scene at this protected
		// lifecycle boundary. Capture only durable IDs and logical paths here; the
		// worker tasks below never access live ECS or ResourceManager state.
		std::unordered_set<TerraEngine::UUID> scheduledAssets;
		std::vector<PendingRuntimeTerrainProxy> pending;
		ObjectManager::instance().forEachObjectWithComponents<TerrainComponent>(
			[&](Object, TerrainComponent& terrain)
			{
				if(!terrain.visible || terrain.terrainAssetId == TerraEngine::EMPTY_UUID
					|| !scheduledAssets.emplace(terrain.terrainAssetId).second) return;
				const std::filesystem::path logicalPath = resources.GetRegisteredAssetPath(
					terrain.terrainAssetId, AssetType::Terrain);
				if(logicalPath.empty())
				{
					TERRAENGINE_LOG(LogLevel::Error,
						"Export terrain asset is absent from game.json: "
						<< TerraEngine::ToHexString(terrain.terrainAssetId));
					return;
				}
				// ResourceManager stores preferred paths as normalized absolute keys.
				// ContentMount accepts only archive-relative logical paths, so remove
				// the configured runtime asset root before crossing into the worker.
				const std::filesystem::path mountedPath = logicalPath.is_absolute()
					? logicalPath.lexically_relative(resources.GetAssetRoot()) : logicalPath;
				const std::string mountedPathText = mountedPath.generic_string();
				if(mountedPath.empty() || mountedPathText.starts_with(".."))
				{
					TERRAENGINE_LOG(LogLevel::Error,
						"Export terrain path is outside the mounted content root: " << logicalPath);
					return;
				}

				auto task = ThreadPool::instance().createTask(
					[assetId = terrain.terrainAssetId, path = mountedPathText]()
					{
						return TerraEngine::Runtime::BuildMountedTerrainRuntime(
							ContentMount::Instance(), assetId, path);
					});
				pending.push_back({terrain.terrainAssetId, task->getFuture()});
				ThreadPool::instance().enqueue(task, TerraEngine::Terrain::kTerrainAreaSampleCount);
			});

		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		for(PendingRuntimeTerrainProxy& item : pending)
		{
			// Resource registration and registry publication remain on this protected
			// coordinator boundary; workers return detached value-only meshes.
			auto completed = item.preparedRuntime.get();
			if(!completed.Succeeded())
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"Failed to prepare exported terrain " << TerraEngine::ToHexString(item.assetId)
					<< ": " << completed.renderProxy.diagnostic);
				continue;
			}
			// Exported terrain uses the same typed provider transaction as editor
			// terrain. The package's committed height/hole generation therefore owns a
			// static Jolt body automatically, without serialized collider components.
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
			if(materialId == TerraEngine::EMPTY_UUID)
			{
				materialId = resources.RegisterMaterial(
					"terrain-runtime-direct-fallback", CreateRuntimeTerrainMaterial(resources));
			}
			const TerraEngine::UUID meshId = resources.RegisterMesh(
				"terrain/runtime/" + TerraEngine::ToHexString(item.assetId), std::move(completed.renderProxy.mesh));
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

	void TerrainDerivedSystem::Update(float)
	{
		// All future work is declared below; this avoids a hidden sequential path.
	}

	void TerrainDerivedSystem::Shutdown()
	{
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
		prepare.activationPredicateKey = "terrain-derived-lifecycle";
		prepare.safeSubwork = "Regenerates value-only terrain products without mutating renderer, physics, or authored storage.";

		FrameWorkloadDeclaration publish = MakeProtectedCommitPhase(
			"TerrainDerivedPublish", SystemName(),
			{ kResourceTerrainPreparedDerived, kResourceTerrainPublishedDerived },
			{ kResourceTerrainPublishedDerived, kResourceTerrainDiagnostics },
			"Terrain derived publication requires a complete, generation-matched candidate set.",
			"TerrainDerived.Publish");
		publish.stableDescriptorId = "TerrainDerivedSystem/Render/TerrainDerivedPublish";
		publish.activationPredicateKey = prepare.activationPredicateKey;
		publish.explicitDependencies = { prepare.stableDescriptorId };
		publish.sameFrameVisibilityRequired = true;

		for(FrameWorkloadDeclaration* workload : { &prepare, &publish })
		{
			workload->active = active;
			workload->ready = active;
			workload->readinessReason = active ? std::string{} : "terrain derived system is not initialized";
		}
		return { std::move(prepare), std::move(publish) };
	}
}
