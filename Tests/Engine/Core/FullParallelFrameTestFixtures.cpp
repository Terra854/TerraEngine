#include "FullParallelFrameTestFixtures.h"

#include "Core/Scheduling/WorkerSafety.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

#include <chrono>
#include <thread>

namespace TerraEngine::Tests::FullParallelFrameFixtures {
	ThreadPoolSession::ThreadPoolSession()
	{
		ThreadPool::instance().shutdown();
		ThreadPool::instance().initialize();
	}

	ThreadPoolSession::~ThreadPoolSession()
	{
		ThreadPool::instance().shutdown();
	}

	TerraEngine::Scheduling::FrameWorkloadDeclaration MakeWorkerWorkload(std::string name, std::vector<std::string> reads, std::vector<std::string> writes)
	{
		TerraEngine::Scheduling::FrameWorkloadDeclaration workload;
		workload.workloadName = std::move(name);
		workload.ownerSystem = "FullParallelFrame";
		workload.phase = "Scene";
		workload.reads = std::move(reads);
		workload.writes = std::move(writes);
		workload.workerEligible = true;
		workload.mainThreadRequired = false;
		workload.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare;
		workload.profilingLabel = workload.workloadName;
		return workload;
	}

	TerraEngine::Scheduling::FrameWorkloadDeclaration MakeProtectedWorkload(std::string name, std::vector<std::string> reads, std::vector<std::string> writes)
	{
		TerraEngine::Scheduling::FrameWorkloadDeclaration workload = MakeWorkerWorkload(std::move(name), std::move(reads), std::move(writes));
		workload.workerEligible = false;
		workload.mainThreadRequired = true;
		workload.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
		workload.protectedReason = "fixture protected commit";
		return workload;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> MakeOverlappingWorkerWorkloads(std::size_t count, std::atomic<int>& activeWorkers, std::atomic<int>& peakWorkers)
	{
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> workloads;
		workloads.reserve(count);
		for (std::size_t index = 0; index < count; ++index)
		{
			auto workload = MakeWorkerWorkload("Worker-" + std::to_string(index), { "ReadOnlyScene" }, {});
			workload.work = [&activeWorkers, &peakWorkers](TerraEngine::Scheduling::JobGraphContext&) {
				const int active = activeWorkers.fetch_add(1) + 1;
				int observed = peakWorkers.load();
				while (active > observed && !peakWorkers.compare_exchange_weak(observed, active))
				{
				}
				const bool workerScopeActive = TerraEngine::Scheduling::IsWorkerSafetyScopeActive();
				if (!workerScopeActive)
				{
					TerraEngine::Scheduling::ReportWorkerSafetyViolation(TerraEngine::Scheduling::WorkerSafetySeverity::Error, "worker-scope", "Worker workload ran without a worker safety scope.", "Execute worker workloads through JobGraph::Execute.");
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				activeWorkers.fetch_sub(1);
			};
			workloads.push_back(std::move(workload));
		}
		return workloads;
	}

	std::vector<TerraEngine::UUID> MakeStressObjectIds(std::size_t count)
	{
		std::vector<TerraEngine::UUID> ids;
		ids.reserve(count);
		for (std::size_t index = 0; index < count; ++index)
		{
			ids.push_back(TerraEngine::GenerateUUID());
		}
		return ids;
	}

	TerraEngine::Scheduling::EcsComponentQueryDeclaration MakeStressComponentQuery(std::string systemName, std::string queryName, std::size_t objectCount)
	{
		TerraEngine::Scheduling::EcsComponentQueryDeclaration query;
		query.queryName = std::move(queryName);
		query.access.ownerSystem = std::move(systemName);
		query.access.queryName = query.queryName;
		query.access.workerEligible = true;
		query.access.readComponents = { "Transform", "MeshRenderComponent" };
		query.access.writeComponents = { "WorldTransform" };
		query.access.scopes = {
			TerraEngine::Scheduling::ComponentAccessScope::Component,
			TerraEngine::Scheduling::ComponentAccessScope::DeferredWrite
		};
		query.matchingObjects = MakeStressObjectIds(objectCount);
		query.workerThreshold = 128u;
		query.preferredChunkSize = 128u;
		query.minChunkSize = 128u;
		query.maxChunkSize = 512u;
		query.maxWorkerChunks = 64u;
		query.adaptiveChunking = true;
		query.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare;
		return query;
	}
}
