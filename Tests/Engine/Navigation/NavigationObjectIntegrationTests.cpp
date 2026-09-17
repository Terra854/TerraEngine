// NavigationObjectIntegrationTests.cpp - component-free object route contracts.

#include "../TestHarness.h"

#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Navigation;

	namespace {
		NavigationLocation Location(const NavigationWorldHandle world, double x) {
			NavigationLocation location{};
			location.world = world;
			location.position.offsetMetres = {x, 0.0, 0.0};
			return location;
		}

		std::shared_ptr<const NavigationGeometrySnapshot> FlatTerrain() {
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{-16.0F, 0.0F, -16.0F}, {16.0F, 0.0F, 16.0F},
				{16.0F, 0.0F, -16.0F}, {-16.0F, 0.0F, 16.0F}};
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
