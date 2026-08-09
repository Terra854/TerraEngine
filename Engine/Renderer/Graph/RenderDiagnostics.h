// RenderDiagnostics.h - opt-in render threading summaries.

#pragma once

#include "RenderFrameBreakdown.h"
#include "RenderGraph.h"
#include "RenderWorkerTaskGroup.h"
#include "Core/Export.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/RendererRecovery.h"
#include "Profiling/PerformanceTelemetry.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Renderer {

	struct RenderFrameSnapshot;
	struct VirtualGeometryDrawRange;
	struct VirtualGeometryViewVisibilitySnapshot;

	struct RenderPassTiming
	{
		std::string passId;
		float milliseconds = 0.0f;
	};

	struct RenderBudgetDiagnostic
	{
		RenderWorkBudgetCategory category = RenderWorkBudgetCategory::DrawPreparation;
		std::size_t limit = 0u;
		std::size_t currentUsage = 0u;
		RenderBudgetOverflowPolicy overflowPolicy = RenderBudgetOverflowPolicy::Delay;
		bool overBudget = false;
		std::string label;
	};

	struct RenderPerformanceGateResult
	{
		std::string workloadName;
		float baselineValue = 0.0f;
		float latestValue = 0.0f;
		float thresholdPercent = 0.0f;
		bool passed = true;
	};

	struct VirtualGeometryBudgetContributor
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		RenderViewType viewType = RenderViewType::Game;
		std::uint64_t residentBytes = 0u;
		std::uint64_t requestedBytes = 0u;
		std::uint64_t reclaimableBytes = 0u;
		std::uint64_t pinnedFallbackBytes = 0u;
		// Per-asset buckets mirror the view summary so Resource Residency can
		// explain which bytes are high-detail pages versus protected fallback.
		std::uint64_t residentHighDetailBytes = 0u;
		std::uint64_t residentFallbackBytes = 0u;
		std::uint64_t embeddedFallbackBytes = 0u;
		std::uint64_t retiringBytes = 0u;
		std::uint64_t owningMeshFallbackBytes = 0u;
		std::uint32_t residentPages = 0u;
		std::uint32_t pinnedFallbackPages = 0u;
		std::uint32_t missingPages = 0u;
	};

	struct VirtualGeometryAdaptiveSettingsDiagnostics
	{
		VirtualGeometrySettingsSource settingsSource = VirtualGeometrySettingsSource::ProjectDefaults;
		VirtualGeometryMaterialSafetyPolicy materialSafetyPolicy = VirtualGeometryMaterialSafetyPolicy::Conservative;
		VirtualGeometryFallbackPolicy fallbackPolicy = VirtualGeometryFallbackPolicy::PreserveHighestSafeFallback;
		std::uint32_t requestedTargetCount = 0u;
		std::uint32_t minimumSourceTriangles = 0u;
		std::uint32_t minimumGeneratedTriangles = 0u;
		float minimumUsefulTriangleReduction = 0.0f;
		float minimumUsefulMeshletReduction = 0.0f;
	};

	struct VirtualGeometryCookOutputDiagnostics
	{
		std::uint32_t generatedLevelCount = 0u;
		std::uint32_t skippedDetailCount = 0u;
		std::uint32_t mergedDetailCount = 0u;
		std::uint32_t rejectedDetailCount = 0u;
		std::uint32_t clusterCount = 0u;
		std::uint32_t pageCount = 0u;
	};

	struct VirtualGeometryClusterSelectionDiagnostics
	{
		std::uint32_t selectedClusters = 0u;
		std::uint32_t fallbackClusters = 0u;
		std::uint32_t rejectedSelections = 0u;
		std::uint32_t parentChildConflictWarnings = 0u;
	};

	struct VirtualGeometryPageResidencyDiagnostics
	{
		// These byte buckets are exact live-state labels for developer tools:
		// resident bytes are renderable now, pending/failed/evicted bytes are
		// explanatory only, and owningMeshFallbackBytes marks legacy coverage.
		std::uint64_t residentHighDetailBytes = 0u;
		std::uint64_t residentFallbackBytes = 0u;
		std::uint64_t embeddedFallbackBytes = 0u;
		std::uint64_t owningMeshFallbackBytes = 0u;
		std::uint64_t metadataBytes = 0u;
		std::uint64_t requestedBytes = 0u;
		std::uint64_t pinnedFallbackBytes = 0u;
		std::uint64_t pendingIoBytes = 0u;
		std::uint64_t pendingUploadBytes = 0u;
		std::uint64_t failedBytes = 0u;
		std::uint64_t evictingBytes = 0u;
		std::uint64_t evictedBytes = 0u;
		std::uint64_t retiringBytes = 0u;
		std::uint32_t pendingIoPages = 0u;
		std::uint32_t pendingUploadPages = 0u;
		std::uint32_t evictingPages = 0u;
		std::uint32_t retiringPages = 0u;
		std::uint32_t pinnedFallbackPages = 0u;
		std::uint32_t failedPageReasons = 0u;
	};

	struct VirtualGeometryPageSourceDiagnostics
	{
		std::uint32_t residentPayloads = 0u;
		std::uint32_t highDetailPayloads = 0u;
		std::uint32_t fallbackPayloads = 0u;
		std::uint32_t embeddedFallbackPayloads = 0u;
		std::uint32_t emittedHighDetailSources = 0u;
		std::uint32_t emittedFallbackSources = 0u;
		std::uint32_t emittedEmbeddedFallbackSources = 0u;
		std::uint32_t missingSources = 0u;
		std::uint32_t emittedMaterialPagePairs = 0u;
		std::uint32_t emittedMultiMaterialPages = 0u;
		std::uint32_t pageRequests = 0u;
		std::uint32_t pageRetirements = 0u;
		std::uint64_t residentPayloadBytes = 0u;
		std::uint64_t fallbackPayloadBytes = 0u;
		std::uint64_t embeddedFallbackBytes = 0u;
	};

	struct VirtualGeometryHiZValidationDiagnostics
	{
		RenderViewType ownerViewType = RenderViewType::Game;
		bool sameViewHistory = false;
		bool sameTargetHistory = false;
		bool projectionMatches = false;
		bool failOpen = true;
		std::string inactiveReason;
	};

	struct VirtualGeometryViewDiagnosticsSnapshot
	{
		RenderViewType viewType = RenderViewType::Game;
		std::uint32_t viewId = 0u;
		std::uint64_t frameId = 0u;
		bool active = false;
		// The editor toolbar chooses a view-local false-color mode. Publishing
		// it here lets diagnostics and tests prove that mode changes reached the
		// next immutable frame snapshot instead of reading stale UI state.
		VirtualGeometryDebugVisualizationMode debugVisualizationMode = VirtualGeometryDebugVisualizationMode::None;
		VirtualGeometryInactiveReason inactiveReason = VirtualGeometryInactiveReason::None;
		std::uint32_t sourceMeshlets = 0u;
		std::uint32_t selectedMeshlets = 0u;
		std::uint32_t submittedMeshlets = 0u;
		std::uint32_t dispatchedMeshlets = 0u;
		std::uint32_t culledMeshlets = 0u;
		std::uint32_t culledByFrustum = 0u;
		std::uint32_t culledByCone = 0u;
		std::uint32_t culledByHiZ = 0u;
		std::uint32_t culledByResidency = 0u;
		std::uint32_t failOpenMeshlets = 0u;
		bool hiZActive = false;
		bool hiZHistoryValid = false;
		bool hiZBaselineMatched = true;
		std::uint32_t hiZInactiveFrames = 0u;
		std::uint32_t hiZFalseOcclusionRiskWarnings = 0u;
		std::string hiZInvalidReason;
		std::uint32_t residentPages = 0u;
		std::uint32_t requestedPages = 0u;
		std::uint32_t missingPages = 0u;
		std::uint32_t failedPages = 0u;
		std::uint32_t evictingPages = 0u;
		std::uint32_t evictedPages = 0u;
		std::uint32_t pinnedFallbackPages = 0u;
		VirtualGeometryCounterSource counterSource = VirtualGeometryCounterSource::Prepared;
		std::uint64_t counterPreparedFrameId = 0u;
		std::uint64_t counterReadbackFrameId = 0u;
		std::uint32_t counterReadbackLatencyFrames = 0u;
		std::string counterUnavailableReason;
		std::uint32_t counterToleranceMeshlets = 1u;
		float counterTolerancePercent = 1.0f;
		bool countersAuthoritative = false;
		VirtualGeometryHiZState hiZState = VirtualGeometryHiZState::MissingPyramid;
		glm::uvec2 hiZPyramidSize = glm::uvec2(0u);
		glm::uvec2 hiZRenderSize = glm::uvec2(0u);
		std::uint64_t hiZCameraSignature = 0u;
		std::uint64_t hiZDepthSceneSignature = 0u;
		std::uint32_t hiZWarmupFrames = 0u;
		std::uint32_t lodTransitionCandidates = 0u;
		std::uint32_t lodTransitionStates = 0u;
		std::uint32_t lodTransitionHeldSelections = 0u;
		std::uint32_t lodTransitionAcceptedSelections = 0u;
		std::uint32_t lodTransitionResets = 0u;
		std::uint32_t poppingRiskWarnings = 0u;
		float maxGeometricError = 0.0f;
		float effectiveLodErrorScale = 1.0f;
		float lodTransitionHysteresisPixels = 0.35f;
		std::uint32_t lodTransitionStableFrames = 3u;
		std::uint32_t configuredMaxLodDrop = 0u;
		std::uint64_t geometryBudgetBytes = 0u;
		std::uint64_t budgetPressureBytes = 0u;
		std::uint64_t estimatedGeometryBytes = 0u;
		std::uint32_t clusterPacketCount = 0u;
		// These counters describe the renderer-neutral packet routes that were
		// copied into this immutable view snapshot. Validation tooling can use
		// them to prove that Hybrid exercised the three real raster paths.
		std::uint32_t hardwareRasterPacketCount = 0u;
		std::uint32_t computeRasterPacketCount = 0u;
		std::uint32_t indexedRasterPacketCount = 0u;
		std::uint32_t candidateClusterCount = 0u;
		std::uint32_t compactedClusterCount = 0u;
		std::uint32_t rejectedClusterCount = 0u;
		std::uint32_t failOpenClusterCount = 0u;
		std::uint32_t submittedBatchCount = 0u;
		std::uint32_t singlePacketFallbackBatchCount = 0u;
		// GPU packet fields are copied from immutable frame snapshots. They let
		// tooling explain whether a frame used prepared CPU estimates, delayed
		// readback, generated submissions, or a fail-open fallback.
		std::uint32_t gpuPacketRecordCount = 0u;
		std::uint32_t gpuBatchDescriptorCount = 0u;
		std::uint32_t gpuPacketFrameSetCount = 0u;
		std::uint32_t gpuValidFrameSetCount = 0u;
		std::uint32_t gpuDelayedFrameSetCount = 0u;
		std::uint32_t gpuInvalidFrameSetCount = 0u;
		std::uint32_t gpuFailOpenFrameSetCount = 0u;
		std::uint32_t gpuCompactionDispatchCount = 0u;
		std::uint32_t generatedSubmissionCount = 0u;
		std::uint32_t validGeneratedSubmissionCount = 0u;
		std::uint32_t invalidGeneratedSubmissionCount = 0u;
		std::uint32_t fallbackGeneratedSubmissionCount = 0u;
		std::uint32_t gpuCounterReadbackCount = 0u;
		std::uint32_t gpuCounterReadbackLatencyFrames = 0u;
		std::uint32_t gpuFallbackRecordCount = 0u;
		std::uint32_t gpuFallbackAffectedPackets = 0u;
		std::uint32_t gpuFallbackAffectedBatches = 0u;
		std::string gpuFallbackReason;
		std::uint32_t residentPagePayloadCount = 0u;
		std::uint32_t emittedPageSourceCount = 0u;
		std::uint32_t embeddedFallbackPayloadCount = 0u;
		std::uint32_t pageRequestCount = 0u;
		std::uint32_t pageRetirementCount = 0u;
		// Animated page facts use the same immutable frame snapshot as ordinary
		// virtual-geometry diagnostics, so automation never races live arenas.
		std::uint32_t animatedPageDemandCount = 0u;
		std::uint32_t animatedUniqueDispatchCount = 0u;
		std::uint32_t animatedReuseCount = 0u;
		// These values describe the immutable deformation plan, not a speculative
		// live GPU readback. They let editor and automation distinguish a shared
		// compatible request from work rejected before command recording.
		std::vector<std::uint32_t> animatedAnimationLods;
		std::uint32_t animatedSkippedWorkCount = 0u;
		std::string animatedBoundsSource;
		std::uint32_t animatedFallbackCount = 0u;
		std::uint64_t deformationCurrentBytes = 0u;
		std::uint64_t deformationHistoryBytes = 0u;
		std::uint64_t deformationBoundsBytes = 0u;
		std::uint64_t deformationHighWaterBytes = 0u;
		std::uint32_t animatedDiagnosticEmitted = 0u;
		std::uint32_t animatedDiagnosticCoalesced = 0u;
		std::uint32_t animatedDiagnosticDropped = 0u;
		bool animatedShaderContractValid = true;
		std::string animatedFallbackReason;
		std::uint32_t shadowViewCount = 0u;
		VirtualGeometryAdaptiveSettingsDiagnostics adaptiveSettings;
		VirtualGeometryCookOutputDiagnostics cookOutput;
		VirtualGeometryClusterSelectionDiagnostics clusterSelection;
		VirtualGeometryPageResidencyDiagnostics pageResidency;
		VirtualGeometryPageSourceDiagnostics pageSources;
		VirtualGeometryHiZValidationDiagnostics hiZValidation;
		std::vector<OcclusionDebugSample> occlusionDebugSamples;
		std::vector<ShadowVisibilitySelection> shadowVisibilitySelections;
		std::vector<PerformanceGateResult> virtualGeometryPerformanceGates;
		std::vector<std::uint32_t> selectedLodHistogram;
		std::vector<VirtualGeometryBudgetContributor> budgetContributors;
		std::vector<std::string> warnings;
	};

	struct VirtualGeometrySchedulerDiagnostics
	{
		std::string owningSystem;
		std::string schedulerPhase;
		std::vector<std::string> readResources;
		std::vector<std::string> writeResources;
		std::string workerSafePreparation;
		std::string protectedBoundary;
		std::string visibilityBoundary;
	};

	struct RenderCaptureSchedulerDiagnostics
	{
		std::string owningSystem;
		std::string schedulerPhase;
		std::string sourceKind;
		std::vector<std::string> readResources;
		std::vector<std::string> writeResources;
		std::string workerSafePreparation;
		std::string protectedBoundary;
		std::string visibilityBoundary;
		bool backendReadback = false;
		bool deterministicFallback = false;
	};

	struct VirtualGeometryHiZDiagnosticsInput
	{
		RenderViewType viewType = RenderViewType::Game;
		RenderViewType ownerViewType = RenderViewType::Game;
		std::uint64_t frameId = 0u;
		bool qualityRequested = false;
		bool historyValid = false;
		bool hiZActive = false;
		bool sameViewHistory = false;
		bool sameTargetHistory = false;
		bool projectionMatches = false;
		bool failOpen = true;
		std::string invalidReason;
		std::uint32_t culledByFrustum = 0u;
		std::uint32_t culledByCone = 0u;
		std::uint32_t culledByHiZ = 0u;
		std::uint32_t falseOcclusionRiskWarnings = 0u;
	};

	enum class BackendResourceResidencyKind : std::uint8_t
	{
		Mesh,
		Texture,
		Other
	};

	struct BackendResourceResidencyRecord
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		BackendResourceResidencyKind kind = BackendResourceResidencyKind::Other;
		std::uint64_t gpuBytes = 0u;
		std::uint64_t exactGpuBytes = 0u;
		std::uint64_t allocationId = 0u;
		std::uint32_t pageId = 0xffffffffu;
		std::uint32_t subresourceCount = 0u;
		std::string byteStatus = "unavailable";
		std::string retirementState = "published";
		std::string unavailableReason;
		std::string detail;
	};

	struct BackendResourceResidencySnapshot
	{
		std::uint64_t frameId = 0u;
		std::vector<BackendResourceResidencyRecord> records;
	};

	struct RenderDiagnosticsSummary
	{
		std::uint64_t frameId = 0u;
		// These identities name the exact sealed root behind diagnostics, avoiding
		// ambiguity when a delayed backend receipt follows presentation recovery.
		std::uint64_t publicationGeneration = 0u;
		std::uint64_t rendererSessionGeneration = 0u;
		std::size_t workerPacketCount = 0u;
		std::size_t serialPacketCount = 0u;
		std::size_t skippedPacketCount = 0u;
		std::size_t failedPacketCount = 0u;
		float visibleFrameMilliseconds = 0.0f;
		float queuedBackgroundMilliseconds = 0.0f;
		std::vector<RenderPassTiming> topPassCosts;
		BackendFrameBreakdown backendFrameBreakdown;
		std::vector<RenderWorkerTaskGroup> renderWorkerGroups;
		std::vector<RenderBudgetDiagnostic> budgetDiagnostics;
		std::vector<RenderQueueReadiness> queueReadiness;
		std::vector<RenderPerformanceGateResult> performanceGates;
		std::vector<VirtualGeometryViewDiagnosticsSnapshot> virtualGeometryViews;
		VirtualGeometryCapabilityDiagnosticsSnapshot virtualGeometryCapabilityDiagnostics;
		std::vector<VirtualGeometryBudgetContributor> virtualGeometryBudgetContributors;
		std::vector<VirtualGeometrySchedulerDiagnostics> virtualGeometrySchedulers;
		std::vector<RenderCaptureSchedulerDiagnostics> captureSchedulers;
		std::size_t runtimeLayoutRenderRecordCount = 0u;
		std::size_t runtimeLayoutRejectedRecordCount = 0u;
		std::vector<std::string> runtimeLayoutRejectionReasons;
		// backendStalls contains only opt-in diagnostics so normal frames do
		// not pay for stall classification while the editor window is closed.
		std::vector<BackendStallDiagnostic> backendStalls;
		// viewSubmitWork explains whether scene and game views reused prepared
		// backend work or had to stay separate because state differed.
		std::vector<ViewSubmitWorkItem> viewSubmitWork;
		// presentationWaits contains typed, unavoidable platform/GPU waits. It is
		// separate from preventable forced-idle and recovery work so profiling
		// does not present synchronization mistakes as normal presentation cost.
		std::vector<TerraEngine::Profiling::PresentationWaitRecord> presentationWaits;
		std::vector<std::string> serializedReasons;
		std::vector<std::string> resourceWarnings;
		std::vector<std::string> backendFallbacks;
		std::vector<std::string> delayedWork;
		std::vector<std::string> cancelledWork;
		std::vector<std::string> degradedWork;
		// recentRendererPhases is a bounded CPU-side ring summary of backend
		// stages. Recovery diagnostics use it after device loss, when querying
		// live Vulkan state would be unsafe.
		std::vector<std::string> recentRendererPhases;
		// Renderer recovery is stored as plain CPU data so editor tooling can
		// keep explaining failures after Vulkan handles are no longer usable.
		RendererRecoverySnapshot rendererRecovery;
		RecoveryDiagnosticSnapshot rendererRecoveryDiagnostics;
		bool valid = true;
		bool detailed = false;
	};

	TERRAENGINE_API RenderBudgetDiagnostic BuildRenderBudgetDiagnostic(const RenderWorkBudget& budget);
	TERRAENGINE_API RenderPerformanceGateResult EvaluateRenderPerformanceGate(std::string workloadName, float baselineValue, float latestValue, float thresholdPercent);
	TERRAENGINE_API PerformanceGateResult EvaluateVirtualGeometryPerformanceGate(std::string gateId, std::string fixtureId, std::string metricName, float threshold, float actual, std::string unit, bool lowerIsBetter, VirtualGeometryPerformanceGateSource source = VirtualGeometryPerformanceGateSource::Prepared, RenderViewType viewType = RenderViewType::Game, std::uint32_t viewId = 0u);
	TERRAENGINE_API std::vector<PerformanceGateResult> BuildVirtualGeometryPerformanceGates(const VirtualGeometryViewDiagnosticsSnapshot& snapshot);
	TERRAENGINE_API std::vector<PerformanceGateResult> BuildVisibleCommandReusePerformanceGates(const RenderStatsSnapshot& stats);
	TERRAENGINE_API VirtualGeometryViewDiagnosticsSnapshot BuildInactiveVirtualGeometryDiagnostics(RenderViewType viewType, std::uint64_t frameId, VirtualGeometryInactiveReason reason);
	TERRAENGINE_API VirtualGeometryAdaptiveSettingsDiagnostics BuildVirtualGeometryAdaptiveSettingsDiagnostics(const AdaptiveVirtualGeometryCookProfile& profile);
	TERRAENGINE_API VirtualGeometryCookOutputDiagnostics BuildVirtualGeometryCookOutputDiagnostics(const std::vector<VirtualGeometryAssetRuntime>& assets);
	TERRAENGINE_API VirtualGeometryViewDiagnosticsSnapshot BuildDefaultAdaptiveVirtualGeometryDiagnostics(RenderViewType viewType, std::uint64_t frameId, const AdaptiveVirtualGeometryCookProfile& profile);
	TERRAENGINE_API void ApplyVirtualGeometryAdaptiveDiagnostics(VirtualGeometryViewDiagnosticsSnapshot& snapshot, VirtualGeometryAdaptiveSettingsDiagnostics settings, VirtualGeometryCookOutputDiagnostics cookOutput, VirtualGeometryClusterSelectionDiagnostics clusterSelection, VirtualGeometryPageResidencyDiagnostics pageResidency, VirtualGeometryHiZValidationDiagnostics hiZValidation);
	TERRAENGINE_API VirtualGeometryViewDiagnosticsSnapshot BuildVirtualGeometryLodQualityDiagnostics(RenderViewType viewType, std::uint64_t frameId, const std::vector<VirtualGeometryAssetRuntime>& assets, const VirtualGeometryQualityProfile& qualityProfile);
	TERRAENGINE_API VirtualGeometryViewDiagnosticsSnapshot BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(const VirtualGeometryViewVisibilitySnapshot& visibility);
	TERRAENGINE_API std::vector<VirtualGeometryViewDiagnosticsSnapshot> BuildVirtualGeometryViewDiagnosticsFromSnapshot(
		const RenderFrameSnapshot& snapshot,
		const VirtualGeometryQualityProfile& qualityProfile,
		VirtualGeometryInactiveReason inactiveReason,
		const GpuDeformationPlan* completedDeformationPlan = nullptr,
		bool includeDetailedEvidence = true);
	TERRAENGINE_API VirtualGeometryViewDiagnosticsSnapshot BuildVirtualGeometryHiZDiagnostics(const VirtualGeometryHiZDiagnosticsInput& input);
	TERRAENGINE_API void AddVirtualGeometryFailOpenDiagnostic(RenderDiagnosticsSummary& summary, const TerraEngine::UUID& meshId, const VirtualGeometryDrawRange& range);
	TERRAENGINE_API VirtualGeometrySchedulerDiagnostics BuildVirtualGeometrySchedulerDiagnostics(std::string owningSystem, std::string schedulerPhase, std::vector<std::string> readResources, std::vector<std::string> writeResources, std::string workerSafePreparation, std::string protectedBoundary, std::string visibilityBoundary);
	TERRAENGINE_API void ApplyVirtualGeometryDiagnostics(RenderDiagnosticsSummary& summary, std::vector<VirtualGeometryViewDiagnosticsSnapshot> viewSnapshots, std::vector<VirtualGeometrySchedulerDiagnostics> schedulerSnapshots);
	TERRAENGINE_API VirtualGeometryGpuTimingSnapshot BuildVirtualGeometryGpuTimingSnapshot(const VirtualGeometryMeasurementRecord& measurement, const VirtualGeometryFrameViewIdentity& publicationIdentity);
	TERRAENGINE_API void ApplyVirtualGeometryGpuTimingDiagnostics(RenderDiagnosticsSummary& summary, std::vector<VirtualGeometryMeasurementRecord> measurements, const VirtualGeometryFrameViewIdentity& publicationIdentity);
	TERRAENGINE_API void ApplyVirtualGeometryCapabilityDiagnostics(RenderDiagnosticsSummary& summary, VirtualGeometryCapabilityDiagnosticsSnapshot diagnostics);
	TERRAENGINE_API RenderCaptureSchedulerDiagnostics BuildRenderCaptureSchedulerDiagnostics(std::string owningSystem, std::string schedulerPhase, std::string sourceKind, bool backendReadback, bool deterministicFallback);
	TERRAENGINE_API std::optional<TerraEngine::Profiling::PresentationWaitKind> ClassifyUnavoidablePresentationWait(BackendSubmitStage stage);
	TERRAENGINE_API std::vector<TerraEngine::Profiling::PresentationWaitRecord> BuildPresentationWaitRecords(const BackendFrameBreakdown& breakdown);
	TERRAENGINE_API void ApplyBackendBreakdownToRenderDiagnostics(RenderDiagnosticsSummary& summary, const BackendFrameBreakdown& breakdown);
	TERRAENGINE_API void ApplyRendererRecoveryDiagnostics(RenderDiagnosticsSummary& summary, RendererRecoverySnapshot snapshot);
	TERRAENGINE_API bool RenderDiagnosticsRequested();
	TERRAENGINE_API void SetRenderDiagnosticsRequested(bool requested);
	TERRAENGINE_API void ClearPublishedRenderDiagnostics();
	TERRAENGINE_API void PublishRenderDiagnostics(RenderDiagnosticsSummary summary);
	TERRAENGINE_API RenderDiagnosticsSummary LatestRenderDiagnostics();
	TERRAENGINE_API std::vector<VirtualGeometryViewDiagnosticsSnapshot> LatestVirtualGeometryDiagnosticsSnapshots();
	TERRAENGINE_API void PublishBackendResourceResidencySnapshot(BackendResourceResidencySnapshot snapshot);
	TERRAENGINE_API BackendResourceResidencySnapshot LatestBackendResourceResidencySnapshot();

}
