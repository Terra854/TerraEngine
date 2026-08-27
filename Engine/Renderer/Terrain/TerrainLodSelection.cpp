// TerrainLodSelection.cpp -- deterministic SSE, hysteresis, balance, and morph helpers.

#include "Renderer/Terrain/TerrainLodSelection.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace TerraEngine::Renderer::Terrain
{
	namespace
	{
		struct NodeKeyHasher
		{
			std::size_t operator()(const TerrainLodNodeKey& key) const noexcept
			{
				std::size_t hash = TerraEngine::Terrain::TerrainAreaCoordinateHasher{}(key.area);
				hash ^= std::hash<std::uint32_t>{}(key.nodeId) + 0x9e3779b9u
					+ (hash << 6u) + (hash >> 2u);
				return hash;
			}
		};

		double NodeDepth(const TerrainLodSelectionNode& node, const TerrainLodView& view) noexcept
		{
			const glm::dvec3 centre = (node.boundsMinMetres + node.boundsMaxMetres) * 0.5;
			const glm::dvec3 direction = glm::length(view.forward) > 0.0
				? glm::normalize(view.forward) : glm::dvec3(0.0, 0.0, -1.0);
			return std::max(1.0e-6, glm::dot(centre - view.cameraPositionMetres, direction));
		}

		bool Adjacent(const TerrainLodSelectionNode& left, const TerrainLodSelectionNode& right) noexcept
		{
			const double epsilon = 1.0e-9;
			const bool overlapX = left.boundsMinMetres.x < right.boundsMaxMetres.x - epsilon
				&& right.boundsMinMetres.x < left.boundsMaxMetres.x - epsilon;
			const bool overlapZ = left.boundsMinMetres.z < right.boundsMaxMetres.z - epsilon
				&& right.boundsMinMetres.z < left.boundsMaxMetres.z - epsilon;
			const bool touchesX = std::abs(left.boundsMaxMetres.x - right.boundsMinMetres.x) <= epsilon
				|| std::abs(right.boundsMaxMetres.x - left.boundsMinMetres.x) <= epsilon;
			const bool touchesZ = std::abs(left.boundsMaxMetres.z - right.boundsMinMetres.z) <= epsilon
				|| std::abs(right.boundsMaxMetres.z - left.boundsMinMetres.z) <= epsilon;
			return (touchesX && overlapZ) || (touchesZ && overlapX);
		}
	}

	double ComputeTerrainProjectedErrorPixels(
		double geometricErrorMetres, double viewDepthMetres,
		const TerrainLodView& view) noexcept
	{
		if(!std::isfinite(geometricErrorMetres) || geometricErrorMetres <= 0.0
			|| !std::isfinite(view.viewportHeightPixels) || view.viewportHeightPixels <= 0.0)
			return 0.0;
		if(view.projection == TerrainLodProjection::Orthographic)
		{
			return std::isfinite(view.orthographicHeightMetres) && view.orthographicHeightMetres > 0.0
				? geometricErrorMetres * view.viewportHeightPixels / view.orthographicHeightMetres : 0.0;
		}
		if(!std::isfinite(viewDepthMetres) || viewDepthMetres <= 0.0
			|| !std::isfinite(view.verticalFovRadians) || view.verticalFovRadians <= 0.0
			|| view.verticalFovRadians >= 3.14159265358979323846) return 0.0;
		return geometricErrorMetres * view.viewportHeightPixels
			/ (2.0 * std::tan(view.verticalFovRadians * 0.5) * viewDepthMetres);
	}

	TerrainLodSelectionResult SelectTerrainLodCut(const TerrainLodSelectionRequest& request)
	{
		TerrainLodSelectionResult result{};
		if(request.nodes.empty() || !std::isfinite(request.settings.targetErrorPixels)
			|| request.settings.targetErrorPixels <= 0.0) return result;

		std::unordered_map<TerrainLodNodeKey, const TerrainLodSelectionNode*, NodeKeyHasher> nodes;
		std::unordered_set<TerrainLodNodeKey, NodeKeyHasher> previous(
			request.previousCut.begin(), request.previousCut.end());
		for(const TerrainLodSelectionNode& node : request.nodes) nodes.emplace(node.key, &node);

		const auto findChild = [&](const TerrainLodSelectionNode& parent, std::uint32_t childId) {
			const auto found = nodes.find({parent.key.area, childId});
			return found == nodes.end() ? static_cast<const TerrainLodSelectionNode*>(nullptr) : found->second;
		};
		std::function<bool(const TerrainLodSelectionNode&)> select = [&](const TerrainLodSelectionNode& node) {
			const double projected = ComputeTerrainProjectedErrorPixels(
				node.geometricErrorMetres, NodeDepth(node, request.view), request.view);
			const bool wasSelected = previous.contains(node.key);
			bool hadSelectedChild = false;
			for(const std::uint32_t childId : node.childNodeIds)
				hadSelectedChild = hadSelectedChild
					|| (childId != kInvalidTerrainLodNodeId
						&& previous.contains({node.key.area, childId}));
			// Hysteresis is two-sided: a previously coarse node refines only above
			// the upper edge, while a previously fine subtree coarsens only below
			// the lower edge. This prevents a stationary cut oscillating at 1.5 px.
			const double upperThreshold = request.settings.targetErrorPixels
				+ request.settings.hysteresisPixels;
			const double lowerThreshold = std::max(0.0,
				request.settings.targetErrorPixels - request.settings.hysteresisPixels);
			bool wantsChildren = node.lodLevel > 0u
				&& (node.crossesHoleBoundary
					|| (wasSelected ? projected > upperThreshold
						: hadSelectedChild ? projected >= lowerThreshold
						: projected > request.settings.targetErrorPixels));
			std::array<const TerrainLodSelectionNode*, 4u> children{};
			if(wantsChildren)
			{
				for(std::size_t index = 0u; index < children.size(); ++index)
				{
					if(node.childNodeIds[index] == kInvalidTerrainLodNodeId) { wantsChildren = false; break; }
					children[index] = findChild(node, node.childNodeIds[index]);
					if(children[index] == nullptr) { wantsChildren = false; break; }
				}
			}
			if(wantsChildren)
			{
				const std::size_t rollback = result.selectedNodes.size();
				bool completeChildren = true;
				for(const TerrainLodSelectionNode* child : children)
					completeChildren = select(*child) && completeChildren;
				if(completeChildren) return true;
				result.selectedNodes.resize(rollback);
			}
			if(!node.resident) return false;
			const double lower = std::max(0.0, request.settings.targetErrorPixels
				- request.settings.hysteresisPixels);
			const double upper = request.settings.targetErrorPixels
				+ request.settings.hysteresisPixels;
			const double morph = upper > lower ? std::clamp((projected - lower) / (upper - lower), 0.0, 1.0) : 1.0;
			result.selectedNodes.push_back({node.key, node.lodLevel, projected, morph, 0u,
				node.crossesHoleBoundary && node.lodLevel == 0u});
			return true;
		};

		result.complete = true;
		for(const TerrainLodSelectionNode& node : request.nodes)
		{
			if(node.parentNodeId != kInvalidTerrainLodNodeId) continue;
			result.complete = select(node) && result.complete;
		}
		if(!result.complete) result.selectedNodes.clear();

		// Repair imbalanced edges by refining the coarser side one level at a
		// time. Refinement is atomic: all four resident children replace the
		// parent together, otherwise the complete prior cut remains diagnosable.
		result.balanced = result.complete;
		for(std::size_t repair = 0u; result.complete
			&& repair < request.nodes.size() * 4u; ++repair)
		{
			std::optional<std::size_t> coarseIndex;
			for(std::size_t leftIndex = 0u; leftIndex < result.selectedNodes.size() && !coarseIndex; ++leftIndex)
			{
				for(std::size_t rightIndex = leftIndex + 1u; rightIndex < result.selectedNodes.size(); ++rightIndex)
				{
					const auto left = nodes.find(result.selectedNodes[leftIndex].key);
					const auto right = nodes.find(result.selectedNodes[rightIndex].key);
					if(left == nodes.end() || right == nodes.end() || !Adjacent(*left->second, *right->second)) continue;
					const std::uint16_t first = result.selectedNodes[leftIndex].lodLevel;
					const std::uint16_t second = result.selectedNodes[rightIndex].lodLevel;
					const std::uint16_t delta = first > second ? first - second : second - first;
					if(delta > request.settings.maximumNeighborLevelDelta)
					{
						coarseIndex = first > second ? leftIndex : rightIndex;
						break;
					}
				}
			}
			if(!coarseIndex) break;
			const auto coarse = nodes.find(result.selectedNodes[*coarseIndex].key);
			if(coarse == nodes.end()) { result.balanced = false; break; }
			std::array<const TerrainLodSelectionNode*, 4u> children{};
			bool canRefine = true;
			for(std::size_t childIndex = 0u; childIndex < children.size(); ++childIndex)
			{
				const std::uint32_t childId = coarse->second->childNodeIds[childIndex];
				children[childIndex] = childId == kInvalidTerrainLodNodeId
					? nullptr : findChild(*coarse->second, childId);
				canRefine = canRefine && children[childIndex] != nullptr && children[childIndex]->resident;
			}
			if(!canRefine) { result.balanced = false; break; }
			result.selectedNodes.erase(result.selectedNodes.begin() + static_cast<std::ptrdiff_t>(*coarseIndex));
			for(const TerrainLodSelectionNode* child : children)
			{
				const double projected = ComputeTerrainProjectedErrorPixels(
					child->geometricErrorMetres, NodeDepth(*child, request.view), request.view);
				const double upper = request.settings.targetErrorPixels + request.settings.hysteresisPixels;
				const double lower = std::max(0.0,
					request.settings.targetErrorPixels - request.settings.hysteresisPixels);
				const double morph = upper > lower
					? std::clamp((projected - lower) / (upper - lower), 0.0, 1.0) : 1.0;
				result.selectedNodes.push_back({child->key, child->lodLevel, projected, morph, 0u,
					child->crossesHoleBoundary && child->lodLevel == 0u});
			}
		}

		result.balanced = result.complete && result.balanced;
		for(std::size_t leftIndex = 0u; leftIndex < result.selectedNodes.size(); ++leftIndex)
			for(std::size_t rightIndex = leftIndex + 1u; rightIndex < result.selectedNodes.size(); ++rightIndex)
			{
				const auto left = nodes.find(result.selectedNodes[leftIndex].key);
				const auto right = nodes.find(result.selectedNodes[rightIndex].key);
				if(left == nodes.end() || right == nodes.end() || !Adjacent(*left->second, *right->second)) continue;
				const auto first = result.selectedNodes[leftIndex].lodLevel;
				const auto second = result.selectedNodes[rightIndex].lodLevel;
				if(first > second ? first - second > request.settings.maximumNeighborLevelDelta
					: second - first > request.settings.maximumNeighborLevelDelta) result.balanced = false;
			}

		std::vector<TerrainLodNodeKey> current;
		current.reserve(result.selectedNodes.size());
		for(const auto& selected : result.selectedNodes) current.push_back(selected.key);
		auto order = [](const TerrainLodNodeKey& left, const TerrainLodNodeKey& right) {
			if(left.area.x != right.area.x) return left.area.x < right.area.x;
			if(left.area.z != right.area.z) return left.area.z < right.area.z;
			return left.nodeId < right.nodeId;
		};
		std::vector<TerrainLodNodeKey> prior = request.previousCut;
		std::sort(current.begin(), current.end(), order);
		std::sort(prior.begin(), prior.end(), order);
		result.changedFromPrevious = current != prior;
		return result;
	}

	std::uint8_t BuildTerrainStitchMask(
		std::uint16_t selectedLevel,
		const std::array<std::uint16_t, 4u>& neighborLevels) noexcept
	{
		std::uint8_t mask = 0u;
		for(std::size_t edge = 0u; edge < neighborLevels.size(); ++edge)
			if(neighborLevels[edge] == static_cast<std::uint16_t>(selectedLevel + 1u))
				mask |= static_cast<std::uint8_t>(1u << edge);
		return mask;
	}

	TerrainMorphedVertex EvaluateTerrainMorph(
		const TerrainMorphEndpoints& endpoints, double factor) noexcept
	{
		factor = std::clamp(std::isfinite(factor) ? factor : 0.0, 0.0, 1.0);
		TerrainMorphedVertex result{};
		result.heightMetres = endpoints.parentHeightMetres
			+ (endpoints.childHeightMetres - endpoints.parentHeightMetres) * factor;
		const glm::dvec3 blended = endpoints.parentNormal
			+ (endpoints.childNormal - endpoints.parentNormal) * factor;
		result.normal = glm::length(blended) > 1.0e-12
			? glm::normalize(blended) : glm::dvec3(0.0, 1.0, 0.0);
		return result;
	}

	TerrainPageResolution ResolveTerrainOwnAncestorPage(
		const TerrainPageSelectionRequest& request,
		std::span<const TerrainPageResidencyEntry> entries) noexcept
	{
		TerrainPageResolution result{};
		result.desiredPageId = request.desiredPageId;
		std::uint32_t candidatePageId = request.desiredPageId;
		for(std::uint16_t depth = 0u; candidatePageId != kInvalidTerrainPageId; ++depth)
		{
			const auto found = std::find_if(entries.begin(), entries.end(), [&](const TerrainPageResidencyEntry& entry) {
				return entry.assetId == request.assetId && entry.area == request.area
					&& entry.pageId == candidatePageId;
			});
			if(found == entries.end()) break;
			const bool resident = found->state == TerraEngine::Resource::VirtualGeometryPagePublicationState::Resident;
			const bool retainedInFlight = found->state == TerraEngine::Resource::VirtualGeometryPagePublicationState::RetirePending
				&& request.frameId < found->retireAfterFrame;
			const bool visible = (resident || retainedInFlight)
				&& found->uploadCompleted && found->published;
			if(visible)
			{
				result.actualPageId = found->pageId;
				result.fallbackDepth = depth;
				result.desiredResident = depth == 0u;
				result.pinnedCoarseCoverage = found->pinnedCoarseCoverage;
				result.completeCoverage = true;
				return result;
			}
			candidatePageId = found->parentPageId;
		}
		return result;
	}

	TerrainPageDemandResult BuildTerrainPageDemands(
		std::span<const TerrainPageSelectionRequest> requests,
		std::span<const TerrainPageResidencyEntry> entries)
	{
		TerrainPageDemandResult result{};
		result.completeCoverage = true;
		result.resolutions.reserve(requests.size());
		for(const TerrainPageSelectionRequest& request : requests)
		{
			const TerrainPageResolution resolution = ResolveTerrainOwnAncestorPage(request, entries);
			result.resolutions.push_back(resolution);
			result.completeCoverage = result.completeCoverage && resolution.completeCoverage;
			if(resolution.desiredResident) continue;
			auto demand = std::find_if(result.demands.begin(), result.demands.end(), [&](const TerrainPageDemand& existing) {
				return existing.assetId == request.assetId && existing.area == request.area
					&& existing.pageId == request.desiredPageId;
			});
			if(demand == result.demands.end())
			{
				result.demands.push_back({request.assetId, request.area,
					request.desiredPageId, request.priority, 1u});
			}
			else
			{
				demand->priority = std::max(demand->priority, request.priority);
				++demand->requestingViewCount;
			}
		}
		std::sort(result.demands.begin(), result.demands.end(), [](const TerrainPageDemand& left, const TerrainPageDemand& right) {
			if(left.priority != right.priority) return left.priority > right.priority;
			if(left.assetId != right.assetId) return left.assetId < right.assetId;
			if(left.area.x != right.area.x) return left.area.x < right.area.x;
			if(left.area.z != right.area.z) return left.area.z < right.area.z;
			return left.pageId < right.pageId;
		});
		return result;
	}
}
