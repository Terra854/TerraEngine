// TerrainLodSeamTests.cpp -- deterministic seam, morph, and neighbor identity tests.

#include "TestHarness.h"

#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Renderer/Terrain/TerrainDistantProxy.h"
#include "Renderer/Terrain/TerrainLodSelection.h"
#include "Renderer/RenderSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/geometric.hpp>

namespace TerraEngine::Tests
{
	namespace
	{
		constexpr std::uint32_t kTerrainHaloSampleExtent = 258u;

		std::vector<std::int32_t> MakeHaloHeights(std::int32_t flatHeight = 0)
		{
			// A renderable 256-cell area owns its final east/north row and needs a
			// one-sample border on every side for continuous central-difference normals.
			return std::vector<std::int32_t>(
				static_cast<std::size_t>(kTerrainHaloSampleExtent) * kTerrainHaloSampleExtent,
				flatHeight);
		}

		Renderer::Terrain::TerrainLodView OrthographicView()
		{
			Renderer::Terrain::TerrainLodView view{};
			view.projection = Renderer::Terrain::TerrainLodProjection::Orthographic;
			view.cameraPositionMetres = {256.0, 100.0, 128.0};
			view.forward = {0.0, -1.0, 0.0};
			view.viewportHeightPixels = 1'000.0;
			view.orthographicHeightMetres = 100.0;
			return view;
		}

		Renderer::Terrain::TerrainLodSelectionNode MakeSelectionNode(
			Terrain::TerrainAreaCoordinate area,
			std::uint32_t nodeId,
			std::uint32_t parentNodeId,
			std::uint16_t lodLevel,
			glm::dvec3 boundsMin,
			glm::dvec3 boundsMax,
			double errorMetres)
		{
			Renderer::Terrain::TerrainLodSelectionNode node{};
			node.key = {area, nodeId};
			node.parentNodeId = parentNodeId;
			node.childNodeIds.fill(Renderer::Terrain::kInvalidTerrainLodNodeId);
			node.lodLevel = lodLevel;
			node.boundsMinMetres = boundsMin;
			node.boundsMaxMetres = boundsMax;
			node.geometricErrorMetres = errorMetres;
			node.resident = true;
			return node;
		}

		void AppendTwoLevelArea(
			std::vector<Renderer::Terrain::TerrainLodSelectionNode>& nodes,
			Terrain::TerrainAreaCoordinate area,
			double minimumX,
			double rootErrorMetres,
			double childErrorMetres)
		{
			using namespace Renderer::Terrain;
			TerrainLodSelectionNode root = MakeSelectionNode(
				area, 1u, kInvalidTerrainLodNodeId, 2u,
				{minimumX, 0.0, 0.0}, {minimumX + 256.0, 1.0, 256.0}, rootErrorMetres);
			root.childNodeIds = {2u, 3u, 4u, 5u};
			nodes.push_back(root);
			for(std::uint32_t child = 0u; child < 4u; ++child)
			{
				const double childX = minimumX + ((child & 1u) != 0u ? 128.0 : 0.0);
				const double childZ = (child & 2u) != 0u ? 128.0 : 0.0;
				nodes.push_back(MakeSelectionNode(
					area, child + 2u, 1u, 1u,
					{childX, 0.0, childZ}, {childX + 128.0, 1.0, childZ + 128.0}, childErrorMetres));
			}
		}

		void AppendFinestChildren(
			std::vector<Renderer::Terrain::TerrainLodSelectionNode>& nodes,
			Terrain::TerrainAreaCoordinate area,
			std::uint32_t parentNodeId,
			std::uint32_t firstChildNodeId,
			double minimumX,
			double minimumZ)
		{
			using namespace Renderer::Terrain;
			const auto parent = std::find_if(nodes.begin(), nodes.end(),
				[area, parentNodeId](const TerrainLodSelectionNode& node) {
					return node.key.area == area && node.key.nodeId == parentNodeId;
				});
			if(parent == nodes.end()) return;
			parent->childNodeIds = {
				firstChildNodeId, firstChildNodeId + 1u,
				firstChildNodeId + 2u, firstChildNodeId + 3u};
			for(std::uint32_t child = 0u; child < 4u; ++child)
			{
				const double childX = minimumX + ((child & 1u) != 0u ? 64.0 : 0.0);
				const double childZ = minimumZ + ((child & 2u) != 0u ? 64.0 : 0.0);
				nodes.push_back(MakeSelectionNode(
					area, firstChildNodeId + child, parentNodeId, 0u,
					{childX, 0.0, childZ}, {childX + 64.0, 1.0, childZ + 64.0}, 0.0));
			}
		}

