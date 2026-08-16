// WorldSpatialSystem.cpp - worker candidate preparation and protected snapshot publication.

#include "WorldSpatialSystem.h"

#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Scene/SceneGraphRuntime.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace TerraEngine::Scene {
	PreparedSpatialPublication PrepareCanonicalHierarchyPublication(
		const SceneGraph& graph,
		const SceneTransformPropagationResult& propagation,
		std::string correlationId,
		std::string requestedBoundary)
	{
		PreparedSpatialPublication publication;
		publication.correlationId = std::move(correlationId);
		publication.expectedSceneGeneration = graph.sceneGeneration;
		publication.expectedHierarchyRevision = graph.hierarchyRevision;
		publication.requestedBoundary = std::move(requestedBoundary);
		if (!propagation.valid || propagation.graphRevision != graph.revision)
		{
			publication.diagnostics.push_back({
				publication.correlationId,
				"SceneGraphRuntime",
				TerraEngine::EMPTY_UUID,
				SpatialTransitionOutcome::Rejected,
				SpatialPersistenceOutcome::None,
				"StaleHierarchyPropagation",
				0U, 0U, 0U, 0U, 0U, 0U, 0.0 });
			return publication;
		}
		publication.objects.reserve(propagation.records.size());
		for (const SceneWorldTransformRecord& record : propagation.records)
		{
			publication.objects.push_back({
				record.objectId,
				record.worldPose,
				record.worldPose.transformRevision,
				record.worldPose.coordinateFrameRevision });
		}
		return publication;
	}
	WorldSpatialSystem& WorldSpatialSystem::Instance()
	{
		static WorldSpatialSystem instance;
		return instance;
	}

	void WorldSpatialSystem::Initialize()
	{
		std::scoped_lock lock(publicationMutex);
		queuedCandidates.clear();
		preparedCandidates.clear();
		currentSnapshot = {};
		diagnostics = {};
		initialized = true;
	}

	void WorldSpatialSystem::Update(float)
	{
		// Converted systems run through declared workloads. Keeping Update inert
		// prevents an accidental live publication outside a protected boundary.
	}

	void WorldSpatialSystem::Shutdown()
	{
		std::scoped_lock lock(publicationMutex);
		initialized = false;
		queuedCandidates.clear();
		preparedCandidates.clear();
		currentSnapshot = {};
	}

	std::string WorldSpatialSystem::SystemName()
	{
		return "WorldSpatialSystem";
	}

	TerraEngine::Scheduling::SystemDeclaration WorldSpatialSystem::SchedulerDeclaration()
	{
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration;
		declaration.name = SystemName();
		declaration.phase = "Scene";
		declaration.reads = { kResourceSpatialTransitionQueue, kResourcePublishedSpatialSnapshot };
		declaration.writes = { kResourcePreparedSpatialTransitions };
		declaration.mainThread = false;
		declaration.after = { "SceneManager" };
		declaration.profilingLabel = "World Spatial Publication";
		declaration.usesDefaultMetadata = false;
		return declaration;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> WorldSpatialSystem::DeclareFrameWorkloads(float)
	{
		using namespace TerraEngine::Scheduling;
		FrameWorkloadDeclaration prepare = MakeWorkerPreparePhase(
			"WorldSpatialPrepare", SystemName(),
			{ kResourceSpatialTransitionQueue, kResourcePublishedSpatialSnapshot },
			{ kResourcePreparedSpatialTransitions }, "World Spatial Prepare");
		prepare.work = [this](JobGraphContext&) { PrepareQueuedCandidates(); };
		return { std::move(prepare) };
	}

	void WorldSpatialSystem::SubmitPreparedPublication(PreparedSpatialPublication publication)
	{
		std::scoped_lock lock(publicationMutex);
		// This private queue carries copied values only. Worker producers cannot make
		// state visible merely by submitting a candidate.
		queuedCandidates.push_back(std::move(publication));
	}

	PublishedSpatialSnapshot WorldSpatialSystem::CurrentSnapshot() const
	{
		std::scoped_lock lock(publicationMutex);
		return currentSnapshot;
	}

	SpatialDiagnosticsSnapshot WorldSpatialSystem::DiagnosticsSnapshot() const
	{
		std::scoped_lock lock(publicationMutex);
		return diagnostics;
	}

	void WorldSpatialSystem::PrepareQueuedCandidates()
	{
		std::scoped_lock lock(publicationMutex);
		if (!initialized || queuedCandidates.empty())
		{
			return;
		}
		preparedCandidates.insert(preparedCandidates.end(),
			std::make_move_iterator(queuedCandidates.begin()),
			std::make_move_iterator(queuedCandidates.end()));
		queuedCandidates.clear();
		std::sort(preparedCandidates.begin(), preparedCandidates.end(), [](const auto& left, const auto& right) {
			return left.correlationId < right.correlationId;
		});
	}

	bool WorldSpatialSystem::PublishPreparedAtProtectedBoundary(ObjectManager& objects, std::string boundary)
	{
		std::scoped_lock lock(publicationMutex);
		if (!initialized || preparedCandidates.empty())
		{
			return false;
		}

		std::vector<PreparedSpatialPublication> eligible;
		std::vector<PreparedSpatialPublication> deferred;
		eligible.reserve(preparedCandidates.size());
		deferred.reserve(preparedCandidates.size());
		for (PreparedSpatialPublication& candidate : preparedCandidates)
		{
			if (candidate.requestedBoundary == boundary)
			{
				eligible.push_back(std::move(candidate));
			}
			else
			{
				deferred.push_back(std::move(candidate));
			}
		}
		preparedCandidates = std::move(deferred);
		if (eligible.empty())
		{
			return false;
		}

		const auto staleCandidate = [&objects, this](const PreparedSpatialPublication& candidate) {
			if ((candidate.expectedSceneGeneration != 0U && candidate.expectedSceneGeneration != objects.GetSceneGeneration())
				|| (candidate.expectedHierarchyRevision != 0U && candidate.expectedHierarchyRevision != objects.GetHierarchyRevision())
				|| (candidate.expectedSpatialGeneration != 0U && candidate.expectedSpatialGeneration != currentSnapshot.spatialGeneration))
			{
				return true;
			}
			for (const PublishedSpatialObject& object : candidate.objects)
			{
				const std::optional<Object> liveObject = objects.getObject(object.objectId);
				const std::optional<WorldPose> livePose = liveObject ? objects.GetCanonicalWorldPose(*liveObject) : std::nullopt;
				if (!livePose || livePose->transformRevision != object.transformRevision
					|| livePose->coordinateFrameRevision != object.coordinateFrameRevision)
				{
					return true;
				}
			}
			return false;
		};
		if (std::any_of(eligible.begin(), eligible.end(), staleCandidate))
		{
			diagnostics.rejectedTransitionCount += static_cast<std::uint32_t>(eligible.size());
			return false;
		}

		PublishedSpatialSnapshot next;
		next.sceneGeneration = objects.GetSceneGeneration();
		next.hierarchyRevision = objects.GetHierarchyRevision();
		next.coordinateFrameRevision = currentSnapshot.coordinateFrameRevision + 1U;
		next.spatialGeneration = currentSnapshot.spatialGeneration + 1U;
		next.residencyGeneration = currentSnapshot.residencyGeneration;
		next.publicationBoundary = std::move(boundary);
		for (PreparedSpatialPublication& candidate : eligible)
		{
			next.residencyGeneration = std::max(next.residencyGeneration, candidate.residencyGeneration);
			next.objects.insert(next.objects.end(), std::make_move_iterator(candidate.objects.begin()), std::make_move_iterator(candidate.objects.end()));
			next.transitionDiagnostics.insert(next.transitionDiagnostics.end(),
				std::make_move_iterator(candidate.diagnostics.begin()), std::make_move_iterator(candidate.diagnostics.end()));
		}
		for (SpatialDiagnosticRecord& record : next.transitionDiagnostics)
		{
			record.outcome = SpatialTransitionOutcome::Committed;
			record.publishedSpatialGeneration = next.spatialGeneration;
		}

		currentSnapshot = std::move(next);
		diagnostics.publishedSpatialGeneration = currentSnapshot.spatialGeneration;
		++diagnostics.publishedSnapshotCount;
		diagnostics.committedTransitionCount += static_cast<std::uint32_t>(currentSnapshot.transitionDiagnostics.size());
		diagnostics.lastPublicationBoundary = currentSnapshot.publicationBoundary;
		return true;
	}
}
