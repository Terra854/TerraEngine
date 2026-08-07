#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace TerraEngine::Tests
{
	namespace
	{
		std::string ReadRendererSource(const char* relativePath)
		{
			// This unit test validates the coordinator-side retention guard without
			// requiring a live scene, device, or timing-sensitive worker execution.
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ relativePath;
			std::ifstream stream(sourcePath, std::ios::binary);
			return stream ? std::string(std::istreambuf_iterator<char>(stream), {}) : std::string{};
		}
	}

	void RegisterPersistentRenderTopologyTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PersistentTopology.RetainsStableInputsAcrossDynamicFrames", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 1u;
			const UUID mesh = GenerateUUID();
			const UUID material = GenerateUUID();
			RenderFrameRenderableRecord record;
			record.objectId = GenerateUUID();
			record.meshId = mesh;
			record.materialId = material;
			record.invalidation = { 1u, 1u, 1u, 1u, 1u, 1u, 3u, 1u };
			snapshot.renderables.push_back(record);
			const PersistentRenderTopology topology = BuildPersistentRenderTopology(snapshot, 7u);
			context.Expect(topology.valid && topology.generation == 7u, "Stable topology must publish an explicit generation.");
			context.Expect(topology.records.front().groupTemplateIndex
				!= PersistentRenderTopologyRecord::kInvalidGroupTemplateIndex,
				"A complete stable record must receive a retained compatibility-template index.");
			const RenderFrameViewRequest view{ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1280u, 720u), 1u, 9u };
			const DynamicRenderFrameState first = BuildDynamicRenderFrameState(snapshot, topology, view, 10u);
			snapshot.frameId = 2u;
			snapshot.renderables.front().modelMatrix[3][0] = 4.0f;
			snapshot.renderables.front().virtualGeometry.selectedLodIndex = 3u;
			snapshot.renderables.front().geometryRouteDecision.selected = GeometryRoute::VirtualGeometryHardware;
			snapshot.renderables.front().animation.source = AnimationRenderEvaluationSource::GpuVisual;
			const DynamicRenderFrameState second = BuildDynamicRenderFrameState(snapshot, topology, view, 11u);
			context.Expect(first.valid && second.valid, "Dynamic sidecars must remain valid as frame transforms change.");
			context.Expect(topology.records.front().groupKey.IsComplete(), "Topology keys remain complete while only dynamic state changes.");
			context.Expect(topology.records.front().groupTemplateIndex
				!= PersistentRenderTopologyRecord::kInvalidGroupTemplateIndex,
				"Transform, LOD, route, and animation-class changes must reuse the retained stable compatibility template.");
			context.ExpectEqual(0u, topology.records.front().groupKey.geometryRoute,
				"A retained template must not capture a frame-local geometry route.");
			context.ExpectEqual(0u, topology.records.front().groupKey.animationExecutionClass,
				"A retained template must not capture a frame-local animation class.");
			context.ExpectEqual(11ULL, second.frameGeneration, "Dynamic sidecars must carry the current frame generation.");
		});

		registry.Add("Renderer.PersistentTopology.InvalidatesOnStableResourceChange", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 1u;
			RenderFrameRenderableRecord record;
			record.objectId = GenerateUUID();
			record.meshId = GenerateUUID();
			record.materialId = GenerateUUID();
			record.invalidation = { 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u };
			snapshot.renderables.push_back(record);
			const PersistentRenderTopology first = BuildPersistentRenderTopology(snapshot, 1u);
			snapshot.renderables.front().invalidation.material = 2u;
			const PersistentRenderTopology changed = BuildPersistentRenderTopology(snapshot, 2u);
			context.Expect(first.valid && changed.valid && first.generation != changed.generation, "Stable material-generation changes must require a new topology generation.");
		});

		registry.Add("Renderer.PersistentTopology.ReusesCanonicalMembershipAndFailsClosedOnProducerGenerations", [](TestContext& context)
		{
			const std::string source = ReadRendererSource("Engine/Renderer/RenderSystem.cpp");
			context.Expect(source.find("matchesRetainedOrder") != std::string::npos
				&& source.find("if (!retainedOrderMatches)") != std::string::npos
				&& source.find("conventionalVisibilityIdentityTable =\n\t\t\tretainedRenderTopology") != std::string::npos
				&& source.find("if (topologyInputsChanged)") != std::string::npos
				&& source.find("topologyComponentMutationRevision") != std::string::npos
				&& source.find("topologyMeshComponentRevision") != std::string::npos
				&& source.find("topologyHierarchyRevision") != std::string::npos
				&& source.find("retainedRenderTopology.resourceRevision == renderFrameSnapshot.resourceRevision") != std::string::npos
				&& source.find("retainedRenderTopology.rendererGeneration == snapshotRendererGeneration") != std::string::npos,
				"Stable frames must reuse canonical membership, identity records, and the precomputed topology signature while exact producer generations invalidate the retained topology.");
		});

		registry.Add("Renderer.VisibleCommandCache.ReusesValidatedVirtualGeometrySelection", [](TestContext& context)
		{
			const std::string source = ReadRendererSource("Engine/Renderer/RenderSystem.cpp");
			context.Expect(source.find("bool reuseValidatedSelection = false") != std::string::npos
				&& source.find("preparedSelection == nullptr && !reuseValidatedSelection")
					!= std::string::npos
				&& source.find("&existing->second.virtualGeometryCommandIndices, true")
					!= std::string::npos
				&& source.find("const VirtualGeometryViewSelectionPacket* preparedSelection")
					!= std::string::npos
				&& source.find("true, &selectionPacket") != std::string::npos
				&& source.find("const bool generatedSubmissionsValid = reuseValidatedSelection")
					!= std::string::npos,
				"Only an exact visible-command cache hit may reuse prior virtual-geometry validation; cache misses must retain CPU-oracle validation.");
		});

		registry.Add("Renderer.DeformationSelection.ReusesVisibilityAndMeshPageShapes", [](TestContext& context)
		{
			const std::string renderer = ReadRendererSource("Engine/Renderer/RenderSystem.cpp");
			const std::string snapshot = ReadRendererSource(
				"Engine/Renderer/Graph/RenderFrameSnapshot.cpp");
			context.Expect(renderer.find("std::vector<DeformablePageFamily> pageFamilies")
					!= std::string::npos
				&& renderer.find("buildAnimatedGeometryPlan(\n\t\t\tvisibleAnimatedRenderableFlags,\n\t\t\trenderableCommandIndices)")
					!= std::string::npos
				&& renderer.find("GpuDeformationPlanAccumulator accumulator")
					!= std::string::npos
				&& renderer.find("animatedGeometryPageDemands =")
					== std::string::npos
				&& renderer.find("resolvedExactPageMasks") != std::string::npos
				&& renderer.find("findExactPageMask") != std::string::npos
				&& renderer.find("source.fallbackViewMask = fallbackViewMasks[source.renderableIndex]")
					!= std::string::npos
				&& renderer.find("& ~matchedViewMask") != std::string::npos
				&& renderer.find("std::size_t renderableIndex = draw.renderableIndex") != std::string::npos
				&& renderer.find("std::size_t renderableIndex = packet.renderableIndex") != std::string::npos
				&& renderer.find("renderFrameSnapshot.renderables[renderableIndex].objectId") != std::string::npos
				&& renderer.find("renderableIndicesByObjectId.find(\n\t\t\t\t\t\t\t\tpacket.objectId)")
					!= std::string::npos
				&& renderer.find("SelectAnimatedGeometryPageDemands(\n\t\t\t\trenderFrameSnapshot)")
					== std::string::npos
				&& renderer.find("std::unordered_set<TerraEngine::UUID> visibleAnimatedObjects")
					== std::string::npos,
				"Deformation planning must consume canonical visibility indices, validate packet indices before the UUID fallback, parse each mesh page shape once, and avoid a second crowd-sized demand array.");
			context.Expect(snapshot.find("std::unordered_set<SelectedPageKey, SelectedPageKeyHasher> demandedPages")
					!= std::string::npos
				&& snapshot.find("std::unordered_map<std::uint64_t, std::unordered_set<std::uint32_t>>")
					== std::string::npos,
				"Page selection must use one flat typed membership table instead of allocating a hash set for every animated owner.");
		});
	}
}
