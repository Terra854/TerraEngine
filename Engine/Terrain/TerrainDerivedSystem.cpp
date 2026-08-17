// TerrainDerivedSystem.cpp — explicit terrain derived-data scheduler contract.

#include "TerrainDerivedSystem.h"

namespace TerraEngine::Terrain
{
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
	}

	void TerrainDerivedSystem::Update(float)
	{
		// All future work is declared below; this avoids a hidden sequential path.
	}

	void TerrainDerivedSystem::Shutdown()
	{
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
