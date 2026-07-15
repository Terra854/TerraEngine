// SceneChangePacketTests.cpp - exact immutable scene change publication tests.

#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/SceneChangePacket.h"
#include "Scene/SceneGraphRuntime.h"

namespace TerraEngine::Tests {
	namespace {
		// A local marker component lets the test exercise component revisions
		// without depending on a rendering or physics subsystem contract.
		struct PacketProbeComponent
		{
			// EnTT intentionally stores truly empty tag components without an
			// address. A value member lets ObjectManager's pointer-returning helper
			// exercise ordinary component membership for this test.
			int value = 0;
		};
	}

	void RegisterSceneChangePacketTests(TestRegistry& registry)
	{
		registry.Add("Engine.SceneChangePacket.PublishesExactAddedMetadataComponentTransformBoundsSets", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object root = objects.createObject("Root");
			Object child = objects.createObject("Child");
			(void)objects.PublishSceneChangePacket();

			objects.setObjectName(child, "Renamed Child");
			objects.AddComponent<PacketProbeComponent>(child);
			objects.MarkObjectTransformDirty(child);
			objects.setObjectStatic(child, true);
			context.Expect(objects.setParent(child, root), "The fixture hierarchy mutation should succeed.");
			const TerraEngine::Scene::SceneChangePacket packet = objects.PublishSceneChangePacket();

			context.Expect(packet.Validate(objects.GetSceneGeneration()).valid, "Published exact change packet should validate for its scene generation.");
			context.ExpectEqual(static_cast<std::size_t>(0), packet.Added().size(), "Previously published objects must not be repeated as additions.");
			context.ExpectEqual(static_cast<std::size_t>(1), packet.MetadataChanged().size(), "Only the renamed/static child should carry metadata change evidence.");
			context.ExpectEqual(static_cast<std::size_t>(1), packet.ComponentChanged().size(), "Only the child component membership change should be published.");
			context.ExpectEqual(static_cast<std::size_t>(1), packet.TransformChanged().size(), "Only the marked/reparented child should be transform-dirty.");
			context.ExpectEqual(static_cast<std::size_t>(1), packet.HierarchyChanged().size(), "Only the reparented child should be in hierarchy changes.");
			context.ExpectEqual(static_cast<std::size_t>(1), packet.BoundsChanged().size(), "Only the child should require a bounds update.");
			context.Expect(packet.TransformChanged().front().isStatic, "The immutable snapshot should retain static metadata captured at publication.");
			objects.clear();
		});

		registry.Add("Engine.SceneChangePacket.CollapsesRemovedObjectsAndOrdersHierarchyDeterministically", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object root = objects.createObject("Root");
			Object first = objects.createObject("First");
			Object second = objects.createObject("Second");
			(void)objects.PublishSceneChangePacket();

			context.Expect(objects.setParent(second, root), "The first child hierarchy mutation should succeed.");
			context.Expect(objects.setParent(first, second), "The nested child hierarchy mutation should succeed.");
			const TerraEngine::Scene::SceneChangePacket hierarchyPacket = objects.PublishSceneChangePacket();
			context.Expect(hierarchyPacket.Validate().valid, "Hierarchy changes should be published in deterministic parent-before-child order.");
			context.ExpectEqual(static_cast<std::size_t>(2), hierarchyPacket.HierarchyChanged().size(), "Both independently reparented objects should be present exactly once.");
			context.Expect(hierarchyPacket.HierarchyChanged()[0].hierarchyDepth <= hierarchyPacket.HierarchyChanged()[1].hierarchyDepth, "Hierarchy packet entries should be ordered by depth before stable identity.");

			objects.removeObject(root);
			const TerraEngine::Scene::SceneChangePacket removalPacket = objects.PublishSceneChangePacket();
			context.ExpectEqual(static_cast<std::size_t>(3), removalPacket.Removed().size(), "Removing a branch should publish each removed object exactly once.");
			context.ExpectEqual(static_cast<std::size_t>(0), removalPacket.TransformChanged().size(), "Removed records should not also request live transform work.");
			objects.clear();
		});

		registry.Add("Engine.SceneChangePacket.RejectsStaleSceneGenerationAndPreventsPropagation", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			objects.createObject("Old Scene Object");
			const TerraEngine::Scene::SceneChangePacket oldPacket = objects.PublishSceneChangePacket();
			const std::uint64_t oldGeneration = oldPacket.SceneGeneration();
			objects.clear();

			context.Expect(objects.GetSceneGeneration() != oldGeneration, "Clearing scene state must advance the packet generation boundary.");
			context.Expect(!oldPacket.Validate(objects.GetSceneGeneration()).valid, "A packet prepared for a previous scene must be rejected.");
			TerraEngine::Scene::SceneGraph emptyGraph;
			emptyGraph.sceneGeneration = objects.GetSceneGeneration();
			const TerraEngine::Scene::SceneDirtyTransformState dirtyState = TerraEngine::Scene::BuildDirtyTransformState(emptyGraph, oldPacket);
			context.Expect(!dirtyState.valid, "A stale packet must not expand into a transform propagation request.");
		});
	}
}
