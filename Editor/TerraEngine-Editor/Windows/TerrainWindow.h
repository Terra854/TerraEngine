// TerrainWindow.h -- Visible create/import/sculpt controls for terrain assets.

#pragma once

#include "../Framework/EditorWindow.h"
#include "../Terrain/TerrainAuthoringSession.h"
#include "../Terrain/TerrainBrushEngine.h"
#include "../Terrain/TerrainSurfaceBrush.h"
#include "../Terrain/TerrainHoleBrush.h"
#include "../Terrain/TerrainProceduralJobs.h"
#include "../Terrain/TerrainAuthoringSystem.h"
#include "../Terrain/TerrainAutomationAdapter.h"
#include "../Terrain/TerrainDirtyState.h"
#include "../Terrain/TerrainPicking.h"

#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainDiagnostics.h"
#include "Terrain/TerrainGarbageCollector.h"
#include "Terrain/TerrainLayerStack.h"
#include "Renderer/Terrain/TerrainDirectMesh.h"
#include "Editor/AssetCompilers/TerrainCompiler.h"

#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class TerrainWindow final : public EditorWindow
{
public:
	struct AutomationOperationStart { bool accepted=false; std::string category,message; std::optional<TerraEngine::UUID> transactionId; };
	TerrainWindow();
	~TerrainWindow() override;
	[[nodiscard]] TerraEngine::Editor::Terrain::TerrainBrushSettings BrushSettings() const noexcept { return brushSettings; }
	[[nodiscard]] bool IsSculpting() const noexcept { return sculpting || holeSculpting; }
	void SetSculpting(bool value);
	[[nodiscard]] bool IsBrushStrokeActive() const noexcept { return brushStrokeActive; }
	[[nodiscard]] static constexpr bool ShouldCaptureSceneSelection(bool sculptEnabled, bool strokeActive) noexcept { return sculptEnabled && strokeActive; }
	// Scene View calls this only between pointer-down and pointer-up after a
	// canonical terrain pick succeeds; enabling the tool alone never blocks selection.
	void SetBrushStrokeActive(bool value) noexcept { brushStrokeActive = value; }
	// Scene View supplies a canonical ray and pointer lifecycle. This method
	// updates only detached local preview data; Apply owns publication.
	void HandleSceneViewPointer(
		const TerraEngine::Scene::WorldPosition& rayOrigin,
		const glm::dvec3& rayDirection,
		bool pressed, bool held, bool released);
	// Called from MainEditorSubsystem's protected editor boundary after widgets
	// have prepared requests. Returns a committed/rejected receipt to the UI.
	void ProcessProtectedAuthoringBoundary();
	[[nodiscard]] std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareAuthoringWorkloads(float deltaTime);
	[[nodiscard]] const std::string& Status() const noexcept { return status; }
	[[nodiscard]] TerraEngine::Editor::Terrain::TerrainAutomationAdapter& AutomationAdapter() noexcept { return automationAdapter; }
	[[nodiscard]] std::optional<TerraEngine::Terrain::TerrainEditTransaction> PrepareAutomationTransaction(const TerraEngine::Editor::Automation::TerrainAutomationRequest& request, std::string& outError);
	[[nodiscard]] AutomationOperationStart BeginAutomationOperation(const TerraEngine::Editor::Automation::TerrainAutomationRequest& request);
	// Installed live validation uses the same window-owned create, typed edit,
	// protected publication, diagnostics, and save paths as an interactive user.
	[[nodiscard]] nlohmann::json RunLiveValidationFixture(const std::filesystem::path& destination);
	// Scene loading restores runtime-only render resources from the durable
	// TerrainComponent reference. The mesh still builds on ThreadPool and only
	// publishes at this window's declared protected authoring boundary.
	[[nodiscard]] bool RestoreSceneTerrainProxy();
	// Inspector actions name an exact scene instance. Opening that instance here
	// binds editing, picking, persistence, and proxy updates to the same asset.
	[[nodiscard]] bool OpenTerrainForEditing(TerraEngine::UUID objectId, std::string& outError);
	[[nodiscard]] bool HasDetachedPreview() const noexcept { return authoringSession.IsDirty(); }
	// Lifecycle owners call this before a scene/project/play-mode/device boundary.
	// It only discards detached data, so an uncommitted brush can never mutate
	// a new scene or project after that boundary becomes visible.
	void CancelDetachedPreview(std::string_view reason);
	[[nodiscard]] bool IsTerrainDirty() const noexcept { return terrainDirtyState.IsDirty(); }
	void MarkTerrainSaved();
	[[nodiscard]] bool SaveTerrain(std::string& outError);
	// These value-only diagnostics are safe for the residency dashboard to read.
	// They never expose mutable terrain buffers outside this editor-owned window.
	[[nodiscard]] TerraEngine::Terrain::TerrainDiagnosticSnapshot DiagnosticSnapshot() const;
	// MainEditor consumes this one-shot value at its protected update boundary and
	// hands the canonical location to Scene View without coupling diagnostics to camera state.
	[[nodiscard]] std::optional<TerraEngine::Terrain::TerrainAreaCoordinate> ConsumeDiagnosticLocateRequest() noexcept;
	[[nodiscard]] bool IsSelectedAreaPinned() const noexcept { return selectedAreaPinned; }
	void SetSelectedAreaPinned(bool value) noexcept { selectedAreaPinned = value; }
	[[nodiscard]] bool RunBoundedGarbageCollection(std::size_t maximumRemovals, std::string& outMessage);
	[[nodiscard]] bool RepairFailedGeneration(std::string& outMessage);
	// Layer and surface previews are detached value objects. They make Apply and
	// Cancel testable without giving ImGui direct mutation access to terrain data.
	[[nodiscard]] const TerraEngine::Terrain::TerrainLayerStack& LayerPreview() const noexcept { return layerPreviewActive ? previewLayerStack : committedLayerStack; }
	[[nodiscard]] bool StageLayerPatch(const TerraEngine::Terrain::TerrainLayerPatch& patch, std::string& outError);
	[[nodiscard]] bool ApplyLayerPreview(std::string& outError);
	void CancelLayerPreview() noexcept;
	[[nodiscard]] TerraEngine::Editor::Terrain::TerrainSurfaceBrushResult PreviewSurfaceBrush(const TerraEngine::Editor::Terrain::TerrainSurfaceBrushSettings& settings, std::span<const std::uint8_t> mask = {});
	[[nodiscard]] bool ApplySurfacePreview(std::string& outError);
	void CancelSurfacePreview() noexcept;
	[[nodiscard]] bool PreviewHoleStamp(const TerraEngine::Editor::Terrain::TerrainHoleBrushStamp& stamp);
	[[nodiscard]] bool ApplyHolePreview(std::string& outError);
	void CancelHolePreview() noexcept;
	void ObserveTerrainMaterialChanges();
	[[nodiscard]] bool StartProceduralPreview(std::string& outError);
	[[nodiscard]] bool ApplyProceduralPreview(std::string& outError);
	void CancelProceduralPreview() noexcept;

protected:
	void OnGui() override;

private:
	void DrawCreateImportControls();
	void DrawSculptControls();
	void DrawSurfaceControls();
	void DrawProceduralControls();
	void DrawDiagnosticControls();
	[[nodiscard]] TerraEngine::Editor::Terrain::TerrainAutomationCommandResult ExecuteAutomationCommand(
		std::string_view capability, const nlohmann::json& arguments);
	void PollProceduralPreview();
	void ScheduleRenderProxyBuild();
	void PollRenderProxyBuild();
	void QueueRenderProxySceneInstance();
	void InitializeAuthoringTerrain(
		TerraEngine::UUID assetId,
		const TerraEngine::AssetPipeline::TerrainHeightfield& heightfield,
		TerraEngine::UUID existingObjectId = TerraEngine::EMPTY_UUID);
	[[nodiscard]] bool SynchronizeCommittedHeightFromPublishedRoot();
	void ApplyBrushAt(const TerraEngine::Editor::Terrain::TerrainPickHit& hit);
	void ApplyHoleBrushAt(const TerraEngine::Editor::Terrain::TerrainPickHit& hit);
	[[nodiscard]] bool QueuePreviewForPublication();
	[[nodiscard]] bool QueueSurfacePreviewForPublication(std::string& outError);
	[[nodiscard]] bool QueueHolePreviewForPublication(std::string& outError);
	TerraEngine::Editor::Terrain::TerrainBrushSettings brushSettings{};
	TerraEngine::Editor::Terrain::TerrainAuthoringSession authoringSession;
	std::filesystem::path sourcePath;
	std::filesystem::path destinationPath;
	float authoredSampleSpacingMetres = 1.0F;
	std::string status;
	int sourceFormat = 0;
	bool sculpting = false;
	bool brushStrokeActive = false;
	bool maskEnabled = false;
	bool holeSculpting = false;
	bool holeBrushActive = false;
	std::unique_ptr<TerraEngine::Terrain::TerrainAssetStore> terrainStore;
	std::unique_ptr<TerraEngine::Terrain::TerrainMutationQueue> terrainQueue;
	std::unique_ptr<TerraEngine::Terrain::TerrainPublication> terrainPublication;
	std::unique_ptr<TerraEngine::Terrain::TerrainGarbageCollector> terrainGarbageCollector;
	TerraEngine::Editor::Terrain::TerrainAuthoringSystem authoringSystem;
	TerraEngine::Editor::Terrain::TerrainAutomationAdapter automationAdapter;
	TerraEngine::Editor::Terrain::TerrainDirtyState terrainDirtyState;
	std::vector<std::int32_t> previewHeightMm;
	std::vector<std::int32_t> committedHeightMm;
	std::vector<std::uint8_t> brushMask;
	std::vector<std::uint8_t> committedHoleMask;
	std::vector<std::uint8_t> previewHoleMask;
	TerraEngine::Editor::Terrain::TerrainHoleBrushSettings holeBrushSettings{};
	std::optional<TerraEngine::Editor::Terrain::TerrainBrushStamp> previousStamp;
	bool applyRequested = false;
	bool selectedAreaPinned = false;
	std::string garbageCollectionStatus;
	std::string repairStatus;
	TerraEngine::Terrain::TerrainLayerStack committedLayerStack;
	TerraEngine::Terrain::TerrainLayerStack previewLayerStack;
	std::vector<TerraEngine::Terrain::TerrainWeightSample> committedSurfaceWeights;
	std::vector<TerraEngine::Terrain::TerrainWeightSample> previewSurfaceWeights;
	TerraEngine::Editor::Terrain::TerrainSurfaceBrushSettings surfaceBrushSettings{};
	std::uint16_t selectedLayerId = 1u;
	std::uint64_t observedTerrainMaterialGeneration = 0u;
	bool layerPreviewActive = false;
	bool surfacePreviewActive = false;
	std::uint16_t displacedLayerWarning = 0u;
	std::optional<TerraEngine::Terrain::TerrainLayerPatch> stagedLayerPatch;
	std::optional<TerraEngine::Terrain::TerrainLayerPatch> stagedInverseLayerPatch;
	std::optional<TerraEngine::UUID> queuedLayerTransactionId;
	std::optional<TerraEngine::UUID> queuedSurfaceTransactionId;
	std::optional<TerraEngine::UUID> queuedHoleTransactionId;
	bool holePreviewActive = false;
	TerraEngine::Editor::Terrain::TerrainProceduralJobs proceduralJobs;
	std::optional<TerraEngine::Editor::Terrain::TerrainProceduralPreview> proceduralPreview;
	std::optional<TerraEngine::UUID> queuedProceduralTransactionId;
	TerraEngine::Terrain::TerrainSelectionCriteria proceduralSelection{};
	// The operator picker feeds immutable core algorithms; all variants still
	// share one preview, transaction, history, and protected publish route.
	int proceduralOperation = 0;
	std::int32_t proceduralMinimumHeightMm = std::numeric_limits<std::int32_t>::min();
	std::int32_t proceduralHeightDeltaMm = 100;
	char diagnosticFilter[96]{};
	std::optional<TerraEngine::Terrain::TerrainAreaCoordinate> diagnosticLocateRequest;
	// Automation sessions are logical approval/history scopes. They never own a
	// mutable terrain buffer and commit only by queuing existing typed previews.
	std::optional<TerraEngine::UUID> automationSessionId;
	// Protected-boundary timings are bounded raw samples consumed by the
	// diagnostics percentile builder. They are never estimated from frame time.
	std::vector<std::uint64_t> publicationStageSamplesNanoseconds;
	// Direct-resident proxy generation is worker-owned. The protected authoring
	// boundary publishes only a completed mesh whose root identity still matches
	// the visible terrain generation.
	std::future<TerraEngine::Renderer::Terrain::TerrainDirectMeshResult> terrainRenderProxyFuture;
	std::shared_ptr<std::atomic_bool> terrainRenderProxyCancellation;
	TerraEngine::UUID terrainRenderProxyMeshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID terrainRenderProxyMaterialId = TerraEngine::EMPTY_UUID;
	// A restored scene supplies its existing object identity. A newly created
	// terrain keeps this empty so publication creates exactly one scene instance.
	TerraEngine::UUID terrainRenderProxyTargetObjectId = TerraEngine::EMPTY_UUID;
	bool terrainRenderProxyBuildActive = false;
	bool terrainRenderProxyRebuildRequested = false;
	bool terrainRenderProxyInstanceQueued = false;
};
