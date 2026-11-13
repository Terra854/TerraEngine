#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"
#include "Renderer/Frame/RenderFrameCaches.h"
#include "Renderer/Frame/RenderFrameCapture.h"
#include <cmath>
#include <map>
#include <set>

namespace TerraEngine::Tests::Review
{
	// This is the one approved friend: invoke the real protected preparation
	// operation on copied inputs and inspect retained values, never replace it.
	struct RenderPublicationIdentityAccess
	{
		static std::pair<bool, bool> PrepareTwice(Renderer::RenderFrameSnapshot& snapshot,
			Renderer::Frame::RetainedRenderTopologyState& retained)
		{
			auto& renderer = RenderSystem::Instance();
			RenderSystem::FrameSceneRevisions revisions{};
			revisions.capturedSceneGeneration = revisions.capturedComponentMutationRevision = 1;
			revisions.capturedRenderBindingRevision = revisions.capturedHierarchyRevision = 1;
			std::vector<Renderer::RenderPassNode> passes;
			const auto capabilities = MakeThreadedRendererProfile();
			const bool first = renderer.PrepareFramePersistentTopology(snapshot, retained, revisions, 1, capabilities, passes);
			const bool second = renderer.PrepareFramePersistentTopology(snapshot, retained, revisions, 1, capabilities, passes);
			return {first, second};
		}
	};
}
namespace TerraEngine::Tests
{
	void RegisterViewPublicationIdentityTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Review.ViewPublication.CacheHitAndMissUseSameNonzeroOrigin", [](TestContext& context) {
			using namespace Renderer;
			Frame::VisibleCommandCacheRecord cache;
			cache.stableOrdinals = {0}; cache.selectedLods = {0}; cache.routeSelectors = {0}; cache.editorTints = {glm::vec4(1)};
			cache.commandLanes = std::make_shared<const std::vector<RenderViewCommandLane>>(1);
			cache.hasViewDrawEvidence = true;
			cache.viewDraws = std::make_shared<const std::vector<VirtualGeometryViewDrawRecord>>();
			auto snapshot = MakeRendererThreadingSnapshot(1, 1);
			std::vector<RenderItemRecord> base(1);
			base[0].canonicalWorldPose.position.offsetMetres = {80.0, 20.0, 4.0};
			base[0].modelMatrix = glm::mat4(1.0f); // Stale canonical/base matrix must not leak into a view.
			for (double origin : {16.0, 64.0})
			{
				auto frame = MakeRendererTestReferenceFrame(1, 1, 1);
				frame.canonicalOrigin.offsetMetres = {origin, 0.0, 0.0};
				const auto localized = BuildLocalizedRenderPoseMatrices(base[0].canonicalWorldPose, frame);
				context.Expect(localized.valid, "The independent nonzero-origin fixture must localize.");
				Frame::VisibleCommandBuildResult miss;
				miss.commands = base; miss.commands[0].modelMatrix = localized.currentMatrix;
				Frame::VisibleCommandBuildResult hit; hit.cachedRecord = &cache; hit.cacheHit = true;
				std::vector<RenderItemRecord> missOutput, hitOutput;
				miss.PopulateSelectedItems(missOutput, snapshot.renderables, base, frame, nullptr, nullptr, nullptr);
				hit.PopulateSelectedItems(hitOutput, snapshot.renderables, base, frame, nullptr, nullptr, nullptr);
				context.Expect(missOutput.size() == 1 && hitOutput.size() == 1, "Both production materialization lanes must execute.");
				context.Expect(std::abs(missOutput[0].modelMatrix[3].x - float(80.0 - origin)) < 1e-5f,
					"The miss control must obey the independently known translation.");
				for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row)
					context.Expect(std::abs(missOutput[0].modelMatrix[column][row] - hitOutput[0].modelMatrix[column][row]) < 1e-5f,
						"Complete compact-cache hits must match actual localized miss matrices.");
			}
		});
		registry.Add("Renderer.Review.ViewPublication.InvalidMiddleKeyKeepsSnapshotSidecarAlignment", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(1, 3);
			for (std::size_t index = 0; index < 3; ++index)
			{
				auto& item = snapshot.renderables[index]; item.objectId = {}; item.objectId[15] = static_cast<std::uint8_t>(index + 1);
				item.invalidation.asset = item.invalidation.material = item.invalidation.renderer = item.invalidation.residency = 1;
			}
			snapshot.renderables[1].invalidation.asset = 0;
			Renderer::Frame::RetainedRenderTopologyState retained;
			const auto [firstRebuilt, secondRebuilt] = Review::RenderPublicationIdentityAccess::PrepareTwice(snapshot, retained);
			context.Expect(firstRebuilt && !secondRebuilt, "An unchanged snapshot with an invalid group must still reuse its retained topology.");
			context.Expect(retained.orderedObjectIds.size() == 3 && retained.orderedSlotKeys.size() == 3,
				"Snapshot-indexed sidecars need an invalid placeholder for the middle renderable.");
			context.Expect(retained.orderedObjectIds[2] == snapshot.renderables[2].objectId,
				"The third object's sidecar must retain its snapshot index.");
			context.Expect(!retained.orderedSlotKeys[1].IsValid()
				&& snapshot.renderables[1].conventionalVisibilityIdentityIndex == 0,
				"An invalid middle owner retains a placeholder and restores the clear identity on reuse.");
			const auto third = retained.orderedSlotKeys[2];
			context.Expect(third.ordinal < retained.renderableIndexByOrdinal.size()
				&& retained.renderableIndexByOrdinal[third.ordinal] == 2,
				"Valid slot reverse lookup must still identify the third snapshot element.");
		});
		registry.Add("Renderer.Review.ViewPublication.ResourceNamespacesRemainDisjoint", [](TestContext& context) {
			for (const auto ids : {std::pair{1u, 2u}, std::pair{1u, 101u}, std::pair{1u, 201u}, std::pair{UINT32_MAX - 1, UINT32_MAX}})
			{
				auto snapshot = MakeRendererThreadingSnapshot(1, 0);
				auto second = snapshot.viewRequests.front(); snapshot.viewRequests.front().outputId = ids.first; second.outputId = ids.second;
				snapshot.viewRequests.push_back(second);
				const auto passes = Renderer::BuildDefaultRenderPasses(snapshot, MakeThreadedRendererProfile());
				context.Expect(Renderer::ValidateRenderGraph(passes).valid, "Distinct output IDs must not create unordered cross-view resource conflicts.");
				std::map<std::uint64_t, std::string> owners;
				for (const auto& pass : passes) for (const auto& output : pass.outputs)
				{
					context.Expect(output.resourceId != 0, "Generated view resources cannot lose identity through uint32 wraparound.");
					const auto prefix = pass.passId.substr(0, pass.passId.find('.'));
					const auto [found, inserted] = owners.emplace(output.resourceId, prefix);
					context.Expect(inserted || found->second == prefix, "Every resource must remain in one output namespace.");
				}
			}
		});
	}
}
