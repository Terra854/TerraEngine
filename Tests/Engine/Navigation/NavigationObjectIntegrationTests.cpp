// NavigationObjectIntegrationTests.cpp - component-free object route contracts.

#include "../TestHarness.h"

#include "Components/Transform.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/TerrainComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Backend/DetourGroundShard.h"
#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/Geometry/TerrainNavigationSource.h"
#include "Navigation/Integration/NavigationRuntimeSceneCapture.h"
#include "Navigation/Integration/NavigationSourceTransformObservation.h"
#include "Navigation/NavigationSystem.h"
#include "Resource/ResourceManager.h"
#include "Scene/Serializer.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Navigation;

	namespace {
		NavigationLocation Location(const NavigationWorldHandle world, double x, double y = 0.0, double z = 0.0) {
			NavigationLocation location{};
			location.world = world;
			location.position.offsetMetres = {x, y, z};
			return location;
		}

		std::shared_ptr<const NavigationGeometrySnapshot> FlatTerrain(float elevationMetres = 0.0F) {
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{-16.0F, elevationMetres, -16.0F}, {16.0F, elevationMetres, 16.0F},
				{16.0F, elevationMetres, -16.0F}, {-16.0F, elevationMetres, 16.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			return NavigationGeometrySnapshot::Create(descriptor, document).snapshot;
		}

		bool PrepareTerrainWorld(NavigationSystem& system) {
			const auto terrain = FlatTerrain();
			if(!terrain || !system.QueueInitialGroundBuild(NavigationProfile::Defaults(), {terrain})) {
				return false;
			}
			system.Update(0.0F);
			return system.PublishedWorld() != nullptr;
		}

		bool PrepareStackedTerrainWorld(NavigationSystem& system) {
			const auto lowerTerrain = FlatTerrain(0.0F);
			const auto upperTerrain = FlatTerrain(128.0F);
			if(!lowerTerrain || !upperTerrain
				|| !system.QueueInitialGroundBuild(NavigationProfile::Defaults(), {lowerTerrain, upperTerrain})) {
				return false;
			}
			system.Update(0.0F);
			return system.PublishedWorld() != nullptr;
		}

		// Builds the same immutable terrain packet that the terrain/physics boundary
		// would publish. A one-cell-wide masked strip spans the whole local Z extent,
		// so a route cannot detour around it at either terrain edge.
		TerrainNavigationCaptureResult CaptureTerrainWithBarrierHole() {
			TerrainNavigationPublishedSamples terrain{};
			terrain.terrainPresent = true;
			terrain.descriptor.sourceUuid.value = TerraEngine::GenerateUUID();
			terrain.descriptor.sourceRevision = 1U;
			terrain.descriptor.canonicalPlacementRevision = 1U;
			terrain.descriptor.physicalReadyRevision = 1U;
			terrain.descriptor.sourceKind = NavigationSourceKind::Terrain;
			terrain.sampleColumns = 33U;
			terrain.sampleRows = 33U;
			terrain.sampleSpacingMetres = 1.0F;
			terrain.heights.assign(static_cast<std::size_t>(terrain.sampleColumns) * terrain.sampleRows, 0.0F);
			terrain.holeMask.assign(static_cast<std::size_t>(terrain.sampleColumns - 1U)
				* (terrain.sampleRows - 1U), 0U);

			constexpr std::uint32_t kBarrierColumn = 16U;
			for(std::uint32_t row = 0U; row + 1U < terrain.sampleRows; ++row) {
				const std::size_t cell = static_cast<std::size_t>(row) * (terrain.sampleColumns - 1U)
					+ kBarrierColumn;
				terrain.holeMask[cell] = 1U;
			}
			return CaptureTerrainNavigationSource(terrain);
		}

		bool SetObjectHeight(ObjectManager& objects, const Object& object, double heightMetres) {
			const auto pose = objects.GetCanonicalWorldPose(object);
			if(!pose) return false;
			Scene::WorldPose updated = *pose;
			updated.position.offsetMetres.y = heightMetres;
			return objects.SetCanonicalWorldPose(object, updated) == Scene::WorldCoordinateError::None;
		}

		bool SetObjectPosition(ObjectManager& objects, const Object& object, double x, double y, double z) {
			const auto pose = objects.GetCanonicalWorldPose(object);
			if(!pose) return false;
			Scene::WorldPose updated = *pose;
			updated.position.offsetMetres = {x, y, z};
			return objects.SetCanonicalWorldPose(object, updated) == Scene::WorldCoordinateError::None;
		}

		[[nodiscard]] UUID SequentialIdentity(const std::uint64_t value) {
			UUID identity{};
			// A fixed non-zero prefix and big-endian suffix make the regression's IDs
			// deterministic while modelling serialized sequential UUID allocation.
			identity[0U] = 0xa5U;
			for(std::size_t byte = 0U; byte < sizeof(value); ++byte) {
				const std::uint32_t shift = static_cast<std::uint32_t>((sizeof(value) - 1U - byte) * 8U);
				identity[identity.size() - sizeof(value) + byte] =
					static_cast<std::uint8_t>(value >> shift);
			}
			return identity;
		}

		[[nodiscard]] Physics::TerrainHeightFieldRecipe CurrentTerrainRecipe(const UUID& terrainAssetId) {
			Physics::TerrainHeightFieldRecipe recipe{};
			recipe.terrainAssetId = terrainAssetId;
			recipe.authoredGeneration = 1U;
			recipe.authoredHash = 1U;
			recipe.heightMillimetres.assign(Physics::TerrainHeightFieldRecipe::kSampleCount, 0);
			recipe.holeBits.assign(Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0U);
			return recipe;
		}

		bool PrepareAutomaticTerrainWorld(NavigationSystem& system) {
			// Model the protected bootstrap packet: all source identity, revision, and
			// geometry values are copied before NavigationSystem starts worker work.
			NavigationEligibleSourceCandidate candidate{};
			candidate.identity.sourceId = TerraEngine::GenerateUUID();
			candidate.identity.objectId = TerraEngine::GenerateUUID();
			candidate.identity.objectLifetimeGeneration = 1U;
			candidate.identity.assetId = TerraEngine::GenerateUUID();
			candidate.identity.kind = NavigationSourceKind::Terrain;
			candidate.revisions = {1U, 1U, 1U, 1U, 1U};
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{-16.0F, 0.0F, -16.0F}, {16.0F, 0.0F, 16.0F},
				{16.0F, 0.0F, -16.0F}, {-16.0F, 0.0F, 16.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceUuid.value = candidate.identity.sourceId;
			descriptor.sourceRevision = ComputeNavigationEligibleSourceRevision(
				candidate.identity, candidate.revisions);
			descriptor.canonicalPlacementRevision = candidate.revisions.transformRevision;
			descriptor.physicalReadyRevision = candidate.revisions.physicalPublicationGeneration;
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			candidate.geometry = NavigationGeometrySnapshot::Create(descriptor, document).snapshot;
			candidate.activeInHierarchy = true;
			candidate.physicalBackingCurrent = true;
			const std::vector<NavigationEligibleSourceCandidate> candidates{candidate};
			NavigationEligibleSourceIndexBuildResult indexBuild =
				BuildNavigationEligibleSourceIndex(candidates);
			if(!candidate.geometry || !indexBuild.accepted || !indexBuild.index.HasCurrentTerrain()) return false;

			NavigationSystem::InitialGroundBuildInput build{};
			build.profile = NavigationProfile::Defaults();
			build.sources = {candidate.geometry};
			build.sourceIndex = {{candidate.identity.sourceId, descriptor.sourceRevision,
				candidate.revisions.geometryRevision, true, candidate.identity, candidate.revisions}};
			build.eligibleSourceIndex = std::make_shared<const NavigationEligibleSourceIndex>(
				std::move(indexBuild.index));
			build.terrainPublicationGeneration = candidate.revisions.physicalPublicationGeneration;
			if(!system.QueueInitialGroundBuilds({std::move(build)})) return false;
			system.Update(0.0F);
			const auto root = system.PublishedWorld();
			return root != nullptr && root->terrainState == NavigationTerrainState::Available;
		}

		void FinishSystem(NavigationSystem& system, ObjectManager& objects) {
			system.Shutdown();
			objects.clear();
		}
	}

	void RegisterNavigationObjectIntegrationTests(TestRegistry& registry) {
		registry.Add("Navigation.ObjectIntegration.SourcePoseObservationIgnoresOrdinaryOwnerMotion", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object geometry = objects.createObject("Published geometry");
			const Object owner = objects.createObject("Ordinary navigation owner");
			objects.AddComponent<Transform>(geometry);
			objects.AddComponent<Transform>(owner);
			NavigationSourceIndexRecord source{};
			source.enabled = true;
			source.identity.objectId = objects.getUUID(geometry);
			source.identity.objectLifetimeGeneration = objects.getObjectLifetimeGeneration(geometry);
			source.revisionEvidence.transformRevision = objects.GetObjectTransformRevision(geometry);
			const std::vector<NavigationSourceIndexRecord> sources{source};
			context.Expect(!NavigationSourceTransformsChanged(objects, sources), "Unchanged source evidence must not request a rebuild.");
			const auto before = objects.GetSpatialRevision();
			context.Expect(SetObjectPosition(objects, owner, 4.0, 0.0, 0.0), "The ordinary owner's canonical move must apply.");
			context.Expect(objects.GetSpatialRevision() != before && !NavigationSourceTransformsChanged(objects, sources),
				"An unrelated owner move must advance scene spatial state without rebuilding navigation geometry.");
			context.Expect(SetObjectPosition(objects, geometry, 8.0, 0.0, 0.0), "The source canonical move must apply.");
			context.Expect(NavigationSourceTransformsChanged(objects, sources),
				"A changed published source must wake recapture even without a separate spatial publication.");
			objects.clear();
		});

		registry.Add("Navigation.ObjectIntegration.InitialCaptureUsesAuthoritativeSerializedTransformRevisions", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			ComponentRegistry::RegisterAllComponents();

			// Serialized WorldPose omits runtime transform revisions. Load enough
			// independent static colliders to make ObjectManager's per-object revision
			// visibly different from the default embedded pose value, matching a real
			// exported workload rather than hand-writing a stale source record.
			std::vector<nlohmann::json> serializedSources;
			serializedSources.reserve(40U);
			for(std::uint32_t index = 0U; index < 40U; ++index) {
				const Object source = objects.createObject("Serialized static navigation source");
				// Keep the fixture explicit about the serialized component contract even
				// if ObjectManager's creation defaults change in a future test harness.
				(void)objects.AddComponent<Transform>(source);
				objects.setObjectStatic(source, true);
				ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(source);
				if(colliders == nullptr) continue;
				Physics::ColliderRecord record{};
				record.recordId = TerraEngine::GenerateUUID();
				record.recordVersion = 1U;
				record.shape = Physics::BoxColliderShape{};
				colliders->colliders.push_back(record);
				serializedSources.push_back(Serializer::SerializeObject(source.EntityId()));
			}

			objects.clear();
			for(const nlohmann::json& serialized : serializedSources) {
				(void)Serializer::DeserializeObject(serialized);
			}

			Physics::Terrain::TerrainCollisionSnapshot collision{};
			const auto capture = CaptureNavigationRuntimeScene(objects, ResourceManager::Instance(), collision);
			const auto index = capture.bootstrap.eligibleSourceIndex;
			std::vector<NavigationSourceIndexRecord> publishedSources;
			bool authoritativeEvidence = index != nullptr && index->Records().size() == serializedSources.size();
			if(index != nullptr) {
				publishedSources.reserve(index->Records().size());
				for(const NavigationEligibleSourceRecord& captured : index->Records()) {
					const auto object = objects.getObject(captured.identity.objectId);
					if(!object || captured.geometry == nullptr) {
						authoritativeEvidence = false;
						continue;
					}
					authoritativeEvidence = authoritativeEvidence
						&& captured.revisions.transformRevision == objects.GetObjectTransformRevision(*object)
						&& captured.geometry->Descriptor().canonicalPlacementRevision
							== objects.GetObjectTransformRevision(*object);
					publishedSources.push_back({captured.identity.sourceId,
						captured.geometry->Descriptor().sourceRevision, captured.revisions.geometryRevision,
						true, captured.identity, captured.revisions});
				}
			}

			const Object unrelated = objects.createObject("Unrelated transform owner");
			(void)objects.AddComponent<Transform>(unrelated);
			const bool initialRowsCurrent = !NavigationSourceTransformsChanged(objects, publishedSources);
			const bool unrelatedMoveApplied = SetObjectPosition(objects, unrelated, 4.0, 0.0, 0.0);
			context.Expect(authoritativeEvidence
				&& initialRowsCurrent
				&& unrelatedMoveApplied
				&& !NavigationSourceTransformsChanged(objects, publishedSources),
				"Initial serialized source capture must use ObjectManager's canonical transform revision and ignore unrelated pose writes.");
			objects.clear();
		});

		registry.Add("Navigation.ObjectIntegration.StaticColliderEligibilityTracksHierarchyAndStaticState", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object parent = objects.createObject("Navigation source parent");
			const Object colliderObject = objects.createObject("Navigation source collider");
			objects.AddComponent<Transform>(parent);
			objects.AddComponent<Transform>(colliderObject);
			objects.setObjectStatic(colliderObject, true);
			objects.setParent(colliderObject, parent, ReparentMode::KeepWorld);
			ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(colliderObject);
			Physics::ColliderRecord record{};
			record.recordId = TerraEngine::GenerateUUID();
			record.recordVersion = 1U;
			record.shape = Physics::BoxColliderShape{};
			if(colliders != nullptr) colliders->colliders.push_back(record);

			NavigationSourceIndexRecord source{};
			source.enabled = true;
			source.identity.objectId = objects.getUUID(colliderObject);
			source.identity.objectLifetimeGeneration = objects.getObjectLifetimeGeneration(colliderObject);
			source.identity.assetId = record.recordId;
			source.identity.kind = NavigationSourceKind::Collider;
			source.identity.subshapeIndex = 0U;
			source.identity.sourceId = MakeNavigationRuntimeSourceId(source.identity.objectId,
			source.identity.assetId, source.identity.kind, source.identity.subshapeIndex);
			source.sourceId = source.identity.sourceId;
			source.sourceRevision = 1U;
			source.geometryRevision = record.recordVersion;
			source.revisionEvidence = {source.identity.objectLifetimeGeneration,
				objects.GetObjectTransformRevision(colliderObject), record.recordVersion,
				record.recordVersion, record.recordVersion};
			const std::vector<NavigationSourceIndexRecord> published{source};

			const bool initiallyCurrent = colliders != nullptr
				&& !NavigationAutomaticSourceStructureChanged(objects, published);
			objects.setObjectActive(parent, false);
			const bool parentDeactivationDetected = NavigationAutomaticSourceStructureChanged(objects, published)
				&& !NavigationAutomaticSourceStructureChanged(objects, {});
			objects.setObjectActive(parent, true);
			const bool parentReactivationDetected = NavigationAutomaticSourceStructureChanged(objects, {});
			objects.setObjectStatic(colliderObject, false);
			const bool staticnessChangeDetected = NavigationAutomaticSourceStructureChanged(objects, published);
			objects.setObjectStatic(colliderObject, true);
			const bool restoredCurrent = !NavigationAutomaticSourceStructureChanged(objects, published);

			// Add and remove a second eligible collider without changing the first
			// source row. Membership comparison must detect the addition, then become
			// current again once the protected object lifetime is removed.
			const Object addedCollider = objects.createObject("Added navigation source collider");
			objects.AddComponent<Transform>(addedCollider);
			objects.setObjectStatic(addedCollider, true);
			ColliderSetComponent* addedColliders = objects.AddComponent<ColliderSetComponent>(addedCollider);
			Physics::ColliderRecord addedRecord{};
			addedRecord.recordId = TerraEngine::GenerateUUID();
			addedRecord.recordVersion = 1U;
			addedRecord.shape = Physics::BoxColliderShape{};
			if(addedColliders != nullptr) addedColliders->colliders.push_back(addedRecord);
			const bool additionDetected = addedColliders != nullptr
				&& NavigationAutomaticSourceStructureChanged(objects, published);
			objects.removeObject(addedCollider);
			const bool additionRemovalRestored = !NavigationAutomaticSourceStructureChanged(objects, published);

			// Record versions are source-local authored evidence. This must wake a new
			// capture even if no object membership or global component revision changes.
			if(colliders != nullptr && !colliders->colliders.empty()) colliders->colliders.front().recordVersion = 2U;
			const bool recordVersionChangeDetected = NavigationAutomaticSourceStructureChanged(objects, published);
			if(colliders != nullptr && !colliders->colliders.empty()) colliders->colliders.front().recordVersion = 1U;
			const bool restoredAfterVersion = !NavigationAutomaticSourceStructureChanged(objects, published);
			objects.removeObject(colliderObject);
			const bool removalDetected = NavigationAutomaticSourceStructureChanged(objects, published);

			context.Expect(initiallyCurrent && parentDeactivationDetected && parentReactivationDetected
				&& staticnessChangeDetected && restoredCurrent && additionDetected && additionRemovalRestored
				&& recordVersionChangeDetected && restoredAfterVersion && removalDetected,
				"Automatic source capture must detect active membership, removal, and authored record-version changes while ignoring inactive collider additions.");
			objects.clear();
		});

		registry.Add("Navigation.ObjectIntegration.AcceptedAutomaticStagePreventsDuplicateStructureCapture", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();

			const NavigationEligibleSourceIdentity identity{
				.sourceId = TerraEngine::GenerateUUID(), .objectId = TerraEngine::GenerateUUID(),
				.objectLifetimeGeneration = 1U, .assetId = TerraEngine::GenerateUUID(),
				.kind = NavigationSourceKind::Terrain, .subshapeIndex = 0U};
			const auto makeBuild = [identity](NavigationGeneration revision)
				-> std::optional<NavigationSystem::InitialGroundBuildInput> {
				NavigationEligibleSourceCandidate candidate{};
				candidate.identity = identity;
				candidate.revisions = {identity.objectLifetimeGeneration, revision, revision, 1U, 1U};
				candidate.activeInHierarchy = true;
				candidate.physicalBackingCurrent = true;
				NavigationGeometryDocument document{};
				document.kind = NavigationGeometryPayloadKind::TriangleMesh;
				document.vertices = {{-8.0F, 0.0F, -8.0F}, {8.0F, 0.0F, 8.0F},
					{8.0F, 0.0F, -8.0F}, {-8.0F, 0.0F, 8.0F}};
				document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
				NavigationGeometrySnapshotDescriptor descriptor{};
				descriptor.sourceUuid.value = identity.sourceId;
				descriptor.sourceRevision = ComputeNavigationEligibleSourceRevision(identity, candidate.revisions);
				descriptor.canonicalPlacementRevision = revision;
				descriptor.physicalReadyRevision = 1U;
				descriptor.sourceKind = NavigationSourceKind::Terrain;
				candidate.geometry = NavigationGeometrySnapshot::Create(descriptor, document).snapshot;
				const NavigationEligibleSourceIndexBuildResult indexed =
					BuildNavigationEligibleSourceIndex(std::span<const NavigationEligibleSourceCandidate>{&candidate, 1U});
				if(!candidate.geometry || !indexed.accepted || !indexed.index.HasCurrentTerrain()
					|| indexed.index.Records().size() != 1U) return std::nullopt;

				const NavigationEligibleSourceRecord& accepted = indexed.index.Records().front();
				NavigationSourceIndexRecord row{};
				row.sourceId = accepted.identity.sourceId;
				row.sourceRevision = accepted.geometry->Descriptor().sourceRevision;
				row.geometryRevision = accepted.revisions.geometryRevision;
				row.enabled = true;
				row.identity = accepted.identity;
				row.revisionEvidence = accepted.revisions;
				NavigationSystem::InitialGroundBuildInput build{};
				build.profile = NavigationProfile::Defaults();
				build.sources = {candidate.geometry};
				build.sourceIndex = {row};
				build.eligibleSourceIndex = std::make_shared<const NavigationEligibleSourceIndex>(indexed.index);
				build.terrainPublicationGeneration = 1U;
				return build;
			};

			std::optional<NavigationSystem::InitialGroundBuildInput> first = makeBuild(1U);
			const bool firstAccepted = first.has_value() && system.QueueInitialGroundBuilds({std::move(*first)});
			const std::shared_ptr<const NavigationWorldRoot> oldRoot = system.PublishedWorld();
			const std::vector<NavigationSourceIndexRecord> pending = system.AutomaticSourceObservationSnapshot();
			NavigationSystem::InitialGroundBuildInput malformed{};
			malformed.profile = NavigationProfile::Defaults();
			const bool rejectedWithoutReplacingStage = !system.QueueInitialGroundBuilds({std::move(malformed)});
			const std::vector<NavigationSourceIndexRecord> afterRejected = system.AutomaticSourceObservationSnapshot();

			std::optional<NavigationSystem::InitialGroundBuildInput> replacement = makeBuild(2U);
			const bool replacementAccepted = replacement.has_value()
				&& system.QueueInitialGroundBuilds({std::move(*replacement)});
			const std::vector<NavigationSourceIndexRecord> newest = system.AutomaticSourceObservationSnapshot();
			const bool pendingPreferredOverOldRoot = oldRoot != nullptr && oldRoot->sources.empty()
				&& pending.size() == 1U && pending.front().sourceId == identity.sourceId;
			const bool rejectionPreservedAcceptedStage = afterRejected.size() == 1U && pending.size() == 1U
				&& afterRejected.front().sourceRevision == pending.front().sourceRevision;
			const bool newestAcceptedStageReplacedPrior = newest.size() == 1U && pending.size() == 1U
				&& newest.front().sourceId == pending.front().sourceId
				&& newest.front().sourceRevision != pending.front().sourceRevision;

			context.Expect(firstAccepted && pendingPreferredOverOldRoot && rejectedWithoutReplacingStage
				&& rejectionPreservedAcceptedStage && replacementAccepted && newestAcceptedStageReplacedPrior,
				"Accepted automatic source evidence must remain observable before root publication, survive a rejected batch, and be replaced only by a newer accepted capture.");
			system.Shutdown();
		});
		registry.Add("Navigation.ObjectIntegration.OrdinaryObjectRoutesToLocationWithoutNavigationComponents", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			const auto world = system.PublishedWorld();
			const Object source = objects.createObject("Ordinary Route Source");
			Transform* transform = objects.AddComponent<Transform>(source);
			if(transform != nullptr) transform->worldPose.position.offsetMetres = {0.0, 0.0, 0.0};
			const bool prepared = PrepareTerrainWorld(system);
			const auto receipt = world && prepared
				? system.RequestRoute(objects, objects.getUUID(source), Location(world->world, 5.0), NavigationProfile::Defaults())
				: NavigationSystem::ObjectRouteReceipt{};
			for(std::uint32_t frame = 0U; frame < 3U; ++frame) system.Update(0.0F);
			const auto request = system.ReadRequest(receipt.request);
			const NavigationRouteHandle routeHandle = system.PublishedRouteForRequest(receipt.request);
			const auto route = system.ReadRoute(routeHandle);
			const auto validation = system.ValidateRoute(routeHandle);

			context.Expect(transform != nullptr && receipt.Succeeded(),
				"Any active object with a Transform must admit a location route without adding a navigation component.");
			context.Expect(request.record.has_value() && request.record->outcome == NavigationOutcome::Complete
				&& route.IsValid() && !route.points.empty() && validation.usable,
				"A terrain-backed ordinary-object request must expose a copied complete route through the request store.");
			context.Expect(transform != nullptr && transform->worldPose.position.offsetMetres.x == 0.0,
				"Route computation must not write the source Transform; gameplay remains the movement authority.");
			FinishSystem(system, objects);
		});

		registry.Add("Navigation.ObjectIntegration.ObjectTargetCapturesCurrentPoseAndDoesNotChase", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			const Object source = objects.createObject("Object Route Source");
			const Object target = objects.createObject("Object Route Target");
			Transform* sourceTransform = objects.AddComponent<Transform>(source);
			Transform* targetTransform = objects.AddComponent<Transform>(target);
			if(sourceTransform != nullptr) sourceTransform->worldPose.position.offsetMetres = {0.0, 0.0, 0.0};
			if(targetTransform != nullptr) targetTransform->worldPose.position.offsetMetres = {5.0, 0.0, 0.0};
			const bool prepared = PrepareTerrainWorld(system);
			const auto receipt = prepared ? system.RequestRoute(objects, objects.getUUID(source),
				objects.getUUID(target), NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
			// The destination is an admission-time value, not a hidden pursuit
			// component. Moving the target afterwards must not rewrite this request.
			if(targetTransform != nullptr) targetTransform->worldPose.position.offsetMetres = {9.0, 0.0, 0.0};
			for(std::uint32_t frame = 0U; frame < 3U; ++frame) system.Update(0.0F);
			const auto request = system.ReadRequest(receipt.request);

			context.Expect(receipt.Succeeded() && request.record.has_value()
				&& request.record->destinationEndpoint.object.objectId == objects.getUUID(target)
				&& targetTransform != nullptr
				&& request.record->destination.position.offsetMetres.x == 5.0,
				"Object-to-object routing must copy the target UUID, lifetime, and current canonical pose at admission.");
			FinishSystem(system, objects);
		});

		registry.Add("Navigation.ObjectIntegration.PosePublicationAfterAdmissionIsRetryableStaleForDirectAndAgentRoutes", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();

			const Object directSource = objects.createObject("Direct Pose Publication Source");
			const Object agentSource = objects.createObject("Agent Pose Publication Source");
			const bool prepared = PrepareTerrainWorld(system);
			const auto world = system.PublishedWorld();
			const UUID directSourceId = objects.getUUID(directSource);
			const UUID agentSourceId = objects.getUUID(agentSource);
			const NavigationObjectIdentity agentIdentity{
				agentSourceId, objects.getObjectLifetimeGeneration(agentSource)};

			const auto direct = prepared && world
				? system.RequestRoute(objects, directSourceId, Location(world->world, 5.0), NavigationProfile::Defaults())
				: NavigationSystem::ObjectRouteReceipt{};
			const auto agent = prepared && world
				? system.RequestAgentRoute(objects, agentIdentity, Location(world->world, 5.0), NavigationProfile::Defaults())
				: NavigationSystem::ObjectRouteReceipt{};

			// Physics publishes an extracted canonical pose at a protected boundary.
			// Re-publishing the exact initial value must advance the transform revision
			// without converting an otherwise valid in-flight request into InvalidStart.
			const auto directPose = objects.GetCanonicalWorldPose(directSource);
			const auto agentPose = objects.GetCanonicalWorldPose(agentSource);
			const std::uint64_t directRevisionBefore = objects.GetObjectTransformRevision(directSource);
			const std::uint64_t agentRevisionBefore = objects.GetObjectTransformRevision(agentSource);
			const bool directPublished = directPose.has_value()
				&& objects.SetCanonicalWorldPose(directSource, *directPose) == Scene::WorldCoordinateError::None;
			const bool agentPublished = agentPose.has_value()
				&& objects.SetCanonicalWorldPose(agentSource, *agentPose) == Scene::WorldCoordinateError::None;
			const bool revisionsAdvanced = objects.GetObjectTransformRevision(directSource) > directRevisionBefore
				&& objects.GetObjectTransformRevision(agentSource) > agentRevisionBefore;

			for(std::uint32_t frame = 0U; frame < 8U; ++frame) system.Update(0.0F);
			const auto directRequest = system.ReadRequest(direct.request);
			const auto agentRequest = system.ReadRequest(agent.request);

			context.Expect(direct.Succeeded() && agent.Succeeded() && directPublished && agentPublished
				&& revisionsAdvanced,
				"A protected same-pose publication must be able to occur after direct and agent route admission.");
			context.Expect(directRequest.record.has_value() && agentRequest.record.has_value()
				&& directRequest.record->outcome == NavigationOutcome::Stale
				&& agentRequest.record->outcome == NavigationOutcome::Stale
				&& directRequest.record->reason == NavigationReason::DirtyCoverage
				&& agentRequest.record->reason == NavigationReason::DirtyCoverage,
				"A source revision race must report retryable Stale rather than misdiagnosing either route as InvalidStart.");
			FinishSystem(system, objects);
		});

		registry.Add("Navigation.ObjectIntegration.BoundarySupportProjectionAcceptsPhysicsEpsilonWithoutMergingFloors", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();

			const Object epsilonDirect = objects.createObject("Negative Epsilon Direct Source");
			const Object epsilonAgent = objects.createObject("Negative Epsilon Agent Source");
			const Object lowerFloor = objects.createObject("Lower Stacked Floor Source");
			const Object upperFloor = objects.createObject("Upper Stacked Floor Source");
			const Object belowFloor = objects.createObject("Far Below Support Source");
			const bool heightsSet = SetObjectHeight(objects, epsilonDirect, -5.0e-9)
				&& SetObjectHeight(objects, epsilonAgent, -5.0e-9)
				&& SetObjectHeight(objects, lowerFloor, 0.0)
				&& SetObjectHeight(objects, upperFloor, 128.0)
				&& SetObjectHeight(objects, belowFloor, -0.5);
			const bool prepared = PrepareStackedTerrainWorld(system);
			const auto world = system.PublishedWorld();
			const NavigationObjectIdentity epsilonAgentIdentity{
				objects.getUUID(epsilonAgent), objects.getObjectLifetimeGeneration(epsilonAgent)};

			const auto directEpsilon = prepared && world ? system.RequestRoute(objects, objects.getUUID(epsilonDirect),
				Location(world->world, 5.0, 0.0), NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
			const auto agentEpsilon = prepared && world ? system.RequestAgentRoute(objects, epsilonAgentIdentity,
				Location(world->world, 5.0, 0.0), NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
			const auto lowerRoute = prepared && world ? system.RequestRoute(objects, objects.getUUID(lowerFloor),
				Location(world->world, 5.0, 0.0), NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
			const auto upperRoute = prepared && world ? system.RequestRoute(objects, objects.getUUID(upperFloor),
				Location(world->world, 5.0, 128.0), NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
			const auto farBelowRoute = prepared && world ? system.RequestRoute(objects, objects.getUUID(belowFloor),
				Location(world->world, 5.0, 0.0), NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
			for(std::uint32_t frame = 0U; frame < 8U; ++frame) system.Update(0.0F);

			const auto directEpsilonRead = system.ReadRequest(directEpsilon.request);
			const auto agentEpsilonRead = system.ReadRequest(agentEpsilon.request);
			const auto lowerRead = system.ReadRequest(lowerRoute.request);
			const auto upperRead = system.ReadRequest(upperRoute.request);
			const auto farBelowRead = system.ReadRequest(farBelowRoute.request);

			context.Expect(heightsSet && directEpsilon.Succeeded() && agentEpsilon.Succeeded()
				&& lowerRoute.Succeeded() && upperRoute.Succeeded() && farBelowRoute.Succeeded(),
				"Boundary projection fixtures must admit direct and agent requests from their exact canonical heights.");
			context.Expect(directEpsilonRead.record && agentEpsilonRead.record
				&& directEpsilonRead.record->outcome == NavigationOutcome::Complete
				&& agentEpsilonRead.record->outcome == NavigationOutcome::Complete,
				"A negative physics contact epsilon must project onto adjacent terrain support for direct and agent requests.");
			context.Expect(lowerRead.record && upperRead.record
				&& lowerRead.record->outcome == NavigationOutcome::Complete
				&& upperRead.record->outcome == NavigationOutcome::Complete,
				"Independent lower and upper vertical bands must retain their own physically supported stacked-floor routes.");
			context.Expect(farBelowRead.record && farBelowRead.record->outcome == NavigationOutcome::UnavailableCoverage
				&& farBelowRead.record->reason == NavigationReason::MissingNeighbor,
				"A point materially below support must not borrow the neighbouring terrain band through endpoint projection.");
			FinishSystem(system, objects);
		});

		registry.Add("Navigation.ObjectIntegration.PublishedTerrainHoleNeverCompletesAcrossMaskedSupport", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			(void)system.ConfigureSettings(NavigationSettings::Defaults());
			system.Initialize();

			// Both owners stand on retained terrain, while their shared destination lies
			// beyond the fully masked strip. Separate owners prevent ordinary object-route
			// replacement semantics from hiding either profile's terminal result.
			const Object smallOwner = objects.createObject("Terrain Hole Small Route Source");
			const Object largeOwner = objects.createObject("Terrain Hole Large Route Source");
			(void)objects.AddComponent<Transform>(smallOwner);
			(void)objects.AddComponent<Transform>(largeOwner);
			const bool ownersPlaced = SetObjectPosition(objects, smallOwner, 8.0, 0.0, 16.0)
				&& SetObjectPosition(objects, largeOwner, 8.0, 0.0, 16.0);

			const TerrainNavigationCaptureResult captured = CaptureTerrainWithBarrierHole();
			const bool barrierWasRemoved = captured.snapshot != nullptr
				&& captured.snapshot->Triangles().size() == 32U * 31U * 2U;

			NavigationProfile largeProfile = NavigationProfile::Defaults();
			largeProfile.radiusMetres = 0.70;
			largeProfile.heightMetres = 2.20;
			largeProfile.stepHeightMetres = 0.30;
			// Queue both profiles as one initial batch. The regression is about the
			// terrain hole, not deferred profile preparation, so both requests must see
			// complete coverage produced from the exact same copied heightfield.
			NavigationSystem::InitialGroundBuildInput smallBuild{};
			smallBuild.profile = NavigationProfile::Defaults();
			NavigationSystem::InitialGroundBuildInput largeBuild{};
			largeBuild.profile = largeProfile;
			if(captured.snapshot != nullptr) {
				smallBuild.sources = {captured.snapshot};
				largeBuild.sources = {captured.snapshot};
			}
			const bool buildQueued = captured.accepted && captured.snapshot != nullptr
				&& system.QueueInitialGroundBuilds({std::move(smallBuild), std::move(largeBuild)});
			for(std::uint32_t frame = 0U; frame < 16U; ++frame) system.Update(0.0F);
			const auto world = system.PublishedWorld();

			const NavigationLocation destination = world
				? Location(world->world, 24.0, 0.0, 16.0) : NavigationLocation{};
			const auto small = ownersPlaced && world
				? system.RequestRoute(objects, objects.getUUID(smallOwner), destination,
					NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
			const auto large = ownersPlaced && world
				? system.RequestRoute(objects, objects.getUUID(largeOwner), destination,
					largeProfile) : NavigationSystem::ObjectRouteReceipt{};
			// A fixed bounded pump gives both route packets their normal protected
			// capture, worker, and publication boundaries without depending on timing.
			for(std::uint32_t frame = 0U; frame < 16U; ++frame) system.Update(0.0F);
			const auto smallRead = system.ReadRequest(small.request);
			const auto largeRead = system.ReadRequest(large.request);

			const auto blockedByPublishedHole = [](const NavigationStoreReadResult<NavigationRequestRecord>& read) {
				return read.record.has_value()
					&& read.record->outcome == NavigationOutcome::Unreachable
					&& read.record->reason == NavigationReason::MissingSupport;
			};
			const auto describeTerminal = [](std::string_view profile,
				const NavigationStoreReadResult<NavigationRequestRecord>& read) {
				if(!read.record) return std::string(profile) + "=missing-record";
				return std::string(profile) + "=" + std::string(ToString(read.record->lifecycle))
					+ "/" + std::string(ToString(read.record->outcome))
					+ "/" + std::string(ToString(read.record->reason));
			};
			context.Expect(ownersPlaced && captured.accepted && captured.hasNavigableTriangles
				&& barrierWasRemoved && buildQueued && world != nullptr
				&& small.Succeeded() && large.Succeeded(),
				"The terrain-hole regression requires a copied heightfield packet, published terrain coverage, and two admitted profile requests.");
			context.Expect(blockedByPublishedHole(smallRead) && blockedByPublishedHole(largeRead),
				"A terrain heightfield hole must remain absent from the published provider mesh: small and large routes across its full-width mask must report Unreachable/MissingSupport, never Complete. Actual "
					+ describeTerminal("small", smallRead) + ", " + describeTerminal("large", largeRead) + ".");

			FinishSystem(system, objects);
			(void)system.ConfigureSettings(NavigationSettings::Defaults());
		});

		registry.Add("Navigation.ObjectIntegration.ExtremeCanonicalCellsRejectCoverageWithoutSignedOverflow", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();

			const Object source = objects.createObject("Extreme Canonical Route Source");
			const bool prepared = PrepareTerrainWorld(system);
			const auto world = system.PublishedWorld();
			NavigationLocation distantDestination{};
			if(world) {
				distantDestination = Location(world->world, 0.0);
				distantDestination.position.cell.x = std::numeric_limits<std::int64_t>::max();
			}
			const auto receipt = prepared && world ? system.RequestRoute(objects, objects.getUUID(source),
				distantDestination, NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
			for(std::uint32_t frame = 0U; frame < 3U; ++frame) system.Update(0.0F);
			const auto request = system.ReadRequest(receipt.request);

			context.Expect(receipt.Succeeded() && request.record
				&& request.record->outcome == NavigationOutcome::UnavailableCoverage
				&& request.record->reason == NavigationReason::MissingNeighbor,
				"A valid extreme canonical address must fail closed as unavailable coverage instead of overflowing its shard key.");
			FinishSystem(system, objects);
		});

		registry.Add("Navigation.ObjectIntegration.ExplicitProfileDemandsAutomaticCoverage", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			const Object source = objects.createObject("Explicit Profile Route Source");
			Transform* transform = objects.AddComponent<Transform>(source);
			if(transform != nullptr) transform->worldPose.position.offsetMetres = {0.0, 0.0, 0.0};
			const bool prepared = PrepareAutomaticTerrainWorld(system);
			const auto root = system.PublishedWorld();
			NavigationProfile explicitProfile = NavigationProfile::Defaults();
			explicitProfile.radiusMetres = 0.4;
			explicitProfile.heightMetres = 1.2;
			const auto receipt = prepared && root ? system.RequestRoute(objects, objects.getUUID(source),
				Location(root->world, 5.0), explicitProfile) : NavigationSystem::ObjectRouteReceipt{};
			// First update publishes profile-specific coverage, the next prepares the
			// route, and the last publishes its result at the protected boundary.
			for(std::uint32_t frame = 0U; frame < 3U; ++frame) system.Update(0.0F);
			const auto request = system.ReadRequest(receipt.request);
			const auto completedRoot = system.PublishedWorld();
			const std::uint64_t explicitKey = MakeGroundProfileBuildKey(explicitProfile);
			const bool profileCoveragePublished = completedRoot != nullptr
				&& std::any_of(completedRoot->coverage.begin(), completedRoot->coverage.end(),
					[explicitKey](const NavigationCoverageRecord& coverage) {
						return coverage.key.profileBuildKey == explicitKey
							&& coverage.state == NavigationCoverageState::Ready && coverage.complete;
					});

			context.Expect(transform != nullptr && receipt.Succeeded() && profileCoveragePublished,
				"A valid caller-owned profile must demand its own automatic terrain coverage without a preset registration.");
			context.Expect(request.record.has_value() && request.record->outcome == NavigationOutcome::Complete,
				"An object route using copied explicit profile values must wait for and then search matching coverage.");

			// A later automatic capture carries new geometry/index evidence. It must
			// retire every old profile key before the same copied profile can rebuild.
			const NavigationGeneration firstIndexRevision = completedRoot != nullptr
				&& completedRoot->eligibleSourceIndex != nullptr
				? completedRoot->eligibleSourceIndex->Revision() : 0U;
			const bool refreshed = PrepareAutomaticTerrainWorld(system);
			const auto refreshedRoot = system.PublishedWorld();
			const bool oldExplicitCoverageRetired = refreshedRoot != nullptr
				&& refreshedRoot->eligibleSourceIndex != nullptr
				&& refreshedRoot->eligibleSourceIndex->Revision() != firstIndexRevision
				&& std::none_of(refreshedRoot->coverage.begin(), refreshedRoot->coverage.end(),
					[explicitKey](const NavigationCoverageRecord& coverage) {
						return coverage.key.profileBuildKey == explicitKey;
					});
			const auto refreshedReceipt = refreshed && refreshedRoot ? system.RequestRoute(objects,
				objects.getUUID(source), Location(refreshedRoot->world, 5.0), explicitProfile)
				: NavigationSystem::ObjectRouteReceipt{};
			for(std::uint32_t frame = 0U; frame < 3U; ++frame) system.Update(0.0F);
			const auto refreshedRequest = system.ReadRequest(refreshedReceipt.request);

			context.Expect(oldExplicitCoverageRetired,
				"A newer automatic source capture must not retain ready explicit-profile coverage from old geometry evidence.");
			context.Expect(refreshedReceipt.Succeeded() && refreshedRequest.record.has_value()
				&& refreshedRequest.record->outcome == NavigationOutcome::Complete,
				"The same explicit profile must demand and receive fresh coverage after automatic geometry changes.");
			FinishSystem(system, objects);
		});

		registry.Add("Navigation.ObjectIntegration.SequentialSourceTuplesDoNotCollapseAutomaticCapture", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();

			// The exported workload assigns sequential UUIDs to objects and collider
			// records. Deliberately make each pair equal: the previous XOR key reduced
			// every such pair to the same source identity and silently dropped blockers.
			const Object terrainObject = objects.createObject("Sequential Terrain");
			const UUID terrainId = SequentialIdentity(1U);
			objects.UpdateObjectUUID(terrainObject, terrainId);
			TerrainComponent* terrain = objects.AddComponent<TerrainComponent>(terrainObject);
			if(terrain != nullptr) terrain->terrainAssetId = terrainId;

			for(std::uint64_t index = 2U; index <= 66U; ++index) {
				const Object blocker = objects.createObject("Sequential Navigation Blocker");
				const UUID pairedId = SequentialIdentity(index);
				objects.UpdateObjectUUID(blocker, pairedId);
				objects.setObjectStatic(blocker, true);
				ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(blocker);
				if(colliders == nullptr) continue;
				Physics::ColliderRecord record{};
				record.recordId = pairedId;
				record.shape = Physics::BoxColliderShape{};
				colliders->colliders.push_back(std::move(record));
			}

			Physics::Terrain::TerrainCollisionSnapshot collision{};
			collision.status = Physics::Terrain::TerrainCollisionStatus::Current;
			collision.sourcePublicationGeneration = 1U;
			collision.currentRecipe = CurrentTerrainRecipe(terrainId);
			const auto capture = CaptureNavigationRuntimeScene(
				objects, ResourceManager::Instance(), collision);
			const auto index = capture.bootstrap.eligibleSourceIndex;
			std::vector<UUID> sourceIds;
			if(index != nullptr) {
				sourceIds.reserve(index->Records().size());
				for(const NavigationEligibleSourceRecord& record : index->Records()) {
					sourceIds.push_back(record.identity.sourceId);
				}
			}
			std::sort(sourceIds.begin(), sourceIds.end());
			const bool uniqueSourceIds = std::adjacent_find(sourceIds.begin(), sourceIds.end()) == sourceIds.end();

			context.Expect(capture.accepted && capture.bootstrap.terrainAvailable && index != nullptr
				&& index->Records().size() == 66U && index->Exclusions().empty()
				&& sourceIds.size() == 66U && uniqueSourceIds,
				"Automatic capture must retain one distinct source ID for terrain plus 65 sequential object/collider tuples.");
			objects.clear();
		});

		registry.Add("Navigation.ObjectIntegration.AutomaticTerrainPlacementRoutesAcrossPublishedShardSeam", [](TestContext& context) {
			// Repeat real capture and diagonal stitching beyond global-double unit precision.
			for(const std::int64_t cell : {0LL, 1000000000000LL, -1000000000000LL}) {
				for(const double elevation : {0.0, 10.0}) {
				ObjectManager& objects = ObjectManager::instance();
				objects.clear();
				NavigationSystem& system = NavigationSystem::Instance();
				system.Shutdown();
				system.Initialize();

				// The runtime fixture's terrain starts at (-64, -64), so the 257-sample
				// physical heightfield spans the -1 to 0 navigation shard seam. This uses
				// the real protected capture packet rather than hand-authored tile keys.
				const auto placeAtCell = [&objects, cell](const Object& object, double x, double y, double z) {
					auto pose = objects.GetCanonicalWorldPose(object);
					if(!pose) return false;
					pose->position.cell = {cell, 0, cell};
					pose->position.offsetMetres = {x, y, z};
					return objects.SetCanonicalWorldPose(object, *pose) == Scene::WorldCoordinateError::None;
				};
				const Object terrainObject = objects.createObject("Placed Automatic Terrain");
				const UUID terrainAssetId = SequentialIdentity(900U);
				TerrainComponent* terrain = objects.AddComponent<TerrainComponent>(terrainObject);
				const bool terrainPlaced = placeAtCell(terrainObject, -64.0, elevation, -64.0);
				if(terrain != nullptr) terrain->terrainAssetId = terrainAssetId;
				Physics::Terrain::TerrainCollisionSnapshot collision{};
				collision.status = Physics::Terrain::TerrainCollisionStatus::Current;
				collision.sourcePublicationGeneration = 1U;
				collision.currentRecipe = CurrentTerrainRecipe(terrainAssetId);
				const auto capture = CaptureNavigationRuntimeScene(
					objects, ResourceManager::Instance(), collision);
				const auto submission = SubmitNavigationRuntimeSceneCapture(system, capture);
				for(std::uint32_t frame = 0U; frame < 4U; ++frame) system.Update(0.0F);
				const auto root = system.PublishedWorld();

				const Object owner = objects.createObject("Placed Terrain Route Owner");
				const bool ownerPlaced = placeAtCell(owner, -62.0, elevation, -62.0);
				const Object negativeShardOwner = objects.createObject("Placed Terrain Negative Shard Owner");
				const bool negativeShardOwnerPlaced = placeAtCell(negativeShardOwner, -62.0, elevation, -62.0);
				const Object positiveShardOwner = objects.createObject("Placed Terrain Positive Shard Owner");
				const bool positiveShardOwnerPlaced = placeAtCell(positiveShardOwner, 2.0, elevation, 2.0);
				const Object singleSeamOwner = objects.createObject("Placed Terrain Single Seam Owner");
				const bool singleSeamOwnerPlaced = placeAtCell(singleSeamOwner, -2.0, elevation, 2.0);
				NavigationLocation destination{};
				NavigationLocation negativeShardDestination{};
				NavigationLocation positiveShardDestination{};
				NavigationLocation singleSeamDestination{};
				if(root != nullptr) {
					destination.world = root->world;
					destination.position.cell = {cell, 0, cell};
					destination.position.offsetMetres = {62.0, elevation, 62.0};
					negativeShardDestination.world = root->world;
					negativeShardDestination.position.cell = {cell, 0, cell};
					negativeShardDestination.position.offsetMetres = {-2.0, elevation, -2.0};
					positiveShardDestination.world = root->world;
					positiveShardDestination.position.cell = {cell, 0, cell};
					positiveShardDestination.position.offsetMetres = {62.0, elevation, 62.0};
					singleSeamDestination.world = root->world;
					singleSeamDestination.position.cell = {cell, 0, cell};
					singleSeamDestination.position.offsetMetres = {2.0, elevation, 2.0};
				}
				const auto receipt = root != nullptr && submission.accepted
					? system.RequestRoute(objects, objects.getUUID(owner), destination, NavigationProfile::Defaults())
					: NavigationSystem::ObjectRouteReceipt{};
				const auto negativeShardReceipt = root != nullptr && submission.accepted
					? system.RequestRoute(objects, objects.getUUID(negativeShardOwner), negativeShardDestination, NavigationProfile::Defaults())
					: NavigationSystem::ObjectRouteReceipt{};
				const auto positiveShardReceipt = root != nullptr && submission.accepted
					? system.RequestRoute(objects, objects.getUUID(positiveShardOwner), positiveShardDestination, NavigationProfile::Defaults())
					: NavigationSystem::ObjectRouteReceipt{};
				const auto singleSeamReceipt = root != nullptr && submission.accepted
					? system.RequestRoute(objects, objects.getUUID(singleSeamOwner), singleSeamDestination, NavigationProfile::Defaults())
					: NavigationSystem::ObjectRouteReceipt{};
				for(std::uint32_t frame = 0U; frame < 16U; ++frame) system.Update(0.0F);
				const auto request = system.ReadRequest(receipt.request);
				const auto negativeShardRequest = system.ReadRequest(negativeShardReceipt.request);
				const auto positiveShardRequest = system.ReadRequest(positiveShardReceipt.request);
				const auto singleSeamRequest = system.ReadRequest(singleSeamReceipt.request);
				const bool bothShardSidesPublished = root != nullptr
					&& std::any_of(root->coverage.begin(), root->coverage.end(), [cell](const NavigationCoverageRecord& coverage) {
						return coverage.key.shardX == cell * 256LL - 1LL && coverage.key.shardZ == cell * 256LL - 1LL
							&& coverage.state == NavigationCoverageState::Ready && coverage.complete;
					})
					&& std::any_of(root->coverage.begin(), root->coverage.end(), [cell](const NavigationCoverageRecord& coverage) {
						return coverage.key.shardX == cell * 256LL && coverage.key.shardZ == cell * 256LL
							&& coverage.state == NavigationCoverageState::Ready && coverage.complete;
					});

				context.Expect(terrain != nullptr && terrainPlaced && ownerPlaced && negativeShardOwnerPlaced
					&& positiveShardOwnerPlaced && singleSeamOwnerPlaced && capture.accepted
					&& submission.accepted && root != nullptr && root->terrainState == NavigationTerrainState::Available
					&& bothShardSidesPublished,
					"A placed terrain capture must publish complete coverage on both sides of its canonical shard seam.");
				// Preserve enough immutable provider evidence in a failure message to
				// distinguish endpoint support failures from a missing cross-shard portal.
				// The test never touches a published shard; it only counts its exposed
				// read-only boundary certificates after route preparation completes.
				const auto shards = system.PublishedGroundShards();
				std::size_t boundarySpanCount = 0U;
				for(const auto& shard : shards) {
					if(shard != nullptr) boundarySpanCount += CollectDetourGroundBoundarySpans(*shard).size();
				}
				const auto describe = [](std::string_view label,
					const NavigationSystem::ObjectRouteReceipt& routeReceipt,
					const NavigationStoreReadResult<NavigationRequestRecord>& routeRequest) {
					const NavigationOutcome outcome = routeRequest.record
						? routeRequest.record->outcome
						: NavigationOutcome::Failed;
					const NavigationReason reason = routeRequest.record
						? routeRequest.record->reason
						: NavigationReason::ProviderFailure;
					return std::string(label) + "=" + (routeReceipt.Succeeded() ? "admitted/" : "rejected/")
						+ std::string(ToString(outcome)) + "/" + std::string(ToString(reason));
				};
				const auto completed = [](const NavigationSystem::ObjectRouteReceipt& routeReceipt,
					const NavigationStoreReadResult<NavigationRequestRecord>& routeRequest) {
					return routeReceipt.Succeeded() && routeRequest.record.has_value()
						&& routeRequest.record->outcome == NavigationOutcome::Complete
						&& routeRequest.record->reason == NavigationReason::None;
				};
				const std::string seamFailure = "Cell " + std::to_string(cell) + ", elevation " + std::to_string(elevation)
					+ ": Placed terrain routes must retain endpoint support and cross its published seam "
					"(diagonal " + describe("-62,-62->62,62", receipt, request)
					+ ", negative " + describe("-62,-62->-2,-2", negativeShardReceipt, negativeShardRequest)
					+ ", positive " + describe("2,2->62,62", positiveShardReceipt, positiveShardRequest)
					+ ", single-seam " + describe("-2,2->2,2", singleSeamReceipt, singleSeamRequest)
					+ ", publishedShards=" + std::to_string(shards.size())
					+ ", boundarySpans=" + std::to_string(boundarySpanCount) + ").";
				context.Expect(completed(receipt, request)
					&& completed(negativeShardReceipt, negativeShardRequest)
					&& completed(positiveShardReceipt, positiveShardRequest)
					&& completed(singleSeamReceipt, singleSeamRequest),
					seamFailure);
				FinishSystem(system, objects);
				}
			}
		});

		registry.Add("Navigation.ObjectIntegration.ReplacementCancellationAndInvalidEndpointsStayIndependent", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			const auto world = system.PublishedWorld();
			const Object source = objects.createObject("Replacement Source");
			(void)objects.AddComponent<Transform>(source);
			const bool prepared = PrepareTerrainWorld(system);
			const TerraEngine::UUID sourceId = objects.getUUID(source);
			const auto first = world && prepared ? system.RequestRoute(objects, sourceId,
				Location(world->world, 2.0), NavigationProfile::Defaults(), NavigationRoutePolicy::ExactDestination, 44U)
				: NavigationSystem::ObjectRouteReceipt{};
			const auto replacement = world && prepared ? system.RequestRoute(objects, sourceId,
				Location(world->world, 6.0), NavigationProfile::Defaults(), NavigationRoutePolicy::ExactDestination, 44U)
				: NavigationSystem::ObjectRouteReceipt{};
			const auto cancelled = system.ReadRequest(first.request);
			const bool cancelIdempotent = system.CancelRequest(replacement.request) && system.CancelRequest(replacement.request);
			objects.removeObject(source);
			const auto invalid = world ? system.RequestRoute(objects, sourceId,
				Location(world->world, 3.0), NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};

			context.Expect(first.Succeeded() && replacement.Succeeded() && cancelled.record.has_value()
				&& cancelled.record->outcome == NavigationOutcome::Cancelled,
				"A replacement key must supersede only its prior request and preserve an explicit cancelled result.");
			context.Expect(cancelIdempotent,
				"Cancelling a component-free request must be idempotent for its generation-qualified handle.");
			context.Expect(invalid.outcome == NavigationOutcome::InvalidEndpoint
				&& invalid.reason == NavigationReason::InvalidStart,
				"A removed source object must fail as InvalidEndpoint rather than borrowing a recycled object identity.");
			FinishSystem(system, objects);
		});
}
}
