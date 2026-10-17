#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Core/EditorSchedulerDeclaration.h"
#include "Editor/TerraEngine-Editor/Profiling/ProfilerDiagnosticAnalysis.h"
#include "Editor/TerraEngine-Editor/Profiling/ProfilerSampleHierarchy.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		// Source-oriented editor tests use repository text because these policies
		// describe when expensive work is allowed to run, not its numeric result.
		std::string ReadRepositorySource(const std::string& path)
		{
			const auto read = [](const std::string& candidate) {
				std::ifstream file(candidate, std::ios::binary);
				if (!file)
				{
					return std::string();
				}
				std::ostringstream buffer;
				buffer << file.rdbuf();
				return buffer.str();
			};

			std::string text = read(path);
			if (!text.empty())
			{
				return text;
			}
			text = read(std::string("../../../") + path);
			if (!text.empty())
			{
				return text;
			}
			const std::string thisFile = __FILE__;
			const auto marker = thisFile.find("Tests\\Editor\\Profiling\\ProfilerWindowTests.cpp");
			if (marker != std::string::npos)
			{
				return read(thisFile.substr(0u, marker) + path);
			}
			const auto slashMarker = thisFile.find("Tests/Editor/Profiling/ProfilerWindowTests.cpp");
			if (slashMarker != std::string::npos)
			{
				return read(thisFile.substr(0u, slashMarker) + path);
			}
			return {};
		}

		std::string ReadProfilerWindowSource()
		{
			return ReadRepositorySource("Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp");
		}

		std::string ReadProfilerSource()
		{
			return ReadRepositorySource("Engine/Profiling/Profiler.cpp");
		}

		std::string ReadEditorProfilerServiceSource()
		{
			return ReadRepositorySource("Editor/TerraEngine-Editor/Profiling/EditorProfilerService.cpp");
		}

		std::size_t CountOccurrences(const std::string& source, const std::string& token)
		{
			std::size_t count = 0u;
			std::size_t cursor = 0u;
			while ((cursor = source.find(token, cursor)) != std::string::npos)
			{
				++count;
				cursor += token.size();
			}
			return count;
		}
	}

	void RegisterProfilerWindowTests(TestRegistry& registry)
	{
		registry.Add("ProfilerWindow.CompletedSessionCache.DefersDetailedRebuild", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			context.Expect(source.find("sampleAnalysisValid") != std::string::npos
				&& source.find("selectedFrame") != std::string::npos,
				"Completed-session detail must remain cached until selection changes.");
		});
		registry.Add("ProfilerWindow.FramePublicationEvidence.DisplaysGenerationsAndOutcomes", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			context.Expect(source.find("Frame Publication Evidence") != std::string::npos
				&& source.find("Generations:") != std::string::npos
				&& source.find("Outcomes:") != std::string::npos
				&& source.find("Animation evaluation") != std::string::npos
				&& source.find("Backend preparation") != std::string::npos
				&& source.find("Uninstrumented remainder") != std::string::npos
				&& source.find("Reconciliation:") != std::string::npos,
				"Profiler window must display frame publication generations, outcomes, and disjoint wall-time reconciliation.");
		});
		registry.Add("ProfilerWindow.SampleHierarchy.ValidatesParentsAndRebuildsPresentationDepth", [](TestContext& context) {
			using TerraEngine::EditorProfiling::BuildProfilerSampleHierarchy;
			using TerraEngine::EditorProfiling::ProfilerSampleDisplayName;
			using TerraEngine::Profiling::ProfilerFrameRecord;
			using TerraEngine::Profiling::ProfilerModuleId;
			using TerraEngine::Profiling::ProfilerSample;

			auto sample = [](std::uint64_t id, std::uint64_t parentId, const char* name,
				const char* task, const char* thread, double startMs, double durationMs, std::uint32_t capturedDepth) {
				ProfilerSample value;
				value.sampleId = id;
				value.parentSampleId = parentId;
				value.moduleId = ProfilerModuleId::CpuEngine;
				value.name = name;
				value.taskLabel = task;
				value.threadLabel = thread;
				value.startMs = startMs;
				value.durationMs = durationMs;
				value.depth = capturedDepth;
				return value;
			};

			ProfilerFrameRecord frame;
			// Deliberately shuffle input order and captured depths. Parent IDs and
			// validated intervals, not serialization order, must define the UI tree.
			frame.samples.push_back(sample(2u, 1u, "Child", "", "Main", 1.0, 4.0, 9u));
			frame.samples.push_back(sample(1u, 0u, "Root", "", "Main", 0.0, 10.0, 7u));
			frame.samples.push_back(sample(3u, 1u, "JobGraph::ProtectedJob", "RenderSystem.RenderSnapshot", "Main", 3.0, 4.0, 12u));
			frame.samples.push_back(sample(4u, 1u, "Wrong Thread", "", "Worker", 2.0, 1.0, 20u));

			const auto hierarchy = BuildProfilerSampleHierarchy(frame, ProfilerModuleId::CpuEngine);
			auto findNode = [&hierarchy](std::uint64_t sampleId) -> const TerraEngine::EditorProfiling::ProfilerSampleHierarchyNode& {
				const auto found = std::find_if(hierarchy.nodes.begin(), hierarchy.nodes.end(), [&hierarchy, sampleId](const auto& node) {
					return hierarchy.samples[node.sampleIndex].sampleId == sampleId;
				});
				return *found;
			};

			context.ExpectEqual(std::uint32_t(0u), findNode(1u).presentationDepth,
				"A validated parent should be presented as a root regardless of captured depth.");
			context.ExpectEqual(std::uint32_t(1u), findNode(2u).presentationDepth,
				"A valid child should be indented exactly one rebuilt level below its parent.");
			context.ExpectEqual(std::uint32_t(0u), findNode(4u).presentationDepth,
				"A cross-thread parent link should be rejected and presented as a root.");
			context.ExpectEqual(std::string("RenderSystem.RenderSnapshot"),
				ProfilerSampleDisplayName(hierarchy.samples[findNode(3u).sampleIndex]),
				"Generic scheduler wrappers should display their workload label.");
			context.ExpectEqual(4.0, findNode(1u).selfDurationMs,
				"Overlapping child intervals should be merged before calculating parent self time.");
		});

		registry.Add("ProfilerWindow.Diagnostics.BuildsTruthfulContextAndCoverage", [](TestContext& context) {
			using namespace TerraEngine::EditorProfiling;
			using namespace TerraEngine::Profiling;
			ProfilerFrameRecord frame;
			frame.durationMs = 10.0;
			frame.samples = {
				{1u, 0u, ProfilerModuleId::CpuEngine, "Engine::Update", "991", "", 0.0, 10.0, 0u, 1u, true},
				{2u, 1u, ProfilerModuleId::CpuEngine, "JobGraph::ProtectedJob", "991", "RenderSystem", 1.0, 4.0, 1u, 1u, true},
				{3u, 2u, ProfilerModuleId::CpuEngine, "Render Child", "991", "", 2.0, 1.0, 2u, 1u, true}
			};
			WorkloadExecutionRecord execution;
			execution.ownerSystem = "RenderSystem";
			execution.workloadName = "RenderSystem";
			execution.phase = "Presentation";
			execution.selectedPath = WorkloadExecutionPath::Protected;
			frame.workloadExecutions.push_back(execution);

			const ProfilerDiagnosticAnalysis analysis = BuildProfilerDiagnosticAnalysis(frame, ProfilerModuleId::CpuEngine);
			context.ExpectEqual(std::size_t(3u), analysis.rows.size(), "Every validated module scope should produce one diagnostic row.");
			context.ExpectEqual(std::string("Main Thread"), analysis.rows[0].threadLabel,
				"The Engine::Update lane should use a readable main-thread name while retaining its raw ID.");
			context.ExpectEqual(std::string("Presentation"), analysis.rows[1].phase,
				"A scheduler workload scope should inherit the typed execution phase.");
			context.ExpectEqual(std::string("Protected"), analysis.rows[1].executionPath,
				"A scheduler workload scope should expose its actual execution path.");
			context.ExpectEqual(10.0, analysis.capturedWallMs,
				"Nested scopes should merge into elapsed wall coverage instead of inflating it.");
			context.ExpectEqual(10.0, analysis.cpuWorkMs,
				"Nested scopes on one thread should contribute CPU work only once.");
			context.Expect(ProfilerDiagnosticRowMatches(analysis.rows[1], "present", 0.0),
				"Diagnostic search should match phase metadata case-insensitively.");
			context.Expect(!ProfilerDiagnosticRowMatches(analysis.rows[2], "", 2.0),
				"Minimum-duration filtering should remove low-cost noise.");
		});

		registry.Add("ProfilerWindow.Diagnostics.LargeFrameAnalysisStaysInteractive", [](TestContext& context) {
			using namespace TerraEngine::EditorProfiling;
			using namespace TerraEngine::Profiling;
			ProfilerFrameRecord frame;
			frame.durationMs = 20.0;
			frame.samples.reserve(10001u);
			frame.samples.push_back({1u, 0u, ProfilerModuleId::CpuEngine, "Engine::Update", "main-id", "", 0.0, 20.0, 0u, 1u, true});
			for (std::uint64_t index = 0u; index < 10000u; ++index)
			{
				const double startMs = static_cast<double>(index) * 0.0019;
				frame.samples.push_back({index + 2u, 1u, ProfilerModuleId::CpuEngine,
					"Small Scope", "main-id", "", startMs, 0.001, 1u, 1u, true});
			}

			const auto started = std::chrono::steady_clock::now();
			const ProfilerDiagnosticAnalysis analysis = BuildProfilerDiagnosticAnalysis(frame, ProfilerModuleId::CpuEngine);
			const auto elapsed = std::chrono::steady_clock::now() - started;
			context.ExpectEqual(std::size_t(10001u), analysis.rows.size(),
				"Large detailed captures should retain every diagnostic row in one cached analysis.");
			context.Expect(elapsed < std::chrono::seconds(2),
				"A ten-thousand-scope frame should analyze within an interactive, non-frame-loop budget.");
		});

		registry.Add("ProfilerWindow.Toolbar.GroupsActionsAndExplainsDisabledStates", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();

			context.Expect(source.find("DrawToolbarGroupLabel(\"Live\")") != std::string::npos, "Profiler toolbar should group live capture controls.");
			context.Expect(source.find("DrawToolbarGroupLabel(\"Captures\")") != std::string::npos, "Profiler toolbar should group capture-management controls.");
			context.Expect(source.find("DrawToolbarGroupLabel(\"Status\")") != std::string::npos, "Profiler toolbar should expose state separately from actions.");
			context.Expect(source.find("DrawDisabledReasonTooltip") != std::string::npos, "Disabled toolbar actions should explain why they are unavailable.");
			context.Expect(source.find("ToolbarButton(\"Clear\"") != std::string::npos, "Destructive clear action should remain separate from live capture controls.");
		});

		registry.Add("ProfilerWindow.TimingStreams.SeparateProfilerTimingFromRendererDiagnostics", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			const std::string profilerSource = ReadProfilerSource();

			context.Expect(source.find("ProfilerTimingStreams") != std::string::npos, "Frame details should show timing streams.");
			context.Expect(source.find("firstEngineCpuWindowMedianNanoseconds") != std::string::npos
				&& source.find("finalEngineCpuWindowP95Nanoseconds") != std::string::npos,
				"Profiler UI should use persisted bounded timing-window summaries.");
			context.Expect(source.find("TimingAggregateValue") != std::string::npos && source.find("return \"n/a\"") != std::string::npos, "Unavailable timing aggregates should display n/a instead of fake zeroes.");
			context.Expect(profilerSource.find("Viewport GPU") != std::string::npos && profilerSource.find("viewport_gpu") != std::string::npos, "GPU timing streams should be consolidated into one viewport row.");
			context.Expect(source.find("only Play Mode frames are captured") != std::string::npos, "Frame overview should state that armed recording ignores editor-preview frames.");
			context.Expect(source.find("Frame = Engine CPU") != std::string::npos, "Frame overview should state that profiler frame timing is game-facing Engine CPU time.");
			context.Expect(source.find("Frame = Engine CPU + Editor Wall") == std::string::npos, "Frame overview must not present editor cost as part of game profiling.");
			context.Expect(source.find("Inclusive timings are nested") != std::string::npos, "Top Offenders should warn that inclusive scope values overlap.");
			context.Expect(profilerSource.find("\"Game frame time\"") == std::string::npos && profilerSource.find("\"Scene frame time\"") == std::string::npos, "Delayed renderer input deltas must not appear as selected-frame profiler timing metrics.");
			context.Expect(source.find("ProfilerVirtualGeometryPacketRows") == std::string::npos, "Profiler should not duplicate VG packet tables owned by Resource Residency.");
			context.Expect(source.find("ProfilerVirtualGeometryPageSourceRows") == std::string::npos, "Profiler should not duplicate VG page-source tables owned by Resource Residency.");
		});

		registry.Add("ProfilerWindow.V1Evidence.PresentsRecoveryAndTypedStreams", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			const std::size_t frameDetails = source.find("void ProfilerWindow::DrawFrameDetails()");
			const std::size_t selectionGuard = source.find("if (!selectedFrame.has_value())", frameDetails);
			const std::size_t captureEvidenceDraw = source.find("DrawCaptureEvidenceSummary();", selectionGuard);

			context.Expect(source.find("DrawCaptureEvidenceSummary") != std::string::npos, "Profiler Window should render capture-level V1 evidence separately from a selected frame.");
			context.Expect(frameDetails != std::string::npos
				&& selectionGuard != std::string::npos
				&& captureEvidenceDraw != std::string::npos
				&& selectionGuard < captureEvidenceDraw,
				"Capture evidence should remain hidden until a profiler session supplies a selected frame.");
			context.Expect(source.find("detailLevel") != std::string::npos, "Profiler Window should identify the detailed, sampled, or aggregate capture level.");
			context.Expect(source.find("Recovery warning") != std::string::npos, "Recovered incomplete capture tails should remain visible to the user.");
			context.Expect(source.find("Physics, Scene View, and Editor Stages") != std::string::npos, "Profiler Window should distinguish native physics, Scene View, and Editor stages.");
			context.Expect(source.find("Protected Boundary Breakdown") != std::string::npos, "Profiler Window should break protected queue, ownership, flush, and publication waits apart.");
			context.Expect(source.find("Worker Timeline Summary") != std::string::npos, "Profiler Window should present bounded worker history summaries.");
			context.Expect(source.find("Renderer Optimization Summary") != std::string::npos,
				"Whole-capture evidence should aggregate grouped submissions, reuse, invalidation, rejection, and fallbacks.");
			context.Expect(source.find("Physics Work Volumes") != std::string::npos, "Profiler Window should present processed-versus-published body volumes.");
			context.Expect(source.find("Renderer Optimization Evidence") != std::string::npos
				&& source.find("grouped + %u fallback") != std::string::npos
				&& source.find("Counts are captured with this frame") != std::string::npos,
				"A selected frame should expose conserved grouped/fallback renderer evidence rather than live renderer state.");
			context.Expect(source.find("unavailable") != std::string::npos, "Unavailable typed streams should be labelled instead of shown as zero.");
		});

		registry.Add("ProfilerWindow.Recording.KeepsUnlimitedDetailedCaptureAndDefersFramePresentation", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			const std::string serviceSource = ReadEditorProfilerServiceSource();

			context.Expect(serviceSource.find("detailLevel = TerraEngine::Profiling::ProfilerCaptureDetailLevel::Detailed") != std::string::npos
				&& serviceSource.find("Thread-owned fixed buffers") != std::string::npos,
				"The ordinary Record action should retain every-frame scope evidence through bounded thread-owned buffers.");
			context.Expect(serviceSource.find("recordMarkers = true") != std::string::npos
				&& serviceSource.find("taskExecutionEnabled = true") != std::string::npos
				&& serviceSource.find("recordTaskContext = true") != std::string::npos,
				"The detailed toolbar capture should retain marker and task context needed to build useful scope trees.");
			context.Expect(serviceSource.find("settings.maxFrames = 0u") != std::string::npos
				&& source.find("Record Every Frame") != std::string::npos,
				"The every-frame toolbar action should clearly identify itself and retain the complete detailed history.");
			context.Expect(serviceSource.find("no shared profiler mutex") != std::string::npos
				&& serviceSource.find("reports drops") != std::string::npos,
				"The service should document why every-frame detailed capture keeps producer work bounded.");
			context.Expect(source.find("if (captureActive)") != std::string::npos
				&& source.find("Frame history will appear after the recording is finished.") != std::string::npos,
				"The Frames subwindow should not refresh or expose rows until recording has finished.");
			context.Expect(source.find("service->GetPresentationSnapshot()") != std::string::npos
				&& source.find("journalSummary.has_value()") != std::string::npos,
				"The V1 window must use the bounded presentation summary instead of copying retained frame history.");
		});

		registry.Add("ProfilerWindow.OpenFramePath.UsesCachedCompactSnapshots", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();

			context.Expect(source.find("kProfilerWindowRefreshInterval") != std::string::npos
				&& source.find("service->GetPresentationSnapshot()") != std::string::npos
				&& source.find("now >= cache->nextProfilerRefresh") != std::string::npos,
				"The open profiler should poll a cheap revision and rate-limit deep presentation snapshots.");
			context.Expect(source.find("presentation->indexPage") != std::string::npos
				&& source.find("service->RequestSelectedFrame") != std::string::npos,
				"The profiler window should consume one compact V1 index page and request only the selected detail.");
			context.Expect(source.find("const std::vector<ProfilerFrameListItem>& displayedFrames = frameIndexOrder ? frames : sortedFrames") != std::string::npos
				&& source.find("if (!frameIndexOrder && (cache->frameOrderDirty || sortSpecs->SpecsDirty))") != std::string::npos,
				"The default frame-index order should traverse the chronological snapshot directly and sort only explicit timing orders.");
			const std::size_t pinGuard = source.find("cache->pinnedFrameIndex.has_value()");
			context.Expect(pinGuard != std::string::npos
				&& source.find("Frame analysis will become available after the recording is finished.") != std::string::npos,
				"Detailed frame inspection should remain unavailable until the recording is finalized.");
			context.ExpectEqual(static_cast<std::size_t>(1u), CountOccurrences(source, "BuildProfilerDiagnosticAnalysis(frame, selectedModule)"),
				"Diagnosis, offenders, hierarchy, and timeline should share one cached frame analysis.");
			context.Expect(source.find("GetLatestRenderGraphDiagnostics()") == std::string::npos
				&& source.find("GetLatestBackendStageTimings()") != std::string::npos
				&& source.find("GetLatestRendererRecoverySnapshot()") != std::string::npos,
				"The profiler should read compact renderer fields instead of copying the complete diagnostics graph.");
			context.Expect(source.find("journalSummary.has_value()") != std::string::npos
				&& source.find("V1 journal:") != std::string::npos
				&& source.find("timingDistributionValid") != std::string::npos
				&& source.find("Refresh Timing Distribution") != std::string::npos
				&& source.find("timingDistributionFrameIndex") == std::string::npos,
				"Expanded whole-capture disclosures should reuse cached analysis until the user requests a refresh.");
			context.Expect(source.find("TreeNodeEx(\"Sample Hierarchy\", ImGuiTreeNodeFlags_DefaultOpen)") != std::string::npos
				&& source.find("TreeNodeEx(\"Top Offenders\", ImGuiTreeNodeFlags_DefaultOpen)") != std::string::npos
				&& source.find("GetBool(openStateId, true)") != std::string::npos,
				"Primary diagnostic disclosures and every nested sample parent should open immediately.");
			context.Expect(source.find("ImGui::TreeNode(") == std::string::npos
				&& source.find("TreeNodeEx(\"Physics Counters\", ImGuiTreeNodeFlags_DefaultOpen)") != std::string::npos
				&& source.find("TreeNodeEx(\"Renderer Optimization Evidence\", ImGuiTreeNodeFlags_DefaultOpen)") != std::string::npos
				&& source.find("TreeNodeEx(\"Scripting Dispatch Counters\", ImGuiTreeNodeFlags_DefaultOpen)") != std::string::npos
				&& source.find("TreeNodeEx(\"Backend Submit Path\", ImGuiTreeNodeFlags_DefaultOpen)") != std::string::npos,
				"Every profiler disclosure should default open when a capture session is presented.");
		});

		registry.Add("ProfilerWindow.IdlePresentation.SkipsEngineRevisionChurnAndEmptyDiagnosticTables", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			const std::string serviceSource = ReadEditorProfilerServiceSource();
			const std::size_t idleRevisionGuard = source.find("!presentationChanged && !stateChanged && !liveDataChanged");
			const std::size_t emptyFrameGuard = source.find("if (frames.empty())");
			const std::size_t emptyFrameReturn = source.find("return;", emptyFrameGuard);
			const std::size_t frameRowsTable = source.find("BeginTable(\"ProfilerFrameRows\"");
			const std::size_t frameDetails = source.find("void ProfilerWindow::DrawFrameDetails()");
			const std::size_t selectionGuard = source.find("if (!selectedFrame.has_value())", frameDetails);
			const std::size_t rendererSummary = source.find("DrawRendererRecoverySummary();", frameDetails);

			context.Expect(serviceSource.find("GetPresentationRevision") != std::string::npos
				&& serviceSource.find("presentationRevision.fetch_add") != std::string::npos,
				"The profiler service should publish a semantic presentation revision separate from engine frame activity.");
			context.Expect(idleRevisionGuard != std::string::npos
				&& source.find("presentation->indexPage") != std::string::npos,
				"Idle engine revisions should update the cheap cache marker without copying a completed capture index.");
			context.Expect(source.find("service->GetPresentationSnapshot()") != std::string::npos
				&& source.find("cache->captures = presentation->captures") != std::string::npos
				&& source.find("ImGuiListClipper captureClipper") != std::string::npos
				&& source.find("const auto& capture = cache->captures[") != std::string::npos
				&& source.find("service->LoadCapture(capture.absolutePath)") != std::string::npos,
				"Capture navigation should reuse revision-cached metadata, virtualize saved-session rows, and keep sessions loadable without per-frame copies.");
			context.Expect(emptyFrameGuard != std::string::npos && emptyFrameReturn != std::string::npos
				&& frameRowsTable != std::string::npos && emptyFrameGuard < emptyFrameReturn
				&& emptyFrameReturn < frameRowsTable,
				"An empty idle history should leave its child immediately instead of building an empty sortable table.");
			context.Expect(frameDetails != std::string::npos && selectionGuard != std::string::npos
				&& rendererSummary != std::string::npos && selectionGuard < rendererSummary,
				"Renderer diagnostic tables should be deferred until a completed profiler frame is selected.");
		});

		registry.Add("ProfilerWindow.IdlePresentation.UsesOpaqueDockedBackground", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			const std::size_t beforeBegin = source.find("void ProfilerWindow::OnBeforeBegin()");
			const std::size_t opaqueBackground = source.find("ImGui::SetNextWindowBgAlpha(1.0f)", beforeBegin);

			context.Expect(beforeBegin != std::string::npos && opaqueBackground != std::string::npos
				&& beforeBegin < opaqueBackground,
				"The large docked Profiler background should be opaque before Begin so idle content does not blend over a dense viewport.");
		});

		registry.Add("ProfilerWindow.Diagnostics.ExposesActionableViews", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			const std::string schedulerSource = ReadRepositorySource("Engine/Core/Scheduling/SystemScheduler.cpp");
			const std::string vulkanSource = ReadRepositorySource("Engine/Renderer/RHI/Vulkan/VulkanContext.cpp");

			context.Expect(source.find("Primary self-time offender") != std::string::npos,
				"The diagnosis summary should name the first scope a developer should investigate.");
			context.Expect(source.find("Search scope, phase, path, or thread") != std::string::npos,
				"Large captures should be searchable by both scope and scheduler context.");
			context.Expect(source.find("[Uninstrumented inside %s]") != std::string::npos,
				"Parent self time should appear explicitly instead of disappearing into an inclusive row.");
			context.Expect(source.find("TreeNodeEx(\"Thread Timeline\", ImGuiTreeNodeFlags_DefaultOpen)") != std::string::npos
				&& source.find("IsMouseHoveringRect") != std::string::npos,
				"The default-open per-thread timeline should expose exact scope context on hover.");
			context.Expect(schedulerSource.find("SystemScheduler: Build System Declaration") != std::string::npos
				&& schedulerSource.find("SystemScheduler: Match Topology Cache") != std::string::npos,
				"Frame-workload construction should identify expensive system declarations and cache matching separately.");
			context.Expect(vulkanSource.find("Vulkan: Submission Plan State Lock Wait") != std::string::npos
				&& vulkanSource.find("Vulkan: Publish GPU Animation Plan") != std::string::npos
				&& vulkanSource.find("Vulkan: Publish Deformation Plan") != std::string::npos,
				"Render submission should separate synchronization from animation and deformation publication.");
		});

		registry.Add("ProfilerWindow.FrameStatistics.ExcludePartialFrames", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			const std::size_t statisticsStart = source.find("FrameDurationStats BuildFrameDurationStats");
			const std::size_t partialFrameGuard = source.find("if (!frame.complete)", statisticsStart);
			const std::size_t skipPartialFrame = source.find("continue;", partialFrameGuard);
			const std::size_t durationSample = source.find("values.push_back(frame.durationMs)", partialFrameGuard);

			context.Expect(statisticsStart != std::string::npos
				&& partialFrameGuard != std::string::npos
				&& skipPartialFrame != std::string::npos
				&& durationSample != std::string::npos
				&& partialFrameGuard < skipPartialFrame
				&& skipPartialFrame < durationSample,
				"Partial frames should remain counted as recovery evidence but must not enter live frame-time statistics.");
			context.Expect(source.find("%zu complete  %zu partial") != std::string::npos,
				"The live Profiler should distinguish completed timing samples from partial recovery records.");
		});

		registry.Add("ProfilerWindow.Presentation.UsesProtectedImmutableServicePublication", [](TestContext& context) {
			const std::string serviceSource = ReadEditorProfilerServiceSource();
			const std::string editorSource = ReadRepositorySource("Editor/TerraEngine-Editor/Core/EditorSystem.cpp");
			const std::string mainEditorSource = ReadRepositorySource("Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp");
			const std::size_t drain = serviceSource.find("void EditorProfilerService::Update(float deltaTime)");
			const std::size_t accept = serviceSource.find("void EditorProfilerService::AcceptWorkerCompletion");
			const std::size_t editorDrain = editorSource.find("context.editorProfiler->Update(deltaTime)");
			const std::size_t editorDraw = editorSource.find("mainEditor->DrawUI()");
			const std::size_t closeEpoch = mainEditorSource.find("editorProfiler->BeginProjectClose()");
			const std::size_t sceneUnload = mainEditorSource.find("SceneManager::Instance().UnloadAll()");

			context.Expect(serviceSource.find("completions.swap(pendingCompletions)") != std::string::npos
				&& drain != std::string::npos && accept != std::string::npos,
				"Workers must enqueue profiler results for the protected service drain.");
			context.Expect(serviceSource.find("GetPresentationSnapshot") != std::string::npos
				&& serviceSource.find("std::atomic_store_explicit(&presentationSnapshot") != std::string::npos
				&& serviceSource.find("discardedStaleCompletionCount") != std::string::npos,
				"Profiler presentation must be published as one immutable snapshot instead of exposing mutable worker state to ImGui.");
			context.Expect(editorDrain != std::string::npos && editorDraw != std::string::npos && editorDrain < editorDraw,
				"EditorSystem must drain profiler completions before profiler-window drawing begins.");
			// Validate the production contract even when metadata lives in a
			// shared header rather than beside the native UI execution code.
			const auto declaration = TerraEngine::Editor::MakeEditorSchedulerDeclaration();
			context.Expect(std::find(declaration.writes.begin(), declaration.writes.end(), "EditorProfilerWorkerCompletions") != declaration.writes.end()
				&& std::find(declaration.writes.begin(), declaration.writes.end(), "EditorProfilerHealthPublication") != declaration.writes.end(),
				"Scheduler diagnostics must declare profiler worker draining and protected health publication resources.");
			context.Expect(closeEpoch != std::string::npos && sceneUnload != std::string::npos && closeEpoch < sceneUnload,
				"Project close must invalidate profiler epochs before scene/resource teardown can expose stale results.");
		});

		registry.Add("ProfilerWindow.ActiveCapture.DefersWholeHistoryReads", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			const std::string documentSnapshot = "Get" "DocumentSnapshot()";
			const std::string framesSnapshot = "Get" "FramesSnapshot()";
			const std::string frameListSnapshot = "Get" "FrameListSnapshot()";
			context.Expect(source.find(documentSnapshot) == std::string::npos
				&& source.find(framesSnapshot) == std::string::npos
				&& source.find(frameListSnapshot) == std::string::npos,
				"The V1 Profiler window must not use global whole-history snapshot APIs in any state.");
		});

		registry.Add("ProfilerWindow.V1Selection.RequestsOnlyTheSelectedJournalFrame", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			const std::string service = ReadEditorProfilerServiceSource();
			context.Expect(source.find("service->RequestSelectedFrame(*requestedFrameSelection)") != std::string::npos
				&& service.find("ReadSelectedJournalFrame(path, frameIndex") != std::string::npos,
				"Frame selection must request one V1 journal frame instead of adopting the full capture document.");
			context.Expect(service.find("ReadJournalIndexPage(capturePath") != std::string::npos
				&& service.find("completion.indexPage = std::move(editorPage)") != std::string::npos,
				"Opening a capture must publish one bounded V1 index page before detail is requested.");
		});

		registry.Add("ProfilerWindow.V1Selection.CoalescesBoundedPagesAndDecodedDetails", [](TestContext& context) {
			const std::string service = ReadEditorProfilerServiceSource();
			context.Expect(service.find("pendingIndexRequest.has_value()") != std::string::npos
				&& service.find("ReadJournalIndexPage(path, offset, boundedPageSize") != std::string::npos
				&& service.find("decodedDetailCache.size() > 4u") != std::string::npos,
				"V1 page requests and decoded detail must have explicit bounded ownership.");
			context.Expect(service.find("pendingDetailRequest.has_value()") != std::string::npos
				&& service.find("cached.artifactId == artifactId && cached.frameIndex == frameIndex") != std::string::npos,
				"Duplicate selected-frame requests must coalesce by capture and frame identity.");
			context.Expect(service.find("Returning to the already published page is a newer user choice.") != std::string::npos
				&& service.find("A cached detail selection is still a newer choice") != std::string::npos
				&& service.find("EditorProfilerOperationKind::LoadingIndex) - 1u];") != std::string::npos
				&& service.find("EditorProfilerOperationKind::LoadingDetail) - 1u];") != std::string::npos
				&& service.find("retiredOperation.active = false;") != std::string::npos
				&& service.find("retiredOperation.state = EditorProfilerOperationState::Cancelled;") != std::string::npos,
				"Selecting an already cached page or detail must cancel its different pending read before it can publish stale UI.");
			const std::string window = ReadProfilerWindowSource();
			context.Expect(window.find("sampleAnalysisArtifactId == cache->selectedCapture->artifactId") != std::string::npos,
				"Derived analysis must not cross-reuse an equal frame index from another capture.");
		});

		registry.Add("ProfilerWindow.V1Presentation.HidesSessionPanelsWithoutSelection", [](TestContext& context) {
			const std::string window = ReadProfilerWindowSource();
			const std::size_t noSelectionGuard = window.find("if (!cache->selectedCapture.has_value())");
			const std::size_t layout = window.find("BeginTable(\"ProfilerLayout\"");
			context.Expect(noSelectionGuard != std::string::npos && layout != std::string::npos
				&& noSelectionGuard < layout,
				"An empty selected session must return before Frames and Frame Analysis panels are created.");
		});

		registry.Add("ProfilerWindow.V1Presentation.DefersCaptureBrowserWhileLive", [](TestContext& context) {
			const std::string window = ReadProfilerWindowSource();
			const std::size_t noSelectionGuard = window.find("if (!cache->selectedCapture.has_value())");
			const std::size_t liveGuard = window.find("if (captureSessionActive)", noSelectionGuard);
			const std::size_t browser = window.find("DrawCaptureList();", liveGuard);
			context.Expect(noSelectionGuard != std::string::npos && liveGuard != std::string::npos
				&& browser != std::string::npos && liveGuard < browser
				&& window.find("Capture browsing resumes after recording finalizes.") != std::string::npos,
				"A live capture must avoid building a saved-capture browser before its journal is publishable.");
			context.Expect(window.find("ToolbarButton(\"Refresh\", canUseProject && !captureSessionActive") != std::string::npos,
				"Refresh must not schedule catalog work while live presentation cannot open a saved capture.");
		});

		registry.Add("ProfilerWindow.V1Finalization.PublishesExactSavedArtifact", [](TestContext& context) {
			const std::string service = ReadEditorProfilerServiceSource();
			context.Expect(!service.empty(), "Finalization source guard requires readable, nonempty service source.");
			// Use actual compiled definitions: comment cleanup must not change the selected policy region.
			const std::size_t update = service.find("void EditorProfilerService::Update(float deltaTime)");
			context.Expect(update != std::string::npos, "The actual profiler Update definition must exist.");
			const std::size_t finalization = service.find("const auto status = Profiler::Instance().GetJournalFinalizationStatus(*ticket)", update);
			const std::size_t finalizationEnd = service.find("void EditorProfilerService::StartRecording()", update);
			context.Expect(finalization != std::string::npos && finalizationEnd != std::string::npos
				&& update < finalization && finalization < finalizationEnd,
				"Finalization must have nonempty bounds within Update before the next compiled definition.");
			const std::string finalizationPath = service.substr(finalization, finalizationEnd - finalization);
			context.Expect(service.find("WorkerCompletionKind::CapturePersisted") != std::string::npos
				&& service.find("lastSavedCaptureArtifact = std::move(completion.savedCaptureArtifact)") != std::string::npos,
				"A completed V1 writer must publish the exact validated artifact before automation can finish.");
			context.Expect(finalizationPath.find("Validating finalized profiler journal") != std::string::npos
				&& finalizationPath.find("ReadJournalIndex(path, journal, error)") != std::string::npos
				&& finalizationPath.find("ReadSelectedJournalFrame") == std::string::npos
				&& finalizationPath.find("OpenProfilerJournalV1") == std::string::npos,
				"Terminal writer completion must validate only the bounded V1 index; detailed frame reads remain a later selection operation.");
		});

		registry.Add("ProfilerWindow.V1Finalization.AutomationWaitsOnlyForPersistence", [](TestContext& context) {
			const std::string service = ReadEditorProfilerServiceSource();
			const std::string automation = ReadRepositorySource("Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp");
			const std::size_t stopJob = automation.find("jobId == activeProfilerCaptureJobId");
			const std::size_t nextJob = automation.find("jobId == activeProfilerLoadJobId", stopJob);
			const std::string stopPath = stopJob != std::string::npos && nextJob != std::string::npos
				? automation.substr(stopJob, nextJob - stopJob)
				: std::string{};
			context.Expect(service.find("bool EditorProfilerService::IsPersisting() const") != std::string::npos
				&& stopPath.find("profiler.IsPersisting()") != std::string::npos
				&& stopPath.find("profiler.IsBusy()") == std::string::npos,
				"Automation stop completion must wait only for finalization/index validation, never unrelated list or detail work.");
		});

		registry.Add("ProfilerWindow.V1Lifecycle.RejectsCrossEpochAndCrossCapturePublication", [](TestContext& context) {
			const std::string service = ReadEditorProfilerServiceSource();
			const std::string window = ReadProfilerWindowSource();
			context.Expect(service.find("projectGeneration != startProjectEpoch") != std::string::npos
				&& service.find("auto& priorPersistence = operations[static_cast<std::size_t>(EditorProfilerOperationKind::Persisting) - 1u];") != std::string::npos
				&& service.find("priorPersistence.active = false;") != std::string::npos
				&& service.find("priorPersistence.state = EditorProfilerOperationState::Cancelled;") != std::string::npos,
				"Starting a new V1 recording must cancel an earlier finalize-validation completion instead of allowing it to publish.");
			const std::size_t close = service.find("void EditorProfilerService::BeginProjectClose()");
			const std::size_t clear = service.find("Profiler::Instance().Clear();", close);
			const std::size_t closePublication = service.find("PublishPresentationLocked();", close);
			context.Expect(close != std::string::npos && clear != std::string::npos
				&& closePublication != std::string::npos && clear < closePublication,
				"Project close must clear the engine session before publishing its empty editor epoch.");
			context.Expect(service.find("completion.selectedArtifactId != selectedCapture->artifactId") != std::string::npos
				&& service.find("completion.selectedArtifactId = artifactId") != std::string::npos,
				"A selected-frame worker result must be rejected when another capture superseded its artifact.");
			context.Expect(service.find("completion.projectEpoch != projectGeneration") != std::string::npos
				&& service.find("operations[index].state = EditorProfilerOperationState::Cancelled;") != std::string::npos,
				"Cross-epoch and cross-capture worker completions must remain rejected and report a cancelled terminal operation.");
			context.Expect(window.find("cache->journalSummary.reset()") != std::string::npos
				&& window.find("cache->frames.clear()") != std::string::npos,
				"An empty immutable service selection must clear old-project rows from the Profiler window.");
		});

		registry.Add("ProfilerWindow.V1Health.PublishesOnlyForTheActiveProject", [](TestContext& context) {
			const std::string service = ReadEditorProfilerServiceSource();
			context.Expect(service.find("if (sessionAccepting)") != std::string::npos
				&& service.find("health = {}") != std::string::npos
				&& service.find("healthPublishAccumulatorSeconds = 0.0f") != std::string::npos,
				"Health polling must reset across project epochs and avoid publishing an unrelated engine session.");
		});

		registry.Add("ProfilerWindow.LiveCapture.DoesNotRepublishLoadedJournalHistory", [](TestContext& context) {
			const std::string service = ReadEditorProfilerServiceSource();
			const std::size_t start = service.find("void EditorProfilerService::StartRecording(const ProfilerCaptureSettings& settings)");
			const std::size_t pause = service.find("void EditorProfilerService::PauseRecording()", start);
			const std::string startPath = start != std::string::npos && pause != std::string::npos
				? service.substr(start, pause - start)
				: std::string{};
			context.Expect(startPath.find("selectedCapture.reset()") != std::string::npos
				&& startPath.find("selectedIndexPage.reset()") != std::string::npos,
				"Starting a capture must discard the previous loaded index before periodic health publication begins.");
			context.Expect(service.find("const bool liveCapture = snapshot->sessionState") != std::string::npos
				&& service.find("ProfilerSessionState::Finalizing && persistenceActive") != std::string::npos
				&& service.find("if (!liveCapture)") != std::string::npos,
				"Live presentation snapshots must carry scalar health only, not an old journal index or selected detail.");
		});
	}
}
