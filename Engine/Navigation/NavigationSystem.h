// NavigationSystem.h - scheduler-owned ground-navigation lifecycle shell.

#pragma once

#include "Core/SystemBase.h"
#include "Navigation/Backend/DetourGroundShard.h"
#include "Navigation/Dynamic/NavigationDirtyIndex.h"
#include "Navigation/Dynamic/NavigationRebuildPipeline.h"
#include "Navigation/Ground/GroundRouteQuery.h"
#include "Navigation/Integration/NavigationProfileResolver.h"
#include "Navigation/NavigationDiagnostics.h"
#include "Navigation/NavigationRouteValidity.h"
#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationStores.h"
#include "Navigation/NavigationWorld.h"
#include "Scene/SceneMutationQueue.h"
#include "Utilities/UUID.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

class ObjectManager;

namespace TerraEngine::Navigation {
	enum class NavigationProtectedBoundary : std::uint8_t {
		Scene,
		Spatial,
		PhysicsSubstep,
		Scripting,
		FrameEnd
	};

	class TERRAENGINE_API NavigationSystem final : public SystemsBase {
	public:
		// A protected scene capture resolves profiles and geometry before filling
		// this packet. Worker preparation receives only these owned snapshots.
		struct InitialGroundBuildInput {
			NavigationProfile profile{};
			std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
			std::vector<NavigationSourceIndexRecord> sourceIndex;
			// Automatic capture retains this complete immutable index and the terrain
			// publication that made it eligible. Worker builds never reconstruct either
			// value from live scene, resource, or streaming state.
			std::shared_ptr<const NavigationEligibleSourceIndex> eligibleSourceIndex;
			NavigationGeneration terrainPublicationGeneration = 0U;
			NavigationGeneration profileConfigurationRevision = 0U;
		};
		// Admission is explicit because malformed object/profile input must not
		// overwrite an earlier caller intent or silently create a partial request.
		struct ObjectRouteReceipt {
			NavigationRequestHandle request{};
			NavigationHandleStatus status = NavigationHandleStatus::Invalid;
			NavigationOutcome outcome = NavigationOutcome::InvalidRequest;
			NavigationReason reason = NavigationReason::None;

			[[nodiscard]] constexpr bool Succeeded() const noexcept {
				return status == NavigationHandleStatus::Alive && request.IsValid();
			}
		};
		struct NavigationInvalidation {
			std::uint64_t cursor = 0U;
			NavigationOwnerHandle owner{};
			NavigationRouteHandle route{};
			NavigationReason reason = NavigationReason::None;
			NavigationOutcome outcome = NavigationOutcome::Queued;
			std::uint64_t sourceRevision = 0U;
			std::uint64_t observationGeneration = 0U;
		};
		struct NavigationInvalidationRead {
			NavigationHandleStatus status = NavigationHandleStatus::Invalid;
			std::uint64_t newestCursor = 0U;
			bool overflowed = false;
			std::vector<NavigationInvalidation> records;
		};
		struct RouteValidation {
			bool usable = false;
			NavigationOutcome outcome = NavigationOutcome::Stale;
			NavigationReason reason = NavigationReason::SceneReplaced;
		};

		static NavigationSystem& Instance();

		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

