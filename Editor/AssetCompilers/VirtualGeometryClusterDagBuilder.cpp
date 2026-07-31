#include "VirtualGeometryClusterDagBuilder.h"

#include "VirtualGeometryIdentity.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace TerraEngine::Editor
{
	namespace
	{
		bool NamesNeighbor(const VirtualGeometryClusterInput &input, std::uint64_t candidate) noexcept
		{
			return std::find(input.neighbors.begin(), input.neighbors.end(), candidate) != input.neighbors.end();
		}

		bool AreAdjacent(const VirtualGeometryClusterInput &first, const VirtualGeometryClusterInput &second) noexcept
		{
			return NamesNeighbor(first, second.stableId) || NamesNeighbor(second, first.stableId);
		}

		bool ConnectsToGroup(
			const VirtualGeometryClusterInput &candidate, const std::vector<std::size_t> &group, const std::vector<VirtualGeometryClusterInput> &inputs)
		{
			return std::any_of(group.begin(), group.end(), [&](std::size_t member) { return AreAdjacent(candidate, inputs[member]); });
		}
	} // namespace

	std::vector<VirtualGeometryDagNode> BuildVirtualGeometryClusterDag(
		std::vector<VirtualGeometryClusterInput> inputs, VirtualGeometryDagBuildSettings settings)
	{
		// Stable ordering makes output independent of worker completion order.
		std::sort(inputs.begin(), inputs.end(), [](const auto &first, const auto &second) { return first.stableId < second.stableId; });
		settings.maximumChildren = std::max(1u, settings.maximumChildren);

		std::vector<VirtualGeometryDagNode> nodes;
		nodes.reserve(inputs.size() * 2u);
		for (const VirtualGeometryClusterInput &input : inputs)
		{
			nodes.push_back({ input.stableId, 0u, input.stableId, {}, input.error, input.stableId != 0u && std::isfinite(input.error) && input.error >= 0.0f });
		}

		std::vector<bool> assigned(inputs.size(), false);
		for (std::size_t seed = 0u; seed < inputs.size(); ++seed)
		{
			if (assigned[seed])
			{
				continue;
			}

			std::vector<std::size_t> group{ seed };
			assigned[seed] = true;
			while (group.size() < settings.maximumChildren)
			{
				auto selected = inputs.size();
				for (std::size_t candidate = seed + 1u; candidate < inputs.size(); ++candidate)
				{
					if (!assigned[candidate] && inputs[candidate].material == inputs[seed].material && ConnectsToGroup(inputs[candidate], group, inputs))
					{
						selected = candidate;
						break;
					}
				}
				if (selected == inputs.size())
				{
					break;
				}
				assigned[selected] = true;
				group.push_back(selected);
			}

			// A disconnected singleton remains a root leaf. Creating a redundant
			// one-child parent would waste hierarchy and page space.
			if (group.size() == 1u)
			{
				continue;
			}

			std::uint64_t groupHash = 1469598103934665603ull;
			float accumulatedError = 0.0f;
			for (const std::size_t member : group)
			{
				groupHash = BuildVirtualGeometryStableId("dag-member", groupHash ^ inputs[member].stableId, static_cast<std::uint32_t>(member));
				accumulatedError = std::max(accumulatedError, inputs[member].error);
			}
			std::uint64_t parentId = BuildVirtualGeometryStableId("dag-parent", groupHash, inputs[seed].material);
			while (std::any_of(nodes.begin(), nodes.end(), [parentId](const VirtualGeometryDagNode &node) { return node.id == parentId; }))
			{
				parentId = BuildVirtualGeometryStableId("dag-parent-collision", parentId, 0u);
			}

			VirtualGeometryDagNode parent{ parentId, 0u, parentId, {}, accumulatedError, true };
			for (const std::size_t member : group)
			{
				nodes[member].parentId = parentId;
				nodes[member].fallbackId = parentId;
				parent.children.push_back(nodes[member].id);
				parent.completeCoverage = parent.completeCoverage && nodes[member].completeCoverage;
			}
			nodes.push_back(std::move(parent));
		}
		return nodes;
	}

	bool ValidateVirtualGeometryDag(const std::vector<VirtualGeometryDagNode> &nodes)
	{
		std::unordered_map<std::uint64_t, const VirtualGeometryDagNode *> byId;
		for (const VirtualGeometryDagNode &node : nodes)
		{
			if (node.id == 0u || !std::isfinite(node.error) || node.error < 0.0f || !node.completeCoverage || !byId.emplace(node.id, &node).second)
			{
				return false;
			}
		}

		for (const VirtualGeometryDagNode &node : nodes)
		{
			if (node.fallbackId == 0u || !byId.contains(node.fallbackId))
			{
				return false;
			}
			if (node.parentId != 0u)
			{
				const auto parent = byId.find(node.parentId);
				if (parent == byId.end() || node.parentId == node.id || parent->second->error < node.error)
				{
					return false;
				}
			}

			std::unordered_set<std::uint64_t> childIds;
			for (const std::uint64_t childId : node.children)
			{
				const auto child = byId.find(childId);
				if (child == byId.end() || !childIds.insert(childId).second || child->second->parentId != node.id)
				{
					return false;
				}
			}

			// Following parents must always terminate. This rejects indirect
			// cycles even when every individual parent reference exists.
			std::unordered_set<std::uint64_t> ancestors;
			const VirtualGeometryDagNode *cursor = &node;
			while (cursor->parentId != 0u)
			{
				if (!ancestors.insert(cursor->id).second)
				{
					return false;
				}
				cursor = byId.at(cursor->parentId);
			}

			// A fallback is either the node itself (a resident root) or one of
			// its ancestors, so missing detail can never jump across coverage.
			cursor = &node;
			bool fallbackFound = node.fallbackId == node.id;
			while (!fallbackFound && cursor->parentId != 0u)
			{
				cursor = byId.at(cursor->parentId);
				fallbackFound = cursor->id == node.fallbackId;
			}
			if (!fallbackFound)
			{
				return false;
			}
		}
		return true;
	}
} // namespace TerraEngine::Editor
