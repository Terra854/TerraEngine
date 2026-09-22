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
		registry.Add("Engine.SceneChangePacket.LiveCacheRejectsEditsAfterPublication", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("Live mesh cache probe");
			const auto initial = objects.PublishSceneChangePacket();
			const auto beforeAdd = objects.PublishSceneChangePacket();
			context.Expect(objects.CanApplySceneChangePacketToLiveState(beforeAdd, initial.PublicationRevision()),
				"A consecutive publication with no later edits can patch a live cache.");

			// This is the editor failure ordering: publish an empty frame packet,
			// then add/remove a component before the renderer captures live revisions.
			objects.AddComponent<PacketProbeComponent>(object);
			context.Expect(!objects.CanApplySceneChangePacketToLiveState(beforeAdd, initial.PublicationRevision()),
				"An empty packet must not hide a component added after publication.");
			const auto added = objects.PublishSceneChangePacket();
			context.Expect(objects.CanApplySceneChangePacketToLiveState(added, beforeAdd.PublicationRevision()),
				"Publishing the pending addition restores safe incremental capture.");
			objects.RemoveComponent<PacketProbeComponent>(object);
			context.Expect(!objects.CanApplySceneChangePacketToLiveState(added, beforeAdd.PublicationRevision()),
				"Even a nonempty packet must not hide a later component removal.");
			const auto removed = objects.PublishSceneChangePacket();
			context.Expect(objects.CanApplySceneChangePacketToLiveState(removed, added.PublicationRevision()),
				"The removal is safe to apply once it reaches publication.");
			context.Expect(!objects.CanApplySceneChangePacketToLiveState(added, beforeAdd.PublicationRevision()),
				"An older retained packet cannot describe current live state.");
			context.Expect(!objects.CanApplySceneChangePacketToLiveState(removed, initial.PublicationRevision()),
				"A missed publication requires a full rebuild.");
			objects.AddComponent<PacketProbeComponent>(object);
			const auto restored = objects.PublishSceneChangePacket();
			objects.notifyObjectComponentValueChanged(object, "PacketProbeComponent");
			context.Expect(!objects.CanApplySceneChangePacketToLiveState(restored, removed.PublicationRevision()),
				"Late mesh/material value notifications also require fresh live capture.");
			const auto valueChanged = objects.PublishSceneChangePacket();
			context.Expect(objects.CanApplySceneChangePacketToLiveState(valueChanged, restored.PublicationRevision()),
				"Published value edits preserve the normal incremental path.");
			objects.clear();
			context.Expect(!objects.CanApplySceneChangePacketToLiveState(removed, added.PublicationRevision()),
				"A previous scene's packet cannot update the replacement scene.");
		});

		registry.Add("Scene.ChangePacket.InvalidationKindsCoverPublicationDependencies", [](TestContext& context) {
			using TerraEngine::Scene::SceneChangeKind;
			context.Expect(static_cast<unsigned>(SceneChangeKind::Bounds) == 6U, "Change packets expose stable bounds/hierarchy/transform/component invalidation kinds.");
		});
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
