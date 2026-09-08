// NavigationSystem.h - scheduler-owned ground-navigation lifecycle shell.

#pragma once

#include "Core/SystemBase.h"
#include "Navigation/Backend/DetourGroundShard.h"
#include "Navigation/Dynamic/NavigationDirtyIndex.h"
#include "Navigation/Dynamic/NavigationRebuildPipeline.h"
#include "Navigation/Ground/GroundRouteQuery.h"
#include "Navigation/Ground/GroundTraversalConnectionStore.h"
#include "Navigation/Integration/NavigationProfileResolver.h"
#include "Navigation/NavigationDiagnostics.h"
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
		};
		struct DestinationCommandReceipt {
			NavigationCommandHandle command{};
			NavigationHandleStatus status = NavigationHandleStatus::Invalid;
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
		[[nodiscard]] NavigationRequestHandle SubmitOneShotRoute(
			Ground::GroundRouteQuery::Request request);
		[[nodiscard]] NavigationStoreReadResult<NavigationRequestRecord> ReadRequest(
			NavigationRequestHandle handle) const;
		[[nodiscard]] NavigationRouteHandle PublishedRouteForRequest(
			NavigationRequestHandle handle) const;
		[[nodiscard]] Ground::GroundRouteQuery::RetainedRoute ReadRoute(
			NavigationRouteHandle handle) const;
		[[nodiscard]] bool CancelRequest(NavigationRequestHandle handle);
		// Explicit result release reclaims both the request record and its copied
		// route atomically. Recycled slots advance their generation, so a historic
		// managed/native handle can never alias the next admitted result.
		[[nodiscard]] bool ReleaseRequestResult(NavigationRequestHandle handle);
		// Agent registration belongs to a scene/component integration, but keeping
		// this copied entry point typed lets that integration publish one stable
		// owner handle before managed destination commands arrive.
		[[nodiscard]] NavigationAgentHandle RegisterAgent(
			NavigationOwnerHandle owner, NavigationLocation currentPosition, NavigationProfile profile);
		[[nodiscard]] DestinationCommandReceipt QueueSetDestination(
			NavigationAgentHandle agent, NavigationLocation destination, std::uint32_t routePolicy,
			std::uint64_t expectedIntentGeneration);
		[[nodiscard]] NavigationStoreReadResult<NavigationCommandRecord> ReadCommand(
			NavigationCommandHandle handle) const;
		// Object bindings are refreshed only at protected scene boundaries. They
		// expose copied runtime state without ever giving gameplay a live ECS
		// component pointer.
		[[nodiscard]] NavigationStoreReadResult<NavigationAgentRecord> ReadAgent(
			NavigationAgentHandle handle) const;
		[[nodiscard]] NavigationAgentHandle ResolveObjectAgent(
			const TerraEngine::UUID& objectId) const;
		[[nodiscard]] bool ReleaseAgent(NavigationAgentHandle handle);
		// MainEngine calls this only after a protected scene/physics/scripting
		// publication. It discovers active authoring components, captures their
		// canonical Transform poses, and revokes bindings no longer in the scene.
		void SynchronizeSceneObjectAgents(ObjectManager& objects);
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
		[[nodiscard]] Ground::GroundTraversalConnectionUpsertResult PublishConnection(
			NavigationOwnerHandle owner,
			const NavigationConnectionComponent& component,
			Ground::GroundTraversalConnectionStore::SupportCertificate certificate);
		[[nodiscard]] Ground::GroundTraversalConnectionSnapshot PublishedConnections() const;
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
		[[nodiscard]] NavigationAgentHandle RegisterAgentLocked(
			NavigationOwnerHandle owner, NavigationLocation currentPosition, NavigationProfile profile);
		[[nodiscard]] bool ReleaseAgentLocked(NavigationAgentHandle handle, NavigationReason reason);
		void SynchronizeSceneObjectAgentsLocked(ObjectManager& objects);
		[[nodiscard]] Ground::GroundRouteQuery::Result SearchPublishedGroundRoute(
			const Ground::GroundRouteQuery::Request& request,
			const std::shared_ptr<const NavigationWorldRoot>& worldRoot,
			const std::vector<std::shared_ptr<const DetourGroundShardGeneration>>& shards,
			const Ground::GroundPortalGraphSnapshot& portals) const;

		struct PendingGroundBuild {
			NavigationProfile profile{};
			std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
			std::vector<NavigationSourceIndexRecord> sourceIndex;
		};
		struct PreparedGroundBuild {
			bool accepted = false;
			std::vector<std::shared_ptr<const DetourGroundShardGeneration>> shards;
			std::vector<NavigationCoverageRecord> coverage;
			std::vector<NavigationSourceIndexRecord> sourceIndex;
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
		struct PendingRoute {
			NavigationRequestHandle handle{};
			// One-shot requests have no agent. Agent-owned requests retain the exact
			// pose and intent generations observed when their search started.
			NavigationAgentHandle agent{};
			std::uint64_t intentGeneration = 0U;
			std::uint64_t poseGeneration = 0U;
			Ground::GroundRouteQuery::Request request{};
			// Capture published generations at admission; worker code receives no
			// live scene, mutable root, or provider-owned address.
			std::shared_ptr<const NavigationWorldRoot> worldRoot;
			std::vector<std::shared_ptr<const DetourGroundShardGeneration>> shards;
			Ground::GroundPortalGraphSnapshot portals;
		};
		struct PreparedRoute {
			NavigationRequestHandle handle{};
			NavigationAgentHandle agent{};
			std::uint64_t intentGeneration = 0U;
			std::uint64_t poseGeneration = 0U;
			Ground::GroundRouteQuery::Result result{};
			NavigationWorldHandle world{};
			NavigationGeneration rootGeneration = 0U;
		};
		struct PendingDestination {
			NavigationCommandHandle command{};
			NavigationAgentHandle agent{};
			NavigationLocation destination{};
			std::uint32_t routePolicy = 0U;
			std::uint64_t expectedIntentGeneration = 0U;
		};

		mutable std::mutex mutex_;
		std::unique_ptr<NavigationWorld> world_;
		std::unique_ptr<NavigationRequestStore> requestStore_;
		std::unique_ptr<NavigationRouteStore> routeStore_;
		std::unique_ptr<NavigationAgentStore> agentStore_;
		std::unique_ptr<NavigationCommandStore> commandStore_;
		Ground::GroundTraversalConnectionStore connectionStore_;
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
		std::deque<PendingRoute> pendingRoutes_;
		std::deque<PreparedRoute> preparedRoutes_;
		std::deque<PendingDestination> pendingDestinations_;
		struct ObjectAgentBinding {
			NavigationAgentHandle agent{};
			std::uint64_t objectLifetimeGeneration = 0U;
			std::uint64_t profileId = 0U;
			NavigationGeneration profileRevision = 0U;
		};
		// UUID avoids aliasing a recycled EnTT entity. The associated runtime
		// handle independently carries scene/world/slot generations.
		std::unordered_map<TerraEngine::UUID, ObjectAgentBinding> objectAgents_;
		NavigationProfileResolver profileResolver_ = NavigationProfileResolver::Create(
			NavigationProfile::Defaults(), {});
		// This protected token changes whenever project/editor profile configuration
		// is replaced. MainEngine combines it with component revision so new profiles
		// receive terrain/mesh coverage even when scene objects were not edited.
		std::uint64_t profileConfigurationRevision_ = 1U;
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
