// NavigationSystem.h - scheduler-owned ground-navigation lifecycle shell.

#pragma once

#include "Core/SystemBase.h"
#include "Navigation/Backend/DetourGroundShard.h"
#include "Navigation/Dynamic/NavigationDirtyIndex.h"
#include "Navigation/Dynamic/NavigationRebuildPipeline.h"
#include "Navigation/Ground/GroundRouteQuery.h"
#include "Navigation/Integration/NavigationProfileResolver.h"
#include "Navigation/NavigationDiagnostics.h"
#include "Navigation/NavigationAsyncPreparation.h"
#include "Navigation/NavigationRouteValidity.h"
#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationStores.h"
#include "Navigation/NavigationWorld.h"
#include "Navigation/Persistence/SavedNavigationCache.h"
#include "Scene/SceneMutationQueue.h"
#include "Utilities/UUID.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
		// A bounded value-only view of work still owned by NavigationSystem. Test
		// tooling uses this after a normal lifecycle drain to prove that cancelling
		// a caller's handles also reclaimed the underlying request and route slots.
		// It deliberately exposes counts only: callers cannot access store records.
		struct StoreUsageSnapshot {
			std::size_t activeRequests = 0U;
			std::size_t activeRoutes = 0U;
			std::size_t pendingRoutes = 0U;
			// A protected capture owns this bounded batch until publication. If a
			// worker frame is cancelled, the next capture returns it to pendingRoutes.
			std::size_t inFlightRoutes = 0U;
			std::size_t deferredRoutes = 0U;
			std::size_t preparedRoutes = 0U;
			// These counters expose the latest bounded route fan-out without exposing
			// mutable request packets to diagnostics or tooling.
			std::size_t lastRoutePreparationBatch = 0U;
			std::size_t routePreparationBatchHighWater = 0U;
			std::size_t replacementIntents = 0U;
			std::size_t agentRequestHandles = 0U;
			// Each entry retains a profile plus shared immutable geometry pointers for
			// later dirty-source rebuilds. Expose only its bounded count so tests and
			// diagnostics can prove failed on-demand profiles do not accumulate.
			std::size_t dynamicSourceStages = 0U;
			// This is an estimate of storage owned by the current system, not an
			// allocator-wide measurement or retained external shared snapshots.
			std::size_t estimatedOwnedBytes = 0U;
		};
		// A mounted project/archive supplies copied cache bytes with the profile that
		// authored them. The logical path is diagnostic provenance only: import never
		// opens a path after this protected handoff, which keeps export archives and
		// editor files on the same immutable-byte contract.
		struct SavedNavigationCacheCandidate {
			std::vector<std::byte> bytes;
			std::string logicalPath;
			// Archive mounting need not guess a project profile. When absent, worker
			// validation tries the authoritative copied profile registry and accepts
			// only the one whose fresh settings fingerprint matches the candidate.
			std::optional<NavigationProfile> profile;
		};

		// Candidate admission says only that bounded bytes were retained for a later
		// authoritative comparison. It does not claim that cached tiles were trusted
		// or visible; callers read SavedNavigationCacheStatus after protected publish.
		struct SavedNavigationCacheReceipt {
			bool accepted = false;
			bool deferredUntilAuthoritativeCapture = false;
			std::string reason;
		};

		// Export contains only the current complete pristine tile coverage that the
		// system itself published. It is a value copy suitable for an editor file or
		// mounted archive writer and never leaks a provider-owned shard pointer.
		struct SavedNavigationCacheSnapshot {
			SavedNavigationDescriptor descriptor{};
			std::vector<SavedNavigationChunk> chunks;
		};

		// This bounded diagnostic separates an accepted cache hit from a fail-open
		// fresh build. A rejection reason remains visible even when the fallback
		// later publishes usable coverage, so tooling cannot mistake fallback for a hit.
		struct SavedNavigationCacheStatus {
			bool candidatePending = false;
			bool cacheUsed = false;
			std::size_t candidateBytes = 0U;
			std::string logicalPath;
			std::string rejectionReason;
		};

		static NavigationSystem& Instance();

		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		// Applies one complete navigation capacity configuration before Initialize
		// constructs bounded request and route stores. Runtime resizing would make
		// retained handles ambiguous, so initialized systems reject this operation.
		[[nodiscard]] bool ConfigureSettings(NavigationSettings settings);
		// Returns a value copy for diagnostics and validation. Callers cannot
		// mutate the live store capacity through this observation.
		[[nodiscard]] NavigationSettings SettingsSnapshot() const;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

		[[nodiscard]] std::shared_ptr<const NavigationWorldRoot> PublishedWorld() const;
		// Returns the latest accepted automatic-source rows as a value copy. During
		// asynchronous bootstrap they are newer than PublishedWorld()->sources, so
		// protected scene capture can compare live structure against accepted copied
		// evidence without repeatedly admitting the identical bootstrap packet.
		[[nodiscard]] std::vector<NavigationSourceIndexRecord> AutomaticSourceObservationSnapshot() const;
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
		struct DynamicGroundSourceReplacement {
			// A protected source bridge supplies one coherent source-index row beside
			// its immutable replacement snapshot. Keeping both values together avoids
			// publishing a new collider mesh against stale automatic-source evidence.
			NavigationSourceIndexRecord source{};
			std::shared_ptr<const NavigationGeometrySnapshot> snapshot;
			std::uint64_t committedChangeSequence = 0U;
		};
		// Replaces only named snapshots in every retained profile stage. This is a
		// protected value-copy operation: it never scans ObjectManager or asks a
		// worker to read mutable collider/component storage.
		[[nodiscard]] bool StageDynamicGroundSourceReplacements(
			std::vector<DynamicGroundSourceReplacement> replacements);
		// Admits one complete per-profile bootstrap batch atomically. Coverage from
		// each resulting build is merged by its profile-qualified shard key, so a
		// second character profile never erases the first profile's navigation.
		[[nodiscard]] bool QueueInitialGroundBuilds(std::vector<InitialGroundBuildInput> builds);
		// Terrain is mandatory. A rejected/missing terrain capture publishes one
		// unavailable root so retained routes cannot continue over stale support.
		// A committed source bridge may additionally request one later automatic
		// capture; ordinary missing-terrain publications keep this false.
		[[nodiscard]] bool PublishUnavailableTerrain(
			std::shared_ptr<const NavigationEligibleSourceIndex> eligibleSourceIndex,
			bool requestFreshAutomaticCapture = false);
		// Consumes the one-shot protected fallback request. Frame End alone calls
		// this before capture; returning false never asks a missing terrain scene to
		// rescan every frame.
		[[nodiscard]] bool ConsumeFreshAutomaticCaptureRequest();
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
		// Agent publication calls this protected bridge after it has copied and
		// revalidated its attachment identity. The bridge preserves direct-query
		// ABI behavior while reserving a bounded share of route capacity for agents.
		[[nodiscard]] ObjectRouteReceipt RequestAgentRoute(
			ObjectManager& objects, NavigationObjectIdentity source,
			NavigationLocation destination, NavigationProfile profile);
		[[nodiscard]] NavigationStoreReadResult<NavigationRequestRecord> ReadRequest(
			NavigationRequestHandle handle) const;
		[[nodiscard]] NavigationRouteHandle PublishedRouteForRequest(
			NavigationRequestHandle handle) const;
		[[nodiscard]] Ground::GroundRouteQuery::RetainedRoute ReadRoute(
			NavigationRouteHandle handle) const;
		// Returns a copied observation only while this exact retained route is still
		// usable against the currently published immutable world. Consumers that keep
		// a route across an unrelated source publication use this instead of comparing
		// a stale, previously copied world tuple themselves.
		[[nodiscard]] std::optional<NavigationObservationEvidence> ReadCurrentRouteObservation(
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
		[[nodiscard]] StoreUsageSnapshot PublishedStoreUsage() const;
		// Retains one mounted/archive cache candidate until a worker can compare it
		// against a complete copied bootstrap or automatic source packet. Invalid
		// bytes remain within the configured bound and cannot change published world.
		[[nodiscard]] SavedNavigationCacheReceipt QueueSavedGroundCacheImport(
			SavedNavigationCacheCandidate candidate);
		// Returns the current complete cacheable coverage for one exact profile. A
		// missing/incomplete/stale profile has no exportable snapshot rather than a
		// partial cache that another runtime might incorrectly trust.
		[[nodiscard]] std::optional<SavedNavigationCacheSnapshot>
		CapturePublishedGroundCache(const NavigationProfile& profile) const;
		[[nodiscard]] SavedNavigationCacheStatus SavedGroundCacheStatus() const;
		// This narrow hook lets scheduler contract tests represent a bounded unit of
		// work without constructing live scene objects or provider state.
		void SetContractTestWorkAvailable(bool available) noexcept;
		// Agent command capture publishes a conservative count before scheduler
		// declarations are assembled. This is liveness metadata only: protected
		// capture still moves only actual PendingRoute packets into worker batches.
		void SetPendingAgentRouteAdmissionDemand(std::size_t demand) noexcept;

	private:
		enum class RouteRequestClass : std::uint8_t {
			Direct,
			Agent
		};
		// A profile may already have usable coverage, be recoverable by an accepted
		// dynamic rebuild, or be unavailable. Keeping these cases distinct prevents a
		// request from either starting a duplicate full rebuild or waiting forever.
		enum class ProfileCoverageAdmission : std::uint8_t {
			Ready,
			PendingRecovery,
			Unavailable
		};

		NavigationSystem() = default;
		void PrepareAvailableWork(bool prepareRoutes = true);
		void PublishPreparedWork();
		[[nodiscard]] std::optional<NavigationObjectEndpoint> CaptureObjectEndpointLocked(
			ObjectManager& objects, const TerraEngine::UUID& objectId) const;
		[[nodiscard]] ObjectRouteReceipt SubmitCapturedObjectRouteLocked(
			const NavigationObjectEndpoint& source, const NavigationObjectEndpoint* target,
			NavigationLocation destination, NavigationProfile profile,
			NavigationRoutePolicy policy, std::uint64_t replacementKey,
			RouteRequestClass requestClass = RouteRequestClass::Direct);
		[[nodiscard]] bool CanAdmitAgentRouteLocked();
		[[nodiscard]] bool CanAdmitDirectRouteLocked();
		void PruneReleasedAgentRoutesLocked();
		[[nodiscard]] Ground::GroundRouteQuery::Result SearchPublishedGroundRoute(
			const Ground::GroundRouteQuery::Request& request,
			const std::shared_ptr<const NavigationWorldRoot>& worldRoot,
			const std::vector<std::shared_ptr<const DetourGroundShardGeneration>>& shards,
			const Ground::GroundPortalGraphSnapshot& portals) const;
		[[nodiscard]] bool HasReadyCoverageForProfileLocked(const NavigationProfile& profile) const;
		[[nodiscard]] bool HasProfileBuildCapacityLocked(const NavigationProfile& profile) const;
		// Builds a profile-aware minimum dirty envelope from immutable active profile
		// values. Missing values for published coverage fail closed at receipt admission
		// rather than allowing a larger Recast border to search stale neighbour tiles.
		[[nodiscard]] std::optional<NavigationDirtyAdmissionExpansion>
		ComputeDirtyAdmissionExpansionLocked(std::span<const NavigationProfile> provisionalProfiles) const;
		// Admission is endpoint-aware: a profile may have ready coverage elsewhere
		// while one requested canonical shard is rebuilding under a live receipt.
		[[nodiscard]] ProfileCoverageAdmission ClassifyProfileCoverageAdmissionLocked(
			const Ground::GroundRouteQuery::Request& request);
		[[nodiscard]] bool HasCurrentDynamicProfileRecoveryLocked(std::uint64_t profileBuildKey) const;
		void RecordUnavailableProfileRecoveryLocked(const NavigationProfile& profile);
		void QueueRouteAgainstPublishedWorldLocked(NavigationRequestHandle handle,
			std::uint64_t intentGeneration, Ground::GroundRouteQuery::Request request);
		// Captured source-index replacement is a protected two-stage operation. The
		// first stage retires only routes whose swept path overlaps changed copied
		// geometry; the second stage refreshes evidence for the unaffected routes
		// only after the replacement root commits.
		void BeginAutomaticSourceIndexReplacementLocked(
			std::shared_ptr<const NavigationEligibleSourceIndex> previous,
			std::shared_ptr<const NavigationEligibleSourceIndex> replacement,
			std::span<const std::uint64_t> requiredProfileBuildKeys);
		[[nodiscard]] bool RouteIsAffectedByAutomaticSourceReplacementLocked(
			const Ground::GroundRouteQuery::RetainedRoute& route,
			const NavigationProfile& profile) const;
		void RefreshUnaffectedRoutesAfterAutomaticSourcePublicationLocked(
			NavigationGeneration publishedSourceIndexRevision, std::uint64_t publishedProfileBuildKey);
		// Dynamic receipts mark intersecting retained paths terminal before their
		// dirty root is published. Every remaining complete or partial route is
		// therefore spatially independent and may observe the new immutable tuple.
		void RefreshUnaffectedRoutesAfterDynamicSourcePublicationLocked(
			const std::shared_ptr<const NavigationWorldRoot>& previousRoot);
		void PromoteDeferredRoutesForProfileLocked(const NavigationProfile& profile);
		void ResolveDeferredProfileBuildFailureLocked(const NavigationProfile& profile,
			bool mayRequeueFromNewCapture, NavigationOutcome terminalOutcome,
			NavigationReason terminalReason, std::string_view diagnosticDetail);
		// A failed on-demand profile has no authoritative coverage to retain. Remove
		// its copied dynamic stage once no build is still live, while preserving
		// stages that back completed coverage or a replacement build in progress.
		void ReleaseUnretainedDynamicSourceStageLocked(const NavigationProfile& profile);

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
			// Worker preparation maps a concrete Recast/Detour failure to the public
			// provider-neutral result here. The detail is published to diagnostics so
			// deferred callers do not mistake a provider/budget failure for a hole.
			NavigationOutcome failureOutcome = NavigationOutcome::UnavailableCoverage;
			NavigationReason failureReason = NavigationReason::MissingNeighbor;
			std::string failureDetail;
			// Saved coverage is assembled through the same worker and protected
			// publication path as fresh coverage. These value fields retain the
			// immutable proof needed for a later export without exposing shards.
			bool cacheCandidateAttempted = false;
			bool cacheImported = false;
			std::string cacheRejectionReason;
			GroundCoverageEvidence cacheEvidence{};
			std::vector<GroundTileBlob> pristineTiles;
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
			NavigationProfile profile{};
			NavigationDynamicRebuildPlan plan{};
		};
		// The result state is separate from the generic coordinator so the worker
		// task can hold only copied provider input while NavigationSystem retains the
		// strongly typed candidate for a later protected completion poll.
		struct AsyncBuildResultState {
			// Keeping the immutable packet through completion gives an exception path a
			// deterministic requeue without cloning large source/cache payloads.
			std::optional<PendingGroundBuild> groundPacket;
			std::optional<SavedNavigationCacheCandidate> savedCacheCandidate;
			std::optional<PendingDynamicRebuild> dynamicPacket;
			std::optional<PreparedGroundBuild> groundBuild;
			std::optional<PreparedDynamicRebuild> dynamicBuild;
		};
		struct AsyncBuildSubmission {
			NavigationAsyncBuildIdentity identity{};
			std::shared_ptr<AsyncBuildResultState> result;
		};
		struct ActiveDynamicBuildChild {
			NavigationAsyncBuildIdentity identity{};
			std::shared_ptr<AsyncBuildResultState> result;
		};
		struct ActiveDynamicBuildGroup {
			// Every child was captured from this exact immutable tuple. Provider work
			// may finish in any order, but protected publication combines only children
			// that still prove this one source/terrain/root observation.
			NavigationDynamicSourceChange change{};
			NavigationWorldHandle world{};
			NavigationGeneration expectedRootGeneration = 0U;
			NavigationGeneration expectedTerrainPublicationGeneration = 0U;
			NavigationGeneration expectedSourceIndexRevision = 0U;
			std::vector<std::uint64_t> requiredProfileBuildKeys;
			std::vector<PendingDynamicRebuild> pendingChildren;
			std::vector<ActiveDynamicBuildChild> activeChildren;
			std::vector<PreparedDynamicRebuild> successfulChildren;
			std::vector<PreparedDynamicRebuild> failedChildren;
		};
		struct PreparedDynamicBuildGroup {
			// A group becomes publishable only after every child completed. Successful
			// profile coverage can then exchange together in one root/portal tuple while
			// permanent sibling failures remain explicitly unavailable.
			NavigationDynamicSourceChange change{};
			NavigationWorldHandle world{};
			NavigationGeneration expectedRootGeneration = 0U;
			NavigationGeneration expectedTerrainPublicationGeneration = 0U;
			NavigationGeneration expectedSourceIndexRevision = 0U;
			std::vector<std::uint64_t> requiredProfileBuildKeys;
			std::vector<PreparedDynamicRebuild> successfulChildren;
			std::vector<PreparedDynamicRebuild> failedChildren;
		};
		struct DynamicSourceStage {
			NavigationProfile profile{};
			std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
		};
		struct DynamicRebuildProgress {
			// One dirty receipt can rebuild several registered profiles. Coverage and
			// dirty dependencies remain retained until every profile in this exact
			// change sequence publishes, so the first profile cannot acknowledge work
			// required by a later one.
			NavigationGeneration changeSequence = 0U;
			std::vector<std::uint64_t> requiredProfileBuildKeys;
			std::vector<std::uint64_t> publishedProfileBuildKeys;
			// A permanent provider failure ends recovery for its exact profile without
			// retrying the same immutable receipt forever. A later source receipt starts
			// fresh progress and may attempt the profile again.
			std::vector<std::uint64_t> terminalFailedProfileBuildKeys;
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
		struct PendingDynamicSourceStageReplacement {
			// A scene bridge prepares this complete immutable replacement before dirty
			// admission. It is not visible to workers or later profile requests until
			// every matching receipt commits and the protected root publication succeeds.
			std::vector<DynamicSourceStage> dynamicStages;
			std::optional<AutomaticGroundSourceStage> automaticStage;
			std::vector<DynamicGroundSourceReplacement> replacements;
		};
		struct AutomaticSourceIndexReplacement {
			// Every entry is immutable source-local geometry from either side of the
			// captured index transition. It is sufficient to conservatively compare a
			// retained route without looking at ECS, physics, or a provider buffer.
			std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> changedGeometry;
			// Every profile in this exact automatic capture must publish a matching
			// root-plus-portal tuple before any old route evidence can be refreshed.
			// This prevents a successful default profile from certifying an omitted or
			// failed alternate-profile route against new source geometry.
			std::vector<std::uint64_t> requiredProfileBuildKeys;
			std::vector<std::uint64_t> publishedProfileBuildKeys;
			NavigationGeneration replacementRevision = 0U;
			bool affectsEveryRoute = false;
		};
		struct PublishedGroundCacheCoverage {
			NavigationProfile profile{};
			GroundCoverageEvidence evidence{};
			std::vector<GroundTileBlob> tiles;
			NavigationGeneration sourceIndexRevision = 0U;
		};
		struct PendingRoute {
			// Protected admission assigns a non-zero sequence once. Worker partitions
			// may finish in any order; publication restores this deterministic order.
			std::uint64_t preparationSequence = 0U;
			// The system advances this value across Initialize/Revoke lifetimes. It
			// prevents a late worker from an old world from matching a reused sequence.
			std::uint64_t preparationEpoch = 0U;
			NavigationRequestHandle handle{};
			std::uint64_t intentGeneration = 0U;
			Ground::GroundRouteQuery::Request request{};
			// Monotonic protected-boundary stamps are copied with the route packet. They
			// do not expose a clock to workers and survive cancellation/requeue without
			// charging the same admission twice.
			std::chrono::steady_clock::time_point readyAdmissionTime{};
			bool captureTimingRecorded = false;
			std::chrono::steady_clock::time_point capturedTime{};
			// This is the exact immutable world tuple paired with worldRoot below.
			// Deferred-profile admission can capture a newer root than the request's
			// original admission snapshot, so publication must not reconstruct it from
			// the mutable request record.
			NavigationObservationEvidence observation{};
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
			std::chrono::steady_clock::time_point admittedTime{};
		};
		struct PreparedRoute {
			std::uint64_t preparationSequence = 0U;
			std::uint64_t preparationEpoch = 0U;
			NavigationRequestHandle handle{};
			std::uint64_t intentGeneration = 0U;
			// Preserve the protected packet's world evidence through worker execution.
			// The request record may still describe its earlier deferred admission.
			NavigationObservationEvidence observation{};
			std::chrono::steady_clock::time_point readyAdmissionTime{};
			std::chrono::steady_clock::time_point capturedTime{};
			std::chrono::steady_clock::time_point workerStartedTime{};
			std::chrono::steady_clock::time_point workerFinishedTime{};
			Ground::GroundRouteQuery::Result result{};
			NavigationWorldHandle world{};
			NavigationGeneration rootGeneration = 0U;
		};
		struct DynamicRouteAffectedBounds {
			// This is the protected receipt's copied, source-local swept range. It
			// never points at a scene object or mutable geometry provider.
			NavigationUuid source{};
			NavigationGeneration changeSequence = 0U;
			Scene::WorldPosition origin{};
			NavigationGeometryBounds previousBounds{};
			NavigationGeometryBounds currentBounds{};
			bool previousBoundsValid = false;
			bool currentBoundsValid = false;
		};
		struct DynamicRouteTransition {
			NavigationWorldHandle world{};
			NavigationGeneration fromRootGeneration = 0U;
			NavigationGeneration toRootGeneration = 0U;
			NavigationGeneration fromTerrainPublicationGeneration = 0U;
			NavigationGeneration toTerrainPublicationGeneration = 0U;
			NavigationGeneration fromSourceIndexRevision = 0U;
			NavigationGeneration toSourceIndexRevision = 0U;
			// An unavailable certificate is deliberately retained as a gap. A later
			// route can then fail closed instead of skipping an unknown publication.
			bool known = false;
			std::vector<DynamicRouteAffectedBounds> affectedBounds;
			// A normal profile addition can advance the world root without changing an
			// existing profile's coverage, shard generations, or same-profile portals.
			// Only those explicitly certified profile keys may cross an empty bounds link.
			std::vector<std::uint64_t> topologyPreservedProfileBuildKeys;
		};
		enum class DynamicRouteRebaseResult : std::uint8_t {
			Rebased,
			Intersected,
			Unknown
		};
		enum class DynamicRouteRebaseUnknownReason : std::uint8_t {
			None,
			InvalidInput,
			TerrainMismatch,
			MissingLink,
			InvalidLink,
			SourceChainMismatch,
			FinalSourceMismatch
		};
		void AppendDynamicRouteTransitionLocked(const NavigationWorldRoot& previous,
			const NavigationWorldRoot& published, std::vector<DynamicRouteAffectedBounds> affectedBounds,
			std::vector<std::uint64_t> topologyPreservedProfileBuildKeys = {});
		[[nodiscard]] std::vector<std::uint64_t> CollectTopologyPreservedProfilesLocked(
			const NavigationWorldRoot& previous, const NavigationWorldRoot& published,
			const std::vector<std::shared_ptr<const DetourGroundShardGeneration>>& previousShards,
			const std::vector<std::shared_ptr<const DetourGroundShardGeneration>>& nextShards) const;
		[[nodiscard]] std::vector<DynamicRouteAffectedBounds>
		FindDynamicRouteAffectedBoundsLocked(NavigationUuid source, NavigationGeneration changeSequence) const;
		[[nodiscard]] DynamicRouteRebaseResult TryRebasePreparedRouteObservationLocked(
			NavigationWorldHandle capturedWorld, NavigationGeneration capturedRootGeneration,
			const NavigationObservationEvidence& capturedObservation, const NavigationProfile& profile,
			const Ground::GroundRouteQuery::RetainedRoute& route,
			const NavigationWorldRoot& currentRoot, NavigationObservationEvidence& rebasedObservation,
			DynamicRouteRebaseUnknownReason& unknownReason) const;
		void RecordPreparedRouteTransitionUnknownLocked(DynamicRouteRebaseUnknownReason reason,
			const PreparedRoute& prepared, const NavigationWorldRoot& currentRoot);
		[[nodiscard]] static PreparedGroundBuild PrepareGroundBuildPacket(const PendingGroundBuild& groundBuild,
			const std::optional<SavedNavigationCacheCandidate>& savedCacheCandidate,
			const std::shared_ptr<NavigationBudget>& budget);
		[[nodiscard]] static PreparedDynamicRebuild PrepareDynamicBuildPacket(const PendingDynamicRebuild& dynamicBuild,
			const std::shared_ptr<NavigationBudget>& budget);
		[[nodiscard]] static std::size_t EstimateGroundBuildPacketBytes(const PendingGroundBuild& build,
			const std::optional<SavedNavigationCacheCandidate>& savedCacheCandidate) noexcept;
		[[nodiscard]] static std::size_t EstimateDynamicBuildPacketBytes(const PendingDynamicRebuild& build) noexcept;
		void DrainCompletedAsyncBuildsLocked();
		void RetireAsyncBuildsLocked();
		void PublishAsyncBuildDiagnosticsLocked();
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
		// At most two profile-specific dynamic children run together. This halves the
		// common two-profile recovery critical path while leaving the route scheduler's
		// eight worker lanes available for query preparation.
		// The coordinator also keeps retired packets charged across world lifetimes.
		NavigationAsyncBuildCoordinator asyncBuildCoordinator_{2U};
		std::shared_ptr<NavigationBudget> asyncBuildBudget_;
		std::uint64_t asyncBuildEpoch_ = 0U;
		// Normal ground/cache work remains singular. Dynamic recovery uses a bounded
		// explicit group so child completion never publishes a half-updated root.
		std::optional<AsyncBuildSubmission> activeAsyncBuild_;
		std::optional<ActiveDynamicBuildGroup> activeDynamicBuildGroup_;
		std::optional<PreparedDynamicBuildGroup> preparedDynamicBuildGroup_;
		std::uint64_t lastProtectedBoundaryGeneration_ = 0U;
		std::deque<PendingGroundBuild> pendingGroundBuilds_;
		std::deque<PreparedGroundBuild> preparedGroundBuilds_;
		// The current mounted candidate may wait for an authoritative startup
		// capture. Replacing, revoking, or shutting down the session drops its
		// copied bytes deterministically so it cannot cross a world lifetime.
		std::optional<SavedNavigationCacheCandidate> savedGroundCacheCandidate_;
		SavedNavigationCacheStatus savedGroundCacheStatus_{};
		// A malformed/stale candidate is attempted at most once per copied profile.
		// This lets an optional candidate wait for another registered profile without
		// repeatedly rebuilding or repeatedly decoding the same bytes every frame.
		std::unordered_set<std::uint64_t> savedGroundCacheAttemptedProfiles_;
		bool savedGroundCacheImportInFlight_ = false;
		std::deque<NavigationDynamicSourceChange> pendingDynamicChanges_;
		std::deque<PendingDynamicRebuild> pendingDynamicRebuilds_;
		std::deque<PreparedDynamicRebuild> preparedDynamicRebuilds_;
		// The journal remains smaller than the retired-root bound. It retains only
		// enough copied evidence to certify in-flight routes across local rebuilds.
		std::deque<DynamicRouteTransition> dynamicRouteTransitions_;
		std::optional<DynamicRebuildProgress> dynamicRebuildProgress_;
		// Immutable geometry staged by bootstrap/source integration. Copying shared
		// pointers into a worker packet is safe because snapshots never mutate.
		std::vector<DynamicSourceStage> dynamicSourceStages_;
		// Metadata is staged beside source pointers and consumed only when the same
		// committed receipt reaches ObserveSceneMutationReceipts. A new receipt can
		// therefore not pair a current geometry snapshot with an old index row.
		std::vector<NavigationSourceIndexRecord> stagedDynamicSourceReplacements_;
		std::optional<PendingDynamicSourceStageReplacement> pendingDynamicSourceStageReplacement_;
		std::optional<AutomaticGroundSourceStage> automaticGroundSourceStage_;
		std::optional<AutomaticSourceIndexReplacement> automaticSourceIndexReplacement_;
		// Set only when a committed source mutation could not be represented by the
		// protected bridge. It is consumed once by the normal Frame End capture host.
		bool freshAutomaticCaptureRequested_ = false;
		std::deque<PendingRoute> pendingRoutes_;
		// Route packets leave the admission queue only at a protected capture. They
		// stay here until protected publication consumes their sequence, allowing a
		// cancelled worker frame to restore the complete batch without losing work.
		std::deque<PendingRoute> inFlightRoutes_;
		std::deque<DeferredRoute> deferredRoutes_;
		std::deque<PreparedRoute> preparedRoutes_;
		// Atomic because the command system may publish its queue estimate before the
		// scheduler declaration thread observes it. Declaration code caps the value;
		// no route authority or request record is stored here.
		std::atomic<std::size_t> pendingAgentRouteAdmissionDemand_{0U};
		std::uint64_t nextRoutePreparationSequence_ = 1U;
		std::uint64_t routePreparationEpoch_ = 0U;
		std::size_t lastRoutePreparationBatch_ = 0U;
		std::size_t routePreparationBatchHighWater_ = 0U;
		struct ReplacementIntent {
			NavigationOwnerHandle owner{};
			std::uint64_t key = 0U;
			RouteRequestClass requestClass = RouteRequestClass::Direct;
			NavigationRequestHandle request{};
		};
		// Replacement keys are caller-owned intent tokens, not state attached to
		// an object. The bounded list is pruned with request release/revocation.
		std::vector<ReplacementIntent> replacementIntents_;
		// Agents receive a bounded reservation within the shared request store. This
		// retains direct-query compatibility while preventing either class from
		// consuming the entire store during synchronized replans.
		std::vector<NavigationRequestHandle> agentRequestHandles_;
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
		// A handful of copied examples accompanies the scalar reason counters. It is
		// reset with the world lifetime and prevents a persistent transition fault
		// from turning diagnostics into an unbounded route-packet history.
		std::uint8_t preparedRouteTransitionUnknownDiagnosticCount_ = 0U;
		std::vector<std::shared_ptr<const DetourGroundShardGeneration>> publishedShards_;
		// Only complete pristine tiles that survived protected publication are kept
		// for optional export. Dynamic rebuilds clear this cache rather than trying
		// to synthesize cache evidence from a changed shard.
		std::vector<PublishedGroundCacheCoverage> publishedGroundCaches_;
		std::vector<Ground::GroundRouteQuery::RetainedRoute> publishedRouteData_;
		std::vector<NavigationRouteHandle> requestRoutes_;
		std::vector<NavigationInvalidation> invalidations_;
		std::uint64_t nextInvalidationCursor_ = 1U;
		NavigationSettings settings_ = NavigationSettings::Defaults();
		std::atomic_bool initialized_{false};
		std::atomic_bool contractTestWorkAvailable_{false};
	};
}
