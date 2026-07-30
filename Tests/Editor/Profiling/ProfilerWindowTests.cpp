#include "TestHarness.h"

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
			context.Expect(source.find("BuildTimingStreamAggregates") != std::string::npos, "Profiler UI should use capture-level timing aggregate helpers.");
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

		registry.Add("ProfilerWindow.V2Evidence.PresentsRecoveryAndTypedStreams", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();

			context.Expect(source.find("DrawCaptureEvidenceSummary") != std::string::npos, "Profiler Window should render capture-level v2 evidence separately from a selected frame.");
			context.Expect(source.find("detailLevel") != std::string::npos, "Profiler Window should identify the detailed, sampled, or aggregate capture level.");
			context.Expect(source.find("Recovery warning") != std::string::npos, "Recovered incomplete capture tails should remain visible to the user.");
			context.Expect(source.find("Physics and Scene View Stages") != std::string::npos, "Profiler Window should distinguish native physics and Scene View stages.");
			context.Expect(source.find("Protected Boundary Breakdown") != std::string::npos, "Profiler Window should break protected queue, ownership, flush, and publication waits apart.");
			context.Expect(source.find("Worker Timeline Summary") != std::string::npos, "Profiler Window should present bounded worker history summaries.");
			context.Expect(source.find("Physics Work Volumes") != std::string::npos, "Profiler Window should present processed-versus-published body volumes.");
			context.Expect(source.find("unavailable") != std::string::npos, "Unavailable typed streams should be labelled instead of shown as zero.");
		});

		registry.Add("ProfilerWindow.Recording.UsesBoundedDetailedCaptureForEveryFrameScopeEvidence", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			const std::string serviceSource = ReadEditorProfilerServiceSource();
			const std::size_t timingDisclosure = source.find("TreeNode(\"Timing Distribution\")");
			const std::size_t frameCopy = source.find("GetFramesSnapshot()", timingDisclosure);
			const std::size_t evidenceDisclosure = source.find("TreeNode(\"Whole Capture Analysis\")");
			const std::size_t documentCopy = source.find("GetDocumentSnapshot()", evidenceDisclosure);

			context.Expect(serviceSource.find("detailLevel = TerraEngine::Profiling::ProfilerCaptureDetailLevel::Detailed") != std::string::npos
				&& serviceSource.find("Thread-owned fixed buffers") != std::string::npos,
				"The ordinary Record action should retain every-frame scope evidence through bounded thread-owned buffers.");
			context.Expect(serviceSource.find("recordMarkers = true") != std::string::npos
				&& serviceSource.find("taskExecutionEnabled = true") != std::string::npos
				&& serviceSource.find("recordTaskContext = true") != std::string::npos,
				"The detailed toolbar capture should retain marker and task context needed to build useful scope trees.");
			context.Expect(serviceSource.find("settings.maxFrames = TerraEngine::Profiling::kDefaultProfilerRollingFrameLimit") != std::string::npos
				&& source.find("Record Every Frame") != std::string::npos,
				"The every-frame toolbar action should clearly identify itself and retain a bounded detailed history.");
			context.Expect(serviceSource.find("no shared profiler mutex") != std::string::npos
				&& serviceSource.find("reports drops") != std::string::npos,
				"The service should document why every-frame detailed capture remains bounded.");
			context.Expect(timingDisclosure != std::string::npos && frameCopy != std::string::npos && timingDisclosure < frameCopy,
				"Rolling timing distributions should copy the complete capture only after the user expands them.");
			context.Expect(evidenceDisclosure != std::string::npos && documentCopy != std::string::npos && evidenceDisclosure < documentCopy,
				"Whole-capture evidence should be copied and summarized only after the user expands it.");
		});

		registry.Add("ProfilerWindow.OpenFramePath.UsesCachedCompactSnapshots", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();

			context.Expect(source.find("kProfilerWindowRefreshInterval") != std::string::npos
				&& source.find("GetSnapshotRevision()") != std::string::npos
				&& source.find("now >= cache->nextProfilerRefresh") != std::string::npos,
				"The open profiler should poll a cheap revision and rate-limit deep presentation snapshots.");
			context.ExpectEqual(static_cast<std::size_t>(1u), CountOccurrences(source, "GetFrameListSnapshot()"),
				"The profiler window should copy its frame list only in the snapshot refresh path.");
			context.ExpectEqual(static_cast<std::size_t>(1u), CountOccurrences(source, "GetSelectedFrameSnapshot()"),
				"The profiler window should copy its selected frame only in the snapshot refresh path.");
			context.Expect(source.find("kProfilerStatsRefreshInterval") != std::string::npos
				&& source.find("now >= cache->nextProfilerStatsRefresh") != std::string::npos,
				"Whole-history percentile sorting should update more slowly than compact live rows.");
			context.Expect(source.find("const std::vector<ProfilerFrameListItem>& displayedFrames = frameIndexOrder ? frames : sortedFrames") != std::string::npos
				&& source.find("if (!frameIndexOrder && (cache->frameOrderDirty || sortSpecs->SpecsDirty))") != std::string::npos,
				"The default frame-index order should traverse the chronological snapshot directly and sort only explicit timing orders.");
			const std::size_t pinGuard = source.find("cache->pinnedFrameIndex.has_value()");
			const std::size_t selectedFrameCopy = source.find("GetSelectedFrameSnapshot()", pinGuard);
			context.Expect(pinGuard != std::string::npos
				&& selectedFrameCopy != std::string::npos
				&& pinGuard < selectedFrameCopy
				&& source.find("Live timings are updating in the frame list") != std::string::npos,
				"Live recording should remain compact until the user explicitly pins a frame for detailed inspection.");
			context.ExpectEqual(static_cast<std::size_t>(1u), CountOccurrences(source, "BuildSampleTree(frame, selectedModule)"),
				"Top offenders and the sample hierarchy should share one cached sample-tree analysis.");
			context.Expect(source.find("GetLatestRenderGraphDiagnostics()") == std::string::npos
				&& source.find("GetLatestBackendStageTimings()") != std::string::npos
				&& source.find("GetLatestRendererRecoverySnapshot()") != std::string::npos,
				"The profiler should read compact renderer fields instead of copying the complete diagnostics graph.");
			context.Expect(source.find("captureEvidenceValid") != std::string::npos
				&& source.find("Refresh Whole Capture Analysis") != std::string::npos
				&& source.find("timingDistributionValid") != std::string::npos
				&& source.find("Refresh Timing Distribution") != std::string::npos
				&& source.find("timingDistributionFrameIndex") == std::string::npos,
				"Expanded whole-capture disclosures should reuse cached analysis until the user requests a refresh.");
			context.Expect(source.find("TreeNode(\"Sample Hierarchy\")") != std::string::npos
				&& source.find("TreeNode(\"Top Offenders\")") != std::string::npos
				&& source.find("GetBool(openStateId, false)") != std::string::npos,
				"Sorting and drawing large scope hierarchies should be opt-in and start collapsed.");
		});
	}
}
