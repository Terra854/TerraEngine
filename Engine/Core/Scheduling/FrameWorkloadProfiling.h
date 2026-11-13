#pragma once

#include "Core/Export.h"
#include "Profiling/PerformanceTelemetry.h"

#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Scheduling {
	// This key copies the profiler's existing capture/frame tokens. Both must be
	// nonzero; a scheduler label or configured worker count is never an identity.
	struct FrameWorkloadProfilingIdentity {
		std::uint64_t sessionGeneration = 0u;
		std::uint64_t frameToken = 0u;
		bool operator==(const FrameWorkloadProfilingIdentity&) const = default;
	};

	// Available with zero rows means an actual collected-empty publication. A
	// missing, consumed, stale, oversized or disabled observation stays false.
	struct FrameWorkloadProfilingSnapshot {
		bool available = false;
		FrameWorkloadProfilingIdentity identity;
		std::vector<TerraEngine::Profiling::WorkloadExecutionRecord> records;
	};

	// All functions copy metadata only at existing protected frame boundaries.
	// They never execute systems, flush mutations or retain workload callbacks.
	TERRAENGINE_API void BeginFrameWorkloadProfilingCapture(FrameWorkloadProfilingIdentity identity);
	TERRAENGINE_API void InvalidateFrameWorkloadProfilingCapture();
	TERRAENGINE_API FrameWorkloadProfilingIdentity PrepareFrameWorkloadProfilingPublication();
	// Optional copy failure returns false and never fails successful frame work.
	TERRAENGINE_API bool PublishFrameWorkloadProfilingSnapshot(
		FrameWorkloadProfilingIdentity identity,
		std::span<const TerraEngine::Profiling::WorkloadExecutionRecord> records) noexcept;
	TERRAENGINE_API FrameWorkloadProfilingSnapshot TakeLatestFrameWorkloadProfilingSnapshot(
		FrameWorkloadProfilingIdentity expectedIdentity);
}
