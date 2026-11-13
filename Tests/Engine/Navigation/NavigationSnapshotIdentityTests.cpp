// Real preparation ownership and protected terrain-capture revision regressions.
#include "TestHarness.h"
#include "Components/TerrainComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Dynamic/NavigationPreparation.h"
#include "Navigation/Integration/NavigationRuntimeSceneCapture.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Resource/ResourceManager.h"
#include "Scene/Serializer.h"
#include "Terrain/TerrainAssetStore.h"

#include <memory>
#include <new>

namespace TerraEngine::Tests
{
	void RegisterNavigationSnapshotIdentityTests(TestRegistry& registry)
	{
		registry.Add("Navigation.SnapshotIdentity.ReusedCallerAddressGetsNewFrozenInput", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			using Bytes = std::vector<std::byte>;
			alignas(Bytes) std::byte storage[sizeof(Bytes)];
			// Each owner destroys its real vector, but the fixed storage is reused
			// under a fresh control block. No allocator coincidence is required.
			auto makeOwner = [&storage](std::byte value) {
				auto* bytes = std::construct_at(reinterpret_cast<Bytes*>(storage), 16U, value);
				return std::shared_ptr<const Bytes>(bytes, [](const Bytes* value) { std::destroy_at(value); });
			};
			NavigationPreparationCoordinator coordinator({});
			NavigationDynamicWorkRequest request;
			request.source.value = TerraEngine::GenerateUUID();
			request.revision = 1U;
			request.estimatedBytes = 16U;
			request.copiedInput = makeOwner(std::byte{0x22});
			const void* firstAddress = request.copiedInput.get();
			const std::weak_ptr<const Bytes> firstLifetime = request.copiedInput;
			const auto first = coordinator.Submit(request);
			context.Expect(first.IsValid(), "The first actual preparation packet must be admitted.");
			request.copiedInput.reset();
			context.Expect(firstLifetime.expired(), "The source lifetime must actually end before reconstructing its storage.");
			request.revision = 2U;
			request.copiedInput = makeOwner(std::byte{0x7f});
			context.Expect(request.copiedInput.get() == firstAddress, "The fixture must reuse exactly the same vector address under a new lifetime.");
			const auto second = coordinator.Submit(request);
			context.Expect(second.IsValid(), "The second lifetime must be admitted independently.");
			// Expired cache ownership must not release an older pending packet's
			// copied bytes while its successor is admitted under the same address.
			(void)coordinator.RunWorkerQuantumForTesting(2U);
			const auto firstResult = coordinator.ReadCompletion(first);
			const auto secondResult = coordinator.ReadCompletion(second);
			context.Expect(firstResult.state == NavigationPreparationState::Ready
				&& secondResult.state == NavigationPreparationState::Ready
				&& firstResult.firstInputByte == std::byte{0x22} && secondResult.firstInputByte == std::byte{0x7f}
				&& firstResult.inputFingerprint != secondResult.inputFingerprint,
				"A new caller lifetime at a reused address must freeze its new bytes rather than reuse the old snapshot.");
		});

		for(const std::uint32_t schema : {TerrainComponent::kSchemaVersion, 0U})
			registry.Add(schema == 0U ? "Navigation.SnapshotIdentity.DecodedZeroSchemaHasStableCaptureEvidence"
				: "Navigation.SnapshotIdentity.CurrentSchemaHasStableCaptureEvidence", [schema](TestContext& context) {
				using namespace TerraEngine::Navigation;
				ObjectManager& objects = ObjectManager::instance();
				objects.clear();
				struct Cleanup { ~Cleanup() { ObjectManager::instance().clear(); } } cleanup;
				ComponentRegistry::RegisterAllComponents();
				Object prototype = objects.createObject("Decoded terrain evidence");
				const auto asset = TerraEngine::GenerateUUID();
				auto* component = objects.AddComponent<TerrainComponent>(prototype);
				context.Expect(component != nullptr, "The actual TerrainComponent contract must be registered.");
				component->terrainAssetId = asset;
				auto authored = Serializer::SerializeObject(prototype);
				authored["components"]["TerrainComponent"]["schemaVersion"] = schema;
				objects.removeObject(prototype);
				const auto prepared = Serializer::PrepareObjectRestore(authored, 1U);
				// Unsupported zero must remain unsupported. Qualify rejection through
				// both concrete preparation and ordinary scene decoding, not substitution.
				const auto decodedEntity = Serializer::DeserializeObjectEntity(authored);
				const auto decoded = objects.getObject(decodedEntity);
				const auto* terrain = decoded ? objects.GetComponent<TerrainComponent>(*decoded) : nullptr;
				if(!prepared.accepted)
				{
					context.Expect(schema == 0U && (terrain == nullptr || terrain->schemaVersion != 0U),
						"Unsupported zero schema must reject consistently in prepared and ordinary decoding.");
					return;
				}
				context.Expect(terrain && terrain->schemaVersion == schema,
					"The exact requested schema must reach capture through the real decoder.");
				auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
				root->assetUuid = asset; root->publicationGeneration = 1U; root->rootContentHash = {1U, 2U};
				TerraEngine::Terrain::TerrainAssetStore store(root);
				TerraEngine::Physics::TerrainHeightFieldRecipe recipe;
				recipe.terrainAssetId = asset; recipe.authoredGeneration = 1U; recipe.authoredHash = 7U;
				recipe.heightMillimetres.assign(decltype(recipe)::kSampleCount, 0);
				recipe.holeBits.assign(decltype(recipe)::kHoleByteCount, 0U);
				const auto candidate = TerraEngine::Physics::Terrain::PrepareTerrainCollisionCandidate(recipe, root->rootContentHash, 1U);
				TerraEngine::Physics::Terrain::TerrainCollisionSnapshot collision;
				context.Expect(TerraEngine::Physics::Terrain::PublishTerrainCollisionCandidate(collision, candidate, store),
					"Capture must use a current production-prepared terrain collision publication.");
				const auto capture = CaptureNavigationRuntimeScene(objects, ResourceManager::Instance(), collision);
				context.Expect(capture.accepted && !capture.bootstrap.builds.empty(),
					"The decoded terrain must produce an actual protected navigation capture.");
				const auto& sources = capture.bootstrap.builds.front().sourceIndex;
				context.Expect(!sources.empty() && !NavigationAutomaticSourceStructureChanged(objects, sources),
					"An unchanged decoded schema must compare equal to its own captured revision evidence.");
				const auto repeated = CaptureNavigationRuntimeScene(objects, ResourceManager::Instance(), collision);
				context.Expect(repeated.accepted && !repeated.bootstrap.builds.empty()
					&& !repeated.bootstrap.builds.front().sourceIndex.empty()
					&& repeated.bootstrap.builds.front().sourceIndex.front().revisionEvidence == sources.front().revisionEvidence,
					"Repeated capture must preserve supported terrain revision evidence without rebuild churn.");
				// Protected fixture input changes must still invalidate the captured
				// evidence; normalization must not hide genuinely different schemas.
				auto* changed = objects.GetComponent<TerrainComponent>(*decoded);
				changed->schemaVersion = TerrainComponent::kSchemaVersion + 1U;
				objects.notifyObjectStructureChanged(*decoded);
				context.Expect(NavigationAutomaticSourceStructureChanged(objects, sources),
					"A real later terrain revision must invalidate previously captured geometry evidence.");
			});
	}
}
