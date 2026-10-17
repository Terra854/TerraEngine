// ProfilerWindow.cpp â€” Profiler Window Implementation
//
// Implements the Profiler Window for the TerraEngine editor, providing a visual interface for performance monitoring.
// It retrieves data from the EditorProfilerService and the engine's core Profiler to display CPU and GPU metrics via ImGui.
// This window allows developers to analyze frame times, task execution, and resource utilization in real-time.

#include "ProfilerWindow.h"

#include "../Core/EditorContext.h"
#include "../Profiling/EditorProfilerService.h"
#include "../Profiling/ProfilerDiagnosticAnalysis.h"
#include "../Profiling/ProfilerSampleHierarchy.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scripting/ScriptingSystem.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using TerraEngine::Profiling::Profiler;
using TerraEngine::Profiling::ProfilerCaptureSettings;
using TerraEngine::Profiling::ProfilerFrameListItem;
using TerraEngine::Profiling::ProfilerFrameRecord;
using TerraEngine::Profiling::ProfilerModuleId;
using TerraEngine::Profiling::ProfilerSample;
using TerraEngine::Profiling::ProfilerSessionState;
using TerraEngine::Profiling::ProfilerTimingStreamAggregate;
using TerraEngine::Profiling::ProfilerTimingStreamStatus;
using TerraEngine::EditorProfiling::ProfilerDiagnosticAnalysis;
using TerraEngine::EditorProfiling::ProfilerDiagnosticRow;
using TerraEngine::EditorProfiling::ProfilerSampleHierarchy;
using TerraEngine::EditorProfiling::ProfilerSampleHierarchyNode;
using TerraEngine::EditorProfiling::BuildProfilerDiagnosticAnalysis;
using TerraEngine::EditorProfiling::ProfilerDiagnosticRowMatches;

namespace
{
// Profiler presentation is intentionally refreshed below the render rate. A
// ten-Hz UI still feels live while keeping deep capture and renderer snapshots
// off the producer-critical path.
constexpr auto kProfilerWindowRefreshInterval = std::chrono::milliseconds(100);
// struct FrameDurationStats keeps capture-level timing answers close to the UI
// code that presents them, without changing the profiler capture file format.
struct FrameDurationStats
{
	std::size_t frameCount = 0u;
	std::size_t partialFrameCount = 0u;
	double latestMs = 0.0;
	double minMs = 0.0;
	double maxMs = 0.0;
	double meanMs = 0.0;
	double medianMs = 0.0;
	double p90Ms = 0.0;
	double p95Ms = 0.0;
	double p99Ms = 0.0;
	double stdDevMs = 0.0;
};


const TerraEngine::Profiling::ProfilerModuleSummary* FindModuleSummary(const ProfilerFrameRecord& frame, ProfilerModuleId moduleId)
{
	const auto found = std::find_if(frame.moduleSummaries.begin(), frame.moduleSummaries.end(), [moduleId](const auto& module) {
		return module.moduleId == moduleId;
	});
	return found == frame.moduleSummaries.end() ? nullptr : &(*found);
}

// Frames are easiest to compare when the color tells whether the frame is
// comfortably below an editor budget, getting expensive, or already over a
// common 60 FPS frame budget.
ImVec4 FrameDurationColor(double durationMs)
{
	if (durationMs >= 16.67)
	{
		return ImVec4(1.0f, 0.35f, 0.25f, 1.0f);
	}
	if (durationMs >= 8.33)
	{
		return ImVec4(1.0f, 0.80f, 0.25f, 1.0f);
	}
	return ImVec4(0.45f, 0.90f, 0.55f, 1.0f);
}

double SafePercent(double value, double total)
{
	if (total <= 0.0 || !std::isfinite(total) || !std::isfinite(value))
	{
		return 0.0;
	}
	return (value / total) * 100.0;
}

// DrawTimeBar renders a compact horizontal bar next to a numeric timing so
// spikes are visible without mentally comparing every row in the table.
void DrawTimeBar(double valueMs, double basisMs)
{
	float fraction = 0.0f;
	if (basisMs > 0.0 && std::isfinite(valueMs) && std::isfinite(basisMs))
	{
		fraction = static_cast<float>(std::clamp(valueMs / basisMs, 0.0, 1.0));
	}
	ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), "");
}

double PercentileFromSorted(const std::vector<double>& sortedValues, double percentile)
{
	if (sortedValues.empty())
	{
		return 0.0;
	}
	if (sortedValues.size() == 1u)
	{
		return sortedValues.front();
	}
	const double clampedPercentile = std::clamp(percentile, 0.0, 1.0);
	const double scaledIndex = clampedPercentile * static_cast<double>(sortedValues.size() - 1u);
	const std::size_t lowerIndex = static_cast<std::size_t>(std::floor(scaledIndex));
	const std::size_t upperIndex = std::min(lowerIndex + 1u, sortedValues.size() - 1u);
	const double blend = scaledIndex - static_cast<double>(lowerIndex);
	return sortedValues[lowerIndex] + (sortedValues[upperIndex] - sortedValues[lowerIndex]) * blend;
}

FrameDurationStats BuildFrameDurationStats(const std::vector<ProfilerFrameListItem>& frames)
{
	FrameDurationStats stats;
	std::vector<double> values;
	values.reserve(frames.size());
	for (const ProfilerFrameListItem& frame : frames)
	{
		if (!frame.complete)
		{
			++stats.partialFrameCount;
			// A partial frame ends when capture lifecycle work interrupts the frame,
			// so its elapsed duration is not comparable to a naturally completed
			// frame. Keep it visible as recovery evidence without allowing it to
			// inflate the live mean or tail percentiles.
			continue;
		}
		if (std::isfinite(frame.durationMs) && frame.durationMs >= 0.0)
		{
			values.push_back(frame.durationMs);
			stats.latestMs = frame.durationMs;
		}
	}
	stats.frameCount = values.size();
	if (values.empty())
	{
		return stats;
	}
	const double sum = std::accumulate(values.begin(), values.end(), 0.0);
	stats.meanMs = sum / static_cast<double>(values.size());
	double varianceSum = 0.0;
	for (double value : values)
	{
		const double delta = value - stats.meanMs;
		varianceSum += delta * delta;
	}
	stats.stdDevMs = std::sqrt(varianceSum / static_cast<double>(values.size()));
	std::sort(values.begin(), values.end());
	stats.minMs = values.front();
	stats.maxMs = values.back();
	stats.medianMs = PercentileFromSorted(values, 0.50);
	stats.p90Ms = PercentileFromSorted(values, 0.90);
	stats.p95Ms = PercentileFromSorted(values, 0.95);
	stats.p99Ms = PercentileFromSorted(values, 0.99);
	return stats;
}

void DrawMetricCell(const char* label, double valueMs, const ImVec4& valueColor)
{
	ImGui::TableNextColumn();
	ImGui::TextDisabled("%s", label);
	ImGui::TextColored(valueColor, "%.3f ms", valueMs);
}

void DrawDisabledReasonTooltip(const char* reason)
{
	if (reason != nullptr && reason[0] != '\0' && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		ImGui::SetTooltip("%s", reason);
	}
}

bool ToolbarButton(const char* label, bool enabled, const char* disabledReason)
{
	if (!enabled)
	{
		ImGui::BeginDisabled();
	}
	const bool pressed = ImGui::Button(label);
	if (!enabled)
	{
		ImGui::EndDisabled();
		DrawDisabledReasonTooltip(disabledReason);
	}
	return enabled && pressed;
}

void DrawToolbarGroupLabel(const char* label)
{
	ImGui::TextDisabled("%s", label);
	ImGui::SameLine();
}

const char* TimingDisplayValue(const ProfilerTimingStreamAggregate& aggregate, char* buffer, std::size_t bufferSize)
{
	if (aggregate.selectedStatus == ProfilerTimingStreamStatus::Valid)
	{
		std::snprintf(buffer, bufferSize, "%.3f ms", aggregate.selectedMilliseconds);
		return buffer;
	}
	return TerraEngine::Profiling::ToString(aggregate.selectedStatus);
}

const char* TimingAggregateValue(const ProfilerTimingStreamAggregate& aggregate, double value, char* buffer, std::size_t bufferSize)
{
	// Aggregate columns are only meaningful when at least one valid sample
	// contributed to the stream. Unavailable GPU timing must not look like a
	// measured zero-millisecond result.
	if (aggregate.sampleCount == 0u)
	{
		return "n/a";
	}
	std::snprintf(buffer, bufferSize, "%.3f", value);
	return buffer;
}


bool DrawDisclosureControl(bool hasChildren, bool isOpen)
{
	constexpr float kArrowSlotWidth = 7.0f;
	constexpr float kArrowSize = 4.0f;
	const float rowHeight = ImGui::GetTextLineHeight();

	if (!hasChildren)
	{
		ImGui::Dummy(ImVec2(kArrowSlotWidth, rowHeight));
		ImGui::SameLine(0.0f, 1.0f);
		return false;
	}

	const ImVec2 buttonPosition = ImGui::GetCursorScreenPos();
	const bool clicked = ImGui::InvisibleButton("SampleDisclosure", ImVec2(kArrowSlotWidth, rowHeight));
	const ImU32 arrowColor = ImGui::GetColorU32(ImGuiCol_Text);
	const ImVec2 center(
		buttonPosition.x + kArrowSlotWidth * 0.5f,
		buttonPosition.y + rowHeight * 0.5f
	);
	ImDrawList* drawList = ImGui::GetWindowDrawList();

	if (isOpen)
	{
		drawList->AddTriangleFilled(
			ImVec2(center.x - kArrowSize, center.y - kArrowSize * 0.45f),
			ImVec2(center.x + kArrowSize, center.y - kArrowSize * 0.45f),
			ImVec2(center.x, center.y + kArrowSize * 0.65f),
			arrowColor
		);
	}
	else
	{
		drawList->AddTriangleFilled(
			ImVec2(center.x - kArrowSize * 0.4f, center.y - kArrowSize),
			ImVec2(center.x - kArrowSize * 0.4f, center.y + kArrowSize),
			ImVec2(center.x + kArrowSize * 0.65f, center.y),
			arrowColor
		);
	}
	ImGui::SameLine(0.0f, 1.0f);
	return clicked;
}

