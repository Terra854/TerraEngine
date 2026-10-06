// Instance-local native checkpoints for deterministic settlement regressions.
#pragma once
#include "Core/Export.h"
#include <cstddef>
#include <functional>
namespace TerraEngine::Scheduling {
	class JobGraph;
	enum class JobGraphCheckpoint { BeforePreparation, BeforeSubmission, BeforeCompletion, WorkerEntry };
	using JobGraphCheckpointCallback = std::function<void(JobGraphCheckpoint, std::size_t)>;
	// This internal seam is not an execution option or automation capability.
	// Set only while this graph is quiescent; Execute snapshots the callback.
	class TERRAENGINE_API JobGraphTestAccess {
	public:
		static void SetCheckpoint(JobGraph& graph, JobGraphCheckpointCallback callback);
	};
}
