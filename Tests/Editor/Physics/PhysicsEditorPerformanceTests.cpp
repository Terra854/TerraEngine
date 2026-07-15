// PhysicsEditorPerformanceTests.cpp - bounded editor performance-regression contracts.
//
// The large fixture exercises creation, serializer load, Play Mode entry, and
// paused Next behavior. It belongs to the dedicated performance CTest target so
// ordinary editor unit-test slices stay small and deterministic.

#include "TestHarness.h"

#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsDiagnostics.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/RenderSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/Serializer.h"
#include "ThreadPool/ThreadPool.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewMarkerPipeline.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests {
	namespace {
		constexpr std::size_t kLargeObjectCount = 10000U;
		constexpr std::size_t kDynamicBodyCount = 1000U;
		constexpr std::uint32_t kPausedNextRequestCount = 100U;

		// A local shared-pool lifetime prevents the high-scale editor fixture from
		// borrowing queued worker work from a prior test collection.
		class ThreadPoolSession final {
		public:
			ThreadPoolSession() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
			}

			~ThreadPoolSession() {
				ThreadPool::instance().shutdown();
			}
		};

		// Tests continue after failures, so always return singleton state to an
		// empty editor-owned scene and stop provider work before clearing objects.
		class EditorPhysicsPerformanceCleanup final {
		public:
			~EditorPhysicsPerformanceCleanup() {
				auto& physics = Physics::PhysicsSystem::Instance();
				(void)physics.StopWorld();
				physics.Shutdown();
				Physics::PublishedPhysicsDebugPackets().Clear();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
			}
		};

		std::vector<nlohmann::json> CreateAndCaptureLargePhysicsScene(TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			std::vector<nlohmann::json> serializedObjects;
			serializedObjects.reserve(kLargeObjectCount);

			for(std::size_t index = 0U; index < kLargeObjectCount; ++index) {
				Object object = objects.createObject("Physics Performance Object " + std::to_string(index));
				Transform* transform = objects.GetComponent<Transform>(object);
				context.Expect(transform != nullptr,
					"Every created editor object must expose its default Transform before performance capture.");
				transform->position = {
					static_cast<float>(index % 100U),
					static_cast<float>(index / 100U),
					static_cast<float>((index / 100U) % 10U)};

				// One thousand representative dynamic bodies make Play Mode entry and
				// fixed steps meaningful while the other objects retain authoring scale.
				if(index < kDynamicBodyCount) {
					RigidBodyComponent* body = objects.AddComponent<RigidBodyComponent>(object);
					ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(object);
					context.Expect(body != nullptr && colliders != nullptr,
						"Each performance dynamic must create a rigid body and a collider set.");
					body->useGravity = true;
					colliders->Add();
				}
				objects.MarkObjectTransformDirty(object);
				serializedObjects.push_back(Serializer::SerializeObject(object));
			}
			return serializedObjects;
		}

		void RestoreLargePhysicsScene(TestContext& context, const std::vector<nlohmann::json>& serializedObjects) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			for(const nlohmann::json& serializedObject : serializedObjects) {
				context.Expect(Serializer::DeserializeObjectEntity(serializedObject) != entt::null,
					"Each captured large-scene object must deserialize into a live editor entity.");
			}
			context.ExpectEqual(kLargeObjectCount, objects.getRootObjects().size(),
				"Loading the captured large scene must restore every authored root object.");
		}

		TerraEngine::UUID MakeMarkerUuid(std::uint64_t value)
		{
			TerraEngine::UUID id{};
			std::memcpy(id.data(), &value, sizeof(value));
			return id;
		}

		TerraEngine::Editor::SceneViewOverlay::MarkerFrameResult PrepareMarkerWorkload(
			std::size_t count,
			bool dense,
			TerraEngine::Editor::SceneViewOverlay::MarkerPipeline& pipeline)
		{
			using namespace TerraEngine::Editor::SceneViewOverlay;
			MarkerViewportQuery query;
			query.viewportKey.viewGeneration = 1u;
			query.viewportKey.projectionGeneration = 1u;
			query.viewportKey.viewportRectangle = {0, 0, 1280, 720};
			query.viewportKey.overlayPreferenceGeneration = 1u;
			query.viewportPosition = {0.0f, 0.0f};
			query.viewportSize = {1280.0f, 720.0f};
			for (std::size_t index = 0u; index < count; ++index)
			{
				const TerraEngine::UUID objectId = MakeMarkerUuid(index + 1u);
				const bool selected = index == 0u;
				const float x = dense ? 0.0f : -0.9f + static_cast<float>(index % 100u) * 0.018f;
				const float y = dense ? 0.0f : -0.9f + static_cast<float>((index / 100u) % 100u) * 0.018f;
				MarkerStaticInput staticInput;
				staticInput.objectId = objectId;
				staticInput.objectLifetimeGeneration = 1u;
				staticInput.entityReference = static_cast<entt::entity>(index + 1u);
				staticInput.componentGeneration = 1u;
				staticInput.metadataGeneration = 1u;
				staticInput.category = selected ? MarkerCategory::SelectedObject : MarkerCategory::Physics;
				staticInput.displayName = "Physics Marker " + std::to_string(index);
				staticInput.tooltip = staticInput.displayName;
				staticInput.selected = selected;
				(void)pipeline.UpsertStaticRecord(staticInput);

				MarkerPoseRecord pose;
				pose.objectId = objectId;
				pose.objectLifetimeGeneration = 1u;
				pose.transformRevision = 1u;
				pose.hierarchyGeneration = 1u;
				pose.worldPosition = {x, y, 0.0f};
				pose.worldBounds = TerraEngine::Scene::MakeFallbackBounds(pose.worldPosition, 0.05f);
				pose.movable = index < kDynamicBodyCount;
				(void)pipeline.UpsertPoseRecord(pose);

				TerraEngine::Scene::SpatialQueryHit hit;
				hit.objectId = objectId;
				hit.entity = staticInput.entityReference;
				hit.bounds = *pose.worldBounds;
				query.spatialCandidates.hits.push_back(hit);
			}
			query.viewportKey.markerPosePublicationGeneration = pipeline.PosePublicationGeneration();
			SceneOverlayPreference preference;
			preference.largeSceneMode = LargeSceneMode::Automatic;
			preference.largeSceneObjectThreshold = dense ? 1u : SceneOverlayPreference::kDefaultLargeSceneObjectThreshold;
			preference.visibleMarkerBudget = 512u;
			preference.markerPresentationBatchSize = 64u;
			preference.densityThreshold = 4u;
			return pipeline.PrepareViewport(query, preference);
		}
	}

	void RegisterPhysicsEditorPerformanceTests(TestRegistry& registry) {
		registry.Add("Physics.Editor.Performance.TenThousandObjectCreationLoadPlayModeAndPausedNextRemainObservableWithoutRenderer", [](TestContext& context) {
			ThreadPoolSession pool;
			EditorPhysicsPerformanceCleanup cleanup;
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& mutations = Scene::SceneMutationQueue::Instance();
			objects.clear();
			mutations.Clear();

			const std::vector<nlohmann::json> serializedObjects = CreateAndCaptureLargePhysicsScene(context);
			context.ExpectEqual(kLargeObjectCount, serializedObjects.size(),
				"The performance capture must serialize every one of the ten thousand created objects.");
			RestoreLargePhysicsScene(context, serializedObjects);

			auto& physics = Physics::PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(),
				"Large-scene Play Mode entry must build a fresh physics world from restored authored data.");
			context.Expect(physics.SetPaused(true),
				"The large-scene performance fixture must pause before issuing exact Next requests.");
			const std::uint64_t publicationsBefore = physics.ExplicitStepPublicationCount();
			for(std::uint32_t request = 0U; request < kPausedNextRequestCount; ++request) {
				context.Expect(physics.RequestExplicitStep(),
					"Each bounded paused Next request must be admitted before the following protected publication.");
				physics.Update(0.0F);
				const auto flush = mutations.Apply(objects, "editor-physics-performance-next");
				physics.OnAfterPhysicsFlush(flush.transactionResults, flush.publicationGroupResults);
			}
			context.ExpectEqual(publicationsBefore + kPausedNextRequestCount, physics.ExplicitStepPublicationCount(),
				"One hundred paused Next requests must publish exactly one fixed physics result each.");
			context.Expect(physics.IsPaused(),
				"The one-hundred-request performance pass must remain paused after every explicit fixed tick.");

			const auto diagnostics = physics.LatestDiagnostics();
			context.Expect(diagnostics != nullptr && diagnostics->counters.bodyCount >= kDynamicBodyCount,
				"Large-scene diagnostics must publish the dynamic-body count after Play Mode entry and fixed ticks.");
			context.Expect(diagnostics
				&& diagnostics->authoring.snapshotReused
				&& diagnostics->authoring.changedObjects == 0U
				&& diagnostics->preparation.estimatedWorkItems == 0U,
				"The canonical unchanged editor frame must avoid recurring authoring and preparation work.");
			context.Expect(diagnostics
				&& diagnostics->extraction.totalBodies >= kDynamicBodyCount
				&& diagnostics->stateStore.bodyCount >= kDynamicBodyCount
				&& diagnostics->stateStore.pageCount < diagnostics->stateStore.bodyCount,
				"The complete-step gate must retain all dynamic bodies in bounded shared state pages.");
			context.Expect(diagnostics && diagnostics->scheduler != nullptr && !diagnostics->scheduler->empty(),
				"Diagnostic publication must retain scheduler declarations without reading a provider object.");
			const auto packets = Physics::PublishedPhysicsDebugPackets().Select(physics.WorldHandle(), diagnostics ? diagnostics->tick : 0U);
			// Explicitly remove the render backend so this check cannot accidentally
			// pass through an RHI left alive by an unrelated editor test fixture.
			RenderSystem::Instance().Shutdown();
			context.Expect(!RenderSystem::Instance().SubmitSceneViewPhysicsDebugPackets(packets, {}),
				"Renderer-unavailable diagnostics submission must fail safely without changing the physics workload.");
			context.Expect(physics.StopWorld(),
				"Large-scene Play Mode exit must drain provider work before the performance fixture clears authored objects.");
		});

		registry.Add("Physics.Editor.Performance.SceneViewCanonicalDenseAndSmallMarkerGatesPreserveSelection", [](TestContext& context) {
			using namespace TerraEngine::Editor::SceneViewOverlay;
			MarkerPipeline canonicalPipeline;
			const MarkerFrameResult canonical = PrepareMarkerWorkload(kLargeObjectCount, false, canonicalPipeline);
			context.ExpectEqual(kLargeObjectCount, static_cast<std::size_t>(canonical.candidates.sourceCount), "The canonical gate must process the accepted ten-thousand-marker identity.");
			context.ExpectEqual(1u, static_cast<unsigned int>(canonical.candidates.reservedMarkerIds.size()), "The selected marker must remain outside the general budget.");
			context.Expect(canonical.candidates.generalCandidateIds.size() <= 512u, "The large-scene general marker budget must remain bounded.");
			context.Expect(!canonical.presentationBatches.empty() && canonical.presentationBatches.front().items.front().category == MarkerCategory::SelectedObject, "Deterministic presentation priority must keep the selected marker first.");

			MarkerPipeline densePipeline;
			const MarkerFrameResult dense = PrepareMarkerWorkload(512u, true, densePipeline);
			std::size_t densePresented = 0u;
			for (const MarkerPresentationBatch& batch : dense.presentationBatches)
			{
				densePresented += batch.items.size();
			}
			context.Expect(dense.screenIndex.entries.size() > densePresented, "A maximally dense neighborhood should consolidate low-priority physics markers.");
			context.ExpectEqual(1u, static_cast<unsigned int>(dense.candidates.reservedMarkerIds.size()), "Dense consolidation must preserve the selected exemption.");

			MarkerPipeline smallPipeline;
			const MarkerFrameResult small = PrepareMarkerWorkload(32u, false, smallPipeline);
			context.Expect(!small.candidates.effectiveLargeSceneMode, "Small scenes should avoid large-scene reduction overhead.");
			context.ExpectEqual(32u, static_cast<unsigned int>(small.screenIndex.entries.size()), "Small-scene markers should retain parity without budget rejection.");
			context.ExpectEqual(0u, small.candidates.budgetRejectedCount, "Small-scene parity must not reject markers through the large-scene budget.");
		});

		registry.Add("Physics.Editor.Performance.MaximumDebugBudgetKeepsPacketHistoryBounded", [](TestContext& context) {
			// This isolates memory retention from renderer allocation: every packet owns
			// a deliberately large immutable vector, so history capacity is the bound
			// that prevents a long maximum-debug session from accumulating old frames.
			Physics::PhysicsDebugPacketHistory history{2U};
			const Physics::PhysicsWorldHandle world{{901U}, 1U};
			for(Physics::PhysicsTick tick = 1U; tick <= 5U; ++tick) {
				auto packet = std::make_shared<Physics::PhysicsDebugPacket>();
				packet->world = world;
				packet->producedTick = tick;
				packet->expiryTick = tick + 8U;
				auto lines = std::make_shared<std::vector<Physics::PhysicsDebugLine>>();
				lines->resize(256U);
				packet->lines = std::move(lines);
				history.Publish(std::move(packet));
			}
			context.ExpectEqual(static_cast<std::size_t>(2U), history.Size(),
				"Maximum-debug packet retention must remain bounded even when each immutable packet reaches its primitive budget.");
			const auto selected = history.Select(world, 5U);
			context.ExpectEqual(static_cast<std::size_t>(2U), selected.size(),
				"Only the configured recent immutable debug packets may remain visible to an active Scene View.");

			auto replacement = std::make_shared<Physics::PhysicsDebugPacket>();
			replacement->world = world;
			replacement->producedTick = 20U;
			replacement->expiryTick = 21U;
			replacement->lines = std::make_shared<std::vector<Physics::PhysicsDebugLine>>(1U);
			history.Publish(std::move(replacement));
			context.ExpectEqual(static_cast<std::size_t>(1U), history.Size(),
				"Publishing a later debug frame must release expired maximum-budget packets before retaining new data.");
		});
	}
}
