#pragma once

#include "ProfilerSampleHierarchy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace TerraEngine::EditorProfiling
{
	// One diagnostic row combines a validated scope with the scheduler metadata
	// that explains where and how it ran. Indices remain stable for the lifetime
	// of the owning analysis and avoid pointers into replaceable frame snapshots.
	struct ProfilerDiagnosticRow
	{
		std::size_t nodeIndex = 0u;
		std::string name;
		std::string threadLabel;
		std::string rawThreadLabel;
		std::string phase;
		std::string executionPath;
		double inclusiveMs = 0.0;
		double selfMs = 0.0;
	};

	// Timeline lanes group chronological scopes by their capture thread. The
	// friendly label is presentation-only; the raw identity remains available in
	// tooltips when a developer needs to correlate another diagnostic stream.
	struct ProfilerTimelineLane
	{
		std::string label;
		std::string rawLabel;
		std::uint32_t maximumDepth = 0u;
		std::vector<std::size_t> nodeIndices;
	};

	// ProfilerDiagnosticAnalysis is computed once for a pinned immutable frame.
	// CPU work sums non-overlapping intervals per thread, while wall coverage
	// merges all threads; keeping both prevents parallel work from looking like an
	// impossible frame duration or nested scopes from being counted repeatedly.
	struct ProfilerDiagnosticAnalysis
	{
		ProfilerSampleHierarchy hierarchy;
		std::vector<ProfilerDiagnosticRow> rows;
		std::vector<std::size_t> hotRowIndices;
		std::vector<ProfilerTimelineLane> timelineLanes;
		double capturedWallMs = 0.0;
		double cpuWorkMs = 0.0;
		double uncoveredFrameMs = 0.0;
	};

	inline const char* ProfilerExecutionPathName(Profiling::WorkloadExecutionPath path)
	{
		switch (path)
		{
		case Profiling::WorkloadExecutionPath::Inline: return "Inline";
		case Profiling::WorkloadExecutionPath::Worker: return "Worker";
		case Profiling::WorkloadExecutionPath::Protected: return "Protected";
		case Profiling::WorkloadExecutionPath::Rejected: return "Rejected";
		case Profiling::WorkloadExecutionPath::None: return "Not dispatched";
		}
		return "Unknown";
	}

	inline double MergeProfilerIntervals(std::vector<std::pair<double, double>> intervals)
	{
		intervals.erase(std::remove_if(intervals.begin(), intervals.end(), [](const auto& interval) {
			return !std::isfinite(interval.first) || !std::isfinite(interval.second)
				|| interval.second <= interval.first;
		}), intervals.end());
		if (intervals.empty())
		{
			return 0.0;
		}
		std::sort(intervals.begin(), intervals.end());
		double coveredMs = 0.0;
		double beginMs = intervals.front().first;
		double endMs = intervals.front().second;
		for (std::size_t index = 1u; index < intervals.size(); ++index)
		{
			if (intervals[index].first <= endMs)
			{
				endMs = std::max(endMs, intervals[index].second);
				continue;
			}
			coveredMs += endMs - beginMs;
			beginMs = intervals[index].first;
			endMs = intervals[index].second;
		}
		return coveredMs + endMs - beginMs;
	}

	inline bool ProfilerTextContains(const std::string& text, const std::string& query)
	{
		if (query.empty())
		{
			return true;
		}
		auto normalize = [](unsigned char value) { return static_cast<char>(std::tolower(value)); };
		std::string normalizedText(text.size(), '\0');
		std::string normalizedQuery(query.size(), '\0');
		std::transform(text.begin(), text.end(), normalizedText.begin(), normalize);
		std::transform(query.begin(), query.end(), normalizedQuery.begin(), normalize);
		return normalizedText.find(normalizedQuery) != std::string::npos;
	}

	inline bool ProfilerDiagnosticRowMatches(
		const ProfilerDiagnosticRow& row,
		const std::string& query,
		double minimumDurationMs)
	{
		if (row.inclusiveMs < std::max(0.0, minimumDurationMs))
		{
			return false;
		}
		return ProfilerTextContains(row.name, query)
			|| ProfilerTextContains(row.threadLabel, query)
			|| ProfilerTextContains(row.rawThreadLabel, query)
			|| ProfilerTextContains(row.phase, query)
			|| ProfilerTextContains(row.executionPath, query);
	}

	inline ProfilerDiagnosticAnalysis BuildProfilerDiagnosticAnalysis(
		const Profiling::ProfilerFrameRecord& frame,
		Profiling::ProfilerModuleId selectedModule)
	{
		ProfilerDiagnosticAnalysis analysis;
		analysis.hierarchy = BuildProfilerSampleHierarchy(frame, selectedModule);
		analysis.rows.resize(analysis.hierarchy.nodes.size());

		std::string mainThreadRawLabel;
		for (const Profiling::ProfilerSample& sample : analysis.hierarchy.samples)
		{
			if (sample.name == "Engine::Update" || sample.name == "Engine Frame")
			{
				mainThreadRawLabel = sample.threadLabel;
				break;
			}
		}

		std::unordered_map<std::string, std::string> friendlyThreads;
		std::unordered_map<std::string, const Profiling::WorkloadExecutionRecord*> executionByLabel;
		executionByLabel.reserve(frame.workloadExecutions.size() * 2u);
		for (const Profiling::WorkloadExecutionRecord& execution : frame.workloadExecutions)
		{
			if (!execution.workloadName.empty()) executionByLabel.try_emplace(execution.workloadName, &execution);
			if (!execution.ownerSystem.empty()) executionByLabel.try_emplace(execution.ownerSystem, &execution);
		}
		std::uint32_t nextUnknownThread = 1u;
		for (std::size_t nodeIndex = 0u; nodeIndex < analysis.hierarchy.nodes.size(); ++nodeIndex)
		{
			const ProfilerSampleHierarchyNode& node = analysis.hierarchy.nodes[nodeIndex];
			const Profiling::ProfilerSample& sample = analysis.hierarchy.samples[node.sampleIndex];
			const Profiling::WorkloadExecutionRecord* execution = nullptr;
			const auto taskExecution = executionByLabel.find(sample.taskLabel);
			const auto namedExecution = executionByLabel.find(ProfilerSampleDisplayName(sample));
			if (!sample.taskLabel.empty() && taskExecution != executionByLabel.end()) execution = taskExecution->second;
			else if (namedExecution != executionByLabel.end()) execution = namedExecution->second;
			std::string friendlyThread;
			if (!mainThreadRawLabel.empty() && sample.threadLabel == mainThreadRawLabel)
			{
				friendlyThread = "Main Thread";
			}
			else if (execution != nullptr && execution->workerIdentityAvailable)
			{
				friendlyThread = "Worker " + std::to_string(execution->workerId);
			}
			else if (sample.threadLabel == "main")
			{
				friendlyThread = "Main Thread";
			}
			else
			{
				auto [known, inserted] = friendlyThreads.try_emplace(
					sample.threadLabel, "Thread " + std::to_string(nextUnknownThread));
				if (inserted)
				{
					++nextUnknownThread;
				}
				friendlyThread = known->second;
			}
			friendlyThreads[sample.threadLabel] = friendlyThread;

			ProfilerDiagnosticRow& row = analysis.rows[nodeIndex];
			row.nodeIndex = nodeIndex;
			row.name = ProfilerSampleDisplayName(sample);
			row.threadLabel = friendlyThread;
			row.rawThreadLabel = sample.threadLabel;
			row.inclusiveMs = std::max(0.0, sample.durationMs);
			row.selfMs = node.selfDurationMs;
			if (execution != nullptr)
			{
				row.phase = execution->phase;
				row.executionPath = ProfilerExecutionPathName(execution->selectedPath);
			}
			analysis.hotRowIndices.push_back(nodeIndex);
		}

		std::stable_sort(analysis.hotRowIndices.begin(), analysis.hotRowIndices.end(), [&](std::size_t left, std::size_t right) {
			const ProfilerDiagnosticRow& leftRow = analysis.rows[left];
			const ProfilerDiagnosticRow& rightRow = analysis.rows[right];
			return leftRow.selfMs == rightRow.selfMs
				? leftRow.inclusiveMs > rightRow.inclusiveMs
				: leftRow.selfMs > rightRow.selfMs;
		});

		std::unordered_map<std::string, std::size_t> laneByThread;
		std::unordered_map<std::string, std::vector<std::pair<double, double>>> intervalsByThread;
		std::vector<std::pair<double, double>> wallIntervals;
		for (std::size_t nodeIndex = 0u; nodeIndex < analysis.hierarchy.nodes.size(); ++nodeIndex)
		{
			const ProfilerDiagnosticRow& row = analysis.rows[nodeIndex];
			const ProfilerSampleHierarchyNode& node = analysis.hierarchy.nodes[nodeIndex];
			const Profiling::ProfilerSample& sample = analysis.hierarchy.samples[node.sampleIndex];
			const double endMs = sample.startMs + std::max(0.0, sample.durationMs);
			wallIntervals.emplace_back(sample.startMs, endMs);
			intervalsByThread[row.rawThreadLabel].emplace_back(sample.startMs, endMs);

			auto [laneEntry, inserted] = laneByThread.try_emplace(row.rawThreadLabel, analysis.timelineLanes.size());
			if (inserted)
			{
				analysis.timelineLanes.push_back({row.threadLabel, row.rawThreadLabel, 0u, {}});
			}
			ProfilerTimelineLane& lane = analysis.timelineLanes[laneEntry->second];
			lane.maximumDepth = std::max(lane.maximumDepth, node.presentationDepth);
			lane.nodeIndices.push_back(nodeIndex);
		}
		analysis.capturedWallMs = MergeProfilerIntervals(std::move(wallIntervals));
		for (auto& [thread, intervals] : intervalsByThread)
		{
			(void)thread;
			analysis.cpuWorkMs += MergeProfilerIntervals(std::move(intervals));
		}
		analysis.uncoveredFrameMs = std::max(0.0, frame.durationMs - analysis.capturedWallMs);
		return analysis;
	}
}
