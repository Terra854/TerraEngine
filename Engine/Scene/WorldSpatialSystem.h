// WorldSpatialSystem.h - immutable spatial-publication ownership.
//
// This system is the only owner of PublishedSpatialSnapshot generations. Worker
// producers submit complete value candidates; a declared protected boundary
// validates their source revisions before one candidate becomes observable.

#pragma once

#include "Core/SystemBase.h"
#include "Scene/SpatialDiagnostics.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class ObjectManager;

namespace TerraEngine::Scene {
	struct SceneGraph;
	struct SceneTransformPropagationResult;
	struct PreparedSpatialPublication
	{
		std::string correlationId;
		std::uint64_t expectedSceneGeneration = 0U;
		std::uint64_t expectedHierarchyRevision = 0U;
		std::uint64_t expectedSpatialGeneration = 0U;
		std::uint64_t residencyGeneration = 0U;
		std::string requestedBoundary = "Frame End";
		std::vector<PublishedSpatialObject> objects;
		std::vector<SpatialDiagnosticRecord> diagnostics;
	};

	// Builds an immutable publication candidate from worker-owned hierarchy
	// propagation output. The helper does not access ObjectManager or publish.
	TERRAENGINE_API PreparedSpatialPublication PrepareCanonicalHierarchyPublication(
		const SceneGraph& graph,
		const SceneTransformPropagationResult& propagation,
		std::string correlationId,
		std::string requestedBoundary = "Frame End");

	// WorldSpatialSystem keeps worker preparation and protected publication
	// separate. It owns snapshots only, not ObjectManager mutation: callers must
	// use TransactionalMutation before submitting a candidate here.
	class TERRAENGINE_API WorldSpatialSystem final : public SystemsBase
	{
	public:
		static WorldSpatialSystem& Instance();

		WorldSpatialSystem(const WorldSpatialSystem&) = delete;
		WorldSpatialSystem& operator=(const WorldSpatialSystem&) = delete;

		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

		// Worker jobs may submit only complete immutable value candidates. Submission
		// never updates the live snapshot or calls ObjectManager.
		void SubmitPreparedPublication(PreparedSpatialPublication publication);
		[[nodiscard]] PublishedSpatialSnapshot CurrentSnapshot() const;
		[[nodiscard]] SpatialDiagnosticsSnapshot DiagnosticsSnapshot() const;

		// Called only by the scheduler's explicit Frame End flush or its conditional
		// Before Physics flush. It publishes only candidates requested for that exact
		// boundary and never lets normal work leak into an earlier physics boundary.
		bool PublishPreparedAtProtectedBoundary(ObjectManager& objects, std::string boundary = "Frame End");

	private:
		WorldSpatialSystem() = default;

		void PrepareQueuedCandidates();

		mutable std::mutex publicationMutex;
		std::vector<PreparedSpatialPublication> queuedCandidates;
		std::vector<PreparedSpatialPublication> preparedCandidates;
		PublishedSpatialSnapshot currentSnapshot;
		SpatialDiagnosticsSnapshot diagnostics;
		bool initialized = false;
	};
}