		std::vector<Renderer::Terrain::TerrainLodNodeKey> KeysFromCut(
			const Renderer::Terrain::TerrainLodSelectionResult& selection)
		{
			std::vector<Renderer::Terrain::TerrainLodNodeKey> keys;
			keys.reserve(selection.selectedNodes.size());
			for(const auto& node : selection.selectedNodes) keys.push_back(node.key);
			return keys;
		}
	}

	void RegisterTerrainLodSeamTests(TestRegistry& registry)
	{
		registry.Add("Terrain.LodSeams.CooksStitchesAndInvalidatesNeighborChanges", [](TestContext& context) {
			std::vector<std::int32_t> heights = MakeHaloHeights();
			for(std::uint32_t z = 0u; z < kTerrainHaloSampleExtent; ++z)
				for(std::uint32_t x = 0u; x < kTerrainHaloSampleExtent; ++x)
					heights[static_cast<std::size_t>(z) * kTerrainHaloSampleExtent + x]
						= static_cast<std::int32_t>(x + z * 2u);
			TerraEngine::Renderer::Terrain::TerrainCookInput input{};
			input.assetId = TerraEngine::GenerateUUID(); input.authoredHash = {11u, 12u}; input.authoredGeneration = 3u; input.heightMillimetres = heights;
			input.neighborAuthoredHashes[0] = {21u, 22u};
			const auto first = TerraEngine::Renderer::Terrain::CookTerrainPage(input);
			input.neighborAuthoredHashes[0] = {23u, 24u};
			const auto changedNeighbor = TerraEngine::Renderer::Terrain::CookTerrainPage(input);
			context.Expect(first.complete && !first.edgeStitchVariants.empty() && first.lodNodes.size() == first.errorPyramid.size(),
				"A cooked terrain page needs explicit morph nodes and border stitch variants.");
			context.Expect(first.sourceIdentity != changedNeighbor.sourceIdentity && first.neighborDependencyHash != changedNeighbor.neighborDependencyHash,
				"A neighbor-only edit must invalidate derived seam data.");
			context.Expect(TerraEngine::Renderer::Terrain::AreTerrainLodLevelsCompatible(4u, 5u)
				&& !TerraEngine::Renderer::Terrain::AreTerrainLodLevelsCompatible(2u, 4u),
				"Terrain cuts may differ by one LOD only, guaranteeing a stitch or parent fallback.");
		});

		registry.Add("Terrain.LodSeams.RegionalCompleteCutProvidesMultiViewAncestorCoverage", [](TestContext& context) {
			const auto asset = TerraEngine::GenerateUUID();
			std::vector<std::int32_t> heights = MakeHaloHeights(120);
			std::vector<TerraEngine::Renderer::Terrain::TerrainPageCookResult> pages;
			for (std::int64_t z = 0; z < 2; ++z)
				for (std::int64_t x = 0; x < 2; ++x)
				{
					TerraEngine::Renderer::Terrain::TerrainCookInput input{};
					input.assetId = asset; input.area = {x, z}; input.authoredHash = {90u, 91u}; input.authoredGeneration = 6u; input.heightMillimetres = heights;
					pages.push_back(TerraEngine::Renderer::Terrain::CookTerrainPage(input));
				}
			const auto regional = TerraEngine::Renderer::Terrain::BuildTerrainDistantProxy(asset, {0, 0}, 2u, pages);
			context.Expect(regional.IsComplete() && regional.conservativeErrorMetres >= 0.0f,
				"A regional boundary requires every child page before it becomes an ancestor fallback proxy.");

			std::vector<TerraEngine::Renderer::Terrain::TerrainViewPacket> views;
			for (std::uint32_t viewId = 1u; viewId <= 2u; ++viewId)
			{
				TerraEngine::Renderer::Terrain::TerrainViewPacket view{};
				view.frameId = 10u; view.viewId = viewId; view.assetId = asset; view.area = {0, 0}; view.authoredHash = {90u, 91u};
				view.authoredGeneration = 6u; view.pageSourceIdentity = pages.front().sourceIdentity; view.requestedLod = static_cast<std::uint16_t>(viewId - 1u);
				views.push_back(view);
			}
			const auto packets = RenderSystem::PrepareCommittedTerrainPackets(views, pages, {});
			context.Expect(packets.size() == 2u && packets[0].view.viewId != packets[1].view.viewId
				&& packets[0].candidate.candidateId != packets[1].candidate.candidateId,
				"Scene and capture-like views must retain independent LOD packet identities while a complete regional cut prevents coverage holes.");
			context.Expect(!pages.front().edgeStitchVariants.empty() && pages.front().lodNodes.front().morphEndMetres > pages.front().lodNodes.front().morphStartMetres,
				"The selected complete cut must carry real stitch variants and parent geomorph intervals.");
		});

		registry.Add("Terrain.LodSeams.PerspectiveAndOrthographicScreenErrorUseCanonicalProjection", [](TestContext& context) {
			using namespace Renderer::Terrain;
			TerrainLodView perspective{};
			perspective.projection = TerrainLodProjection::Perspective;
			perspective.viewportHeightPixels = 1'080.0;
			perspective.verticalFovRadians = 1.0471975511965976; // sixty degrees
			const double perspectivePixels = ComputeTerrainProjectedErrorPixels(
				2.0, 100.0, perspective);
			const double expectedPerspective = 2.0 * 1'080.0
				/ (2.0 * std::tan(perspective.verticalFovRadians * 0.5) * 100.0);

			TerrainLodView orthographic = OrthographicView();
			const double orthographicNear = ComputeTerrainProjectedErrorPixels(
				2.0, 10.0, orthographic);
			const double orthographicFar = ComputeTerrainProjectedErrorPixels(
				2.0, 10'000.0, orthographic);
			context.Expect(std::abs(perspectivePixels - expectedPerspective) < 1.0e-9,
				"Perspective terrain SSE must include vertical FOV, viewport height, and positive view depth.");
			context.Expect(std::abs(orthographicNear - 20.0) < 1.0e-9
				&& std::abs(orthographicFar - orthographicNear) < 1.0e-9,
				"Orthographic terrain SSE must use world units per pixel and remain independent of camera depth.");
		});

		registry.Add("Terrain.LodSeams.OnePointFivePixelTargetUsesStableHysteresis", [](TestContext& context) {
			using namespace Renderer::Terrain;
			TerrainLodSelectionRequest request{};
			request.view = OrthographicView();
			request.settings.targetErrorPixels = 1.5;
			request.settings.hysteresisPixels = 0.25;
			request.nodes.push_back(MakeSelectionNode(
				{0, 0}, 1u, kInvalidTerrainLodNodeId, 1u,
				{0.0, 0.0, 0.0}, {256.0, 1.0, 256.0}, 0.16));
			request.nodes.front().childNodeIds = {2u, 3u, 4u, 5u};
			for(std::uint32_t child = 0u; child < 4u; ++child)
			{
				const double x = (child & 1u) != 0u ? 128.0 : 0.0;
				const double z = (child & 2u) != 0u ? 128.0 : 0.0;
				request.nodes.push_back(MakeSelectionNode(
					{0, 0}, child + 2u, 1u, 0u,
					{x, 0.0, z}, {x + 128.0, 1.0, z + 128.0}, 0.0));
			}

			request.previousCut = {{{0, 0}, 1u}};
			const TerrainLodSelectionResult coarseHistory = SelectTerrainLodCut(request);
			context.Expect(coarseHistory.complete && coarseHistory.selectedNodes.size() == 1u
				&& coarseHistory.selectedNodes.front().key.nodeId == 1u
				&& !coarseHistory.changedFromPrevious,
				"A previous coarse cut must not refine at 1.6 pixels while it remains inside the 1.5-pixel target's hysteresis band.");

			request.nodes.front().geometricErrorMetres = 0.14;
			request.previousCut = {
				{{0, 0}, 2u}, {{0, 0}, 3u}, {{0, 0}, 4u}, {{0, 0}, 5u}};
			const TerrainLodSelectionResult fineHistory = SelectTerrainLodCut(request);
			context.Expect(fineHistory.complete && fineHistory.selectedNodes.size() == 4u
				&& !fineHistory.changedFromPrevious,
				"A previous fine cut must not coarsen at 1.4 pixels while it remains inside the same hysteresis band.");
		});

		registry.Add("Terrain.LodSeams.CompleteCutBalancesAcrossAreaBoundaries", [](TestContext& context) {
			using namespace Renderer::Terrain;
			TerrainLodSelectionRequest request{};
			request.view = OrthographicView();
			request.settings.targetErrorPixels = 1.5;
			request.settings.maximumNeighborLevelDelta = 1u;
			AppendTwoLevelArea(request.nodes, {0, 0}, 0.0, 10.0, 0.01);
			AppendFinestChildren(request.nodes, {0, 0}, 3u, 6u, 128.0, 0.0);
			AppendFinestChildren(request.nodes, {0, 0}, 5u, 10u, 128.0, 128.0);
			// The east-side level-one nodes need refinement, creating a desired
			// level-zero edge beside the coarse neighboring area's level-two root.
			for(auto& node : request.nodes)
				if(node.key.area == Terrain::TerrainAreaCoordinate{0, 0}
					&& (node.key.nodeId == 3u || node.key.nodeId == 5u))
					node.geometricErrorMetres = 10.0;
			AppendTwoLevelArea(request.nodes, {1, 0}, 256.0, 0.01, 0.0);

			const TerrainLodSelectionResult selection = SelectTerrainLodCut(request);
			const bool refinedNeighbor = std::any_of(
				selection.selectedNodes.begin(), selection.selectedNodes.end(),
				[](const TerrainLodSelectedNode& node) {
					return node.key.area == Terrain::TerrainAreaCoordinate{1, 0}
						&& node.lodLevel == 1u;
				});
			context.Expect(selection.complete && selection.balanced,
				"Selection must submit only a complete cut whose spatial roots cover both authored areas.");
			context.Expect(refinedNeighbor,
				"Cross-area balancing must refine the resident coarse side until every shared edge differs by at most one level.");
		});

		registry.Add("Terrain.LodSeams.AllSixteenDirectionalStitchMasksAreReachable", [](TestContext& context) {
			using namespace Renderer::Terrain;
			bool allMasksMatched = true;
			for(std::uint8_t expectedMask = 0u; expectedMask < 16u; ++expectedMask)
			{
				std::array<std::uint16_t, 4u> neighborLevels{1u, 1u, 1u, 1u};
				if((expectedMask & TerrainStitchNorth) != 0u) neighborLevels[0] = 2u;
				if((expectedMask & TerrainStitchEast) != 0u) neighborLevels[1] = 2u;
				if((expectedMask & TerrainStitchSouth) != 0u) neighborLevels[2] = 2u;
				if((expectedMask & TerrainStitchWest) != 0u) neighborLevels[3] = 2u;
				allMasksMatched = allMasksMatched
					&& BuildTerrainStitchMask(1u, neighborLevels) == expectedMask;
			}
			context.Expect(allMasksMatched,
				"North/east/south/west coarse-neighbor bits must address every one of the sixteen cooked stitch variants.");
		});

		registry.Add("Terrain.LodSeams.MorphsHeightAndNormalAndPinsHoleBoundaries", [](TestContext& context) {
			using namespace Renderer::Terrain;
			const TerrainMorphEndpoints endpoints{
				10.0, 14.0, {0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}};
			const TerrainMorphedVertex midpoint = EvaluateTerrainMorph(endpoints, 0.5);
			context.Expect(std::abs(midpoint.heightMetres - 12.0) < 1.0e-9
				&& std::abs(glm::length(midpoint.normal) - 1.0) < 1.0e-9
				&& midpoint.normal.x > 0.0 && midpoint.normal.y > 0.0,
				"Geomorphing must interpolate parent-projected height and a finite normalized geometric normal together.");

			TerrainLodSelectionRequest request{};
			request.view = OrthographicView();
			request.nodes.push_back(MakeSelectionNode(
				{0, 0}, 1u, kInvalidTerrainLodNodeId, 1u,
				{0.0, 0.0, 0.0}, {256.0, 1.0, 256.0}, 0.0));
			request.nodes.front().childNodeIds = {2u, 3u, 4u, 5u};
			request.nodes.front().crossesHoleBoundary = true;
			for(std::uint32_t child = 0u; child < 4u; ++child)
			{
				const double x = (child & 1u) != 0u ? 128.0 : 0.0;
				const double z = (child & 2u) != 0u ? 128.0 : 0.0;
				auto leaf = MakeSelectionNode(
					{0, 0}, child + 2u, 1u, 0u,
					{x, 0.0, z}, {x + 128.0, 1.0, z + 128.0}, 0.0);
				leaf.crossesHoleBoundary = child == 0u;
				request.nodes.push_back(leaf);
			}
			const TerrainLodSelectionResult selection = SelectTerrainLodCut(request);
			const bool pinnedHoleLeaf = std::any_of(selection.selectedNodes.begin(), selection.selectedNodes.end(),
				[](const TerrainLodSelectedNode& node) {
					return node.key.nodeId == 2u && node.lodLevel == 0u && node.holePinned;
				});
			context.Expect(selection.complete && pinnedHoleLeaf,
				"A node crossing an authored hole boundary must remain at finest detail and diagnose that pin in the selected cut.");
		});

		registry.Add("Terrain.LodSeams.StationaryCameraProducesIdenticalCut", [](TestContext& context) {
			using namespace Renderer::Terrain;
			TerrainLodSelectionRequest request{};
			request.view = OrthographicView();
			AppendTwoLevelArea(request.nodes, {0, 0}, 0.0, 1.0, 0.0);
			const TerrainLodSelectionResult first = SelectTerrainLodCut(request);
			request.previousCut = KeysFromCut(first);
			const TerrainLodSelectionResult second = SelectTerrainLodCut(request);
			const auto firstKeys = KeysFromCut(first);
			const auto secondKeys = KeysFromCut(second);
			context.Expect(first.complete && second.complete && firstKeys == secondKeys
				&& !second.changedFromPrevious,
				"Repeated selection for a stationary camera and unchanged residency snapshot must not oscillate or reorder the cut.");
		});
	}
}
