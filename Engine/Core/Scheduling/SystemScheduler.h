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
#include <cstdint>
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

	// Animation crosses scripting, physics, presentation, rendering, and managed
	// delivery. Stable resource names make every immutable handoff visible in the
	// scheduler summary instead of hiding ownership behind a broad ObjectManager
	// declaration. Later evaluator stages can refine payload types without
	// changing these diagnostics-facing contract names.
	inline constexpr const char kResourceAnimatorComponents[] = "AnimatorComponents";
	inline constexpr const char kResourceAnimationCommandQueues[] = "AnimationCommandQueues";
	inline constexpr const char kResourceAnimationFrameInputs[] = "AnimationFrameInputs";
	inline constexpr const char kResourceAnimationEvaluationPrograms[] = "AnimationEvaluationPrograms";
	inline constexpr const char kResourceAnimationResidencyCapabilities[] = "AnimationResidencyCapabilities";
	inline constexpr const char kResourceAnimationExecutionModes[] = "AnimationExecutionModes";
	// Processing classes are separate from execution modes: a GPU visual model
	// can still require complete CPU work when an authoritative consumer exists.
	inline constexpr const char kResourceAnimationProcessingClasses[] = "AnimationProcessingClasses";
	inline constexpr const char kResourceAnimationConsumerMasks[] = "AnimationConsumerMasks";
	inline constexpr const char kResourceAnimationWorkPackets[] = "AnimationWorkPackets";
	inline constexpr const char kResourceAnimationVisualPackets[] = "AnimationVisualPackets";
	inline constexpr const char kResourceAnimationGpuVisualFastPackets[] = "AnimationGpuVisualFastPackets";
	inline constexpr const char kResourceAnimationGpuVisualFastPrepared[] = "AnimationGpuVisualFastPrepared";
	inline constexpr const char kResourceAnimationGpuTopology[] = "AnimationGpuTopology";
	inline constexpr const char kResourceAnimationOptimizationEvidence[] = "AnimationOptimizationEvidence";
	inline constexpr const char kResourceAnimationClipSegments[] = "AnimationClipSegments";
	inline constexpr const char kResourceAnimationRollbackState[] = "AnimationRollbackState";
	inline constexpr const char kResourceAnimationAuthoritativeOutputs[] = "AnimationAuthoritativeOutputs";
	inline constexpr const char kResourceAnimationRuntimeState[] = "AnimationRuntimeState";
	inline constexpr const char kResourceAnimationPhysicsHandoffQueue[] = "AnimationPhysicsHandoffQueue";
	inline constexpr const char kResourceAnimationManagedCallbackBatches[] = "AnimationManagedCallbackBatches";
	inline constexpr const char kResourceAnimationRagdollMappings[] = "AnimationRagdollMappings";
	inline constexpr const char kResourceAnimationRecordingSamples[] = "AnimationRecordingSamples";
	inline constexpr const char kResourceAnimationPresentationPrepared[] = "AnimationPresentationPrepared";
	inline constexpr const char kResourceAnimationPresentationPlan[] = "AnimationPresentationPlan";
	// The authority root is the pre-physics gameplay publication. Presentation
	// is a distinct post-physics product that strongly references that exact
	// authority generation; neither name represents a compatibility path.
	inline constexpr const char kResourceAnimationAuthorityPages[] = "AnimationAuthorityPages";
	inline constexpr const char kResourceAnimationFrameSidecars[] = "AnimationFrameSidecars";
	inline constexpr const char kResourceAnimationAuthorityFrame[] = "AnimationAuthorityFrame";
	inline constexpr const char kResourceAnimationEvaluationCache[] = "AnimationEvaluationCache";
	inline constexpr const char kResourceAnimationPresentationFrame[] = "AnimationPresentationFrame";

	// Virtual geometry resource names are centralized here so RenderSystem,
	// tests, and diagnostics all describe the same scheduler handoff. The names
	// are plain strings because the scheduler validates declarations before any
	// renderer-specific type system is available during startup.
	inline constexpr const char kResourceVirtualGeometryQualityProfile[] = "VirtualGeometryQualityProfile";
	inline constexpr const char kResourceSceneChangePublication[] = "SceneChangePublication";
	inline constexpr const char kResourcePersistentRenderSceneState[] = "PersistentRenderSceneState";
	inline constexpr const char kResourceDynamicRenderFrameState[] = "DynamicRenderFrameState";
	inline constexpr const char kResourceRenderViewSelections[] = "RenderViewSelections";
	inline constexpr const char kResourceRenderFramePublication[] = "RenderFramePublication";
	inline constexpr const char kResourceProfilerFramePublicationEvidence[] = "ProfilerFramePublicationEvidence";
	// Retained topology is immutable across compatible generations. Workers write
	// only frame-owned sidecars and groups before protected backend publication.
	inline constexpr const char kResourcePersistentRenderTopology[] = "PersistentRenderTopology";
	inline constexpr const char kResourceDynamicRenderInstanceCandidates[] = "DynamicRenderInstanceCandidates";
	inline constexpr const char kResourceRenderCompatibilityGroups[] = "RenderCompatibilityGroups";
	inline constexpr const char kResourceRenderInstanceStreamPublication[] = "RenderInstanceStreamPublication";
	inline constexpr const char kResourceCachedRenderViewEvidence[] = "CachedRenderViewEvidence";
	inline constexpr const char kResourceRenderOptimizationEvidence[] = "RenderOptimizationEvidence";
	inline constexpr const char kResourceAnimatedObjectVisibilitySnapshot[] = "AnimatedObjectVisibilitySnapshot";
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
	inline constexpr const char kResourcePhysicalVirtualGeometryPagePayloads[] = "PhysicalVirtualGeometryPagePayloads";
	inline constexpr const char kResourceExactResidencySnapshots[] = "ExactResidencySnapshots";
	inline constexpr const char kResourceBackendGpuAllocationSnapshots[] = "BackendGpuAllocationSnapshots";
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
	inline constexpr const char kResourceAnimatedGeometryPageDemands[] = "AnimatedGeometryPageDemands";
	inline constexpr const char kResourceAnimatedGeometryDeformationPlans[] = "AnimatedGeometryDeformationPlans";
	inline constexpr const char kResourceAnimatedGeometryOutputs[] = "AnimatedGeometryOutputs";
	inline constexpr const char kResourceAnimatedGeometryDynamicBounds[] = "AnimatedGeometryDynamicBounds";
	inline constexpr const char kResourceAnimatedGeometryPublications[] = "AnimatedGeometryPublications";
	inline constexpr const char kResourceAnimatedGeometryRetirements[] = "AnimatedGeometryRetirements";
	// Managed gameplay publishes copied runtime rendering requests into this
	// queue; RenderSystem owns the effective settings publication at its boundary.
	inline constexpr const char kResourceRuntimeRenderSettingsQueue[] = "RuntimeRenderSettingsQueue";
	inline constexpr const char kResourceRuntimeRenderSettings[] = "RuntimeRenderSettings";
	inline constexpr const char kResourceVisibleStreamingTouchSets[] = "VisibleStreamingTouchSets";
	inline constexpr const char kResourceRendererRecoveryStateSnapshot[] = "RendererRecoveryStateSnapshot";
	inline constexpr const char kResourceRendererDeviceIncidentLog[] = "RendererDeviceIncidentLog";

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
		// Converted systems can intentionally return no descriptors when their
		// bounded membership check finds no useful work. Legacy systems leave this
		// false so an empty declaration still receives the Update() compatibility
		// wrapper instead of disappearing from the frame accidentally.
		bool omitWhenNoFrameWorkloads = false;
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
		// These fields explain whether the optional protected After Physics
		// boundary is active. Diagnostics can therefore distinguish an omitted
		// no-op flush from a missing publication boundary.
		bool afterPhysicsFlushActive = false;
		std::string afterPhysicsConsumer;
		std::string afterPhysicsReason;
		// The immutable dependency topology is reused only after exact descriptor,
		// generation, activation, and order checks. These counters keep that hot-path
		// decision visible in scheduler diagnostics.
		bool frameTopologyCacheHit = false;
		std::uint64_t frameTopologyCacheHitCount = 0u;
		std::uint64_t frameTopologyCacheMissCount = 0u;
		std::string formatted;
	};

	struct MutationFlushRuntimeState {
		bool flushAttempted = true;
		std::size_t pendingAfterFlush = 0;
		std::string message;
	};

	using MutationFlushCallback = std::function<MutationFlushRuntimeState(const MutationFlushPoint&)>;

	// Physics publishes transforms at After Physics only when a later declared
	// system consumes flushed scene state in the same frame. Headless or reduced
	// schedules use Frame End and avoid an otherwise empty protected boundary.
	struct SameFramePhysicsPublicationState {
		bool active = false;
		std::string consumerSystem;
		std::string reason;
	};

	void SetSameFramePhysicsPublicationConsumerActive(bool active) noexcept;
	void SetSameFramePhysicsPublicationState(SameFramePhysicsPublicationState state);
	[[nodiscard]] bool SameFramePhysicsPublicationConsumerActive() noexcept;
	[[nodiscard]] SameFramePhysicsPublicationState SameFramePhysicsPublicationStateSnapshot();

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
		// Direct diagnostic callers retain detailed output by default. ExecuteFrame
		// selects the compact path because it runs every Play Mode frame.
		FrameExecutionPlan ExecuteFrameWorkloads(
			float deltaTime,
			const MutationFlushCallback& flushCallback = {},
			bool compactRuntimeDiagnostics = false);
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
			// The cache owns only stable descriptor metadata. Per-frame callbacks,
			// activation, readiness, estimates, and immutable inputs are rebound
			// without rebuilding resource/dependency declarations.
			std::vector<FrameWorkloadDeclaration> stableWorkloadDescriptors;
			std::uint64_t stableDescriptorGeneration = 0;
		};

		struct FrameTopologyCacheEntry {
			std::string stableDescriptorId;
			std::string phase;
			std::string ownerSystem;
			std::uint64_t stableDescriptorGeneration = 0u;
			std::size_t executionOrder = 0u;
			bool active = false;
			bool ready = false;
			bool sameFrameVisibilityRequired = false;
		};

		struct FrameTopologyCache {
			bool valid = false;
			std::vector<FrameTopologyCacheEntry> entries;
			std::vector<std::vector<std::string>> loweredDependencies;
			std::uint64_t hitCount = 0u;
			std::uint64_t missCount = 0u;
		};

		std::vector<FrameWorkloadDeclaration> BuildSystemOwnedWorkloadDeclarations(RegisteredSystem& registeredSystem, float deltaTime, std::size_t executionOrder);
		SameFramePhysicsPublicationState ResolveSameFramePhysicsPublicationState() const;

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
		FrameTopologyCache frameTopologyCache;
		bool frameWorkloadCompatibilityFallbackEnabled = true;
		bool dirty = true;
	};

}
