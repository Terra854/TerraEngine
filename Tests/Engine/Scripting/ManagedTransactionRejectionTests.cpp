// Managed transaction failures must reject complete batches at the real boundary.
#include "TestHarness.h"
#include "Components/ScriptComponent.h"
#include "Core/Object/Component.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/Serializer.h"
#include "Scripting/ScriptGlue.h"

#include <array>
#include <mutex>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		// Always discard pending singleton work, including when a regression throws.
		struct ManagedRejectionFixture {
			ObjectManager& objects = ObjectManager::instance();
			Scene::SceneMutationQueue& queue = Scene::SceneMutationQueue::Instance();
			Object root;
			ManagedRejectionFixture() {
				queue.Clear();
				objects.clear();
				ComponentRegistry::RegisterAllComponents();
				root = objects.createObject("Managed rejection root");
				(void)objects.AddComponent<ScriptComponent>(root);
				(void)objects.PublishSceneChangePacket();
			}
			~ManagedRejectionFixture() { queue.Clear(); objects.clear(); }
			ComponentAccessFunctions ScriptFunctions() {
				// Copy callbacks while locked; executing one must not hold the registry lock.
				std::lock_guard lock(ComponentAccessRegistryMutex());
				return ComponentAccessRegistry().at("Script");
			}
		};
	}

	void RegisterManagedTransactionRejectionTests(TestRegistry& registry) {
		registry.Add("Scripting.TransactionRejection.MalformedBindingsDecodeNormally", [](TestContext& context) {
			ManagedRejectionFixture fixture;
			const auto functions = fixture.ScriptFunctions();
			context.Expect(static_cast<bool>(functions.decodePrepared), "Script must expose its production prepared decoder.");
			for(const std::string input : std::array<std::string, 2>{"[", "{}"}) {
				std::string firstReason, secondReason;
				const auto first = functions.decodePrepared({{"BindingsJson", input}}, firstReason);
				const auto second = functions.decodePrepared({{"BindingsJson", input}}, secondReason);
				context.Expect(!first && !second && !firstReason.empty() && firstReason == secondReason,
					"Malformed binding text must return a normal rejection with a stable reason.");
			}
			std::string reason;
			context.Expect(functions.decodePrepared({{"BindingsJson", "[]"}}, reason) != nullptr,
				"A well-formed empty binding array must remain accepted.");
		});
		registry.Add("Scripting.TransactionRejection.MalformedBindingsDetachedNormally", [](TestContext& context) {
			ManagedRejectionFixture fixture;
			const auto functions = fixture.ScriptFunctions();
			const auto previous = functions.capturePrepared(fixture.objects.getRegistry(), fixture.root.EntityId());
			context.Expect(previous != nullptr, "The detached setter needs an actual captured Script component.");
			for(const std::string input : std::array<std::string, 2>{"[", "{}"}) {
				std::string firstReason, secondReason;
				const auto first = functions.prepareDetachedProperty(*previous, "BindingsJson", Reflection::Variant(input), firstReason);
				const auto second = functions.prepareDetachedProperty(*previous, "BindingsJson", Reflection::Variant(input), secondReason);
				context.Expect(!first && !second && !firstReason.empty() && firstReason == secondReason,
					"Detached binding preparation must reject malformed text without throwing or changing its reason.");
			}
		});
		registry.Add("Scripting.TransactionRejection.MalformedBindingsRollBackQueuedRename", [](TestContext& context) {
			ManagedRejectionFixture fixture;
			const auto before = Serializer::SerializeObject(fixture.root.EntityId());
			const auto revision = fixture.objects.GetHierarchyRevision();
			const auto id = fixture.objects.getUUID(fixture.root);
			// The managed JSON value adapter has no string kind. Use the same typed
			// transaction preparation path rather than expanding the managed ABI here.
			fixture.queue.EnqueueTransaction(Scene::TransactionalMutationBuilder("malformed-bindings", Scene::TransactionOrigin::ManagedCallback)
				.SetName(id, "Must remain unapplied")
				.UpdateComponentProperty(id, "Script", "BindingsJson", Reflection::Variant(std::string("["))).Build());
			context.Expect(Serializer::SerializeObject(fixture.root.EntityId()) == before, "Queued writes must remain invisible before flushing.");
			const auto result = fixture.queue.Apply(fixture.objects, "managed-rejection-test");
			context.Expect(Serializer::SerializeObject(fixture.root.EntityId()) == before
				&& fixture.objects.GetHierarchyRevision() == revision && fixture.objects.PublishSceneChangePacket().Empty(),
				"Malformed bindings must preserve every authored field and pending publication.");
			context.Expect(result.transactionResults.size() == 1U
				&& result.transactionResults.front().status == Scene::TransactionStatus::Rejected
				&& !result.transactionResults.front().message.empty(),
				"The protected flush must publish a normal rejection receipt with a diagnostic.");
		});
		registry.Add("Scripting.TransactionRejection.UnresolvedManagedDeleteRejectsWholeBatch", [](TestContext& context) {
			ManagedRejectionFixture fixture;
			const Object removed = fixture.objects.createObject("Removed managed target");
			const auto staleEntity = static_cast<std::uint32_t>(removed.EntityId());
			fixture.objects.removeObject(removed);
			(void)fixture.objects.PublishSceneChangePacket();
			const auto before = Serializer::SerializeObject(fixture.root.EntityId());
			const auto revision = fixture.objects.GetHierarchyRevision();
			const nlohmann::json payload = {{"id", "managed-unresolved-delete"}, {"writes", nlohmann::json::array({
				{{"op", "setName"}, {"object", ToHexString(fixture.objects.getUUID(fixture.root))}, {"name", "Must remain unapplied"}},
				{{"op", "delete"}, {"object", staleEntity}}
			})}};
			const std::string request = payload.dump();
			const char* borrowed = Native_Scene_SubmitTransactionJson(request.c_str());
			const auto response = nlohmann::json::parse(borrowed != nullptr ? borrowed : "{}");
			context.Expect(Serializer::SerializeObject(fixture.root.EntityId()) == before, "Managed submission must not publish before its boundary.");
			(void)fixture.queue.Apply(fixture.objects, "managed-unresolved-test");
			// Flush before asserting the response so a pre-fix test exposes the actual
			// partial rename, rather than stopping at the misleading admission reply.
			context.Expect(Serializer::SerializeObject(fixture.root.EntityId()) == before
				&& fixture.objects.GetHierarchyRevision() == revision && fixture.objects.PublishSceneChangePacket().Empty(),
				"An unresolved numeric managed target must not silently drop its operation and commit the surviving rename.");
			context.Expect(!response.value("ok", true) && !response.value("error", std::string{}).empty(),
				"Unresolved managed targets must produce a diagnostic rejection.");
		});
	}
}
