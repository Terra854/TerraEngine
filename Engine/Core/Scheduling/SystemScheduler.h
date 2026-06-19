// SystemScheduler.h - deterministic frame scheduler metadata and validation.
//
// Part 1 of the scheduler work keeps execution single-threaded, but it records
// the phase, dependency, resource, main-thread, mutation-flush, and profiling
// metadata that a later job graph can lower into worker tasks safely.

#pragma once

#include "Core/Export.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

class SystemsBase;

namespace TerraEngine::Scheduling {

	enum class ResourceAccessMode {
		Read,
		Write
	};

	enum class DiagnosticSeverity {
		Info,
		Warning,
		Error
	};

	enum class DiagnosticCategory {
		Phase,
		Dependency,
		Resource,
		MainThread,
		Mutation,
		DefaultMetadata,
		Summary
	};

	enum class MutationFlushPosition {
		BeforePhase,
		AfterPhase,
		FrameEnd
	};

	struct ResourceAccess {
		std::string resourceName;
		ResourceAccessMode mode = ResourceAccessMode::Read;
	};

	struct SystemDeclaration {
		std::string name;
		std::string phase = "PostFrame";
		std::vector<std::string> before;
		std::vector<std::string> after;
		std::vector<std::string> reads;
		std::vector<std::string> writes;
		bool mainThread = true;
		bool requiresFlushedSceneMutations = false;
		std::string profilingLabel;
		bool usesDefaultMetadata = true;
	};

	struct SchedulerPhaseDefinition {
		std::string name;
		int order = 0;
		bool mainThreadCompatible = true;
		bool allowWorkerExecutionFuture = false;
		bool flushBefore = false;
		bool flushAfter = false;
		std::string description;
	};

	struct SchedulerResourceDefinition {
		std::string name;
		std::string description;
		// Future Part 3 can refine broad resources like SceneGraph/ObjectManager
		// into component read/write declarations without changing diagnostics.
		std::string futureComponentScope;
	};

	struct MutationFlushPoint {
		std::string name;
		MutationFlushPosition position = MutationFlushPosition::FrameEnd;
		std::string phaseName;
		std::vector<std::string> requiredBeforeSystems;
		bool allowEmptyQueue = true;
		bool diagnosePendingAtFrameEnd = false;
	};

	struct ScheduleDiagnostic {
		DiagnosticSeverity severity = DiagnosticSeverity::Info;
		DiagnosticCategory category = DiagnosticCategory::Summary;
		std::string systemName;
		std::string phaseName;
		std::string resourceName;
		std::string message;
		std::string recommendedAction;
	};

	struct ScheduleSummary {
		bool valid = true;
		std::vector<std::string> phases;
		std::vector<std::string> systems;
		std::vector<std::string> flushPoints;
		std::vector<std::string> mainThreadSystems;
		std::vector<std::string> defaultedSystems;
		std::vector<ScheduleDiagnostic> diagnostics;
		std::string formatted;
	};

	struct MutationFlushRuntimeState {
		bool flushAttempted = true;
		std::size_t pendingAfterFlush = 0;
		std::string message;
	};

	using MutationFlushCallback = std::function<MutationFlushRuntimeState(const MutationFlushPoint&)>;

	std::vector<SchedulerPhaseDefinition> DefaultSchedulerPhases();
	std::vector<SchedulerResourceDefinition> DefaultSchedulerResources();
	std::vector<MutationFlushPoint> DefaultMutationFlushPoints();
	std::string ToString(ResourceAccessMode mode);
	std::string ToString(DiagnosticSeverity severity);
	std::string ToString(DiagnosticCategory category);
	std::string ToString(MutationFlushPosition position);
	std::string FormatScheduleSummary(const ScheduleSummary& summary);

	class TERRAENGINE_API SystemScheduler {
	public:
		SystemScheduler();

		void Clear();
		void SetPhaseDefinitions(std::vector<SchedulerPhaseDefinition> phases);
		void SetResourceDefinitions(std::vector<SchedulerResourceDefinition> resources);
		void SetMutationFlushPoints(std::vector<MutationFlushPoint> flushPoints);
		void RegisterSystem(SystemsBase& system);
		void RegisterSystem(SystemsBase& system, SystemDeclaration declaration);

		const ScheduleSummary& BuildSchedule();
		const ScheduleSummary& ExecuteFrame(float deltaTime, const MutationFlushCallback& flushCallback);
		const ScheduleSummary& LatestSummary() const;
		const std::vector<SystemDeclaration>& OrderedDeclarations() const;
		const std::vector<SchedulerPhaseDefinition>& PhaseDefinitions() const;
		const std::vector<MutationFlushPoint>& MutationFlushPoints() const;

	private:
		struct RegisteredSystem {
			SystemsBase* system = nullptr;
			SystemDeclaration declaration;
			std::size_t registrationIndex = 0;
		};

		std::vector<SchedulerPhaseDefinition> phases;
		std::vector<SchedulerResourceDefinition> resources;
		std::vector<MutationFlushPoint> flushPoints;
		std::vector<RegisteredSystem> systems;
		std::vector<std::size_t> orderedIndices;
		std::vector<SystemDeclaration> orderedDeclarations;
		ScheduleSummary latestSummary;
		bool dirty = true;
	};

}
