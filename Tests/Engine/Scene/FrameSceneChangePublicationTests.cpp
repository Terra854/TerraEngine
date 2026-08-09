// FrameSceneChangePublicationTests.cpp - shared retained scene-delta ownership.

#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"

namespace TerraEngine::Tests {
	void RegisterFrameSceneChangePublicationTests(TestRegistry& registry)
	{
		registry.Add("Engine.FrameSceneChangePublication.RetainsOneProtectedPacketForEveryConsumer", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("Published Once");

			const TerraEngine::Scene::SceneChangePacket first = objects.PublishSceneChangePacket();
			const TerraEngine::Scene::SceneChangePacket rendererRead = objects.LoadPublishedSceneChangePacket();
			const TerraEngine::Scene::SceneChangePacket editorRead = objects.LoadPublishedSceneChangePacket();
			context.ExpectEqual(first.PublicationRevision(), rendererRead.PublicationRevision(),
				"Renderer preparation must retain the exact protected scene publication.");
			context.ExpectEqual(first.PublicationRevision(), editorRead.PublicationRevision(),
				"Editor preparation must retain the same packet without draining it again.");
			context.ExpectEqual(static_cast<std::size_t>(1), rendererRead.Added().size(),
				"Every consumer should observe the added object from the shared packet.");

			objects.setObjectName(object, "Changed After Publication");
			const TerraEngine::Scene::SceneChangePacket retained = objects.LoadPublishedSceneChangePacket();
			context.ExpectEqual(first.PublicationRevision(), retained.PublicationRevision(),
				"Unpublished edits must not mutate an immutable retained packet.");
			context.ExpectEqual(static_cast<std::size_t>(0), retained.MetadataChanged().size(),
				"The prior publication remains byte-stable while the next journal is pending.");

			const TerraEngine::Scene::SceneChangePacket second = objects.PublishSceneChangePacket();
			context.Expect(second.PublicationRevision() > first.PublicationRevision(),
				"The next protected boundary should advance the shared packet revision.");
			context.ExpectEqual(static_cast<std::size_t>(1), second.MetadataChanged().size(),
				"The pending edit should become visible exactly once in the next packet.");
			context.ExpectEqual(first.PublicationRevision(), rendererRead.PublicationRevision(),
				"An in-flight reader must keep its exact older immutable packet alive.");

			objects.clear();
			context.Expect(objects.LoadPublishedSceneChangePacket().Empty(),
				"Clearing a scene must invalidate the retained packet before slot reuse.");
		});
	}
}
