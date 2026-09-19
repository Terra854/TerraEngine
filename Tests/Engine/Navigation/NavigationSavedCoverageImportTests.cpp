// NavigationSavedCoverageImportTests.cpp - real cache-to-coverage acceptance.
//
// These tests deliberately build provider tile bytes through the production
// Recast path, then hand those bytes back through NavigationSystem. Codec-only
// acceptance is insufficient: a successful case must publish coverage that a
// normal public route request can actually consume.

#include "../TestHarness.h"

#include "Navigation/Backend/RecastGroundBuilder.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Navigation/Persistence/SavedNavigation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace TerraEngine::Tests {
	namespace {
		using namespace Navigation;

		std::shared_ptr<const NavigationGeometrySnapshot> MakeTerrain() {
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{1.0F, 0.0F, 1.0F}, {31.0F, 0.0F, 31.0F},
				{31.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 31.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			const auto created = NavigationGeometrySnapshot::Create(descriptor, document);
			return created.accepted ? created.snapshot : nullptr;
		}

		bool QueueFreshTerrain(NavigationSystem& system, const NavigationProfile& profile,
			const std::shared_ptr<const NavigationGeometrySnapshot>& terrain) {
			return terrain != nullptr && system.QueueInitialGroundBuild(profile, {terrain});
		}

		std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> MakeNarrowPassageSources() {
			NavigationGeometryDocument floor{};
			floor.kind = NavigationGeometryPayloadKind::TriangleMesh;
			floor.vertices = {{0.0F, 0.0F, 0.0F}, {32.0F, 0.0F, 0.0F},
				{32.0F, 0.0F, 8.0F}, {0.0F, 0.0F, 8.0F}};
			// Keep the floor winding upward (+Y), matching the provider's walkable
			// surface convention; the opposite winding is treated as back-facing.
			floor.triangles = {{0U, 2U, 1U}, {0U, 3U, 2U}};

			NavigationGeometrySnapshotDescriptor floorDescriptor{};
			floorDescriptor.sourceUuid = NavigationUuid::FromString("10000000000000000000000000000001");
			floorDescriptor.sourceRevision = 1U;
			floorDescriptor.canonicalPlacementRevision = 1U;
			floorDescriptor.physicalReadyRevision = 1U;
			floorDescriptor.sourceKind = NavigationSourceKind::Terrain;
			const auto floorSnapshot = NavigationGeometrySnapshot::Create(floorDescriptor, floor);
			if(!floorSnapshot.accepted) return {};

			// Runtime scene capture emits one immutable navigation source per collider
			// record. Keep that production shape here: a single source holding all three
			// boxes would have one aggregate obstruction AABB, which intentionally
			// conservatively seals the gap between otherwise separate primitives.
			const std::array<NavigationGeometryPrimitive, 3U> passageColliders{{
				{NavigationGeometryPrimitiveKind::Box, {16.0F, 0.875F, 1.0F}, {0.25F, 0.875F, 1.0F}},
				{NavigationGeometryPrimitiveKind::Box, {16.0F, 0.875F, 7.0F}, {0.25F, 0.875F, 1.0F}},
				{NavigationGeometryPrimitiveKind::Box, {16.0F, 2.25F, 4.0F}, {0.25F, 0.65F, 2.0F}}}};
			const std::array<std::string_view, 3U> colliderIds{{
				"20000000000000000000000000000002",
				"20000000000000000000000000000003",
				"20000000000000000000000000000004"}};
			std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
			sources.reserve(1U + passageColliders.size());
			sources.push_back(floorSnapshot.snapshot);
			for(std::size_t index = 0U; index < passageColliders.size(); ++index) {
				NavigationGeometryDocument collider{};
				collider.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
				collider.primitives.push_back(passageColliders[index]);
				NavigationGeometrySnapshotDescriptor descriptor = floorDescriptor;
				descriptor.sourceUuid = NavigationUuid::FromString(colliderIds[index]);
				descriptor.sourceKind = NavigationSourceKind::Collider;
				const auto snapshot = NavigationGeometrySnapshot::Create(descriptor, collider);
				if(!snapshot.accepted) return {};
				sources.push_back(snapshot.snapshot);
			}
			return sources;
		}

		bool Drain(NavigationSystem& system) {
			for(std::uint32_t frame = 0U; frame < 12U; ++frame) {
				system.Update(1.0F / 60.0F);
				if(system.PublishedWorld() && !system.PublishedWorld()->coverage.empty()) return true;
			}
			return false;
		}

		NavigationSystem::SavedNavigationCacheCandidate Candidate(
			const NavigationSystem::SavedNavigationCacheSnapshot& snapshot,
			const NavigationProfile& profile, bool corrupt) {
			const auto encoded = EncodeSavedNavigation(snapshot.descriptor, snapshot.chunks);
			NavigationSystem::SavedNavigationCacheCandidate candidate{};
			candidate.bytes = encoded.bytes;
			candidate.logicalPath = "Navigation/ground.tenav";
			candidate.profile = profile;
			if(corrupt && !candidate.bytes.empty()) candidate.bytes.back() ^= std::byte{0x7f};
			return candidate;
		}

		NavigationSystem::SavedNavigationCacheCandidate CandidateVariant(
			const NavigationSystem::SavedNavigationCacheSnapshot& snapshot,
			std::uint64_t sourceFingerprintDelta, std::uint64_t settingsFingerprintDelta,
			bool removeChunks, bool duplicateChunk) {
			Navigation::SavedNavigationDescriptor descriptor = snapshot.descriptor;
			descriptor.sourceFingerprint += sourceFingerprintDelta;
			descriptor.settingsFingerprint += settingsFingerprintDelta;
			std::vector<Navigation::SavedNavigationChunk> chunks = snapshot.chunks;
			if(removeChunks) chunks.clear();
			if(duplicateChunk && !chunks.empty()) chunks.push_back(chunks.front());
			const auto encoded = EncodeSavedNavigation(descriptor, chunks);
			NavigationSystem::SavedNavigationCacheCandidate candidate{};
			candidate.bytes = encoded.bytes;
			candidate.logicalPath = "Navigation/ground.tenav";
			candidate.profile = NavigationProfile::Defaults();
			return candidate;
		}

		std::pair<NavigationOutcome, NavigationReason> RequestOutcome(
			NavigationSystem& system, const NavigationProfile& profile) {
			const auto world = system.PublishedWorld();
			if(!world) return {NavigationOutcome::Failed, NavigationReason::ProviderFailure};
			Ground::GroundRouteQuery::Request request{};
			request.owner = {world->world, NavigationOwnerId{7U}, 1U};
			request.start.world = world->world;
			request.destination.world = world->world;
			request.start.position.offsetMetres = {4.0, 0.0, 4.0};
			request.destination.position.offsetMetres = {28.0, 0.0, 4.0};
			request.profile = profile;
			const NavigationRequestHandle handle = system.SubmitOneShotRoute(request);
			if(!handle.IsValid()) return {NavigationOutcome::RejectedCapacity, NavigationReason::DomainLimit};
			for(std::uint32_t frame = 0U; frame < 24U; ++frame) {
				system.Update(1.0F / 60.0F);
				const auto read = system.ReadRequest(handle);
				if(read.record && IsTerminalNavigationOutcome(read.record->outcome))
					return std::pair<NavigationOutcome, NavigationReason>{read.record->outcome, read.record->reason};
			}
			return {NavigationOutcome::Failed, NavigationReason::ProviderFailure};
		}

		bool RequestCompletes(NavigationSystem& system, const NavigationProfile& profile) {
			return RequestOutcome(system, profile).first == NavigationOutcome::Complete;
		}
	}

	void RegisterNavigationSavedCoverageImportTests(TestRegistry& registry) {
		registry.Add("Navigation.SavedCoverage.ImportPublishesRealRouteCoverage", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			const NavigationProfile profile = NavigationProfile::Defaults();
			const auto terrain = MakeTerrain();
			system.Shutdown();
			system.Initialize();
			context.Expect(QueueFreshTerrain(system, profile, terrain) && Drain(system),
				"The cache import fixture must first publish real Recast coverage.");
			const auto snapshot = system.CapturePublishedGroundCache(profile);
			context.Expect(snapshot.has_value() && !snapshot->chunks.empty(),
				"Published complete coverage must be exportable as a saved cache.");
			if(!snapshot) { system.Shutdown(); return; }
			const auto candidate = Candidate(*snapshot, profile, false);
			system.Shutdown();
			system.Initialize();
			const auto receipt = system.QueueSavedGroundCacheImport(candidate);
			context.Expect(receipt.accepted && receipt.deferredUntilAuthoritativeCapture,
				"A bounded cache candidate must defer until the current automatic source is captured.");
			context.Expect(QueueFreshTerrain(system, profile, terrain) && Drain(system),
				"Import must be compared with a fresh authoritative source stage.");
			const auto status = system.SavedGroundCacheStatus();
			context.Expect(status.cacheUsed && status.rejectionReason.empty(),
				"A compatible real cache must be used rather than merely decoded.");
			context.Expect(RequestCompletes(system, profile),
				"A route request must complete from imported saved coverage.");
			system.Shutdown();
		});

		registry.Add("Navigation.SavedCoverage.CorruptCacheFallsBackAtomicallyToFreshCoverage", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			const NavigationProfile profile = NavigationProfile::Defaults();
			const auto terrain = MakeTerrain();
			system.Shutdown();
			system.Initialize();
			context.Expect(QueueFreshTerrain(system, profile, terrain) && Drain(system),
				"The fallback fixture requires an authoritative fresh source.");
			const auto snapshot = system.CapturePublishedGroundCache(profile);
			if(!snapshot) { context.Expect(false, "Fresh coverage must produce a cache snapshot."); system.Shutdown(); return; }
			auto candidate = Candidate(*snapshot, profile, true);
			system.Shutdown();
			system.Initialize();
			const auto receipt = system.QueueSavedGroundCacheImport(std::move(candidate));
			context.Expect(receipt.accepted, "Bounded corrupt bytes must be retained for checked rejection.");
			context.Expect(QueueFreshTerrain(system, profile, terrain) && Drain(system),
				"Corrupt cache rejection must not prevent fresh generation.");
			const auto status = system.SavedGroundCacheStatus();
			context.Expect(!status.cacheUsed && !status.rejectionReason.empty(),
				"Corrupt cache must be rejected with a diagnostic while fresh coverage remains authoritative.");
			context.Expect(RequestCompletes(system, profile),
				"Fresh fallback must preserve normal route reachability.");
			system.Shutdown();
		});

		registry.Add("Navigation.SavedCoverage.StaleIncompleteAndDuplicateCachesRejectWithoutPartialPublication", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			const NavigationProfile profile = NavigationProfile::Defaults();
			const auto terrain = MakeTerrain();
			system.Shutdown();
			system.Initialize();
			context.Expect(QueueFreshTerrain(system, profile, terrain) && Drain(system),
				"The negative import fixture requires complete authoritative coverage.");
			const auto snapshot = system.CapturePublishedGroundCache(profile);
			if(!snapshot) { context.Expect(false, "Fresh coverage must produce cache metadata."); system.Shutdown(); return; }

			const std::array<NavigationSystem::SavedNavigationCacheCandidate, 4U> candidates{
				CandidateVariant(*snapshot, 1U, 0U, false, false),
				CandidateVariant(*snapshot, 0U, 1U, false, false),
				CandidateVariant(*snapshot, 0U, 0U, true, false),
				CandidateVariant(*snapshot, 0U, 0U, false, true)};
			std::size_t rejected = 0U;
			for(const auto& candidate : candidates) {
				system.Shutdown();
				system.Initialize();
				const auto receipt = system.QueueSavedGroundCacheImport(candidate);
				// Duplicate keys are rejected by the canonical encoder before queueing;
				// the other variants reach the protected import validator.
				context.Expect(receipt.accepted == !candidate.bytes.empty(),
					"Each negative cache candidate must either queue for checked import or be rejected atomically during encoding.");
				(void)QueueFreshTerrain(system, profile, terrain);
				(void)Drain(system);
				const auto status = system.SavedGroundCacheStatus();
				const bool rejectedWithReason = !receipt.accepted
					|| (!status.cacheUsed && !status.rejectionReason.empty());
				if(rejectedWithReason) ++rejected;
				context.Expect(!status.cacheUsed && rejectedWithReason
					&& RequestCompletes(system, profile),
					"Stale, incomplete, or duplicate coverage must reject atomically and fall back to a complete fresh route.");
			}
			context.Expect(rejected == candidates.size(),
				"Every negative cache variant must retain a caller-visible rejection reason.");
			system.Shutdown();
		});

		registry.Add("Navigation.SavedCoverage.NarrowPassageProfileCacheMatchesFreshOutcome", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			NavigationProfile small = NavigationProfile::Defaults();
			small.radiusMetres = 0.25;
			small.heightMetres = 1.20;
			NavigationProfile large = NavigationProfile::Defaults();
			large.radiusMetres = 0.55;
			large.heightMetres = 2.10;
			const auto sources = MakeNarrowPassageSources();
			context.Expect(sources.size() == 4U,
				"The narrow-passage fixture must retain one terrain floor and three distinct collider records.");
			if(sources.size() != 4U) return;

			system.Shutdown();
			system.Initialize();
			context.Expect(system.QueueInitialGroundBuild(small, sources) && Drain(system),
				"The small profile must publish the real narrow-passage provider coverage.");
			const auto smallFresh = RequestOutcome(system, small);
			const auto smallSnapshot = system.CapturePublishedGroundCache(small);
			context.Expect(smallFresh.first == NavigationOutcome::Complete,
				"The small profile must complete uncached (outcome="
				+ std::string(ToString(smallFresh.first)) + ", reason="
				+ std::string(ToString(smallFresh.second)) + ").");
			context.Expect(smallSnapshot.has_value() && !smallSnapshot->chunks.empty(),
				"The small profile must produce exportable coverage.");
			if(!smallSnapshot) { system.Shutdown(); return; }

			system.Shutdown();
			system.Initialize();
			const auto smallImport = system.QueueSavedGroundCacheImport(Candidate(*smallSnapshot, small, false));
			context.Expect(smallImport.accepted && system.QueueInitialGroundBuild(small, sources) && Drain(system),
				"A matching small-profile cache must be checked against fresh authoritative geometry.");
			const auto smallStatus = system.SavedGroundCacheStatus();
			const auto smallCached = RequestOutcome(system, small);
			context.Expect(smallStatus.cacheUsed && smallCached == smallFresh,
				"Small-profile imported coverage must preserve the uncached route outcome and reason.");

			system.Shutdown();
			system.Initialize();
			context.Expect(system.QueueInitialGroundBuild(large, sources) && Drain(system),
				"The large profile must publish coverage before its uncached query is compared.");
			const auto largeFresh = RequestOutcome(system, large);
			const auto largeSnapshot = system.CapturePublishedGroundCache(large);
			context.Expect(largeFresh.first != NavigationOutcome::Complete && largeSnapshot.has_value(),
				"The large profile must not falsely complete through the narrow passage.");
			if(!largeSnapshot) { system.Shutdown(); return; }
			system.Shutdown();
			system.Initialize();
			const auto largeImport = system.QueueSavedGroundCacheImport(Candidate(*largeSnapshot, large, false));
			context.Expect(largeImport.accepted && system.QueueInitialGroundBuild(large, sources) && Drain(system),
				"The matching large-profile cache must publish real imported coverage.");
			context.Expect(system.SavedGroundCacheStatus().cacheUsed && RequestOutcome(system, large) == largeFresh,
				"Imported clearance rejection must match fresh coverage, not become a false complete route.");

			system.Shutdown();
			system.Initialize();
			const auto wrongProfile = system.QueueSavedGroundCacheImport(Candidate(*smallSnapshot, large, false));
			context.Expect(wrongProfile.accepted && system.QueueInitialGroundBuild(large, sources) && Drain(system),
				"A small-profile cache must remain queued for explicit rejection when a large profile is requested.");
			const auto wrongStatus = system.SavedGroundCacheStatus();
			const auto largeFallback = RequestOutcome(system, large);
			context.Expect(!wrongStatus.cacheUsed && !wrongStatus.rejectionReason.empty()
				&& largeFallback == largeFresh,
				"A wrong-profile cache must reject and fall back to the same large-profile outcome.");
			system.Shutdown();
		});
	}
}
