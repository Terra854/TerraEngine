// Camera profile identity uses the ordinary detached component transaction and
// durable scene serializer. These CPU tests start no renderer, editor or GPU.
#include "TestHarness.h"
#include "Components/CameraComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/WorkerSafety.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/Serializer.h"
#include <future>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Scene;
		UUID ProfileId(std::uint8_t marker)
		{
			UUID id{}; id[0] = marker; id[15] = 0x91; return id;
		}
		class CameraAuthoringScene final
		{
		public:
			ObjectManager& objects = ObjectManager::instance();
			Object object;
			UUID objectId = EMPTY_UUID;
			CameraAuthoringScene()
			{
				ComponentRegistry::RegisterAllComponents();
				objects.clear();
				object = objects.createObject("AuthoredProfileCamera");
				objectId = objects.getUUID(object);
				objects.AddComponent<CameraComponent>(object);
			}
			~CameraAuthoringScene() { objects.clear(); }
			CameraComponent& Camera() { return *objects.GetComponent<CameraComponent>(object); }
			Transaction Patch(const std::string& id, Reflection::Variant value)
			{
				return TransactionalMutationBuilder(id, TransactionOrigin::Test, "camera-profile-test")
					.ObserveScene(objects).ObserveObject(objects, objectId)
					.UpdateComponentProperty(objectId, std::string(CameraComponent::kComponentName),
						"PostProcessProfileId", std::move(value)).Build();
			}
		};
		const TransactionResult& OnlyReceipt(TestContext& context, const SceneMutationBatchResult& result)
		{
			context.ExpectEqual(std::size_t{1}, result.transactionResults.size(), "The exact queued transaction must receive one terminal receipt.");
			return result.transactionResults.front();
		}
	}

	void RegisterPostProcessCameraAuthoringTransactionTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PostProcess.CameraAuthoring.SceneDefaultUuidHasIndependentSerialization", [](TestContext& context) {
			const UUID profile = ProfileId(8);
			nlohmann::json declared{{"postProcessProfileId", ToHexString(profile)}, {"indirectStrength", 2.0f}};
			context.Expect(Serializer::DeserializeScenePostProcessProfileId(declared) == profile,
				"Scene defaults must select the exact authored asset UUID.");
			const auto scalar = Serializer::DeserializeSceneRenderSettings(declared);
			context.Expect(scalar.indirectStrength == 2.0f && !Serializer::SerializeSceneRenderSettings(scalar).contains("postProcessProfileId"),
				"Legacy managed scalar settings must keep their independent serialized shape.");
			declared["postProcessProfileId"] = 17;
			context.Expect(!Serializer::DeserializeScenePostProcessProfileId(declared), "Reject a malformed profile before scene activation.");
			declared.erase("postProcessProfileId");
			context.Expect(Serializer::DeserializeScenePostProcessProfileId(declared) == EMPTY_UUID, "Scenes without the field inherit legacy defaults.");
		});

		registry.Add("Renderer.PostProcess.CameraAuthoring.ProfileUuidUsesProtectedFlush", [](TestContext& context) {
			CameraAuthoringScene scene;
			const auto property = Reflection::FindProperty(Reflection::ResolveType<CameraComponent>(), "PostProcessProfileId");
			context.Expect(Reflection::IsValid(property) && Reflection::IsType<UUID>(Reflection::GetPropertyType(property)),
				"Managed convention must resolve an actual typed UUID property, not a display string.");
			context.Expect(scene.Camera().PostProcessProfileId == EMPTY_UUID, "Legacy cameras must inherit settings with no authored profile.");
			const UUID profile = ProfileId(1);
			const auto componentGeneration = scene.objects.getObjectComponentGeneration(scene.object);
			const auto bindingGeneration = scene.objects.getObjectBindingGeneration(scene.object);
			SceneMutationQueue queue;
			auto transaction = scene.Patch("camera-profile-deferred", Reflection::Variant{profile});
			// Prepare observations on the protected side. The worker submits only
			// this owned typed packet and never reads live ECS/component storage.
			auto worker = std::async(std::launch::async, [&queue, transaction = std::move(transaction)]() mutable {
				Scheduling::WorkerSafetyScope scope("Camera profile packet producer");
				queue.EnqueueTransaction(std::move(transaction));
			});
			worker.get();
			context.Expect(scene.Camera().PostProcessProfileId == EMPTY_UUID, "Worker admission must not expose the UUID before its protected flush.");
			const auto result = queue.Apply(scene.objects, "Frame End");
			context.Expect(OnlyReceipt(context, result).status == TransactionStatus::Committed, "The ordinary reflected updateComponentProperty transaction must commit.");
			context.Expect(scene.Camera().PostProcessProfileId == profile, "The exact UUID must become visible at Frame End.");
			context.Expect(scene.objects.getObjectComponentGeneration(scene.object) == componentGeneration
				&& scene.objects.getObjectBindingGeneration(scene.object) == bindingGeneration,
				"An authored profile edit must preserve the camera and its exact view binding lifetime.");
			context.ExpectEqual(std::string("Frame End"), result.commitName, "Visibility diagnostics must retain the declared protected boundary.");
			context.Expect(queue.Empty(), "Committed profile work must be consumed exactly once.");
		});

		registry.Add("Renderer.PostProcess.CameraAuthoring.InvalidMixedBatchIsAtomic", [](TestContext& context) {
			CameraAuthoringScene scene;
			for (const auto& malformed : {Reflection::Variant{std::string("not-a-uuid")}, Reflection::Variant{17u}})
			{
				const auto before = Serializer::SerializeObject(scene.object.EntityId());
				const auto componentRevision = scene.objects.GetComponentMutationRevision();
				SceneMutationQueue queue;
				auto transaction = TransactionalMutationBuilder("camera-profile-invalid", TransactionOrigin::Test)
					.ObserveScene(scene.objects).ObserveObject(scene.objects, scene.objectId)
					.SetName(scene.objectId, "MustNotPublish")
					.UpdateComponentProperty(scene.objectId, std::string(CameraComponent::kComponentName), "PostProcessProfileId", malformed).Build();
				queue.EnqueueTransaction(std::move(transaction));
				const auto result = queue.Apply(scene.objects, "Frame End");
				const auto& receipt = OnlyReceipt(context, result);
				context.Expect(receipt.status == TransactionStatus::Rejected && receipt.appliedWriteCount == 0 && !receipt.conflicts.empty(),
					"A string or numeric value cannot impersonate a typed profile UUID; the full mixed batch must reject.");
				context.Expect(Serializer::SerializeObject(scene.object.EntityId()) == before,
					"Rejected UUID validation must preserve the earlier name and every authored camera field.");
				context.ExpectEqual(componentRevision, scene.objects.GetComponentMutationRevision(), "Rejected profile writes cannot advance component visibility.");
			}
		});

		registry.Add("Renderer.PostProcess.CameraAuthoring.LaterCommitFailureRollsBackUuid", [](TestContext& context) {
			CameraAuthoringScene scene;
			const UUID original = ProfileId(2);
			SceneMutationQueue initial;
			initial.EnqueueTransaction(scene.Patch("camera-profile-original", Reflection::Variant{original}));
			context.Expect(OnlyReceipt(context, initial.Apply(scene.objects, "Frame End")).status == TransactionStatus::Committed,
				"The rollback fixture starts from a genuinely committed nonempty UUID.");
			const auto before = Serializer::SerializeObject(scene.object.EntityId());
			const auto revision = scene.objects.GetComponentMutationRevision();
			auto failed = TransactionalMutationBuilder("camera-profile-rollback", TransactionOrigin::Test)
				.ObserveScene(scene.objects).ObserveObject(scene.objects, scene.objectId)
				.UpdateComponentProperty(scene.objectId, std::string(CameraComponent::kComponentName), "PostProcessProfileId", Reflection::Variant{ProfileId(3)})
				.SetName(scene.objectId, "NeverPublished").Build();
			// Real production failure injection runs after the first live write;
			// this checks journal restoration, not only preflight rejection.
			failed.injectedFailureOperationIndex = 1;
			SceneMutationQueue queue; queue.EnqueueTransaction(std::move(failed));
			const auto result = queue.Apply(scene.objects, "Frame End");
			context.Expect(OnlyReceipt(context, result).status == TransactionStatus::Rejected, "The later injected failure must reject the complete transaction.");
			context.Expect(scene.Camera().PostProcessProfileId == original && Serializer::SerializeObject(scene.object.EntityId()) == before,
				"Rollback must restore the exact prior UUID, component fields and metadata.");
			context.ExpectEqual(revision, scene.objects.GetComponentMutationRevision(), "Rolled-back UUID writes must not publish component revisions.");
		});

		registry.Add("Renderer.PostProcess.CameraAuthoring.SceneJsonPersistsAndValidatesUuid", [](TestContext& context) {
			CameraAuthoringScene scene;
			const UUID profile = ProfileId(4);
			SceneMutationQueue queue; queue.EnqueueTransaction(scene.Patch("camera-profile-persist", Reflection::Variant{profile}));
			context.Expect(OnlyReceipt(context, queue.Apply(scene.objects, "Frame End")).status == TransactionStatus::Committed,
				"Scene persistence starts from an actual protected camera transaction.");
			const auto saved = Serializer::SerializeObject(scene.object.EntityId());
			const auto& cameraJson = saved.at("components").at("CameraComponent");
			context.ExpectEqual(ToHexString(profile), cameraJson.at("PostProcessProfileId").get<std::string>(),
				"Actual camera scene JSON must persist the exact UUID as canonical hex.");
			std::string reason;
			auto malformed = cameraJson; malformed["PostProcessProfileId"] = "not-a-uuid";
			context.Expect(!Serializer::PrepareComponentRestore("CameraComponent", malformed, reason) && !reason.empty(),
				"Strict authored restoration must reject malformed UUID text before touching live components.");
			auto legacy = cameraJson; legacy.erase("PostProcessProfileId");
			context.Expect(bool(Serializer::PrepareComponentRestore("CameraComponent", legacy, reason)),
				"Legacy camera JSON may omit the optional profile and inherit the empty default.");
			scene.objects.clear();
			const Object restored = Serializer::DeserializeObject(saved);
			const auto* camera = scene.objects.GetComponent<CameraComponent>(restored);
			context.Expect(camera && camera->PostProcessProfileId == profile, "Ordinary scene deserialization must restore the actual camera UUID field.");
		});
	}
}
