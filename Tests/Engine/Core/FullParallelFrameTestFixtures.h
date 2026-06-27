#pragma once

#include "Core/Scheduling/EcsQueryTaskGraph.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Utilities/UUID.h"

#include <atomic>
#include <string>
#include <vector>

namespace TerraEngine::Tests::FullParallelFrameFixtures {
	class ThreadPoolSession {
	public:
		ThreadPoolSession();
		~ThreadPoolSession();
		ThreadPoolSession(const ThreadPoolSession&) = delete;
		ThreadPoolSession& operator=(const ThreadPoolSession&) = delete;
	};

	TerraEngine::Scheduling::FrameWorkloadDeclaration MakeWorkerWorkload(std::string name, std::vector<std::string> reads = {}, std::vector<std::string> writes = {});
	TerraEngine::Scheduling::FrameWorkloadDeclaration MakeProtectedWorkload(std::string name, std::vector<std::string> reads = {}, std::vector<std::string> writes = {});
	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> MakeOverlappingWorkerWorkloads(std::size_t count, std::atomic<int>& activeWorkers, std::atomic<int>& peakWorkers);
	std::vector<TerraEngine::UUID> MakeStressObjectIds(std::size_t count);
	TerraEngine::Scheduling::EcsComponentQueryDeclaration MakeStressComponentQuery(std::string systemName, std::string queryName, std::size_t objectCount);
}
