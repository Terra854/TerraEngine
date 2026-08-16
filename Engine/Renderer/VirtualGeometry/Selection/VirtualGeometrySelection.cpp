#include "VirtualGeometrySelection.h"

#include <algorithm>
#include <unordered_map>

namespace TerraEngine::Renderer
{
	namespace
	{
		[[nodiscard]] const VirtualGeometryOracleNode *FindNearestResidentAncestor(
			const VirtualGeometryOracleNode &node,
			const std::unordered_map<std::uint64_t, const VirtualGeometryOracleNode *> &nodesById,
			bool &outMalformedChain)
		{
			const VirtualGeometryOracleNode *candidate = &node;
			// A hierarchy is finite. The visit cap also prevents a corrupt cycle from
			// turning a worker-safe oracle into an unbounded preparation task.
			for(std::size_t visited = 0u; visited <= nodesById.size(); ++visited)
			{
				if(candidate->resident)
				{
					return candidate;
				}
				if(candidate->parent == 0u)
				{
					return candidate;
				}
				const auto parent = nodesById.find(candidate->parent);
				if(parent == nodesById.end())
				{
					outMalformedChain = true;
					return candidate;
				}
				candidate = parent->second;
			}

			outMalformedChain = true;
			return &node;
		}

		void SortAndDeduplicate(std::vector<std::uint64_t> &values)
		{
			std::sort(values.begin(), values.end());
			values.erase(std::unique(values.begin(), values.end()), values.end());
		}
	}

	VirtualGeometrySelectionResult SelectVirtualGeometryOracle(
		const std::vector<VirtualGeometryOracleNode> &nodes,
		float threshold,
		const VirtualGeometryOracleOptions &options)
	{
		VirtualGeometrySelectionResult result{};
		if(options.selectionCapacity == 0u)
		{
			result.failOpen = !nodes.empty();
			result.fallbackReason = result.failOpen
				? "Virtual-geometry selection capacity is zero; retain conservative fallback coverage."
				: "No virtual-geometry nodes require selection.";
			return result;
		}

		std::vector<const VirtualGeometryOracleNode *> canonicalNodes;
		canonicalNodes.reserve(nodes.size());
		for(const VirtualGeometryOracleNode &node : nodes)
		{
			canonicalNodes.push_back(&node);
		}
		std::sort(canonicalNodes.begin(), canonicalNodes.end(), [](const auto *left, const auto *right) {
			if(left->id != right->id) return left->id < right->id;
			if(left->parent != right->parent) return left->parent < right->parent;
			if(left->resident != right->resident) return left->resident < right->resident;
			return left->error < right->error;
		});

		std::unordered_map<std::uint64_t, const VirtualGeometryOracleNode *> nodesById;
		nodesById.reserve(canonicalNodes.size());
		for(const VirtualGeometryOracleNode *node : canonicalNodes)
		{
			// Zero is reserved for an absent object/view identity. Rejecting it here
			// prevents a stale recycled selection from becoming a valid page request.
			if(node->id == 0u)
			{
				continue;
			}
			// Duplicate identities are invalid authored input. Preserve the first
			// canonical record so the result cannot depend on container
			// iteration or worker completion order.
			nodesById.try_emplace(node->id, node);
		}

		std::vector<const VirtualGeometryOracleNode *> orderedNodes;
		orderedNodes.reserve(nodesById.size());
		for(const auto &[id, node] : nodesById)
		{
			(void)id;
			orderedNodes.push_back(node);
		}
		std::sort(orderedNodes.begin(), orderedNodes.end(), [](const auto *left, const auto *right) {
			return left->id < right->id;
		});

		bool malformedChain = false;
		bool missingResidentAncestor = false;
		for(const VirtualGeometryOracleNode *node : orderedNodes)
		{
			if(node->error < threshold)
			{
				continue;
			}

			const VirtualGeometryOracleNode *resolved = FindNearestResidentAncestor(
				*node, nodesById, malformedChain);
			missingResidentAncestor = missingResidentAncestor || !resolved->resident;
			if(!node->resident && options.requestMissingDetail)
			{
				result.requests.push_back(node->id);
			}

			const bool alreadySelected = std::find(
				result.selected.begin(), result.selected.end(), resolved->id) != result.selected.end();
			if(!alreadySelected && result.selected.size() >= options.selectionCapacity)
			{
				result.failOpen = true;
				continue;
			}
			result.selected.push_back(resolved->id);
		}

		SortAndDeduplicate(result.selected);
		SortAndDeduplicate(result.requests);
		if(malformedChain)
		{
			result.failOpen = true;
			result.fallbackReason = "Virtual-geometry hierarchy contains a missing parent or cycle; retain conservative fallback coverage.";
		}
		else if(missingResidentAncestor)
		{
			result.failOpen = true;
			result.fallbackReason = "Virtual-geometry hierarchy has no resident ancestor for a selected detail node; retain conservative fallback coverage.";
		}
		else if(result.failOpen)
		{
			result.fallbackReason = "Virtual-geometry selection exceeded its bounded packet capacity; retain conservative fallback coverage.";
		}
		return result;
	}

	VirtualGeometrySelectionResult SelectVirtualGeometryOracle(
		const std::vector<VirtualGeometryOracleNode> &nodes,
		float threshold,
		std::size_t capacity)
	{
		return SelectVirtualGeometryOracle(nodes, threshold, { capacity, true });
	}
} // namespace TerraEngine::Renderer
