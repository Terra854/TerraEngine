// NavigationSystem.cpp - bounded worker preparation and protected publication.

#include "Navigation/NavigationSystem.h"

#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/JobGraph.h"
#include "Core/Scheduling/SystemScheduler.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <iterator>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Navigation {
	namespace {
		using RouteClock = std::chrono::steady_clock;

		[[nodiscard]] std::uint64_t RouteDurationNanoseconds(
			RouteClock::time_point start, RouteClock::time_point finish) noexcept {
			if(start == RouteClock::time_point{} || finish < start) return 0U;
			const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
			return duration > 0 ? static_cast<std::uint64_t>(duration) : 0U;
		}

		void RecordRouteDuration(std::uint64_t& count, std::uint64_t& total,
			std::uint64_t& maximum, RouteClock::time_point start, RouteClock::time_point finish) noexcept {
			const std::uint64_t duration = RouteDurationNanoseconds(start, finish);
			++count;
			if(total <= std::numeric_limits<std::uint64_t>::max() - duration)
				total += duration;
			else total = std::numeric_limits<std::uint64_t>::max();
			maximum = std::max(maximum, duration);
		}

		// Agent intent keys occupy an internal namespace so a caller-owned direct
		// replacement token can never supersede an optional agent's route.
		constexpr std::uint64_t kNavigationAgentReplacementKeyNamespace = 0xA600000000000000ULL;
		constexpr std::uint64_t kNavigationAgentReplacementKeyPayloadMask = 0x00FFFFFFFFFFFFFFULL;

		[[nodiscard]] NavigationDirtyIndex MakeSessionDirtyIndex() {
			// Dirty sequences certify one world lifetime only. Reusing their watermark
			// after Shutdown or a scene revoke would reject a valid new scene receipt
			// merely because its independent transaction counter starts lower.
			return NavigationDirtyIndex({
				.clearanceExpansionMetres = 0.5F,
				.buildHaloMetres = 0.25F});
		}

		[[nodiscard]] NavigationBudgetLimits MakeNavigationBudgetLimits(const NavigationSettings& settings) {
			// Background packets and their Recast output must charge the same bounded
			// categories as an active navigation world. The reservation state is shared
			// into retired tasks, so these limits remain meaningful after a revoke.
			NavigationBudgetLimits limits{};
			limits.geometryBytes = static_cast<std::size_t>(settings.maxGeometryBytes);
			limits.publishedNavigationBytes = static_cast<std::size_t>(settings.maxPublishedAndRetiredBytes);
			limits.buildScratchBytes = static_cast<std::size_t>(settings.maxBuildScratchBytes);
			limits.requestRouteBytes = static_cast<std::size_t>(settings.maxRequestAndRouteBytes);
			limits.profileBuildCount = settings.maxActiveProfileBuilds;
			limits.diagnosticBytes = static_cast<std::size_t>(settings.maxDiagnosticBytes);
			return limits;
		}

		[[nodiscard]] std::size_t SaturatingAdd(std::size_t left, std::size_t right) noexcept {
			return right > std::numeric_limits<std::size_t>::max() - left
				? std::numeric_limits<std::size_t>::max() : left + right;
		}

		[[nodiscard]] std::size_t EstimateRetainedGeometryBytes(
			const std::vector<std::shared_ptr<const NavigationGeometrySnapshot>>& sources) noexcept {
			// Geometry snapshots do not currently own NavigationBudget reservations.
			// A retired async packet can become their final owner after a revoke, so
			// charge each unique copied payload to BuildScratch until task completion.
			std::unordered_set<const NavigationGeometrySnapshot*> unique;
			unique.reserve(sources.size());
			std::size_t bytes = 0U;
			for(const auto& source : sources) {
				if(!source || !unique.insert(source.get()).second) continue;
				bytes = SaturatingAdd(bytes, sizeof(NavigationGeometrySnapshot));
				bytes = SaturatingAdd(bytes, source->Triangles().capacity() * sizeof(NavigationGeometryTriangleRecord));
				bytes = SaturatingAdd(bytes, source->Primitives().capacity() * sizeof(NavigationGeometryPrimitive));
				bytes = SaturatingAdd(bytes, source->Bvh().Records().capacity() * sizeof(NavigationGeometryBvhRecord));
			}
			return bytes;
		}

		struct DeferredProfileBuildFailure {
			NavigationOutcome outcome = NavigationOutcome::Failed;
			NavigationReason reason = NavigationReason::ProviderFailure;
		};

		// Ground builders expose provider-specific status values. Convert them once
		// at the worker boundary so public request results remain stable while still
		// distinguishing capacity, localization, source, and provider failures.
		[[nodiscard]] DeferredProfileBuildFailure ToDeferredProfileBuildFailure(
			RecastGroundBuildStatus status) noexcept {
			switch(status) {
			case RecastGroundBuildStatus::BudgetExceeded:
				return {NavigationOutcome::BudgetExhausted, NavigationReason::OutputLimit};
			case RecastGroundBuildStatus::LocalizationFailed:
				return {NavigationOutcome::InvalidRequest, NavigationReason::DomainLimit};
			case RecastGroundBuildStatus::InvalidGeometry:
				return {NavigationOutcome::UnavailableCoverage, NavigationReason::MissingSource};
			case RecastGroundBuildStatus::Cancelled:
				return {NavigationOutcome::Stale, NavigationReason::DirtyCoverage};
			case RecastGroundBuildStatus::InvalidProfile:
				return {NavigationOutcome::InvalidRequest, NavigationReason::ProviderFailure};
			case RecastGroundBuildStatus::RecastConfigurationFailed:
			case RecastGroundBuildStatus::RecastHeightfieldFailed:
			case RecastGroundBuildStatus::RecastRasterizationFailed:
			case RecastGroundBuildStatus::RecastCompactHeightfieldFailed:
			case RecastGroundBuildStatus::ProviderFailure:
				return {NavigationOutcome::Failed, NavigationReason::ProviderFailure};
			case RecastGroundBuildStatus::Success:
				break;
			}
			return {NavigationOutcome::Failed, NavigationReason::ProviderFailure};
		}

		[[nodiscard]] NavigationDynamicChangeKind ToDynamicChangeKind(
			Scene::NavigationSourceMutationOperation operation) noexcept {
			using SceneOperation = Scene::NavigationSourceMutationOperation;
			switch(operation) {
			case SceneOperation::Create: return NavigationDynamicChangeKind::Created;
			case SceneOperation::Enable: return NavigationDynamicChangeKind::Enabled;
			case SceneOperation::Disable: return NavigationDynamicChangeKind::Disabled;
			case SceneOperation::Move: return NavigationDynamicChangeKind::Moved;
			case SceneOperation::Rotate: return NavigationDynamicChangeKind::Rotated;
			case SceneOperation::Scale: return NavigationDynamicChangeKind::Scaled;
			case SceneOperation::Delete: return NavigationDynamicChangeKind::Removed;
			case SceneOperation::Replace: return NavigationDynamicChangeKind::Replaced;
			case SceneOperation::Reshape: return NavigationDynamicChangeKind::Reshaped;
			}
			return NavigationDynamicChangeKind::Reshaped;
		}

		[[nodiscard]] bool LocalizeMutationBounds(
			const Scene::NavigationSourceMutationBounds& bounds,
			const Scene::WorldPosition& origin,
			NavigationGeometryBounds& localized) noexcept {
			if(!bounds.valid) return true;
			const Scene::WorldLocalizationResult minimum = Scene::LocalizeWorldPosition(
				bounds.minimum, origin, Scene::kTransformSpaceMaximumRelativeRadiusMetres);
			const Scene::WorldLocalizationResult maximum = Scene::LocalizeWorldPosition(
				bounds.maximum, origin, Scene::kTransformSpaceMaximumRelativeRadiusMetres);
			if(!minimum.Succeeded() || !maximum.Succeeded()) return false;
			const auto fitsFloat = [](double value) {
				return std::isfinite(value)
					&& std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
			};
			if(!fitsFloat(minimum.offsetMetres.x) || !fitsFloat(minimum.offsetMetres.y)
				|| !fitsFloat(minimum.offsetMetres.z) || !fitsFloat(maximum.offsetMetres.x)
				|| !fitsFloat(maximum.offsetMetres.y) || !fitsFloat(maximum.offsetMetres.z)) return false;
			localized = {{static_cast<float>(minimum.offsetMetres.x), static_cast<float>(minimum.offsetMetres.y),
				static_cast<float>(minimum.offsetMetres.z)},
				{static_cast<float>(maximum.offsetMetres.x), static_cast<float>(maximum.offsetMetres.y),
				static_cast<float>(maximum.offsetMetres.z)}};
			return true;
		}

		[[nodiscard]] bool SetCanonicalDirtyOrigin(
			const Scene::WorldPosition& origin, NavigationDynamicSourceChange& change) noexcept {
			constexpr std::int64_t kTilesPerCell = static_cast<std::int64_t>(
				Scene::kWorldCellEdgeMetres / static_cast<double>(kGroundTileEdgeMetres));
			constexpr std::int64_t kBandsPerCell = static_cast<std::int64_t>(
				Scene::kWorldCellEdgeMetres / static_cast<double>(kGroundVerticalBandMetres));
			const auto multiply = [](std::int64_t value, std::int64_t scale) -> std::optional<std::int64_t> {
				if(scale <= 0 || value > std::numeric_limits<std::int64_t>::max() / scale
					|| value < std::numeric_limits<std::int64_t>::min() / scale) return std::nullopt;
				return value * scale;
			};
			const auto tileX = multiply(origin.cell.x, kTilesPerCell);
			const auto tileZ = multiply(origin.cell.z, kTilesPerCell);
			const auto verticalBand = multiply(origin.cell.y, kBandsPerCell);
			if(!tileX || !tileZ || !verticalBand) return false;
			change.canonicalTileOriginX = *tileX;
			change.canonicalTileOriginZ = *tileZ;
			change.canonicalVerticalBandOrigin = *verticalBand;
			return true;
		}

		struct EndpointCoverageCandidates {
			NavigationCoverageKey primary{};
			std::optional<NavigationCoverageKey> boundaryAlternative;
		};

		[[nodiscard]] std::optional<EndpointCoverageCandidates> CoverageCandidatesForEndpoint(
			const NavigationLocation& location, const NavigationProfile& profile,
			std::uint64_t profileBuildKey) noexcept {
			// This is shared by protected admission and worker route preparation. A
			// canonical cell is exactly 256 horizontal shards and 512 vertical bands,
			// so never convert a distant cell through one lossy global float.
			constexpr std::int64_t kShardsPerCell = static_cast<std::int64_t>(
				Scene::kWorldCellEdgeMetres / (kGroundTileEdgeMetres * 8.0F));
			constexpr std::int64_t kBandsPerCell = static_cast<std::int64_t>(
				Scene::kWorldCellEdgeMetres / kGroundVerticalBandMetres);
			const auto addCell = [](std::int64_t cell, std::int64_t scale, std::int64_t local)
				-> std::optional<std::int64_t> {
				if(scale <= 0 || cell > std::numeric_limits<std::int64_t>::max() / scale
					|| cell < std::numeric_limits<std::int64_t>::min() / scale) return std::nullopt;
				const std::int64_t scaled = cell * scale;
				if((local > 0 && scaled > std::numeric_limits<std::int64_t>::max() - local)
					|| (local < 0 && scaled < std::numeric_limits<std::int64_t>::min() - local)) return std::nullopt;
				return scaled + local;
			};
			const auto horizontalLocal = [](double offset) {
				return static_cast<std::int64_t>(std::floor(offset / (kGroundTileEdgeMetres * 8.0F)));
			};
			const auto verticalLocal = [](double offset) {
				return static_cast<std::int64_t>(std::floor(offset / kGroundVerticalBandMetres));
			};
			const auto shardX = addCell(location.position.cell.x, kShardsPerCell,
				horizontalLocal(location.position.offsetMetres.x));
			const auto shardZ = addCell(location.position.cell.z, kShardsPerCell,
				horizontalLocal(location.position.offsetMetres.z));
			const std::int64_t localVerticalBand = verticalLocal(location.position.offsetMetres.y);
			const auto band = addCell(location.position.cell.y, kBandsPerCell, localVerticalBand);
			if(!shardX || !shardZ || !band || *band < std::numeric_limits<std::int32_t>::min()
				|| *band > std::numeric_limits<std::int32_t>::max()) return std::nullopt;

			EndpointCoverageCandidates candidates{};
			candidates.primary = {*shardX, *shardZ, static_cast<std::int32_t>(*band), profileBuildKey};
			// Match the worker's narrow physics-contact tolerance. An alternative band
			// matters only close to a boundary and remains separate from stacked floors.
			const double supportProjectionTolerance = std::min<double>(
				static_cast<double>(kGroundVerticalBandMetres) * 0.5,
				profile.verticalResolutionMetres + profile.clearanceMarginMetres);
			const double localBandHeight = location.position.offsetMetres.y
				- static_cast<double>(localVerticalBand) * static_cast<double>(kGroundVerticalBandMetres);
			const std::int64_t alternativeBand = localBandHeight <= supportProjectionTolerance ? *band - 1LL
				: static_cast<double>(kGroundVerticalBandMetres) - localBandHeight <= supportProjectionTolerance ? *band + 1LL
				: *band;
			if(alternativeBand != *band && alternativeBand >= std::numeric_limits<std::int32_t>::min()
				&& alternativeBand <= std::numeric_limits<std::int32_t>::max()) {
				candidates.boundaryAlternative = {*shardX, *shardZ,
					static_cast<std::int32_t>(alternativeBand), profileBuildKey};
			}
			return candidates;
		}

		[[nodiscard]] std::int64_t FloorShard(std::int64_t tile) noexcept {
			// Division truncates toward zero in C++. Adjust a negative remainder so
			// the canonical tile-to-shard mapping remains floor-based at every int64
			// address, including INT64_MIN without negating it.
			std::int64_t quotient = tile / 8LL;
			if(tile % 8LL < 0LL) --quotient;
			return quotient;
		}

		[[nodiscard]] bool RequiresReplacementSnapshot(NavigationDynamicChangeKind kind) noexcept {
			return kind != NavigationDynamicChangeKind::Removed
				&& kind != NavigationDynamicChangeKind::Disabled;
		}

		[[nodiscard]] bool HasCommittedSnapshot(
			const std::vector<std::shared_ptr<const NavigationGeometrySnapshot>>& sources,
			const NavigationDynamicSourceChange& change) noexcept {
			return std::any_of(sources.begin(), sources.end(), [&change](const auto& source) {
				if(!source) return false;
				const NavigationGeometrySnapshotDescriptor& descriptor = source->Descriptor();
				return descriptor.sourceUuid == change.source
					&& descriptor.sourceRevision == change.sourceRevision
					&& descriptor.physicalReadyRevision >= change.physicalReadyRevision;
			});
		}

		[[nodiscard]] bool RouteIntersectsLocalizedBounds(
			const Ground::GroundRouteQuery::RetainedRoute& route,
			const NavigationProfile& profile,
			const Scene::WorldPosition& origin,
			const NavigationGeometryBounds& previousBounds, bool previousBoundsValid,
			const NavigationGeometryBounds& currentBounds, bool currentBoundsValid) noexcept {
			// Missing bounds cannot prove a route is unaffected, so fail safe and
			// invalidate. Complete bounds let unrelated routes remain available.
			if(!route.IsValid() || route.points.empty()
				|| (!previousBoundsValid && !currentBoundsValid)) return true;
			return (previousBoundsValid && RouteSegmentsIntersectLocalizedBounds(
				route.points, profile, origin, previousBounds))
				|| (currentBoundsValid && RouteSegmentsIntersectLocalizedBounds(
					route.points, profile, origin, currentBounds));
		}

		[[nodiscard]] bool RouteIntersectsLocalizedChange(
			const Ground::GroundRouteQuery::RetainedRoute& route,
			const NavigationProfile& profile,
			const Scene::WorldPosition& origin,
			const Scene::NavigationSourceMutationIntent& intent,
			const NavigationDynamicSourceChange& change) noexcept {
			return RouteIntersectsLocalizedBounds(route, profile, origin,
				change.previousBounds, intent.oldBounds.valid,
				change.currentBounds, intent.newBounds.valid);
		}

		[[nodiscard]] NavigationGeneration ChangeSequence(const NavigationDynamicSourceChange& change) noexcept {
			// Hand-authored native tests retain the historic source-revision ordering.
			// Committed scene receipts provide a separate sequence so content hashes
			// never decide which dynamic publication is newer.
			return change.changeSequence != 0U ? change.changeSequence : change.sourceRevision;
		}

		[[nodiscard]] bool RouteIntersectsCapturedSource(
			const Ground::GroundRouteQuery::RetainedRoute& route,
			const NavigationProfile& profile,
			const NavigationGeometrySnapshot& source) noexcept {
			// Automatic source indexes retain geometry in each source's own canonical
			// frame. Re-localising route waypoints here keeps the comparison exact at
			// large-world cell boundaries without reaching back into live scene state.
			if(!route.IsValid() || route.points.empty()) return true;
			NavigationGeometryBounds routeBounds{};
			routeBounds.minimum = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max()};
			routeBounds.maximum = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
				std::numeric_limits<float>::lowest()};
			for(const NavigationLocation& point : route.points) {
				const Scene::WorldLocalizationResult localized = Scene::LocalizeWorldPosition(
					point.position, source.Descriptor().canonicalFrameOrigin,
					Scene::kTransformSpaceMaximumRelativeRadiusMetres);
				if(!localized.Succeeded()
					|| !std::isfinite(localized.offsetMetres.x) || !std::isfinite(localized.offsetMetres.y)
					|| !std::isfinite(localized.offsetMetres.z)
					|| std::abs(localized.offsetMetres.x) > static_cast<double>(std::numeric_limits<float>::max())
					|| std::abs(localized.offsetMetres.y) > static_cast<double>(std::numeric_limits<float>::max())
					|| std::abs(localized.offsetMetres.z) > static_cast<double>(std::numeric_limits<float>::max())) {
					// A failed localisation cannot prove a retained route independent of
					// the replacement, so the caller must fail that route closed.
					return true;
				}
				const NavigationGeometryVector3 value{static_cast<float>(localized.offsetMetres.x),
					static_cast<float>(localized.offsetMetres.y), static_cast<float>(localized.offsetMetres.z)};
				routeBounds.minimum.x = std::min(routeBounds.minimum.x, value.x);
				routeBounds.minimum.y = std::min(routeBounds.minimum.y, value.y);
				routeBounds.minimum.z = std::min(routeBounds.minimum.z, value.z);
				routeBounds.maximum.x = std::max(routeBounds.maximum.x, value.x);
				routeBounds.maximum.y = std::max(routeBounds.maximum.y, value.y);
				routeBounds.maximum.z = std::max(routeBounds.maximum.z, value.z);
			}
			const float horizontalMargin = static_cast<float>(profile.radiusMetres + profile.clearanceMarginMetres);
			const float verticalMargin = static_cast<float>(profile.heightMetres + profile.clearanceMarginMetres);
			routeBounds.minimum.x -= horizontalMargin;
			routeBounds.minimum.y -= verticalMargin;
			routeBounds.minimum.z -= horizontalMargin;
			routeBounds.maximum.x += horizontalMargin;
			routeBounds.maximum.y += verticalMargin;
			routeBounds.maximum.z += horizontalMargin;
			const NavigationGeometryBounds& affected = source.Bounds();
			return routeBounds.minimum.x <= affected.maximum.x && routeBounds.maximum.x >= affected.minimum.x
				&& routeBounds.minimum.y <= affected.maximum.y && routeBounds.maximum.y >= affected.minimum.y
				&& routeBounds.minimum.z <= affected.maximum.z && routeBounds.maximum.z >= affected.minimum.z;
		}

		[[nodiscard]] bool SameCapturedSource(
			const NavigationEligibleSourceRecord& left,
			const NavigationEligibleSourceRecord& right) noexcept {
			if(left.identity != right.identity || left.revisions != right.revisions
				|| left.geometry == nullptr || right.geometry == nullptr) return false;
			const NavigationGeometrySnapshotDescriptor& leftDescriptor = left.geometry->Descriptor();
			const NavigationGeometrySnapshotDescriptor& rightDescriptor = right.geometry->Descriptor();
			return left.geometry->ContentFingerprint() == right.geometry->ContentFingerprint()
				&& leftDescriptor.sourceUuid == rightDescriptor.sourceUuid
				&& leftDescriptor.sourceRevision == rightDescriptor.sourceRevision
				&& leftDescriptor.canonicalPlacementRevision == rightDescriptor.canonicalPlacementRevision
				&& leftDescriptor.physicalReadyRevision == rightDescriptor.physicalReadyRevision
				&& leftDescriptor.sourceKind == rightDescriptor.sourceKind
				&& leftDescriptor.subshapeIndex == rightDescriptor.subshapeIndex
				&& leftDescriptor.canonicalFrameOrigin == rightDescriptor.canonicalFrameOrigin;
		}

		[[nodiscard]] std::vector<Ground::GroundBoundaryInterval> CollectBoundaryIntervals(
			const std::vector<std::shared_ptr<const DetourGroundShardGeneration>>& shards) {
			std::vector<Ground::GroundBoundaryInterval> intervals;
			for(const auto& shard : shards) {
				if(!shard) continue;
				const NavigationCoverageKey key{shard->reference.shard.x, shard->reference.shard.z,
					shard->reference.shard.verticalBand, shard->reference.shard.profileBuildKey};
				for(const DetourGroundBoundarySpan& span : CollectDetourGroundBoundarySpans(*shard)) {
					intervals.push_back({key, shard->reference.generation, span.boundaryAxis,
						span.positiveSide, span.layer, span.intervalStart, span.intervalEnd,
						span.minimumClearanceMetres, span.supported, span.clearanceValid,
						0xFFFFFFFFU, 1.0F, span.heightAtIntervalStart,
						span.heightAtIntervalEnd, span.maximumStepHeightMetres});
				}
			}
			return intervals;
		}
	}

	NavigationSystem& NavigationSystem::Instance() {
		static NavigationSystem system;
		return system;
	}

	void NavigationSystem::Initialize() {
		std::lock_guard<std::mutex> lock(mutex_);
		if(initialized_.load(std::memory_order_acquire)) {
			return;
		}
		// Scene integration replaces this bootstrap identity with the active scene
		// lifetime. The foundation still starts from a valid, generation-qualified
		// root so no caller can mistake an uninitialized provider pointer for a world.
		world_ = std::make_unique<NavigationWorld>(NavigationWorldHandle{NavigationWorldId{1U}, 1U});
		// A fresh system world receives a new budget facade, while any retired task
		// retains the old reservation state through its permit. Coordinator capacity
		// spans both facades, so a rapid reinitialize cannot over-admit packet bytes.
		asyncBuildBudget_ = std::make_shared<NavigationBudget>(MakeNavigationBudgetLimits(settings_));
		if(asyncBuildEpoch_ != std::numeric_limits<std::uint64_t>::max()) ++asyncBuildEpoch_;
		if(asyncBuildEpoch_ == 0U) asyncBuildEpoch_ = 1U;
		requestStore_ = std::make_unique<NavigationRequestStore>(settings_.maxAdmittedRequests);
		routeStore_ = std::make_unique<NavigationRouteStore>(settings_.maxAdmittedRequests);
		pendingGroundBuilds_.clear();
		preparedGroundBuilds_.clear();
		pendingDynamicChanges_.clear();
		pendingDynamicRebuilds_.clear();
		preparedDynamicRebuilds_.clear();
		dynamicRouteTransitions_.clear();
		dirtyIndex_ = MakeSessionDirtyIndex();
		dynamicDiagnostics_ = {};
		preparedRouteTransitionUnknownDiagnosticCount_ = 0U;
		diagnostics_.PublishDynamic(dynamicDiagnostics_);
		lastProtectedBoundaryGeneration_ = 0U;
		dynamicRebuildProgress_.reset();
		dynamicSourceStages_.clear();
		stagedDynamicSourceReplacements_.clear();
		pendingDynamicSourceStageReplacement_.reset();
		automaticGroundSourceStage_.reset();
		automaticSourceIndexReplacement_.reset();
		freshAutomaticCaptureRequested_ = false;
		pendingRoutes_.clear();
		inFlightRoutes_.clear();
		deferredRoutes_.clear();
		preparedRoutes_.clear();
		pendingAgentRouteAdmissionDemand_.store(0U, std::memory_order_release);
		publishedShards_.clear();
		publishedGroundCaches_.clear();
		publishedRouteData_.clear();
		requestRoutes_.clear();
		replacementIntents_.clear();
		agentRequestHandles_.clear();
		invalidations_.clear();
		nextInvalidationCursor_ = 1U;
		nextRoutePreparationSequence_ = 1U;
		if(routePreparationEpoch_ != std::numeric_limits<std::uint64_t>::max()) {
			++routePreparationEpoch_;
		}
		if(routePreparationEpoch_ == 0U) routePreparationEpoch_ = 1U;
		lastRoutePreparationBatch_ = 0U;
		routePreparationBatchHighWater_ = 0U;
		latestAutomaticSourceIndexRevision_ = 0U;
		if(savedGroundCacheCandidate_) {
			// A normal project/archive mount can happen before scene geometry is
			// captured. Keep its copied bytes through Initialize and wait for the
			// authoritative packet instead of trusting a self-declared descriptor.
			savedGroundCacheStatus_.candidatePending = true;
			savedGroundCacheStatus_.cacheUsed = false;
			savedGroundCacheStatus_.candidateBytes = savedGroundCacheCandidate_->bytes.size();
			savedGroundCacheStatus_.logicalPath = savedGroundCacheCandidate_->logicalPath;
			savedGroundCacheStatus_.rejectionReason = "SavedNavigationAwaitingAuthoritativeCapture";
			savedGroundCacheAttemptedProfiles_.clear();
			savedGroundCacheImportInFlight_ = false;
		}
		initialized_.store(true, std::memory_order_release);
		if(asyncBuildCoordinator_.Status().retiredCount != 0U)
			contractTestWorkAvailable_.store(true, std::memory_order_release);
		PublishAsyncBuildDiagnosticsLocked();
	}

	bool NavigationSystem::ConfigureSettings(NavigationSettings settings) {
		// Request/route handles retain slot generations from the stores constructed
		// during Initialize. Refusing late edits keeps those lifetime guarantees
		// intact instead of attempting an unsafe in-place store resize.
		if(!settings.Validate().Succeeded()) {
			return false;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		if(initialized_.load(std::memory_order_acquire)) {
			return false;
		}
		settings_ = std::move(settings);
		return true;
	}

	NavigationSettings NavigationSystem::SettingsSnapshot() const {
		std::lock_guard<std::mutex> lock(mutex_);
		// Copying the small settings record makes validation evidence stable without
		// exposing the mutable configuration retained by the protected system.
		return settings_;
	}

	void NavigationSystem::Update(float) {
		// Converted scheduler execution uses the two declarations below. This method
		// remains a safe lifecycle fallback for direct host integrations.
		if(!initialized_.load(std::memory_order_acquire)) {
			return;
		}
		PrepareAvailableWork();
		PublishPreparedWork();
	}

	void NavigationSystem::Shutdown() {
		std::lock_guard<std::mutex> lock(mutex_);
		RetireAsyncBuildsLocked();
		// Shutdown has no later Navigation frame to poll. The ThreadPool task still
		// owns its permit; a later Initialize resumes polling, otherwise pool teardown
		// joins the task before process destruction. Capacity remains bounded either way.
		contractTestWorkAvailable_.store(false, std::memory_order_release);
		pendingGroundBuilds_.clear();
		preparedGroundBuilds_.clear();
		savedGroundCacheCandidate_.reset();
		savedGroundCacheStatus_ = {};
		savedGroundCacheAttemptedProfiles_.clear();
		savedGroundCacheImportInFlight_ = false;
		pendingDynamicChanges_.clear();
		pendingDynamicRebuilds_.clear();
		preparedDynamicRebuilds_.clear();
		dynamicRouteTransitions_.clear();
		// Shutdown ends the world lifetime even if the singleton survives. Reset the
		// per-world ordering and counters before a later Initialize receives fresh
		// scene transaction sequences.
		dirtyIndex_ = MakeSessionDirtyIndex();
		dynamicDiagnostics_ = {};
		preparedRouteTransitionUnknownDiagnosticCount_ = 0U;
		diagnostics_.PublishDynamic(dynamicDiagnostics_);
		lastProtectedBoundaryGeneration_ = 0U;
		dynamicRebuildProgress_.reset();
		dynamicSourceStages_.clear();
		stagedDynamicSourceReplacements_.clear();
		pendingDynamicSourceStageReplacement_.reset();
		automaticGroundSourceStage_.reset();
		automaticSourceIndexReplacement_.reset();
		freshAutomaticCaptureRequested_ = false;
		pendingRoutes_.clear();
		inFlightRoutes_.clear();
		deferredRoutes_.clear();
		preparedRoutes_.clear();
		pendingAgentRouteAdmissionDemand_.store(0U, std::memory_order_release);
		publishedShards_.clear();
		publishedGroundCaches_.clear();
		publishedRouteData_.clear();
		requestRoutes_.clear();
		replacementIntents_.clear();
		agentRequestHandles_.clear();
		requestStore_.reset();
		routeStore_.reset();
		world_.reset();
		asyncBuildBudget_.reset();
		initialized_.store(false, std::memory_order_release);
		PublishAsyncBuildDiagnosticsLocked();
	}

	std::string NavigationSystem::SystemName() {
		return "NavigationSystem";
	}

	TerraEngine::Scheduling::SystemDeclaration NavigationSystem::SchedulerDeclaration() {
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration;
		declaration.name = SystemName();
		declaration.phase = "Navigation";
		declaration.after = {"WorldSpatialSystem"};
		declaration.before = {"ScriptingSystem"};
		declaration.reads = {
			kResourceSceneChangePublication,
			kResourcePublishedSpatialSnapshot,
			kResourcePublishedWorldRegionResidency,
			kResourceTerrainPublishedCollision,
			kResourceNavigationCaptureInputs,
			kResourceNavigationGeometryPublications,
			kResourceNavigationRequestQueue,
			kResourceNavigationPreparedCandidates,
			kResourceNavigationAsyncBuildTasks
		};
		declaration.writes = {
			kResourceNavigationRequestQueue,
			kResourceNavigationPreparedCandidates,
			kResourceNavigationEligibleSourceIndex,
			kResourceNavigationWorldRoot,
			kResourceNavigationRouteValidity,
			kResourceNavigationResults,
			kResourceNavigationDiagnostics,
			kResourceNavigationAsyncBuildTasks
		};
		declaration.mainThread = false;
		declaration.requiresFlushedSceneMutations = true;
		declaration.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		declaration.boundaryClassification = HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Prepare bounded geometry, BVH, tile, seam, and route candidates from retained immutable inputs.";
		declaration.protectedCommit = "Capture active object endpoint values, revalidate complete candidate groups, and exchange immutable navigation roots, route validity, results, and diagnostics.";
		declaration.protectedReason = "Scene, owner, source, profile, intent, and dependency generations must match atomically before publication.";
		declaration.profilingLabel = "Navigation";
		declaration.omitWhenNoFrameWorkloads = true;
		declaration.usesDefaultMetadata = false;
		return declaration;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration>
	NavigationSystem::DeclareFrameWorkloads(float) {
		using namespace TerraEngine::Scheduling;
		const std::size_t anticipatedAgentDemand =
			pendingAgentRouteAdmissionDemand_.load(std::memory_order_acquire);
		if(!initialized_.load(std::memory_order_acquire)
			|| (!contractTestWorkAvailable_.load(std::memory_order_acquire)
				&& anticipatedAgentDemand == 0U)) {
			return {};
		}
		// Declaration may run for diagnostics without executing a frame. It therefore
		// observes queue size only; the protected capture workload below is the sole
		// place that moves packets out of the live admission queue.
		// A Detour search is an independent worker job. Each lane receives a very
		// small FIFO range so a frame can absorb a sustained request rate without
		// giving one worker an unbounded serial queue. Publication still waits for
		// every route in the captured packet, preserving cancellation recovery.
		constexpr std::size_t kMaximumRoutePreparationLanes = 8U;
		constexpr std::size_t kRouteSearchesPerPreparationLane = 4U;
		auto routeBatch = std::make_shared<std::vector<PendingRoute>>();
		std::size_t declaredRouteCount = 0U;
		std::size_t maximumRoutePartitions = 1U;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			// A smaller configured search budget constrains both admission into this
			// worker handoff and partition count. Four searches per lane keep the
			// maximum packet at 32 routes while retaining an eight-worker ceiling.
			maximumRoutePartitions = std::max<std::size_t>(1U,
				std::min<std::size_t>(kMaximumRoutePreparationLanes, settings_.maxConcurrentSearches));
			const std::size_t maximumRouteBatch = maximumRoutePartitions * kRouteSearchesPerPreparationLane;
			declaredRouteCount = std::min(maximumRouteBatch,
				pendingRoutes_.size() + inFlightRoutes_.size());
			const std::size_t remainingCapacity = maximumRouteBatch - declaredRouteCount;
			declaredRouteCount += std::min(remainingCapacity, anticipatedAgentDemand);
		}
		// Keep small packets fully parallel while spreading a full 32-route packet
		// across the same bounded lanes. The declaration is diagnostic-only, so the
		// protected capture still clamps the live FIFO handoff independently.
		const std::size_t partitionWorkItemCount = std::max<std::size_t>(1U,
			(std::max<std::size_t>(1U, declaredRouteCount) + maximumRoutePartitions - 1U)
				/ maximumRoutePartitions);

		auto capture = MakeProtectedCommitPhase(
			"NavigationCaptureRoutes",
			SystemName(),
			{kResourceNavigationRequestQueue, kResourceNavigationAsyncBuildTasks},
			{kResourceNavigationRequestQueue, kResourceNavigationPreparedCandidates,
				kResourceNavigationAsyncBuildTasks},
			"NavigationCapture");
		capture.phase = "Navigation";
		capture.stableDescriptorId = "NavigationSystem/Navigation/Capture";
		capture.hardBoundary = true;
		capture.boundaryClassification = HardBoundaryClassification::Protected;
		capture.protectedCommit = "Move one bounded route packet batch into an immutable worker handoff while preserving cancellation recovery.";
		capture.estimatedWorkItemCount = 1U;
		capture.work = [this, routeBatch, declaredRouteCount, maximumRoutePartitions](JobGraphContext&) {
			{
			std::lock_guard<std::mutex> lock(mutex_);
			// A cancelled or rejected prior frame never reached publication. Restore its
			// packets in original order before taking new work, so scheduler diagnostics
			// and cancellations cannot silently discard accepted requests.
			if(!inFlightRoutes_.empty()) {
				std::unordered_set<std::uint64_t> recoveringSequences;
				recoveringSequences.reserve(inFlightRoutes_.size());
				for(const PendingRoute& route : inFlightRoutes_) {
					recoveringSequences.insert(route.preparationSequence);
				}
				// A cancelled frame can leave results from only the earliest ranges.
				// Discard those before requeueing the full batch; otherwise a later
				// publication could process the same handle twice.
				std::erase_if(preparedRoutes_, [this, &recoveringSequences](const PreparedRoute& route) {
					return route.preparationEpoch == routePreparationEpoch_
						&& recoveringSequences.contains(route.preparationSequence);
				});
			}
			while(!inFlightRoutes_.empty()) {
				pendingRoutes_.push_front(std::move(inFlightRoutes_.back()));
				inFlightRoutes_.pop_back();
			}

			const std::size_t maximumRouteBatch = maximumRoutePartitions * kRouteSearchesPerPreparationLane;
			const std::size_t count = std::min({maximumRouteBatch,
				declaredRouteCount, pendingRoutes_.size()});
			routeBatch->clear();
			routeBatch->reserve(count);
			const RouteClock::time_point capturedAt = RouteClock::now();
			for(std::size_t index = 0U; index < count; ++index) {
				inFlightRoutes_.push_back(std::move(pendingRoutes_.front()));
				pendingRoutes_.pop_front();
				PendingRoute& capturedRoute = inFlightRoutes_.back();
				if(!capturedRoute.captureTimingRecorded) {
					RecordRouteDuration(dynamicDiagnostics_.routeAdmissionToCaptureCount,
						dynamicDiagnostics_.routeAdmissionToCaptureNanoseconds,
						dynamicDiagnostics_.routeAdmissionToCaptureMaxNanoseconds,
						capturedRoute.readyAdmissionTime, capturedAt);
					capturedRoute.captureTimingRecorded = true;
				}
				capturedRoute.capturedTime = capturedAt;
				routeBatch->push_back(capturedRoute);
			}
			lastRoutePreparationBatch_ = count;
			routePreparationBatchHighWater_ = std::max(routePreparationBatchHighWater_, count);
			}
			// Recast/Detour admission is a protected capture operation. The worker task
			// receives only the immutable packet retained here and cannot join this frame.
			PrepareAvailableWork(false);
		};

		auto prepare = MakeWorkerPreparePhase(
			"NavigationPrepare",
			SystemName(),
			{kResourceNavigationCaptureInputs, kResourceNavigationEligibleSourceIndex,
				kResourceNavigationWorldRoot,
				kResourcePublishedSpatialSnapshot, kResourcePublishedWorldRegionResidency,
				kResourceTerrainPublishedCollision, kResourceNavigationGeometryPublications,
				kResourceNavigationPreparedCandidates},
			{kResourceNavigationPreparedCandidates},
			"NavigationPrepare");
		prepare.phase = "Navigation";
		prepare.stableDescriptorId = "NavigationSystem/Navigation/Prepare";
		prepare.safeSubwork = "Prepare disjoint route ranges from immutable published navigation inputs; expensive ground/dynamic work was admitted as a bounded asynchronous copied packet at the protected capture boundary.";
		prepare.estimatedWorkItemCount = std::max<std::size_t>(1U, declaredRouteCount);
		prepare.partitionPolicy = FrameWorkloadPartitionPolicy::DeterministicRanges;
		// The scheduler partitions the bounded packet into at most eight FIFO ranges.
		// Small packets stay one-search-per-lane; a full packet reaches four searches
		// per lane, bounding serial work without reducing low-load parallelism.
		prepare.partitionWorkItemCount = partitionWorkItemCount;
		prepare.maximumPartitionCount = maximumRoutePartitions;
		prepare.explicitDependencies = {capture.stableDescriptorId};
		prepare.rangeWork = [this, routeBatch](const FrameWorkloadPartition& partition, JobGraphContext& context) {
			if(context.IsCancellationRequested()) {
				return;
			}
			const std::size_t begin = std::min(partition.itemBegin, routeBatch->size());
			const std::size_t end = std::min(partition.itemEnd, routeBatch->size());
			std::vector<PreparedRoute> prepared;
			prepared.reserve(end - begin);
			for(std::size_t index = begin; index < end; ++index) {
				const PendingRoute& route = (*routeBatch)[index];
				const RouteClock::time_point workerStarted = RouteClock::now();
				auto result = SearchPublishedGroundRoute(route.request, route.worldRoot, route.shards, route.portals);
				const RouteClock::time_point workerFinished = RouteClock::now();
				prepared.push_back({route.preparationSequence, route.preparationEpoch,
					route.handle, route.intentGeneration, route.observation,
					route.readyAdmissionTime, route.capturedTime, workerStarted, workerFinished,
					std::move(result), route.worldRoot ? route.worldRoot->world : NavigationWorldHandle{},
					route.worldRoot ? route.worldRoot->rootGeneration : 0U});
			}
			if(!prepared.empty()) {
				std::lock_guard<std::mutex> lock(mutex_);
				for(PreparedRoute& route : prepared) {
					RecordRouteDuration(dynamicDiagnostics_.routeWorkerSearchCount,
						dynamicDiagnostics_.routeWorkerSearchNanoseconds,
						dynamicDiagnostics_.routeWorkerSearchMaxNanoseconds,
						route.workerStartedTime, route.workerFinishedTime);
					preparedRoutes_.push_back(std::move(route));
				}
			}
		};

		auto publish = MakeProtectedCommitPhase(
			"NavigationPublish",
			SystemName(),
			{kResourceNavigationPreparedCandidates, kResourceNavigationAsyncBuildTasks, kResourceSceneChangePublication,
				kResourceNavigationCaptureInputs, kResourceNavigationEligibleSourceIndex,
				kResourceNavigationWorldRoot},
			{kResourceNavigationAsyncBuildTasks, kResourceNavigationEligibleSourceIndex, kResourceNavigationWorldRoot,
				kResourceNavigationRouteValidity,
				kResourceNavigationResults, kResourceNavigationDiagnostics},
			"Generation-qualified navigation authority can change only at the protected Navigation boundary.",
			"NavigationPublish");
		publish.phase = "Navigation";
		publish.stableDescriptorId = "NavigationSystem/Navigation/Publish";
		publish.explicitDependencies = {prepare.stableDescriptorId};
		// Publication exchanges authority visible to later scripting, so it is a
		// hard protected boundary rather than an ordinary main-thread callback.
		publish.boundaryClassification = HardBoundaryClassification::Protected;
		publish.hardBoundary = true;
		publish.protectedCommit = "Validate captured object endpoint and intent generations, then exchange one complete navigation publication group.";
		publish.estimatedWorkItemCount = 1U;
		publish.work = [this](JobGraphContext&) { PublishPreparedWork(); };

		return {std::move(capture), std::move(prepare), std::move(publish)};
	}

	std::shared_ptr<const NavigationWorldRoot> NavigationSystem::PublishedWorld() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return world_ ? world_->PublishedRoot() : nullptr;
	}

	std::vector<NavigationSourceIndexRecord> NavigationSystem::AutomaticSourceObservationSnapshot() const {
		std::lock_guard<std::mutex> lock(mutex_);
		// The stage was admitted at a protected boundary and owns immutable source
		// snapshots. Prefer its copied index while provider work is pending: the
		// published root intentionally remains old until the full tuple commits.
		if(automaticGroundSourceStage_ && !automaticGroundSourceStage_->sourceIndex.empty()) {
			return automaticGroundSourceStage_->sourceIndex;
		}
		const std::shared_ptr<const NavigationWorldRoot> root = world_ ? world_->PublishedRoot() : nullptr;
		return root ? root->sources : std::vector<NavigationSourceIndexRecord>{};
	}

	bool NavigationSystem::QueueInitialGroundBuild(
		NavigationProfile profile,
		std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources) {
		InitialGroundBuildInput input{};
		input.profile = std::move(profile);
		input.sources = std::move(sources);
		return QueueInitialGroundBuilds({std::move(input)});
	}

	bool NavigationSystem::QueueInitialGroundBuilds(std::vector<InitialGroundBuildInput> builds) {
		if(builds.empty() || builds.size() > settings_.maxActiveProfileBuilds) return false;
		std::vector<std::uint64_t> profileKeys;
		profileKeys.reserve(builds.size());
		for(const InitialGroundBuildInput& build : builds) {
			if(!build.profile.Validate().accepted || build.sources.empty()) return false;
			for(const auto& source : build.sources) if(!source) return false;
			if(build.eligibleSourceIndex != nullptr
				&& (!build.eligibleSourceIndex->HasCurrentTerrain()
					|| build.terrainPublicationGeneration == 0U)) return false;
			profileKeys.push_back(MakeGroundProfileBuildKey(build.profile));
		}
		std::sort(profileKeys.begin(), profileKeys.end());
		if(std::adjacent_find(profileKeys.begin(), profileKeys.end()) != profileKeys.end()) return false;
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire)) return false;
		std::unordered_set<std::uint64_t> retainedProfiles;
		if(const std::shared_ptr<const NavigationWorldRoot> root = world_ ? world_->PublishedRoot() : nullptr) {
			for(const NavigationCoverageRecord& coverage : root->coverage)
				retainedProfiles.insert(coverage.key.profileBuildKey);
		}
		for(const PendingGroundBuild& pending : pendingGroundBuilds_)
			retainedProfiles.insert(MakeGroundProfileBuildKey(pending.profile));
		for(const PreparedGroundBuild& prepared : preparedGroundBuilds_)
			retainedProfiles.insert(MakeGroundProfileBuildKey(prepared.profile));
		for(const std::uint64_t profileKey : profileKeys) {
			if(!retainedProfiles.contains(profileKey)
				&& retainedProfiles.size() >= settings_.maxActiveProfileBuilds) return false;
			retainedProfiles.insert(profileKey);
		}
		// Automatic captures are one coherent scene-evidence tuple across every
		// profile. Reject a mixed batch before it can supersede queued work or change
		// route validity, preserving all prior state on an invalid submission.
		std::optional<NavigationGeneration> automaticSourceIndexRevision;
		for(const InitialGroundBuildInput& build : builds) {
			if(build.eligibleSourceIndex == nullptr) continue;
			const NavigationGeneration revision = build.eligibleSourceIndex->Revision();
			if(automaticSourceIndexRevision && *automaticSourceIndexRevision != revision) return false;
			automaticSourceIndexRevision = revision;
		}
		const std::shared_ptr<const NavigationWorldRoot> current = world_ ? world_->PublishedRoot() : nullptr;
		const bool replacesAutomaticSourceStage = automaticSourceIndexRevision != std::nullopt
			&& (!current || !current->eligibleSourceIndex
				|| current->eligibleSourceIndex->Revision() != *automaticSourceIndexRevision);
		if(replacesAutomaticSourceStage) {
			const auto automaticBuild = std::find_if(builds.begin(), builds.end(),
				[](const InitialGroundBuildInput& build) { return build.eligibleSourceIndex != nullptr; });
			// This protected capture observes a complete replacement tuple. Compare the
			// old and new immutable indexes now, before any worker can publish guidance
			// that crosses changed geometry. Unrelated retained paths remain valid until
			// the later root exchange refreshes their observation evidence.
			BeginAutomaticSourceIndexReplacementLocked(
				current ? current->eligibleSourceIndex : nullptr,
				automaticBuild != builds.end() ? automaticBuild->eligibleSourceIndex : nullptr,
				profileKeys);
			// The replacement capture owns the complete new profile set. Retaining a
			// profile omitted by this authoritative packet would later rebuild it from
			// stale source pointers, so replace the stage collection as one unit.
			dynamicSourceStages_.clear();
			dynamicRebuildProgress_.reset();
			// A prior transactional bridge packet refers to the old source-index tuple.
			// Do not let it promote after this full capture introduces new or removed
			// rows; its caller will receive normal fallback handling instead.
			pendingDynamicSourceStageReplacement_.reset();
			stagedDynamicSourceReplacements_.clear();
		}
		for(const std::uint64_t profileKey : profileKeys) {
			// A newer captured packet supersedes queued work for the same copied
			// profile. Coalescing happens only after every admission check succeeds,
			// so a rejected batch cannot mutate live queued work.
			std::erase_if(pendingGroundBuilds_, [profileKey](const PendingGroundBuild& pending) {
				return MakeGroundProfileBuildKey(pending.profile) == profileKey;
			});
			std::erase_if(preparedGroundBuilds_, [profileKey](const PreparedGroundBuild& prepared) {
				return MakeGroundProfileBuildKey(prepared.profile) == profileKey;
			});
		}
		for(InitialGroundBuildInput& build : builds) {
			if(build.eligibleSourceIndex != nullptr) {
				latestAutomaticSourceIndexRevision_ = build.eligibleSourceIndex->Revision();
				// Retain the captured immutable packet rather than a profile-specific
				// result. A caller may provide any valid copied profile after bootstrap,
				// and that profile must be able to build coverage from this same evidence.
				automaticGroundSourceStage_ = AutomaticGroundSourceStage{
					build.sources, build.sourceIndex, build.eligibleSourceIndex,
					build.terrainPublicationGeneration, profileConfigurationRevision_};
			}
			// Keep the same immutable source packet available for later dynamic
			// replacements. This is a shared-pointer copy, not a retained scene or
			// resource-manager reference.
			const std::uint64_t buildKey = MakeGroundProfileBuildKey(build.profile);
			auto staged = std::find_if(dynamicSourceStages_.begin(), dynamicSourceStages_.end(),
				[buildKey](const DynamicSourceStage& stage) {
					return MakeGroundProfileBuildKey(stage.profile) == buildKey;
				});
			if(staged == dynamicSourceStages_.end())
				dynamicSourceStages_.push_back({build.profile, build.sources});
			else *staged = {build.profile, build.sources};
			pendingGroundBuilds_.push_back({std::move(build.profile), std::move(build.sources),
				std::move(build.sourceIndex), std::move(build.eligibleSourceIndex),
				build.terrainPublicationGeneration, profileConfigurationRevision_});
		}
		contractTestWorkAvailable_.store(true, std::memory_order_release);
		return true;
	}

	bool NavigationSystem::PublishUnavailableTerrain(
		std::shared_ptr<const NavigationEligibleSourceIndex> eligibleSourceIndex,
		bool requestFreshAutomaticCapture) {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire) || !world_) return false;
		const std::shared_ptr<const NavigationWorldRoot> current = world_->PublishedRoot();
		if(!current) return false;
		const NavigationGeneration eligibleSourceRevision = eligibleSourceIndex != nullptr
			? eligibleSourceIndex->Revision() : 0U;
		NavigationWorldRoot candidate = world_->CreateCandidate();
		candidate.terrainState = NavigationTerrainState::Unavailable;
		candidate.terrainPublicationGeneration = 0U;
		candidate.sourceIndexRevision = eligibleSourceRevision != 0U ? eligibleSourceRevision
			: candidate.rootGeneration;
		candidate.eligibleSourceIndex = std::move(eligibleSourceIndex);
		candidate.coverage.clear();
		candidate.shards.clear();
		candidate.generatedPortals.clear();
		candidate.routeValidityGeneration = candidate.rootGeneration;
		candidate.routeValidity = {candidate.routeValidityGeneration, 0U};
		if(!world_->Publish(std::move(candidate), current->rootGeneration)) return false;

		// The unavailable root is visible before this cleanup begins. Retire the
		// current immutable provider task only after the candidate commit succeeds,
		// so a failed compare-and-publish cannot disturb a still-valid ready world.
		// Retired work remains charged until completion, but can no longer restore
		// the old terrain tuple through a later protected publication.
		RetireAsyncBuildsLocked();
		if(eligibleSourceRevision != 0U) latestAutomaticSourceIndexRevision_ = eligibleSourceRevision;
		else latestAutomaticSourceIndexRevision_ = 0U;
		freshAutomaticCaptureRequested_ = freshAutomaticCaptureRequested_ || requestFreshAutomaticCapture;

		// The root becomes unavailable before any later build. Discard every copied
		// packet tied to the old source/terrain evidence under this same system lock;
		// agent owners remain alive and will observe the terminal request normally.
		pendingGroundBuilds_.clear();
		preparedGroundBuilds_.clear();
		pendingDynamicChanges_.clear();
		pendingDynamicRebuilds_.clear();
		preparedDynamicRebuilds_.clear();
		dynamicRouteTransitions_.clear();
		dynamicRebuildProgress_.reset();
		dirtyIndex_ = MakeSessionDirtyIndex();
		dynamicSourceStages_.clear();
		stagedDynamicSourceReplacements_.clear();
		pendingDynamicSourceStageReplacement_.reset();
		publishedShards_.clear();
		publishedGroundCaches_.clear();
		automaticGroundSourceStage_.reset();
		automaticSourceIndexReplacement_.reset();
		savedGroundCacheImportInFlight_ = false;
		savedGroundCacheAttemptedProfiles_.clear();
		if(savedGroundCacheCandidate_) {
			savedGroundCacheStatus_.candidatePending = true;
			savedGroundCacheStatus_.cacheUsed = false;
			savedGroundCacheStatus_.rejectionReason = "NavigationUnavailableTerrain";
		}

		auto terminalizeRequest = [this](NavigationRequestHandle handle) {
			if(!requestStore_ || !handle.IsValid()) return;
			(void)requestStore_->Update(handle, [](NavigationRequestRecord& request) {
				// Cancellation is caller intent, not a terrain result. Preserve it when
				// a queued worker packet races this protected unavailable publication.
				if(request.outcome == NavigationOutcome::Cancelled) return;
				request.lifecycle = NavigationRequestLifecycle::Terminal;
				request.outcome = NavigationOutcome::UnavailableCoverage;
				request.reason = NavigationReason::MissingTerrain;
			});
		};
		for(const PendingRoute& pending : pendingRoutes_) terminalizeRequest(pending.handle);
		for(const PendingRoute& pending : inFlightRoutes_) terminalizeRequest(pending.handle);
		for(const DeferredRoute& deferred : deferredRoutes_) terminalizeRequest(deferred.handle);
		for(const PreparedRoute& prepared : preparedRoutes_) terminalizeRequest(prepared.handle);
		pendingRoutes_.clear();
		inFlightRoutes_.clear();
		deferredRoutes_.clear();
		preparedRoutes_.clear();
		if(routePreparationEpoch_ != std::numeric_limits<std::uint64_t>::max()) ++routePreparationEpoch_;
		if(routePreparationEpoch_ == 0U) routePreparationEpoch_ = 1U;
		const Ground::GroundPortalGraphSnapshot portals = portalGraph_.AcquireSnapshot();
		(void)portalGraph_.PublishBoundaryIntervals({}, portals.Revision());
		for(const NavigationRouteHandle route : requestRoutes_) {
			if(!route.IsValid() || !routeStore_ || routeStore_->Status(route) != NavigationHandleStatus::Alive) continue;
			const auto routeRecord = routeStore_->Read(route);
			if(!routeRecord.record) continue;
			const auto requestRecord = requestStore_
				? requestStore_->Read(routeRecord.record->request)
				: NavigationStoreReadResult<NavigationRequestRecord>{};
			const bool cancelled = requestRecord.record
				&& requestRecord.record->outcome == NavigationOutcome::Cancelled;
			if(cancelled) {
				// The caller already received a cancellation disposition. Clearing the
				// route payload below still makes its old geometry unusable, but never
				// rewrites that caller-owned terminal outcome as MissingTerrain.
				continue;
			}
			if(invalidations_.size() >= settings_.maxAdmittedRequests) invalidations_.erase(invalidations_.begin());
			invalidations_.push_back({nextInvalidationCursor_++, route.owner, route,
				NavigationReason::MissingTerrain, NavigationOutcome::UnavailableCoverage, 0U,
				lastProtectedBoundaryGeneration_});
			if(requestStore_) (void)requestStore_->Update(routeRecord.record->request,
				[](NavigationRequestRecord& request) {
					request.lifecycle = NavigationRequestLifecycle::Terminal;
					request.outcome = NavigationOutcome::UnavailableCoverage;
					request.reason = NavigationReason::MissingTerrain;
				});
			(void)routeStore_->Update(route, [](NavigationRouteRecord& record) {
				record.lifecycle = NavigationRequestLifecycle::Terminal;
				record.outcome = NavigationOutcome::UnavailableCoverage;
				record.reason = NavigationReason::MissingTerrain;
			});
		}
		publishedRouteData_.clear();
		requestRoutes_.clear();
		PublishAsyncBuildDiagnosticsLocked();
		const NavigationAsyncBuildStatus asyncStatus = asyncBuildCoordinator_.Status();
		contractTestWorkAvailable_.store(asyncStatus.currentCount != 0U || asyncStatus.retiredCount != 0U,
			std::memory_order_release);
		return true;
	}

	bool NavigationSystem::ConsumeFreshAutomaticCaptureRequest() {
		std::lock_guard<std::mutex> lock(mutex_);
		const bool requested = freshAutomaticCaptureRequested_;
		freshAutomaticCaptureRequested_ = false;
		return requested;
	}

	bool NavigationSystem::StageDynamicGroundSources(
		NavigationProfile profile,
		std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources) {
		if(!profile.Validate().accepted) return false;
		for(const auto& source : sources) if(!source) return false;
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire)) return false;
		const std::uint64_t buildKey = MakeGroundProfileBuildKey(profile);
		auto staged = std::find_if(dynamicSourceStages_.begin(), dynamicSourceStages_.end(),
			[buildKey](const DynamicSourceStage& stage) {
				return MakeGroundProfileBuildKey(stage.profile) == buildKey;
			});
		if(staged == dynamicSourceStages_.end()) dynamicSourceStages_.push_back({std::move(profile), std::move(sources)});
		else *staged = {std::move(profile), std::move(sources)};
		// An external complete source packet supersedes any still-provisional bridge
		// packet. Keeping the old provisional copy could otherwise publish against a
		// profile stage the caller has explicitly replaced at this boundary.
		pendingDynamicSourceStageReplacement_.reset();
		// A source bridge can arrive after a committed mutation in editor tooling.
		// Keep the scheduler awake so that retained dirty work is prepared next.
		if(!pendingDynamicChanges_.empty()) contractTestWorkAvailable_.store(true, std::memory_order_release);
		return true;
	}

	bool NavigationSystem::StageDynamicGroundSourceReplacements(
		std::vector<DynamicGroundSourceReplacement> replacements) {
		if(replacements.empty()) return false;
		for(const DynamicGroundSourceReplacement& replacement : replacements) {
			if(replacement.committedChangeSequence == 0U
				|| replacement.source.sourceId == EMPTY_UUID || replacement.source.sourceRevision == 0U
				|| replacement.source.geometryRevision == 0U || !replacement.source.enabled
				|| !replacement.snapshot) return false;
			const NavigationGeometrySnapshotDescriptor& descriptor = replacement.snapshot->Descriptor();
			if(descriptor.sourceUuid.value != replacement.source.sourceId
				|| descriptor.sourceRevision != replacement.source.sourceRevision
				|| descriptor.physicalReadyRevision < replacement.source.revisionEvidence.physicalPublicationGeneration) return false;
			if(replacement.source.identity.IsValid()
				&& (!replacement.source.revisionEvidence.IsComplete()
					|| replacement.source.identity.sourceId != replacement.source.sourceId
					|| ComputeNavigationEligibleSourceRevision(replacement.source.identity,
						replacement.source.revisionEvidence) != replacement.source.sourceRevision)) return false;
			for(const DynamicGroundSourceReplacement& earlier : replacements) {
				if(&earlier == &replacement) break;
				if(earlier.source.sourceId == replacement.source.sourceId) return false;
			}
		}

		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire) || dynamicSourceStages_.empty()) return false;
		for(const DynamicGroundSourceReplacement& replacement : replacements) {
			for(const DynamicSourceStage& stage : dynamicSourceStages_) {
				auto source = std::find_if(stage.sources.begin(), stage.sources.end(), [&replacement](const auto& current) {
					return current != nullptr && current->Descriptor().sourceUuid.value == replacement.source.sourceId;
				});
				if(source == stage.sources.end()) return false;
			}
			if(automaticGroundSourceStage_) {
				const auto source = std::find_if(automaticGroundSourceStage_->sources.begin(),
					automaticGroundSourceStage_->sources.end(), [&replacement](const auto& current) {
						return current != nullptr && current->Descriptor().sourceUuid.value == replacement.source.sourceId;
					});
				if(source == automaticGroundSourceStage_->sources.end()) return false;
				const auto row = std::find_if(automaticGroundSourceStage_->sourceIndex.begin(),
					automaticGroundSourceStage_->sourceIndex.end(), [&replacement](const NavigationSourceIndexRecord& current) {
						return current.sourceId == replacement.source.sourceId;
					});
				if(row == automaticGroundSourceStage_->sourceIndex.end()) return false;
			}
		}
		// Prepare copies first. A transaction receipt can still fail dirty admission,
		// so staging must not let a new snapshot become worker-visible ahead of its
		// matching protected root/source publication.
		std::vector<DynamicSourceStage> nextDynamicStages = dynamicSourceStages_;
		for(const DynamicGroundSourceReplacement& replacement : replacements) {
			for(DynamicSourceStage& stage : nextDynamicStages) {
				auto source = std::find_if(stage.sources.begin(), stage.sources.end(),
					[&replacement](const auto& current) {
						return current != nullptr
							&& current->Descriptor().sourceUuid.value == replacement.source.sourceId;
					});
				if(source == stage.sources.end()) return false;
				*source = replacement.snapshot;
			}
		}
		// On-demand profiles reuse this automatic packet after the original profiles
		// have published. Rebuild its eligibility index from the same copied rows and
		// snapshots before changing any live stage, so a later profile cannot combine
		// new collider geometry with an old immutable eligibility certificate.
		std::optional<AutomaticGroundSourceStage> nextAutomaticStage;
		if(automaticGroundSourceStage_) {
			nextAutomaticStage = *automaticGroundSourceStage_;
			for(const DynamicGroundSourceReplacement& replacement : replacements) {
				auto source = std::find_if(nextAutomaticStage->sources.begin(),
					nextAutomaticStage->sources.end(), [&replacement](const auto& current) {
						return current != nullptr
							&& current->Descriptor().sourceUuid.value == replacement.source.sourceId;
					});
				auto row = std::find_if(nextAutomaticStage->sourceIndex.begin(),
					nextAutomaticStage->sourceIndex.end(), [&replacement](const NavigationSourceIndexRecord& current) {
						return current.sourceId == replacement.source.sourceId;
					});
				if(source == nextAutomaticStage->sources.end() || row == nextAutomaticStage->sourceIndex.end())
					return false;
				*source = replacement.snapshot;
				*row = replacement.source;
			}
			std::vector<NavigationEligibleSourceCandidate> candidates;
			candidates.reserve(nextAutomaticStage->sources.size());
			for(const std::shared_ptr<const NavigationGeometrySnapshot>& source : nextAutomaticStage->sources) {
				if(!source) return false;
				const NavigationGeometrySnapshotDescriptor& descriptor = source->Descriptor();
				const auto row = std::find_if(nextAutomaticStage->sourceIndex.begin(),
					nextAutomaticStage->sourceIndex.end(), [&descriptor](const NavigationSourceIndexRecord& current) {
						return current.enabled && current.sourceId == descriptor.sourceUuid.value
							&& current.sourceRevision == descriptor.sourceRevision;
					});
				if(row == nextAutomaticStage->sourceIndex.end() || !row->identity.IsValid()
					|| !row->revisionEvidence.IsComplete()) return false;
				candidates.push_back({row->identity, row->revisionEvidence, source, true, true});
			}
			NavigationEligibleSourceIndexBuildResult indexed = BuildNavigationEligibleSourceIndex(candidates);
			if(!indexed.accepted) return false;
			nextAutomaticStage->eligibleSourceIndex =
				std::make_shared<const NavigationEligibleSourceIndex>(std::move(indexed.index));
		}
		PendingDynamicSourceStageReplacement pending{};
		pending.dynamicStages = std::move(nextDynamicStages);
		pending.automaticStage = std::move(nextAutomaticStage);
		pending.replacements.reserve(replacements.size());
		for(const DynamicGroundSourceReplacement& replacement : replacements)
			pending.replacements.push_back(replacement);
		// Replacing a prior provisional packet is safe: no earlier packet has become
		// visible yet, and the live stages still describe the published root.
		pendingDynamicSourceStageReplacement_ = std::move(pending);
		// A receipt may already be waiting for its exact replacement snapshot. Keep
		// the scheduler awake, but do not publish until the normal worker/commit path.
		if(!pendingDynamicChanges_.empty()) contractTestWorkAvailable_.store(true, std::memory_order_release);
		return true;
	}

	bool NavigationSystem::HasReadyCoverageForProfileLocked(const NavigationProfile& profile) const {
		if(!world_) return false;
		const std::shared_ptr<const NavigationWorldRoot> root = world_->PublishedRoot();
		if(!root || root->terrainState != NavigationTerrainState::Available) return false;
		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
		return std::any_of(root->coverage.begin(), root->coverage.end(),
			[profileBuildKey](const NavigationCoverageRecord& coverage) {
				return coverage.key.profileBuildKey == profileBuildKey
					&& coverage.state == NavigationCoverageState::Ready
					&& coverage.complete && coverage.coverageGeneration != 0U;
			});
	}

	std::optional<NavigationDirtyAdmissionExpansion>
	NavigationSystem::ComputeDirtyAdmissionExpansionLocked(
		std::span<const NavigationProfile> provisionalProfiles) const {
		// A receipt must dirty every currently active Recast border before the root
		// advertises the new source tuple. Keep values, rather than profile keys, so
		// a hash collision cannot shrink the envelope used for a larger profile.
		std::vector<NavigationProfile> activeProfiles;
		activeProfiles.reserve(settings_.maxActiveProfileBuilds);
		auto appendProfile = [this, &activeProfiles](const NavigationProfile& profile) {
			if(!profile.Validate().accepted) return false;
			const std::uint64_t profileKey = MakeGroundProfileBuildKey(profile);
			const auto existing = std::find_if(activeProfiles.begin(), activeProfiles.end(),
				[profileKey](const NavigationProfile& current) {
					return MakeGroundProfileBuildKey(current) == profileKey;
				});
			if(existing != activeProfiles.end()) return *existing == profile;
			if(activeProfiles.size() >= settings_.maxActiveProfileBuilds) return false;
			activeProfiles.push_back(profile);
			return true;
		};

		for(const DynamicSourceStage& stage : dynamicSourceStages_)
			if(!appendProfile(stage.profile)) return std::nullopt;
		for(const PendingGroundBuild& build : pendingGroundBuilds_)
			if(!appendProfile(build.profile)) return std::nullopt;
		for(const PreparedGroundBuild& build : preparedGroundBuilds_)
			if(!appendProfile(build.profile)) return std::nullopt;
		for(const PendingDynamicRebuild& build : pendingDynamicRebuilds_)
			if(!appendProfile(build.request.profile)) return std::nullopt;
		for(const PreparedDynamicRebuild& build : preparedDynamicRebuilds_)
			if(!appendProfile(build.profile)) return std::nullopt;
		if(activeDynamicBuildGroup_
			&& activeDynamicBuildGroup_->world == (world_ ? world_->Handle() : NavigationWorldHandle{})) {
			for(const PendingDynamicRebuild& build : activeDynamicBuildGroup_->pendingChildren)
				if(!appendProfile(build.request.profile)) return std::nullopt;
			for(const ActiveDynamicBuildChild& child : activeDynamicBuildGroup_->activeChildren) {
				if(child.result && child.result->dynamicPacket
					&& !appendProfile(child.result->dynamicPacket->request.profile)) return std::nullopt;
			}
		}
		if(preparedDynamicBuildGroup_
			&& preparedDynamicBuildGroup_->world == (world_ ? world_->Handle() : NavigationWorldHandle{})) {
			for(const PreparedDynamicRebuild& build : preparedDynamicBuildGroup_->successfulChildren)
				if(!appendProfile(build.profile)) return std::nullopt;
			for(const PreparedDynamicRebuild& build : preparedDynamicBuildGroup_->failedChildren)
				if(!appendProfile(build.profile)) return std::nullopt;
		}
		for(const PublishedGroundCacheCoverage& cache : publishedGroundCaches_)
			if(!appendProfile(cache.profile)) return std::nullopt;
		if(activeAsyncBuild_ && activeAsyncBuild_->result
			&& activeAsyncBuild_->identity.world == (world_ ? world_->Handle() : NavigationWorldHandle{})
			&& activeAsyncBuild_->identity.lifecycleEpoch == asyncBuildEpoch_) {
			// The worker reads these immutable packets but never mutates them. Include
			// their profiles while they own a build permit, otherwise a receipt accepted
			// mid-build could under-expand the still-active profile's Recast border.
			if(activeAsyncBuild_->result->groundPacket
				&& !appendProfile(activeAsyncBuild_->result->groundPacket->profile)) return std::nullopt;
			if(activeAsyncBuild_->result->dynamicPacket
				&& !appendProfile(activeAsyncBuild_->result->dynamicPacket->request.profile)) return std::nullopt;
		}
		for(const NavigationProfile& profile : provisionalProfiles)
			if(!appendProfile(profile)) return std::nullopt;

		const std::shared_ptr<const NavigationWorldRoot> root = world_ ? world_->PublishedRoot() : nullptr;
		if(root) {
			for(const NavigationCoverageRecord& coverage : root->coverage) {
				const auto known = std::find_if(activeProfiles.begin(), activeProfiles.end(), [&coverage](
					const NavigationProfile& profile) {
					return MakeGroundProfileBuildKey(profile) == coverage.key.profileBuildKey;
				});
				// A coverage key alone cannot recover its Recast radius/resolution. Do
				// not accept an under-expanded mutation when its retained profile value
				// was lost across a lifecycle or malformed import.
				if(known == activeProfiles.end()) return std::nullopt;
			}
		}

		if(activeProfiles.empty()) {
			// Before the first captured profile exists, preserve the established default
			// envelope. Later arbitrary profiles use the normal exact-profile full build.
			if(!appendProfile(profileResolver_.DefaultProfile())) return std::nullopt;
		}

		NavigationDirtyAdmissionExpansion result{};
		for(const NavigationProfile& profile : activeProfiles) {
			const std::optional<GroundTileBuildHalo> halo = ComputeGroundTileBuildHalo(profile);
			if(!halo || !std::isfinite(halo->horizontalMetres) || !std::isfinite(halo->verticalMetres)
				|| halo->horizontalMetres < 0.0 || halo->verticalMetres < 0.0) return std::nullopt;
			result.minimumHorizontalExpansionMetres = std::max(
				result.minimumHorizontalExpansionMetres, halo->horizontalMetres);
			result.minimumVerticalExpansionMetres = std::max(
				result.minimumVerticalExpansionMetres, halo->verticalMetres);
		}
		return result;
	}

	bool NavigationSystem::HasProfileBuildCapacityLocked(const NavigationProfile& profile) const {
		std::unordered_set<std::uint64_t> retainedProfiles;
		if(world_) {
			const std::shared_ptr<const NavigationWorldRoot> root = world_->PublishedRoot();
			if(root) {
				for(const NavigationCoverageRecord& coverage : root->coverage) {
					retainedProfiles.insert(coverage.key.profileBuildKey);
				}
			}
		}
		for(const PendingGroundBuild& build : pendingGroundBuilds_)
			retainedProfiles.insert(MakeGroundProfileBuildKey(build.profile));
		for(const PreparedGroundBuild& build : preparedGroundBuilds_)
			retainedProfiles.insert(MakeGroundProfileBuildKey(build.profile));
		// Dynamic work owns the same profile-scoped immutable geometry as a normal
		// on-demand build. Count it too, so a stream of distinct dirty profiles
		// cannot bypass the configured active-profile bound while a worker is busy.
		for(const DynamicSourceStage& stage : dynamicSourceStages_)
			retainedProfiles.insert(MakeGroundProfileBuildKey(stage.profile));
		for(const PendingDynamicRebuild& build : pendingDynamicRebuilds_)
			retainedProfiles.insert(MakeGroundProfileBuildKey(build.request.profile));
		for(const PreparedDynamicRebuild& build : preparedDynamicRebuilds_)
			retainedProfiles.insert(MakeGroundProfileBuildKey(build.profile));
		if(activeDynamicBuildGroup_
			&& activeDynamicBuildGroup_->world == (world_ ? world_->Handle() : NavigationWorldHandle{})) {
			for(const PendingDynamicRebuild& build : activeDynamicBuildGroup_->pendingChildren)
				retainedProfiles.insert(MakeGroundProfileBuildKey(build.request.profile));
			for(const ActiveDynamicBuildChild& child : activeDynamicBuildGroup_->activeChildren) {
				if(child.result && child.result->dynamicPacket)
					retainedProfiles.insert(MakeGroundProfileBuildKey(child.result->dynamicPacket->request.profile));
			}
		}
		if(preparedDynamicBuildGroup_
			&& preparedDynamicBuildGroup_->world == (world_ ? world_->Handle() : NavigationWorldHandle{})) {
			for(const PreparedDynamicRebuild& build : preparedDynamicBuildGroup_->successfulChildren)
				retainedProfiles.insert(MakeGroundProfileBuildKey(build.profile));
			for(const PreparedDynamicRebuild& build : preparedDynamicBuildGroup_->failedChildren)
				retainedProfiles.insert(MakeGroundProfileBuildKey(build.profile));
		}
		if(activeAsyncBuild_ && activeAsyncBuild_->result
			&& activeAsyncBuild_->identity.world == (world_ ? world_->Handle() : NavigationWorldHandle{})
			&& activeAsyncBuild_->identity.lifecycleEpoch == asyncBuildEpoch_) {
			if(activeAsyncBuild_->result->groundPacket)
				retainedProfiles.insert(MakeGroundProfileBuildKey(activeAsyncBuild_->result->groundPacket->profile));
			if(activeAsyncBuild_->result->dynamicPacket)
				retainedProfiles.insert(MakeGroundProfileBuildKey(activeAsyncBuild_->result->dynamicPacket->request.profile));
		}
		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
		return retainedProfiles.contains(profileBuildKey)
			|| retainedProfiles.size() < settings_.maxActiveProfileBuilds;
	}

	bool NavigationSystem::HasCurrentDynamicProfileRecoveryLocked(std::uint64_t profileBuildKey) const {
		if(!world_) return false;
		const auto isCurrentChange = [this](const NavigationDynamicSourceChange& change) {
			const NavigationGeneration sequence = ChangeSequence(change);
			return sequence != 0U
				&& dirtyIndex_.LatestChangeSequence() == sequence
				&& dirtyIndex_.LatestChangeSequence(change.source) == sequence;
		};
		const auto progressContainsProfile = [profileBuildKey](const DynamicRebuildProgress& progress) {
			return std::binary_search(progress.requiredProfileBuildKeys.begin(),
				progress.requiredProfileBuildKeys.end(), profileBuildKey)
				&& !std::binary_search(progress.publishedProfileBuildKeys.begin(),
					progress.publishedProfileBuildKeys.end(), profileBuildKey)
				&& !std::binary_search(progress.terminalFailedProfileBuildKeys.begin(),
					progress.terminalFailedProfileBuildKeys.end(), profileBuildKey);
		};

		// Before preparation constructs progress, a current admitted receipt plus an
		// exact copied stage is already sufficient recovery evidence. It avoids a
		// duplicate normal ground build in the one-frame receipt-to-worker gap.
		const bool hasMatchingStage = std::any_of(dynamicSourceStages_.begin(), dynamicSourceStages_.end(),
			[profileBuildKey](const DynamicSourceStage& stage) {
				return MakeGroundProfileBuildKey(stage.profile) == profileBuildKey;
			});
		const bool progressIsCurrent = dynamicRebuildProgress_
			&& dynamicRebuildProgress_->changeSequence != 0U
			&& dirtyIndex_.LatestChangeSequence() == dynamicRebuildProgress_->changeSequence;
		if(!progressIsCurrent) {
			return hasMatchingStage && std::any_of(pendingDynamicChanges_.begin(), pendingDynamicChanges_.end(),
				[&isCurrentChange](const NavigationDynamicSourceChange& change) { return isCurrentChange(change); });
		}

		const DynamicRebuildProgress& progress = *dynamicRebuildProgress_;
		if(!progressContainsProfile(progress)) return false;
		const auto matchesProgress = [&isCurrentChange, &progress](const NavigationDynamicSourceChange& change) {
			return ChangeSequence(change) == progress.changeSequence && isCurrentChange(change);
		};
		if(std::any_of(pendingDynamicChanges_.begin(), pendingDynamicChanges_.end(), matchesProgress)) return true;
		if(std::any_of(pendingDynamicRebuilds_.begin(), pendingDynamicRebuilds_.end(),
			[&matchesProgress, profileBuildKey](const PendingDynamicRebuild& build) {
				return MakeGroundProfileBuildKey(build.request.profile) == profileBuildKey
					&& matchesProgress(build.change);
			})) return true;
		if(std::any_of(preparedDynamicRebuilds_.begin(), preparedDynamicRebuilds_.end(),
			[&matchesProgress, profileBuildKey](const PreparedDynamicRebuild& build) {
				return MakeGroundProfileBuildKey(build.profile) == profileBuildKey
					&& matchesProgress(build.change);
			})) return true;
		if(activeDynamicBuildGroup_ && activeDynamicBuildGroup_->world == world_->Handle()
			&& matchesProgress(activeDynamicBuildGroup_->change)) {
			const auto matchesGroupProfile = [profileBuildKey](const PendingDynamicRebuild& build) {
				return MakeGroundProfileBuildKey(build.request.profile) == profileBuildKey;
			};
			if(std::any_of(activeDynamicBuildGroup_->pendingChildren.begin(),
				activeDynamicBuildGroup_->pendingChildren.end(), matchesGroupProfile)) return true;
			if(std::any_of(activeDynamicBuildGroup_->activeChildren.begin(),
				activeDynamicBuildGroup_->activeChildren.end(), [profileBuildKey](const ActiveDynamicBuildChild& child) {
					return child.result && child.result->dynamicPacket
						&& MakeGroundProfileBuildKey(child.result->dynamicPacket->request.profile) == profileBuildKey;
				})) return true;
		}
		if(preparedDynamicBuildGroup_ && preparedDynamicBuildGroup_->world == world_->Handle()
			&& matchesProgress(preparedDynamicBuildGroup_->change)) {
			return std::any_of(preparedDynamicBuildGroup_->successfulChildren.begin(),
				preparedDynamicBuildGroup_->successfulChildren.end(), [profileBuildKey](const PreparedDynamicRebuild& build) {
					return MakeGroundProfileBuildKey(build.profile) == profileBuildKey;
				});
		}
		return activeAsyncBuild_ && activeAsyncBuild_->result
			&& activeAsyncBuild_->identity.world == world_->Handle()
			&& activeAsyncBuild_->identity.lifecycleEpoch == asyncBuildEpoch_
			&& activeAsyncBuild_->identity.profileBuildKey == profileBuildKey
			&& activeAsyncBuild_->result->dynamicPacket
			&& matchesProgress(activeAsyncBuild_->result->dynamicPacket->change);
	}

	void NavigationSystem::RecordUnavailableProfileRecoveryLocked(const NavigationProfile& profile) {
		// A dirty profile without a current matching recovery must fail visibly. A
		// request therefore uses the immutable dirty root's normal fail-closed query
		// path instead of joining a deferred queue with no owner for recovery work.
		diagnostics_.Append({NavigationDiagnosticSeverity::Warning,
			NavigationMeasurementReason::MeasurementUnavailable, lastProtectedBoundaryGeneration_, 0U,
			"navigation.profile_recovery_unavailable", std::to_string(MakeGroundProfileBuildKey(profile))});
	}

	NavigationSystem::ProfileCoverageAdmission
	NavigationSystem::ClassifyProfileCoverageAdmissionLocked(
		const Ground::GroundRouteQuery::Request& request) {
		const NavigationProfile& profile = request.profile;
		if(!profile.Validate().accepted || !world_) return ProfileCoverageAdmission::Unavailable;
		const std::shared_ptr<const NavigationWorldRoot> root = world_->PublishedRoot();
		if(!root || root->terrainState != NavigationTerrainState::Available) return ProfileCoverageAdmission::Unavailable;

		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
		if(HasReadyCoverageForProfileLocked(profile)) {
			const auto endpointIsDirty = [&root](const EndpointCoverageCandidates& candidates) {
				auto matchesDirtyCoverage = [&root](const NavigationCoverageKey& key) {
					return std::any_of(root->coverage.begin(), root->coverage.end(), [&key](
						const NavigationCoverageRecord& coverage) {
						return coverage.key == key && coverage.state == NavigationCoverageState::Dirty;
					});
				};
				return matchesDirtyCoverage(candidates.primary)
					|| (candidates.boundaryAlternative && matchesDirtyCoverage(*candidates.boundaryAlternative));
			};
			const std::optional<EndpointCoverageCandidates> start = CoverageCandidatesForEndpoint(
				request.start, profile, profileBuildKey);
			const std::optional<EndpointCoverageCandidates> destination = CoverageCandidatesForEndpoint(
				request.destination, profile, profileBuildKey);
			// An unlocalizable endpoint preserves the old queued worker outcome. Only a
			// known dirty canonical endpoint with an owned recovery may defer admission.
			if((start && endpointIsDirty(*start)) || (destination && endpointIsDirty(*destination))) {
				if(HasCurrentDynamicProfileRecoveryLocked(profileBuildKey))
					return ProfileCoverageAdmission::PendingRecovery;
			}
			return ProfileCoverageAdmission::Ready;
		}

		const auto matchesProfile = [profileBuildKey](const auto& build) {
			return MakeGroundProfileBuildKey(build.profile) == profileBuildKey;
		};
		const bool normalBuildInFlight = std::any_of(pendingGroundBuilds_.begin(), pendingGroundBuilds_.end(), matchesProfile)
			|| std::any_of(preparedGroundBuilds_.begin(), preparedGroundBuilds_.end(), matchesProfile)
			|| (activeAsyncBuild_ && activeAsyncBuild_->result
				&& activeAsyncBuild_->identity.world == world_->Handle()
				&& activeAsyncBuild_->identity.lifecycleEpoch == asyncBuildEpoch_
				&& activeAsyncBuild_->identity.profileBuildKey == profileBuildKey
				&& activeAsyncBuild_->result->groundPacket.has_value());
		if(normalBuildInFlight) return ProfileCoverageAdmission::PendingRecovery;

		const bool profileExists = std::any_of(root->coverage.begin(), root->coverage.end(),
			[profileBuildKey](const NavigationCoverageRecord& coverage) {
				return coverage.key.profileBuildKey == profileBuildKey;
			});
		const bool profileDirty = std::any_of(root->coverage.begin(), root->coverage.end(),
			[profileBuildKey](const NavigationCoverageRecord& coverage) {
				return coverage.key.profileBuildKey == profileBuildKey
					&& coverage.state == NavigationCoverageState::Dirty;
			});
		if(profileDirty) {
			if(HasCurrentDynamicProfileRecoveryLocked(profileBuildKey))
				return ProfileCoverageAdmission::PendingRecovery;
			RecordUnavailableProfileRecoveryLocked(profile);
			return ProfileCoverageAdmission::Unavailable;
		}
		// A non-ready existing record has no safe automatic recovery unless it is
		// represented by a real copied normal build above. Do not search it as if
		// it were complete or start an unrelated rebuild from stale geometry.
		if(profileExists) return ProfileCoverageAdmission::Unavailable;

		if(!automaticGroundSourceStage_ || !automaticGroundSourceStage_->eligibleSourceIndex
			|| !automaticGroundSourceStage_->eligibleSourceIndex->HasCurrentTerrain()
			|| automaticGroundSourceStage_->terrainPublicationGeneration == 0U
			|| automaticGroundSourceStage_->profileConfigurationRevision != profileConfigurationRevision_
			|| !HasProfileBuildCapacityLocked(profile)) return ProfileCoverageAdmission::Unavailable;

		const AutomaticGroundSourceStage& stage = *automaticGroundSourceStage_;
		// The staged packet was captured as one unit. Reusing only its immutable
		// values guarantees the new profile cannot mix terrain or source revisions.
		pendingGroundBuilds_.push_back({profile, stage.sources, stage.sourceIndex,
			stage.eligibleSourceIndex, stage.terrainPublicationGeneration,
			stage.profileConfigurationRevision});
		auto staged = std::find_if(dynamicSourceStages_.begin(), dynamicSourceStages_.end(),
			[profileBuildKey](const DynamicSourceStage& current) {
				return MakeGroundProfileBuildKey(current.profile) == profileBuildKey;
			});
		if(staged == dynamicSourceStages_.end()) dynamicSourceStages_.push_back({profile, stage.sources});
		else *staged = {profile, stage.sources};
		contractTestWorkAvailable_.store(true, std::memory_order_release);
		return ProfileCoverageAdmission::PendingRecovery;
	}

	void NavigationSystem::QueueRouteAgainstPublishedWorldLocked(NavigationRequestHandle handle,
		std::uint64_t intentGeneration, Ground::GroundRouteQuery::Request request) {
		// Admission snapshots the completed publication only after matching profile
		// coverage exists. The worker can therefore remain wholly scene-independent.
		const std::shared_ptr<const NavigationWorldRoot> worldRoot = world_ ? world_->PublishedRoot() : nullptr;
		NavigationObservationEvidence observation{};
		if(requestStore_) {
			const auto existing = requestStore_->Read(handle);
			if(existing.record) observation = existing.record->observation;
		}
		// Preserve endpoint revisions from the original request, while pairing the
		// route packet with the exact root it will search. The request record is
		// updated only after endpoint validation at protected result publication.
		if(worldRoot) {
			observation.terrainPublicationGeneration = worldRoot->terrainPublicationGeneration;
			observation.worldPublicationGeneration = worldRoot->rootGeneration;
			observation.sourceIndexRevision = worldRoot->sourceIndexRevision;
		}
		const std::uint64_t sequence = nextRoutePreparationSequence_++;
		if(nextRoutePreparationSequence_ == 0U) nextRoutePreparationSequence_ = 1U;
		const RouteClock::time_point admittedAt = RouteClock::now();
		pendingRoutes_.push_back({sequence, routePreparationEpoch_, handle, intentGeneration, std::move(request),
			admittedAt, false, {}, observation, worldRoot, publishedShards_, portalGraph_.AcquireSnapshot()});
	}

	void NavigationSystem::BeginAutomaticSourceIndexReplacementLocked(
		std::shared_ptr<const NavigationEligibleSourceIndex> previous,
		std::shared_ptr<const NavigationEligibleSourceIndex> replacement,
		std::span<const std::uint64_t> requiredProfileBuildKeys) {
		if(!replacement) return;
		AutomaticSourceIndexReplacement transition{};
		transition.replacementRevision = replacement->Revision();
		transition.requiredProfileBuildKeys.assign(requiredProfileBuildKeys.begin(), requiredProfileBuildKeys.end());
		std::sort(transition.requiredProfileBuildKeys.begin(), transition.requiredProfileBuildKeys.end());
		transition.requiredProfileBuildKeys.erase(std::unique(transition.requiredProfileBuildKeys.begin(),
			transition.requiredProfileBuildKeys.end()), transition.requiredProfileBuildKeys.end());

		const bool extendsSameReplacement = automaticSourceIndexReplacement_
			&& automaticSourceIndexReplacement_->replacementRevision == transition.replacementRevision;
		if(extendsSameReplacement) {
			// One protected capture may add a profile after another profile from the
			// same copied index is queued. Preserve completed profile evidence, but
			// require the newly queued profile before any route refresh can occur.
			transition.changedGeometry = automaticSourceIndexReplacement_->changedGeometry;
			transition.publishedProfileBuildKeys = automaticSourceIndexReplacement_->publishedProfileBuildKeys;
			transition.affectsEveryRoute = automaticSourceIndexReplacement_->affectsEveryRoute;
			transition.requiredProfileBuildKeys.insert(transition.requiredProfileBuildKeys.end(),
				automaticSourceIndexReplacement_->requiredProfileBuildKeys.begin(),
				automaticSourceIndexReplacement_->requiredProfileBuildKeys.end());
			std::sort(transition.requiredProfileBuildKeys.begin(), transition.requiredProfileBuildKeys.end());
			transition.requiredProfileBuildKeys.erase(std::unique(transition.requiredProfileBuildKeys.begin(),
				transition.requiredProfileBuildKeys.end()), transition.requiredProfileBuildKeys.end());
		} else if(automaticSourceIndexReplacement_) {
			// A later capture supersedes an uncommitted one. A route already retired for
			// A->B must never be revived merely because B->C moves the same source back,
			// so union the old conservative geometry certificate with the new delta.
			transition.changedGeometry = automaticSourceIndexReplacement_->changedGeometry;
			transition.affectsEveryRoute = automaticSourceIndexReplacement_->affectsEveryRoute;
		}
		const auto appendChangedGeometry = [this, &transition](
			const std::shared_ptr<const NavigationGeometrySnapshot>& geometry) {
			if(!geometry || transition.affectsEveryRoute) {
				transition.affectsEveryRoute = true;
				return;
			}
			if(std::find(transition.changedGeometry.begin(), transition.changedGeometry.end(), geometry)
				!= transition.changedGeometry.end()) return;
			if(transition.changedGeometry.size() >= settings_.maxAdmittedRequests) {
				// Rapid capture churn must remain bounded. Once the immutable transition
				// certificate reaches its cap, fail every retained route closed instead
				// of silently discarding older changed geometry.
				transition.changedGeometry.clear();
				transition.affectsEveryRoute = true;
				return;
			}
			transition.changedGeometry.push_back(geometry);
		};
		if(!previous) {
			// There is no previous immutable index to compare. A pre-bootstrap route
			// cannot prove independence from the first automatic authority.
			transition.affectsEveryRoute = true;
		} else {
			const std::vector<NavigationEligibleSourceRecord>& oldRecords = previous->Records();
			const std::vector<NavigationEligibleSourceRecord>& newRecords = replacement->Records();
			for(const NavigationEligibleSourceRecord& oldRecord : oldRecords) {
				const auto replacementRecord = std::find_if(newRecords.begin(), newRecords.end(),
					[&oldRecord](const NavigationEligibleSourceRecord& candidate) {
						return candidate.identity == oldRecord.identity;
					});
				if(replacementRecord != newRecords.end() && SameCapturedSource(oldRecord, *replacementRecord)) {
					continue;
				}
				appendChangedGeometry(oldRecord.geometry);
				if(replacementRecord != newRecords.end()) {
					appendChangedGeometry(replacementRecord->geometry);
				}
			}
			for(const NavigationEligibleSourceRecord& newRecord : newRecords) {
				const bool existed = std::any_of(oldRecords.begin(), oldRecords.end(),
					[&newRecord](const NavigationEligibleSourceRecord& candidate) {
						return candidate.identity == newRecord.identity;
					});
				if(existed) continue;
				appendChangedGeometry(newRecord.geometry);
			}
			// A different revision without a selected-geometry delta can result from
			// capture exclusions. It cannot certify that an existing route is safe.
			if(transition.changedGeometry.empty()) transition.affectsEveryRoute = true;
		}
		automaticSourceIndexReplacement_ = std::move(transition);

		if(!requestStore_ || !routeStore_) return;
		for(const NavigationRouteHandle route : requestRoutes_) {
			if(!route.IsValid() || routeStore_->Status(route) != NavigationHandleStatus::Alive
				|| route.slotIndex >= publishedRouteData_.size()) continue;
			const NavigationStoreReadResult<NavigationRouteRecord> routeRecord = routeStore_->Read(route);
			const NavigationStoreReadResult<NavigationRequestRecord> request = routeRecord.record
				? requestStore_->Read(routeRecord.record->request) : NavigationStoreReadResult<NavigationRequestRecord>{};
			if(!routeRecord.record || !request.record
				|| !RouteIsAffectedByAutomaticSourceReplacementLocked(
					publishedRouteData_[route.slotIndex], request.record->profile)) continue;
			(void)requestStore_->Update(routeRecord.record->request, [](NavigationRequestRecord& record) {
				record.lifecycle = NavigationRequestLifecycle::Terminal;
				record.outcome = NavigationOutcome::Stale;
				record.reason = NavigationReason::DirtyCoverage;
			});
			(void)routeStore_->Update(route, [](NavigationRouteRecord& record) {
				record.lifecycle = NavigationRequestLifecycle::Terminal;
				record.outcome = NavigationOutcome::Stale;
				record.reason = NavigationReason::DirtyCoverage;
			});
			publishedRouteData_[route.slotIndex].valid = false;
			if(invalidations_.size() >= settings_.maxAdmittedRequests) invalidations_.erase(invalidations_.begin());
			invalidations_.push_back({nextInvalidationCursor_++, route.owner, route,
				NavigationReason::DirtyCoverage, NavigationOutcome::Stale,
				automaticSourceIndexReplacement_->replacementRevision, lastProtectedBoundaryGeneration_});
		}
	}

	bool NavigationSystem::RouteIsAffectedByAutomaticSourceReplacementLocked(
		const Ground::GroundRouteQuery::RetainedRoute& route,
		const NavigationProfile& profile) const {
		if(!automaticSourceIndexReplacement_) return false;
		const AutomaticSourceIndexReplacement& transition = *automaticSourceIndexReplacement_;
		return transition.affectsEveryRoute || std::any_of(transition.changedGeometry.begin(),
			transition.changedGeometry.end(), [&route, &profile](const auto& source) {
				return !source || RouteIntersectsCapturedSource(route, profile, *source);
			});
	}

	void NavigationSystem::RefreshUnaffectedRoutesAfterAutomaticSourcePublicationLocked(
		NavigationGeneration publishedSourceIndexRevision, std::uint64_t publishedProfileBuildKey) {
		if(!automaticSourceIndexReplacement_
			|| automaticSourceIndexReplacement_->replacementRevision != publishedSourceIndexRevision
			|| !world_ || !requestStore_ || !routeStore_) return;
		const std::shared_ptr<const NavigationWorldRoot> published = world_->PublishedRoot();
		if(!published || published->sourceIndexRevision != publishedSourceIndexRevision) return;
		AutomaticSourceIndexReplacement& transition = *automaticSourceIndexReplacement_;
		if(!std::binary_search(transition.requiredProfileBuildKeys.begin(),
			transition.requiredProfileBuildKeys.end(), publishedProfileBuildKey)) return;
		if(!std::binary_search(transition.publishedProfileBuildKeys.begin(),
			transition.publishedProfileBuildKeys.end(), publishedProfileBuildKey)) {
			transition.publishedProfileBuildKeys.insert(std::upper_bound(transition.publishedProfileBuildKeys.begin(),
				transition.publishedProfileBuildKeys.end(), publishedProfileBuildKey), publishedProfileBuildKey);
		}
		if(!std::includes(transition.publishedProfileBuildKeys.begin(), transition.publishedProfileBuildKeys.end(),
			transition.requiredProfileBuildKeys.begin(), transition.requiredProfileBuildKeys.end())) return;
		for(const NavigationRouteHandle route : requestRoutes_) {
			if(!route.IsValid() || route.slotIndex >= publishedRouteData_.size()
				|| !publishedRouteData_[route.slotIndex].IsValid()
				|| routeStore_->Status(route) != NavigationHandleStatus::Alive) continue;
			const NavigationStoreReadResult<NavigationRouteRecord> routeRecord = routeStore_->Read(route);
			const NavigationStoreReadResult<NavigationRequestRecord> request = routeRecord.record
				? requestStore_->Read(routeRecord.record->request) : NavigationStoreReadResult<NavigationRequestRecord>{};
			const std::uint64_t routeProfileBuildKey = request.record
				? MakeGroundProfileBuildKey(request.record->profile) : 0U;
			const bool profilePublished = std::binary_search(transition.publishedProfileBuildKeys.begin(),
				transition.publishedProfileBuildKeys.end(), routeProfileBuildKey)
				&& std::any_of(published->coverage.begin(), published->coverage.end(),
					[routeProfileBuildKey](const NavigationCoverageRecord& coverage) {
						return coverage.key.profileBuildKey == routeProfileBuildKey
							&& coverage.state == NavigationCoverageState::Ready && coverage.complete;
					});
			if(!routeRecord.record || !request.record || !profilePublished
				|| request.record->outcome != NavigationOutcome::Complete
				|| RouteIsAffectedByAutomaticSourceReplacementLocked(
					publishedRouteData_[route.slotIndex], request.record->profile)) continue;
			(void)routeStore_->Update(route, [&published](NavigationRouteRecord& record) {
				record.observation.terrainPublicationGeneration = published->terrainPublicationGeneration;
				record.observation.worldPublicationGeneration = published->rootGeneration;
				record.observation.sourceIndexRevision = published->sourceIndexRevision;
			});
		}
		// The successful root exchange is the only point at which a preserved route
		// may observe the new evidence. A rejected candidate leaves this certificate
		// intact, so it can never revive an already invalidated path.
		automaticSourceIndexReplacement_.reset();
	}

	void NavigationSystem::RefreshUnaffectedRoutesAfterDynamicSourcePublicationLocked(
		const std::shared_ptr<const NavigationWorldRoot>& previousRoot) {
		if(!world_ || !requestStore_ || !routeStore_ || !previousRoot) return;
		const std::shared_ptr<const NavigationWorldRoot> published = world_->PublishedRoot();
		if(!published) return;
		const NavigationObservationEvidence previousEvidence{
			lastProtectedBoundaryGeneration_, 0U, 0U, previousRoot->terrainPublicationGeneration,
			previousRoot->rootGeneration, previousRoot->sourceIndexRevision};
		for(const NavigationRouteHandle route : requestRoutes_) {
			if(!route.IsValid() || route.slotIndex >= publishedRouteData_.size()
				|| !publishedRouteData_[route.slotIndex].IsValid()
				|| routeStore_->Status(route) != NavigationHandleStatus::Alive) continue;
			const NavigationStoreReadResult<NavigationRouteRecord> routeRecord = routeStore_->Read(route);
			const NavigationStoreReadResult<NavigationRequestRecord> request = routeRecord.record
				? requestStore_->Read(routeRecord.record->request) : NavigationStoreReadResult<NavigationRequestRecord>{};
			if(!routeRecord.record || !request.record
				|| (request.record->outcome != NavigationOutcome::Complete
					&& request.record->outcome != NavigationOutcome::Partial)) continue;
			const std::uint64_t profileKey = MakeGroundProfileBuildKey(request.record->profile);
			const bool profileRetained = std::any_of(previousRoot->coverage.begin(), previousRoot->coverage.end(),
				[profileKey](const NavigationCoverageRecord& coverage) {
					return coverage.key.profileBuildKey == profileKey;
				});
			if(!profileRetained
				|| !ValidateNavigationRouteObservation(request.record->observation, previousEvidence).usable
				|| !ValidateNavigationRouteObservation(routeRecord.record->observation, previousEvidence).usable) continue;
			// Intersecting paths were terminalized before this root exchange. Never
			// revive them here: this helper certifies only routes still independently
			// complete/partial after the protected dirty-receipt decision.
			const auto refreshObservation = [&published](NavigationObservationEvidence& observation) {
				observation.terrainPublicationGeneration = published->terrainPublicationGeneration;
				observation.worldPublicationGeneration = published->rootGeneration;
				observation.sourceIndexRevision = published->sourceIndexRevision;
			};
			(void)requestStore_->Update(routeRecord.record->request, [&refreshObservation](NavigationRequestRecord& record) {
				refreshObservation(record.observation);
			});
			(void)routeStore_->Update(route, [&refreshObservation](NavigationRouteRecord& record) {
				refreshObservation(record.observation);
			});
		}
	}

	void NavigationSystem::AppendDynamicRouteTransitionLocked(const NavigationWorldRoot& previous,
		const NavigationWorldRoot& published, std::vector<DynamicRouteAffectedBounds> affectedBounds,
		std::vector<std::uint64_t> topologyPreservedProfileBuildKeys) {
		// A route may cross only a small number of protected publications while it is
		// in flight. Retaining one certificate per retired-root slot bounds memory and
		// makes an older, unrepresented publication fail closed at result publication.
		constexpr std::size_t kMaximumRetainedTransitions = 64U;
		constexpr std::size_t kMaximumAffectedBoundsPerTransition = 4096U;
		constexpr std::size_t kMaximumTopologyPreservedProfiles = 64U;
		while(dynamicRouteTransitions_.size() >= kMaximumRetainedTransitions)
			dynamicRouteTransitions_.pop_front();

		DynamicRouteTransition transition{};
		transition.world = published.world;
		transition.fromRootGeneration = previous.rootGeneration;
		transition.toRootGeneration = published.rootGeneration;
		transition.fromTerrainPublicationGeneration = previous.terrainPublicationGeneration;
		transition.toTerrainPublicationGeneration = published.terrainPublicationGeneration;
		transition.fromSourceIndexRevision = previous.sourceIndexRevision;
		transition.toSourceIndexRevision = published.sourceIndexRevision;
		const bool boundsFit = !affectedBounds.empty()
			&& affectedBounds.size() <= kMaximumAffectedBoundsPerTransition
			&& affectedBounds.size() <= settings_.maxAdmittedRequests;
		if(!boundsFit) {
			// clear() retains the caller's capacity. Drop it before journaling a failed
			// certificate so an oversized dirty packet cannot evade the bounded-memory
			// contract merely by becoming an empty vector.
			std::vector<DynamicRouteAffectedBounds>{}.swap(affectedBounds);
		}
		std::sort(topologyPreservedProfileBuildKeys.begin(), topologyPreservedProfileBuildKeys.end());
		topologyPreservedProfileBuildKeys.erase(std::unique(topologyPreservedProfileBuildKeys.begin(),
			topologyPreservedProfileBuildKeys.end()), topologyPreservedProfileBuildKeys.end());
		const bool topologyFits = !topologyPreservedProfileBuildKeys.empty()
			&& topologyPreservedProfileBuildKeys.size() <= kMaximumTopologyPreservedProfiles
			&& topologyPreservedProfileBuildKeys.size() <= settings_.maxActiveProfileBuilds;
		if(!topologyFits) {
			// As with dirty bounds, discard capacity as well as contents. A malformed
			// profile list must never make an otherwise bounded root journal grow.
			std::vector<std::uint64_t>{}.swap(topologyPreservedProfileBuildKeys);
		}
		transition.affectedBounds = std::move(affectedBounds);
		transition.topologyPreservedProfileBuildKeys = std::move(topologyPreservedProfileBuildKeys);
		// A certificate must describe exactly one same-world publication and at least
		// one copied dirty range or an exact unchanged-profile proof. Terrain replacement
		// has different support semantics, so it deliberately cannot be rebased here.
		transition.known = previous.world == published.world
			&& previous.rootGeneration != 0U
			&& published.rootGeneration == previous.rootGeneration + 1U
			&& previous.terrainPublicationGeneration != 0U
			&& previous.terrainPublicationGeneration == published.terrainPublicationGeneration
			&& previous.sourceIndexRevision != 0U && published.sourceIndexRevision != 0U
			&& (boundsFit || topologyFits);
		try {
			dynamicRouteTransitions_.push_back(std::move(transition));
		}
		catch(...) {
			// The authoritative root is already visible at every call site. If this
			// bounded diagnostic certificate cannot allocate, omit it and let later
			// prepared routes fail closed instead of throwing through publication.
		}
	}

	std::vector<std::uint64_t> NavigationSystem::CollectTopologyPreservedProfilesLocked(
		const NavigationWorldRoot& previous, const NavigationWorldRoot& published,
		const std::vector<std::shared_ptr<const DetourGroundShardGeneration>>& previousShards,
		const std::vector<std::shared_ptr<const DetourGroundShardGeneration>>& nextShards) const {
		constexpr std::size_t kMaximumTopologyPreservedProfiles = 64U;
		// Coverage/shard equality is meaningful only inside the same physical world
		// evidence tuple. A terrain or source-index change requires the ordinary dirty
		// bounds certificate even if an unrelated profile happened to keep its shards.
		if(previous.world != published.world || previous.terrainPublicationGeneration == 0U
			|| previous.terrainPublicationGeneration != published.terrainPublicationGeneration
			|| previous.sourceIndexRevision == 0U
			|| previous.sourceIndexRevision != published.sourceIndexRevision) return {};
		const std::size_t maximumProfiles = std::min<std::size_t>(settings_.maxActiveProfileBuilds,
			kMaximumTopologyPreservedProfiles);
		std::vector<std::uint64_t> candidates;
		candidates.reserve(maximumProfiles);
		for(const NavigationCoverageRecord& coverage : previous.coverage) {
			const std::uint64_t profileKey = coverage.key.profileBuildKey;
			if(std::find(candidates.begin(), candidates.end(), profileKey) == candidates.end()) {
				if(candidates.size() >= maximumProfiles) return {};
				candidates.push_back(profileKey);
			}
		}

		std::vector<std::uint64_t> preserved;
		preserved.reserve(candidates.size());
		for(const std::uint64_t profileKey : candidates) {
			const auto coverageMatches = [&previous, &published, profileKey]() {
				std::size_t previousCount = 0U;
				std::size_t publishedCount = 0U;
				for(const NavigationCoverageRecord& before : previous.coverage) {
					if(before.key.profileBuildKey != profileKey) continue;
					if(before.state != NavigationCoverageState::Ready || !before.complete
						|| before.coverageGeneration == 0U) return false;
					++previousCount;
					const auto after = std::find_if(published.coverage.begin(), published.coverage.end(),
						[&before](const NavigationCoverageRecord& current) { return current.key == before.key; });
					if(after == published.coverage.end() || after->coverageGeneration != before.coverageGeneration
						|| after->state != before.state || after->complete != before.complete
						|| after->chargedBytes != before.chargedBytes) return false;
				}
				for(const NavigationCoverageRecord& after : published.coverage)
					if(after.key.profileBuildKey == profileKey) ++publishedCount;
				return previousCount != 0U && previousCount == publishedCount;
			};
			const auto shardsMatch = [&previous, &previousShards, &nextShards, profileKey]() {
				std::size_t previousCount = 0U;
				std::size_t nextCount = 0U;
				for(const NavigationCoverageRecord& coverage : previous.coverage) {
					if(coverage.key.profileBuildKey != profileKey) continue;
					const GroundShardKey expected{coverage.key.shardX, coverage.key.shardZ,
						coverage.key.verticalBand, profileKey};
					const auto before = std::find_if(previousShards.begin(), previousShards.end(),
						[&expected, &coverage](const auto& shard) {
							return shard && shard->reference.shard == expected
								&& shard->reference.generation == coverage.coverageGeneration;
						});
					if(before == previousShards.end()) return false;
					++previousCount;
					const auto after = std::find_if(nextShards.begin(), nextShards.end(),
						[&before](const auto& current) {
							return current && current->reference.shard == (*before)->reference.shard
								&& current->reference.generation == (*before)->reference.generation
								&& current.get() == before->get();
						});
					if(after == nextShards.end()) return false;
				}
				for(const auto& after : nextShards)
					if(after && after->reference.shard.profileBuildKey == profileKey) ++nextCount;
				return previousCount != 0U && previousCount == nextCount;
			};
			// Portal construction is deterministic from immutable shard intervals and
			// never joins different profile keys. Equal ready coverage plus identical
			// shard objects therefore proves this profile's portal topology is unchanged.
			if(coverageMatches() && shardsMatch()) preserved.push_back(profileKey);
		}
		return preserved;
	}

	std::vector<NavigationSystem::DynamicRouteAffectedBounds>
	NavigationSystem::FindDynamicRouteAffectedBoundsLocked(
		NavigationUuid source, NavigationGeneration changeSequence) const {
		constexpr std::size_t kMaximumCopiedBounds = 4096U;
		std::vector<DynamicRouteAffectedBounds> result;
		for(auto transition = dynamicRouteTransitions_.rbegin();
			transition != dynamicRouteTransitions_.rend(); ++transition) {
			for(const DynamicRouteAffectedBounds& affected : transition->affectedBounds) {
				if(affected.source == source && affected.changeSequence == changeSequence) {
					// A certificate may be repeated once for each profile publication. Do
					// not let a malformed receipt fan this temporary vector beyond the same
					// fixed transition bound used by the retained journal.
					if(result.size() >= kMaximumCopiedBounds) {
						std::vector<DynamicRouteAffectedBounds>{}.swap(result);
						return result;
					}
					result.push_back(affected);
				}
			}
			// One committed receipt owns each source/sequence tuple. Searching an older
			// certificate after finding it would duplicate the same swept range.
			if(!result.empty()) break;
		}
		return result;
	}

	NavigationSystem::DynamicRouteRebaseResult
	NavigationSystem::TryRebasePreparedRouteObservationLocked(
		NavigationWorldHandle capturedWorld, NavigationGeneration capturedRootGeneration,
		const NavigationObservationEvidence& capturedObservation, const NavigationProfile& profile,
		const Ground::GroundRouteQuery::RetainedRoute& route,
		const NavigationWorldRoot& currentRoot, NavigationObservationEvidence& rebasedObservation,
		DynamicRouteRebaseUnknownReason& unknownReason) const {
		unknownReason = DynamicRouteRebaseUnknownReason::None;
		if(!capturedWorld.IsValid() || capturedWorld != currentRoot.world
			|| capturedRootGeneration == 0U
			|| capturedObservation.worldPublicationGeneration != capturedRootGeneration
			|| currentRoot.rootGeneration <= capturedRootGeneration) {
			unknownReason = DynamicRouteRebaseUnknownReason::InvalidInput;
			return DynamicRouteRebaseResult::Unknown;
		}
		if(capturedObservation.terrainPublicationGeneration == 0U
			|| capturedObservation.terrainPublicationGeneration != currentRoot.terrainPublicationGeneration) {
			unknownReason = DynamicRouteRebaseUnknownReason::TerrainMismatch;
			return DynamicRouteRebaseResult::Unknown;
		}

		NavigationGeneration cursor = capturedRootGeneration;
		NavigationGeneration sourceIndexRevision = capturedObservation.sourceIndexRevision;
		const std::uint64_t routeProfileBuildKey = MakeGroundProfileBuildKey(profile);
		while(cursor < currentRoot.rootGeneration) {
			const auto transition = std::find_if(dynamicRouteTransitions_.begin(), dynamicRouteTransitions_.end(),
				[capturedWorld, cursor](const DynamicRouteTransition& current) {
					return current.world == capturedWorld && current.fromRootGeneration == cursor;
				});
			if(transition == dynamicRouteTransitions_.end()) {
				unknownReason = DynamicRouteRebaseUnknownReason::MissingLink;
				return DynamicRouteRebaseResult::Unknown;
			}
			if(!transition->known || transition->toRootGeneration != cursor + 1U
				|| transition->fromTerrainPublicationGeneration != capturedObservation.terrainPublicationGeneration
				|| transition->toTerrainPublicationGeneration != capturedObservation.terrainPublicationGeneration) {
				unknownReason = DynamicRouteRebaseUnknownReason::InvalidLink;
				return DynamicRouteRebaseResult::Unknown;
			}
			if(transition->fromSourceIndexRevision != sourceIndexRevision) {
				unknownReason = DynamicRouteRebaseUnknownReason::SourceChainMismatch;
				return DynamicRouteRebaseResult::Unknown;
			}
			const bool topologyPreserved = std::binary_search(
				transition->topologyPreservedProfileBuildKeys.begin(),
				transition->topologyPreservedProfileBuildKeys.end(), routeProfileBuildKey);
			if(!topologyPreserved && transition->affectedBounds.empty()) {
				// This link exists only to certify other profiles through an additive
				// publication. A route for an unlisted profile has no spatial proof.
				unknownReason = DynamicRouteRebaseUnknownReason::InvalidLink;
				return DynamicRouteRebaseResult::Unknown;
			}
			if(topologyPreserved && transition->affectedBounds.empty()) {
				// Equal profile coverage and the exact same immutable shard objects prove
				// this profile's portal topology did not change. Empty bounds identifies an
				// additive profile publication; a real source change still uses its swept
				// bounds even when another profile's shard tuple happens to be unchanged.
				cursor = transition->toRootGeneration;
				sourceIndexRevision = transition->toSourceIndexRevision;
				continue;
			}
			for(const DynamicRouteAffectedBounds& affected : transition->affectedBounds) {
				if(RouteIntersectsLocalizedBounds(route, profile, affected.origin,
					affected.previousBounds, affected.previousBoundsValid,
					affected.currentBounds, affected.currentBoundsValid)) {
					return DynamicRouteRebaseResult::Intersected;
				}
			}
			cursor = transition->toRootGeneration;
			sourceIndexRevision = transition->toSourceIndexRevision;
		}
		if(sourceIndexRevision != currentRoot.sourceIndexRevision) {
			unknownReason = DynamicRouteRebaseUnknownReason::FinalSourceMismatch;
			return DynamicRouteRebaseResult::Unknown;
		}

		// Endpoint transform revisions remain the protected request's responsibility.
		// Only the independently proven world tuple is refreshed for the retained result.
		rebasedObservation = capturedObservation;
		rebasedObservation.terrainPublicationGeneration = currentRoot.terrainPublicationGeneration;
		rebasedObservation.worldPublicationGeneration = currentRoot.rootGeneration;
		rebasedObservation.sourceIndexRevision = currentRoot.sourceIndexRevision;
		return DynamicRouteRebaseResult::Rebased;
	}

	void NavigationSystem::RecordPreparedRouteTransitionUnknownLocked(
		DynamicRouteRebaseUnknownReason reason, const PreparedRoute& prepared,
		const NavigationWorldRoot& currentRoot) {
		// Count exactly one first-failure reason for every fail-closed rebase. The
		// scalar data is cheap enough for all frames; only eight value-only tuples
		// are retained to make an installed smoke actionable without route history.
		switch(reason) {
		case DynamicRouteRebaseUnknownReason::InvalidInput:
			++dynamicDiagnostics_.preparedRouteTransitionInvalidInputCount;
			break;
		case DynamicRouteRebaseUnknownReason::TerrainMismatch:
			++dynamicDiagnostics_.preparedRouteTransitionTerrainMismatchCount;
			break;
		case DynamicRouteRebaseUnknownReason::MissingLink:
			++dynamicDiagnostics_.preparedRouteTransitionMissingLinkCount;
			break;
		case DynamicRouteRebaseUnknownReason::InvalidLink:
			++dynamicDiagnostics_.preparedRouteTransitionInvalidLinkCount;
			break;
		case DynamicRouteRebaseUnknownReason::SourceChainMismatch:
			++dynamicDiagnostics_.preparedRouteTransitionSourceChainMismatchCount;
			break;
		case DynamicRouteRebaseUnknownReason::FinalSourceMismatch:
			++dynamicDiagnostics_.preparedRouteTransitionFinalSourceMismatchCount;
			break;
		case DynamicRouteRebaseUnknownReason::None:
			return;
		}
		if(preparedRouteTransitionUnknownDiagnosticCount_ >= 8U) return;
		++preparedRouteTransitionUnknownDiagnosticCount_;
		const auto reasonText = [reason]() -> std::string_view {
			switch(reason) {
			case DynamicRouteRebaseUnknownReason::InvalidInput: return "invalid-input";
			case DynamicRouteRebaseUnknownReason::TerrainMismatch: return "terrain-mismatch";
			case DynamicRouteRebaseUnknownReason::MissingLink: return "missing-link";
			case DynamicRouteRebaseUnknownReason::InvalidLink: return "invalid-link";
			case DynamicRouteRebaseUnknownReason::SourceChainMismatch: return "source-chain-mismatch";
			case DynamicRouteRebaseUnknownReason::FinalSourceMismatch: return "final-source-mismatch";
			case DynamicRouteRebaseUnknownReason::None: return "none";
			}
			return "unknown";
		};
		diagnostics_.Append({NavigationDiagnosticSeverity::Warning,
			NavigationMeasurementReason::MeasurementUnavailable, lastProtectedBoundaryGeneration_, 0U,
			"navigation.prepared_route_transition_unknown",
			std::string(reasonText()) + ": capturedRoot=" + std::to_string(prepared.rootGeneration)
				+ ", currentRoot=" + std::to_string(currentRoot.rootGeneration)
				+ ", capturedTerrain=" + std::to_string(prepared.observation.terrainPublicationGeneration)
				+ ", currentTerrain=" + std::to_string(currentRoot.terrainPublicationGeneration)
				+ ", capturedSource=" + std::to_string(prepared.observation.sourceIndexRevision)
				+ ", currentSource=" + std::to_string(currentRoot.sourceIndexRevision)});
	}

	void NavigationSystem::PromoteDeferredRoutesForProfileLocked(const NavigationProfile& profile) {
		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
		for(auto iterator = deferredRoutes_.begin(); iterator != deferredRoutes_.end();) {
			if(MakeGroundProfileBuildKey(iterator->request.profile) != profileBuildKey) {
				++iterator;
				continue;
			}
			if(!requestStore_ || requestStore_->Status(iterator->handle) != NavigationHandleStatus::Alive) {
				iterator = deferredRoutes_.erase(iterator);
				continue;
			}
			const ProfileCoverageAdmission admission = ClassifyProfileCoverageAdmissionLocked(iterator->request);
			if(admission == ProfileCoverageAdmission::PendingRecovery) {
				// A newer receipt may still own this exact endpoint even though another
				// shard for the same profile just published. Keep it visibly pending.
				++iterator;
				continue;
			}
			// Ready coverage may now search. Unavailable intentionally follows the
			// legacy queued worker path so detailed Missing* outcomes remain public.
			if(requestStore_) {
				(void)requestStore_->Update(iterator->handle, [](NavigationRequestRecord& request) {
					request.lifecycle = NavigationRequestLifecycle::Queued;
					request.outcome = NavigationOutcome::Queued;
					request.reason = NavigationReason::None;
				});
			}
			const RouteClock::time_point promotedAt = RouteClock::now();
			RecordRouteDuration(dynamicDiagnostics_.routeDeferredCoverageWaitCount,
				dynamicDiagnostics_.routeDeferredCoverageWaitNanoseconds,
				dynamicDiagnostics_.routeDeferredCoverageWaitMaxNanoseconds,
				iterator->admittedTime, promotedAt);
			QueueRouteAgainstPublishedWorldLocked(iterator->handle, iterator->intentGeneration,
				std::move(iterator->request));
			iterator = deferredRoutes_.erase(iterator);
		}
	}

	void NavigationSystem::ResolveDeferredProfileBuildFailureLocked(const NavigationProfile& profile,
		bool mayRequeueFromNewCapture, NavigationOutcome terminalOutcome,
		NavigationReason terminalReason, std::string_view diagnosticDetail) {
		// A newly captured automatic packet may have replaced the failed candidate.
		// Requeue from that complete packet when possible; otherwise make the request
		// terminal so callers never wait forever for coverage that cannot be built.
		if(mayRequeueFromNewCapture) {
			// Evaluate each deferred request against its own endpoint. This may promote
			// unrelated ready routes while retaining only endpoints still covered by an
			// owned dirty recovery; it never retries a provider failure when this flag is
			// false.
			PromoteDeferredRoutesForProfileLocked(profile);
			const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
			const bool stillRecovering = std::any_of(deferredRoutes_.begin(), deferredRoutes_.end(),
				[profileBuildKey](const DeferredRoute& deferred) {
					return MakeGroundProfileBuildKey(deferred.request.profile) == profileBuildKey;
				});
			if(stillRecovering) return;
			// Every matching deferred route either entered normal preparation or was
			// already released. Do not overwrite its visible outcome below.
			return;
		}
		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
		if(!diagnosticDetail.empty()) {
			diagnostics_.Append({NavigationDiagnosticSeverity::Warning,
				NavigationMeasurementReason::MeasurementUnavailable, lastProtectedBoundaryGeneration_, 0U,
				"navigation.deferred_profile_build_failed", std::string(diagnosticDetail)});
		}
		for(auto iterator = deferredRoutes_.begin(); iterator != deferredRoutes_.end();) {
			if(MakeGroundProfileBuildKey(iterator->request.profile) != profileBuildKey) {
				++iterator;
				continue;
			}
			if(requestStore_) {
				(void)requestStore_->Update(iterator->handle,
					[terminalOutcome, terminalReason](NavigationRequestRecord& request) {
						request.lifecycle = NavigationRequestLifecycle::Terminal;
						request.outcome = terminalOutcome;
						request.reason = terminalReason;
					});
			}
			iterator = deferredRoutes_.erase(iterator);
		}
		ReleaseUnretainedDynamicSourceStageLocked(profile);
	}

	void NavigationSystem::ReleaseUnretainedDynamicSourceStageLocked(const NavigationProfile& profile) {
		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
		const auto matchesProfile = [profileBuildKey](const auto& build) {
			return MakeGroundProfileBuildKey(build.profile) == profileBuildKey;
		};
		// A stage continues to serve a published profile and any copied build that
		// has not reached its protected terminal result. Only a terminal failure with
		// neither kind of ownership may release its shared snapshot vector.
		if(HasReadyCoverageForProfileLocked(profile)
			|| std::any_of(pendingGroundBuilds_.begin(), pendingGroundBuilds_.end(), matchesProfile)
			|| std::any_of(preparedGroundBuilds_.begin(), preparedGroundBuilds_.end(), matchesProfile)
			|| (activeAsyncBuild_ && activeAsyncBuild_->result
				&& activeAsyncBuild_->identity.world == (world_ ? world_->Handle() : NavigationWorldHandle{})
				&& activeAsyncBuild_->identity.lifecycleEpoch == asyncBuildEpoch_
				&& activeAsyncBuild_->identity.profileBuildKey == profileBuildKey
				&& (activeAsyncBuild_->result->groundPacket || activeAsyncBuild_->result->dynamicPacket))) return;
		std::erase_if(dynamicSourceStages_, [profileBuildKey](const DynamicSourceStage& stage) {
			return MakeGroundProfileBuildKey(stage.profile) == profileBuildKey;
		});
	}

	NavigationRequestHandle NavigationSystem::SubmitOneShotRoute(
		Ground::GroundRouteQuery::Request request) {
		if(!request.owner.IsValid() || !request.profile.Validate().accepted
			|| request.maxExpansions == 0U
			|| !ValidateNavigationLocation(request.start).Succeeded()
			|| !ValidateNavigationLocation(request.destination).Succeeded()
			|| request.start.world != request.owner.world || request.destination.world != request.owner.world) return {};
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire) || !requestStore_ || !world_
			|| world_->Handle() != request.owner.world) return {};
		if(!CanAdmitDirectRouteLocked()) return {};
		NavigationRequestRecord record{};
		record.createdTick = 1U;
		record.lifecycle = NavigationRequestLifecycle::Queued;
		record.outcome = NavigationOutcome::Queued;
		record.start = request.start;
		record.destination = request.destination;
		record.profile = request.profile;
		record.allowPartial = request.allowPartial;
		const auto created = requestStore_->Create(request.owner, std::move(record));
		if(!created.Succeeded()) return {};
		if(requestRoutes_.size() <= created.handle.slotIndex) requestRoutes_.resize(created.handle.slotIndex + 1U);
		requestRoutes_[created.handle.slotIndex] = {};
		// If automatic capture retained current terrain inputs, demand exact coverage
		// for this caller-provided profile before a route worker snapshots the world.
		// Direct low-level callers without that capture keep the existing fail-closed
		// unavailable-coverage result instead of silently substituting a preset.
		const ProfileCoverageAdmission profileAdmission = ClassifyProfileCoverageAdmissionLocked(request);
		if(profileAdmission == ProfileCoverageAdmission::Ready
			|| profileAdmission == ProfileCoverageAdmission::Unavailable) {
			QueueRouteAgainstPublishedWorldLocked(created.handle, 0U, std::move(request));
		} else if(profileAdmission == ProfileCoverageAdmission::PendingRecovery) {
			// The request is admitted but cannot reach a route worker until this exact
			// profile's copied terrain build publishes. Do not report it as merely queued.
			(void)requestStore_->Update(created.handle, [](NavigationRequestRecord& record) {
				record.lifecycle = NavigationRequestLifecycle::PendingPreparation;
				record.outcome = NavigationOutcome::PendingPreparation;
				record.reason = NavigationReason::DirtyCoverage;
			});
			deferredRoutes_.push_back({created.handle, 0U, std::move(request), RouteClock::now()});
		}
		contractTestWorkAvailable_.store(true, std::memory_order_release);
		return created.handle;
	}

	std::optional<NavigationObjectEndpoint> NavigationSystem::CaptureObjectEndpointLocked(
		ObjectManager& objects, const TerraEngine::UUID& objectId) const {
		if(!world_ || objectId == TerraEngine::EMPTY_UUID) {
			return std::nullopt;
		}
		const std::optional<Object> object = objects.getObject(objectId);
		if(!object || !objects.isObjectActiveInHierarchy(*object)) {
			return std::nullopt;
		}
		const std::optional<Scene::WorldPose> pose = objects.GetCanonicalWorldPose(*object);
		const NavigationGeneration lifetime = objects.getObjectLifetimeGeneration(*object);
		const NavigationGeneration transformRevision = objects.GetObjectTransformRevision(*object);
		if(!pose || lifetime == 0U || transformRevision == 0U) {
			return std::nullopt;
		}
		const Scene::WorldCoordinateResult normalized = Scene::NormalizeWorldPosition(pose->position);
		if(!normalized.Succeeded()) {
			return std::nullopt;
		}
		NavigationObjectEndpoint endpoint{};
		endpoint.object = {objectId, lifetime};
		endpoint.location = {world_->Handle(), normalized.position};
		endpoint.transformRevision = transformRevision;
		endpoint.activeInHierarchy = true;
		return ValidateNavigationObjectEndpoint(endpoint).accepted
			? std::optional<NavigationObjectEndpoint>{endpoint} : std::nullopt;
	}

	NavigationSystem::ObjectRouteReceipt NavigationSystem::RequestRoute(
		ObjectManager& objects, const TerraEngine::UUID& sourceObject,
		NavigationLocation destination, NavigationProfile profile,
		NavigationRoutePolicy policy, std::uint64_t replacementKey) {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire) || !world_ || !requestStore_) {
			return {{}, NavigationHandleStatus::Invalid, NavigationOutcome::InvalidRequest, NavigationReason::SceneReplaced};
		}
		const std::optional<NavigationObjectEndpoint> source = CaptureObjectEndpointLocked(objects, sourceObject);
		if(!source) {
			return {{}, NavigationHandleStatus::Invalid, NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidStart};
		}
		return SubmitCapturedObjectRouteLocked(*source, nullptr, std::move(destination), std::move(profile), policy, replacementKey);
	}

	NavigationSystem::ObjectRouteReceipt NavigationSystem::RequestRoute(
		ObjectManager& objects, const TerraEngine::UUID& sourceObject,
		const TerraEngine::UUID& targetObject, NavigationProfile profile,
		NavigationRoutePolicy policy, std::uint64_t replacementKey) {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire) || !world_ || !requestStore_) {
			return {{}, NavigationHandleStatus::Invalid, NavigationOutcome::InvalidRequest, NavigationReason::SceneReplaced};
		}
		const std::optional<NavigationObjectEndpoint> source = CaptureObjectEndpointLocked(objects, sourceObject);
		if(!source) {
			return {{}, NavigationHandleStatus::Invalid, NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidStart};
		}
		const std::optional<NavigationObjectEndpoint> target = CaptureObjectEndpointLocked(objects, targetObject);
		if(!target) {
			return {{}, NavigationHandleStatus::Invalid, NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidDestination};
		}
		return SubmitCapturedObjectRouteLocked(*source, &*target, target->location, std::move(profile), policy, replacementKey);
	}

	NavigationSystem::ObjectRouteReceipt NavigationSystem::RequestAgentRoute(
		ObjectManager& objects, NavigationObjectIdentity sourceIdentity,
		NavigationLocation destination, NavigationProfile profile) {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire) || !world_ || !requestStore_) {
			return {{}, NavigationHandleStatus::Invalid, NavigationOutcome::InvalidRequest, NavigationReason::SceneReplaced};
		}
		if(!sourceIdentity.IsValid()) {
			return {{}, NavigationHandleStatus::Invalid, NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidStart};
		}
		const std::optional<NavigationObjectEndpoint> source =
			CaptureObjectEndpointLocked(objects, sourceIdentity.objectId);
		// The copied lifetime must match the protected recapture. This rejects a
		// remove-and-readd race without retaining an ObjectManager pointer in work.
		if(!source || source->object != sourceIdentity) {
			return {{}, NavigationHandleStatus::StaleOwner, NavigationOutcome::Stale, NavigationReason::SceneReplaced};
		}
		const std::uint64_t replacementKey = kNavigationAgentReplacementKeyNamespace
			| (TerraEngine::MakeObjectStableHash(sourceIdentity.objectId)
				& kNavigationAgentReplacementKeyPayloadMask);
		return SubmitCapturedObjectRouteLocked(*source, nullptr, std::move(destination),
			std::move(profile), NavigationRoutePolicy::NearestReachable, replacementKey,
			RouteRequestClass::Agent);
	}

	bool NavigationSystem::CanAdmitAgentRouteLocked() {
		PruneReleasedAgentRoutesLocked();
		if(!requestStore_) return false;
		// Tiny test/configuration stores retain historical direct-query behavior;
		// production-sized stores reserve one quarter for agent replans.
		const std::size_t reservedAgentSlots = requestStore_->Capacity() < 4U ? 0U
			: std::max<std::size_t>(1U, requestStore_->Capacity() / 4U);
		const std::size_t alive = requestStore_->AliveCount();
		if(alive >= requestStore_->Capacity()) return false;
		const std::size_t agentAlive = agentRequestHandles_.size();
		const std::size_t directAlive = alive >= agentAlive ? alive - agentAlive : 0U;
		// Each class may consume its share while preserving the other class's
		// reservation. Once the other class has filled its reservation, spare
		// capacity becomes shared again.
		return agentAlive < requestStore_->Capacity() - reservedAgentSlots
			|| directAlive >= reservedAgentSlots;
	}

	bool NavigationSystem::CanAdmitDirectRouteLocked() {
		PruneReleasedAgentRoutesLocked();
		if(!requestStore_) return false;
		const std::size_t capacity = requestStore_->Capacity();
		if(capacity == 0U) return false;
		const std::size_t reservedAgentSlots = capacity < 4U ? 0U
			: std::max<std::size_t>(1U, capacity / 4U);
		const std::size_t alive = requestStore_->AliveCount();
		if(alive >= capacity) return false;
		const std::size_t agentAlive = agentRequestHandles_.size();
		const std::size_t directAlive = alive >= agentAlive ? alive - agentAlive : 0U;
		return directAlive < capacity - reservedAgentSlots
			|| agentAlive >= reservedAgentSlots;
	}

	void NavigationSystem::PruneReleasedAgentRoutesLocked() {
		if(!requestStore_) {
			agentRequestHandles_.clear();
			return;
		}
		std::erase_if(agentRequestHandles_, [this](NavigationRequestHandle handle) {
			return requestStore_->Status(handle) != NavigationHandleStatus::Alive;
		});
	}

	NavigationSystem::ObjectRouteReceipt NavigationSystem::SubmitCapturedObjectRouteLocked(
		const NavigationObjectEndpoint& source, const NavigationObjectEndpoint* target,
		NavigationLocation destination, NavigationProfile profile,
		NavigationRoutePolicy policy, std::uint64_t replacementKey,
		RouteRequestClass requestClass) {
		if(!world_ || !requestStore_ || !ValidateNavigationObjectEndpoint(source).accepted
			|| !profile.Validate().accepted || !ValidateNavigationLocation(destination).accepted
			|| source.location.world != world_->Handle() || destination.world != world_->Handle()) {
			return {{}, NavigationHandleStatus::Invalid, NavigationOutcome::InvalidRequest, NavigationReason::InvalidDestination};
		}
		if(target != nullptr && !ValidateNavigationObjectEndpoint(*target).accepted) {
			return {{}, NavigationHandleStatus::Invalid, NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidDestination};
		}
		if(policy != NavigationRoutePolicy::ExactDestination && policy != NavigationRoutePolicy::NearestReachable) {
			return {{}, NavigationHandleStatus::Invalid, NavigationOutcome::InvalidRequest, NavigationReason::InvalidDestination};
		}
		if(requestClass == RouteRequestClass::Agent && !CanAdmitAgentRouteLocked()) {
			return {{}, NavigationHandleStatus::CapacityExceeded, NavigationOutcome::RejectedCapacity, NavigationReason::OutputLimit};
		}
		if(requestClass == RouteRequestClass::Direct) {
			if(!CanAdmitDirectRouteLocked()) {
				return {{}, NavigationHandleStatus::CapacityExceeded, NavigationOutcome::RejectedCapacity, NavigationReason::OutputLimit};
			}
		}

		NavigationOwnerId ownerId{TerraEngine::MakeObjectStableHash(source.object.objectId)};
		if(!ownerId.IsValid()) {
			return {{}, NavigationHandleStatus::InvalidOwner, NavigationOutcome::InvalidEndpoint, NavigationReason::InvalidStart};
		}
		const NavigationOwnerHandle owner{world_->Handle(), ownerId, source.object.lifetimeGeneration};
		NavigationRequestRecord record{};
		record.createdTick = lastProtectedBoundaryGeneration_;
		record.lifecycle = NavigationRequestLifecycle::Queued;
		record.outcome = NavigationOutcome::Queued;
		// Each admission owns one immutable intent. A replacement receives a new
		// record instead of mutating this value while a worker may still read it.
		record.intentGeneration = 1U;
		record.sourceEndpoint = source;
		if(target != nullptr) {
			record.destinationEndpoint = *target;
		}
		record.profile = profile;
		record.routePolicy = policy;
		record.replacementKey = replacementKey;
		record.start = source.location;
		record.destination = destination;
		record.allowPartial = policy == NavigationRoutePolicy::NearestReachable;
		const std::shared_ptr<const NavigationWorldRoot> root = world_->PublishedRoot();
		record.observation = {lastProtectedBoundaryGeneration_, source.transformRevision,
			target != nullptr ? target->transformRevision : 0U,
			root ? root->terrainPublicationGeneration : 0U,
			root ? root->rootGeneration : 0U,
			root ? root->sourceIndexRevision : 0U};
		const NavigationStoreCreateResult<NavigationRequestHandle> created = requestStore_->Create(owner, std::move(record));
		if(!created.Succeeded()) {
			return {{}, created.status, NavigationOutcome::RejectedCapacity, NavigationReason::OutputLimit};
		}
		if(requestClass == RouteRequestClass::Agent) {
			agentRequestHandles_.push_back(created.handle);
		}
		if(replacementKey != 0U) {
			for(auto iterator = replacementIntents_.begin(); iterator != replacementIntents_.end();) {
				if(iterator->owner != owner || iterator->key != replacementKey
					|| iterator->requestClass != requestClass) {
					++iterator;
					continue;
				}
				const NavigationRequestHandle replacedRequest = iterator->request;
				(void)requestStore_->Cancel(replacedRequest);
				if(replacedRequest.slotIndex < requestRoutes_.size()
					&& requestRoutes_[replacedRequest.slotIndex].IsValid()) {
					if(invalidations_.size() >= settings_.maxAdmittedRequests) {
						invalidations_.erase(invalidations_.begin());
					}
					invalidations_.push_back({nextInvalidationCursor_++, iterator->owner,
						requestRoutes_[replacedRequest.slotIndex], NavigationReason::SupersededRequest,
						NavigationOutcome::Stale, 0U, lastProtectedBoundaryGeneration_});
				}
				std::erase_if(pendingRoutes_, [replacedRequest](const PendingRoute& pending) {
					return pending.handle == replacedRequest;
				});
				std::erase_if(inFlightRoutes_, [replacedRequest](const PendingRoute& pending) {
					return pending.handle == replacedRequest;
				});
				std::erase_if(deferredRoutes_, [replacedRequest](const DeferredRoute& deferred) {
					return deferred.handle == replacedRequest;
				});
				iterator = replacementIntents_.erase(iterator);
			}
			replacementIntents_.push_back({owner, replacementKey, requestClass, created.handle});
		}
		if(requestRoutes_.size() <= created.handle.slotIndex) {
			requestRoutes_.resize(created.handle.slotIndex + 1U);
		}
		requestRoutes_[created.handle.slotIndex] = {};
		Ground::GroundRouteQuery::Request request{};
		request.owner = owner;
		request.start = source.location;
		request.destination = destination;
		request.profile = profile;
		request.allowPartial = policy == NavigationRoutePolicy::NearestReachable;
		request.maxExpansions = settings_.maxDetourNodesPerQuery;
		const ProfileCoverageAdmission profileAdmission = ClassifyProfileCoverageAdmissionLocked(request);
		if(profileAdmission == ProfileCoverageAdmission::Ready
			|| profileAdmission == ProfileCoverageAdmission::Unavailable) {
			QueueRouteAgainstPublishedWorldLocked(created.handle, 1U, std::move(request));
		} else if(profileAdmission == ProfileCoverageAdmission::PendingRecovery) {
			// Public object/agent callers observe the same explicit pending profile
			// preparation state as low-level one-shot callers.
			(void)requestStore_->Update(created.handle, [](NavigationRequestRecord& record) {
				record.lifecycle = NavigationRequestLifecycle::PendingPreparation;
				record.outcome = NavigationOutcome::PendingPreparation;
				record.reason = NavigationReason::DirtyCoverage;
			});
			deferredRoutes_.push_back({created.handle, 1U, std::move(request), RouteClock::now()});
		}
		contractTestWorkAvailable_.store(true, std::memory_order_release);
		return {created.handle, NavigationHandleStatus::Alive, NavigationOutcome::Queued, NavigationReason::None};
	}

	NavigationStoreReadResult<NavigationRequestRecord> NavigationSystem::ReadRequest(
		NavigationRequestHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!requestStore_ || !world_) return {};
		NavigationOwnerHandle currentOwner = handle.owner;
		currentOwner.world = world_->Handle();
		return requestStore_->Read(handle, currentOwner);
	}

	NavigationRouteHandle NavigationSystem::PublishedRouteForRequest(NavigationRequestHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!requestStore_ || !world_) return {};
		NavigationOwnerHandle currentOwner = handle.owner;
		currentOwner.world = world_->Handle();
		if(requestStore_->Status(handle, currentOwner) != NavigationHandleStatus::Alive
			|| handle.slotIndex >= requestRoutes_.size()) return {};
		return requestRoutes_[handle.slotIndex];
	}

	Ground::GroundRouteQuery::RetainedRoute NavigationSystem::ReadRoute(NavigationRouteHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!routeStore_ || !world_) return {};
		NavigationOwnerHandle currentOwner = handle.owner;
		currentOwner.world = world_->Handle();
		if(routeStore_->Status(handle, currentOwner) != NavigationHandleStatus::Alive
			|| handle.slotIndex >= publishedRouteData_.size()) return {};
		return publishedRouteData_[handle.slotIndex];
	}

	std::optional<NavigationObservationEvidence> NavigationSystem::ReadCurrentRouteObservation(
		NavigationRouteHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!routeStore_ || !requestStore_ || !world_) return std::nullopt;
		NavigationOwnerHandle currentOwner = handle.owner;
		currentOwner.world = world_->Handle();
		if(routeStore_->Status(handle, currentOwner) != NavigationHandleStatus::Alive) return std::nullopt;
		const auto route = routeStore_->Read(handle);
		if(!route.record) return std::nullopt;
		const auto request = requestStore_->Read(route.record->request);
		if(!request.record || request.record->outcome != NavigationOutcome::Complete) return std::nullopt;
		const std::shared_ptr<const NavigationWorldRoot> root = world_->PublishedRoot();
		if(!root || root->world != handle.owner.world) return std::nullopt;
		const NavigationObservationEvidence current{
			lastProtectedBoundaryGeneration_, 0U, 0U, root->terrainPublicationGeneration,
			root->rootGeneration, root->sourceIndexRevision};
		const NavigationRouteObservationValidation validation =
			ValidateNavigationRouteObservation(route.record->observation, current);
		if(!validation.usable) return std::nullopt;
		return route.record->observation;
	}

	NavigationSystem::RouteValidation NavigationSystem::ValidateRoute(NavigationRouteHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!routeStore_ || !requestStore_ || !world_) {
			return {false, NavigationOutcome::Stale, NavigationReason::SceneReplaced};
		}
		const auto route = routeStore_->Read(handle);
		if(!route.record) {
			return {false, NavigationOutcome::Stale, NavigationReason::SceneReplaced};
		}
		const auto request = requestStore_->Read(route.record->request);
		if(!request.record) {
			return {false, NavigationOutcome::Stale, NavigationReason::SceneReplaced};
		}
		// Partial/NearestReachable is a terminal route with retained points too.
		// It must remain consumable by the direct/editor route APIs, while agent
		// observation deliberately continues to require Complete above.
		if(request.record->outcome != NavigationOutcome::Complete
			&& request.record->outcome != NavigationOutcome::Partial) {
			return {false, request.record->outcome, request.record->reason};
		}
		const std::shared_ptr<const NavigationWorldRoot> root = world_->PublishedRoot();
		if(!root || root->world != handle.owner.world) {
			return {false, NavigationOutcome::Stale, NavigationReason::SceneReplaced};
		}
		const NavigationObservationEvidence current{
			lastProtectedBoundaryGeneration_, 0U, 0U, root->terrainPublicationGeneration,
			root->rootGeneration, root->sourceIndexRevision};
		const NavigationRouteObservationValidation validation =
			ValidateNavigationRouteObservation(route.record->observation, current);
		if(!validation.usable) return {false, validation.outcome, validation.reason};
		if(request.record->outcome == NavigationOutcome::Partial)
			return {true, NavigationOutcome::Partial, request.record->reason};
		return {true, validation.outcome, validation.reason};
	}

	bool NavigationSystem::CancelRequest(NavigationRequestHandle handle) {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!requestStore_ || !world_) return false;
		NavigationOwnerHandle currentOwner = handle.owner;
		currentOwner.world = world_->Handle();
		if(requestStore_->Status(handle, currentOwner) != NavigationHandleStatus::Alive) return false;
		const bool cancelled = requestStore_->Cancel(handle);
		if(cancelled) {
			std::erase_if(pendingRoutes_, [handle](const PendingRoute& pending) { return pending.handle == handle; });
			std::erase_if(inFlightRoutes_, [handle](const PendingRoute& pending) { return pending.handle == handle; });
			std::erase_if(deferredRoutes_, [handle](const DeferredRoute& deferred) { return deferred.handle == handle; });
			std::erase_if(replacementIntents_, [handle](const ReplacementIntent& intent) {
				return intent.request == handle;
			});
			if(handle.slotIndex < requestRoutes_.size() && requestRoutes_[handle.slotIndex].IsValid()) {
				if(invalidations_.size() >= settings_.maxAdmittedRequests) invalidations_.erase(invalidations_.begin());
				invalidations_.push_back({nextInvalidationCursor_++, handle.owner, requestRoutes_[handle.slotIndex],
					NavigationReason::Cancelled, NavigationOutcome::Cancelled, 0U, lastProtectedBoundaryGeneration_});
			}
		}
		return cancelled;
	}

	bool NavigationSystem::ReleaseRequestResult(NavigationRequestHandle handle) {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!requestStore_ || !world_) return false;
		NavigationOwnerHandle currentOwner = handle.owner;
		currentOwner.world = world_->Handle();
		const auto request = requestStore_->Read(handle, currentOwner);
		if(!request.record || request.record->lifecycle != NavigationRequestLifecycle::Terminal)
			return false;
		if(handle.slotIndex < requestRoutes_.size()) {
			const NavigationRouteHandle route = requestRoutes_[handle.slotIndex];
			if(route.IsValid() && routeStore_) (void)routeStore_->Release(route);
			requestRoutes_[handle.slotIndex] = {};
			if(route.IsValid() && route.slotIndex < publishedRouteData_.size())
				publishedRouteData_[route.slotIndex] = {};
		}
		const bool released = requestStore_->Release(handle);
		if(released) {
			std::erase(agentRequestHandles_, handle);
			std::erase_if(deferredRoutes_, [handle](const DeferredRoute& deferred) {
				return deferred.handle == handle;
			});
			std::erase_if(replacementIntents_, [handle](const ReplacementIntent& intent) {
				return intent.request == handle;
			});
		}
		return released;
	}

	void NavigationSystem::ConfigureProfileRegistrations(
		NavigationProfile defaultProfile,
		std::vector<NavigationProfileRegistration> registrations) {
		NavigationProfileResolver resolver = NavigationProfileResolver::Create(
			std::move(defaultProfile), registrations);
		std::lock_guard<std::mutex> lock(mutex_);
		profileResolver_ = std::move(resolver);
		// Configuration is copied as a whole at this protected call. Incrementing
		// the token makes the next frame-end source capture build any newly selected
		// character profile without exposing this mutable member to workers.
		++profileConfigurationRevision_;
		if(profileConfigurationRevision_ == 0U) profileConfigurationRevision_ = 1U;
	}

	NavigationProfileResolver NavigationSystem::ProfileResolverSnapshot() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return profileResolver_;
	}

	std::uint64_t NavigationSystem::ProfileConfigurationRevision() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return profileConfigurationRevision_;
	}

	NavigationSystem::NavigationInvalidationRead NavigationSystem::ReadInvalidations(
		NavigationOwnerHandle owner, std::uint64_t afterCursor) const {
		NavigationInvalidationRead result{};
		if(!owner.IsValid()) { result.status = NavigationHandleStatus::InvalidOwner; return result; }
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire)) return result;
		result.status = NavigationHandleStatus::Alive;
		result.newestCursor = nextInvalidationCursor_ == 0U ? 0U : nextInvalidationCursor_ - 1U;
		std::uint64_t earliestOwnerCursor = std::numeric_limits<std::uint64_t>::max();
		for(const NavigationInvalidation& invalidation : invalidations_) {
			if(invalidation.owner == owner)
				earliestOwnerCursor = std::min(earliestOwnerCursor, invalidation.cursor);
			if(invalidation.owner == owner && invalidation.cursor > afterCursor)
				result.records.push_back(invalidation);
		}
		// A global cursor may contain records for other owners, so compare only
		// against this owner's earliest retained record before declaring loss.
		result.overflowed = afterCursor != 0U
			&& earliestOwnerCursor != std::numeric_limits<std::uint64_t>::max()
			&& afterCursor < earliestOwnerCursor - 1U;
		return result;
	}

	void NavigationSystem::RevokeAllOwners(NavigationReason reason) {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire) || !world_) return;
		RetireAsyncBuildsLocked();
		const NavigationWorldHandle previous = world_->Handle();
		const NavigationGeneration nextGeneration = previous.worldGeneration
			== std::numeric_limits<NavigationGeneration>::max()
			? 1U : previous.worldGeneration + 1U;
		world_ = std::make_unique<NavigationWorld>(NavigationWorldHandle{previous.worldId, nextGeneration});
		requestStore_ = std::make_unique<NavigationRequestStore>(settings_.maxAdmittedRequests);
		routeStore_ = std::make_unique<NavigationRouteStore>(settings_.maxAdmittedRequests);
		// Revocation advances the world generation. Start a new dirty certificate so
		// a fresh scene mutation is never ordered against retired-world evidence.
		dirtyIndex_ = MakeSessionDirtyIndex();
		dynamicDiagnostics_ = {};
		preparedRouteTransitionUnknownDiagnosticCount_ = 0U;
		diagnostics_.PublishDynamic(dynamicDiagnostics_);
		lastProtectedBoundaryGeneration_ = 0U;
		pendingGroundBuilds_.clear();
		preparedGroundBuilds_.clear();
		// A lifecycle revoke marks the end of this world/session. The normal archive
		// mount submits fresh copied bytes after the next Initialize, so retaining an
		// old candidate here would both inflate diagnostics and risk cross-scene reuse.
		savedGroundCacheCandidate_.reset();
		savedGroundCacheStatus_ = {};
		savedGroundCacheAttemptedProfiles_.clear();
		savedGroundCacheImportInFlight_ = false;
		pendingDynamicChanges_.clear();
		pendingDynamicRebuilds_.clear();
		preparedDynamicRebuilds_.clear();
		dynamicRouteTransitions_.clear();
		dynamicRebuildProgress_.reset();
		dynamicSourceStages_.clear();
		stagedDynamicSourceReplacements_.clear();
		pendingDynamicSourceStageReplacement_.reset();
		automaticGroundSourceStage_.reset();
		automaticSourceIndexReplacement_.reset();
		freshAutomaticCaptureRequested_ = false;
		pendingRoutes_.clear();
		inFlightRoutes_.clear();
		deferredRoutes_.clear();
		preparedRoutes_.clear();
		pendingAgentRouteAdmissionDemand_.store(0U, std::memory_order_release);
		publishedShards_.clear();
		publishedGroundCaches_.clear();
		publishedRouteData_.clear();
		requestRoutes_.clear();
		replacementIntents_.clear();
		agentRequestHandles_.clear();
		invalidations_.clear();
		nextInvalidationCursor_ = 1U;
		nextRoutePreparationSequence_ = 1U;
		if(routePreparationEpoch_ != std::numeric_limits<std::uint64_t>::max()) {
			++routePreparationEpoch_;
		}
		if(routePreparationEpoch_ == 0U) routePreparationEpoch_ = 1U;
		lastRoutePreparationBatch_ = 0U;
		routePreparationBatchHighWater_ = 0U;
		// A new world must continue declaring its protected poll while a former
		// epoch still owns a copied worker packet and its BuildScratch permit.
		contractTestWorkAvailable_.store(asyncBuildCoordinator_.Status().retiredCount != 0U,
			std::memory_order_release);
		asyncBuildBudget_ = std::make_shared<NavigationBudget>(MakeNavigationBudgetLimits(settings_));
		if(asyncBuildEpoch_ != std::numeric_limits<std::uint64_t>::max()) ++asyncBuildEpoch_;
		if(asyncBuildEpoch_ == 0U) asyncBuildEpoch_ = 1U;
		PublishAsyncBuildDiagnosticsLocked();
		diagnostics_.Append({NavigationDiagnosticSeverity::Information,
			NavigationMeasurementReason::MeasurementUnavailable, nextGeneration, 0U,
			"navigation.owner_lifetime_revoked",
			std::string("Revoked navigation owners: ") + std::string(ToString(reason))});
	}

	std::vector<std::shared_ptr<const DetourGroundShardGeneration>> NavigationSystem::PublishedGroundShards() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return publishedShards_;
	}

	std::size_t NavigationSystem::ObserveSceneMutationReceipts(
		std::span<const Scene::NavigationSourceMutationReceipt> receipts) {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!world_ || !initialized_.load(std::memory_order_acquire)) return 0U;

		// A bridge replacement is provisional until this exact receipt batch reaches
		// dirty admission. Match both the content evidence and the queue-assigned
		// sequence; source content may legitimately repeat after a transform returns
		// to an earlier pose, whereas a sequence never identifies an older commit.
		std::optional<PendingDynamicSourceStageReplacement> readyStageReplacement;
		if(pendingDynamicSourceStageReplacement_) {
			bool sawStagedSource = false;
			bool completeReceiptBatch = true;
			for(const DynamicGroundSourceReplacement& staged : pendingDynamicSourceStageReplacement_->replacements) {
				bool exactReceipt = false;
				for(const Scene::NavigationSourceMutationReceipt& receipt : receipts) {
					if(receipt.intent.sourceId != staged.source.sourceId) continue;
					sawStagedSource = true;
					if(receipt.accepted && receipt.transactionStatus == Scene::TransactionStatus::Committed
						&& receipt.intent.committedSourceRevision == staged.source.sourceRevision
						&& receipt.intent.committedChangeSequence == staged.committedChangeSequence) {
						exactReceipt = true;
						break;
					}
				}
				if(!exactReceipt) completeReceiptBatch = false;
			}
			if(sawStagedSource) {
				if(completeReceiptBatch) readyStageReplacement = *pendingDynamicSourceStageReplacement_;
				else {
					// A receipt for this provisional source arrived without every exact
					// sequence-matched sibling. Do not fall through to the generic row
					// update: that would publish source evidence without its immutable
					// replacement snapshot and bypass the bridge's atomicity contract.
					pendingDynamicSourceStageReplacement_.reset();
					++dynamicDiagnostics_.rejectedUpdateCount;
					return 0U;
				}
			}
		}

		std::vector<NavigationProfile> provisionalProfiles;
		if(readyStageReplacement) {
			provisionalProfiles.reserve(readyStageReplacement->dynamicStages.size());
			for(const DynamicSourceStage& stage : readyStageReplacement->dynamicStages)
				provisionalProfiles.push_back(stage.profile);
		}
		const std::optional<NavigationDirtyAdmissionExpansion> admissionExpansion =
			ComputeDirtyAdmissionExpansionLocked(provisionalProfiles);
		if(!admissionExpansion) {
			// The copied transaction already committed in Scene, but Navigation cannot
			// safely publish only part of its source/coverage evidence. Match the normal
			// batch-rejection cleanup and leave the previous navigation root untouched.
			if(readyStageReplacement || pendingDynamicSourceStageReplacement_)
				pendingDynamicSourceStageReplacement_.reset();
			++dynamicDiagnostics_.rejectedUpdateCount;
			diagnostics_.Append({NavigationDiagnosticSeverity::Warning,
				NavigationMeasurementReason::MeasurementUnavailable, lastProtectedBoundaryGeneration_, 0U,
				"navigation.profile_dirty_admission_rejected",
				"Missing, invalid, or over-cap active profile envelope."});
			diagnostics_.PublishDynamic(dynamicDiagnostics_);
			return 0U;
		}

		std::size_t acceptedCount = 0U;
		std::vector<TerraEngine::UUID> admittedStagedSources;
		// Trial admission keeps a rejected receipt batch from changing the live dirty
		// index, tombstones, preparation queue, or route state. The copied index is
		// promoted only with the candidate root that carries its exact dirty evidence.
		NavigationDirtyIndex trialDirtyIndex = dirtyIndex_;
		struct AdmittedChange {
			Scene::NavigationSourceMutationIntent intent{};
			NavigationDynamicSourceChange change{};
			Scene::WorldPosition origin{};
		};
		std::vector<AdmittedChange> admittedChanges;
		admittedChanges.reserve(receipts.size());
		bool rejectedReceiptBatch = false;
		for(const Scene::NavigationSourceMutationReceipt& receipt : receipts) {
			if(!receipt.accepted || receipt.transactionStatus != Scene::TransactionStatus::Committed) {
				// Navigation receipts share their scene transaction boundary. Do not
				// admit a successful sibling when another source in that same supplied
				// batch was rejected; callers must receive one all-or-nothing outcome.
				if(receipt.intent.sourceId != EMPTY_UUID) rejectedReceiptBatch = true;
				continue;
			}
			const Scene::NavigationSourceMutationIntent& intent = receipt.intent;
			Scene::WorldPosition origin{};
			origin.cell = intent.oldBounds.valid ? intent.oldBounds.minimum.cell : intent.newBounds.minimum.cell;
			NavigationDynamicSourceChange change{};
			change.source = NavigationUuid{intent.sourceId};
			change.sourceRevision = intent.committedSourceRevision;
			change.changeSequence = intent.committedChangeSequence;
			change.physicalReadyRevision = intent.physicalReadyRevision;
			change.kind = ToDynamicChangeKind(intent.operation);
			if(!LocalizeMutationBounds(intent.oldBounds, origin, change.previousBounds)
				|| !LocalizeMutationBounds(intent.newBounds, origin, change.currentBounds)
				|| !SetCanonicalDirtyOrigin(origin, change)) {
				++dynamicDiagnostics_.rejectedUpdateCount;
				rejectedReceiptBatch = true;
				continue;
			}
			if(!trialDirtyIndex.CommitSourceChange(change, *admissionExpansion).accepted) {
				++dynamicDiagnostics_.rejectedUpdateCount;
				rejectedReceiptBatch = true;
				continue;
			}
			if(readyStageReplacement) {
				const auto staged = std::find_if(readyStageReplacement->replacements.begin(),
					readyStageReplacement->replacements.end(), [&receipt](const DynamicGroundSourceReplacement& current) {
						return current.source.sourceId == receipt.intent.sourceId
							&& current.source.sourceRevision == receipt.intent.committedSourceRevision
							&& current.committedChangeSequence == receipt.intent.committedChangeSequence;
					});
				if(staged != readyStageReplacement->replacements.end()
					&& std::find(admittedStagedSources.begin(), admittedStagedSources.end(),
						receipt.intent.sourceId) == admittedStagedSources.end())
					admittedStagedSources.push_back(receipt.intent.sourceId);
			}
			++acceptedCount;
			admittedChanges.push_back({intent, change, origin});
		}
		if(rejectedReceiptBatch) {
			// `trialDirtyIndex` and `admittedChanges` are local copies. Returning here
			// leaves the live dirty backlog, source root, route records, and staged
			// worker inputs exactly as they were before this rejected batch.
			if(readyStageReplacement || pendingDynamicSourceStageReplacement_)
				pendingDynamicSourceStageReplacement_.reset();
			return 0U;
		}
		if(acceptedCount == 0U) {
			if(readyStageReplacement) pendingDynamicSourceStageReplacement_.reset();
			return 0U;
		}
		const bool promoteReadyStage = readyStageReplacement
			&& admittedStagedSources.size() == readyStageReplacement->replacements.size();
		if(readyStageReplacement && !promoteReadyStage) {
			// At least one exact receipt failed dirty admission. Drop all provisional
			// snapshots so the published old evidence continues to force full-capture
			// fallback instead of silently reusing a staged replacement.
			pendingDynamicSourceStageReplacement_.reset();
		}

		const std::shared_ptr<const NavigationWorldRoot> current = world_->PublishedRoot();
		NavigationWorldRoot candidate = world_->CreateCandidate();
		candidate.tombstones = trialDirtyIndex.ObserveAtProtectedBoundary().entries;
		for(const NavigationDirtyCoverageDependency& dirty : trialDirtyIndex.DirtyCoverage()) {
			for(NavigationCoverageRecord& coverage : candidate.coverage) {
				if(coverage.key.shardX == FloorShard(dirty.tileX)
					&& coverage.key.shardZ == FloorShard(dirty.tileZ)
					&& coverage.key.verticalBand == dirty.verticalBand) {
					coverage.state = NavigationCoverageState::Dirty;
					coverage.complete = false;
				}
			}
		}
		for(const Scene::NavigationSourceMutationReceipt& receipt : receipts) {
			if(!receipt.accepted || receipt.transactionStatus != Scene::TransactionStatus::Committed) continue;
			if(trialDirtyIndex.LatestChangeSequence(NavigationUuid{receipt.intent.sourceId})
				!= (receipt.intent.committedChangeSequence != 0U
					? receipt.intent.committedChangeSequence : receipt.intent.committedSourceRevision)) continue;
			const DynamicGroundSourceReplacement* stagedReplacement = nullptr;
			if(promoteReadyStage) {
				const auto staged = std::find_if(readyStageReplacement->replacements.begin(),
					readyStageReplacement->replacements.end(), [&receipt](const DynamicGroundSourceReplacement& current) {
						return current.source.sourceId == receipt.intent.sourceId
							&& current.source.sourceRevision == receipt.intent.committedSourceRevision
							&& current.committedChangeSequence == receipt.intent.committedChangeSequence;
					});
				if(staged != readyStageReplacement->replacements.end()) stagedReplacement = &*staged;
			}
			const auto existing = std::find_if(candidate.sources.begin(), candidate.sources.end(),
				[&receipt](const NavigationSourceIndexRecord& source) {
					return source.sourceId == receipt.intent.sourceId;
				});
			if(stagedReplacement != nullptr) {
				if(existing == candidate.sources.end()) candidate.sources.push_back(stagedReplacement->source);
				else *existing = stagedReplacement->source;
				continue;
			}
			const bool enabled = receipt.intent.newEnabled
				&& receipt.intent.operation != Scene::NavigationSourceMutationOperation::Delete;
			if(existing == candidate.sources.end()) {
				candidate.sources.push_back({receipt.intent.sourceId, receipt.intent.committedSourceRevision,
					receipt.intent.committedSourceRevision, enabled});
			} else {
				existing->sourceRevision = receipt.intent.committedSourceRevision;
				existing->geometryRevision = receipt.intent.committedSourceRevision;
				existing->enabled = enabled;
			}
		}
		if(promoteReadyStage && readyStageReplacement->automaticStage) {
			// Source rows and the automatic eligibility certificate are one authority.
			// Exchange both in this root candidate so direct routes cannot observe a
			// newly moved collider alongside the pre-move index while profile work waits.
			candidate.eligibleSourceIndex = readyStageReplacement->automaticStage->eligibleSourceIndex;
			candidate.sourceIndexRevision = candidate.eligibleSourceIndex->Revision();
		}
		if(!world_->Publish(std::move(candidate), current->rootGeneration)) {
			// The root exchange is the final admission step. Do not retain a staged
			// packet after a rejected world candidate: its snapshots were never made
			// authoritative and live transform evidence must trigger fallback capture.
			if(readyStageReplacement) pendingDynamicSourceStageReplacement_.reset();
			++dynamicDiagnostics_.rejectedUpdateCount;
			diagnostics_.PublishDynamic(dynamicDiagnostics_);
			return 0U;
		}
		dirtyIndex_ = std::move(trialDirtyIndex);
		// The receipt has exchanged a newer immutable source/root tuple. Dynamic
		// children captured from the previous tuple cannot safely publish afterward;
		// retire their permits (rather than freeing them) and discard their protected
		// group bookkeeping before admitting the newer receipt recovery.
		RetireAsyncBuildsLocked();
		pendingDynamicRebuilds_.clear();
		preparedDynamicRebuilds_.clear();
		std::vector<DynamicRouteAffectedBounds> transitionBounds;
		transitionBounds.reserve(admittedChanges.size());
		for(const AdmittedChange& admitted : admittedChanges) {
			// Preserve the exact protected receipt bounds. A later worker result may be
			// rebased only after its copied corridor avoids every old/new swept range.
			transitionBounds.push_back({admitted.change.source, ChangeSequence(admitted.change), admitted.origin,
				admitted.change.previousBounds, admitted.change.currentBounds,
				admitted.intent.oldBounds.valid, admitted.intent.newBounds.valid});
		}
		if(const std::shared_ptr<const NavigationWorldRoot> published = world_->PublishedRoot())
			AppendDynamicRouteTransitionLocked(*current, *published, std::move(transitionBounds));
		for(const AdmittedChange& admitted : admittedChanges) {
			// The dirty index owns revision ordering. Queue only the root-published
			// packet, so worker preparation can never see a receipt whose root failed.
			pendingDynamicChanges_.push_back(admitted.change);
			// Compare the route's profile-inflated swept bounds with the committed old
			// and new source coverage. A spatially unrelated route remains usable;
			// an uncertain localization fails safe and requests a replan.
			for(const NavigationRouteHandle route : requestRoutes_) {
				if(!route.IsValid()) continue;
				if(!routeStore_ || routeStore_->Status(route) != NavigationHandleStatus::Alive
					|| route.slotIndex >= publishedRouteData_.size()) continue;
				const auto routeRecord = routeStore_->Read(route);
				if(!routeRecord.record) continue;
				const auto request = requestStore_ ? requestStore_->Read(routeRecord.record->request)
					: NavigationStoreReadResult<NavigationRequestRecord>{};
				if(!request.record || !RouteIntersectsLocalizedChange(publishedRouteData_[route.slotIndex], request.record->profile,
					admitted.origin, admitted.intent, admitted.change)) continue;
				if(invalidations_.size() >= settings_.maxAdmittedRequests) invalidations_.erase(invalidations_.begin());
				invalidations_.push_back({nextInvalidationCursor_++, route.owner, route,
					NavigationReason::DirtyCoverage, NavigationOutcome::Stale, admitted.change.sourceRevision,
					lastProtectedBoundaryGeneration_});
				if(requestStore_) (void)requestStore_->Update(routeRecord.record->request,
					[](NavigationRequestRecord& current) {
						current.outcome = NavigationOutcome::Stale;
						current.reason = NavigationReason::DirtyCoverage;
						current.lifecycle = NavigationRequestLifecycle::Terminal;
					});
			}
		}
		if(promoteReadyStage) {
			// Publish the copied worker inputs only after their matching source rows
			// are in the immutable root. This is the point at which a later profile
			// may safely reuse the replacement eligibility certificate.
			dynamicSourceStages_ = std::move(readyStageReplacement->dynamicStages);
			if(readyStageReplacement->automaticStage) {
				latestAutomaticSourceIndexRevision_ =
					readyStageReplacement->automaticStage->eligibleSourceIndex->Revision();
				automaticGroundSourceStage_ = std::move(*readyStageReplacement->automaticStage);
			}
			for(const DynamicGroundSourceReplacement& replacement : readyStageReplacement->replacements) {
				auto staged = std::find_if(stagedDynamicSourceReplacements_.begin(),
					stagedDynamicSourceReplacements_.end(), [&replacement](const NavigationSourceIndexRecord& current) {
						return current.sourceId == replacement.source.sourceId;
					});
				if(staged == stagedDynamicSourceReplacements_.end())
					stagedDynamicSourceReplacements_.push_back(replacement.source);
				else *staged = replacement.source;
			}
			pendingDynamicSourceStageReplacement_.reset();
		}
		// A root change would otherwise make every retained observation appear stale.
		// The preceding loop already made paths intersecting this batch terminal; only
		// those proven unaffected may receive the new immutable root evidence.
		RefreshUnaffectedRoutesAfterDynamicSourcePublicationLocked(current);
		dynamicDiagnostics_.dynamicSourceLagCount = dirtyIndex_.PendingPreparationCount();
		dynamicDiagnostics_.dirtyCoverageCount = dirtyIndex_.DirtyCoverage().size();
		dynamicDiagnostics_.queueDepth = dirtyIndex_.PendingPreparationCount();
		dynamicDiagnostics_.retiredGenerationCount = world_->RetiredRootCount();
		diagnostics_.PublishDynamic(dynamicDiagnostics_);
		contractTestWorkAvailable_.store(true, std::memory_order_release);
		return acceptedCount;
	}

	const NavigationDirtyIndex& NavigationSystem::DynamicDirtyIndexForTesting() const noexcept {
		return dirtyIndex_;
	}

	void NavigationSystem::ObserveProtectedBoundary(
		NavigationProtectedBoundary boundary,
		std::uint64_t publicationGeneration) {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire)) return;
		lastProtectedBoundaryGeneration_ = std::max(lastProtectedBoundaryGeneration_, publicationGeneration);
		// Object route admission is the only operation that captures a live pose.
		// This boundary therefore performs bookkeeping only and never scans objects
		// for a navigation component or keeps Transform storage for workers.
		// Boundary observations are deliberately bounded bookkeeping only. Tile
		// generation and route search remain in scheduler worker preparation.
		dynamicDiagnostics_.dynamicSourceLagCount = dirtyIndex_.PendingPreparationCount();
		dynamicDiagnostics_.dirtyCoverageCount = dirtyIndex_.DirtyCoverage().size();
		dynamicDiagnostics_.queueDepth = dirtyIndex_.PendingPreparationCount();
		if(boundary == NavigationProtectedBoundary::FrameEnd && world_)
			dynamicDiagnostics_.retiredGenerationCount = world_->RetiredRootCount();
		diagnostics_.PublishDynamic(dynamicDiagnostics_);
	}

	std::shared_ptr<const NavigationDiagnosticSnapshot> NavigationSystem::PublishedDiagnostics() const {
		return diagnostics_.Snapshot();
	}

	NavigationSystem::StoreUsageSnapshot NavigationSystem::PublishedStoreUsage() const {
		std::lock_guard<std::mutex> lock(mutex_);
		StoreUsageSnapshot snapshot{};
		snapshot.activeRequests = requestStore_ ? requestStore_->AliveCount() : 0U;
		snapshot.activeRoutes = routeStore_ ? routeStore_->AliveCount() : 0U;
		snapshot.pendingRoutes = pendingRoutes_.size();
		snapshot.inFlightRoutes = inFlightRoutes_.size();
		snapshot.deferredRoutes = deferredRoutes_.size();
		snapshot.preparedRoutes = preparedRoutes_.size();
		snapshot.lastRoutePreparationBatch = lastRoutePreparationBatch_;
		snapshot.routePreparationBatchHighWater = routePreparationBatchHighWater_;
		snapshot.replacementIntents = replacementIntents_.size();
		snapshot.agentRequestHandles = agentRequestHandles_.size();
		snapshot.dynamicSourceStages = dynamicSourceStages_.size();

		// Include current bounded work records plus copied cache evidence. Route
		// point allocations remain provider-owned and are intentionally excluded
		// rather than being reported as an invented allocator-wide total.
		snapshot.estimatedOwnedBytes = (requestStore_ ? requestStore_->MemoryBytes() : 0U)
			+ (routeStore_ ? routeStore_->MemoryBytes() : 0U)
			+ snapshot.pendingRoutes * sizeof(PendingRoute)
			+ snapshot.inFlightRoutes * sizeof(PendingRoute)
			+ snapshot.deferredRoutes * sizeof(DeferredRoute)
			+ snapshot.preparedRoutes * sizeof(PreparedRoute)
			+ snapshot.replacementIntents * sizeof(ReplacementIntent)
			+ snapshot.agentRequestHandles * sizeof(NavigationRequestHandle)
			+ dynamicSourceStages_.capacity() * sizeof(DynamicSourceStage);
		for(const DynamicSourceStage& stage : dynamicSourceStages_)
			snapshot.estimatedOwnedBytes += stage.sources.capacity()
				* sizeof(std::shared_ptr<const NavigationGeometrySnapshot>);
		for(const DynamicRouteTransition& transition : dynamicRouteTransitions_) {
			snapshot.estimatedOwnedBytes += sizeof(DynamicRouteTransition)
				+ transition.affectedBounds.capacity() * sizeof(DynamicRouteAffectedBounds)
				+ transition.topologyPreservedProfileBuildKeys.capacity() * sizeof(std::uint64_t);
		}
		// Dynamic reuse retains complete Recast blob proof beside each published shard.
		// Count those bytes once at their published owner; async packets hold shared
		// pointers only, so charging them again would overstate current-owned memory.
		for(const std::shared_ptr<const DetourGroundShardGeneration>& shard : publishedShards_) {
			if(!shard || !shard->retainedTiles) continue;
			snapshot.estimatedOwnedBytes += sizeof(DetourGroundShardGeneration)
				+ shard->retainedTiles->tiles.capacity() * sizeof(GroundTileBlob);
			for(const GroundTileBlob& tile : shard->retainedTiles->tiles) {
				snapshot.estimatedOwnedBytes += tile.pristineBytes.capacity()
					+ tile.sourceFingerprints.capacity() * sizeof(std::uint64_t);
			}
		}
		if(savedGroundCacheCandidate_) {
			snapshot.estimatedOwnedBytes += savedGroundCacheCandidate_->bytes.capacity()
				+ savedGroundCacheCandidate_->logicalPath.capacity();
		}
		for(const PublishedGroundCacheCoverage& cache : publishedGroundCaches_) {
			snapshot.estimatedOwnedBytes += sizeof(cache) + cache.tiles.capacity() * sizeof(GroundTileBlob)
				+ cache.evidence.sourceFingerprints.capacity() * sizeof(std::uint64_t)
				+ cache.evidence.tileKeys.capacity() * sizeof(GroundTileKey);
			for(const GroundTileBlob& tile : cache.tiles) {
				snapshot.estimatedOwnedBytes += tile.pristineBytes.capacity()
					+ tile.sourceFingerprints.capacity() * sizeof(std::uint64_t);
			}
		}
		return snapshot;
	}

	NavigationSystem::SavedNavigationCacheReceipt
	NavigationSystem::QueueSavedGroundCacheImport(SavedNavigationCacheCandidate candidate) {
		SavedNavigationCacheReceipt receipt{};
		if(candidate.bytes.empty() || candidate.bytes.size() > kMaximumSavedNavigationBytes) {
			receipt.reason = "SavedNavigationCandidateCapacityExceeded";
			return receipt;
		}
		if(candidate.logicalPath.empty() || candidate.logicalPath.size() > 1024U) {
			receipt.reason = "SavedNavigationCandidateLogicalPathInvalid";
			return receipt;
		}
		if(candidate.profile && !candidate.profile->Validate().Succeeded()) {
			receipt.reason = "SavedNavigationCandidateProfileInvalid";
			return receipt;
		}

		std::lock_guard<std::mutex> lock(mutex_);
		// A mounted archive can submit before the scene bridge has published its
		// first automatic source packet. Retaining this one bounded replacement is
		// safe because no cache byte is decoded or made visible at admission time.
		savedGroundCacheCandidate_ = std::move(candidate);
		savedGroundCacheStatus_.candidatePending = true;
		savedGroundCacheStatus_.cacheUsed = false;
		savedGroundCacheStatus_.candidateBytes = savedGroundCacheCandidate_->bytes.size();
		savedGroundCacheStatus_.logicalPath = savedGroundCacheCandidate_->logicalPath;
		savedGroundCacheStatus_.rejectionReason = "SavedNavigationAwaitingAuthoritativeCapture";
		savedGroundCacheAttemptedProfiles_.clear();
		savedGroundCacheImportInFlight_ = false;
		contractTestWorkAvailable_.store(true, std::memory_order_release);
		receipt.accepted = true;
		receipt.deferredUntilAuthoritativeCapture = true;
		receipt.reason = savedGroundCacheStatus_.rejectionReason;
		return receipt;
	}

	std::optional<NavigationSystem::SavedNavigationCacheSnapshot>
	NavigationSystem::CapturePublishedGroundCache(const NavigationProfile& profile) const {
		if(!profile.Validate().Succeeded()) {
			return std::nullopt;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		if(!world_) {
			return std::nullopt;
		}
		const std::shared_ptr<const NavigationWorldRoot> root = world_->PublishedRoot();
		if(!root || root->terrainState != NavigationTerrainState::Available) {
			return std::nullopt;
		}
		const std::uint64_t profileKey = MakeGroundProfileBuildKey(profile);
		const auto cached = std::find_if(publishedGroundCaches_.begin(), publishedGroundCaches_.end(),
			[profileKey](const PublishedGroundCacheCoverage& current) {
				return MakeGroundProfileBuildKey(current.profile) == profileKey;
			});
		if(cached == publishedGroundCaches_.end() || cached->profile != profile
			|| !cached->evidence.accepted || cached->sourceIndexRevision != root->sourceIndexRevision
			|| cached->tiles.empty()) {
			return std::nullopt;
		}
		if(!std::all_of(cached->tiles.begin(), cached->tiles.end(), [](const GroundTileBlob& tile) {
			return tile.supportComplete && tile.clearanceComplete;
		})) {
			return std::nullopt;
		}
		const SavedGroundTileExportResult encodedTiles = BuildSavedNavigationChunks(cached->tiles);
		if(!encodedTiles.accepted || encodedTiles.chunks.empty()) {
			return std::nullopt;
		}
		SavedNavigationCacheSnapshot snapshot{};
		snapshot.descriptor.formatVersion = kSavedNavigationFormatVersion;
		snapshot.descriptor.backendAbi = std::string(kSavedNavigationBackendAbi);
		snapshot.descriptor.sourceFingerprint = cached->evidence.sourceFingerprint;
		snapshot.descriptor.settingsFingerprint = cached->evidence.settingsFingerprint;
		if(snapshot.descriptor.sourceFingerprint == 0U || snapshot.descriptor.settingsFingerprint == 0U) {
			return std::nullopt;
		}
		snapshot.chunks = encodedTiles.chunks;
		return snapshot;
	}

	NavigationSystem::SavedNavigationCacheStatus NavigationSystem::SavedGroundCacheStatus() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return savedGroundCacheStatus_;
	}

	void NavigationSystem::SetContractTestWorkAvailable(bool available) noexcept {
		contractTestWorkAvailable_.store(available, std::memory_order_release);
	}

	void NavigationSystem::SetPendingAgentRouteAdmissionDemand(std::size_t demand) noexcept {
		// The declaration path applies the per-frame cap. Keeping this setter a plain
		// atomic store avoids taking NavigationSystem's protected mutex from command
		// capture and permits a conservative overestimate to be corrected later.
		pendingAgentRouteAdmissionDemand_.store(demand, std::memory_order_release);
	}

	Ground::GroundRouteQuery::Result NavigationSystem::SearchPublishedGroundRoute(
		const Ground::GroundRouteQuery::Request& request,
		const std::shared_ptr<const NavigationWorldRoot>& worldRoot,
		const std::vector<std::shared_ptr<const DetourGroundShardGeneration>>& shards,
		const Ground::GroundPortalGraphSnapshot& portals) const {
		Ground::GroundRouteQuery::Result result{};
		if(!request.owner.IsValid() || !request.profile.Validate().accepted) {
			result.outcome = NavigationOutcome::InvalidRequest;
			result.reason = NavigationReason::InvalidDestination;
			return result;
		}
		if(!ValidateNavigationLocation(request.start).Succeeded()) {
			result.outcome = NavigationOutcome::InvalidEndpoint;
			result.reason = NavigationReason::InvalidStart;
			return result;
		}
		if(!ValidateNavigationLocation(request.destination).Succeeded()) {
			result.outcome = NavigationOutcome::InvalidEndpoint;
			result.reason = NavigationReason::InvalidDestination;
			return result;
		}
		if(!worldRoot || worldRoot->world != request.owner.world
			|| request.start.world != worldRoot->world || request.destination.world != worldRoot->world) {
			result.outcome = NavigationOutcome::UnavailableCoverage;
			result.reason = NavigationReason::MissingTerrain;
			return result;
		}
		if(worldRoot->terrainState == NavigationTerrainState::Unavailable) {
			result.outcome = NavigationOutcome::UnavailableCoverage;
			result.reason = NavigationReason::MissingTerrain;
			return result;
		}
		result.sourceRevision = worldRoot->rootGeneration;

		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(request.profile);
		const auto startCandidates = CoverageCandidatesForEndpoint(request.start, request.profile, profileBuildKey);
		const auto destinationCandidates = CoverageCandidatesForEndpoint(request.destination, request.profile, profileBuildKey);
		if(!startCandidates || !destinationCandidates) {
			result.outcome = NavigationOutcome::UnavailableCoverage;
			result.reason = NavigationReason::MissingNeighbor;
			return result;
		}
		const auto findCoverage = [&worldRoot](const NavigationCoverageKey& key) {
			return std::find_if(worldRoot->coverage.begin(), worldRoot->coverage.end(), [&key](const NavigationCoverageRecord& record) {
				return record.key == key && record.state == NavigationCoverageState::Ready
					&& record.complete && record.coverageGeneration != 0U;
			});
		};
		const auto findShard = [&shards](const NavigationCoverageRecord& coverage)
			-> std::shared_ptr<const DetourGroundShardGeneration> {
			// GroundShardKey retains the same canonical int64 address as coverage.
			// Provider-local floats are introduced only after this exact lookup wins.
			const GroundShardKey key{coverage.key.shardX, coverage.key.shardZ,
				coverage.key.verticalBand, coverage.key.profileBuildKey};
			const auto found = std::find_if(shards.begin(), shards.end(), [&key, &coverage](const auto& candidate) {
				return candidate && candidate->reference.generation == coverage.coverageGeneration && candidate->reference.shard == key;
			});
			return found == shards.end() ? std::shared_ptr<const DetourGroundShardGeneration>{} : *found;
		};
		const auto floorDiv = [](std::int64_t value, std::int64_t divisor) {
			std::int64_t quotient = value / divisor;
			const std::int64_t remainder = value % divisor;
			if(remainder < 0) --quotient;
			return quotient;
		};
		const auto shardOrigin = [&floorDiv](const NavigationCoverageKey& key) {
			// Express a shard origin as an exact canonical cell plus a small offset.
			// This avoids ever converting a distant cell address into one giant float.
			Scene::WorldPosition origin{};
			origin.cell.x = floorDiv(key.shardX, 256);
			origin.cell.y = floorDiv(key.verticalBand, 512);
			origin.cell.z = floorDiv(key.shardZ, 256);
			origin.offsetMetres.x = static_cast<double>(key.shardX - origin.cell.x * 256) * 256.0;
			origin.offsetMetres.y = static_cast<double>(key.verticalBand - origin.cell.y * 512) * kGroundVerticalBandMetres;
			origin.offsetMetres.z = static_cast<double>(key.shardZ - origin.cell.z * 256) * 256.0;
			return origin;
		};
		const auto localizeLocation = [&shardOrigin](const NavigationLocation& location,
			const NavigationCoverageKey& key) -> std::optional<NavigationGeometryVector3> {
			const Scene::WorldLocalizationResult localized = Scene::LocalizeWorldPosition(
				location.position, shardOrigin(key), 1024.0);
			if(!localized.Succeeded()) return std::nullopt;
			return NavigationGeometryVector3{static_cast<float>(localized.offsetMetres.x),
				static_cast<float>(localized.offsetMetres.y), static_cast<float>(localized.offsetMetres.z)};
		};
		const auto selectEndpointCoverage = [&findCoverage, &findShard, &localizeLocation, &request, &worldRoot](
			const EndpointCoverageCandidates& candidates, const NavigationLocation& endpoint)
			-> std::optional<NavigationCoverageKey> {
			const auto primary = findCoverage(candidates.primary);
			if(!candidates.boundaryAlternative) {
				return primary != worldRoot->coverage.end() && findShard(*primary) ? std::optional<NavigationCoverageKey>{candidates.primary}
					: std::nullopt;
			}
			const double projectionTolerance = request.profile.verticalResolutionMetres
				+ request.profile.clearanceMarginMetres;
			const auto hasProjectedSupport = [&findShard, &localizeLocation, &request, projectionTolerance](
				const NavigationCoverageRecord& coverage, const NavigationLocation& location) {
				const std::shared_ptr<const DetourGroundShardGeneration> shard = findShard(coverage);
				const std::optional<NavigationGeometryVector3> local = localizeLocation(location, coverage.key);
				if(!shard || !local) return false;
				// This is a support probe, not a route. Its narrow vertical extent makes
				// the adjacent band eligible only when it contains physical support at
				// the endpoint, preserving separated stacked floors.
				DetourGroundRouteQuery probe{};
				probe.start = *local;
				probe.destination = *local;
				probe.horizontalSearchRadius = static_cast<float>(request.profile.radiusMetres
					+ request.profile.clearanceMarginMetres + 1.0);
				probe.verticalSearchRadius = static_cast<float>(projectionTolerance);
				probe.maximumPoints = 2U;
				return QueryDetourGroundShard(*shard, probe).outcome == NavigationOutcome::Complete;
			};
			if(primary != worldRoot->coverage.end() && hasProjectedSupport(*primary, endpoint)) {
				return candidates.primary;
			}
			const auto alternative = findCoverage(*candidates.boundaryAlternative);
			if(alternative != worldRoot->coverage.end() && hasProjectedSupport(*alternative, endpoint)) {
				return *candidates.boundaryAlternative;
			}
			// Preserve the normal detailed provider result when the preferred coverage
			// exists but neither band has boundary-local support.
			return primary != worldRoot->coverage.end() && findShard(*primary)
				? std::optional<NavigationCoverageKey>{candidates.primary} : std::nullopt;
		};
		const auto startKey = selectEndpointCoverage(*startCandidates, request.start);
		const auto destinationKey = selectEndpointCoverage(*destinationCandidates, request.destination);
		if(!startKey || !destinationKey) {
			result.outcome = NavigationOutcome::UnavailableCoverage;
			result.reason = NavigationReason::MissingNeighbor;
			return result;
		}
		const auto startCoverage = findCoverage(*startKey);
		const auto destinationCoverage = findCoverage(*destinationKey);
		if(startCoverage == worldRoot->coverage.end() || destinationCoverage == worldRoot->coverage.end()
			|| !findShard(*startCoverage) || !findShard(*destinationCoverage)) {
			result.outcome = NavigationOutcome::UnavailableCoverage;
			result.reason = NavigationReason::MissingNeighbor;
			return result;
		}
		const Ground::GroundPortalSearchResult portalPath = portals.Search(*startKey, *destinationKey,
			request.profile, std::min<std::uint32_t>(request.maxExpansions, settings_.maxEngineNodesPerRequest),
			settings_.maxRoutePoints);
		if(!portalPath.Succeeded()) {
			if(portalPath.outcome == NavigationOutcome::UnavailableCoverage
				&& portalPath.reason == NavigationReason::MissingNeighbor) {
				// Portal construction is profile-sensitive. If complete shard coverage is
				// continuous between supported endpoints, an empty portal path describes
				// a real obstruction (for example, a low roof), not missing streaming
				// coverage. Build this bounded evidence only on the failure path so the
				// normal high-throughput route query stays a direct portal search.
				std::vector<NavigationCoverageKey> completeCoverage;
				completeCoverage.reserve(worldRoot->coverage.size());
				for(const NavigationCoverageRecord& coverage : worldRoot->coverage) {
					if(coverage.state == NavigationCoverageState::Ready && coverage.complete
						&& coverage.coverageGeneration != 0U && findShard(coverage)) {
						completeCoverage.push_back(coverage.key);
					}
				}
				const Ground::GroundPortalSearchResult coverageClassification =
					Ground::ClassifyPortalMissingNeighbor(*startKey, *destinationKey, completeCoverage,
						std::min<std::size_t>(settings_.maxEngineNodesPerRequest, completeCoverage.size()));
				if(coverageClassification.outcome == NavigationOutcome::Unreachable) {
					result.outcome = coverageClassification.outcome;
					result.reason = coverageClassification.reason;
					return result;
				}
			}
			result.outcome = portalPath.outcome;
			result.reason = portalPath.reason;
			return result;
		}
		// Every coverage node in the portal path must still have a matching complete
		// published shard. A graph edge alone never authorizes a route through dirty
		// or evicted coverage.
		for(const NavigationCoverageKey& key : portalPath.coveragePath) {
			const auto coverage = findCoverage(key);
			if(coverage == worldRoot->coverage.end() || !findShard(*coverage)) {
				result.outcome = NavigationOutcome::UnavailableCoverage;
				result.reason = NavigationReason::MissingNeighbor;
				return result;
			}
		}
		const std::uint32_t maximumSearchNodes = std::min<std::uint32_t>(
			request.maxExpansions, settings_.maxDetourNodesPerQuery);
		const std::uint32_t maximumRoutePoints = settings_.maxRoutePoints;
		// A distinct start and destination require two public points even when Detour
		// projects both endpoints onto the same polygon. Reject before provider work
		// so a configured one-point guidance budget cannot publish an incomplete path.
		if(request.start != request.destination && maximumRoutePoints < 2U) {
			result.outcome = NavigationOutcome::BudgetExhausted;
			result.reason = NavigationReason::OutputLimit;
			return result;
		}
		const auto querySegment = [&request, maximumSearchNodes, maximumRoutePoints](const DetourGroundShardGeneration& shard,
			const NavigationGeometryVector3& start, const NavigationGeometryVector3& destination) {
			DetourGroundRouteQuery query{};
			query.start = start; query.destination = destination;
			query.horizontalSearchRadius = static_cast<float>(request.profile.radiusMetres + request.profile.clearanceMarginMetres + 1.0);
			query.verticalSearchRadius = static_cast<float>(request.profile.heightMetres + request.profile.stepHeightMetres + 2.0);
			query.maximumSearchNodes = maximumSearchNodes;
			query.maximumPoints = maximumRoutePoints;
			query.allowPartial = request.allowPartial;
			return QueryDetourGroundShard(shard, query);
		};
		std::vector<Ground::GroundPortalRecord> routePortals;
		routePortals.reserve(portalPath.coveragePath.size() > 1U ? portalPath.coveragePath.size() - 1U : 0U);
		for(std::size_t index = 1U; index < portalPath.coveragePath.size(); ++index) {
			const auto portal = std::find_if(portals.Portals().begin(), portals.Portals().end(), [&portalPath, index](const Ground::GroundPortalRecord& record) {
				return record.fromCoverage == portalPath.coveragePath[index - 1U] && record.toCoverage == portalPath.coveragePath[index];
			});
			if(portal == portals.Portals().end()) { result.outcome = NavigationOutcome::UnavailableCoverage; result.reason = NavigationReason::MissingNeighbor; return result; }
			routePortals.push_back(*portal);
		}
		const auto localPortalPoint = [](const Ground::GroundPortalRecord& portal,
			const NavigationCoverageKey& key) {
			const NavigationCoverageKey& from = portal.fromCoverage;
			const NavigationCoverageKey& to = portal.toCoverage;
			const double midpoint = (portal.intervalStart + portal.intervalEnd) * 0.5;
			// Boundary intervals are already in the shared transverse [0,256] local
			// frame of two adjacent shards. Derive only the longitudinal edge (0 or
			// 256) from the selected shard; this avoids converting distant int64 keys
			// into lossy global doubles while preserving the existing portal topology.
			const bool keyIsFrom = key == from;
			const bool positiveFrom = portal.boundaryAxis == 0U
				? to.shardX > from.shardX : to.shardZ > from.shardZ;
			const double longitudinal = keyIsFrom == positiveFrom ? 256.0 : 0.0;
			const double localX = portal.boundaryAxis == 0U ? longitudinal : midpoint;
			const double localZ = portal.boundaryAxis == 0U ? midpoint : longitudinal;
			// Each side retains its actual edge height. Keeping that local Y avoids
			// projecting an elevated floor to the vertical-band origin at a seam.
			const float localY = keyIsFrom ? portal.fromHeightMetres : portal.toHeightMetres;
			return NavigationGeometryVector3{static_cast<float>(localX), localY, static_cast<float>(localZ)};
		};
		std::vector<NavigationLocation> canonicalPoints;
		bool partialResult = false;
		NavigationReason partialReason = NavigationReason::DomainLimit;
		for(std::size_t index = 0U; index < portalPath.coveragePath.size(); ++index) {
			const NavigationCoverageKey& key = portalPath.coveragePath[index];
			const auto coverage = findCoverage(key);
			const auto shard = coverage == worldRoot->coverage.end() ? nullptr : findShard(*coverage);
			if(!shard) { result.outcome = NavigationOutcome::UnavailableCoverage; result.reason = NavigationReason::MissingNeighbor; return result; }
			const std::optional<NavigationGeometryVector3> localizedStart = index == 0U
				? localizeLocation(request.start, key)
				: std::optional<NavigationGeometryVector3>{localPortalPoint(routePortals[index - 1U], key)};
			const std::optional<NavigationGeometryVector3> localizedDestination = index + 1U == portalPath.coveragePath.size()
				? localizeLocation(request.destination, key)
				: std::optional<NavigationGeometryVector3>{localPortalPoint(routePortals[index], key)};
			if(!localizedStart || !localizedDestination) {
				result.outcome = NavigationOutcome::UnavailableCoverage;
				result.reason = NavigationReason::DomainLimit;
				return result;
			}
			const DetourGroundRouteResult segment = querySegment(*shard, *localizedStart, *localizedDestination);
			const bool segmentIsPartial = segment.outcome == NavigationOutcome::Partial && request.allowPartial;
			if(segment.outcome != NavigationOutcome::Complete && !segmentIsPartial) {
				result.outcome = segment.outcome;
				result.reason = segment.reason;
				return result;
			}
			for(const NavigationGeometryVector3& point : segment.points) {
				Scene::WorldPosition position = shardOrigin(key);
				position.offsetMetres.x += point.x;
				position.offsetMetres.y += point.y;
				position.offsetMetres.z += point.z;
				const Scene::WorldCoordinateResult normalized = Scene::NormalizeWorldPosition(position);
				NavigationLocation location{request.owner.world, normalized.position};
				if(!normalized.Succeeded() || !ValidateNavigationLocation(location).Succeeded()) {
					result.points.clear(); result.outcome = NavigationOutcome::UnavailableCoverage;
					result.reason = NavigationReason::ProviderFailure; return result;
				}
				if(canonicalPoints.empty() || canonicalPoints.back() != location) {
					// Multiple Detour shards may each emit a portal point. Enforce the
					// configured public route budget during stitching as well as inside
					// each provider query, otherwise a long portal chain could bypass it.
					if(canonicalPoints.size() >= maximumRoutePoints) {
						result.points.clear();
						result.outcome = NavigationOutcome::BudgetExhausted;
						result.reason = NavigationReason::OutputLimit;
						return result;
					}
					canonicalPoints.push_back(location);
				}
			}
			if(segmentIsPartial) {
				partialResult = true;
				partialReason = segment.reason;
				break;
			}
		}
		if(canonicalPoints.empty()) { result.outcome = NavigationOutcome::Unreachable; result.reason = NavigationReason::MissingSupport; return result; }
		if(partialResult) {
			// Keep the captured start as the first point, but deliberately do not
			// append request.destination: that endpoint is not reachable by this route.
			if(canonicalPoints.front() != request.start) {
				if(canonicalPoints.size() >= maximumRoutePoints) {
					result.outcome = NavigationOutcome::BudgetExhausted;
					result.reason = NavigationReason::OutputLimit;
					return result;
				}
				canonicalPoints.insert(canonicalPoints.begin(), request.start);
			}
			result.points = std::move(canonicalPoints);
			result.outcome = NavigationOutcome::Partial;
			result.reason = partialReason;
			return result;
		}
		// Detour returns points projected onto its polygon surface. Preserve those
		// interior waypoints, but keep the public route anchored to the exact
		// generation-qualified object pose and destination captured for this
		// request. Gameplay can therefore consume the first point without guessing
		// whether it represents the object's current position.
		if(canonicalPoints.front() != request.start) {
			if(canonicalPoints.size() >= maximumRoutePoints) {
				result.outcome = NavigationOutcome::BudgetExhausted;
				result.reason = NavigationReason::OutputLimit;
				return result;
			}
			canonicalPoints.insert(canonicalPoints.begin(), request.start);
		}
		else
			canonicalPoints.front() = request.start;
		if(canonicalPoints.back() != request.destination) {
			if(canonicalPoints.size() >= maximumRoutePoints) {
				result.points.clear();
				result.outcome = NavigationOutcome::BudgetExhausted;
				result.reason = NavigationReason::OutputLimit;
				return result;
			}
			canonicalPoints.push_back(request.destination);
		}
		else
			canonicalPoints.back() = request.destination;
		result.points = std::move(canonicalPoints);
		result.outcome = NavigationOutcome::Complete;
		result.reason = NavigationReason::None;
		return result;
	}

	NavigationSystem::PreparedGroundBuild NavigationSystem::PrepareGroundBuildPacket(
		const PendingGroundBuild& groundBuild,
		const std::optional<SavedNavigationCacheCandidate>& savedCacheCandidate,
		const std::shared_ptr<NavigationBudget>& budget) {
		// This function receives only values retained by the protected capture. It is
		// intentionally static so a ThreadPool job cannot accidentally access this
		// system, a mutable world root, ECS storage, or a live terrain provider.
		RecastGroundBuildInput buildInput{};
		buildInput.profile = groundBuild.profile;
		buildInput.sources = groundBuild.sources;
		buildInput.budget = budget.get();
		PreparedGroundBuild prepared{};
		prepared.profile = groundBuild.profile;
		prepared.sourceIndex = groundBuild.sourceIndex;
		prepared.eligibleSourceIndex = groundBuild.eligibleSourceIndex;
		prepared.terrainPublicationGeneration = groundBuild.terrainPublicationGeneration;
		prepared.profileConfigurationRevision = groundBuild.profileConfigurationRevision;
		std::vector<GroundTileBlob> assembledTiles;
		if(savedCacheCandidate) {
			prepared.cacheCandidateAttempted = true;
			const GroundCoverageEvidence evidence = CaptureGroundCoverageEvidence(buildInput);
			SavedNavigationDescriptor expected{};
			expected.formatVersion = kSavedNavigationFormatVersion;
			expected.backendAbi = std::string(kSavedNavigationBackendAbi);
			expected.sourceFingerprint = evidence.sourceFingerprint;
			expected.settingsFingerprint = evidence.settingsFingerprint;
			const SavedNavigationReadResult decoded = evidence.accepted
				? ReadSavedNavigation(savedCacheCandidate->bytes, expected) : SavedNavigationReadResult{};
			const SavedGroundTileImportResult imported = decoded.accepted
				? ImportSavedGroundTiles(decoded, evidence, budget.get()) : SavedGroundTileImportResult{};
			if(imported.accepted) {
				prepared.cacheImported = true;
				prepared.cacheEvidence = evidence;
				assembledTiles = imported.tiles;
			}
			else {
				prepared.cacheRejectionReason = !evidence.accepted ? evidence.reason
					: !decoded.accepted ? decoded.reason : imported.reason;
			}
		}
		if(!prepared.cacheImported) {
			// A rejected cache always falls through to a fresh build from this same
			// copied packet; neither branch can expose partially imported coverage.
			const RecastGroundBuildResult fresh = BuildRecastGroundTiles(buildInput);
			if(fresh.Succeeded()) {
				assembledTiles = fresh.tiles;
				prepared.cacheEvidence = CaptureGroundCoverageEvidence(buildInput);
			}
			else {
				const DeferredProfileBuildFailure failure = ToDeferredProfileBuildFailure(fresh.status);
				prepared.failureOutcome = failure.outcome;
				prepared.failureReason = failure.reason;
				prepared.failureDetail = fresh.reason;
				if(prepared.cacheRejectionReason.empty()) prepared.cacheRejectionReason = fresh.reason;
			}
		}
		auto assembleTiles = [&prepared, &groundBuild, &budget](const std::vector<GroundTileBlob>& tiles) {
			if(tiles.empty()) return false;
			using GroupKey = std::tuple<std::int64_t, std::int64_t, std::int32_t>;
			std::map<GroupKey, std::vector<GroundTileBlob>> groups;
			prepared.shards.clear();
			prepared.coverage.clear();
			prepared.pristineTiles = tiles;
			for(const GroundTileBlob& tile : tiles)
				groups[{FloorShard(tile.key.x), FloorShard(tile.key.z), tile.key.verticalBand}].push_back(tile);
			for(auto& [key, groupTiles] : groups) {
				GroundShardKey shard{std::get<0>(key), std::get<1>(key), std::get<2>(key),
					MakeGroundProfileBuildKey(groundBuild.profile)};
				DetourGroundShardBuildInput shardInput{shard, 1U, std::move(groupTiles)};
				// The retained proof may outlive this worker packet, so it shares the same
				// durable async budget backing as the Recast input and provider shard.
				shardInput.budget = budget.get();
				shardInput.minimumClearanceMetres = static_cast<float>(groundBuild.profile.clearanceMarginMetres);
				const DetourGroundShardBuildResult assembled = AssembleDetourGroundShard(shardInput);
				if(!assembled.accepted) {
					prepared.shards.clear();
					prepared.coverage.clear();
					prepared.pristineTiles.clear();
					return false;
				}
				prepared.shards.push_back(assembled.generation);
				prepared.coverage.push_back({{shard.x, shard.z, shard.verticalBand, shard.profileBuildKey},
					assembled.generation->reference.generation, NavigationCoverageState::Ready, 0U, true});
			}
			return true;
		};
		prepared.accepted = assembleTiles(assembledTiles);
		if(!prepared.accepted && prepared.failureDetail.empty()) {
			prepared.failureOutcome = NavigationOutcome::Failed;
			prepared.failureReason = NavigationReason::ProviderFailure;
			prepared.failureDetail = "DetourGroundShardAssemblyRejected";
		}
		if(!prepared.accepted && prepared.cacheImported) {
			// A cached payload must pass the same provider assembly as fresh tiles.
			prepared.cacheImported = false;
			prepared.cacheRejectionReason = "SavedNavigationDetourAssemblyRejected";
			const RecastGroundBuildResult fresh = BuildRecastGroundTiles(buildInput);
			if(fresh.Succeeded()) {
				prepared.cacheEvidence = CaptureGroundCoverageEvidence(buildInput);
				prepared.accepted = assembleTiles(fresh.tiles);
			}
			else {
				prepared.cacheRejectionReason = fresh.reason;
				const DeferredProfileBuildFailure failure = ToDeferredProfileBuildFailure(fresh.status);
				prepared.failureOutcome = failure.outcome;
				prepared.failureReason = failure.reason;
				prepared.failureDetail = fresh.reason;
			}
			if(!prepared.accepted && prepared.failureDetail.empty()) {
				prepared.failureOutcome = NavigationOutcome::Failed;
				prepared.failureReason = NavigationReason::ProviderFailure;
				prepared.failureDetail = "DetourGroundShardAssemblyRejected";
			}
		}
		return prepared;
	}

	NavigationSystem::PreparedDynamicRebuild NavigationSystem::PrepareDynamicBuildPacket(
		const PendingDynamicRebuild& dynamicBuild, const std::shared_ptr<NavigationBudget>& budget) {
		// Dynamic rebuild preparation follows the same value-only rule as bootstrap
		// work. Its result still requires the existing protected portal/root commit.
		NavigationDynamicRebuildRequest request = dynamicBuild.request;
		request.budget = budget.get();
		return {dynamicBuild.change, request.profile, PrepareDynamicGroundReplacement(request)};
	}

	std::size_t NavigationSystem::EstimateGroundBuildPacketBytes(const PendingGroundBuild& build,
		const std::optional<SavedNavigationCacheCandidate>& savedCacheCandidate) noexcept {
		// A copied packet keeps immutable source snapshots alive after the scene can
		// revoke them. Charge that retained geometry conservatively, as well as the
		// copied pointer/index/cache vectors, until the worker actually completes.
		std::size_t bytes = EstimateRetainedGeometryBytes(build.sources);
		bytes = SaturatingAdd(bytes, sizeof(PendingGroundBuild));
		bytes = SaturatingAdd(bytes, build.sources.capacity() * sizeof(build.sources.front()) * 2U);
		bytes = SaturatingAdd(bytes, build.sourceIndex.capacity() * sizeof(NavigationSourceIndexRecord) * 2U);
		return SaturatingAdd(bytes, savedCacheCandidate ? savedCacheCandidate->bytes.size() : 0U);
	}

	std::size_t NavigationSystem::EstimateDynamicBuildPacketBytes(const PendingDynamicRebuild& build) noexcept {
		const NavigationDynamicRebuildRequest& request = build.request;
		std::size_t bytes = EstimateRetainedGeometryBytes(request.sources);
		bytes = SaturatingAdd(bytes, sizeof(PendingDynamicRebuild));
		bytes = SaturatingAdd(bytes, request.sources.capacity() * sizeof(request.sources.front()) * 2U);
		bytes = SaturatingAdd(bytes, request.sourceIndex.capacity() * sizeof(NavigationSourceIndexRecord) * 2U);
		bytes = SaturatingAdd(bytes, request.pendingChanges.capacity() * sizeof(NavigationDynamicSourceChange));
		bytes = SaturatingAdd(bytes, request.dirtyCoverage.capacity() * sizeof(NavigationDirtyCoverageDependency));
		// Reuse baselines retain shared published tile packs. Charge only the copied
		// pointer records here; PublishedStoreUsage accounts the actual pack at its
		// shard owner so an async packet cannot double-count its immutable payload.
		bytes = SaturatingAdd(bytes, request.reuseBaselines.capacity() * sizeof(NavigationDynamicShardReuseBaseline));
		return SaturatingAdd(bytes, request.tombstones.entries.capacity() * sizeof(NavigationSourceTombstone));
	}

	void NavigationSystem::PublishAsyncBuildDiagnosticsLocked() {
		const NavigationAsyncBuildStatus status = asyncBuildCoordinator_.Status();
		dynamicDiagnostics_.asyncBuildCurrentCount = status.currentCount;
		dynamicDiagnostics_.asyncBuildRetiredCount = status.retiredCount;
		dynamicDiagnostics_.asyncBuildChargedBytes = status.chargedBytes;
		dynamicDiagnostics_.asyncBuildPeakChargedBytes = status.peakChargedBytes;
		dynamicDiagnostics_.asyncBuildAdmissionRejectedCount = status.admissionRejectedCount;
		dynamicDiagnostics_.asyncBuildCompletionCount = status.completedCount;
		dynamicDiagnostics_.asyncBuildStaleDiscardCount = status.staleDiscardCount;
		dynamicDiagnostics_.asyncBuildGroundCompletionCount = status.groundCompletedCount;
		dynamicDiagnostics_.asyncBuildDynamicCompletionCount = status.dynamicCompletedCount;
		dynamicDiagnostics_.asyncBuildTotalWorkerNanoseconds = status.totalWorkerNanoseconds;
		dynamicDiagnostics_.asyncBuildMaxWorkerNanoseconds = status.maxWorkerNanoseconds;
		dynamicDiagnostics_.asyncBuildTotalQueueWaitNanoseconds = status.totalQueueWaitNanoseconds;
		dynamicDiagnostics_.asyncBuildMaxQueueWaitNanoseconds = status.maxQueueWaitNanoseconds;
		dynamicDiagnostics_.asyncBuildTotalPublicationWaitNanoseconds = status.totalPublicationWaitNanoseconds;
		dynamicDiagnostics_.asyncBuildMaxPublicationWaitNanoseconds = status.maxPublicationWaitNanoseconds;
		diagnostics_.PublishDynamic(dynamicDiagnostics_);
	}

	void NavigationSystem::RetireAsyncBuildsLocked() {
		asyncBuildCoordinator_.RetireCurrent();
		activeAsyncBuild_.reset();
		// The coordinator retains every retired child permit until the worker has
		// really finished. The group owns only protected bookkeeping, so dropping it
		// here cannot free copied provider input early or let a revoked lifetime
		// publish a partial root later.
		activeDynamicBuildGroup_.reset();
		preparedDynamicBuildGroup_.reset();
	}

	void NavigationSystem::DrainCompletedAsyncBuildsLocked() {
		for(const NavigationAsyncBuildCompletion& completion : asyncBuildCoordinator_.PollCompleted()) {
			if(completion.retired || !world_ || completion.identity.world != world_->Handle()
				|| completion.identity.lifecycleEpoch != asyncBuildEpoch_) {
				// Retired work is intentionally discarded. Its receipt group was revoked
				// with the world, so it must neither publish nor acknowledge later work.
				++dynamicDiagnostics_.staleDropCount;
				continue;
			}

			// Dynamic recovery children belong to one immutable receipt group. Keep
			// their independent worker results private until every child wave is
			// terminal; publishing one profile here would recreate the serial-root
			// exchange that grouping is intended to remove.
			if(activeDynamicBuildGroup_) {
				auto child = std::find_if(activeDynamicBuildGroup_->activeChildren.begin(),
					activeDynamicBuildGroup_->activeChildren.end(), [&completion](const ActiveDynamicBuildChild& current) {
						return current.identity == completion.identity;
					});
				if(child != activeDynamicBuildGroup_->activeChildren.end()) {
					std::shared_ptr<AsyncBuildResultState> result = std::move(child->result);
					activeDynamicBuildGroup_->activeChildren.erase(child);
					PreparedDynamicRebuild prepared{};
					bool hasPreparedResult = false;
					if(completion.succeeded && result && result->dynamicBuild) {
						prepared = std::move(*result->dynamicBuild);
						hasPreparedResult = true;
					}
					else if(result && result->dynamicPacket) {
						// An exception is a typed permanent provider failure for this
						// exact copied profile. A newer receipt is rejected above by its
						// lifetime/identity, so it cannot terminalize fresh deferred work.
						prepared.change = result->dynamicPacket->change;
						prepared.profile = result->dynamicPacket->request.profile;
						prepared.plan.reason = "NavigationAsyncBuildException";
						prepared.plan.failureOutcome = NavigationOutcome::Failed;
						prepared.plan.failureReason = NavigationReason::ProviderFailure;
						prepared.plan.failurePermanent = true;
						hasPreparedResult = true;
						++dynamicDiagnostics_.rejectedUpdateCount;
					}
					if(!hasPreparedResult) {
						++dynamicDiagnostics_.staleDropCount;
					}
					else if(prepared.plan.accepted) {
						activeDynamicBuildGroup_->successfulChildren.push_back(std::move(prepared));
					}
					else {
						activeDynamicBuildGroup_->failedChildren.push_back(std::move(prepared));
					}
					continue;
				}
			}

			if(!activeAsyncBuild_ || completion.identity != activeAsyncBuild_->identity) {
				++dynamicDiagnostics_.staleDropCount;
				continue;
			}

			std::shared_ptr<AsyncBuildResultState> result = activeAsyncBuild_->result;
			if(completion.succeeded && result) {
				if(result->groundBuild) preparedGroundBuilds_.push_back(std::move(*result->groundBuild));
				if(result->dynamicBuild) preparedDynamicRebuilds_.push_back(std::move(*result->dynamicBuild));
			}
			else if(result) {
				// A current worker exception must cross the same protected publication
				// boundary as an ordinary provider failure. Re-queuing the original packet
				// would retry forever and strand callers deferred behind this recovery.
				// Do not retain exception text: it may be arbitrary/unbounded provider data.
				if(result->groundPacket) {
					PreparedGroundBuild failedBuild{};
					failedBuild.failureOutcome = NavigationOutcome::Failed;
					failedBuild.failureReason = NavigationReason::ProviderFailure;
					failedBuild.failureDetail = "NavigationAsyncBuildException";
					failedBuild.cacheCandidateAttempted = result->savedCacheCandidate.has_value();
					failedBuild.cacheRejectionReason = "NavigationAsyncBuildException";
					failedBuild.profile = result->groundPacket->profile;
					failedBuild.eligibleSourceIndex = result->groundPacket->eligibleSourceIndex;
					failedBuild.terrainPublicationGeneration = result->groundPacket->terrainPublicationGeneration;
					failedBuild.profileConfigurationRevision = result->groundPacket->profileConfigurationRevision;
					preparedGroundBuilds_.push_back(std::move(failedBuild));
				}
				if(result->dynamicPacket) {
					PreparedDynamicRebuild failedBuild{};
					failedBuild.change = result->dynamicPacket->change;
					failedBuild.profile = result->dynamicPacket->request.profile;
					failedBuild.plan.reason = "NavigationAsyncBuildException";
					failedBuild.plan.failureOutcome = NavigationOutcome::Failed;
					failedBuild.plan.failureReason = NavigationReason::ProviderFailure;
					failedBuild.plan.failurePermanent = true;
					preparedDynamicRebuilds_.push_back(std::move(failedBuild));
				}
				++dynamicDiagnostics_.rejectedUpdateCount;
			}
			activeAsyncBuild_.reset();
		}
		if(activeDynamicBuildGroup_ && activeDynamicBuildGroup_->activeChildren.empty()
			&& activeDynamicBuildGroup_->pendingChildren.empty()) {
			// Only this protected poll observes all child futures. Moving the group now
			// creates a single publishable unit without any worker waiting on a sibling.
			PreparedDynamicBuildGroup prepared{};
			prepared.change = activeDynamicBuildGroup_->change;
			prepared.world = activeDynamicBuildGroup_->world;
			prepared.expectedRootGeneration = activeDynamicBuildGroup_->expectedRootGeneration;
			prepared.expectedTerrainPublicationGeneration =
				activeDynamicBuildGroup_->expectedTerrainPublicationGeneration;
			prepared.expectedSourceIndexRevision = activeDynamicBuildGroup_->expectedSourceIndexRevision;
			prepared.requiredProfileBuildKeys = std::move(activeDynamicBuildGroup_->requiredProfileBuildKeys);
			prepared.successfulChildren = std::move(activeDynamicBuildGroup_->successfulChildren);
			prepared.failedChildren = std::move(activeDynamicBuildGroup_->failedChildren);
			preparedDynamicBuildGroup_ = std::move(prepared);
			activeDynamicBuildGroup_.reset();
		}
		PublishAsyncBuildDiagnosticsLocked();
	}

	void NavigationSystem::PrepareAvailableWork(bool prepareRoutes) {
		std::optional<PendingGroundBuild> groundBuild;
		std::optional<SavedNavigationCacheCandidate> savedCacheCandidate;
		std::optional<PendingDynamicRebuild> dynamicBuild;
		std::vector<PendingDynamicRebuild> dynamicBuildWave;
		std::vector<PendingRoute> routes;
		bool capturedDynamicGroup = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			DrainCompletedAsyncBuildsLocked();
			const NavigationAsyncBuildStatus asyncStatus = asyncBuildCoordinator_.Status();
			const bool asyncSlotAvailable = asyncBuildBudget_ != nullptr && !activeAsyncBuild_
				&& asyncStatus.currentCount + asyncStatus.retiredCount < asyncStatus.capacity;
			// A completed provider packet still names the root from which it was
			// captured. PublishPreparedWork exchanges that root later in this same
			// scheduler frame. Starting another ground or dynamic packet before that
			// protected exchange would make a sibling profile search against the old
			// root, only for publication to discard it as stale on the next boundary.
			//
			// Keep route capture independent: route workers are explicitly
			// generation-qualified at publication. Provider work instead changes the
			// shared root/portal tuple, so it must wait one protected boundary for the
			// earlier provider result to resolve.
			const bool providerPublicationPending = !preparedGroundBuilds_.empty()
				|| !preparedDynamicRebuilds_.empty() || preparedDynamicBuildGroup_.has_value();
			// Direct Update remains the synchronous compatibility path used by bounded
			// native fixtures and hosts without scheduler frame workloads. Only the
			// protected scheduler capture launches background provider preparation.
			// A compatibility host may prepare synchronously only after former async
			// packets drain. Otherwise it could bypass the cross-epoch task budget.
			const bool canCaptureBuild = !providerPublicationPending
				&& ((prepareRoutes && !activeAsyncBuild_
					&& asyncStatus.currentCount == 0U && asyncStatus.retiredCount == 0U)
					|| asyncSlotAvailable);
			if(canCaptureBuild && !activeDynamicBuildGroup_ && !pendingGroundBuilds_.empty()) {
				groundBuild = std::move(pendingGroundBuilds_.front());
				pendingGroundBuilds_.pop_front();
				const std::uint64_t profileKey = MakeGroundProfileBuildKey(groundBuild->profile);
				if(savedGroundCacheCandidate_ && !savedGroundCacheStatus_.cacheUsed
					&& !savedGroundCacheImportInFlight_
					&& !savedGroundCacheAttemptedProfiles_.contains(profileKey)
					&& (!savedGroundCacheCandidate_->profile
						|| MakeGroundProfileBuildKey(*savedGroundCacheCandidate_->profile) == profileKey)) {
					// The worker receives a full byte copy and a full immutable geometry
					// packet. It cannot consult the mount, ECS, or mutable world state.
					savedCacheCandidate = *savedGroundCacheCandidate_;
					savedGroundCacheImportInFlight_ = true;
				}
			}
			// A dynamic group captures the root tuple it will later validate at
			// publication. Do not capture it while another provider packet can still
			// exchange that tuple at this frame's protected boundary; leaving the
			// receipt queued lets the next capture use the newly published root.
			if(canCaptureBuild && !groundBuild && (prepareRoutes ? pendingDynamicRebuilds_.empty()
				: !activeDynamicBuildGroup_ && !preparedDynamicBuildGroup_) && !pendingDynamicChanges_.empty()
				&& world_ && dirtyIndex_.LatestChangeSequence() == ChangeSequence(pendingDynamicChanges_.front())
				&& dirtyIndex_.LatestChangeSequence(pendingDynamicChanges_.front().source)
					== ChangeSequence(pendingDynamicChanges_.front())) {
				const NavigationDynamicSourceChange change = pendingDynamicChanges_.front();
				const std::shared_ptr<const NavigationWorldRoot> root = world_->PublishedRoot();
				if(root) {
					const NavigationGeneration sequence = ChangeSequence(change);
					if(!dynamicRebuildProgress_ || dynamicRebuildProgress_->changeSequence != sequence) {
						DynamicRebuildProgress progress{};
						progress.changeSequence = sequence;
						for(const DynamicSourceStage& stage : dynamicSourceStages_)
							progress.requiredProfileBuildKeys.push_back(MakeGroundProfileBuildKey(stage.profile));
						std::sort(progress.requiredProfileBuildKeys.begin(), progress.requiredProfileBuildKeys.end());
						progress.requiredProfileBuildKeys.erase(std::unique(progress.requiredProfileBuildKeys.begin(),
							progress.requiredProfileBuildKeys.end()), progress.requiredProfileBuildKeys.end());
						dynamicRebuildProgress_ = std::move(progress);
					}

					std::vector<PendingDynamicRebuild> rebuiltProfiles;
					bool everyRequiredProfileReady = !dynamicRebuildProgress_->requiredProfileBuildKeys.empty();
					for(const DynamicSourceStage& stage : dynamicSourceStages_) {
						const std::uint64_t profileKey = MakeGroundProfileBuildKey(stage.profile);
						if(std::binary_search(dynamicRebuildProgress_->publishedProfileBuildKeys.begin(),
							dynamicRebuildProgress_->publishedProfileBuildKeys.end(), profileKey)) continue;
						if(std::binary_search(dynamicRebuildProgress_->terminalFailedProfileBuildKeys.begin(),
							dynamicRebuildProgress_->terminalFailedProfileBuildKeys.end(), profileKey)) {
							// A current receipt already produced a typed permanent failure for
							// this profile. A later scene receipt may retry it, but rebuilding
							// the same immutable packet would otherwise spin forever.
							everyRequiredProfileReady = false;
							continue;
						}
						std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
						sources.reserve(stage.sources.size());
						for(const auto& source : stage.sources) {
							if(!source) continue;
							const NavigationGeometrySnapshotDescriptor& descriptor = source->Descriptor();
							if(descriptor.sourceUuid == change.source) {
								// A removal must not reintroduce deleted geometry. For a
								// replacement, retain only the exact committed revision.
								if(!RequiresReplacementSnapshot(change.kind)
									|| descriptor.sourceRevision != change.sourceRevision) continue;
							}
							sources.push_back(source);
						}
						if(RequiresReplacementSnapshot(change.kind) && !HasCommittedSnapshot(sources, change)) {
							everyRequiredProfileReady = false;
							break;
						}
						NavigationDynamicRebuildRequest request{};
						request.change = change;
						request.expectedRootGeneration = root->rootGeneration;
						request.expectedTerrainPublicationGeneration = root->terrainPublicationGeneration;
						request.expectedSourceIndexRevision = root->sourceIndexRevision;
						request.profile = stage.profile;
						request.tombstones = dirtyIndex_.ObserveAtProtectedBoundary();
						request.pendingChanges = dirtyIndex_.ObservePendingChanges();
						request.dirtyCoverage = dirtyIndex_.DirtyCoverage();
						request.sourceIndex = root->sources;
						request.sources = std::move(sources);
						const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(request.profile);
						const auto floorShard = [](std::int64_t tile) noexcept {
							std::int64_t quotient = tile / 8LL;
							if(tile % 8LL < 0LL) --quotient;
							return quotient;
						};
						for(const std::shared_ptr<const DetourGroundShardGeneration>& shard : publishedShards_) {
							if(!shard || shard->reference.shard.profileBuildKey != profileBuildKey
								|| !shard->retainedTiles || shard->retainedTiles->tiles.empty()) continue;
							const bool isDirtyShard = std::any_of(request.dirtyCoverage.begin(), request.dirtyCoverage.end(),
								[&shard, &floorShard](const NavigationDirtyCoverageDependency& dirty) {
									return floorShard(dirty.tileX) == shard->reference.shard.x
										&& floorShard(dirty.tileZ) == shard->reference.shard.z
										&& dirty.verticalBand == shard->reference.shard.verticalBand;
								});
							if(!isDirtyShard) continue;
							const NavigationCoverageKey coverageKey{shard->reference.shard.x, shard->reference.shard.z,
								shard->reference.shard.verticalBand, profileBuildKey};
							const auto coverage = std::find_if(root->coverage.begin(), root->coverage.end(),
								[&coverageKey](const NavigationCoverageRecord& current) {
									return current.key == coverageKey;
								});
							// Dirty coverage still owns the prior complete shard. It is precisely
							// the immutable baseline a worker may reuse while rebuilding its
							// receipt halo; route admission remains fail-closed until replacement.
							if(coverage == root->coverage.end()
								|| (coverage->state != NavigationCoverageState::Ready
									&& coverage->state != NavigationCoverageState::Dirty)
								|| (coverage->state == NavigationCoverageState::Ready && !coverage->complete)
								|| coverage->coverageGeneration != shard->reference.generation) continue;
							request.reuseBaselines.push_back({shard->reference, root->rootGeneration,
								root->terrainPublicationGeneration, root->sourceIndexRevision, shard->retainedTiles});
						}
						rebuiltProfiles.push_back({change, std::move(request)});
					}
					std::vector<std::uint64_t> availableProfileBuildKeys =
						dynamicRebuildProgress_->publishedProfileBuildKeys;
					for(const PendingDynamicRebuild& pending : rebuiltProfiles)
						availableProfileBuildKeys.push_back(MakeGroundProfileBuildKey(pending.request.profile));
					std::sort(availableProfileBuildKeys.begin(), availableProfileBuildKeys.end());
					availableProfileBuildKeys.erase(std::unique(availableProfileBuildKeys.begin(),
						availableProfileBuildKeys.end()), availableProfileBuildKeys.end());
					everyRequiredProfileReady = everyRequiredProfileReady
						&& std::includes(availableProfileBuildKeys.begin(), availableProfileBuildKeys.end(),
							dynamicRebuildProgress_->requiredProfileBuildKeys.begin(),
							dynamicRebuildProgress_->requiredProfileBuildKeys.end());
					// A change is admitted as a profile group. Do not let one profile build
					// from the new snapshot while another still lacks that exact immutable
					// revision; the dirty certificate must remain atomic across the group.
					if(everyRequiredProfileReady && !rebuiltProfiles.empty()) {
						if(prepareRoutes) {
							// Synchronous Update remains a compact compatibility path for
							// native fixtures that do not execute scheduler worker tasks.
							pendingDynamicRebuilds_.insert(pendingDynamicRebuilds_.end(),
								std::make_move_iterator(rebuiltProfiles.begin()), std::make_move_iterator(rebuiltProfiles.end()));
						}
						else {
							ActiveDynamicBuildGroup group{};
							group.change = change;
							group.world = world_->Handle();
							group.expectedRootGeneration = root->rootGeneration;
							group.expectedTerrainPublicationGeneration = root->terrainPublicationGeneration;
							group.expectedSourceIndexRevision = root->sourceIndexRevision;
							group.requiredProfileBuildKeys = dynamicRebuildProgress_->requiredProfileBuildKeys;
							group.pendingChildren = std::move(rebuiltProfiles);
							activeDynamicBuildGroup_ = std::move(group);
							capturedDynamicGroup = true;
						}
					}
				}
				// Leave the committed change queued when the source bridge has not yet
				// supplied its matching immutable revision. Preparing from an old mesh
				// would make a dynamic obstacle silently traversable again.
				if((prepareRoutes && !pendingDynamicRebuilds_.empty()) || capturedDynamicGroup)
					pendingDynamicChanges_.pop_front();
			} else if(!pendingDynamicChanges_.empty()
				&& dirtyIndex_.LatestChangeSequence() != ChangeSequence(pendingDynamicChanges_.front())) {
				// A newer globally ordered change rebuilds the merged dirty set, so an
				// older packet can never pass the publication revision check.
				pendingDynamicChanges_.pop_front();
			}
			if(prepareRoutes && canCaptureBuild && !groundBuild && !pendingDynamicRebuilds_.empty()) {
				dynamicBuild = std::move(pendingDynamicRebuilds_.front());
				pendingDynamicRebuilds_.pop_front();
			}
			if(!prepareRoutes && canCaptureBuild && !groundBuild && activeDynamicBuildGroup_
				&& activeDynamicBuildGroup_->activeChildren.empty()
				&& !activeDynamicBuildGroup_->pendingChildren.empty()) {
				// One bounded wave uses both coordinator permits. The worker tasks do not
				// wait on each other; this protected boundary polls them before capturing
				// a later wave and publishes the whole group only when every child ended.
				const std::size_t occupied = asyncStatus.currentCount + asyncStatus.retiredCount;
				const std::size_t available = occupied < asyncStatus.capacity ? asyncStatus.capacity - occupied : 0U;
				const std::size_t count = std::min({std::size_t{2U}, available,
					activeDynamicBuildGroup_->pendingChildren.size()});
				dynamicBuildWave.reserve(count);
				for(std::size_t index = 0U; index < count; ++index) {
					dynamicBuildWave.push_back(std::move(activeDynamicBuildGroup_->pendingChildren.front()));
					activeDynamicBuildGroup_->pendingChildren.erase(activeDynamicBuildGroup_->pendingChildren.begin());
				}
			}
			if(prepareRoutes) {
				// Compatibility Update has no task-graph range callback, so it drains a
				// bounded batch itself. This avoids the historical one-route-per-frame
				// ceiling without allowing an unbounded caller queue to monopolize a tick.
				constexpr std::size_t kCompatibilityRouteBatchCapacity = 256U;
				const std::size_t count = std::min(kCompatibilityRouteBatchCapacity, pendingRoutes_.size());
				routes.reserve(count);
				for(std::size_t index = 0U; index < count; ++index) {
					routes.push_back(std::move(pendingRoutes_.front()));
					pendingRoutes_.pop_front();
				}
				const RouteClock::time_point capturedAt = RouteClock::now();
				for(PendingRoute& route : routes) {
					if(!route.captureTimingRecorded) {
						RecordRouteDuration(dynamicDiagnostics_.routeAdmissionToCaptureCount,
							dynamicDiagnostics_.routeAdmissionToCaptureNanoseconds,
							dynamicDiagnostics_.routeAdmissionToCaptureMaxNanoseconds,
							route.readyAdmissionTime, capturedAt);
						route.captureTimingRecorded = true;
					}
					route.capturedTime = capturedAt;
				}
				lastRoutePreparationBatch_ = count;
				routePreparationBatchHighWater_ = std::max(routePreparationBatchHighWater_, count);
			}
		}

		if(!prepareRoutes && !dynamicBuildWave.empty()) {
			std::lock_guard<std::mutex> lock(mutex_);
			// A world transition can retire the group after Capture released its lock.
			// In that case dropping the local copies is safe: RetireCurrent keeps any
			// already-admitted permits alive, while these never entered a worker.
			if(activeDynamicBuildGroup_ && world_
				&& activeDynamicBuildGroup_->world == world_->Handle()
				&& activeDynamicBuildGroup_->activeChildren.empty()) {
				std::vector<NavigationAsyncBuildCoordinator::Submission> submissions;
				std::vector<ActiveDynamicBuildChild> children;
				submissions.reserve(dynamicBuildWave.size());
				children.reserve(dynamicBuildWave.size());
				const std::shared_ptr<NavigationBudget> budget = asyncBuildBudget_;
				for(PendingDynamicRebuild& packet : dynamicBuildWave) {
					const std::size_t packetBytes = EstimateDynamicBuildPacketBytes(packet);
					if(!budget || packetBytes > budget->LimitBytes(NavigationBudgetCategory::BuildScratch)) {
						// A packet larger than the configured category can never fit, even
						// after unrelated retired work drains. Terminalize this exact profile
						// through the normal grouped failure publication instead of retrying a
						// permanently impossible two-child wave forever.
						PreparedDynamicRebuild failed{};
						failed.change = packet.change;
						failed.profile = packet.request.profile;
						failed.plan.reason = "NavigationAsyncBuildPacketExceedsBuildScratch";
						failed.plan.failureOutcome = NavigationOutcome::BudgetExhausted;
						failed.plan.failureReason = NavigationReason::OutputLimit;
						failed.plan.failurePermanent = true;
						activeDynamicBuildGroup_->failedChildren.push_back(std::move(failed));
						continue;
					}
					auto result = std::make_shared<AsyncBuildResultState>();
					result->dynamicPacket = std::move(packet);
					NavigationAsyncBuildIdentity identity{};
					identity.world = activeDynamicBuildGroup_->world;
					identity.lifecycleEpoch = asyncBuildEpoch_;
					identity.groupSequence = ChangeSequence(activeDynamicBuildGroup_->change);
					identity.profileBuildKey = MakeGroundProfileBuildKey(result->dynamicPacket->request.profile);
					identity.dynamicRebuild = true;
					submissions.push_back({identity, budget, packetBytes, [result, budget]() {
						result->dynamicBuild = NavigationSystem::PrepareDynamicBuildPacket(
							*result->dynamicPacket, budget);
					}});
					children.push_back({identity, std::move(result)});
				}
				std::string rejectionReason;
				// Every packet can be classified as permanently over-budget. In that
				// case there is no worker submission to reject; the next protected poll
				// moves the accumulated typed failures into group publication.
				bool submitted = children.empty()
					|| asyncBuildCoordinator_.TrySubmitBatch(std::move(submissions), rejectionReason);
				bool usedSingleChildFallback = false;
				if(!submitted && children.size() > 1U) {
					// Two children are preferred, not required. A combined scratch budget can
					// legitimately reject the wave even though one profile fits. Retry one
					// immutable child now; otherwise the same pair would be deferred forever.
					ActiveDynamicBuildChild& first = children.front();
					if(first.result && first.result->dynamicPacket) {
						const std::size_t packetBytes = EstimateDynamicBuildPacketBytes(*first.result->dynamicPacket);
						std::vector<NavigationAsyncBuildCoordinator::Submission> oneChild;
						oneChild.push_back({first.identity, budget, packetBytes, [result = first.result, budget]() {
							result->dynamicBuild = NavigationSystem::PrepareDynamicBuildPacket(
								*result->dynamicPacket, budget);
						}});
						submitted = asyncBuildCoordinator_.TrySubmitBatch(std::move(oneChild), rejectionReason);
						if(submitted) {
							activeDynamicBuildGroup_->activeChildren.push_back(std::move(first));
							children.erase(children.begin());
							usedSingleChildFallback = true;
						}
					}
				}
				if(submitted && !usedSingleChildFallback && !children.empty()) {
					// A successful two-child batch keeps both children. The one-child budget
					// fallback moved its admitted child above and restores the rest below.
					activeDynamicBuildGroup_->activeChildren.insert(activeDynamicBuildGroup_->activeChildren.end(),
						std::make_move_iterator(children.begin()), std::make_move_iterator(children.end()));
					children.clear();
				}
				if(!submitted) {
					// Batch admission is all-or-nothing. Restore FIFO order without creating
					// an untracked child whose copied packet could outlive this group.
					for(auto child = children.rbegin(); child != children.rend(); ++child) {
						if(child->result && child->result->dynamicPacket)
							activeDynamicBuildGroup_->pendingChildren.insert(
								activeDynamicBuildGroup_->pendingChildren.begin(),
								std::move(*child->result->dynamicPacket));
					}
					diagnostics_.Append({NavigationDiagnosticSeverity::Information,
						NavigationMeasurementReason::MeasurementUnavailable, lastProtectedBoundaryGeneration_, 0U,
						"navigation.async_dynamic_group_deferred", rejectionReason});
				}
				else if(usedSingleChildFallback && !children.empty()) {
					// The one-child fallback admitted exactly one profile. Put all remaining
					// profiles back at the front in their original capture order for the next
					// protected wave.
					for(auto child = children.rbegin(); child != children.rend(); ++child) {
						if(child->result && child->result->dynamicPacket)
							activeDynamicBuildGroup_->pendingChildren.insert(
								activeDynamicBuildGroup_->pendingChildren.begin(),
								std::move(*child->result->dynamicPacket));
					}
				}
				PublishAsyncBuildDiagnosticsLocked();
			}
		}

		if(!prepareRoutes && (groundBuild || dynamicBuild)) {
			auto result = std::make_shared<AsyncBuildResultState>();
			result->groundPacket = std::move(groundBuild);
			result->savedCacheCandidate = std::move(savedCacheCandidate);
			result->dynamicPacket = std::move(dynamicBuild);
			const bool isGroundPacket = result->groundPacket.has_value();
			const std::size_t packetBytes = isGroundPacket
				? EstimateGroundBuildPacketBytes(*result->groundPacket, result->savedCacheCandidate)
				: EstimateDynamicBuildPacketBytes(*result->dynamicPacket);
			std::lock_guard<std::mutex> lock(mutex_);
			NavigationAsyncBuildIdentity identity{};
			identity.world = world_ ? world_->Handle() : NavigationWorldHandle{};
			identity.lifecycleEpoch = asyncBuildEpoch_;
			identity.groupSequence = isGroundPacket
				? MakeGroundProfileBuildKey(result->groundPacket->profile)
				: ChangeSequence(result->dynamicPacket->change);
			identity.profileBuildKey = isGroundPacket
				? MakeGroundProfileBuildKey(result->groundPacket->profile)
				: MakeGroundProfileBuildKey(result->dynamicPacket->request.profile);
			// The worker receives no live system state. Retain this value-only kind so
			// protected diagnostics can separate initial/profile preparation from dirty
			// recovery work without exposing the copied packet itself.
			identity.dynamicRebuild = !isGroundPacket;
			const std::shared_ptr<NavigationBudget> budget = asyncBuildBudget_;
			std::string rejectionReason;
			const bool submitted = asyncBuildCoordinator_.TrySubmit(identity, budget, packetBytes,
				[result, budget]() {
					if(result->groundPacket) {
						result->groundBuild = NavigationSystem::PrepareGroundBuildPacket(
							*result->groundPacket, result->savedCacheCandidate, budget);
					}
					else if(result->dynamicPacket) {
						result->dynamicBuild = NavigationSystem::PrepareDynamicBuildPacket(*result->dynamicPacket, budget);
					}
				}, rejectionReason);
			if(submitted) {
				activeAsyncBuild_ = {identity, std::move(result)};
			}
			else {
				// The protected queue retains all rejected work. In particular, dynamic
				// receipts remain dirty until a later task completes and publishes.
				if(result->groundPacket) {
					pendingGroundBuilds_.push_front(std::move(*result->groundPacket));
					savedGroundCacheImportInFlight_ = false;
				}
				if(result->dynamicPacket) pendingDynamicRebuilds_.push_front(std::move(*result->dynamicPacket));
				diagnostics_.Append({NavigationDiagnosticSeverity::Information,
					NavigationMeasurementReason::MeasurementUnavailable, lastProtectedBoundaryGeneration_, 0U,
					"navigation.async_build_deferred", rejectionReason});
			}
			PublishAsyncBuildDiagnosticsLocked();
		}

		if(!prepareRoutes) {
			// Scheduler capture transferred this packet to a detached ThreadPool task.
			// The compatibility branch below remains deliberately synchronous.
			groundBuild.reset();
			savedCacheCandidate.reset();
			dynamicBuild.reset();
		}

		std::optional<PreparedGroundBuild> builtCoverage;
		if(groundBuild) {
			RecastGroundBuildInput buildInput{};
			buildInput.profile = groundBuild->profile;
			buildInput.sources = std::move(groundBuild->sources);
			PreparedGroundBuild prepared{};
			prepared.profile = groundBuild->profile;
			prepared.sourceIndex = std::move(groundBuild->sourceIndex);
			prepared.eligibleSourceIndex = std::move(groundBuild->eligibleSourceIndex);
			prepared.terrainPublicationGeneration = groundBuild->terrainPublicationGeneration;
			prepared.profileConfigurationRevision = groundBuild->profileConfigurationRevision;
			std::vector<GroundTileBlob> assembledTiles;
			if(savedCacheCandidate) {
				prepared.cacheCandidateAttempted = true;
				const GroundCoverageEvidence evidence = CaptureGroundCoverageEvidence(buildInput);
				SavedNavigationDescriptor expected{};
				expected.formatVersion = kSavedNavigationFormatVersion;
				expected.backendAbi = std::string(kSavedNavigationBackendAbi);
				expected.sourceFingerprint = evidence.sourceFingerprint;
				expected.settingsFingerprint = evidence.settingsFingerprint;
				const SavedNavigationReadResult decoded = evidence.accepted
					? ReadSavedNavigation(savedCacheCandidate->bytes, expected) : SavedNavigationReadResult{};
				const SavedGroundTileImportResult imported = decoded.accepted
					? ImportSavedGroundTiles(decoded, evidence) : SavedGroundTileImportResult{};
				if(imported.accepted) {
					prepared.cacheImported = true;
					prepared.cacheEvidence = evidence;
					assembledTiles = imported.tiles;
				}
				else {
					prepared.cacheRejectionReason = !evidence.accepted ? evidence.reason
						: !decoded.accepted ? decoded.reason : imported.reason;
				}
			}
			if(!prepared.cacheImported) {
				// Rejection never leaves a partially imported root. The same immutable
				// packet immediately follows the established fresh Recast path.
				const RecastGroundBuildResult fresh = BuildRecastGroundTiles(buildInput);
				if(fresh.Succeeded()) {
					assembledTiles = fresh.tiles;
					prepared.cacheEvidence = CaptureGroundCoverageEvidence(buildInput);
				} else {
					const DeferredProfileBuildFailure failure = ToDeferredProfileBuildFailure(fresh.status);
					prepared.failureOutcome = failure.outcome;
					prepared.failureReason = failure.reason;
					prepared.failureDetail = fresh.reason;
					if(prepared.cacheRejectionReason.empty()) prepared.cacheRejectionReason = fresh.reason;
				}
			}
			auto assembleTiles = [&prepared, &groundBuild](const std::vector<GroundTileBlob>& tiles) {
				if(tiles.empty()) {
					return false;
				}
				using GroupKey = std::tuple<std::int64_t, std::int64_t, std::int32_t>;
				std::map<GroupKey, std::vector<GroundTileBlob>> groups;
				prepared.shards.clear();
				prepared.coverage.clear();
				prepared.pristineTiles = tiles;
				for(const GroundTileBlob& tile : tiles) {
					groups[{FloorShard(tile.key.x), FloorShard(tile.key.z), tile.key.verticalBand}].push_back(tile);
				}
				for(auto& [key, groupTiles] : groups) {
					GroundShardKey shard{std::get<0>(key), std::get<1>(key), std::get<2>(key),
						MakeGroundProfileBuildKey(groundBuild->profile)};
					DetourGroundShardBuildInput shardInput{shard, 1U, std::move(groupTiles)};
					shardInput.minimumClearanceMetres = static_cast<float>(
						groundBuild->profile.clearanceMarginMetres);
					const DetourGroundShardBuildResult assembled = AssembleDetourGroundShard(shardInput);
					if(!assembled.accepted) {
						prepared.shards.clear();
						prepared.coverage.clear();
						prepared.pristineTiles.clear();
						return false;
					}
					prepared.shards.push_back(assembled.generation);
					prepared.coverage.push_back({{shard.x, shard.z, shard.verticalBand, shard.profileBuildKey},
						assembled.generation->reference.generation, NavigationCoverageState::Ready, 0U, true});
				}
				return true;
			};
			prepared.accepted = assembleTiles(assembledTiles);
			if(!prepared.accepted && prepared.failureDetail.empty()) {
				prepared.failureOutcome = NavigationOutcome::Failed;
				prepared.failureReason = NavigationReason::ProviderFailure;
				prepared.failureDetail = "DetourGroundShardAssemblyRejected";
			}
			if(!prepared.accepted && prepared.cacheImported) {
				// A decoded tile still has to assemble against this runtime's provider.
				// If that final provider check fails, discard every imported artifact and
				// rebuild from the same copied sources instead of publishing a subset.
				prepared.cacheImported = false;
				prepared.cacheRejectionReason = "SavedNavigationDetourAssemblyRejected";
				const RecastGroundBuildResult fresh = BuildRecastGroundTiles(buildInput);
				if(fresh.Succeeded()) {
					prepared.cacheEvidence = CaptureGroundCoverageEvidence(buildInput);
					prepared.accepted = assembleTiles(fresh.tiles);
				}
				else {
					prepared.cacheRejectionReason = fresh.reason;
					const DeferredProfileBuildFailure failure = ToDeferredProfileBuildFailure(fresh.status);
					prepared.failureOutcome = failure.outcome;
					prepared.failureReason = failure.reason;
					prepared.failureDetail = fresh.reason;
				}
				if(!prepared.accepted && prepared.failureDetail.empty()) {
					prepared.failureOutcome = NavigationOutcome::Failed;
					prepared.failureReason = NavigationReason::ProviderFailure;
					prepared.failureDetail = "DetourGroundShardAssemblyRejected";
				}
			}
			builtCoverage = std::move(prepared);
		}

		std::optional<PreparedDynamicRebuild> rebuiltDynamicCoverage;
		if(dynamicBuild) {
			rebuiltDynamicCoverage = {dynamicBuild->change, dynamicBuild->request.profile,
				PrepareDynamicGroundReplacement(dynamicBuild->request)};
		}

		std::vector<PreparedRoute> searchedRoutes;
		searchedRoutes.reserve(routes.size());
		for(const PendingRoute& route : routes) {
			const RouteClock::time_point workerStarted = RouteClock::now();
			auto result = SearchPublishedGroundRoute(route.request, route.worldRoot, route.shards, route.portals);
			const RouteClock::time_point workerFinished = RouteClock::now();
			searchedRoutes.push_back({route.preparationSequence, route.preparationEpoch,
				route.handle, route.intentGeneration, route.observation,
				route.readyAdmissionTime, route.capturedTime, workerStarted, workerFinished,
				std::move(result), route.worldRoot ? route.worldRoot->world : NavigationWorldHandle{},
				route.worldRoot ? route.worldRoot->rootGeneration : 0U});
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if(builtCoverage) preparedGroundBuilds_.push_back(std::move(*builtCoverage));
			if(rebuiltDynamicCoverage) preparedDynamicRebuilds_.push_back(std::move(*rebuiltDynamicCoverage));
			for(PreparedRoute& route : searchedRoutes) {
				RecordRouteDuration(dynamicDiagnostics_.routeWorkerSearchCount,
					dynamicDiagnostics_.routeWorkerSearchNanoseconds,
					dynamicDiagnostics_.routeWorkerSearchMaxNanoseconds,
					route.workerStartedTime, route.workerFinishedTime);
				preparedRoutes_.push_back(std::move(route));
			}
		}
	}

	void NavigationSystem::PublishPreparedWork() {
		std::lock_guard<std::mutex> lock(mutex_);
		// A completion can occur after protected capture but before this publication
		// boundary. Polling here avoids an unnecessary frame of route/root latency.
		DrainCompletedAsyncBuildsLocked();
		if(world_ && !preparedGroundBuilds_.empty()) {
			PreparedGroundBuild preparedGroundBuild = std::move(preparedGroundBuilds_.front());
			preparedGroundBuilds_.pop_front();
			auto finalizeSavedCacheAttempt = [this, &preparedGroundBuild](bool published) {
				if(!preparedGroundBuild.cacheCandidateAttempted) {
					return;
				}
				savedGroundCacheImportInFlight_ = false;
				const std::uint64_t profileKey = MakeGroundProfileBuildKey(preparedGroundBuild.profile);
				savedGroundCacheAttemptedProfiles_.insert(profileKey);
				if(published && preparedGroundBuild.cacheImported && preparedGroundBuild.accepted) {
					savedGroundCacheStatus_.candidatePending = false;
					savedGroundCacheStatus_.cacheUsed = true;
					savedGroundCacheStatus_.rejectionReason.clear();
					// The status retains byte-count and logical-path evidence, but the
					// successful candidate can no longer help another profile. Release its
					// potentially large copied payload as soon as protected publication wins.
					savedGroundCacheCandidate_.reset();
					return;
				}
				savedGroundCacheStatus_.candidatePending = savedGroundCacheCandidate_.has_value();
				savedGroundCacheStatus_.cacheUsed = false;
				savedGroundCacheStatus_.rejectionReason = preparedGroundBuild.cacheRejectionReason.empty()
					? "SavedNavigationFreshBuildRejected" : preparedGroundBuild.cacheRejectionReason;
			};
			if(preparedGroundBuild.accepted) {
				if(preparedGroundBuild.profileConfigurationRevision != 0U
					&& preparedGroundBuild.profileConfigurationRevision != profileConfigurationRevision_) {
					// Profile edits select different traversal constraints and shard keys.
					// Rejecting the older copied packet prevents a mixed world tuple.
					++dynamicDiagnostics_.staleDropCount;
					finalizeSavedCacheAttempt(false);
					ResolveDeferredProfileBuildFailureLocked(preparedGroundBuild.profile, true,
						NavigationOutcome::Stale, NavigationReason::DirtyCoverage,
						"ProfileConfigurationChangedBeforeGroundPublication");
					return;
				}
				if(preparedGroundBuild.eligibleSourceIndex != nullptr
					&& preparedGroundBuild.eligibleSourceIndex->Revision()
						!= latestAutomaticSourceIndexRevision_) {
					// A later protected capture observed scene, transform, terrain,
					// resource, streaming, or procedural evidence. Drop this complete
					// old candidate before it can exchange only part of the world tuple.
					++dynamicDiagnostics_.staleDropCount;
					finalizeSavedCacheAttempt(false);
					ResolveDeferredProfileBuildFailureLocked(preparedGroundBuild.profile, true,
						NavigationOutcome::Stale, NavigationReason::DirtyCoverage,
						"AutomaticSourceCaptureChangedBeforeGroundPublication");
					return;
				}
				const auto current = world_->PublishedRoot();
				NavigationWorldRoot candidate = world_->CreateCandidate();
				const bool replacesAutomaticCapture = preparedGroundBuild.eligibleSourceIndex != nullptr
					&& (!current->eligibleSourceIndex || current->eligibleSourceIndex->Revision()
						!= preparedGroundBuild.eligibleSourceIndex->Revision());
				if(replacesAutomaticCapture) {
					// A new automatic capture changes the source evidence for every profile.
					// Drop old profile coverage as one candidate group; subsequent requests
					// demand fresh profile-specific coverage instead of searching old geometry.
					candidate.coverage.clear();
					candidate.shards.clear();
					candidate.sources.clear();
				}
				// QueueInitialGroundBuild is a direct prepared-terrain test/runtime
				// input. Its accepted immutable packet establishes the first available
				// terrain generation; automatic capture supplies the same fields in play.
				candidate.terrainState = NavigationTerrainState::Available;
				candidate.terrainPublicationGeneration = preparedGroundBuild.terrainPublicationGeneration != 0U
					? preparedGroundBuild.terrainPublicationGeneration : candidate.rootGeneration;
				candidate.sourceIndexRevision = preparedGroundBuild.eligibleSourceIndex != nullptr
					? preparedGroundBuild.eligibleSourceIndex->Revision() : candidate.rootGeneration;
				candidate.eligibleSourceIndex = preparedGroundBuild.eligibleSourceIndex;
				candidate.routeValidityGeneration = candidate.rootGeneration;
				candidate.routeValidity = {candidate.routeValidityGeneration, 0U};
				for(const NavigationCoverageRecord& replacement : preparedGroundBuild.coverage) {
					auto existing = std::find_if(candidate.coverage.begin(), candidate.coverage.end(),
						[&replacement](const NavigationCoverageRecord& currentCoverage) {
							return currentCoverage.key == replacement.key;
						});
					if(existing == candidate.coverage.end()) candidate.coverage.push_back(replacement);
					else *existing = replacement;
				}
				for(const NavigationSourceIndexRecord& replacement : preparedGroundBuild.sourceIndex) {
					auto existing = std::find_if(candidate.sources.begin(), candidate.sources.end(),
						[&replacement](const NavigationSourceIndexRecord& currentSource) {
							return currentSource.sourceId == replacement.sourceId;
						});
					if(existing == candidate.sources.end()) candidate.sources.push_back(replacement);
					else *existing = replacement;
				}
				// Prepare the detached shard/portal tuple before publishing the root. The
				// portal graph performs the root callback under its expected revision lock,
				// so a rejected candidate cannot expose new coverage without matching seams.
				std::vector<std::shared_ptr<const DetourGroundShardGeneration>> nextShards =
					replacesAutomaticCapture ? std::vector<std::shared_ptr<const DetourGroundShardGeneration>>{}
					: publishedShards_;
				for(const auto& replacement : preparedGroundBuild.shards) {
					if(!replacement) continue;
					auto existing = std::find_if(nextShards.begin(), nextShards.end(),
						[&replacement](const auto& currentShard) {
							return currentShard && currentShard->reference.shard == replacement->reference.shard;
						});
					if(existing == nextShards.end()) nextShards.push_back(replacement);
					else *existing = replacement;
				}
				// The detached candidate and shard tuple are complete before the portal
				// transaction. Compute a continuity proof now so an allocation failure
				// cannot occur after root publication and leave tuple maintenance midway.
				std::vector<std::uint64_t> preservedProfiles;
				if(!replacesAutomaticCapture) {
					try {
						preservedProfiles = CollectTopologyPreservedProfilesLocked(
							*current, candidate, publishedShards_, nextShards);
					}
					catch(...) {
						// Optional proof allocation must not discard an accepted build packet.
						// Publishing without it is safe because later prepared routes fail closed.
					}
				}
				const std::vector<Ground::GroundBoundaryInterval> nextIntervals = CollectBoundaryIntervals(nextShards);
				const Ground::GroundPortalGraphSnapshot priorPortals = portalGraph_.AcquireSnapshot();
				const bool portalsPublished = portalGraph_.PublishBoundaryIntervalsTransactionally(
					nextIntervals, priorPortals.Revision(), [this, &candidate, &current] {
						return world_->Publish(std::move(candidate), current->rootGeneration);
					});
				if(portalsPublished) {
					if(replacesAutomaticCapture) {
						// The root exchange succeeded, so detached shards/stages can now be
						// replaced safely. Clearing earlier would violate rejected-candidate
						// atomicity by leaving the current root without its matching shards.
						publishedGroundCaches_.clear();
						// Keep every stage captured for this automatic tuple. The first
						// profile is published before its peers, but later dirty-source
						// rebuilds must retain the peers' matching immutable snapshots too.
					}
					if(!replacesAutomaticCapture && !preservedProfiles.empty()) {
						if(const std::shared_ptr<const NavigationWorldRoot> published = world_->PublishedRoot())
							AppendDynamicRouteTransitionLocked(*current, *published, {}, std::move(preservedProfiles));
					}
					publishedShards_ = std::move(nextShards);
					if(preparedGroundBuild.eligibleSourceIndex != nullptr) {
						// A multi-profile automatic capture can publish one profile per
						// protected boundary. The transition records this exact profile key and
						// refreshes routes only once every required key has committed.
						RefreshUnaffectedRoutesAfterAutomaticSourcePublicationLocked(
							preparedGroundBuild.eligibleSourceIndex->Revision(),
							MakeGroundProfileBuildKey(preparedGroundBuild.profile));
					}
					// A saved artifact is exported only from the exact complete tile set
					// that just became authoritative. Dynamic rebuilds clear this retained
					// evidence rather than reverse-engineering it from Detour shards.
					if(preparedGroundBuild.cacheEvidence.accepted
						&& !preparedGroundBuild.pristineTiles.empty()
						&& std::all_of(preparedGroundBuild.pristineTiles.begin(),
							preparedGroundBuild.pristineTiles.end(), [](const GroundTileBlob& tile) {
								return tile.supportComplete && tile.clearanceComplete;
							})) {
						PublishedGroundCacheCoverage coverage{};
						coverage.profile = preparedGroundBuild.profile;
						coverage.evidence = preparedGroundBuild.cacheEvidence;
						coverage.tiles = preparedGroundBuild.pristineTiles;
						const std::shared_ptr<const NavigationWorldRoot> published = world_->PublishedRoot();
						coverage.sourceIndexRevision = published ? published->sourceIndexRevision : 0U;
						const std::uint64_t profileKey = MakeGroundProfileBuildKey(coverage.profile);
						auto existing = std::find_if(publishedGroundCaches_.begin(), publishedGroundCaches_.end(),
							[profileKey](const PublishedGroundCacheCoverage& currentCoverage) {
								return MakeGroundProfileBuildKey(currentCoverage.profile) == profileKey;
							});
						if(existing == publishedGroundCaches_.end()) {
							publishedGroundCaches_.push_back(std::move(coverage));
						}
						else {
							*existing = std::move(coverage);
						}
					}
					finalizeSavedCacheAttempt(true);
					// The complete profile-qualified root and its portal snapshot are now
					// published together. Only then may requests awaiting this profile enter
					// a worker, preventing a stale default-profile route from being searched.
					PromoteDeferredRoutesForProfileLocked(preparedGroundBuild.profile);
				}
				else {
					// A failed compare-and-publish never changes the old root. Record the
					// candidate result and let a later protected capture queue fresh work.
					finalizeSavedCacheAttempt(false);
					ResolveDeferredProfileBuildFailureLocked(preparedGroundBuild.profile, true,
						NavigationOutcome::Stale, NavigationReason::DirtyCoverage,
						"GroundRootPublicationRejected");
				}
			} else {
				// An exception may finish after a newer profile edit or automatic capture
				// has already replaced its copied input. In that case it cannot terminalize
				// the newer recovery; reuse the accepted-packet supersession policy.
				const bool profileConfigurationChanged = preparedGroundBuild.profileConfigurationRevision != 0U
					&& preparedGroundBuild.profileConfigurationRevision != profileConfigurationRevision_;
				const bool automaticSourceChanged = preparedGroundBuild.eligibleSourceIndex != nullptr
					&& preparedGroundBuild.eligibleSourceIndex->Revision() != latestAutomaticSourceIndexRevision_;
				finalizeSavedCacheAttempt(false);
				if(profileConfigurationChanged || automaticSourceChanged) {
					++dynamicDiagnostics_.staleDropCount;
					ResolveDeferredProfileBuildFailureLocked(preparedGroundBuild.profile, true,
						NavigationOutcome::Stale, NavigationReason::DirtyCoverage,
						profileConfigurationChanged
							? "ProfileConfigurationChangedBeforeGroundFailurePublication"
							: "AutomaticSourceCaptureChangedBeforeGroundFailurePublication");
				}
				else {
					ResolveDeferredProfileBuildFailureLocked(preparedGroundBuild.profile, false,
						preparedGroundBuild.failureOutcome, preparedGroundBuild.failureReason,
						preparedGroundBuild.failureDetail);
				}
			}
		}
		if(world_ && preparedDynamicBuildGroup_) {
			PreparedDynamicBuildGroup group = std::move(*preparedDynamicBuildGroup_);
			preparedDynamicBuildGroup_.reset();
			// Worker completion order is intentionally unconstrained. Sort the tiny
			// profile lists before merging so equal captured input produces the same
			// candidate/shard ordering on one, four, or eight scheduler workers.
			auto sortByProfile = [](std::vector<PreparedDynamicRebuild>& builds) {
				std::sort(builds.begin(), builds.end(), [](const PreparedDynamicRebuild& left,
					const PreparedDynamicRebuild& right) {
					return MakeGroundProfileBuildKey(left.profile) < MakeGroundProfileBuildKey(right.profile);
				});
			};
			sortByProfile(group.successfulChildren);
			sortByProfile(group.failedChildren);
			const NavigationGeneration sequence = ChangeSequence(group.change);
			const std::shared_ptr<const NavigationWorldRoot> currentRoot = world_->PublishedRoot();
			const bool groupIsCurrent = currentRoot && group.world == world_->Handle()
				&& currentRoot->rootGeneration == group.expectedRootGeneration
				&& currentRoot->terrainPublicationGeneration == group.expectedTerrainPublicationGeneration
				&& currentRoot->sourceIndexRevision == group.expectedSourceIndexRevision
				&& dirtyIndex_.LatestChangeSequence() == sequence
				&& dirtyIndex_.LatestChangeSequence(group.change.source) == sequence;
			if(!groupIsCurrent) {
				// A newer source/root tuple supersedes every copied child at once. No
				// failure from this retired group may terminalize deferred requests that
				// already joined the newer receipt recovery.
				++dynamicDiagnostics_.staleDropCount;
				if(dirtyIndex_.LatestChangeSequence() == sequence) pendingDynamicChanges_.push_front(group.change);
			}
			else {
				bool transientFailure = false;
				for(const PreparedDynamicRebuild& failed : group.failedChildren) {
					++dynamicDiagnostics_.rejectedUpdateCount;
					diagnostics_.Append({NavigationDiagnosticSeverity::Warning,
						NavigationMeasurementReason::MeasurementUnavailable, lastProtectedBoundaryGeneration_, 0U,
						"navigation.dynamic_rebuild_unavailable", failed.plan.reason});
					if(!failed.plan.failurePermanent) {
						transientFailure = true;
						continue;
					}
					ResolveDeferredProfileBuildFailureLocked(failed.profile, false,
						failed.plan.failureOutcome, failed.plan.failureReason, failed.plan.reason);
					if(dynamicRebuildProgress_ && dynamicRebuildProgress_->changeSequence == sequence) {
						const std::uint64_t profileKey = MakeGroundProfileBuildKey(failed.profile);
						auto& terminal = dynamicRebuildProgress_->terminalFailedProfileBuildKeys;
						if(!std::binary_search(terminal.begin(), terminal.end(), profileKey))
							terminal.insert(std::upper_bound(terminal.begin(), terminal.end(), profileKey), profileKey);
					}
				}
				if(transientFailure) {
					// Cancellation and transient provider unavailability leave the current
					// dirty tuple fail-closed. Do not immediately requeue the identical
					// copied receipt: that would turn a cancelled worker into an unbounded
					// retry loop. Deferred endpoints return through their established queued
					// unavailable path until a later committed receipt owns a fresh recovery.
					for(const PreparedDynamicRebuild& prepared : group.successfulChildren)
						PromoteDeferredRoutesForProfileLocked(prepared.profile);
					for(const PreparedDynamicRebuild& failed : group.failedChildren)
						PromoteDeferredRoutesForProfileLocked(failed.profile);
				}
				else if(!group.successfulChildren.empty()) {
					NavigationPublicationCandidate candidate{};
					candidate.expectedRootGeneration = group.expectedRootGeneration;
					candidate.expectedSourceRevision = sequence;
					std::vector<std::shared_ptr<const DetourGroundShardGeneration>> nextShards = publishedShards_;
					bool coherent = true;
					bool copiedSharedCandidate = false;
					std::vector<std::uint64_t> successfulProfileKeys;
					std::vector<GroundShardKey> groupShardKeys;
					auto sameSourceRows = [](const std::vector<NavigationSourceIndexRecord>& left,
						const std::vector<NavigationSourceIndexRecord>& right) {
						if(left.size() != right.size()) return false;
						return std::all_of(left.begin(), left.end(), [&right](const NavigationSourceIndexRecord& row) {
							const auto match = std::find_if(right.begin(), right.end(), [&row](
								const NavigationSourceIndexRecord& other) { return other.sourceId == row.sourceId; });
							return match != right.end() && match->sourceRevision == row.sourceRevision
								&& match->geometryRevision == row.geometryRevision && match->enabled == row.enabled
								&& match->identity == row.identity && match->revisionEvidence == row.revisionEvidence;
						});
					};
					auto sameTombstones = [](const NavigationSourceTombstoneSnapshot& left,
						const NavigationSourceTombstoneSnapshot& right) {
						if(left.entries.size() != right.entries.size()) return false;
						return std::all_of(left.entries.begin(), left.entries.end(), [&right](const NavigationSourceTombstone& row) {
							return std::any_of(right.entries.begin(), right.entries.end(), [&row](
								const NavigationSourceTombstone& other) {
									return other.sourceId == row.sourceId && other.removedRevision == row.removedRevision;
								});
						});
					};
					for(const PreparedDynamicRebuild& prepared : group.successfulChildren) {
						const NavigationDynamicRebuildPlan& plan = prepared.plan;
						if(plan.incrementalReuseAttempted) ++dynamicDiagnostics_.dynamicTileReuseAttemptCount;
						dynamicDiagnostics_.dynamicTileReuseTileCount += plan.reusedTileCount;
						dynamicDiagnostics_.dynamicTileRebuildTileCount += plan.rebuiltTileCount;
						dynamicDiagnostics_.dynamicTileReuseFallbackCount += plan.fullShardFallbackCount;
						if(plan.fullShardFallbackCount != 0U && !plan.incrementalReuseFallbackReason.empty()) {
							diagnostics_.Append({NavigationDiagnosticSeverity::Information,
								NavigationMeasurementReason::Measured, lastProtectedBoundaryGeneration_, 0U,
								"navigation.dynamic_tile_reuse_fallback", plan.incrementalReuseFallbackReason});
						}
						if(!plan.accepted || plan.candidate.expectedRootGeneration != candidate.expectedRootGeneration
							|| plan.candidate.expectedSourceRevision != candidate.expectedSourceRevision) {
							coherent = false;
							break;
						}
						const std::uint64_t profileKey = MakeGroundProfileBuildKey(prepared.profile);
						if(!std::binary_search(group.requiredProfileBuildKeys.begin(),
							group.requiredProfileBuildKeys.end(), profileKey)
							|| std::find(successfulProfileKeys.begin(), successfulProfileKeys.end(), profileKey)
							!= successfulProfileKeys.end()) {
							coherent = false;
							break;
						}
						successfulProfileKeys.push_back(profileKey);
						if(!copiedSharedCandidate) {
							candidate.tombstones = plan.candidate.tombstones;
							candidate.sourceReplacements = plan.candidate.sourceReplacements;
							copiedSharedCandidate = true;
						}
						else if(!sameTombstones(candidate.tombstones, plan.candidate.tombstones)
							|| !sameSourceRows(candidate.sourceReplacements, plan.candidate.sourceReplacements)) {
							coherent = false;
							break;
						}
						for(const NavigationCoverageRecord& coverage : plan.candidate.coverage) {
							if(coverage.key.profileBuildKey != profileKey) {
								coherent = false;
								break;
							}
							if(std::any_of(candidate.coverage.begin(), candidate.coverage.end(), [&coverage](
								const NavigationCoverageRecord& current) { return current.key == coverage.key; })) {
								coherent = false;
								break;
							}
							candidate.coverage.push_back(coverage);
						}
						if(!coherent) break;
						for(const auto& replacement : plan.shards) {
							if(!replacement) continue;
							if(replacement->reference.shard.profileBuildKey != profileKey
								|| std::find(groupShardKeys.begin(), groupShardKeys.end(),
									replacement->reference.shard) != groupShardKeys.end()) {
								coherent = false;
								break;
							}
							groupShardKeys.push_back(replacement->reference.shard);
							auto existing = std::find_if(nextShards.begin(), nextShards.end(), [&replacement](const auto& current) {
								return current && current->reference.shard == replacement->reference.shard;
							});
							if(existing == nextShards.end()) nextShards.push_back(replacement);
							else *existing = replacement;
						}
						if(!coherent) break;
					}
					if(!coherent) {
						++dynamicDiagnostics_.rejectedUpdateCount;
						pendingDynamicChanges_.push_front(group.change);
					}
					else {
						const std::shared_ptr<const NavigationWorldRoot> previousRoot = currentRoot;
						const std::vector<std::shared_ptr<const DetourGroundShardGeneration>> previousShards = publishedShards_;
						const Ground::GroundPortalGraphSnapshot previousPortals = portalGraph_.AcquireSnapshot();
						const std::vector<Ground::GroundBoundaryInterval> intervals = CollectBoundaryIntervals(nextShards);
						NavigationPublicationReceipt receipt{};
						const bool publishedTuple = portalGraph_.PublishBoundaryIntervalsTransactionally(
							intervals, previousPortals.Revision(), [this, &candidate, &receipt] {
								receipt = PublishNavigationCandidateAtProtectedBoundary(*world_, dirtyIndex_, candidate);
								return receipt.accepted;
							});
						if(!publishedTuple) {
							++dynamicDiagnostics_.rejectedUpdateCount;
							pendingDynamicChanges_.push_front(group.change);
						}
						else {
							publishedShards_ = std::move(nextShards);
							const std::vector<DynamicRouteAffectedBounds> transitionBounds =
								FindDynamicRouteAffectedBoundsLocked(group.change.source, sequence);
							if(const std::shared_ptr<const NavigationWorldRoot> published = world_->PublishedRoot()) {
								std::vector<std::uint64_t> preservedProfiles;
								try {
									preservedProfiles = CollectTopologyPreservedProfilesLocked(
										*previousRoot, *published, previousShards, publishedShards_);
								}
								catch(...) {}
								AppendDynamicRouteTransitionLocked(*previousRoot, *published, transitionBounds,
									std::move(preservedProfiles));
							}
							RefreshUnaffectedRoutesAfterDynamicSourcePublicationLocked(previousRoot);
							for(const PreparedDynamicRebuild& prepared : group.successfulChildren)
								PromoteDeferredRoutesForProfileLocked(prepared.profile);
							publishedGroundCaches_.clear();
							if(dynamicRebuildProgress_ && dynamicRebuildProgress_->changeSequence == sequence) {
								auto& published = dynamicRebuildProgress_->publishedProfileBuildKeys;
								for(const std::uint64_t profileKey : successfulProfileKeys) {
									if(!std::binary_search(published.begin(), published.end(), profileKey))
										published.insert(std::upper_bound(published.begin(), published.end(), profileKey), profileKey);
								}
								if(std::includes(published.begin(), published.end(),
									dynamicRebuildProgress_->requiredProfileBuildKeys.begin(),
									dynamicRebuildProgress_->requiredProfileBuildKeys.end())) {
									(void)dirtyIndex_.AcknowledgePublishedThrough(sequence);
									dynamicRebuildProgress_.reset();
								}
							}
						}
					}
				}
			}
		}
		while(world_ && !preparedDynamicRebuilds_.empty()) {
			PreparedDynamicRebuild prepared = std::move(preparedDynamicRebuilds_.front());
			preparedDynamicRebuilds_.pop_front();
			if(!prepared.plan.accepted) {
				++dynamicDiagnostics_.rejectedUpdateCount;
				diagnostics_.Append({NavigationDiagnosticSeverity::Warning,
					NavigationMeasurementReason::MeasurementUnavailable, lastProtectedBoundaryGeneration_, 0U,
					"navigation.dynamic_rebuild_unavailable", prepared.plan.reason});
				const NavigationGeneration sequence = ChangeSequence(prepared.change);
				const bool failureIsCurrent = dirtyIndex_.LatestChangeSequence() == sequence
					&& dirtyIndex_.LatestChangeSequence(prepared.change.source) == sequence;
				if(failureIsCurrent) {
					if(prepared.plan.failurePermanent) {
						// This exact current packet reached Recast/Detour and failed with a
						// typed permanent disposition. Only matching deferred requests may
						// become terminal; a later source receipt never reaches this branch.
						ResolveDeferredProfileBuildFailureLocked(prepared.profile, false,
							prepared.plan.failureOutcome, prepared.plan.failureReason, prepared.plan.reason);
					}
					else {
						// A cancelled or stale preparation has no live worker owner after this
						// packet is removed. Do not resubmit it in a loop: let each deferred
						// endpoint take the established queued unavailable path until a new
						// committed receipt creates a fresh recovery.
						PromoteDeferredRoutesForProfileLocked(prepared.profile);
					}
				}
				continue;
			}
			if(prepared.plan.incrementalReuseAttempted) ++dynamicDiagnostics_.dynamicTileReuseAttemptCount;
			dynamicDiagnostics_.dynamicTileReuseTileCount += prepared.plan.reusedTileCount;
			dynamicDiagnostics_.dynamicTileRebuildTileCount += prepared.plan.rebuiltTileCount;
			dynamicDiagnostics_.dynamicTileReuseFallbackCount += prepared.plan.fullShardFallbackCount;
			if(prepared.plan.fullShardFallbackCount != 0U
				&& !prepared.plan.incrementalReuseFallbackReason.empty()) {
				// One bounded record per prepared plan keeps an installed smoke actionable
				// without retaining any provider bytes or a growing fallback history.
				diagnostics_.Append({NavigationDiagnosticSeverity::Information,
					NavigationMeasurementReason::Measured, lastProtectedBoundaryGeneration_, 0U,
					"navigation.dynamic_tile_reuse_fallback", prepared.plan.incrementalReuseFallbackReason});
			}

			const std::shared_ptr<const NavigationWorldRoot> previousRoot = world_->PublishedRoot();
			// World root, shard cache, and portal graph are replaced while this same
			// protected NavigationSystem mutex is held. Route admission also holds it,
			// so a query observes either the old complete tuple or this new one.
			// Retain the old immutable shard pointers through the transaction. A proof
			// is optional, but it must compare exactly the tuples that were exchanged.
			const std::vector<std::shared_ptr<const DetourGroundShardGeneration>> previousShards = publishedShards_;
			std::vector<std::shared_ptr<const DetourGroundShardGeneration>> nextShards = publishedShards_;
			for(const auto& replacement : prepared.plan.shards) {
				if(!replacement) continue;
				auto existing = std::find_if(nextShards.begin(), nextShards.end(),
					[&replacement](const auto& currentShard) {
						return currentShard && currentShard->reference.shard == replacement->reference.shard;
					});
				if(existing == nextShards.end()) nextShards.push_back(replacement);
				else *existing = replacement;
			}
			const Ground::GroundPortalGraphSnapshot previousPortals = portalGraph_.AcquireSnapshot();
			const std::vector<Ground::GroundBoundaryInterval> intervals = CollectBoundaryIntervals(nextShards);
			NavigationPublicationReceipt receipt{};
			const bool publishedTuple = portalGraph_.PublishBoundaryIntervalsTransactionally(
				intervals, previousPortals.Revision(), [this, &prepared, &receipt] {
					receipt = PublishNavigationCandidateAtProtectedBoundary(
						*world_, dirtyIndex_, prepared.plan.candidate);
					return receipt.accepted;
				});
			if(!publishedTuple) {
				// The callback runs while the portal revision is locked. A false result
				// therefore leaves both the immutable portal snapshot and world root
				// unchanged; requeue the copied change instead of exposing mixed state.
				++dynamicDiagnostics_.rejectedUpdateCount;
				if(dirtyIndex_.LatestChangeSequence(prepared.change.source) == ChangeSequence(prepared.change)
					&& dirtyIndex_.LatestChangeSequence() == ChangeSequence(prepared.change)) {
					const bool alreadyQueued = std::any_of(pendingDynamicChanges_.begin(), pendingDynamicChanges_.end(),
						[&prepared](const NavigationDynamicSourceChange& change) {
							return change.source == prepared.change.source
								&& ChangeSequence(change) == ChangeSequence(prepared.change);
						});
					if(!alreadyQueued) pendingDynamicChanges_.push_front(prepared.change);
				}
				continue;
			}
			publishedShards_ = std::move(nextShards);
			// The rebuilt profile changes the immutable root without introducing a new
			// scene mutation. Reuse the original receipt range so a query captured from
			// the dirty intermediate root still proves its corridor avoided that source.
			const std::vector<DynamicRouteAffectedBounds> transitionBounds =
				FindDynamicRouteAffectedBoundsLocked(prepared.change.source, ChangeSequence(prepared.change));
			if(const std::shared_ptr<const NavigationWorldRoot> published = world_->PublishedRoot()) {
				std::vector<std::uint64_t> preservedProfiles;
				try {
					// Dynamic publication is already committed. Dropping optional topology
					// proof on allocation failure preserves the coherent new root/shards and
					// makes a later prepared route conservatively fail closed.
					preservedProfiles = CollectTopologyPreservedProfilesLocked(
						*previousRoot, *published, previousShards, publishedShards_);
				}
				catch(...) {}
				AppendDynamicRouteTransitionLocked(*previousRoot, *published, transitionBounds,
					std::move(preservedProfiles));
			}
			RefreshUnaffectedRoutesAfterDynamicSourcePublicationLocked(previousRoot);
			// This exact profile's root and portal tuple are authoritative now. Requests
			// that joined its dynamic recovery may enter route preparation without
			// spawning a competing normal ground build.
			PromoteDeferredRoutesForProfileLocked(prepared.profile);
			// Dynamic geometry changed the authoritative root. Its rebuilt Detour
			// shards do not retain v2 complete-tile evidence, so exporting older
			// pristine bytes would be unsafe even if some shard keys still overlap.
			publishedGroundCaches_.clear();
			const NavigationGeneration sequence = ChangeSequence(prepared.change);
			if(!dynamicRebuildProgress_ || dynamicRebuildProgress_->changeSequence != sequence) {
				// A late worker cannot certify a dirty certificate from another sequence.
				// Keep the current dirty set and retry its latest copied change instead of
				// acknowledging coverage whose sibling profile was never rebuilt.
				if(dirtyIndex_.LatestChangeSequence() == sequence)
					pendingDynamicChanges_.push_front(prepared.change);
				continue;
			}
			DynamicRebuildProgress& progress = *dynamicRebuildProgress_;
			const std::uint64_t profileKey = MakeGroundProfileBuildKey(prepared.profile);
			if(std::binary_search(progress.requiredProfileBuildKeys.begin(), progress.requiredProfileBuildKeys.end(), profileKey)
				&& !std::binary_search(progress.publishedProfileBuildKeys.begin(),
					progress.publishedProfileBuildKeys.end(), profileKey)) {
				progress.publishedProfileBuildKeys.insert(std::upper_bound(progress.publishedProfileBuildKeys.begin(),
					progress.publishedProfileBuildKeys.end(), profileKey), profileKey);
			}
			if(std::includes(progress.publishedProfileBuildKeys.begin(), progress.publishedProfileBuildKeys.end(),
				progress.requiredProfileBuildKeys.begin(), progress.requiredProfileBuildKeys.end())) {
				// The root, shards, and generated portals now expose every profile for
				// this exact dirty certificate. Only this group completion may retire the
				// shared dependency set.
				(void)dirtyIndex_.AcknowledgePublishedThrough(sequence);
				dynamicRebuildProgress_.reset();
			}
			else {
				// Every sibling was captured against the previous root generation. Drop
				// those stale packets and rebuild only the profiles still missing from the
				// same immutable dirty certificate on the next worker pass.
				std::erase_if(pendingDynamicRebuilds_, [sequence](const PendingDynamicRebuild& pending) {
					return ChangeSequence(pending.change) == sequence;
				});
				std::erase_if(preparedDynamicRebuilds_, [sequence](const PreparedDynamicRebuild& pending) {
					return ChangeSequence(pending.change) == sequence;
				});
				const bool alreadyQueued = std::any_of(pendingDynamicChanges_.begin(), pendingDynamicChanges_.end(),
					[sequence](const NavigationDynamicSourceChange& pending) {
						return ChangeSequence(pending) == sequence;
					});
				if(!alreadyQueued && dirtyIndex_.LatestChangeSequence() == sequence)
					pendingDynamicChanges_.push_front(prepared.change);
			}
		}
		// Late jobs from an earlier Initialize/Revoke lifetime must not interact with
		// a newly allocated queue, even when both worlds happen to use the same ID.
		std::erase_if(preparedRoutes_, [this](const PreparedRoute& route) {
			return !world_ || route.preparationEpoch != routePreparationEpoch_
				|| route.world != world_->Handle();
		});
		const bool capturedBatchComplete = std::all_of(inFlightRoutes_.begin(), inFlightRoutes_.end(),
			[this](const PendingRoute& pending) {
				return std::any_of(preparedRoutes_.begin(), preparedRoutes_.end(),
					[&pending](const PreparedRoute& prepared) {
						return prepared.preparationEpoch == pending.preparationEpoch
							&& prepared.preparationSequence == pending.preparationSequence
							&& prepared.handle == pending.handle;
					});
			});
		// Do not publish a partial worker batch. If any range was cancelled or
		// rejected, its whole captured packet remains in flight and the next protected
		// capture restores it to pending order before retrying every range.
		if(capturedBatchComplete && preparedRoutes_.size() > 1U) {
			std::sort(preparedRoutes_.begin(), preparedRoutes_.end(),
				[](const PreparedRoute& left, const PreparedRoute& right) {
					return std::tie(left.preparationEpoch, left.preparationSequence)
						< std::tie(right.preparationEpoch, right.preparationSequence);
				});
		}
		while(capturedBatchComplete && !preparedRoutes_.empty()) {
			PreparedRoute prepared = std::move(preparedRoutes_.front());
			preparedRoutes_.pop_front();
			// Range workers complete in scheduling order only by coincidence. The
			// admission sequence is assigned at the protected boundary and restores a
			// repeatable publication order across one, four, or eight workers.
			std::erase_if(inFlightRoutes_, [&prepared](const PendingRoute& pending) {
				return pending.preparationEpoch == prepared.preparationEpoch
					&& pending.preparationSequence == prepared.preparationSequence
					&& pending.handle == prepared.handle;
			});
			if(!requestStore_) continue;
			const std::shared_ptr<const NavigationWorldRoot> currentRoot = world_ ? world_->PublishedRoot() : nullptr;
			const auto requestRead = requestStore_->Read(prepared.handle);
			// A cancellation keeps its slot allocated for deterministic reads, so the
			// handle can still be alive while the result is no longer publishable.
			if(!requestRead.record || requestRead.record->lifecycle == NavigationRequestLifecycle::Terminal)
				continue;
			// Re-read only at this protected publication boundary. A deleted/reused
			// object cannot receive a worker route that was captured for an older
			// UUID lifetime or transform generation.
			const NavigationRequestRecord& requestRecord = *requestRead.record;
			// The worker packet, not the request's original admission record, owns the
			// world tuple that was actually searched. This matters when a deferred
			// profile build publishes before the route is captured.
			NavigationObservationEvidence routeObservation = prepared.observation;
			if(!currentRoot || currentRoot->world != prepared.world) {
				// Scene replacement and a missing root have no copied dynamic transition
				// certificate. A worker result from either must remain fail-closed.
				prepared.result.points.clear();
				prepared.result.outcome = NavigationOutcome::Stale;
				prepared.result.reason = NavigationReason::DirtyCoverage;
			} else if(currentRoot->rootGeneration != prepared.rootGeneration) {
				++dynamicDiagnostics_.preparedRouteRootMismatchCount;
				if(prepared.result.outcome == NavigationOutcome::Complete
					|| prepared.result.outcome == NavigationOutcome::Partial) {
					const Ground::GroundRouteQuery::RetainedRoute candidateRoute{
						prepared.result.points, prepared.result.sourceRevision, true};
					DynamicRouteRebaseUnknownReason unknownReason = DynamicRouteRebaseUnknownReason::None;
					const DynamicRouteRebaseResult rebase = TryRebasePreparedRouteObservationLocked(
						prepared.world, prepared.rootGeneration, prepared.observation,
						requestRecord.profile, candidateRoute, *currentRoot, routeObservation, unknownReason);
					if(rebase == DynamicRouteRebaseResult::Rebased) {
						++dynamicDiagnostics_.preparedRouteTransitionRebasedCount;
					} else {
						if(rebase == DynamicRouteRebaseResult::Intersected)
							++dynamicDiagnostics_.preparedRouteTransitionIntersectedCount;
						else {
							++dynamicDiagnostics_.preparedRouteTransitionUnknownCount;
							RecordPreparedRouteTransitionUnknownLocked(unknownReason, prepared, *currentRoot);
						}
						prepared.result.points.clear();
						prepared.result.outcome = NavigationOutcome::Stale;
						prepared.result.reason = NavigationReason::DirtyCoverage;
					}
				} else {
					// Non-route terminal answers cannot be recertified from a corridor.
					prepared.result.points.clear();
					prepared.result.outcome = NavigationOutcome::Stale;
					prepared.result.reason = NavigationReason::DirtyCoverage;
				}
			}
			if(requestRecord.sourceEndpoint.object.IsValid()) {
				const auto currentSource = CaptureObjectEndpointLocked(
					ObjectManager::instance(), requestRecord.sourceEndpoint.object.objectId);
				if(!currentSource || currentSource->object != requestRecord.sourceEndpoint.object) {
					// A missing source or a reused UUID cannot safely receive a route.
					// Unlike a pose revision, this is not recoverable by resubmitting the
					// same agent intent because its authoritative owner no longer exists.
					prepared.result.points.clear();
					prepared.result.outcome = NavigationOutcome::InvalidEndpoint;
					prepared.result.reason = NavigationReason::InvalidStart;
				}
				else if(currentSource->transformRevision != requestRecord.sourceEndpoint.transformRevision) {
					// A protected pose publication can legitimately occur after admission
					// and before an asynchronous search completes. Do not publish a route
					// calculated from the old origin, but expose a retryable stale result
					// so NavigationAgentSystem can re-capture and replan. DirtyCoverage is
					// the existing public reason for an immutable query input becoming old;
					// invalid/missing geometry continues to use InvalidStart above.
					prepared.result.points.clear();
					prepared.result.outcome = NavigationOutcome::Stale;
					prepared.result.reason = NavigationReason::DirtyCoverage;
				}
			}
			if(requestRecord.destinationEndpoint.object.IsValid()) {
				const auto currentDestination = CaptureObjectEndpointLocked(
					ObjectManager::instance(), requestRecord.destinationEndpoint.object.objectId);
				if(!currentDestination || currentDestination->object != requestRecord.destinationEndpoint.object
					|| currentDestination->transformRevision != requestRecord.destinationEndpoint.transformRevision) {
					prepared.result.points.clear();
					prepared.result.outcome = NavigationOutcome::InvalidEndpoint;
					prepared.result.reason = NavigationReason::InvalidDestination;
				}
			}
			if(prepared.result.outcome == NavigationOutcome::Complete
				|| prepared.result.outcome == NavigationOutcome::Partial) {
				// A queued automatic replacement has not exchanged the root yet, but
				// changed geometry is already unsafe for new guidance. Reject only a
				// result whose copied path intersects that immutable transition; routes
				// outside it may continue until their evidence is refreshed at commit.
				const Ground::GroundRouteQuery::RetainedRoute candidateRoute{
					prepared.result.points, prepared.result.sourceRevision, true};
				if(RouteIsAffectedByAutomaticSourceReplacementLocked(candidateRoute, requestRecord.profile)) {
					prepared.result.points.clear();
					prepared.result.outcome = NavigationOutcome::Stale;
					prepared.result.reason = NavigationReason::DirtyCoverage;
				}
			}
			NavigationRouteHandle routeHandle{};
			// Partial/NearestReachable has a useful retained corridor ending at the
			// nearest reachable polygon. Publish it as a route without upgrading the
			// request to Complete; callers can copy and validate the partial points.
			if((prepared.result.outcome == NavigationOutcome::Complete
				|| prepared.result.outcome == NavigationOutcome::Partial) && routeStore_) {
				NavigationRouteRecord routeRecord{};
				routeRecord.request = prepared.handle;
				routeRecord.intentGeneration = requestRecord.intentGeneration;
				routeRecord.observation = routeObservation;
				routeRecord.lifecycle = NavigationRequestLifecycle::Terminal;
				routeRecord.outcome = prepared.result.outcome;
				routeRecord.reason = prepared.result.reason;
				const auto created = routeStore_->Create(prepared.handle.owner, std::move(routeRecord));
				if(created.Succeeded()) {
					routeHandle = created.handle;
					if(publishedRouteData_.size() <= routeHandle.slotIndex) publishedRouteData_.resize(routeHandle.slotIndex + 1U);
					publishedRouteData_[routeHandle.slotIndex] = {prepared.result.points, prepared.result.sourceRevision, true};
				}
			}
			if(prepared.handle.slotIndex < requestRoutes_.size()) requestRoutes_[prepared.handle.slotIndex] = routeHandle;
			(void)requestStore_->Update(prepared.handle, [&prepared, &routeObservation](NavigationRequestRecord& record) {
				record.lifecycle = NavigationRequestLifecycle::Terminal;
				record.outcome = prepared.result.outcome;
				record.reason = prepared.result.reason;
				record.observation = routeObservation;
			});
			// Only a route that received a live route-store handle is a successful
			// protected publication sample. Stale, cancelled, rejected, and diagnostic
			// terminal packets intentionally do not enter this aggregate.
			if(routeHandle.IsValid()) {
				RecordRouteDuration(dynamicDiagnostics_.routeWorkerToPublicationCount,
					dynamicDiagnostics_.routeWorkerToPublicationNanoseconds,
					dynamicDiagnostics_.routeWorkerToPublicationMaxNanoseconds,
					prepared.workerFinishedTime, RouteClock::now());
			}
		}
		if(world_) {
			world_->CollectRetiredRoots();
		}
		const NavigationAsyncBuildStatus asyncStatus = asyncBuildCoordinator_.Status();
		contractTestWorkAvailable_.store(
			!pendingGroundBuilds_.empty() || !preparedGroundBuilds_.empty()
				|| !pendingDynamicChanges_.empty() || !pendingDynamicRebuilds_.empty()
				|| !preparedDynamicRebuilds_.empty() || activeDynamicBuildGroup_.has_value()
				|| preparedDynamicBuildGroup_.has_value()
				|| !pendingRoutes_.empty() || !inFlightRoutes_.empty() || !preparedRoutes_.empty()
				// Retired tasks must keep the scheduler alive across a later Initialize
				// so completion polling releases their permits instead of leaking capacity.
				|| asyncStatus.currentCount != 0U || asyncStatus.retiredCount != 0U,
			std::memory_order_release);
		// Route aggregates are updated at the protected capture/worker handoff and
		// publication points above. Publish the copied scalar snapshot while this
		// mutex is still held so readers never observe a partially updated group.
		PublishAsyncBuildDiagnosticsLocked();
	}
}