		[[nodiscard]] std::shared_ptr<const NavigationWorldRoot> PublishedWorld() const;
		// These provider-neutral entry points copy all inputs before returning.
		// Scheduler worker preparation never retains a live scene/component pointer.
		[[nodiscard]] bool QueueInitialGroundBuild(
			NavigationProfile profile,
			std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources);
		// The scene/bootstrap bridge publishes only immutable copied geometry here.
		// A dynamic worker reads this retained vector, never an authoring object.
		[[nodiscard]] bool StageDynamicGroundSources(
			NavigationProfile profile,
			std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources);
		// Admits one complete per-profile bootstrap batch atomically. Coverage from
		// each resulting build is merged by its profile-qualified shard key, so a
		// second character profile never erases the first profile's navigation.
		[[nodiscard]] bool QueueInitialGroundBuilds(std::vector<InitialGroundBuildInput> builds);
		// Terrain is mandatory. A rejected/missing terrain capture publishes one
		// unavailable root so retained routes cannot continue over stale support.
		[[nodiscard]] bool PublishUnavailableTerrain(
			std::shared_ptr<const NavigationEligibleSourceIndex> eligibleSourceIndex);
		[[nodiscard]] NavigationRequestHandle SubmitOneShotRoute(
			Ground::GroundRouteQuery::Request request);
		// These protected-boundary entry points make every active object eligible
		// to request navigation. They copy identity and pose values before any
		// worker runs; navigation never stores a component on either object.
		[[nodiscard]] ObjectRouteReceipt RequestRoute(
			ObjectManager& objects, const TerraEngine::UUID& sourceObject,
			NavigationLocation destination, NavigationProfile profile,
			NavigationRoutePolicy policy = NavigationRoutePolicy::ExactDestination,
			std::uint64_t replacementKey = 0U);
		[[nodiscard]] ObjectRouteReceipt RequestRoute(
			ObjectManager& objects, const TerraEngine::UUID& sourceObject,
			const TerraEngine::UUID& targetObject, NavigationProfile profile,
			NavigationRoutePolicy policy = NavigationRoutePolicy::ExactDestination,
			std::uint64_t replacementKey = 0U);
		[[nodiscard]] NavigationStoreReadResult<NavigationRequestRecord> ReadRequest(
			NavigationRequestHandle handle) const;
		[[nodiscard]] NavigationRouteHandle PublishedRouteForRequest(
			NavigationRequestHandle handle) const;
		[[nodiscard]] Ground::GroundRouteQuery::RetainedRoute ReadRoute(
			NavigationRouteHandle handle) const;
		// Gameplay validates a retained route immediately before consuming a
		// waypoint. This fails closed when a newer immutable world supersedes it.
		[[nodiscard]] RouteValidation ValidateRoute(NavigationRouteHandle handle) const;
		[[nodiscard]] bool CancelRequest(NavigationRequestHandle handle);
		// Explicit result release reclaims both the request record and its copied
		// route atomically. Recycled slots advance their generation, so a historic
		// managed/native handle can never alias the next admitted result.
		[[nodiscard]] bool ReleaseRequestResult(NavigationRequestHandle handle);
		// Project/scene loading supplies a copied immutable registry. Replacing it
		// is protected by the navigation mutex and affects the next scene scan.
		void ConfigureProfileRegistrations(
			NavigationProfile defaultProfile,
			std::vector<NavigationProfileRegistration> registrations);
		// The protected scene-capture bridge takes this value copy when deciding
		// which profile-specific ground shards to prepare. Returning the resolver
		// rather than a reference keeps editor/project configuration out of worker
		// packets and lets a later configuration change request a new bootstrap.
		[[nodiscard]] NavigationProfileResolver ProfileResolverSnapshot() const;
		[[nodiscard]] std::uint64_t ProfileConfigurationRevision() const;
		[[nodiscard]] NavigationInvalidationRead ReadInvalidations(
			NavigationOwnerHandle owner, std::uint64_t afterCursor) const;
		// Project, scene, script-domain, and Play Mode lifetimes revoke all copied
		// work in one protected operation. Old handles cannot address the new world.
		void RevokeAllOwners(NavigationReason reason);
		[[nodiscard]] std::vector<std::shared_ptr<const DetourGroundShardGeneration>> PublishedGroundShards() const;
		// Scene flushes call this while they still own the protected observation
		// boundary. Accepted receipts are copied into the dirty index and their
		// tombstones become visible before any later route-derived movement.
		[[nodiscard]] std::size_t ObserveSceneMutationReceipts(
			std::span<const Scene::NavigationSourceMutationReceipt> receipts);
		[[nodiscard]] const NavigationDirtyIndex& DynamicDirtyIndexForTesting() const noexcept;
		void ObserveProtectedBoundary(NavigationProtectedBoundary boundary, std::uint64_t publicationGeneration);
		[[nodiscard]] std::shared_ptr<const NavigationDiagnosticSnapshot> PublishedDiagnostics() const;
		// This narrow hook lets scheduler contract tests represent a bounded unit of
		// work without constructing live scene objects or provider state.
		void SetContractTestWorkAvailable(bool available) noexcept;