bool DiagnosticNodeOrDescendantMatches(
	const ProfilerDiagnosticAnalysis& analysis,
	std::size_t nodeIndex,
	const std::string& query,
	double minimumDurationMs)
{
	if (ProfilerDiagnosticRowMatches(analysis.rows[nodeIndex], query, minimumDurationMs))
	{
		return true;
	}
	for (const std::size_t childIndex : analysis.hierarchy.nodes[nodeIndex].childIndices)
	{
		if (DiagnosticNodeOrDescendantMatches(analysis, childIndex, query, minimumDurationMs))
		{
			return true;
		}
	}
	return false;
}

// DrawSampleTreeRow renders validated hierarchy links plus an explicit row for
// parent self time. The latter is the part of a scope with no child marker and
// is usually the first place a developer should add finer instrumentation.
void DrawSampleTreeRow(
	const ProfilerDiagnosticAnalysis& analysis,
	std::size_t nodeIndex,
	std::uint64_t frameIndex,
	double frameDurationMs,
	const std::string& query,
	double minimumDurationMs,
	bool showUninstrumentedRows)
{
	const ProfilerSampleHierarchy& tree = analysis.hierarchy;
	const ProfilerSampleHierarchyNode& node = tree.nodes[nodeIndex];
	const ProfilerSample& sample = tree.samples[node.sampleIndex];
	const ProfilerDiagnosticRow& row = analysis.rows[nodeIndex];
	if (!DiagnosticNodeOrDescendantMatches(analysis, nodeIndex, query, minimumDurationMs))
	{
		return;
	}
	const bool hasChildren = std::any_of(node.childIndices.begin(), node.childIndices.end(), [&](std::size_t childIndex) {
		return DiagnosticNodeOrDescendantMatches(analysis, childIndex, query, minimumDurationMs);
	});
	constexpr float kTreeIndentPixels = 12.0f;

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(node.presentationDepth) * kTreeIndentPixels);
	bool isOpen = true;
	const std::string rowId = std::to_string(frameIndex) + ":" + std::to_string(sample.sampleId);
	ImGui::PushID(rowId.c_str());

	if (hasChildren)
	{
		ImGuiStorage* storage = ImGui::GetStateStorage();
		const ImGuiID openStateId = ImGui::GetID("SampleOpen");
		// A newly opened profiler frame should expose the complete diagnostic
		// hierarchy immediately. The stored value still preserves any manual
		// collapse the developer makes after that first presentation.
		isOpen = storage->GetBool(openStateId, true);

		if (DrawDisclosureControl(true, isOpen))
		{
			isOpen = !isOpen;
			storage->SetBool(openStateId, isOpen);
		}
	}
	else
	{
		// Leaf rows reserve the same tiny arrow slot so names align within
		// each depth level without using ImGui tree nodes inside a table row.
		DrawDisclosureControl(false, true);
	}
	ImGui::TextUnformatted(row.name.c_str());

	ImGui::TableSetColumnIndex(1);
	ImGui::Text("%.3f", sample.durationMs);
	ImGui::TableSetColumnIndex(2);
	ImGui::Text("%.3f", node.selfDurationMs);
	ImGui::TableSetColumnIndex(3);
	ImGui::Text("%.1f%%", SafePercent(sample.durationMs, frameDurationMs));
	ImGui::TableSetColumnIndex(4);
	DrawTimeBar(sample.durationMs, frameDurationMs);
	ImGui::TableSetColumnIndex(5);
	ImGui::TextUnformatted(row.threadLabel.c_str());
	if (ImGui::IsItemHovered() && row.rawThreadLabel != row.threadLabel)
	{
		ImGui::SetTooltip("Captured thread: %s", row.rawThreadLabel.c_str());
	}
	ImGui::TableSetColumnIndex(6);
	ImGui::TextUnformatted(row.phase.empty() ? "-" : row.phase.c_str());
	ImGui::TableSetColumnIndex(7);
	ImGui::TextUnformatted(row.executionPath.empty() ? "-" : row.executionPath.c_str());


	if (hasChildren && isOpen)
	{
		if (showUninstrumentedRows && node.selfDurationMs >= minimumDurationMs)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(node.presentationDepth + 1u) * kTreeIndentPixels + 8.0f);
			ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "[Uninstrumented inside %s]", row.name.c_str());
			ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f", node.selfDurationMs);
			ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f", node.selfDurationMs);
			ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f%%", SafePercent(node.selfDurationMs, frameDurationMs));
			ImGui::TableSetColumnIndex(4); DrawTimeBar(node.selfDurationMs, frameDurationMs);
			ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(row.threadLabel.c_str());
			ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(row.phase.empty() ? "-" : row.phase.c_str());
			ImGui::TableSetColumnIndex(7); ImGui::TextUnformatted("No child scope");
		}
		for (const std::size_t childIndex : node.childIndices)
		{
			DrawSampleTreeRow(analysis, childIndex, frameIndex, frameDurationMs, query,
				minimumDurationMs, showUninstrumentedRows);
		}
	}
	ImGui::PopID();
}
}

// ProfilerWindowCache owns immutable presentation data. It is defined in this
// translation unit so the public window header does not expose renderer or ImGui
// analysis implementation types.
struct ProfilerWindowCache
{
	std::uint64_t profilerRevision = 0u;
	// The service revision represents discrete editor-visible changes. Unlike the
	// engine revision, it remains stable while the idle host loop advances.
	std::uint64_t presentationRevision = 0u;
	ProfilerSessionState state = ProfilerSessionState::Idle;
	// The immutable service snapshot owns these values. Keeping them beside the
	// cached catalog lets an open idle window draw its toolbar without taking the
	// service mutex or allocating status/error strings on every ImGui frame.
	bool hasActiveProject = false;
	bool hasActiveOperation = false;
	std::string statusText;
	std::string lastError;
	ProfilerCaptureSettings settings;
	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures;
	std::filesystem::path profilerDirectory;
	std::vector<ProfilerFrameListItem> frames;
	std::size_t indexPageOffset = 0u;
	std::size_t indexPageSize = 128u;
	std::size_t indexTotalFrameCount = 0u;
	std::optional<TerraEngine::Profiling::ProfilerJournalV1Summary> journalSummary;
	std::optional<EditorProfilerCaptureArtifact> selectedCapture;
	std::vector<ProfilerFrameListItem> sortedFrames;
	FrameDurationStats frameDurationStats;
	// Detailed frame payloads remain outside the live refresh path until the user
	// explicitly pins a row. Completed frames are immutable, so one copy can be
	// reused while newer compact frame-list entries continue arriving.
	std::optional<std::uint64_t> pinnedFrameIndex;
	std::optional<ProfilerFrameRecord> selectedFrame;
	bool frameOrderDirty = true;
	bool sampleAnalysisValid = false;
	std::string sampleAnalysisArtifactId;
	std::uint64_t sampleAnalysisFrameIndex = 0u;
	ProfilerModuleId sampleAnalysisModule = ProfilerModuleId::CpuEngine;
	ProfilerDiagnosticAnalysis diagnosticAnalysis;
	// Filters are presentation state and never trigger a new capture analysis.
	// This keeps typing in Search cheap even for a frame with thousands of scopes.
	std::array<char, 128u> sampleSearch{};
	float minimumSampleDurationMs = 0.05f;
	bool showUninstrumentedRows = true;
	bool timingDistributionOpenLastFrame = false;
	bool timingDistributionValid = false;
	std::vector<ProfilerTimingStreamAggregate> timingStreams;
	std::chrono::steady_clock::time_point nextProfilerRefresh{};
	std::chrono::steady_clock::time_point nextRendererRefresh{};
	TerraEngine::Renderer::RendererRecoverySnapshot rendererRecovery;
	std::vector<TerraEngine::Renderer::BackendStageTiming> backendStageTimings;
	RenderStatsSnapshot gameStats;
	RenderStatsSnapshot sceneStats;
};

// ScopedProfilerWindowDrawTiming publishes only the derived window's CPU work.
// EditorSystem measures later ImGui build/present stages independently, so this
// boundary lets open-idle automation distinguish UI construction from GPU/UI
// presentation or scheduler contention without starting a profiler capture.
class ScopedProfilerWindowDrawTiming
{
public:
	explicit ScopedProfilerWindowDrawTiming(EditorProfilerService* profilerService)
		: service(profilerService), startedAt(std::chrono::steady_clock::now())
	{
	}

	~ScopedProfilerWindowDrawTiming()
	{
		if (service == nullptr)
		{
			return;
		}
		const double elapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - startedAt).count();
		service->PublishProfilerWindowDrawTiming(
			static_cast<std::uint64_t>(std::max(0, ImGui::GetFrameCount())), elapsedMs);
	}

	ScopedProfilerWindowDrawTiming(const ScopedProfilerWindowDrawTiming&) = delete;
	ScopedProfilerWindowDrawTiming& operator=(const ScopedProfilerWindowDrawTiming&) = delete;

private:
	EditorProfilerService* service = nullptr;
	std::chrono::steady_clock::time_point startedAt;
};

void ProfilerWindow::OnBeforeBegin()
{
	// The profiler commonly occupies a large docked panel above the live scene.
	// Force only this panel's background opaque before Begin so the GPU does not
	// blend a full profiler-sized translucent surface over a dense viewport.
	// Docking, content, interaction, and the global editor style stay unchanged.
	ImGui::SetNextWindowBgAlpha(1.0f);
}

ProfilerWindow::ProfilerWindow(MainEditorSubsystem* windowOwner)

	: EditorWindow("Profiler", false, "Rendering"), owner(windowOwner), cache(std::make_unique<ProfilerWindowCache>())
{
}

ProfilerWindow::~ProfilerWindow() = default;

