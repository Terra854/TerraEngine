// PhysicsFrameTests.cpp - frame-local conversion and isolated-world contracts.

#include "../TestHarness.h"

#include "Physics/PhysicsWorld.h"
#include "Physics/TransformPublicationPacket.h"

namespace TerraEngine::Tests {
	void RegisterPhysicsFrameTests(TestRegistry& registry) {
		registry.Add("Physics.Frame.ReconstructsCanonicalPoseAcrossDistantCells", [](TestContext& context) {
			Physics::PhysicsFrameDescriptor frame;
			frame.frame = {{7U}, 3U};
			frame.world = {{7U}, 1U};
			frame.canonicalOrigin.cell = {15'258'789LL, -15'258'789LL, 0LL};
			Scene::WorldPose canonical;
			canonical.position = frame.canonicalOrigin;
			canonical.position.offsetMetres = {10.5, -4.25, 2.0};
			const Physics::PhysicsFramePoseConversion localized = Physics::LocalizePhysicsPose(canonical, frame);
			context.Expect(localized.Succeeded(), "A bounded pose must localize against a distant exact cell.");
			const Physics::PhysicsCanonicalPoseConversion reconstructed =
				Physics::ReconstructCanonicalPhysicsPose(localized.localPose, frame);
			context.Expect(reconstructed.Succeeded() && reconstructed.canonicalPose.position == canonical.position,
				"Frame conversion must reconstruct the exact canonical cell and bounded offset.");
		});

		registry.Add("Physics.Frame.RegistryCreatesIndependentWorlds", [](TestContext& context) {
			Physics::PhysicsFrameRegistry frames;
			const auto first = frames.Create({.frameId = {11U}});
			const auto second = frames.Create({.frameId = {12U}});
			context.Expect(first.has_value() && second.has_value(), "Two unique physics frames must create independently.");
			context.Expect(first->world != second->world && frames.Descriptors().size() == 2U,
				"Each frame must retain an isolated world handle rather than reuse a singleton world.");
			context.Expect(frames.Destroy(first->frame) && !frames.Find(first->frame).has_value(),
				"Destroying a frame must retire only its matching generation.");
		});

		registry.Add("Physics.Frame.StaleSecondaryFramePacketIsRejectedBeforePublication", [](TestContext& context) {
			const Physics::PhysicsWorldHandle world{{91U}, 2U};
			const Physics::PhysicsFrameHandle activeFrame{{71U}, 4U};
			Physics::TransformPublicationSourceRecord source;
			source.objectId = TerraEngine::GenerateUUID();
			source.body = {world.worldId, world.worldGeneration, 0U, 3U};
			source.frame = activeFrame; source.objectLifetimeGeneration = 1U;
			source.bodyGeneration = 3U; source.componentGeneration = 1U;
			source.expectedTransformRevision = 1U; source.expectedHierarchyGeneration = 1U;
			source.localPose = {{2.0F, 0.0F, -4.0F}, {1.0F, 0.0F, 0.0F, 0.0F}};
			source.worldPoseReceipt = source.localPose; source.poseChanged = true;
			Physics::TransformPublicationPacket packet = Physics::PrepareTransformPublicationPacket(
				{world, activeFrame, 1U, 1U, 1U}, 1U, 1U, 1U, 1U, {&source, 1U}, 1U,
				Physics::TransformPublicationVisibilityBoundary::AfterPhysics);
			context.Expect(Physics::ValidateTransformPublicationPacket(packet).accepted,
				"A packet originating from the active secondary frame should be structurally valid.");
			packet.entries.front().frame.frameGeneration += 1U;
			const auto stale = Physics::ValidateTransformPublicationPacket(packet);
			context.Expect(!stale.accepted && stale.reason == Physics::TransformPublicationValidationReason::InvalidGeneration,
				"A packet from a recreated secondary frame must be rejected before any scene write is built.");
		});
	}
}
