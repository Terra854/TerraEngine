// NavigationSystem.cpp - bounded worker preparation and protected publication.

#include "Navigation/NavigationSystem.h"

#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/SystemScheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Navigation {
	namespace {
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

		[[nodiscard]] std::int64_t FloorShard(std::int32_t tile) noexcept {
			return tile >= 0 ? tile / 8 : -static_cast<std::int64_t>((-static_cast<std::int64_t>(tile) + 7) / 8);
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

		[[nodiscard]] bool RouteIntersectsLocalizedChange(
			const Ground::GroundRouteQuery::RetainedRoute& route,
			const NavigationProfile& profile,
			const Scene::WorldPosition& origin,
			const Scene::NavigationSourceMutationIntent& intent,
			const NavigationDynamicSourceChange& change) noexcept {
			// Missing bounds cannot prove a route is unaffected, so fail safe and
			// invalidate. Complete bounds let unrelated routes remain available.
			if(!route.IsValid() || route.points.empty()
				|| (!intent.oldBounds.valid && !intent.newBounds.valid)) return true;
			NavigationGeometryBounds routeBounds{};
			routeBounds.minimum = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max()};
			routeBounds.maximum = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
				std::numeric_limits<float>::lowest()};
			for(const NavigationLocation& point : route.points) {
				const Scene::WorldLocalizationResult local = Scene::LocalizeWorldPosition(
					point.position, origin, Scene::kTransformSpaceMaximumRelativeRadiusMetres);
				if(!local.Succeeded()) return true;
				const NavigationGeometryVector3 value{static_cast<float>(local.offsetMetres.x),
					static_cast<float>(local.offsetMetres.y), static_cast<float>(local.offsetMetres.z)};
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
			const auto intersects = [&routeBounds](const NavigationGeometryBounds& affected) {
				return routeBounds.minimum.x <= affected.maximum.x && routeBounds.maximum.x >= affected.minimum.x
					&& routeBounds.minimum.y <= affected.maximum.y && routeBounds.maximum.y >= affected.minimum.y
					&& routeBounds.minimum.z <= affected.maximum.z && routeBounds.maximum.z >= affected.minimum.z;
			};
			return (intent.oldBounds.valid && intersects(change.previousBounds))
				|| (intent.newBounds.valid && intersects(change.currentBounds));
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
						span.minimumClearanceMetres, span.supported, span.clearanceValid});
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
		requestStore_ = std::make_unique<NavigationRequestStore>(settings_.maxAdmittedRequests);
		routeStore_ = std::make_unique<NavigationRouteStore>(settings_.maxAdmittedRequests);
		pendingGroundBuilds_.clear();
		preparedGroundBuilds_.clear();
		pendingDynamicChanges_.clear();
		pendingDynamicRebuilds_.clear();
		preparedDynamicRebuilds_.clear();
		dynamicSourceStages_.clear();
		automaticGroundSourceStage_.reset();
		pendingRoutes_.clear();
		deferredRoutes_.clear();
		preparedRoutes_.clear();
		publishedShards_.clear();
		publishedRouteData_.clear();
		requestRoutes_.clear();
		replacementIntents_.clear();
		invalidations_.clear();
		nextInvalidationCursor_ = 1U;
		latestAutomaticSourceIndexRevision_ = 0U;
		initialized_.store(true, std::memory_order_release);
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
		contractTestWorkAvailable_.store(false, std::memory_order_release);
		pendingGroundBuilds_.clear();
		preparedGroundBuilds_.clear();
		pendingDynamicChanges_.clear();
		pendingDynamicRebuilds_.clear();
		preparedDynamicRebuilds_.clear();
		dynamicSourceStages_.clear();
		automaticGroundSourceStage_.reset();
		pendingRoutes_.clear();
		deferredRoutes_.clear();
		preparedRoutes_.clear();
		publishedShards_.clear();
		publishedRouteData_.clear();
		requestRoutes_.clear();
		replacementIntents_.clear();
		requestStore_.reset();
		routeStore_.reset();
		world_.reset();
		initialized_.store(false, std::memory_order_release);
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
			kResourceNavigationPreparedCandidates
		};
		declaration.writes = {
			kResourceNavigationEligibleSourceIndex,
			kResourceNavigationWorldRoot,
			kResourceNavigationRouteValidity,
			kResourceNavigationResults,
			kResourceNavigationDiagnostics
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
		if(!initialized_.load(std::memory_order_acquire)
			|| !contractTestWorkAvailable_.load(std::memory_order_acquire)) {
			return {};
		}

		auto prepare = MakeWorkerPreparePhase(
			"NavigationPrepare",
			SystemName(),
			{kResourceNavigationCaptureInputs, kResourceNavigationEligibleSourceIndex,
				kResourceNavigationWorldRoot,
				kResourcePublishedSpatialSnapshot, kResourcePublishedWorldRegionResidency,
				kResourceTerrainPublishedCollision, kResourceNavigationGeometryPublications,
				kResourceNavigationRequestQueue},
			{kResourceNavigationPreparedCandidates},
			"NavigationPrepare");
		prepare.phase = "Navigation";
		prepare.stableDescriptorId = "NavigationSystem/Navigation/Prepare";
		prepare.safeSubwork = "Prepare one bounded immutable navigation candidate without live scene or provider publication access.";
		prepare.estimatedWorkItemCount = 1U;
		prepare.work = [this](JobGraphContext&) { PrepareAvailableWork(); };

		auto publish = MakeProtectedCommitPhase(
			"NavigationPublish",
			SystemName(),
			{kResourceNavigationPreparedCandidates, kResourceSceneChangePublication,
				kResourceNavigationCaptureInputs, kResourceNavigationEligibleSourceIndex,
				kResourceNavigationWorldRoot},
			{kResourceNavigationEligibleSourceIndex, kResourceNavigationWorldRoot,
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

		return {std::move(prepare), std::move(publish)};
	}

	std::shared_ptr<const NavigationWorldRoot> NavigationSystem::PublishedWorld() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return world_ ? world_->PublishedRoot() : nullptr;
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
		std::shared_ptr<const NavigationEligibleSourceIndex> eligibleSourceIndex) {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire) || !world_) return false;
		if(eligibleSourceIndex != nullptr) {
			latestAutomaticSourceIndexRevision_ = eligibleSourceIndex->Revision();
		}
		const std::shared_ptr<const NavigationWorldRoot> current = world_->PublishedRoot();
		if(!current) return false;
		NavigationWorldRoot candidate = world_->CreateCandidate();
		candidate.terrainState = NavigationTerrainState::Unavailable;
		candidate.terrainPublicationGeneration = 0U;
		candidate.sourceIndexRevision = eligibleSourceIndex != nullptr ? eligibleSourceIndex->Revision()
			: candidate.rootGeneration;
		candidate.eligibleSourceIndex = std::move(eligibleSourceIndex);
		candidate.coverage.clear();
		candidate.shards.clear();
		candidate.generatedPortals.clear();
		candidate.routeValidityGeneration = candidate.rootGeneration;
		candidate.routeValidity = {candidate.routeValidityGeneration, 0U};
		if(!world_->Publish(std::move(candidate), current->rootGeneration)) return false;

		// The root becomes unavailable before any later build. Clear the detached
		// route topology under this same system lock so no request can observe the
		// unavailable root alongside old shards or portals.
		publishedShards_.clear();
		automaticGroundSourceStage_.reset();
		for(const DeferredRoute& deferred : deferredRoutes_) {
			if(!requestStore_) break;
			(void)requestStore_->Update(deferred.handle,
				[](NavigationRequestRecord& request) {
					request.lifecycle = NavigationRequestLifecycle::Terminal;
					request.outcome = NavigationOutcome::UnavailableCoverage;
					request.reason = NavigationReason::MissingTerrain;
				});
		}
		deferredRoutes_.clear();
		const Ground::GroundPortalGraphSnapshot portals = portalGraph_.AcquireSnapshot();
		(void)portalGraph_.PublishBoundaryIntervals({}, portals.Revision());
		for(const NavigationRouteHandle route : requestRoutes_) {
			if(!route.IsValid() || !routeStore_ || routeStore_->Status(route) != NavigationHandleStatus::Alive) continue;
			const auto routeRecord = routeStore_->Read(route);
			if(!routeRecord.record) continue;
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
		}
		return true;
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
		// A source bridge can arrive after a committed mutation in editor tooling.
		// Keep the scheduler awake so that retained dirty work is prepared next.
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
		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
		return retainedProfiles.contains(profileBuildKey)
			|| retainedProfiles.size() < settings_.maxActiveProfileBuilds;
	}

	bool NavigationSystem::QueueOnDemandProfileBuildLocked(const NavigationProfile& profile) {
		if(!profile.Validate().accepted || !automaticGroundSourceStage_ || !world_) return false;
		const std::shared_ptr<const NavigationWorldRoot> root = world_->PublishedRoot();
		const AutomaticGroundSourceStage& stage = *automaticGroundSourceStage_;
		if(!root || root->terrainState != NavigationTerrainState::Available
			|| !stage.eligibleSourceIndex || !stage.eligibleSourceIndex->HasCurrentTerrain()
			|| stage.terrainPublicationGeneration == 0U
			|| stage.profileConfigurationRevision != profileConfigurationRevision_) return false;

		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
		const auto matchesProfile = [profileBuildKey](const auto& build) {
			return MakeGroundProfileBuildKey(build.profile) == profileBuildKey;
		};
		if(HasReadyCoverageForProfileLocked(profile)
			|| std::any_of(pendingGroundBuilds_.begin(), pendingGroundBuilds_.end(), matchesProfile)
			|| std::any_of(preparedGroundBuilds_.begin(), preparedGroundBuilds_.end(), matchesProfile)) return true;
		if(!HasProfileBuildCapacityLocked(profile)) return false;

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
		return true;
	}

	void NavigationSystem::QueueRouteAgainstPublishedWorldLocked(NavigationRequestHandle handle,
		std::uint64_t intentGeneration, Ground::GroundRouteQuery::Request request) {
		// Admission snapshots the completed publication only after matching profile
		// coverage exists. The worker can therefore remain wholly scene-independent.
		pendingRoutes_.push_back({handle, intentGeneration, std::move(request),
			world_ ? world_->PublishedRoot() : nullptr, publishedShards_, portalGraph_.AcquireSnapshot()});
	}

	void NavigationSystem::PromoteDeferredRoutesForProfileLocked(const NavigationProfile& profile) {
		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
		for(auto iterator = deferredRoutes_.begin(); iterator != deferredRoutes_.end();) {
			if(MakeGroundProfileBuildKey(iterator->request.profile) != profileBuildKey) {
				++iterator;
				continue;
			}
			if(requestStore_ && requestStore_->Status(iterator->handle) == NavigationHandleStatus::Alive) {
				QueueRouteAgainstPublishedWorldLocked(iterator->handle, iterator->intentGeneration,
					std::move(iterator->request));
			}
			iterator = deferredRoutes_.erase(iterator);
		}
	}

	void NavigationSystem::ResolveDeferredProfileBuildFailureLocked(const NavigationProfile& profile,
		bool mayRequeueFromNewCapture) {
		// A newly captured automatic packet may have replaced the failed candidate.
		// Requeue from that complete packet when possible; otherwise make the request
		// terminal so callers never wait forever for coverage that cannot be built.
		if(mayRequeueFromNewCapture && QueueOnDemandProfileBuildLocked(profile)) return;
		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
		for(auto iterator = deferredRoutes_.begin(); iterator != deferredRoutes_.end();) {
			if(MakeGroundProfileBuildKey(iterator->request.profile) != profileBuildKey) {
				++iterator;
				continue;
			}
			if(requestStore_) {
				(void)requestStore_->Update(iterator->handle,
					[](NavigationRequestRecord& request) {
						request.lifecycle = NavigationRequestLifecycle::Terminal;
						request.outcome = NavigationOutcome::UnavailableCoverage;
						request.reason = NavigationReason::MissingNeighbor;
					});
			}
			iterator = deferredRoutes_.erase(iterator);
		}
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
		if(HasReadyCoverageForProfileLocked(request.profile)
			|| !QueueOnDemandProfileBuildLocked(request.profile)) {
			QueueRouteAgainstPublishedWorldLocked(created.handle, 0U, std::move(request));
		} else {
			deferredRoutes_.push_back({created.handle, 0U, std::move(request)});
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

	NavigationSystem::ObjectRouteReceipt NavigationSystem::SubmitCapturedObjectRouteLocked(
		const NavigationObjectEndpoint& source, const NavigationObjectEndpoint* target,
		NavigationLocation destination, NavigationProfile profile,
		NavigationRoutePolicy policy, std::uint64_t replacementKey) {
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
		if(replacementKey != 0U) {
			for(auto iterator = replacementIntents_.begin(); iterator != replacementIntents_.end();) {
				if(iterator->owner != owner || iterator->key != replacementKey) {
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
				std::erase_if(deferredRoutes_, [replacedRequest](const DeferredRoute& deferred) {
					return deferred.handle == replacedRequest;
				});
				iterator = replacementIntents_.erase(iterator);
			}
			replacementIntents_.push_back({owner, replacementKey, created.handle});
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
		if(HasReadyCoverageForProfileLocked(request.profile)
			|| !QueueOnDemandProfileBuildLocked(request.profile)) {
			QueueRouteAgainstPublishedWorldLocked(created.handle, 1U, std::move(request));
		} else {
			deferredRoutes_.push_back({created.handle, 1U, std::move(request)});
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
		if(request.record->outcome != NavigationOutcome::Complete) {
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
		return {validation.usable, validation.outcome, validation.reason};
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
		const NavigationWorldHandle previous = world_->Handle();
		const NavigationGeneration nextGeneration = previous.worldGeneration
			== std::numeric_limits<NavigationGeneration>::max()
			? 1U : previous.worldGeneration + 1U;
		world_ = std::make_unique<NavigationWorld>(NavigationWorldHandle{previous.worldId, nextGeneration});
		requestStore_ = std::make_unique<NavigationRequestStore>(settings_.maxAdmittedRequests);
		routeStore_ = std::make_unique<NavigationRouteStore>(settings_.maxAdmittedRequests);
		pendingGroundBuilds_.clear();
		preparedGroundBuilds_.clear();
		pendingDynamicChanges_.clear();
		pendingDynamicRebuilds_.clear();
		preparedDynamicRebuilds_.clear();
		dynamicSourceStages_.clear();
		automaticGroundSourceStage_.reset();
		pendingRoutes_.clear();
		deferredRoutes_.clear();
		preparedRoutes_.clear();
		publishedShards_.clear();
		publishedRouteData_.clear();
		requestRoutes_.clear();
		replacementIntents_.clear();
		invalidations_.clear();
		nextInvalidationCursor_ = 1U;
		contractTestWorkAvailable_.store(false, std::memory_order_release);
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

		std::size_t acceptedCount = 0U;
		for(const Scene::NavigationSourceMutationReceipt& receipt : receipts) {
			if(!receipt.accepted || receipt.transactionStatus != Scene::TransactionStatus::Committed) continue;
			const Scene::NavigationSourceMutationIntent& intent = receipt.intent;
			Scene::WorldPosition origin{};
			origin.cell = intent.oldBounds.valid ? intent.oldBounds.minimum.cell : intent.newBounds.minimum.cell;
			NavigationDynamicSourceChange change{};
			change.source = NavigationUuid{intent.sourceId};
			change.sourceRevision = intent.committedSourceRevision;
			change.physicalReadyRevision = intent.physicalReadyRevision;
			change.kind = ToDynamicChangeKind(intent.operation);
			if(!LocalizeMutationBounds(intent.oldBounds, origin, change.previousBounds)
				|| !LocalizeMutationBounds(intent.newBounds, origin, change.currentBounds)) {
				++dynamicDiagnostics_.rejectedUpdateCount;
				continue;
			}
			if(!dirtyIndex_.CommitSourceChange(change).accepted) {
				++dynamicDiagnostics_.rejectedUpdateCount;
				continue;
			}
			++acceptedCount;
			// The dirty index owns revision ordering. Keep a matching copied change
			// packet for worker preparation; a newer commit will deterministically
			// supersede this one at publication rather than mutate a live root.
			pendingDynamicChanges_.push_back(change);
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
					origin, intent, change)) continue;
				if(invalidations_.size() >= settings_.maxAdmittedRequests) invalidations_.erase(invalidations_.begin());
				invalidations_.push_back({nextInvalidationCursor_++, route.owner, route,
					NavigationReason::DirtyCoverage, NavigationOutcome::Stale, change.sourceRevision,
					lastProtectedBoundaryGeneration_});
				if(requestStore_) (void)requestStore_->Update(routeRecord.record->request,
					[](NavigationRequestRecord& current) {
						current.outcome = NavigationOutcome::Stale;
						current.reason = NavigationReason::DirtyCoverage;
						current.lifecycle = NavigationRequestLifecycle::Terminal;
					});
			}
		}
		if(acceptedCount == 0U) return 0U;

		const std::shared_ptr<const NavigationWorldRoot> current = world_->PublishedRoot();
		NavigationWorldRoot candidate = world_->CreateCandidate();
		candidate.tombstones = dirtyIndex_.ObserveAtProtectedBoundary().entries;
		for(const NavigationDirtyCoverageDependency& dirty : dirtyIndex_.DirtyCoverage()) {
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
			if(dirtyIndex_.LatestSourceRevision(NavigationUuid{receipt.intent.sourceId})
				!= receipt.intent.committedSourceRevision) continue;
			const auto existing = std::find_if(candidate.sources.begin(), candidate.sources.end(),
				[&receipt](const NavigationSourceIndexRecord& source) {
					return source.sourceId == receipt.intent.sourceId;
				});
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
		if(!world_->Publish(std::move(candidate), current->rootGeneration)) {
			++dynamicDiagnostics_.rejectedUpdateCount;
			diagnostics_.PublishDynamic(dynamicDiagnostics_);
			return 0U;
		}
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

	void NavigationSystem::SetContractTestWorkAvailable(bool available) noexcept {
		contractTestWorkAvailable_.store(available, std::memory_order_release);
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

		const auto coverageFor = [](const NavigationLocation& location, std::uint64_t profileBuildKey)
			-> std::optional<NavigationCoverageKey> {
			// A canonical cell is exactly 256 horizontal shards and 512 vertical
			// bands. Keep this arithmetic in int64 so reference shifts do not lose
			// identity through a float conversion before coverage lookup.
			constexpr std::int64_t kShardsPerCell = static_cast<std::int64_t>(Scene::kWorldCellEdgeMetres / (kGroundTileEdgeMetres * 8.0F));
			constexpr std::int64_t kBandsPerCell = static_cast<std::int64_t>(Scene::kWorldCellEdgeMetres / kGroundVerticalBandMetres);
			const auto addCell = [](std::int64_t cell, std::int64_t scale, std::int64_t local) -> std::optional<std::int64_t> {
				if(cell > (std::numeric_limits<std::int64_t>::max() - local) / scale
					|| cell < (std::numeric_limits<std::int64_t>::min() - local) / scale) return std::nullopt;
				return cell * scale + local;
			};
			const auto horizontalLocal = [](double offset) { return static_cast<std::int64_t>(std::floor(offset / (kGroundTileEdgeMetres * 8.0F))); };
			const auto verticalLocal = [](double offset) { return static_cast<std::int64_t>(std::floor(offset / kGroundVerticalBandMetres)); };
			const auto shardX = addCell(location.position.cell.x, kShardsPerCell, horizontalLocal(location.position.offsetMetres.x));
			const auto shardZ = addCell(location.position.cell.z, kShardsPerCell, horizontalLocal(location.position.offsetMetres.z));
			const auto band = addCell(location.position.cell.y, kBandsPerCell, verticalLocal(location.position.offsetMetres.y));
			if(!shardX || !shardZ || !band || *band < std::numeric_limits<std::int32_t>::min()
				|| *band > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
			return NavigationCoverageKey{*shardX, *shardZ, static_cast<std::int32_t>(*band), profileBuildKey};
		};
		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(request.profile);
		const auto startKey = coverageFor(request.start, profileBuildKey);
		const auto destinationKey = coverageFor(request.destination, profileBuildKey);
		if(!startKey || !destinationKey) {
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
			if(coverage.key.shardX < std::numeric_limits<std::int32_t>::min()
				|| coverage.key.shardX > std::numeric_limits<std::int32_t>::max()
				|| coverage.key.shardZ < std::numeric_limits<std::int32_t>::min()
				|| coverage.key.shardZ > std::numeric_limits<std::int32_t>::max()) return {};
			const GroundShardKey key{static_cast<std::int32_t>(coverage.key.shardX), static_cast<std::int32_t>(coverage.key.shardZ),
				coverage.key.verticalBand, coverage.key.profileBuildKey};
			const auto found = std::find_if(shards.begin(), shards.end(), [&key, &coverage](const auto& candidate) {
				return candidate && candidate->reference.generation == coverage.coverageGeneration && candidate->reference.shard == key;
			});
			return found == shards.end() ? std::shared_ptr<const DetourGroundShardGeneration>{} : *found;
		};
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
		const auto querySegment = [&request](const DetourGroundShardGeneration& shard,
			const NavigationGeometryVector3& start, const NavigationGeometryVector3& destination) {
			DetourGroundRouteQuery query{};
			query.start = start; query.destination = destination;
			query.horizontalSearchRadius = static_cast<float>(request.profile.radiusMetres + request.profile.clearanceMarginMetres + 1.0);
			query.verticalSearchRadius = static_cast<float>(request.profile.heightMetres + request.profile.stepHeightMetres + 2.0);
			query.maximumPoints = request.maxExpansions;
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
			const double seam = portal.boundaryAxis == 0U
				? static_cast<double>(portal.toCoverage.shardX > from.shardX ? from.shardX + 1 : from.shardX) * 256.0
				: static_cast<double>(portal.toCoverage.shardZ > from.shardZ ? from.shardZ + 1 : from.shardZ) * 256.0;
			const double midpoint = (portal.intervalStart + portal.intervalEnd) * 0.5;
			const double localX = portal.boundaryAxis == 0U ? seam - static_cast<double>(key.shardX) * 256.0
				: midpoint - static_cast<double>(key.shardX) * 256.0;
			const double localZ = portal.boundaryAxis == 0U ? midpoint - static_cast<double>(key.shardZ) * 256.0
				: seam - static_cast<double>(key.shardZ) * 256.0;
			return NavigationGeometryVector3{static_cast<float>(localX), 0.0F, static_cast<float>(localZ)};
		};
		std::vector<NavigationLocation> canonicalPoints;
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
			if(segment.outcome != NavigationOutcome::Complete) { result.outcome = segment.outcome; result.reason = segment.reason; return result; }
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
				if(canonicalPoints.empty() || canonicalPoints.back() != location)
					canonicalPoints.push_back(location);
			}
		}
		if(canonicalPoints.empty()) { result.outcome = NavigationOutcome::Unreachable; result.reason = NavigationReason::MissingSupport; return result; }
		// Detour returns points projected onto its polygon surface. Preserve those
		// interior waypoints, but keep the public route anchored to the exact
		// generation-qualified object pose and destination captured for this
		// request. Gameplay can therefore consume the first point without guessing
		// whether it represents the object's current position.
		if(canonicalPoints.front() != request.start)
			canonicalPoints.insert(canonicalPoints.begin(), request.start);
		else
			canonicalPoints.front() = request.start;
		if(canonicalPoints.back() != request.destination)
			canonicalPoints.push_back(request.destination);
		else
			canonicalPoints.back() = request.destination;
		result.points = std::move(canonicalPoints);
		result.outcome = NavigationOutcome::Complete;
		result.reason = NavigationReason::None;
		return result;
	}

	void NavigationSystem::PrepareAvailableWork() {
		std::optional<PendingGroundBuild> groundBuild;
		std::optional<PendingDynamicRebuild> dynamicBuild;
		std::optional<PendingRoute> route;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if(!pendingGroundBuilds_.empty()) {
				groundBuild = std::move(pendingGroundBuilds_.front());
				pendingGroundBuilds_.pop_front();
			}
			if(pendingDynamicRebuilds_.empty() && !pendingDynamicChanges_.empty()
				&& world_ && dirtyIndex_.LatestSourceRevision() == pendingDynamicChanges_.front().sourceRevision
				&& dirtyIndex_.LatestSourceRevision(pendingDynamicChanges_.front().source)
					== pendingDynamicChanges_.front().sourceRevision) {
				const NavigationDynamicSourceChange change = pendingDynamicChanges_.front();
				const std::shared_ptr<const NavigationWorldRoot> root = world_->PublishedRoot();
				if(root) {
					for(const DynamicSourceStage& stage : dynamicSourceStages_) {
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
						if(RequiresReplacementSnapshot(change.kind) && !HasCommittedSnapshot(sources, change)) continue;
						NavigationDynamicRebuildRequest request{};
						request.change = change;
						request.expectedRootGeneration = root->rootGeneration;
						request.profile = stage.profile;
						request.tombstones = dirtyIndex_.ObserveAtProtectedBoundary();
						request.dirtyCoverage = dirtyIndex_.DirtyCoverage();
						request.sourceIndex = root->sources;
						request.sources = std::move(sources);
						pendingDynamicRebuilds_.push_back({change, std::move(request)});
					}
				}
				// Leave the committed change queued when the source bridge has not yet
				// supplied its matching immutable revision. Preparing from an old mesh
				// would make a dynamic obstacle silently traversable again.
				if(!pendingDynamicRebuilds_.empty()) pendingDynamicChanges_.pop_front();
			} else if(!pendingDynamicChanges_.empty()
				&& dirtyIndex_.LatestSourceRevision() != pendingDynamicChanges_.front().sourceRevision) {
				// A newer globally ordered change rebuilds the merged dirty set, so an
				// older packet can never pass the publication revision check.
				pendingDynamicChanges_.pop_front();
			}
			if(!pendingDynamicRebuilds_.empty()) {
				dynamicBuild = std::move(pendingDynamicRebuilds_.front());
				pendingDynamicRebuilds_.pop_front();
			}
			if(!pendingRoutes_.empty()) {
				route = std::move(pendingRoutes_.front());
				pendingRoutes_.pop_front();
			}
		}

		std::optional<PreparedGroundBuild> builtCoverage;
		if(groundBuild) {
			RecastGroundBuildInput buildInput{};
			buildInput.profile = groundBuild->profile;
			buildInput.sources = std::move(groundBuild->sources);
			const RecastGroundBuildResult tiles = BuildRecastGroundTiles(buildInput);
			PreparedGroundBuild prepared{};
			prepared.accepted = tiles.Succeeded();
			prepared.profile = groundBuild->profile;
			prepared.sourceIndex = std::move(groundBuild->sourceIndex);
			prepared.eligibleSourceIndex = std::move(groundBuild->eligibleSourceIndex);
			prepared.terrainPublicationGeneration = groundBuild->terrainPublicationGeneration;
			prepared.profileConfigurationRevision = groundBuild->profileConfigurationRevision;
			if(prepared.accepted) {
				using GroupKey = std::tuple<std::int32_t, std::int32_t, std::int32_t>;
				std::map<GroupKey, std::vector<GroundTileBlob>> groups;
				auto floorShard = [](std::int32_t tile) {
					return tile >= 0 ? tile / 8 : -static_cast<std::int32_t>((-static_cast<std::int64_t>(tile) + 7) / 8);
				};
				for(const GroundTileBlob& tile : tiles.tiles) {
					groups[{floorShard(tile.key.x), floorShard(tile.key.z), tile.key.verticalBand}].push_back(tile);
				}
				for(auto& [key, groupTiles] : groups) {
					GroundShardKey shard{std::get<0>(key), std::get<1>(key), std::get<2>(key),
						MakeGroundProfileBuildKey(groundBuild->profile)};
					DetourGroundShardBuildInput shardInput{shard, 1U, std::move(groupTiles)};
					shardInput.minimumClearanceMetres = static_cast<float>(
						groundBuild->profile.clearanceMarginMetres);
					const DetourGroundShardBuildResult assembled = AssembleDetourGroundShard(shardInput);
					if(!assembled.accepted) { prepared.accepted = false; prepared.shards.clear(); prepared.coverage.clear(); break; }
					prepared.shards.push_back(assembled.generation);
					prepared.coverage.push_back({{shard.x, shard.z, shard.verticalBand, shard.profileBuildKey},
						assembled.generation->reference.generation, NavigationCoverageState::Ready, 0U, true});
				}
			}
			builtCoverage = std::move(prepared);
		}

		std::optional<PreparedDynamicRebuild> rebuiltDynamicCoverage;
		if(dynamicBuild) {
			rebuiltDynamicCoverage = {dynamicBuild->change,
				PrepareDynamicGroundReplacement(dynamicBuild->request)};
		}

		std::optional<PreparedRoute> searchedRoute;
		if(route) {
			searchedRoute = PreparedRoute{route->handle, route->intentGeneration,
				SearchPublishedGroundRoute(route->request, route->worldRoot, route->shards, route->portals),
				route->worldRoot ? route->worldRoot->world : NavigationWorldHandle{},
				route->worldRoot ? route->worldRoot->rootGeneration : 0U};
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if(builtCoverage) preparedGroundBuilds_.push_back(std::move(*builtCoverage));
			if(rebuiltDynamicCoverage) preparedDynamicRebuilds_.push_back(std::move(*rebuiltDynamicCoverage));
			if(searchedRoute) preparedRoutes_.push_back(std::move(*searchedRoute));
		}
	}

	void NavigationSystem::PublishPreparedWork() {
		std::lock_guard<std::mutex> lock(mutex_);
		if(world_ && !preparedGroundBuilds_.empty()) {
			PreparedGroundBuild preparedGroundBuild = std::move(preparedGroundBuilds_.front());
			preparedGroundBuilds_.pop_front();
			if(preparedGroundBuild.accepted) {
				if(preparedGroundBuild.profileConfigurationRevision != 0U
					&& preparedGroundBuild.profileConfigurationRevision != profileConfigurationRevision_) {
					// Profile edits select different traversal constraints and shard keys.
					// Rejecting the older copied packet prevents a mixed world tuple.
					++dynamicDiagnostics_.staleDropCount;
					ResolveDeferredProfileBuildFailureLocked(preparedGroundBuild.profile, true);
					return;
				}
				if(preparedGroundBuild.eligibleSourceIndex != nullptr
					&& preparedGroundBuild.eligibleSourceIndex->Revision()
						!= latestAutomaticSourceIndexRevision_) {
					// A later protected capture observed scene, transform, terrain,
					// resource, streaming, or procedural evidence. Drop this complete
					// old candidate before it can exchange only part of the world tuple.
					++dynamicDiagnostics_.staleDropCount;
					ResolveDeferredProfileBuildFailureLocked(preparedGroundBuild.profile, true);
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
				if(world_->Publish(std::move(candidate), current->rootGeneration)) {
					if(replacesAutomaticCapture) {
						// The root exchange succeeded, so detached shards/stages can now be
						// replaced safely. Clearing earlier would violate rejected-candidate
						// atomicity by leaving the current root without its matching shards.
						publishedShards_.clear();
						const std::uint64_t retainedProfileKey = MakeGroundProfileBuildKey(preparedGroundBuild.profile);
						std::erase_if(dynamicSourceStages_, [retainedProfileKey](const DynamicSourceStage& stage) {
							return MakeGroundProfileBuildKey(stage.profile) != retainedProfileKey;
						});
					}
					for(const auto& replacement : preparedGroundBuild.shards) {
						if(!replacement) continue;
						auto existing = std::find_if(publishedShards_.begin(), publishedShards_.end(),
							[&replacement](const auto& currentShard) {
								return currentShard && currentShard->reference.shard == replacement->reference.shard;
							});
						if(existing == publishedShards_.end()) publishedShards_.push_back(replacement);
						else *existing = replacement;
					}
					// Detour extracts only polygons that genuinely reach a shard exterior.
					// Pairing them through GroundPortalGraph therefore cannot manufacture a
					// seam merely because two coverage keys happen to be adjacent.
					std::vector<Ground::GroundBoundaryInterval> intervals;
					for(const auto& shard : publishedShards_) {
						if(!shard) continue;
						const NavigationCoverageKey key{shard->reference.shard.x, shard->reference.shard.z,
							shard->reference.shard.verticalBand, shard->reference.shard.profileBuildKey};
						for(const DetourGroundBoundarySpan& span : CollectDetourGroundBoundarySpans(*shard)) {
							intervals.push_back({key, shard->reference.generation, span.boundaryAxis,
								span.positiveSide, span.layer, span.intervalStart, span.intervalEnd,
								span.minimumClearanceMetres, span.supported, span.clearanceValid});
						}
					}
					const Ground::GroundPortalGraphSnapshot priorPortals = portalGraph_.AcquireSnapshot();
					(void)portalGraph_.PublishBoundaryIntervals(intervals, priorPortals.Revision());
					// The complete profile-qualified root and its portal snapshot are now
					// published together. Only then may requests awaiting this profile enter
					// a worker, preventing a stale default-profile route from being searched.
					PromoteDeferredRoutesForProfileLocked(preparedGroundBuild.profile);
				}
			} else {
				ResolveDeferredProfileBuildFailureLocked(preparedGroundBuild.profile, false);
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
				continue;
			}

			const NavigationPublicationReceipt receipt = PublishNavigationCandidateAtProtectedBoundary(
				*world_, dirtyIndex_, prepared.plan.candidate);
			if(!receipt.accepted) {
				++dynamicDiagnostics_.staleDropCount;
				// A root race is expected when more than one profile is rebuilt from one
				// source change. Requeue the copied change so the next worker captures the
				// latest complete root instead of attempting an in-place repair.
				if(dirtyIndex_.LatestSourceRevision(prepared.change.source) == prepared.change.sourceRevision
					&& dirtyIndex_.LatestSourceRevision() == prepared.change.sourceRevision) {
					const bool alreadyQueued = std::any_of(pendingDynamicChanges_.begin(), pendingDynamicChanges_.end(),
						[&prepared](const NavigationDynamicSourceChange& change) {
							return change.source == prepared.change.source
								&& change.sourceRevision == prepared.change.sourceRevision;
						});
					if(!alreadyQueued) pendingDynamicChanges_.push_front(prepared.change);
				}
				continue;
			}

			// World root, shard cache, and portal graph are replaced while this same
			// protected NavigationSystem mutex is held. Route admission also holds it,
			// so a query observes either the old complete tuple or this new one.
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
			if(!portalGraph_.PublishBoundaryIntervals(intervals, previousPortals.Revision())) {
				// The graph is private to this system and no other writer exists, so this
				// indicates an integration violation. Keeping new shards unpublished makes
				// routes fail closed against the new root rather than use mixed topology.
				++dynamicDiagnostics_.rejectedUpdateCount;
				diagnostics_.Append({NavigationDiagnosticSeverity::Error,
					NavigationMeasurementReason::MeasurementUnavailable, receipt.publishedRootGeneration, 0U,
					"navigation.dynamic_portal_publish_failed",
					"Dynamic root published but its private portal graph revision changed unexpectedly."});
				continue;
			}
			publishedShards_ = std::move(nextShards);
			// The root, shards, and generated portals now expose one coherent source
			// revision. Retire its copied dirty backlog so a long-running world cannot
			// exhaust the bounded admission queue with already-published changes.
			(void)dirtyIndex_.AcknowledgePublishedThrough(prepared.change.sourceRevision);
		}
		while(!preparedRoutes_.empty()) {
			PreparedRoute prepared = std::move(preparedRoutes_.front());
			preparedRoutes_.pop_front();
			if(!requestStore_) continue;
			// A source mutation publishes a new immutable root before route results
			// become visible. Never publish a Complete route calculated from a retired
			// root after its coverage has become dirty, removed, or replaced.
			const std::shared_ptr<const NavigationWorldRoot> currentRoot = world_ ? world_->PublishedRoot() : nullptr;
			if(!currentRoot || currentRoot->world != prepared.world
				|| currentRoot->rootGeneration != prepared.rootGeneration) {
				prepared.result.points.clear();
				prepared.result.outcome = NavigationOutcome::Stale;
				prepared.result.reason = NavigationReason::DirtyCoverage;
			}
			const auto requestRead = requestStore_->Read(prepared.handle);
			// A cancellation keeps its slot allocated for deterministic reads, so the
			// handle can still be alive while the result is no longer publishable.
			if(!requestRead.record || requestRead.record->lifecycle == NavigationRequestLifecycle::Terminal)
				continue;
			// Re-read only at this protected publication boundary. A deleted/reused
			// object cannot receive a worker route that was captured for an older
			// UUID lifetime or transform generation.
			const NavigationRequestRecord& requestRecord = *requestRead.record;
			if(requestRecord.sourceEndpoint.object.IsValid()) {
				const auto currentSource = CaptureObjectEndpointLocked(
					ObjectManager::instance(), requestRecord.sourceEndpoint.object.objectId);
				if(!currentSource || currentSource->object != requestRecord.sourceEndpoint.object
					|| currentSource->transformRevision != requestRecord.sourceEndpoint.transformRevision) {
					prepared.result.points.clear();
					prepared.result.outcome = NavigationOutcome::InvalidEndpoint;
					prepared.result.reason = NavigationReason::InvalidStart;
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
			NavigationRouteHandle routeHandle{};
			if(prepared.result.outcome == NavigationOutcome::Complete && routeStore_) {
				NavigationRouteRecord routeRecord{};
				routeRecord.request = prepared.handle;
				routeRecord.intentGeneration = requestRecord.intentGeneration;
				routeRecord.observation = requestRecord.observation;
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
			(void)requestStore_->Update(prepared.handle, [&prepared](NavigationRequestRecord& record) {
				record.lifecycle = NavigationRequestLifecycle::Terminal;
				record.outcome = prepared.result.outcome;
				record.reason = prepared.result.reason;
			});
		}
		if(world_) {
			world_->CollectRetiredRoots();
		}
		contractTestWorkAvailable_.store(
			!pendingGroundBuilds_.empty() || !preparedGroundBuilds_.empty()
				|| !pendingDynamicChanges_.empty() || !pendingDynamicRebuilds_.empty()
				|| !preparedDynamicRebuilds_.empty()
				|| !pendingRoutes_.empty() || !preparedRoutes_.empty(),
			std::memory_order_release);
	}
}