void ProfilerWindow::RefreshLiveSnapshot()
{
	Profiler& profiler = Profiler::Instance();
	EditorProfilerService* service = owner && owner->GetContext()
		? owner->GetContext()->editorProfiler.get() : nullptr;
	if (service == nullptr)
	{
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	// The scalar revision is cheap to acquire on every open-idle frame. Retain
	// the shared immutable snapshot only after a protected service publication,
	// avoiding reference-count traffic and catalog fields when nothing changed.
	const std::uint64_t serviceRevision = service->GetPresentationRevision();
	const bool presentationChanged = cache->presentationRevision != serviceRevision;
	if (presentationChanged)
	{
		const std::shared_ptr<const EditorProfilerPresentationSnapshot> presentation =
			service->GetPresentationSnapshot();
		if (!presentation)
		{
			return;
		}
		// The service publishes one immutable catalog value from its protected
		// completion boundary. Retaining it here avoids racing worker discovery or
		// taking the service mutex while ImGui is drawing virtualized rows.
		cache->hasActiveProject = presentation->hasActiveProject;
		cache->hasActiveOperation = std::any_of(
			presentation->operations.begin(), presentation->operations.end(),
			[](const EditorProfilerOperationStatus& operation) {
				return operation.active;
			});
		cache->statusText = presentation->statusText;
		cache->lastError = presentation->lastError;
		cache->state = presentation->sessionState;
		cache->captures = presentation->captures;
		cache->profilerDirectory = presentation->profilerDirectory;
		if (presentation->selectedCapture.has_value())
		{
			cache->journalSummary = presentation->selectedSummary;
			cache->selectedCapture = presentation->selectedCapture;
			if (presentation->indexPage.has_value())
			{
				cache->frames = presentation->indexPage->frames;
				cache->indexPageOffset = presentation->indexPage->offset;
				cache->indexPageSize = presentation->indexPage->pageSize;
				cache->indexTotalFrameCount = presentation->indexPage->totalFrameCount;
			}
			else
			{
				cache->frames.clear();
				cache->indexPageOffset = 0u;
				cache->indexTotalFrameCount = 0u;
			}
			cache->frameDurationStats = BuildFrameDurationStats(cache->frames);
			cache->frameOrderDirty = true;
			cache->selectedFrame = presentation->selectedFrame;
		}
		else
		{
			// A project close or a failed capture open publishes no selected artifact.
			// Clear every cached detail so an old project's rows cannot remain visible
			// while the immutable service snapshot is already empty.
			cache->journalSummary.reset();
			cache->selectedCapture.reset();
			cache->frames.clear();
			cache->indexPageOffset = 0u;
			cache->indexTotalFrameCount = 0u;
			cache->sortedFrames.clear();
			cache->frameDurationStats = {};
			cache->frameOrderDirty = true;
			cache->pinnedFrameIndex.reset();
			cache->selectedFrame.reset();
		}
		// The revision can advance immediately before its shared pointer is
		// published. Remember the snapshot's own identity so that narrow window
		// is retried next frame instead of accidentally treating an older snapshot
		// as the newest protected publication.
		cache->presentationRevision = presentation->presentationRevision;
	}

	if (now >= cache->nextProfilerRefresh)
	{
		// State remains cheap enough to poll at the UI cadence. The engine's
		// snapshot revision advances every host frame, even while idle, so it is
		// meaningful only while a live capture is producing new data.
		cache->nextProfilerRefresh = now + kProfilerWindowRefreshInterval;
		const ProfilerSessionState previousState = cache->state;
		cache->state = profiler.GetState();
		const bool stateChanged = previousState != cache->state;
		const bool liveCapture = cache->state == ProfilerSessionState::Recording
			|| cache->state == ProfilerSessionState::Paused;
		const std::uint64_t requestedRevision = profiler.GetSnapshotRevision();
		const bool liveDataChanged = liveCapture && cache->profilerRevision != requestedRevision;
		if (!presentationChanged && !stateChanged && !liveDataChanged)
		{
			// An idle EndFrame changes the engine revision without changing any
			// profiler content. Remember it and skip all document copies.
			cache->profilerRevision = requestedRevision;
		}
		else
		{
			cache->settings = profiler.GetSettings();
			const std::optional<std::uint64_t> previousSelectedFrameIndex = cache->selectedFrame.has_value()
				? std::optional<std::uint64_t>{cache->selectedFrame->frameIndex}
				: std::nullopt;
			if (!liveCapture && !cache->selectedCapture.has_value())
			{
				// V1 capture history is exposed only through a selected journal index.
				// An idle global profiler is not a second document source for this UI.
				cache->frames.clear();
				cache->frameDurationStats = {};
				cache->frameOrderDirty = true;
			}
			else if (previousState != cache->state && cache->state == ProfilerSessionState::Recording)
			{
				// Starting a new capture clears stale rows once. The subwindow then stays
				// empty and stable until the recording is finalized.
				cache->frames.clear();
				cache->sortedFrames.clear();
				cache->frameDurationStats = {};
				cache->pinnedFrameIndex.reset();
				cache->selectedFrame.reset();
				cache->sampleAnalysisValid = false;
				cache->frameOrderDirty = true;
			}
			bool shouldCopySelectedFrame = false;
			if (cache->pinnedFrameIndex.has_value())
			{
				shouldCopySelectedFrame = !cache->selectedFrame.has_value()
					|| cache->selectedFrame->frameIndex != *cache->pinnedFrameIndex;
			}
			else if (!liveCapture && !cache->selectedCapture.has_value())
			{
				// Loaded and completed captures retain the established default frame
				// selection. Active recording has no selectable rows because the Frames
				// subwindow remains deferred until the capture is finalized.
				shouldCopySelectedFrame = false;
			}
			else
			{
				cache->selectedFrame.reset();
			}
			if (shouldCopySelectedFrame) cache->selectedFrame.reset();
			const std::optional<std::uint64_t> refreshedSelectedFrameIndex = cache->selectedFrame.has_value()
				? std::optional<std::uint64_t>{cache->selectedFrame->frameIndex}
				: std::nullopt;
			if (previousSelectedFrameIndex != refreshedSelectedFrameIndex)
			{
				// Completed frames are immutable. Preserve sample analysis when a
				// user is inspecting an older frame while recording continues.
				cache->sampleAnalysisValid = false;
			}
			if (cache->state != ProfilerSessionState::Recording || previousState != cache->state)
			{
				// Loading, clearing, pausing, or finishing changes the document as a
				// whole. Refresh an expanded evidence panel once for those discrete
				// events, while ordinary recorded frames remain explicitly cached.
				cache->timingDistributionValid = false;
				cache->sampleAnalysisValid = false;
			}

			// If a frame completed between the individual immutable reads, leave the
			// cache revision stale so the next editor frame immediately converges.
			const std::uint64_t completedRevision = profiler.GetSnapshotRevision();
			cache->profilerRevision = completedRevision == requestedRevision ? completedRevision : 0u;
		}
	}

	// Live renderer diagnostics are meaningful only beside a selected completed
	// frame. Skipping them in idle/empty state avoids both renderer reads and the
	// large diagnostic table work that previously ran whenever the window opened.
	if (!cache->selectedFrame.has_value())
	{
		return;
	}
	if (now < cache->nextRendererRefresh)
	{
		return;
	}

	// Compact renderer reads avoid copying virtual-geometry visibility records,
	// residency diagnostics, and string-heavy render graph evidence that this
	// window never displays.
	RenderSystem& renderer = RenderSystem::Instance();
	cache->rendererRecovery = renderer.GetLatestRendererRecoverySnapshot();
	cache->backendStageTimings = renderer.GetLatestBackendStageTimings();
	cache->gameStats = renderer.GetLatestRenderStats(RenderViewType::Game);
	cache->sceneStats = renderer.GetLatestRenderStats(RenderViewType::Scene);
	cache->nextRendererRefresh = now + kProfilerWindowRefreshInterval;
}

void ProfilerWindow::RefreshSampleAnalysis(const ProfilerFrameRecord& frame)
{
	if (cache->sampleAnalysisValid
		&& cache->selectedCapture.has_value()
		&& cache->sampleAnalysisArtifactId == cache->selectedCapture->artifactId
		&& cache->sampleAnalysisFrameIndex == frame.frameIndex
		&& cache->sampleAnalysisModule == selectedModule)
	{
		return;
	}

	// Build hierarchy, scheduler context, hot rows, and timeline lanes once for
	// the cached selected frame. Every diagnostic view shares this immutable work.
	cache->diagnosticAnalysis = BuildProfilerDiagnosticAnalysis(frame, selectedModule);
	cache->sampleAnalysisArtifactId = cache->selectedCapture.has_value()
		? cache->selectedCapture->artifactId : std::string{};
	cache->sampleAnalysisFrameIndex = frame.frameIndex;
	cache->sampleAnalysisModule = selectedModule;
	cache->sampleAnalysisValid = true;
}


void ProfilerWindow::OnGui()
{
	EditorProfilerService* service = nullptr;
	if (owner && owner->GetContext())
	{
		service = owner->GetContext()->editorProfiler.get();
	}
	ScopedProfilerWindowDrawTiming drawTiming(service);
	if (service != nullptr)
	{
		RefreshLiveSnapshot();
	}

	DrawToolbar();
	ImGui::Separator();


	if (service == nullptr || !cache->hasActiveProject)
	{
		ImGui::TextDisabled("Open a project to use the profiler.");
		return;
	}

	ImGui::Text("Status: %s", cache->statusText.c_str());

	if (!cache->lastError.empty())
	{
		ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", cache->lastError.c_str());
	}
	// Capture navigation is useful before a selection exists, but frame rows and
	// diagnostic panels belong to one immutable selected session. Returning here
	// prevents an empty project from looking like a zero-valued profiler capture.
	if (!cache->selectedCapture.has_value())
	{
		const bool captureSessionActive = cache->state == ProfilerSessionState::Recording
			|| cache->state == ProfilerSessionState::Paused
			|| cache->state == ProfilerSessionState::Finalizing;
		if (captureSessionActive)
		{
			// Live presentation deliberately withholds a selected journal until its
			// writer has completed. Avoid building saved-capture rows that cannot be
			// opened yet, which keeps recording-time UI work independent of catalog size.
			ImGui::TextDisabled("Capture browsing resumes after recording finalizes.");
			return;
		}
		DrawCaptureList();
		ImGui::TextDisabled("Select a project capture to inspect its frame index and details.");
		return;
	}

	constexpr ImGuiTableFlags layoutFlags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings;

	if (ImGui::BeginTable("ProfilerLayout", 2, layoutFlags, ImGui::GetContentRegionAvail()))
	{
		ImGui::TableSetupColumn("Capture Navigation", ImGuiTableColumnFlags_WidthStretch, 0.9f);
		ImGui::TableSetupColumn("Frame Analysis", ImGuiTableColumnFlags_WidthStretch, 2.7f);
		ImGui::TableNextRow();

		ImGui::TableSetColumnIndex(0);
		DrawCaptureList();
		ImGui::Separator();
		DrawFrameList();
		ImGui::TableSetColumnIndex(1);
		DrawFrameDetails();

		ImGui::EndTable();
	}
}


void ProfilerWindow::DrawToolbar()
{
	EditorProfilerService* service = owner && owner->GetContext() ? owner->GetContext()->editorProfiler.get() : nullptr;
	const bool hasService = service != nullptr;
	const bool hasProject = cache->hasActiveProject;
	const bool busy = cache->hasActiveOperation;
	const ProfilerSessionState state = cache->state;
	const bool canUseProject = hasProject && !busy;
	const bool recording = state == ProfilerSessionState::Recording;
	const bool paused = state == ProfilerSessionState::Paused;
	const bool captureSessionActive = recording || paused || state == ProfilerSessionState::Finalizing;
	const bool hasFrames = !cache->frames.empty();
	const char* unavailableReason = !hasService
		? "Profiler service is not available."
		: (!hasProject ? "Open a project before recording profiler captures." : (busy ? "Profiler service is finishing another action." : ""));
	const char* browseUnavailableReason = captureSessionActive
		? "Capture browsing resumes after recording finalizes."
		: unavailableReason;

	DrawToolbarGroupLabel("Live");
	if (ToolbarButton(recording ? "Recording Every Frame" : "Record Every Frame", canUseProject && !recording, unavailableReason))
	{
		cache->pinnedFrameIndex.reset();
		cache->selectedFrame.reset();
		cache->sampleAnalysisValid = false;
		service->StartRecording();
	}
	ImGui::SameLine();
	if (ToolbarButton("Pause", canUseProject && recording, recording ? "" : "Pause is available only while recording."))
	{
		service->PauseRecording();
	}
	ImGui::SameLine();
	if (ToolbarButton("Resume", canUseProject && paused, paused ? "" : "Resume is available only while paused."))
	{
		service->ResumeRecording();
	}
	ImGui::SameLine();
	if (ToolbarButton("Finish", canUseProject && (recording || paused), (recording || paused) ? "" : "Start or resume a capture before finishing it."))
	{
		service->FinishCapture();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	DrawToolbarGroupLabel("Captures");
	if (ToolbarButton("Refresh", canUseProject && !captureSessionActive, browseUnavailableReason))
	{
		service->RefreshCaptureList();
	}
	ImGui::SameLine();
	if (ToolbarButton("Clear", canUseProject && hasFrames, hasFrames ? "" : "No captured frames are available to clear."))
	{
		cache->pinnedFrameIndex.reset();
		cache->selectedFrame.reset();
		cache->sampleAnalysisValid = false;
		service->Clear();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	DrawToolbarGroupLabel("Status");
	ImGui::TextDisabled("%s", TerraEngine::Profiling::ToString(state));
}


void ProfilerWindow::DrawCaptureList()
{
	EditorProfilerService* service = owner && owner->GetContext() ? owner->GetContext()->editorProfiler.get() : nullptr;

	if (service == nullptr)
	{
		return;
	}

	ImGui::TextUnformatted("Project Captures");
	const float capturesHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.30f);
	ImGui::BeginChild("ProfilerCaptures", ImVec2(0, capturesHeight), true);

	if (cache->captures.empty())
	{
		ImGui::TextDisabled("No captures in %s", cache->profilerDirectory.string().c_str());
	}
	// The project may retain hundreds of completed sessions. Submit only rows in
	// the child viewport so an open idle profiler does not format every historic
	// file name and tooltip on every editor frame.
	ImGuiListClipper captureClipper;
	captureClipper.Begin(static_cast<int>(cache->captures.size()));
	while (captureClipper.Step())
	{
		for (int captureIndex = captureClipper.DisplayStart;
			captureIndex < captureClipper.DisplayEnd; ++captureIndex)
		{
			const auto& capture = cache->captures[static_cast<std::size_t>(captureIndex)];
			if (ImGui::Selectable(capture.fileName.c_str()))
			{
				cache->pinnedFrameIndex.reset();
				cache->selectedFrame.reset();
				cache->sampleAnalysisValid = false;
				service->LoadCapture(capture.absolutePath);
			}

			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%llu frames\n%s", static_cast<unsigned long long>(capture.frameCount), capture.metadata.savedAt.c_str());
			}
		}
	}
	ImGui::EndChild();
}


void ProfilerWindow::DrawFrameList()
{
	ImGui::TextUnformatted("Frames");
	ImGui::BeginChild("ProfilerFrames", ImVec2(0, 0), true);
	const bool captureActive = cache->state == ProfilerSessionState::Recording
		|| cache->state == ProfilerSessionState::Paused;
	if (captureActive)
	{
		ImGui::TextDisabled("Frame history will appear after the recording is finished.");
		ImGui::EndChild();
		return;
	}
	const std::vector<ProfilerFrameListItem>& frames = cache->frames;

	if (frames.empty())
	{
		ImGui::TextDisabled(cache->indexTotalFrameCount == 0u
			? "No frames captured."
			: "Loading the requested frame-index page.");
		ImGui::EndChild();
		return;
	}
	// Navigation requests at most one bounded page. It deliberately never walks
	// or sorts the history that is outside the page currently visible to ImGui.
	const bool hasPreviousPage = cache->indexPageOffset > 0u;
	const bool hasNextPage = cache->indexPageOffset + frames.size() < cache->indexTotalFrameCount;
	if (ImGui::Button("Previous Page") && hasPreviousPage)
	{
		const std::size_t previousOffset = cache->indexPageOffset > cache->indexPageSize
			? cache->indexPageOffset - cache->indexPageSize : 0u;
		if (EditorProfilerService* service = owner && owner->GetContext()
			? owner->GetContext()->editorProfiler.get() : nullptr)
		{
			service->RequestIndexPage(previousOffset, cache->indexPageSize);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Next Page") && hasNextPage)
	{
		if (EditorProfilerService* service = owner && owner->GetContext()
			? owner->GetContext()->editorProfiler.get() : nullptr)
		{
			service->RequestIndexPage(cache->indexPageOffset + frames.size(), cache->indexPageSize);
		}
	}
	ImGui::SameLine();
	ImGui::TextDisabled("Frames %zu-%zu of %zu", cache->indexPageOffset + 1u,
		cache->indexPageOffset + frames.size(), cache->indexTotalFrameCount);
	const FrameDurationStats& stats = cache->frameDurationStats;
	if (!frames.empty())
	{
		ImGui::TextDisabled("%zu complete  %zu partial  mean %.2f ms  p95 %.2f ms",
			stats.frameCount,
			stats.partialFrameCount,
			stats.meanMs,
			stats.p95Ms);
	}
	std::vector<ProfilerFrameListItem>& sortedFrames = cache->sortedFrames;
	std::optional<std::uint64_t> requestedFrameSelection;
	if (ImGui::BeginTable("ProfilerFrameRows", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable))
	{
		ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, 80.0f);
		ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, 82.0f);
		ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();
		bool frameIndexOrder = true;
		bool frameIndexAscending = false;
		if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs && sortSpecs->SpecsCount > 0)
		{
			const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
			frameIndexOrder = spec.ColumnIndex == 0;
			frameIndexAscending = spec.SortDirection == ImGuiSortDirection_Ascending;
			if (!frameIndexOrder && (cache->frameOrderDirty || sortSpecs->SpecsDirty))
			{
				const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
				sortedFrames = frames;
				std::sort(sortedFrames.begin(), sortedFrames.end(), [&](const ProfilerFrameListItem& left, const ProfilerFrameListItem& right) {
					int result = left.durationMs < right.durationMs ? -1 : (left.durationMs > right.durationMs ? 1 : 0);
					if (result == 0)
					{
						// Stable tie-breaks keep rows from jumping when two frames
						// have nearly identical timings.
						result = left.frameIndex < right.frameIndex ? -1 : (left.frameIndex > right.frameIndex ? 1 : 0);
					}
					return ascending ? result < 0 : result > 0;
				});
				cache->frameOrderDirty = false;
			}
			sortSpecs->SpecsDirty = false;
		}
		// The compact snapshot is already chronological. Frame-index sorting is
		// therefore a direct or reverse view and never needs an O(H log H) copy.
		const std::vector<ProfilerFrameListItem>& displayedFrames = frameIndexOrder ? frames : sortedFrames;
		// A completed capture can hold thousands of frames. Clip visible rows so
		// opening the profiler does not spend every editor frame formatting labels
		// that are outside the scroll viewport.
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(displayedFrames.size()));
		while (clipper.Step())
		{
			for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
			{
				const std::size_t displayIndex = frameIndexOrder && !frameIndexAscending
					? displayedFrames.size() - 1u - static_cast<std::size_t>(rowIndex)
					: static_cast<std::size_t>(rowIndex);
				const ProfilerFrameListItem& frame = displayedFrames[displayIndex];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				char label[64];
				std::snprintf(label, sizeof(label), "#%llu%s",
					static_cast<unsigned long long>(frame.frameIndex),
					frame.complete ? "" : "*");
				const bool selected = cache->pinnedFrameIndex == frame.frameIndex
					|| (cache->selectedFrame.has_value()
						&& cache->selectedFrame->frameIndex == frame.frameIndex);

				if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns))
				{
					requestedFrameSelection = frame.frameIndex;
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::TextColored(FrameDurationColor(frame.durationMs), "%.2f", frame.durationMs);
				ImGui::TableSetColumnIndex(2);
				DrawTimeBar(frame.durationMs, std::max(stats.p99Ms, 1.0));
			}
		}
		ImGui::EndTable();
	}
	if (requestedFrameSelection.has_value())
	{
		// Selection publishes a new profiler revision. The next OnGui call takes
		// one coherent detailed copy, then pins that immutable frame while the
		// compact live list continues to grow without repeated payload copies.
		cache->pinnedFrameIndex = *requestedFrameSelection;
		cache->selectedFrame.reset();
		cache->sampleAnalysisValid = false;
		if (EditorProfilerService* service = owner && owner->GetContext() ? owner->GetContext()->editorProfiler.get() : nullptr)
		{
			// V1 detail is loaded asynchronously from only the selected journal frame.
			service->RequestSelectedFrame(*requestedFrameSelection);
		}
	}
	ImGui::EndChild();
}


