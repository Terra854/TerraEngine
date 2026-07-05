// SystemScheduler.h - deterministic frame scheduler metadata and validation.
//
// Part 1 of the scheduler work keeps execution single-threaded, but it records
// the phase, dependency, resource, main-thread, mutation-flush, and profiling
// metadata that a later job graph can lower into worker tasks safely.

#pragma once

#include "Core/Export.h"
#include "FrameWorkload.h"
#include "Scene/TransactionalMutation.h"

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
		Scheduler,
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

	// Virtual geometry resource names are centralized here so RenderSystem,
	// tests, and diagnostics all describe the same scheduler handoff. The names
	// are plain strings because the scheduler validates declarations before any
	// renderer-specific type system is available during startup.
	inline constexpr const char kResourceVirtualGeometryQualityProfile[] = "VirtualGeometryQualityProfile";
	inline constexpr const char kResourceVirtualGeometryResidencySnapshots[] = "VirtualGeometryResidencySnapshots";
	inline constexpr const char kResourceVirtualGeometryPageResidencySnapshots[] = "VirtualGeometryPageResidencySnapshots";
	inline constexpr const char kResourceVirtualGeometryHiZValidationState[] = "VirtualGeometryHiZValidationState";
	inline constexpr const char kResourceVirtualGeometryClusterPackets[] = "VirtualGeometryClusterPackets";
	inline constexpr const char kResourceVirtualGeometryBatchPlans[] = "VirtualGeometryBatchPlans";
	inline constexpr const char kResourceVirtualGeometryGpuPacketFrameSets[] = "VirtualGeometryGpuPacketFrameSets";
	inline constexpr const char kResourceVirtualGeometryGpuCompactionDispatches[] = "VirtualGeometryGpuCompactionDispatches";
	inline constexpr const char kResourceVirtualGeometryGeneratedSubmissions[] = "VirtualGeometryGeneratedSubmissions";
	inline constexpr const char kResourceVirtualGeometryCounterReadbacks[] = "VirtualGeometryCounterReadbacks";
	inline constexpr const char kResourceVirtualGeometryCompactionResults[] = "VirtualGeometryCompactionResults";
	inline constexpr const char kResourceVirtualGeometryPagePayloads[] = "VirtualGeometryPagePayloads";
	inline constexpr const char kResourceVirtualGeometryPageRequests[] = "VirtualGeometryPageRequests";
	inline constexpr const char kResourceVirtualGeometryPageNeedPackets[] = "VirtualGeometryPageNeedPackets";
	inline constexpr const char kResourceVirtualGeometryPagePublications[] = "VirtualGeometryPagePublications";
	inline constexpr const char kResourceVirtualGeometryPageRetirements[] = "VirtualGeometryPageRetirements";
	inline constexpr const char kResourceVirtualGeometryPageDiagnostics[] = "VirtualGeometryPageDiagnostics";
	inline constexpr const char kResourceVirtualGeometryVisibilitySnapshots[] = "VirtualGeometryVisibilitySnapshots";
	inline constexpr const char kResourceVirtualGeometryCounterSnapshots[] = "VirtualGeometryCounterSnapshots";
	inline constexpr const char kResourceVirtualGeometryCullingDiagnostics[] = "VirtualGeometryCullingDiagnostics";
	inline constexpr const char kResourceVirtualGeometryPerformanceCounters[] = "VirtualGeometryPerformanceCounters";
	inline constexpr const char kResourceVirtualGeometryShadowVisibilitySnapshots[] = "VirtualGeometryShadowVisibilitySnapshots";
	inline constexpr const char kResourceVirtualGeometryDiagnostics[] = "VirtualGeometryDiagnostics";
	inline constexpr const char kResourceVisibleCommandReuseDiagnostics[] = "VisibleCommandReuseDiagnostics";
	inline constexpr const char kResourceVisibleStreamingTouchSets[] = "VisibleStreamingTouchSets";

	struct SystemDeclaration {
		std::string name;
		std::string phase = "PostFrame";
		std::vector<std::string> before;
		std::vector<std::string> after;
		std::vector<std::string> reads;
		std::vector<std::string> writes;
		bool mainThread = true;
		bool requiresFlushedSceneMutations = false;
		WorkerSystemPhaseKind phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
		HardBoundaryClassification boundaryClassification = HardBoundaryClassification::Protected;
		bool hardBoundary = false;
		std::string safeSubwork;
		std::string protectedCommit;
		std::string protectedReason;
		std::string fallbackReason;
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
		std::vector<std::string> componentQueryGroups;
		std::string ecsGraphSummary;
		std::vector<ScheduleDiagnostic> diagnostics;
		WorkerCoverageReport workerCoverageReport;
		TerraEngine::Scene::TransactionDiagnostics transactionDiagnostics;
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
		void SetLatestEcsGraphSummary(std::string summary);
		void SetLatestComponentQueryGroups(std::vector<std::string> queryGroups);
		void SetFrameWorkloadCompatibilityFallbackEnabled(bool enabled);
		bool FrameWorkloadCompatibilityFallbackEnabled() const;

		const ScheduleSummary& BuildSchedule();
		const ScheduleSummary& ExecuteFrame(float deltaTime, const MutationFlushCallback& flushCallback);
		std::vector<FrameWorkloadDeclaration> BuildFrameWorkloadDeclarations(float deltaTime);
		FrameExecutionPlan ExecuteFrameWorkloads(float deltaTime, const MutationFlushCallback& flushCallback = {});
		WorkerCoverageReport BuildWorkerCoverageReport(float deltaTime);
		const ScheduleSummary& LatestSummary() const;
		const FrameExecutionPlan& LatestFrameExecutionPlan() const;
		const std::vector<SystemDeclaration>& OrderedDeclarations() const;
		const std::vector<SchedulerPhaseDefinition>& PhaseDefinitions() const;
		const std::vector<MutationFlushPoint>& MutationFlushPoints() const;

	private:
		struct RegisteredSystem {
			SystemsBase* system = nullptr;
			SystemDeclaration declaration;
			std::size_t registrationIndex = 0;
		};

		std::vector<FrameWorkloadDeclaration> BuildSystemOwnedWorkloadDeclarations(SystemsBase& system, const SystemDeclaration& declaration, float deltaTime, std::size_t executionOrder) const;

		std::vector<SchedulerPhaseDefinition> phases;
		std::vector<SchedulerResourceDefinition> resources;
		std::vector<MutationFlushPoint> flushPoints;
		std::vector<RegisteredSystem> systems;
		std::vector<std::size_t> orderedIndices;
		std::vector<SystemDeclaration> orderedDeclarations;
		std::vector<std::string> latestComponentQueryGroups;
		std::string latestEcsGraphSummary;
		ScheduleSummary latestSummary;
		FrameExecutionPlan latestFrameExecutionPlan;
		bool frameWorkloadCompatibilityFallbackEnabled = true;
		bool dirty = true;
	};

}