	private:
		NavigationSystem() = default;
		void PrepareAvailableWork();
		void PublishPreparedWork();
		[[nodiscard]] std::optional<NavigationObjectEndpoint> CaptureObjectEndpointLocked(
			ObjectManager& objects, const TerraEngine::UUID& objectId) const;
		[[nodiscard]] ObjectRouteReceipt SubmitCapturedObjectRouteLocked(
			const NavigationObjectEndpoint& source, const NavigationObjectEndpoint* target,
			NavigationLocation destination, NavigationProfile profile,
			NavigationRoutePolicy policy, std::uint64_t replacementKey);
		[[nodiscard]] Ground::GroundRouteQuery::Result SearchPublishedGroundRoute(
			const Ground::GroundRouteQuery::Request& request,
			const std::shared_ptr<const NavigationWorldRoot>& worldRoot,
			const std::vector<std::shared_ptr<const DetourGroundShardGeneration>>& shards,
			const Ground::GroundPortalGraphSnapshot& portals) const;
		[[nodiscard]] bool HasReadyCoverageForProfileLocked(const NavigationProfile& profile) const;
		[[nodiscard]] bool HasProfileBuildCapacityLocked(const NavigationProfile& profile) const;
		[[nodiscard]] bool QueueOnDemandProfileBuildLocked(const NavigationProfile& profile);
		void QueueRouteAgainstPublishedWorldLocked(NavigationRequestHandle handle,
			std::uint64_t intentGeneration, Ground::GroundRouteQuery::Request request);
		void PromoteDeferredRoutesForProfileLocked(const NavigationProfile& profile);
		void ResolveDeferredProfileBuildFailureLocked(const NavigationProfile& profile,
			bool mayRequeueFromNewCapture);

