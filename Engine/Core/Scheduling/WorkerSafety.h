// WorkerSafety.h - runtime guardrails for worker-safe frame execution.
//
// The scheduler proves most worker safety statically through access metadata,
// but runtime code still needs a lightweight way to identify "this callback is
// currently running on a worker" and record unsafe direct mutation attempts.

#pragma once

#include "Core/Export.h"

#include <string>
#include <vector>

namespace TerraEngine::Scheduling {

	enum class WorkerSafetySeverity {
		Info,
		Warning,
		Error
	};

	struct WorkerSafetyViolation {
		WorkerSafetySeverity severity = WorkerSafetySeverity::Info;
		std::string workloadName;
		std::string operationName;
		std::string message;
		std::string recommendedAction;
	};

	class TERRAENGINE_API WorkerSafetyScope {
	public:
		explicit WorkerSafetyScope(std::string workloadName);
		~WorkerSafetyScope();
		WorkerSafetyScope(const WorkerSafetyScope&) = delete;
		WorkerSafetyScope& operator=(const WorkerSafetyScope&) = delete;

	private:
		std::string previousWorkloadName;
		bool previousWorkerState = false;
	};

	TERRAENGINE_API bool IsWorkerSafetyScopeActive();
	TERRAENGINE_API std::string CurrentWorkerSafetyWorkload();
	TERRAENGINE_API void ReportWorkerSafetyViolation(WorkerSafetySeverity severity, std::string operationName, std::string message, std::string recommendedAction);
	TERRAENGINE_API std::vector<WorkerSafetyViolation> SnapshotWorkerSafetyViolations();
	TERRAENGINE_API std::vector<WorkerSafetyViolation> DrainWorkerSafetyViolations();
	TERRAENGINE_API std::string FormatWorkerSafetyViolations(const std::vector<WorkerSafetyViolation>& violations);

}
