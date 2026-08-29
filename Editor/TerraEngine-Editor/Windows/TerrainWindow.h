// TerrainWindow.h -- Inspector-backed terrain authoring state and operations.

#pragma once

#include "../Terrain/TerrainAuthoringSession.h"
#include "../Terrain/TerrainBrushEngine.h"
#include "../Terrain/TerrainSurfaceBrush.h"
#include "../Terrain/TerrainHoleBrush.h"
#include "../Terrain/TerrainProceduralJobs.h"
#include "../Terrain/TerrainAuthoringSystem.h"
#include "../Terrain/TerrainAutomationAdapter.h"
#include "../Terrain/TerrainDirtyState.h"
#include "../Terrain/TerrainPicking.h"
#include "../Terrain/TerrainResizeJobs.h"

#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainDiagnostics.h"
#include "Terrain/TerrainGarbageCollector.h"
#include "Terrain/TerrainLayerStack.h"
#include "Terrain/TerrainMaterial.h"
#include "Renderer/Terrain/TerrainDirectMesh.h"
#include "Editor/AssetCompilers/TerrainCompiler.h"

#include <atomic>
#include <array>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The historical type name is retained to avoid disrupting terrain automation
// and lifecycle integrations. It is deliberately not an EditorWindow: the
// TerrainComponent Inspector owns all interactive terrain presentation.
class TerrainWindow final
{
public:
	struct AutomationOperationStart { bool accepted=false; std::string category,message; std::optional<TerraEngine::UUID> transactionId; };
	// Scene View renders this canonical footprint through its own camera. Keeping
	// the radius in metres prevents zoom from changing the apparent brush coverage.
	struct SceneViewBrushFootprint
	{
		TerraEngine::Scene::WorldPosition center{};
		float radiusMetres = 0.0F;
		bool holeBrush = false;
	};
	TerrainWindow();
	~TerrainWindow();
	[[nodiscard]] TerraEngine::Editor::Terrain::TerrainBrushSettings BrushSettings() const noexcept { return brushSettings; }
	[[nodiscard]] bool IsSculpting() const noexcept { return sculpting || surfacePainting || holeSculpting; }
	[[nodiscard]] bool IsHeightSculpting() const noexcept { return sculpting; }
	[[nodiscard]] bool IsSurfacePainting() const noexcept { return surfacePainting; }
	[[nodiscard]] bool IsHoleSculpting() const noexcept { return holeSculpting; }
	void SetSculpting(bool value);
	void SetHoleSculpting(bool value);
	[[nodiscard]] bool IsBrushStrokeActive() const noexcept { return brushStrokeActive || surfaceBrushActive || holeBrushActive; }
	[[nodiscard]] const std::optional<SceneViewBrushFootprint>& BrushFootprint() const noexcept { return sceneViewBrushFootprint; }
	// Terrain input belongs to this window only while the exact scene instance
	// opened for editing is selected. Comparing durable UUIDs avoids accidentally
	// editing the old terrain after the user selects another object.
	[[nodiscard]] bool IsEditingTargetSelected(const std::optional<TerraEngine::UUID>& selectedObjectId) const noexcept;
	[[nodiscard]] static constexpr bool ShouldCaptureSceneSelection(
		bool sculptEnabled, bool editingTargetSelected) noexcept
	{
		return sculptEnabled && editingTargetSelected;
	}
	// Selection changes can happen outside Scene View while a drag is active.
	// Canceling the detached work prevents that partial stroke from being committed
	// after the terrain is no longer selected.
	void HandleEditingTargetDeselected();
	void SetBrushStrokeActive(bool value) noexcept { brushStrokeActive = value; }
	// Scene View supplies a canonical ray and pointer lifecycle. A release turns
	// the detached stroke into one typed, undoable transaction automatically.
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
	// MainEditor calls this when the last scene component referencing an authored
	// asset disappears. Tool-owned jobs and store pointers are detached together.
	void ReleaseSceneTerrain(TerraEngine::UUID assetId);
	// Project shutdown also invalidates generated resource UUIDs even when the
	// same terrain asset is loaded again in the next project session.
	void ReleaseProjectTerrainState();
	// Inspector actions name an exact scene instance. Opening that instance here
	// binds editing, picking, persistence, and proxy updates to the same asset.
	[[nodiscard]] bool OpenTerrainForEditing(TerraEngine::UUID objectId, std::string& outError);
	// MainEditor calls this once per protected editor update. Terrain authoring is
	// selected-object driven: choosing a Terrain component opens its asset and
	// activates the last inspector tool without a separate enable checkbox.
	void SynchronizeEditingSelection(const std::optional<TerraEngine::UUID>& selectedTerrainObjectId);
	// InspectorWindow owns presentation while this class continues to own terrain
	// transactions, previews, jobs, persistence, and Scene View brush state.
	void DrawInspectorControls(TerraEngine::UUID selectedTerrainObjectId);
	// Terrain height and holes are their own collision source. Play Mode calls this
	// after creating a physics world, while committed edits call it automatically;
	// users never need to add or configure a separate collider component.
	[[nodiscard]] bool QueueAutomaticTerrainCollision(std::string& outError);
	[[nodiscard]] bool HasDetachedPreview() const noexcept { return authoringSession.IsDirty(); }
	[[nodiscard]] const TerraEngine::Terrain::TerrainResizePreview* ActiveResizePreview() const noexcept
	{
		return resizePreview ? &resizePreview->preview : nullptr;
	}
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
	// Material Inspector edits bypass the delayed filesystem watcher and ask any
	// open terrain using this UUID to republish only its small material binding.
	void RefreshMaterialAsset(TerraEngine::UUID materialId);
	[[nodiscard]] bool PreviewHoleStamp(const TerraEngine::Editor::Terrain::TerrainHoleBrushStamp& stamp);
	[[nodiscard]] bool ApplyHolePreview(std::string& outError);
	void CancelHolePreview();
	void ObserveTerrainMaterialChanges();
	[[nodiscard]] bool StartProceduralPreview(std::string& outError);
	[[nodiscard]] bool ApplyProceduralPreview(std::string& outError);
	void CancelProceduralPreview() noexcept;

private:
	enum class InspectorTool : std::uint8_t
	{
		Sculpt,
		Paint,
		Holes,
		Layers,
		Settings
	};
	void ApplyInspectorToolMode();
	void DrawInspectorToolPalette();
	void DrawInspectorSculptControls();
	void DrawInspectorPaintControls();
	void DrawInspectorHoleControls();
	void DrawInspectorLayerControls();
	void DrawInspectorSettingsControls();
	void DrawCreateImportControls();
	void DrawSculptControls();
	void DrawMaterialControls();
	void DrawSurfaceControls();
	void DrawProceduralControls();
	void DrawResizeControls();
	void DrawDiagnosticControls();
	void PollResizePreview();
	[[nodiscard]] bool StartResizePreview(TerraEngine::Terrain::TerrainResizeKind kind, std::string& outError);
	[[nodiscard]] bool CommitResizePreview(bool confirmDataLoss, std::string& outError);
	[[nodiscard]] TerraEngine::Editor::Terrain::TerrainAutomationCommandResult ExecuteAutomationCommand(
		std::string_view capability, const nlohmann::json& arguments);
	void PollProceduralPreview();
	void ScheduleRenderProxyBuild();
	void PollRenderProxyBuild();
	void ResetSceneTerrainState(std::string_view statusMessage);
	void EnsureValidPaintLayer() noexcept;
	void SynchronizeTerrainMaterial(bool forceLayerMaterialRefresh = false);
	void QueueRenderProxySceneInstance();
	void InitializeAuthoringTerrain(
		TerraEngine::UUID assetId,
		const TerraEngine::AssetPipeline::TerrainHeightfield& heightfield,
		TerraEngine::UUID existingObjectId = TerraEngine::EMPTY_UUID,
		std::optional<TerraEngine::Terrain::TerrainSampleBounds> authoredBounds = std::nullopt);
	// Undo and redo publish immutable terrain roots without rebuilding the detached
	// editor buffers. Mirror every authored channel so the next tool operation
	// starts from the replayed value instead of overwriting it with stale state.
	[[nodiscard]] bool SynchronizeCommittedAuthoringStateFromPublishedRoot();
	void ApplyBrushAt(const TerraEngine::Editor::Terrain::TerrainPickHit& hit);
	void ApplySurfaceBrushAt(const TerraEngine::Editor::Terrain::TerrainPickHit& hit);
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
	bool editingSelectionActive = false;
	InspectorTool inspectorTool = InspectorTool::Sculpt;
	bool brushStrokeActive = false;
	bool maskEnabled = false;
	bool surfacePainting = false;
	bool surfaceBrushActive = false;
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
	std::map<TerraEngine::Terrain::TerrainAreaCoordinate, std::vector<std::int32_t>> committedAreaHeightMm;
	// Height sculpting can cross area boundaries in one detached session. The
	// map holds only copied canonical area buffers and is published as one typed
	// multi-area transaction on release.
	std::map<TerraEngine::Terrain::TerrainAreaCoordinate, std::vector<std::int32_t>> previewAreaHeightMm;
	TerraEngine::Terrain::TerrainAreaCoordinate selectedTerrainArea{};
	std::vector<std::uint8_t> brushMask;
	std::vector<std::uint8_t> committedHoleMask;
	std::vector<std::uint8_t> previewHoleMask;
	// Hole and surface mirrors are area-keyed for the same reason as height:
	// a single brush footprint may cross a signed area boundary and must publish
	// all changed channels in one protected transaction.
	std::map<TerraEngine::Terrain::TerrainAreaCoordinate, std::vector<std::uint8_t>> committedAreaHoleMasks;
	std::map<TerraEngine::Terrain::TerrainAreaCoordinate, std::vector<std::uint8_t>> previewAreaHoleMasks;
	TerraEngine::Editor::Terrain::TerrainHoleBrushSettings holeBrushSettings{};
	std::optional<TerraEngine::Editor::Terrain::TerrainBrushStamp> previousStamp;
	std::optional<TerraEngine::Editor::Terrain::TerrainSurfaceBrushStamp> previousSurfaceStamp;
	std::optional<SceneViewBrushFootprint> sceneViewBrushFootprint;
	bool applyRequested = false;
	bool selectedAreaPinned = false;
	std::string garbageCollectionStatus;
	std::string repairStatus;
	TerraEngine::Terrain::TerrainLayerStack committedLayerStack;
	TerraEngine::Terrain::TerrainLayerStack previewLayerStack;
	std::vector<TerraEngine::Terrain::TerrainWeightSample> committedSurfaceWeights;
	std::vector<TerraEngine::Terrain::TerrainWeightSample> previewSurfaceWeights;
	std::map<TerraEngine::Terrain::TerrainAreaCoordinate, std::vector<TerraEngine::Terrain::TerrainWeightSample>> committedAreaSurfaceWeights;
	std::map<TerraEngine::Terrain::TerrainAreaCoordinate, std::vector<TerraEngine::Terrain::TerrainWeightSample>> previewAreaSurfaceWeights;
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
	bool proceduralAutoApplyRequested = false;
	TerraEngine::Terrain::TerrainSelectionCriteria proceduralSelection{};
	// The operator picker feeds immutable core algorithms; all variants still
	// share one preview, transaction, history, and protected publish route.
	int proceduralOperation = 0;
	std::int32_t proceduralMinimumHeightMm = std::numeric_limits<std::int32_t>::min();
	std::int32_t proceduralHeightDeltaMm = 100;
	TerraEngine::Editor::Terrain::TerrainResizeJobs resizeJobs;
	std::optional<TerraEngine::Editor::Terrain::TerrainPreparedResize> resizePreview;
	std::optional<TerraEngine::UUID> queuedResizeTransactionId;
	std::array<int, 4u> resizeExpansionAreas{0, 1, 0, 0};
	std::array<std::int64_t, 4u> resizeCropBounds{0, 0, 256, 256};
	int resizeExpansionPolicy = static_cast<int>(TerraEngine::Terrain::TerrainExpansionPolicy::EdgeBlend);
	int resizeHolePolicy = static_cast<int>(TerraEngine::Terrain::TerrainBoundaryHolePolicy::CloseAtFormerEdge);
	int resizeTransitionSamples = 64;
	bool resizeCropAcknowledged = false;
	char diagnosticFilter[96]{};
	std::optional<TerraEngine::Terrain::TerrainAreaCoordinate> diagnosticLocateRequest;
	// Automation sessions are logical approval/history scopes. They never own a
	// mutable terrain buffer and commit only by queuing existing typed previews.
	std::optional<TerraEngine::UUID> automationSessionId;
	// Protected-boundary timings are bounded raw samples consumed by the
	// diagnostics percentile builder. They are never estimated from frame time.
	std::vector<std::uint64_t> publicationStageSamplesNanoseconds;
	// Direct-resident proxy generation is worker-owned. Detached preview meshes
	// may be published at the protected boundary for responsive sculpt feedback,
	// but only terrain transactions may change durable roots, collision, or history.
	std::future<TerraEngine::Renderer::Terrain::TerrainDirectMeshResult> terrainRenderProxyFuture;
	std::shared_ptr<std::atomic_bool> terrainRenderProxyCancellation;
	TerraEngine::UUID terrainRenderProxyMeshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID terrainRenderProxyMaterialId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID terrainWeightTextureId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID terrainLayerIdTextureId = TerraEngine::EMPTY_UUID;
	std::optional<TerraEngine::Terrain::TerrainMaterialSettings> publishedTerrainMaterial;
	TerraEngine::Terrain::ContentHash publishedSurfaceControlHash{};
	TerraEngine::Terrain::Generation publishedTerrainLayerGeneration = 0u;
	TerraEngine::UUID publishedTerrainLayerMaterialId = TerraEngine::EMPTY_UUID;
	bool publishedTerrainLayerSourceAvailable = false;
	// A restored scene supplies its existing object identity. A newly created
	// terrain keeps this empty so publication creates exactly one scene instance.
	TerraEngine::UUID terrainRenderProxyTargetObjectId = TerraEngine::EMPTY_UUID;
	bool terrainRenderProxyBuildActive = false;
	bool terrainRenderProxyBuildWasPreview = false;
	bool terrainRenderProxyRebuildRequested = false;
	bool terrainRenderProxyInstanceQueued = false;
};
