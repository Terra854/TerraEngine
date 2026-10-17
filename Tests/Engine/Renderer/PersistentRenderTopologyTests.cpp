#include "TestHarness.h"
#include <string_view>
#include <algorithm>

#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "RendererThreadingTestFixtures.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace TerraEngine::Tests
{
	namespace
	{
		// Required reads and lexical definition bounds fail before any token or
		// ordering assertion can accidentally accept an empty/missing region.
		std::string RequiredSource(TestContext& context, const char* path)
		{
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / path, std::ios::binary);
			context.Expect(input.good(), std::string("Required source is unreadable: ") + path);
			std::string text((std::istreambuf_iterator<char>(input)), {});
			context.Expect(!text.empty(), std::string("Required source is empty: ") + path);
			text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
			return text;
		}
		std::string RequiredDefinition(TestContext& context, const char* path, std::string_view signature)
		{
			const auto text = RequiredSource(context, path);
			const auto start = text.find(signature);
			context.Expect(start != std::string::npos, std::string("Required definition is absent: ") + std::string(signature));
			const auto open = text.find('{', start + signature.size());
			context.Expect(open != std::string::npos, "Required definition has no opening body.");
			std::size_t depth = 0;
			bool lineComment = false, blockComment = false, quoted = false, character = false, escape = false;
			for(std::size_t index = open; index < text.size(); ++index)
			{
				const char value = text[index], next = index + 1 < text.size() ? text[index + 1] : '\0';
				if(lineComment) { if(value == '\n') lineComment = false; continue; }
				if(blockComment) { if(value == '*' && next == '/') { blockComment = false; ++index; } continue; }
				if(quoted || character) {
					if(escape) { escape = false; continue; }
					if(value == '\\') { escape = true; continue; }
					if((quoted && value == '"') || (character && value == '\'')) quoted = character = false;
					continue;
				}
				if(value == '/' && next == '/') { lineComment = true; ++index; continue; }
				if(value == '/' && next == '*') { blockComment = true; ++index; continue; }
				if(value == '"') { quoted = true; continue; }
				if(value == '\'') { character = true; continue; }
				if(value == '{') ++depth;
				if(value == '}' && --depth == 0) return text.substr(start, index + 1 - start);
			}
			context.Expect(false, "Required definition has no balanced closing body.");
			return {};
		}
		bool SourceBefore(const std::string& text, std::string_view first, std::string_view second)
		{
			const auto a = text.find(first), b = text.find(second);
			return a != std::string::npos && b != std::string::npos && a < b;
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
			RenderFrameViewRequest view{ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1280u, 720u), 1u, 9u };
			view.referenceFrame = MakeRendererTestReferenceFrame(1u, 1u, 1u);
			const DynamicRenderFrameState first = BuildDynamicRenderFrameState(snapshot, topology, view, 10u);
			snapshot.frameId = 2u;
			// The topology is retained while the canonical pose changes; the
			// per-view dynamic phase creates its bounded float matrix afterwards.
			snapshot.renderables.front().canonicalWorldPose.position.offsetMetres.x = 4.0;
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
			const auto source = RequiredDefinition(context, "Engine/Renderer/Frame/RenderTopologyPreparation.cpp", "RenderSystem::PrepareFramePersistentTopology(");
			context.Expect(source.find("matchesRetainedOrder") != std::string::npos
				&& source.find("if (!retainedOrderMatches)") != std::string::npos
				&& source.find("retainedRenderTopology.conventionalVisibilityIdentityTable") != std::string::npos
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
			const auto visible = RequiredDefinition(context, "Engine/Renderer/Frame/RenderViewPreparation.cpp", "RenderSystem::PrepareFrameVisibleCommands(");
			const auto selection = RequiredDefinition(context, "Engine/Renderer/Frame/RenderVirtualGeometrySelection.cpp", "RenderSystem::RecordFrameVirtualGeometrySelection(");
			const auto finalize = RequiredDefinition(context, "Engine/Renderer/Frame/RenderVirtualGeometrySelection.cpp", "RenderSystem::FinalizeFrameVirtualGeometryEvidence(");
			const auto declarations = RequiredSource(context, "Engine/Renderer/RenderSystem.h");
			context.Expect(declarations.find("bool reuseValidatedSelection = false") != std::string::npos
				&& finalize.find("preparedSelection == nullptr && !reuseValidatedSelection") != std::string::npos
				&& selection.find("const TerraEngine::Renderer::Frame::VirtualGeometryViewSelectionPacket* preparedSelection") != std::string::npos
				&& selection.find("FinalizeFrameVirtualGeometryEvidence(") != std::string::npos
				&& visible.find("&existing->second.virtualGeometryCommandIndices, true") != std::string::npos
				&& visible.find("true, &selectionPacket") != std::string::npos
				&& finalize.find("const bool generatedSubmissionsValid = reuseValidatedSelection") != std::string::npos,
				"Only an exact visible-command cache hit may reuse prior virtual-geometry validation; cache misses must retain CPU-oracle validation.");

		});

		registry.Add("Renderer.PersistentTopology.StaticReuseRetainsPublicationOrdinals", [](TestContext& context)
		{
			const auto base = RequiredDefinition(context, "Engine/Renderer/Frame/RenderSpatialPreparation.cpp", "RenderSystem::PrepareFrameBaseCommands(");
			const auto visible = RequiredDefinition(context, "Engine/Renderer/Frame/RenderViewPreparation.cpp", "RenderSystem::PrepareFrameVisibleCommands(");
			const auto coordinator = RequiredDefinition(context, "Engine/Renderer/RenderSystem.cpp", "RenderSystem::BuildFrameExecution(");
			const auto scene = RequiredDefinition(context, "Engine/Renderer/Frame/RenderViewBuilders.cpp", "RenderSystem::PrepareFrameSceneView(");
			const auto reuseBegin = base.find("} else {");
			const auto reuseEnd = reuseBegin == std::string::npos ? std::string::npos
				: base.find("const bool drawMembershipChanged", reuseBegin);
			context.Expect(reuseBegin != std::string::npos && reuseEnd != std::string::npos && reuseBegin < reuseEnd,
				"The base-command reuse branch must exist before inspecting its canonical ordinal writes.");
			const auto reuse = base.substr(reuseBegin, reuseEnd - reuseBegin);
			context.Expect(reuse.find("renderableDrawCommands[index].conventionalVisibilityIdentityIndex =") != std::string::npos
				&& reuse.find("renderFrameSnapshot.renderables[index]") != std::string::npos
				&& reuse.find(".conventionalVisibilityIdentityIndex;") != std::string::npos
				&& visible.find("materializeCachedCommands") != std::string::npos
				&& SourceBefore(coordinator, "PrepareFrameBaseCommands(", "PrepareFrameSceneView(")
				&& scene.find("PrepareFrameVisibleCommands(") != std::string::npos,
				"Static draw-command reuse must restore each canonical visibility ordinal before cached views republish compact selectors; otherwise direct-only crowds resolve no backend draws.");

		});

		registry.Add("Renderer.PersistentTopology.StaticSnapshotProvidesCompleteBaselineGenerations", [](TestContext& context)
		{
			const auto source = RequiredDefinition(context, "Engine/Renderer/Frame/RenderSnapshotPreparation.cpp", "RenderSystem::AssembleFrameSnapshotRows(");
			context.Expect(SourceBefore(source, "RenderFrameRenderableRecord& snapshotRenderable", "snapshotRenderable.invalidation.asset =")
				&& SourceBefore(source, "snapshotRenderable.invalidation.asset =", "snapshotRenderable.invalidation = binding.invalidation")
				&& SourceBefore(source, "snapshotRenderable.invalidation.material =", "snapshotRenderable.invalidation = binding.invalidation")
				&& SourceBefore(source, "snapshotRenderable.invalidation.renderer =", "snapshotRenderable.invalidation = binding.invalidation")
				&& SourceBefore(source, "snapshotRenderable.invalidation.residency =", "snapshotRenderable.invalidation = binding.invalidation"),
				"Every static snapshot row must initialize asset, material, renderer, and residency generations before an optional animation binding overrides them; otherwise direct-only topology slots are incomplete and publish no draws.");

		});

		registry.Add("Renderer.HotReload.InvalidatesSnapshotTopologyDynamicAndVisibleCaches", [](TestContext& context)
		{
			const auto capture = RequiredDefinition(context, "Engine/Renderer/Frame/RenderFrameCapture.cpp", "RenderSystem::CaptureFrameRenderableInputs(");
			const auto topology = RequiredDefinition(context, "Engine/Renderer/Frame/RenderTopologyPreparation.cpp", "RenderSystem::PrepareFramePersistentTopology(");
			const auto selection = RequiredDefinition(context, "Engine/Renderer/Frame/RenderVirtualGeometrySelection.cpp", "RenderSystem::RecordFrameVirtualGeometrySelection(");
			const auto visible = RequiredDefinition(context, "Engine/Renderer/Frame/RenderViewPreparation.cpp", "BuildVisibleCommandCacheKey(");
			const auto dynamic = RequiredDefinition(context, "Engine/Renderer/Frame/RenderDynamicPublication.cpp", "RenderSystem::PrepareFrameDynamicPages(");
			const auto publication = RequiredSource(context, "Engine/Renderer/Graph/RenderFramePublication.h");
			context.Expect(capture.find("resourceManager.GetRendererResourcePublicationGeneration()") != std::string::npos
				&& topology.find("retainedRenderTopology.resourceRevision == renderFrameSnapshot.resourceRevision") != std::string::npos
				&& selection.find("policyState.geometryGeneration = renderFrameSnapshot.resourceRevision") != std::string::npos
				&& visible.find("HashCombine(seed, rendererResourceGeneration)") != std::string::npos,
				"A successful renderer-resource reload must reach the immutable snapshot, retained topology, virtual-geometry history, and visible-command cache keys.");
			context.Expect(publication.find("std::uint64_t resourceGeneration = 0u;") != std::string::npos
				&& dynamic.find("lane.resourceGeneration") != std::string::npos
				&& dynamic.find("renderable.invalidation.residency") != std::string::npos
				&& dynamic.find("left.resourceGeneration == right.resourceGeneration") != std::string::npos,
				"Dynamic COW pages must reject lanes whose renderer resource generation changed even when transforms remain stable.");

		});

		registry.Add("Renderer.DeformationSelection.ReusesVisibilityAndMeshPageShapes", [](TestContext& context)
		{
			const auto renderer = RequiredDefinition(context, "Engine/Renderer/Frame/RenderAnimatedGeometryPreparation.cpp", "RenderSystem::BuildFrameAnimatedGeometryPlan(");
			const auto coordinator = RequiredDefinition(context, "Engine/Renderer/RenderSystem.cpp", "RenderSystem::BuildFrameExecution(");

			const std::string snapshot = RequiredDefinition(context, "Engine/Renderer/Graph/RenderFrameSnapshot.cpp", "SelectAnimatedGeometryPageDemands(");
			context.Expect(renderer.find("std::vector<DeformablePageFamily> pageFamilies")
					!= std::string::npos
				&& coordinator.find("BuildFrameAnimatedGeometryPlan(") != std::string::npos
				&& coordinator.find("visibleAnimatedRenderableFlags")
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
				&& renderer.find("renderableIndicesByObjectId.find(")
					!= std::string::npos
				&& renderer.find("packet.objectId)")
					!= std::string::npos
				&& renderer.find("SelectAnimatedGeometryPageDemands(")
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
