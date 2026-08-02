#pragma once

#include "Engine/Profiling/ProfilerCapture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace TerraEngine::EditorProfiling
{
	// A hierarchy node references the immutable sample copy stored by its owning
	// hierarchy. The presentation depth is rebuilt from validated parent links;
	// capture-provided depth is evidence, not a safe UI indentation value.
	struct ProfilerSampleHierarchyNode
	{
		std::size_t sampleIndex = 0u;
		std::vector<std::size_t> childIndices;
		double selfDurationMs = 0.0;
		std::uint32_t presentationDepth = 0u;
	};

	// The editor keeps samples and their tree in one value so cached frame
	// analysis never retains pointers into a profiler snapshot that may be replaced.
	struct ProfilerSampleHierarchy
	{
		std::vector<Profiling::ProfilerSample> samples;
		std::vector<ProfilerSampleHierarchyNode> nodes;
		std::vector<std::size_t> rootIndices;
	};

	inline bool IsProfilerJobWrapper(const std::string& name)
	{
		return name == "JobGraph::ProtectedJob"
			|| name == "JobGraph::WorkerJob"
			|| name == "ThreadPool Task"
			|| name == "SystemScheduler: Build System Declaration";
	}

	// Scheduler wrappers carry the useful workload name in taskLabel. Presenting
	// that name first turns repeated "ProtectedJob" rows into the actual systems
	// and phases developers need to inspect.
	inline const std::string& ProfilerSampleDisplayName(const Profiling::ProfilerSample& sample)
	{
		if (IsProfilerJobWrapper(sample.name)
			&& !sample.taskLabel.empty()
			&& sample.taskLabel != sample.name)
		{
			return sample.taskLabel;
		}
		return sample.name;
	}

	// When a workload label replaces a generic wrapper in the Name column, retain
	// the wrapper here as context. Ordinary samples continue to expose taskLabel.
	inline const std::string& ProfilerSampleContextName(const Profiling::ProfilerSample& sample)
	{
		if (&ProfilerSampleDisplayName(sample) == &sample.taskLabel)
		{
			return sample.name;
		}
		return sample.taskLabel;
	}

	inline bool ProfilerSampleIntervalContains(
		const Profiling::ProfilerSample& parent,
		const Profiling::ProfilerSample& child)
	{
		constexpr double kTimestampToleranceMs = 0.001;
		if (!std::isfinite(parent.startMs) || !std::isfinite(parent.durationMs)
			|| !std::isfinite(child.startMs) || !std::isfinite(child.durationMs))
		{
			return false;
		}
		const double parentEndMs = parent.startMs + std::max(0.0, parent.durationMs);
		const double childEndMs = child.startMs + std::max(0.0, child.durationMs);
		return parent.startMs <= child.startMs + kTimestampToleranceMs
			&& parentEndMs + kTimestampToleranceMs >= childEndMs;
	}

	inline bool ProfilerParentChainFormsCycle(
		std::size_t childIndex,
		std::size_t parentIndex,
		const std::vector<std::size_t>& parentIndices)
	{
		const std::size_t noParent = std::numeric_limits<std::size_t>::max();
		std::size_t cursor = parentIndex;
		for (std::size_t step = 0u; step <= parentIndices.size() && cursor != noParent; ++step)
		{
			if (cursor == childIndex)
			{
				return true;
			}
			cursor = parentIndices[cursor];
		}
		return cursor != noParent;
	}

	inline double CalculateProfilerSampleSelfDuration(
		const ProfilerSampleHierarchy& hierarchy,
		const ProfilerSampleHierarchyNode& node)
	{
		const Profiling::ProfilerSample& parent = hierarchy.samples[node.sampleIndex];
		const double parentDurationMs = std::max(0.0, parent.durationMs);
		const double parentEndMs = parent.startMs + parentDurationMs;
		std::vector<std::pair<double, double>> childIntervals;
		childIntervals.reserve(node.childIndices.size());
		for (const std::size_t childIndex : node.childIndices)
		{
			const Profiling::ProfilerSample& child = hierarchy.samples[hierarchy.nodes[childIndex].sampleIndex];
			const double beginMs = std::clamp(child.startMs, parent.startMs, parentEndMs);
			const double endMs = std::clamp(child.startMs + std::max(0.0, child.durationMs), parent.startMs, parentEndMs);
			if (endMs > beginMs)
			{
				childIntervals.emplace_back(beginMs, endMs);
			}
		}
		std::sort(childIntervals.begin(), childIntervals.end());

		// Merge intervals before subtracting them. Corrupt or rounded captures can
		// contain overlapping siblings, and subtracting both would under-report self.
		double coveredMs = 0.0;
		if (!childIntervals.empty())
		{
			double mergedBeginMs = childIntervals.front().first;
			double mergedEndMs = childIntervals.front().second;
			for (std::size_t intervalIndex = 1u; intervalIndex < childIntervals.size(); ++intervalIndex)
			{
				const auto& interval = childIntervals[intervalIndex];
				if (interval.first <= mergedEndMs)
				{
					mergedEndMs = std::max(mergedEndMs, interval.second);
					continue;
				}
				coveredMs += mergedEndMs - mergedBeginMs;
				mergedBeginMs = interval.first;
				mergedEndMs = interval.second;
			}
			coveredMs += mergedEndMs - mergedBeginMs;
		}
		return std::max(0.0, parentDurationMs - coveredMs);
	}

	inline ProfilerSampleHierarchy BuildProfilerSampleHierarchy(
		const Profiling::ProfilerFrameRecord& frame,
		Profiling::ProfilerModuleId selectedModule)
	{
		ProfilerSampleHierarchy hierarchy;
		for (const Profiling::ProfilerSample& sample : frame.samples)
		{
			if (sample.moduleId == selectedModule)
			{
				hierarchy.samples.push_back(sample);
			}
		}
		std::stable_sort(hierarchy.samples.begin(), hierarchy.samples.end(), [](const auto& left, const auto& right) {
			if (left.startMs != right.startMs)
			{
				return left.startMs < right.startMs;
			}
			if (left.depth != right.depth)
			{
				return left.depth < right.depth;
			}
			return left.sampleId < right.sampleId;
		});

		hierarchy.nodes.resize(hierarchy.samples.size());
		std::unordered_map<std::uint64_t, std::size_t> uniqueIndexById;
		std::unordered_set<std::uint64_t> ambiguousIds;
		uniqueIndexById.reserve(hierarchy.samples.size());
		for (std::size_t index = 0u; index < hierarchy.samples.size(); ++index)
		{
			hierarchy.nodes[index].sampleIndex = index;
			const std::uint64_t sampleId = hierarchy.samples[index].sampleId;
			if (sampleId == 0u || ambiguousIds.contains(sampleId))
			{
				continue;
			}
			const auto [found, inserted] = uniqueIndexById.emplace(sampleId, index);
			if (!inserted)
			{
				uniqueIndexById.erase(found);
				ambiguousIds.insert(sampleId);
			}
		}

		const std::size_t noParent = std::numeric_limits<std::size_t>::max();
		std::vector<std::size_t> parentIndices(hierarchy.samples.size(), noParent);
		for (std::size_t index = 0u; index < hierarchy.samples.size(); ++index)
		{
			const Profiling::ProfilerSample& child = hierarchy.samples[index];
			const auto parent = uniqueIndexById.find(child.parentSampleId);
			if (child.parentSampleId == 0u || parent == uniqueIndexById.end() || parent->second == index)
			{
				continue;
			}
			const Profiling::ProfilerSample& candidate = hierarchy.samples[parent->second];
			if (candidate.threadLabel != child.threadLabel
				|| !ProfilerSampleIntervalContains(candidate, child))
			{
				continue;
			}
			parentIndices[index] = parent->second;
		}

		for (std::size_t index = 0u; index < hierarchy.samples.size(); ++index)
		{
			const std::size_t parentIndex = parentIndices[index];
			if (parentIndex == noParent
				|| ProfilerParentChainFormsCycle(index, parentIndex, parentIndices))
			{
				hierarchy.rootIndices.push_back(index);
				continue;
			}
			hierarchy.nodes[parentIndex].childIndices.push_back(index);
		}

		// Assign depth from the rebuilt links so an orphaned or filtered sample is
		// displayed as a root instead of retaining a misleading captured indent.
		std::vector<std::pair<std::size_t, std::uint32_t>> pending;
		for (auto root = hierarchy.rootIndices.rbegin(); root != hierarchy.rootIndices.rend(); ++root)
		{
			pending.emplace_back(*root, 0u);
		}
		while (!pending.empty())
		{
			const auto [nodeIndex, depth] = pending.back();
			pending.pop_back();
			hierarchy.nodes[nodeIndex].presentationDepth = depth;
			const auto& children = hierarchy.nodes[nodeIndex].childIndices;
			for (auto child = children.rbegin(); child != children.rend(); ++child)
			{
				pending.emplace_back(*child, depth + 1u);
			}
		}

		for (ProfilerSampleHierarchyNode& node : hierarchy.nodes)
		{
			node.selfDurationMs = CalculateProfilerSampleSelfDuration(hierarchy, node);
		}
		return hierarchy;
	}
}
