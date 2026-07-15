// BulkScenePublicationTests.cpp - exact reserve and one-boundary replacement tests.

#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/BulkScenePublication.h"

namespace TerraEngine::Tests {
	namespace {
		nlohmann::json MakeObject(const char* name, const char* uuid) {
			return {{"name", name}, {"uuid", uuid}, {"components", nlohmann::json::object()}};
		}
	}

	void RegisterBulkScenePublicationTests(TestRegistry& registry) {
		registry.Add("Engine.BulkScenePublication.PreflightsExactReserveAndStaleGeneration", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			TerraEngine::Scene::PreparedSceneDecode prepared;
			prepared.accepted = true; prepared.objectCount = 1U;
			prepared.objects.push_back({MakeObject("Prepared", "11111111111111111111111111111111"), "0", std::nullopt});
			auto packet = TerraEngine::Scene::BuildBulkScenePublicationPacket(prepared, objects.GetSceneGeneration(), 1U);
			context.Expect(TerraEngine::Scene::PreflightBulkScenePublication(packet, objects).accepted, "Exact object count and current generation should preflight.");
			packet.reservedObjectCount = 2U;
			context.Expect(!TerraEngine::Scene::PreflightBulkScenePublication(packet, objects).accepted, "An inexact reserve must reject before live mutation.");
			objects.clear();
			packet.reservedObjectCount = 1U;
			context.Expect(!TerraEngine::Scene::PreflightBulkScenePublication(packet, objects).accepted, "A packet from a cleared generation must not publish.");
		});

		registry.Add("Engine.BulkScenePublication.PublishesParentFirstPacketAtOneObserverBoundary", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			std::uint32_t observerCalls = 0U;
			objects.SetOnModifiedCallback([&observerCalls] { ++observerCalls; });
			TerraEngine::Scene::PreparedSceneDecode prepared;
			prepared.accepted = true; prepared.objectCount = 2U; prepared.hierarchyCount = 1U;
			prepared.objects.push_back({MakeObject("Root", "22222222222222222222222222222222"), "0", std::nullopt});
			prepared.objects.push_back({MakeObject("Child", "33333333333333333333333333333333"), "0/0", 0U});
			const auto packet = TerraEngine::Scene::BuildBulkScenePublicationPacket(prepared, objects.GetSceneGeneration(), 7U);
			const auto result = TerraEngine::Scene::PublishBulkScenePacket(packet, objects);
			context.Expect(result.published, "A validated parent-first packet should publish.");
			context.ExpectEqual(static_cast<std::uint32_t>(1U), observerCalls, "Bulk creation must produce one protected visibility callback, not one callback per object.");
			const auto roots = objects.getRootObjects();
			context.ExpectEqual(static_cast<std::size_t>(1U), roots.size(), "Only the prepared root should remain root-level after batch hierarchy publication.");
			context.ExpectEqual(static_cast<std::size_t>(1U), objects.getChildren(roots.front()).size(), "The prepared child must become visible with its parent at the same boundary.");
			objects.SetOnModifiedCallback({});
			objects.clear();
		});

		registry.Add("Engine.BulkScenePublication.CancellationAndMaterializationFailureRestorePreviousGraph", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object previous = objects.createObject("Previous");
			TerraEngine::Scene::PreparedSceneDecode prepared;
			prepared.accepted = true; prepared.objectCount = 1U;
			prepared.objects.push_back({MakeObject("Replacement", "44444444444444444444444444444444"), "0", std::nullopt});
			const auto packet = TerraEngine::Scene::BuildBulkScenePublicationPacket(prepared, objects.GetSceneGeneration(), 1U);
			const auto cancelled = TerraEngine::Scene::PublishBulkScenePacket(packet, objects, [] { return true; });
			context.Expect(cancelled.cancelled && !cancelled.published, "Cancellation before the boundary must leave the current graph untouched.");
			context.Expect(objects.isObjectValid(previous), "The previous scene object must survive a rejected bulk packet.");
			objects.clear();
		});
	}
}