		struct PendingGroundBuild {
			NavigationProfile profile{};
			std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
			std::vector<NavigationSourceIndexRecord> sourceIndex;
			std::shared_ptr<const NavigationEligibleSourceIndex> eligibleSourceIndex;
			NavigationGeneration terrainPublicationGeneration = 0U;
			NavigationGeneration profileConfigurationRevision = 0U;
		};
		struct PreparedGroundBuild {
			bool accepted = false;
			NavigationProfile profile{};
			std::vector<std::shared_ptr<const DetourGroundShardGeneration>> shards;
			std::vector<NavigationCoverageRecord> coverage;
			std::vector<NavigationSourceIndexRecord> sourceIndex;
			std::shared_ptr<const NavigationEligibleSourceIndex> eligibleSourceIndex;
			NavigationGeneration terrainPublicationGeneration = 0U;
			NavigationGeneration profileConfigurationRevision = 0U;
		};
		struct PendingDynamicRebuild {
			// The request is a full copied worker packet: no field may point into ECS
			// storage, a mutable NavigationWorldRoot, or a provider-owned buffer.
			NavigationDynamicSourceChange change{};
			NavigationDynamicRebuildRequest request{};
		};
		struct PreparedDynamicRebuild {
			NavigationDynamicSourceChange change{};
			NavigationDynamicRebuildPlan plan{};
		};
		struct DynamicSourceStage {
			NavigationProfile profile{};
			std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
		};
		struct AutomaticGroundSourceStage {
			// This is the last complete protected capture. It owns only immutable
			// geometry and copied evidence, so arbitrary caller profiles can request
			// coverage without consulting ECS, physics, or resource state on a worker.
			std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
			std::vector<NavigationSourceIndexRecord> sourceIndex;
			std::shared_ptr<const NavigationEligibleSourceIndex> eligibleSourceIndex;
			NavigationGeneration terrainPublicationGeneration = 0U;
			NavigationGeneration profileConfigurationRevision = 0U;
		};
		struct PendingRoute {
			NavigationRequestHandle handle{};
			std::uint64_t intentGeneration = 0U;
			Ground::GroundRouteQuery::Request request{};
			// Capture published generations at admission; worker code receives no
			// live scene, mutable root, or provider-owned address.
			std::shared_ptr<const NavigationWorldRoot> worldRoot;
			std::vector<std::shared_ptr<const DetourGroundShardGeneration>> shards;
			Ground::GroundPortalGraphSnapshot portals;
		};
		struct DeferredRoute {
			NavigationRequestHandle handle{};
			std::uint64_t intentGeneration = 0U;
			Ground::GroundRouteQuery::Request request{};
		};
		struct PreparedRoute {
			NavigationRequestHandle handle{};
			std::uint64_t intentGeneration = 0U;
			Ground::GroundRouteQuery::Result result{};
			NavigationWorldHandle world{};
			NavigationGeneration rootGeneration = 0U;
		};
		mutable std::mutex mutex_;
		std::unique_ptr<NavigationWorld> world_;
		std::unique_ptr<NavigationRequestStore> requestStore_;
		std::unique_ptr<NavigationRouteStore> routeStore_;
		// The portal graph contains only boundary spans extracted from published
		// Detour generations. It is not a synthetic neighboring-shard shortcut.
		Ground::GroundPortalGraph portalGraph_;
		NavigationDirtyIndex dirtyIndex_{NavigationDirtyIndexSettings{
			.clearanceExpansionMetres = 0.5F,
			.buildHaloMetres = 0.25F}};
		NavigationDiagnostics diagnostics_{};
		NavigationDynamicDiagnostics dynamicDiagnostics_{};
		std::uint64_t lastProtectedBoundaryGeneration_ = 0U;
		std::deque<PendingGroundBuild> pendingGroundBuilds_;
		std::deque<PreparedGroundBuild> preparedGroundBuilds_;
		std::deque<NavigationDynamicSourceChange> pendingDynamicChanges_;
		std::deque<PendingDynamicRebuild> pendingDynamicRebuilds_;
		std::deque<PreparedDynamicRebuild> preparedDynamicRebuilds_;
		// Immutable geometry staged by bootstrap/source integration. Copying shared
		// pointers into a worker packet is safe because snapshots never mutate.
		std::vector<DynamicSourceStage> dynamicSourceStages_;
		std::optional<AutomaticGroundSourceStage> automaticGroundSourceStage_;
		std::deque<PendingRoute> pendingRoutes_;
		std::deque<DeferredRoute> deferredRoutes_;
		std::deque<PreparedRoute> preparedRoutes_;
		struct ReplacementIntent {
			NavigationOwnerHandle owner{};
			std::uint64_t key = 0U;
			NavigationRequestHandle request{};
		};
		// Replacement keys are caller-owned intent tokens, not state attached to
		// an object. The bounded list is pruned with request release/revocation.
		std::vector<ReplacementIntent> replacementIntents_;
		NavigationProfileResolver profileResolver_ = NavigationProfileResolver::Create(
			NavigationProfile::Defaults(), {});
		// This protected token changes whenever project/editor profile configuration
		// is replaced. MainEngine combines it with component revision so new profiles
		// receive terrain/mesh coverage even when scene objects were not edited.
		std::uint64_t profileConfigurationRevision_ = 1U;
		// The latest automatic index revision is capture-wide evidence for scene,
		// transform/spatial, terrain, resource, streaming, and procedural inputs.
		// Publication rejects a prepared older index as one atomic stale candidate.
		NavigationGeneration latestAutomaticSourceIndexRevision_ = 0U;
		std::vector<std::shared_ptr<const DetourGroundShardGeneration>> publishedShards_;
		std::vector<Ground::GroundRouteQuery::RetainedRoute> publishedRouteData_;
		std::vector<NavigationRouteHandle> requestRoutes_;
		std::vector<NavigationInvalidation> invalidations_;
		std::uint64_t nextInvalidationCursor_ = 1U;
		NavigationSettings settings_ = NavigationSettings::Defaults();
		std::atomic_bool initialized_{false};
		std::atomic_bool contractTestWorkAvailable_{false};
	};
}
