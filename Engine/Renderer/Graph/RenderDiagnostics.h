// RenderDiagnostics.h - opt-in render threading summaries.

#pragma once

#include "RenderFrameBreakdown.h"
#include "RenderWorkerTaskGroup.h"
#include "RenderSubmissionPlan.h"
#include "Core/Export.h"
#include "Renderer/RenderTypes.h"

#include <atomic>
#include <cstdint>
#include <mutex>
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
		std::uint64_t requestedBytes = 0u;
		std::uint64_t pinnedFallbackBytes = 0u;
		std::uint64_t pendingIoBytes = 0u;
		std::uint64_t pendingUploadBytes = 0u;
		std::uint64_t failedBytes = 0u;
		std::uint64_t evictingBytes = 0u;
		std::uint64_t evictedBytes = 0u;
		std::uint32_t pendingIoPages = 0u;
		std::uint32_t pendingUploadPages = 0u;
		std::uint32_t evictingPages = 0u;
		std::uint32_t pinnedFallbackPages = 0u;
		std::uint32_t failedPageReasons = 0u;
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
		std::uint64_t frameId = 0u;
		bool active = false;
		// The editor toolbar chooses a view-local false-color mode. Publishing
		// it here lets diagnostics and tests prove that mode changes reached the
		// next immutable frame snapshot instead of reading stale UI state.
		VirtualGeometryDebugVisualizationMode debugVisualizationMode = VirtualGeometryDebugVisualizationMode::None;
		VirtualGeometryInactiveReason inactiveReason = VirtualGeometryInactiveReason::None;
		std::uint32_t sourceMeshlets = 0u;
		std::uint32_t selectedMeshlets = 0u;
		std::uint32_t dispatchedMeshlets = 0u;
		std::uint32_t culledMeshlets = 0u;
		std::uint32_t culledByFrustum = 0u;
		std::uint32_t culledByCone = 0u;
		std::uint32_t culledByHiZ = 0u;
		bool hiZActive = false;
		bool hiZHistoryValid = false;
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
		VirtualGeometryAdaptiveSettingsDiagnostics adaptiveSettings;
		VirtualGeometryCookOutputDiagnostics cookOutput;
		VirtualGeometryClusterSelectionDiagnostics clusterSelection;
		VirtualGeometryPageResidencyDiagnostics pageResidency;
		VirtualGeometryHiZValidationDiagnostics hiZValidation;
		std::vector<OcclusionDebugSample> occlusionDebugSamples;
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

	struct RenderDiagnosticsSummary
	{
		std::uint64_t frameId = 0u;
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
		std::vector<std::string> serializedReasons;
		std::vector<std::string> resourceWarnings;
		std::vector<std::string> backendFallbacks;
		std::vector<std::string> delayedWork;
		std::vector<std::string> cancelledWork;
		std::vector<std::string> degradedWork;
		bool valid = true;
		bool detailed = false;
	};

	TERRAENGINE_API RenderBudgetDiagnostic BuildRenderBudgetDiagnostic(const RenderWorkBudget& budget);
	TERRAENGINE_API RenderPerformanceGateResult EvaluateRenderPerformanceGate(std::string workloadName, float baselineValue, float latestValue, float thresholdPercent);
	TERRAENGINE_API VirtualGeometryViewDiagnosticsSnapshot BuildInactiveVirtualGeometryDiagnostics(RenderViewType viewType, std::uint64_t frameId, VirtualGeometryInactiveReason reason);
	TERRAENGINE_API VirtualGeometryAdaptiveSettingsDiagnostics BuildVirtualGeometryAdaptiveSettingsDiagnostics(const AdaptiveVirtualGeometryCookProfile& profile);
	TERRAENGINE_API VirtualGeometryCookOutputDiagnostics BuildVirtualGeometryCookOutputDiagnostics(const std::vector<VirtualGeometryAssetRuntime>& assets);
	TERRAENGINE_API VirtualGeometryViewDiagnosticsSnapshot BuildDefaultAdaptiveVirtualGeometryDiagnostics(RenderViewType viewType, std::uint64_t frameId, const AdaptiveVirtualGeometryCookProfile& profile);
	TERRAENGINE_API void ApplyVirtualGeometryAdaptiveDiagnostics(VirtualGeometryViewDiagnosticsSnapshot& snapshot, VirtualGeometryAdaptiveSettingsDiagnostics settings, VirtualGeometryCookOutputDiagnostics cookOutput, VirtualGeometryClusterSelectionDiagnostics clusterSelection, VirtualGeometryPageResidencyDiagnostics pageResidency, VirtualGeometryHiZValidationDiagnostics hiZValidation);
	TERRAENGINE_API VirtualGeometryViewDiagnosticsSnapshot BuildVirtualGeometryLodQualityDiagnostics(RenderViewType viewType, std::uint64_t frameId, const std::vector<VirtualGeometryAssetRuntime>& assets, const VirtualGeometryQualityProfile& qualityProfile);
	TERRAENGINE_API VirtualGeometryViewDiagnosticsSnapshot BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(const VirtualGeometryViewVisibilitySnapshot& visibility);
	TERRAENGINE_API std::vector<VirtualGeometryViewDiagnosticsSnapshot> BuildVirtualGeometryViewDiagnosticsFromSnapshot(const RenderFrameSnapshot& snapshot, const VirtualGeometryQualityProfile& qualityProfile, VirtualGeometryInactiveReason inactiveReason);
	TERRAENGINE_API VirtualGeometryViewDiagnosticsSnapshot BuildVirtualGeometryHiZDiagnostics(const VirtualGeometryHiZDiagnosticsInput& input);
	TERRAENGINE_API void AddVirtualGeometryFailOpenDiagnostic(RenderDiagnosticsSummary& summary, const TerraEngine::UUID& meshId, const VirtualGeometryDrawRange& range);
	TERRAENGINE_API VirtualGeometrySchedulerDiagnostics BuildVirtualGeometrySchedulerDiagnostics(std::string owningSystem, std::string schedulerPhase, std::vector<std::string> readResources, std::vector<std::string> writeResources, std::string workerSafePreparation, std::string protectedBoundary, std::string visibilityBoundary);
	TERRAENGINE_API void ApplyVirtualGeometryDiagnostics(RenderDiagnosticsSummary& summary, std::vector<VirtualGeometryViewDiagnosticsSnapshot> viewSnapshots, std::vector<VirtualGeometrySchedulerDiagnostics> schedulerSnapshots);
	TERRAENGINE_API RenderCaptureSchedulerDiagnostics BuildRenderCaptureSchedulerDiagnostics(std::string owningSystem, std::string schedulerPhase, std::string sourceKind, bool backendReadback, bool deterministicFallback);
	TERRAENGINE_API void ApplyBackendBreakdownToRenderDiagnostics(RenderDiagnosticsSummary& summary, const BackendFrameBreakdown& breakdown);
	TERRAENGINE_API bool RenderDiagnosticsRequested();
	TERRAENGINE_API void SetRenderDiagnosticsRequested(bool requested);
	TERRAENGINE_API void ClearPublishedRenderDiagnostics();
	TERRAENGINE_API void PublishRenderDiagnostics(RenderDiagnosticsSummary summary);
	TERRAENGINE_API RenderDiagnosticsSummary LatestRenderDiagnostics();
	TERRAENGINE_API std::vector<VirtualGeometryViewDiagnosticsSnapshot> LatestVirtualGeometryDiagnosticsSnapshots();
	TERRAENGINE_API RenderDiagnosticsSummary BuildRenderDiagnosticsSummary(
		const RenderFrameSnapshot& snapshot,
		const RenderWorkPacketPlan& packetPlan,
		const RenderSubmissionPlan& submissionPlan,
		const BackendCapabilityProfile& capabilities);

}