void ProfilerWindow::DrawFrameDetails()
{
	const std::optional<ProfilerFrameRecord>& selectedFrame = cache->selectedFrame;

	if (!selectedFrame.has_value())
	{
		const bool captureActive = cache->state == ProfilerSessionState::Recording
			|| cache->state == ProfilerSessionState::Paused;
		ImGui::TextDisabled(captureActive
			? "Frame analysis will become available after the recording is finished."
			: "Select a captured frame to inspect detailed scopes.");
		return;
	}
	// Renderer recovery is auxiliary context for a completed profiler frame.
	// Keep it behind the selection guard so idle windows never build this table.
	DrawRendererRecoverySummary();
	if (cache->pinnedFrameIndex.has_value())
	{
		ImGui::TextDisabled("Pinned frame #%llu; live frame-list updates remain compact.",
			static_cast<unsigned long long>(*cache->pinnedFrameIndex));
		ImGui::SameLine();
		if (ImGui::SmallButton("Return to Live List"))
		{
			cache->pinnedFrameIndex.reset();
			cache->selectedFrame.reset();
			cache->sampleAnalysisValid = false;
			return;
		}
	}
	// Capture-wide evidence is meaningful only after a capture session has
	// supplied a selected frame. Keeping it behind this guard prevents the empty
	// profiler state from presenting a fabricated zero-frame analysis.
	DrawCaptureEvidenceSummary();

	DrawFrameOverview(*selectedFrame);
	DrawPhysicsSummary(*selectedFrame);
	if (selectedFrame->animationOptimization.has_value()
		&& ImGui::TreeNodeEx("Animation Optimization Evidence", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const TerraEngine::Profiling::ProfilerAnimationOptimizationEvidence& evidence =
			*selectedFrame->animationOptimization;
		if (!evidence.valid)
		{
			ImGui::TextDisabled("unavailable");
		}
		else if (ImGui::BeginTable("ProfilerAnimationOptimizationEvidence", 2,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			auto row = [](const char* name, const char* format, auto... values) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(name);
				ImGui::TableSetColumnIndex(1);
				ImGui::Text(format, values...);
			};
			ImGui::TableSetupColumn("Evidence");
			ImGui::TableSetupColumn("Value");
			ImGui::TableHeadersRow();
			row("Animators", "%llu active; %llu suspended",
				static_cast<unsigned long long>(evidence.activeAnimators),
				static_cast<unsigned long long>(evidence.suspendedAnimators));
			row("Compact", "%llu / %llu eligible",
				static_cast<unsigned long long>(evidence.compactProcessedAnimators),
				static_cast<unsigned long long>(evidence.compactEligibleAnimators));
			row("Complete", "%llu / %llu captured",
				static_cast<unsigned long long>(evidence.completeProcessedAnimators),
				static_cast<unsigned long long>(evidence.completeCapturedAnimators));
			row("Transitions", "%llu compact->complete; %llu complete->compact",
				static_cast<unsigned long long>(evidence.compactToCompleteTransitions),
				static_cast<unsigned long long>(evidence.completeToCompactTransitions));
			row("Topology", "%llu hits; %llu misses",
				static_cast<unsigned long long>(evidence.retainedTopologyHits),
				static_cast<unsigned long long>(evidence.retainedTopologyMisses));
			row("GPU plan", "%llu groups; %llu dispatches",
				static_cast<unsigned long long>(evidence.compactPlanGroups),
				static_cast<unsigned long long>(evidence.compactDispatches));
			static constexpr std::array<const char*, 8U> kTimingLabels = {
				"Compact capture", "Compact evaluation", "Compact publication",
				"Complete capture", "Complete evaluation", "Complete publication",
				"Topology build", "Topology refresh"};
			for (std::size_t timingIndex = 0U;
				timingIndex < evidence.timingNanoseconds.size(); ++timingIndex)
			{
				row(kTimingLabels[timingIndex], "%.3f ms",
					static_cast<double>(evidence.timingNanoseconds[timingIndex]) / 1000000.0);
			}
			row("Rejected", "%llu candidates",
				static_cast<unsigned long long>(evidence.rejectedCandidateCount));
			row("Generations", "candidate %llu; accepted %llu; rejected %llu; topology %llu; presentation %llu; renderer %llu; scene %llu",
				static_cast<unsigned long long>(evidence.candidateGeneration),
				static_cast<unsigned long long>(evidence.acceptedGeneration),
				static_cast<unsigned long long>(evidence.rejectedGeneration),
				static_cast<unsigned long long>(evidence.topologyGeneration),
				static_cast<unsigned long long>(evidence.presentationGeneration),
				static_cast<unsigned long long>(evidence.rendererGeneration),
				static_cast<unsigned long long>(evidence.sceneGeneration));
			static constexpr std::array<const char*, 13U> kFallbackReasonLabels = {
				"None", "Missing asset", "Incompatible asset", "Allocation failure",
				"Budget pressure", "Cancelled", "Stale generation", "Renderer unavailable",
				"Renderer lost", "Device lost", "Physics unavailable", "Validation failure",
				"Internal failure"};
			for (std::size_t reasonIndex = 1U;
				reasonIndex < evidence.fallbackReasonCounts.size(); ++reasonIndex)
			{
				if (evidence.fallbackReasonCounts[reasonIndex] != 0U)
				{
					row(kFallbackReasonLabels[reasonIndex], "%llu",
						static_cast<unsigned long long>(evidence.fallbackReasonCounts[reasonIndex]));
				}
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}
	if (selectedFrame->renderOptimization.has_value()
		&& ImGui::TreeNodeEx("Renderer Optimization Evidence", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::BeginTable("ProfilerRendererOptimizationEvidence", 9,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("View");
			ImGui::TableSetupColumn("Instances");
			ImGui::TableSetupColumn("Submissions");
			ImGui::TableSetupColumn("Avoided");
			ImGui::TableSetupColumn("Stream");
			ImGui::TableSetupColumn("CPU Timings");
			ImGui::TableSetupColumn("Reuse / Invalidations");
			ImGui::TableSetupColumn("Rejected / Fallback");
			ImGui::TableSetupColumn("Generations");
			ImGui::TableHeadersRow();
			for (const TerraEngine::Profiling::ProfilerRenderOptimizationViewSummary& view
				: selectedFrame->renderOptimization->views)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(ToString(static_cast<RenderViewType>(view.viewType)));
				if (!view.valid)
				{
					ImGui::TableSetColumnIndex(1);
					ImGui::TextDisabled("unavailable");
					continue;
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%u = %u grouped + %u fallback", view.visibleInstanceCount,
					view.groupedInstanceCount, view.fallbackInstanceCount);
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%u grouped + %u fallback", view.groupedSubmissionCount,
					view.fallbackSubmissionCount);
				ImGui::TableSetColumnIndex(3);
				ImGui::Text("%u", view.avoidedSubmissionCount);
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%u / %u B", view.instanceStreamBytes, view.instanceStreamCapacityBytes);
				ImGui::TableSetColumnIndex(5);
				ImGui::Text("topology %.3f; materialize %.3f; upload %.3f; record %.3f ms",
					static_cast<double>(view.topologyBuildNanoseconds) / 1000000.0,
					static_cast<double>(view.dynamicMaterializationNanoseconds) / 1000000.0,
					static_cast<double>(view.instanceStreamUploadNanoseconds) / 1000000.0,
					static_cast<double>(view.groupedCommandRecordingNanoseconds) / 1000000.0);
				ImGui::TableSetColumnIndex(6);
				ImGui::Text("topology %u/%u; view %u/%u", view.topologyReuseHits,
					view.topologyInvalidations, view.viewEvidenceReuseHits,
					view.viewEvidenceInvalidations);
				ImGui::TableSetColumnIndex(7);
				const std::uint32_t rejected = view.rejectedIncompleteKeyCount
					+ view.rejectedUnsupportedVariantCount + view.rejectedOutOfRangeCount
					+ view.rejectedStaleGenerationCount;
				const std::uint32_t fallbacks = view.fallbackAllocationFailureCount
					+ view.fallbackRecoveryCount;
				ImGui::Text("%u / %u", rejected, fallbacks);
				ImGui::TableSetColumnIndex(8);
				ImGui::Text("frame %llu; topo %llu; view %llu; renderer %llu",
					static_cast<unsigned long long>(view.frameGeneration),
					static_cast<unsigned long long>(view.topologyGeneration),
					static_cast<unsigned long long>(view.viewGeneration),
					static_cast<unsigned long long>(view.rendererGeneration));
			}
			ImGui::EndTable();
		}
		ImGui::TextDisabled("Counts are captured with this frame; grouped and fallback instances must conserve the visible population.");
		ImGui::TreePop();
	}
	if (selectedFrame->framePublication.has_value()
		&& ImGui::TreeNodeEx("Frame Publication Evidence", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const auto& evidence = *selectedFrame->framePublication;
		ImGui::Text("Schema %u; availability 0x%llx; views %u", evidence.schemaVersion,
			static_cast<unsigned long long>(evidence.availabilityMask), evidence.viewCount);
		ImGui::Text("Generations: frame %llu scene %llu topology %llu authority %llu presentation %llu",
			static_cast<unsigned long long>(evidence.frameGeneration),
			static_cast<unsigned long long>(evidence.sceneGeneration),
			static_cast<unsigned long long>(evidence.topologyGeneration),
			static_cast<unsigned long long>(evidence.authorityGeneration),
			static_cast<unsigned long long>(evidence.presentationGeneration));
		ImGui::Text("Outcomes: render %u animation %u rejection %u", evidence.renderOutcome,
			evidence.animationOutcome, evidence.rejectionReason);
		ImGui::Text("Reuse/rebuild: persistent %u/%u dynamic %u/%u authority %u/%u views %u/%u",
			evidence.persistentReuseCount, evidence.persistentRebuildCount,
			evidence.dynamicReuseCount, evidence.dynamicRebuildCount,
			evidence.authorityReuseCount, evidence.authorityRebuildCount,
			evidence.viewSelectionReuseCount, evidence.viewSelectionRebuildCount);
		// Only coordinator-owned wall spans appear in this table. Worker samples stay
		// available in the hierarchy as explanatory detail, but are never added to
		// their enclosing phase because parallel ranges would inflate frame time.
		constexpr std::array<const char*, 7U> kTimingNames{
			"Persistent state maintenance",
			"Dynamic refresh",
			"View selection",
			"Animation evaluation",
			"Protected publication",
			"Backend preparation",
			"Uninstrumented remainder"
		};
		constexpr std::array<std::size_t, 7U> kTimingIndices{0U, 1U, 2U, 3U, 4U, 5U, 7U};
		const double frameNanoseconds = std::max(0.0, selectedFrame->durationMs * 1000000.0);
		std::uint64_t attributedNanoseconds = 0U;
		if (ImGui::BeginTable("ProfilerFramePublicationDisjointTimings", 3,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Disjoint phase");
			ImGui::TableSetupColumn("Wall time", ImGuiTableColumnFlags_WidthFixed, 100.0F);
			ImGui::TableSetupColumn("% frame", ImGuiTableColumnFlags_WidthFixed, 80.0F);
			ImGui::TableHeadersRow();
			for (std::size_t row = 0U; row < kTimingNames.size(); ++row)
			{
				const std::uint64_t nanoseconds =
					evidence.disjointTimingNanoseconds[kTimingIndices[row]];
				attributedNanoseconds += nanoseconds;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(kTimingNames[row]);
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%.3f ms", static_cast<double>(nanoseconds) / 1000000.0);
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%.1f%%", frameNanoseconds > 0.0
					? static_cast<double>(nanoseconds) * 100.0 / frameNanoseconds : 0.0);
			}
			ImGui::EndTable();
		}
		const double reconciliationErrorPercent = frameNanoseconds > 0.0
			? std::abs(static_cast<double>(attributedNanoseconds) - frameNanoseconds)
				* 100.0 / frameNanoseconds : 0.0;
		ImGui::Text("Reconciliation: %.3f / %.3f ms (error %.2f%%)",
			static_cast<double>(attributedNanoseconds) / 1000000.0,
			selectedFrame->durationMs, reconciliationErrorPercent);
		if (evidence.disjointTimingNanoseconds[6] != 0U)
		{
			ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.25F, 1.0F),
				"Invalid reserved timing lane: %.3f ms",
				static_cast<double>(evidence.disjointTimingNanoseconds[6]) / 1000000.0);
		}
		ImGui::TreePop();
	}

	const ScriptingSystem::ScriptingFrameResult& scriptingResult = ScriptingSystem::Instance().GetLatestScriptingFrameResult();
	if (scriptingResult.frameId != 0 && ImGui::TreeNodeEx("Scripting Dispatch Counters", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Text("Updated: %zu  Worker: %zu  Protected: %zu", scriptingResult.updatedScripts, scriptingResult.workerScripts, scriptingResult.protectedScripts);
		ImGui::Text("Cache hits: %zu  Misses: %zu  Bridge avoided: %zu", scriptingResult.cacheHits, scriptingResult.cacheMisses, scriptingResult.bridgeCallsAvoided);
		ImGui::Text("Typed writes: %zu  JSON fallbacks: %zu  Coalesced: %zu", scriptingResult.typedMutations, scriptingResult.jsonMutationFallbacks, scriptingResult.coalescedMutations);
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx("Visible Command Reuse", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::BeginTable("ProfilerVisibleCommandReuseRows", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("View");
			ImGui::TableSetupColumn("Cache");
			ImGui::TableSetupColumn("Commands");
			ImGui::TableSetupColumn("Copied");
			ImGui::TableSetupColumn("Unique Touches");
			ImGui::TableSetupColumn("Deduped");
			ImGui::TableSetupColumn("Diagnostics");
			ImGui::TableHeadersRow();
			const auto drawStatsRow = [](const RenderStatsSnapshot& stats) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(ToString(stats.viewType));
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(stats.visibleCommandCacheHit ? "hit" : "miss");
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%u", stats.visibleCommandCount);
				ImGui::TableSetColumnIndex(3);
				ImGui::Text("%u", stats.visibleCommandCopiedCount);
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%u", stats.visibleStreamingUniqueMeshes + stats.visibleStreamingUniqueMaterials + stats.visibleStreamingUniqueTextures);
				ImGui::TableSetColumnIndex(5);
				ImGui::Text("%u", stats.visibleStreamingDuplicateTouchesEliminated);
				ImGui::TableSetColumnIndex(6);
				ImGui::TextUnformatted(stats.detailedRenderDiagnosticsRequested ? "detailed" : "compact");
			};
			drawStatsRow(cache->gameStats);
			drawStatsRow(cache->sceneStats);
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}
	if (!cache->backendStageTimings.empty() && ImGui::TreeNodeEx("Backend Submit Path", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::BeginTable("ProfilerBackendSubmitStages", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Stage");
			ImGui::TableSetupColumn("View");
			ImGui::TableSetupColumn("Time");
			ImGui::TableSetupColumn("Backend");
			ImGui::TableHeadersRow();
			for (const TerraEngine::Renderer::BackendStageTiming& timing : cache->backendStageTimings)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(timing.label.empty() ? TerraEngine::Renderer::ToString(timing.stage) : timing.label.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(timing.viewType == RenderViewType::Scene ? "Scene" : "Game");
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%.3f ms", timing.milliseconds);
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(timing.backendId.c_str());
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}

	if (ImGui::BeginTabBar("ProfilerModules"))
	{
		for (ProfilerModuleId moduleId : {ProfilerModuleId::CpuEngine, ProfilerModuleId::TaskExecution})
		{

			if (ImGui::BeginTabItem(TerraEngine::Profiling::ToString(moduleId)))
			{
				selectedModule = moduleId;
				DrawModuleSummary(*selectedFrame);
				DrawDiagnosticSummary(*selectedFrame);
				DrawHotSamples(*selectedFrame);
				DrawTimeline(*selectedFrame);
				DrawSamples(*selectedFrame);
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();
	}
}

void ProfilerWindow::DrawCaptureEvidenceSummary()
{
	ImGui::SeparatorText("Capture Evidence");
	if (cache->journalSummary.has_value())
	{
		const auto& summary = *cache->journalSummary;
		ImGui::Text("V1 journal: %llu committed frames, %llu complete, %llu segments",
			static_cast<unsigned long long>(summary.frameCount),
			static_cast<unsigned long long>(summary.completeFrameCount),
			static_cast<unsigned long long>(summary.committedSegmentCount));
		ImGui::Text("%s capture", TerraEngine::Profiling::ToString(cache->settings.detailLevel));
		ImGui::Text("Duration %.3f ms  Dropped detail: %llu records / %llu B",
			static_cast<double>(summary.totalDurationNanoseconds) / 1000000.0,
			static_cast<unsigned long long>(summary.droppedDetailRecordCount),
			static_cast<unsigned long long>(summary.droppedDetailByteCount));
		ImGui::Text("Profiler cost: collect %.3f ms  seal %.3f ms  queue %.3f ms  persist %.3f ms",
			static_cast<double>(summary.profilerCollectionNanoseconds) / 1000000.0,
			static_cast<double>(summary.profilerSealingNanoseconds) / 1000000.0,
			static_cast<double>(summary.profilerQueueingNanoseconds) / 1000000.0,
			static_cast<double>(summary.profilerPersistenceNanoseconds) / 1000000.0);
		ImGui::Text("Persistence: %llu B in %llu operations, %llu B/s; peak lag %llu frames",
			static_cast<unsigned long long>(summary.persistedBytes),
			static_cast<unsigned long long>(summary.persistenceOperationCount),
			static_cast<unsigned long long>(summary.persistenceThroughputBytesPerSecond),
			static_cast<unsigned long long>(summary.persistenceLagHighWaterFrames));
		if (summary.committedSegmentCount != 0u
			&& summary.persistenceOperationCount != 0u
			&& summary.encodedPayloadBytes != 0u)
		{
			const double framesPerOperation = summary.persistenceOperationCount == 0u
				? 0.0
				: static_cast<double>(summary.frameCount)
					/ static_cast<double>(summary.persistenceOperationCount);
			const double storedRatio = summary.encodedPayloadBytes == 0u
				? 0.0
				: static_cast<double>(summary.storedSegmentBytes)
					/ static_cast<double>(summary.encodedPayloadBytes);
			ImGui::Text("Batching: %.2f frames/op; logical %llu B, encoded %llu B, stored %llu B (%.3fx)",
				framesPerOperation,
				static_cast<unsigned long long>(summary.logicalInputBytes),
				static_cast<unsigned long long>(summary.encodedPayloadBytes),
				static_cast<unsigned long long>(summary.storedSegmentBytes),
				storedRatio);
		}
		ImGui::Text("Engine CPU first/final 5 min: median %.3f / %.3f ms, P95 %.3f / %.3f ms",
			static_cast<double>(summary.firstEngineCpuWindowMedianNanoseconds) / 1000000.0,
			static_cast<double>(summary.finalEngineCpuWindowMedianNanoseconds) / 1000000.0,
			static_cast<double>(summary.firstEngineCpuWindowP95Nanoseconds) / 1000000.0,
			static_cast<double>(summary.finalEngineCpuWindowP95Nanoseconds) / 1000000.0);
		if (cache->selectedCapture && cache->selectedCapture->artifact.recoveredIncompleteTail)
			ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.25f, 1.0f), "Recovery warning: recovered incomplete tail; only committed segments are shown.");
		// V1 keeps capture-wide history bounded. Domains that do not yet publish a
		// persisted scalar aggregate remain explicit unavailable sections rather
		// than silently disappearing or being synthesized by rescanning all frames.
		static constexpr std::array<const char*, 5u> unavailableCaptureDomains = {
			"Physics, Scene View, and Editor Stages",
			"Protected Boundary Breakdown",
			"Worker Timeline Summary",
			"Renderer Optimization Summary",
			"Physics Work Volumes"};
		for (const char* domain : unavailableCaptureDomains)
		{
			if (ImGui::TreeNodeEx(domain, ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::TextDisabled("Capture-wide aggregate unavailable; select a frame for typed evidence.");
				ImGui::TreePop();
			}
		}
		return;
	}
	ImGui::TextDisabled("No persisted capture summary is selected.");
}

void ProfilerWindow::DrawPhysicsSummary(const TerraEngine::Profiling::ProfilerFrameRecord& frame)
{
	// Read captured fields instead of a live snapshot so replay works after the
	// world stops and never depends on the dedicated diagnostics window.
	if (!frame.physics.has_value())
	{
		ImGui::TextDisabled("No physics counters were recorded for this frame.");
		return;
	}
	const TerraEngine::Profiling::ProfilerPhysicsFrameCounters& physics = *frame.physics;
	if (!ImGui::TreeNodeEx("Physics Counters", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}
	ImGui::Text("Tick %llu  Bodies %llu  Contacts %llu  Queries %llu  Events %llu",
		static_cast<unsigned long long>(physics.tick), static_cast<unsigned long long>(physics.bodyCount),
		static_cast<unsigned long long>(physics.contactCount), static_cast<unsigned long long>(physics.queryCount),
		static_cast<unsigned long long>(physics.eventCount));
	ImGui::Text("Rejected %llu  Stale %llu  Command queue %llu",
		static_cast<unsigned long long>(physics.rejectedCount), static_cast<unsigned long long>(physics.staleCount),
		static_cast<unsigned long long>(physics.commandQueueDepth));
	ImGui::Text("Shape cache %llu B (hits %llu / misses %llu)",
		static_cast<unsigned long long>(physics.shapeCacheBytes),
		static_cast<unsigned long long>(physics.shapeCacheHits),
		static_cast<unsigned long long>(physics.shapeCacheMisses));
	ImGui::Text("Snapshots %llu B / %llu B high-water; provider allocations %llu / %llu high-water",
		static_cast<unsigned long long>(physics.snapshotBytes),
		static_cast<unsigned long long>(physics.snapshotHighWaterBytes),
		static_cast<unsigned long long>(physics.providerAllocations),
		static_cast<unsigned long long>(physics.providerAllocationHighWater));
	if (ImGui::BeginTable("ProfilerPhysicsTiming", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Prepare"); ImGui::TableSetupColumn("Commit"); ImGui::TableSetupColumn("Simulate");
		ImGui::TableSetupColumn("Queries"); ImGui::TableSetupColumn("Publish"); ImGui::TableHeadersRow(); ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0); ImGui::Text("%.3f ms", physics.prepareMs);
		ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f ms", physics.commitMs);
		ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f ms", physics.simulateMs);
		ImGui::TableSetColumnIndex(3); ImGui::Text("%.3f ms", physics.queryMs);
		ImGui::TableSetColumnIndex(4); ImGui::Text("%.3f ms", physics.publishMs);
		ImGui::EndTable();
	}
	ImGui::TreePop();
}

void ProfilerWindow::DrawRendererRecoverySummary()
{
	const TerraEngine::Renderer::RendererRecoverySnapshot& recovery = cache->rendererRecovery;
	if (!TerraEngine::Renderer::IsRendererRecoveryActive(recovery.state)
		&& recovery.finalOutcome == TerraEngine::Renderer::RendererRecoveryOutcome::None)
	{
		return;
	}

	ImGui::SeparatorText("Renderer Recovery");
	ImGui::Text("%s: %s", TerraEngine::Renderer::ToString(recovery.state), recovery.message.c_str());
	if (ImGui::BeginTable("ProfilerRendererRecovery", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Incident");
		ImGui::TableSetupColumn("Stage");
		ImGui::TableSetupColumn("Duplicates");
		ImGui::TableSetupColumn("Outcome");
		ImGui::TableSetupColumn("Dump");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(recovery.incidentId.empty() ? "-" : recovery.incidentId.c_str());
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%s  %s", TerraEngine::Renderer::ToString(recovery.firstFailureStage), recovery.firstFailureResult.c_str());
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%u", recovery.duplicateFailureCount);
		ImGui::TableSetColumnIndex(3);
		ImGui::TextUnformatted(TerraEngine::Renderer::ToString(recovery.finalOutcome));
		ImGui::TableSetColumnIndex(4);
		ImGui::TextUnformatted(recovery.crashDumpStatus.empty() ? "n/a" : recovery.crashDumpStatus.c_str());
		ImGui::EndTable();
	}
}


void ProfilerWindow::DrawFrameOverview(const ProfilerFrameRecord& frame)
{
	const FrameDurationStats& stats = cache->frameDurationStats;

	ImGui::SeparatorText("Frame Overview");
	ImGui::Text("Frame %llu", static_cast<unsigned long long>(frame.frameIndex));
	ImGui::SameLine();
	ImGui::TextColored(FrameDurationColor(frame.durationMs), "%.3f ms", frame.durationMs);
	if (!frame.complete)
	{
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "partial");
	}

	if (stats.frameCount == 0u)
	{
		ImGui::TextDisabled("No complete timing window is available yet.");
		return;
	}

	// Distribution analysis copies and sorts the rolling frame capture. Keep that
	// work behind an explicit disclosure so the normal profiler redraw stays
	// proportional to the lightweight frame list instead of the capture length.
	const bool timingDistributionOpen = ImGui::TreeNodeEx("Timing Distribution", ImGuiTreeNodeFlags_DefaultOpen);
	if (timingDistributionOpen)
	{
		if (cache->state == ProfilerSessionState::Recording
			|| cache->state == ProfilerSessionState::Paused
			|| cache->state == ProfilerSessionState::Finalizing)
		{
			// The selected frame may be a retained pre-recording selection. Do not
			// let that stale UI state request all detailed frames from a live capture.
			ImGui::TextDisabled("Timing distribution becomes available after finalization.");
			ImGui::TreePop();
			return;
		}
		const bool timingDistributionOpenedThisFrame = !cache->timingDistributionOpenLastFrame;
		cache->timingDistributionOpenLastFrame = true;
		const bool refreshTimingDistribution = ImGui::Button("Refresh Timing Distribution");
		if (timingDistributionOpenedThisFrame
			|| refreshTimingDistribution)
		{
			cache->timingDistributionValid = false;
		}
		if (!cache->timingDistributionValid)
		{
			// Aggregate the complete frame set only on explicit presentation
			// changes. The cached table remains stable while recording grows.
			// V1 timing aggregates belong to the persisted bounded summary. Do not
			// deep-copy every frame merely to populate an optional UI disclosure.
			cache->timingStreams.clear();
			cache->timingDistributionValid = true;
		}
		else
		{
			ImGui::SameLine();
			ImGui::TextDisabled("cached; refresh after selection or to include newer frames");
		}
		if (!cache->timingStreams.empty() && ImGui::BeginTable("ProfilerTimingStreams", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Timing");
			ImGui::TableSetupColumn("Selected");
			ImGui::TableSetupColumn("Mean");
			ImGui::TableSetupColumn("Median");
			ImGui::TableSetupColumn("P95");
			ImGui::TableSetupColumn("P99");
			ImGui::TableSetupColumn("Min");
			ImGui::TableSetupColumn("Max");
			ImGui::TableSetupColumn("Samples");
			ImGui::TableSetupColumn("Source");
			ImGui::TableHeadersRow();
			for (const ProfilerTimingStreamAggregate& stream : cache->timingStreams)
			{
				char selectedValue[64];
				char aggregateValue[64];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(stream.label.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(TimingDisplayValue(stream, selectedValue, sizeof(selectedValue)));
				if (!stream.selectedStatusReason.empty() && ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("%s", stream.selectedStatusReason.c_str());
				}
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(TimingAggregateValue(stream, stream.meanMs, aggregateValue, sizeof(aggregateValue)));
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(TimingAggregateValue(stream, stream.medianMs, aggregateValue, sizeof(aggregateValue)));
				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(TimingAggregateValue(stream, stream.p95Ms, aggregateValue, sizeof(aggregateValue)));
				ImGui::TableSetColumnIndex(5);
				ImGui::TextUnformatted(TimingAggregateValue(stream, stream.p99Ms, aggregateValue, sizeof(aggregateValue)));
				ImGui::TableSetColumnIndex(6);
				ImGui::TextUnformatted(TimingAggregateValue(stream, stream.minMs, aggregateValue, sizeof(aggregateValue)));
				ImGui::TableSetColumnIndex(7);
				ImGui::TextUnformatted(TimingAggregateValue(stream, stream.maxMs, aggregateValue, sizeof(aggregateValue)));
				ImGui::TableSetColumnIndex(8);
				ImGui::Text("%zu / %zu missing", stream.sampleCount, stream.missingCount);
				ImGui::TableSetColumnIndex(9);
				ImGui::TextUnformatted(TerraEngine::Profiling::ToString(stream.source));
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}
	else
	{
		cache->timingDistributionOpenLastFrame = false;
	}
	// Explain the accounting boundary beside the values it governs. The frame
	// duration excludes editor work; viewport CPU is nested and GPU can overlap.
	ImGui::TextDisabled("Game profiling: only Play Mode frames are captured. Frame = Engine CPU; Viewport CPU is included and GPU is independent.");

	if (ImGui::BeginTable("ProfilerFrameStats", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV))
	{
		DrawMetricCell("Mean", stats.meanMs, FrameDurationColor(stats.meanMs));
		DrawMetricCell("Median", stats.medianMs, FrameDurationColor(stats.medianMs));
		DrawMetricCell("P90", stats.p90Ms, FrameDurationColor(stats.p90Ms));
		DrawMetricCell("P95", stats.p95Ms, FrameDurationColor(stats.p95Ms));
		DrawMetricCell("P99", stats.p99Ms, FrameDurationColor(stats.p99Ms));
		ImGui::EndTable();
	}
	if (ImGui::BeginTable("ProfilerFrameStatsSecondary", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV))
	{
		DrawMetricCell("Min", stats.minMs, FrameDurationColor(stats.minMs));
		DrawMetricCell("Max", stats.maxMs, FrameDurationColor(stats.maxMs));
		DrawMetricCell("Std Dev", stats.stdDevMs, FrameDurationColor(stats.stdDevMs));
		ImGui::TableNextColumn();
		ImGui::TextDisabled("Complete");
		ImGui::Text("%zu", stats.frameCount);
		ImGui::TableNextColumn();
		ImGui::TextDisabled("Partial");
		ImGui::Text("%zu", stats.partialFrameCount);
		ImGui::EndTable();
	}

	ImGui::TextDisabled("Selected frame vs p95");
	DrawTimeBar(frame.durationMs, std::max(stats.p95Ms, 1.0));
}


void ProfilerWindow::DrawModuleSummary(const ProfilerFrameRecord& frame)
{
	const TerraEngine::Profiling::ProfilerModuleSummary* moduleSummary = FindModuleSummary(frame, selectedModule);

	if (moduleSummary == nullptr || !moduleSummary->available)
	{
		ImGui::TextDisabled("No data recorded for this module.");
		return;
	}


	if (ImGui::BeginTable("ProfilerModuleMetrics", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Metric");
		ImGui::TableSetupColumn("Value");
		ImGui::TableSetupColumn("Unit");
		ImGui::TableHeadersRow();

		for (const auto& metric : moduleSummary->metrics)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(metric.name.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.3f", metric.value);
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(metric.unit.c_str());
		}
		ImGui::EndTable();
	}
}

void ProfilerWindow::DrawDiagnosticSummary(const ProfilerFrameRecord& frame)
{
	RefreshSampleAnalysis(frame);
	const ProfilerDiagnosticAnalysis& analysis = cache->diagnosticAnalysis;
	ImGui::SeparatorText("Diagnosis");
	if (analysis.rows.empty())
	{
		ImGui::TextDisabled("No detailed scopes were captured for this module.");
		return;
	}

	const double coveragePercent = SafePercent(analysis.capturedWallMs, frame.durationMs);
	if (ImGui::BeginTable("ProfilerDiagnosisSummary", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Frame wall");
		ImGui::TableSetupColumn("Captured wall");
		ImGui::TableSetupColumn("CPU work");
		ImGui::TableSetupColumn("Coverage");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0); ImGui::Text("%.3f ms", frame.durationMs);
		ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f ms", analysis.capturedWallMs);
		ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f ms", analysis.cpuWorkMs);
		ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f%%", coveragePercent);
		ImGui::EndTable();
	}

	if (!analysis.hotRowIndices.empty())
	{
		const ProfilerDiagnosticRow& offender = analysis.rows[analysis.hotRowIndices.front()];
		ImGui::Text("Primary self-time offender: %s (%.3f ms self, %.3f ms inclusive)",
			offender.name.c_str(), offender.selfMs, offender.inclusiveMs);
		ImGui::TextDisabled("Thread: %s  Phase: %s  Path: %s",
			offender.threadLabel.c_str(), offender.phase.empty() ? "-" : offender.phase.c_str(),
			offender.executionPath.empty() ? "-" : offender.executionPath.c_str());
	}
	if (analysis.uncoveredFrameMs > std::max(0.10, frame.durationMs * 0.01))
	{
		ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
			"%.3f ms of this frame has no scope coverage.", analysis.uncoveredFrameMs);
	}

	ImGui::SetNextItemWidth(260.0f);
	ImGui::InputTextWithHint("##ProfilerSampleSearch", "Search scope, phase, path, or thread",
		cache->sampleSearch.data(), cache->sampleSearch.size());
	ImGui::SameLine();
	ImGui::SetNextItemWidth(110.0f);
	ImGui::DragFloat("Min ms", &cache->minimumSampleDurationMs, 0.01f, 0.0f,
		static_cast<float>(std::max(1.0, frame.durationMs)), "%.2f");
	ImGui::SameLine();
	ImGui::Checkbox("Show uninstrumented", &cache->showUninstrumentedRows);
}


void ProfilerWindow::DrawHotSamples(const ProfilerFrameRecord& frame)
{
	// Sorting every captured scope is analysis work, not ordinary live display.
	// Keep it behind an explicit disclosure and reuse the result for the full
	// hierarchy if the user opens both sections for the same pinned frame.
	// The three primary diagnostic views should be immediately useful when a
	// developer opens a frame. ImGui still remembers any manual collapse made
	// after this first appearance.
	if (!ImGui::TreeNodeEx("Top Offenders", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextDisabled("Expand to sort the cached frame's hottest scopes.");
		return;
	}
	RefreshSampleAnalysis(frame);
	const ProfilerDiagnosticAnalysis& analysis = cache->diagnosticAnalysis;
	const std::string query(cache->sampleSearch.data());
	std::vector<std::size_t> visibleRows;
	visibleRows.reserve(analysis.hotRowIndices.size());
	for (const std::size_t rowIndex : analysis.hotRowIndices)
	{
		if (ProfilerDiagnosticRowMatches(analysis.rows[rowIndex], query, cache->minimumSampleDurationMs))
		{
			visibleRows.push_back(rowIndex);
		}
	}
	if (visibleRows.empty())
	{
		ImGui::TextDisabled("No samples match the current diagnostic filter.");
		ImGui::TreePop();
		return;
	}
	// Inclusive rows contain their child scopes, so adding rows would count the
	// same execution interval more than once. Self time is the non-overlapping cost.
	ImGui::TextDisabled("Inclusive timings are nested and must not be added together; Self excludes child scopes.");

	const std::size_t rowLimit = std::min<std::size_t>(visibleRows.size(), 20u);
	if (ImGui::BeginTable("ProfilerHotSamples", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch, 2.5f);
		ImGui::TableSetupColumn("Self");
		ImGui::TableSetupColumn("Incl.");
		ImGui::TableSetupColumn("% Frame");
		ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Thread");
		ImGui::TableSetupColumn("Phase");
		ImGui::TableSetupColumn("Path");
		ImGui::TableHeadersRow();
		for (std::size_t rowIndex = 0u; rowIndex < rowLimit; ++rowIndex)
		{
			const ProfilerDiagnosticRow& row = analysis.rows[visibleRows[rowIndex]];
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(row.name.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.3f", row.selfMs);
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%.3f", row.inclusiveMs);
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%.1f%%", SafePercent(row.inclusiveMs, frame.durationMs));
			ImGui::TableSetColumnIndex(4);
			DrawTimeBar(row.selfMs, std::max(frame.durationMs, 1.0));
			ImGui::TableSetColumnIndex(5);
			ImGui::TextUnformatted(row.threadLabel.c_str());
			if (ImGui::IsItemHovered() && row.rawThreadLabel != row.threadLabel)
			{
				ImGui::SetTooltip("Captured thread: %s", row.rawThreadLabel.c_str());
			}
			ImGui::TableSetColumnIndex(6);
			ImGui::TextUnformatted(row.phase.empty() ? "-" : row.phase.c_str());
			ImGui::TableSetColumnIndex(7);
			ImGui::TextUnformatted(row.executionPath.empty() ? "-" : row.executionPath.c_str());
		}
		ImGui::EndTable();
	}
	ImGui::TreePop();
}

void ProfilerWindow::DrawTimeline(const ProfilerFrameRecord& frame)
{
	if (!ImGui::TreeNodeEx("Thread Timeline", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextDisabled("Expand to correlate overlapping work and scheduler paths chronologically.");
		return;
	}
	RefreshSampleAnalysis(frame);
	const ProfilerDiagnosticAnalysis& analysis = cache->diagnosticAnalysis;
	const std::string query(cache->sampleSearch.data());
	constexpr float kLabelWidth = 96.0f;
	constexpr float kBarHeight = 12.0f;
	constexpr std::uint32_t kVisibleDepthLimit = 8u;
	ImGui::TextDisabled("Horizontal position is frame time; nested scopes stack downward. Hover a bar for exact context.");
	ImGui::BeginChild("ProfilerThreadTimelineScroll", ImVec2(0.0f, 280.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
	for (std::size_t laneIndex = 0u; laneIndex < analysis.timelineLanes.size(); ++laneIndex)
	{
		const auto& lane = analysis.timelineLanes[laneIndex];
		const float laneHeight = 20.0f + kBarHeight * static_cast<float>(std::min(lane.maximumDepth, kVisibleDepthLimit) + 1u);
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float width = std::max(320.0f, ImGui::GetContentRegionAvail().x);
		ImGui::PushID(static_cast<int>(laneIndex));
		ImGui::InvisibleButton("TimelineLane", ImVec2(width, laneHeight));
		ImGui::PopID();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddText(origin, ImGui::GetColorU32(ImGuiCol_Text), lane.label.c_str());
		const float plotBeginX = origin.x + kLabelWidth;
		const float plotWidth = std::max(1.0f, width - kLabelWidth);
		drawList->AddRectFilled(ImVec2(plotBeginX, origin.y), ImVec2(origin.x + width, origin.y + laneHeight),
			ImGui::GetColorU32(ImGuiCol_FrameBg));
		for (const std::size_t nodeIndex : lane.nodeIndices)
		{
			const ProfilerDiagnosticRow& row = analysis.rows[nodeIndex];
			if (!ProfilerDiagnosticRowMatches(row, query, cache->minimumSampleDurationMs))
			{
				continue;
			}
			const ProfilerSampleHierarchyNode& node = analysis.hierarchy.nodes[nodeIndex];
			const ProfilerSample& sample = analysis.hierarchy.samples[node.sampleIndex];
			const float startFraction = static_cast<float>(std::clamp(sample.startMs / std::max(frame.durationMs, 0.001), 0.0, 1.0));
			const float endFraction = static_cast<float>(std::clamp((sample.startMs + sample.durationMs) / std::max(frame.durationMs, 0.001), 0.0, 1.0));
			const float x0 = plotBeginX + startFraction * plotWidth;
			const float x1 = std::max(x0 + 1.0f, plotBeginX + endFraction * plotWidth);
			const float y0 = origin.y + 3.0f + kBarHeight * static_cast<float>(std::min(node.presentationDepth, kVisibleDepthLimit));
			const ImVec2 barMin(x0, y0);
			const ImVec2 barMax(x1, y0 + kBarHeight - 2.0f);
			const ImU32 color = ImGui::GetColorU32(ImVec4(0.20f + 0.06f * static_cast<float>(node.presentationDepth % 5u), 0.55f, 0.90f, 1.0f));
			drawList->AddRectFilled(barMin, barMax, color, 2.0f);
			if (ImGui::IsMouseHoveringRect(barMin, barMax))
			{
				ImGui::SetTooltip("%s\nInclusive %.3f ms | Self %.3f ms\n%s | %s | %s\nRaw thread: %s",
					row.name.c_str(), row.inclusiveMs, row.selfMs, row.threadLabel.c_str(),
					row.phase.empty() ? "No phase" : row.phase.c_str(),
					row.executionPath.empty() ? "No scheduler path" : row.executionPath.c_str(), row.rawThreadLabel.c_str());
			}
		}
	}
	ImGui::EndChild();
	ImGui::TreePop();
}


void ProfilerWindow::DrawSamples(const ProfilerFrameRecord& frame)
{
	ImGui::SeparatorText("Samples");
	// Open both the disclosure and every nested sample parent by default so the
	// complete captured hierarchy is visible without repetitive expansion.
	if (!ImGui::TreeNodeEx("Sample Hierarchy", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextDisabled("Expand to inspect the cached scope tree.");
		return;
	}
	RefreshSampleAnalysis(frame);
	const ProfilerDiagnosticAnalysis& analysis = cache->diagnosticAnalysis;
	const ProfilerSampleHierarchy& tree = analysis.hierarchy;

	if (tree.samples.empty())
	{
		ImGui::TextDisabled("No samples for this module.");
		ImGui::TreePop();
		return;
	}

	const ImVec2 originalCellPadding = ImGui::GetStyle().CellPadding;
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(1.0f, originalCellPadding.y));

	if (ImGui::BeginTable("ProfilerSamples", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.5f);
		ImGui::TableSetupColumn("Incl.");
		ImGui::TableSetupColumn("Self");
		ImGui::TableSetupColumn("% Frame");
		ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Thread");
		ImGui::TableSetupColumn("Phase");
		ImGui::TableSetupColumn("Path");
		ImGui::TableHeadersRow();

		for (const std::size_t rootIndex : tree.rootIndices)
		{
			DrawSampleTreeRow(analysis, rootIndex, frame.frameIndex, frame.durationMs,
				std::string(cache->sampleSearch.data()), cache->minimumSampleDurationMs,
				cache->showUninstrumentedRows);
		}
		ImGui::EndTable();
	}
	ImGui::PopStyleVar();
	ImGui::TreePop();
}
