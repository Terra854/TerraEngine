// NavigationSystem.cpp - bounded worker preparation and protected publication.

#include "Navigation/NavigationSystem.h"

#include "Components/Navigation/NavigationAgentComponent.h"
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
		agentStore_ = std::make_unique<NavigationAgentStore>(settings_.maxActiveAgents);
		commandStore_ = std::make_unique<NavigationCommandStore>(settings_.maxAdmittedRequests);
		pendingGroundBuilds_.clear();
		preparedGroundBuilds_.clear();
		pendingDynamicChanges_.clear();
		pendingDynamicRebuilds_.clear();
		preparedDynamicRebuilds_.clear();
		dynamicSourceStages_.clear();
		pendingRoutes_.clear();
		preparedRoutes_.clear();
		publishedShards_.clear();
		publishedRouteData_.clear();
		requestRoutes_.clear();
		pendingDestinations_.clear();
		objectAgents_.clear();
		invalidations_.clear();
		nextInvalidationCursor_ = 1U;
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
		pendingRoutes_.clear();
		preparedRoutes_.clear();
		publishedShards_.clear();
		publishedRouteData_.clear();
		requestRoutes_.clear();
		objectAgents_.clear();
		requestStore_.reset();
		routeStore_.reset();
		agentStore_.reset();
		commandStore_.reset();
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
			kResourceNavigationAuthoring,
			kResourceGeometryPublications,
			kResourceNavigationCommandQueue,
			kResourceNavigationPreparedResults
		};
		declaration.writes = {
			kResourceNavigationSourceSnapshot,
			kResourceNavigationCoverageRoot,
			kResourceNavigationAgentState,
			kResourceNavigationResults,
			kResourceNavigationDiagnostics
		};
		declaration.mainThread = false;
		declaration.requiresFlushedSceneMutations = true;
		declaration.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		declaration.boundaryClassification = HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Prepare bounded geometry, BVH, tile, seam, and route candidates from retained immutable inputs.";
		declaration.protectedCommit = "Capture active scene-agent canonical poses, revalidate complete candidate groups, and exchange immutable navigation roots, state, results, and diagnostics.";
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
			{kResourceNavigationSourceSnapshot, kResourceNavigationCoverageRoot,
				kResourcePublishedSpatialSnapshot, kResourcePublishedWorldRegionResidency,
				kResourceGeometryPublications, kResourceNavigationCommandQueue},
			{kResourceNavigationPreparedResults},
			"NavigationPrepare");
		prepare.phase = "Navigation";
		prepare.stableDescriptorId = "NavigationSystem/Navigation/Prepare";
		prepare.safeSubwork = "Prepare one bounded immutable navigation candidate without live scene or provider publication access.";
		prepare.estimatedWorkItemCount = 1U;
		prepare.work = [this](JobGraphContext&) { PrepareAvailableWork(); };

		auto publish = MakeProtectedCommitPhase(
			"NavigationPublish",
			SystemName(),
			{kResourceNavigationPreparedResults, kResourceSceneChangePublication,
				kResourceNavigationSourceSnapshot, kResourceNavigationCoverageRoot},
			{kResourceNavigationCoverageRoot, kResourceNavigationAgentState,
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
		publish.protectedCommit = "Validate scene-agent pose/intent generations and exchange one complete navigation publication group.";
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
			profileKeys.push_back(MakeGroundProfileBuildKey(build.profile));
		}
		std::sort(profileKeys.begin(), profileKeys.end());
		if(std::adjacent_find(profileKeys.begin(), profileKeys.end()) != profileKeys.end()) return false;
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire)
			|| pendingGroundBuilds_.size() + preparedGroundBuilds_.size() + builds.size()
				> settings_.maxActiveProfileBuilds) return false;
		for(InitialGroundBuildInput& build : builds) {
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
				std::move(build.sourceIndex)});
		}
		contractTestWorkAvailable_.store(true, std::memory_order_release);
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
		record.allowPartial = request.allowPartial;
		const auto created = requestStore_->Create(request.owner, std::move(record));
		if(!created.Succeeded()) return {};
		if(requestRoutes_.size() <= created.handle.slotIndex) requestRoutes_.resize(created.handle.slotIndex + 1U);
		requestRoutes_[created.handle.slotIndex] = {};
		// Retain exactly the immutable root and provider-neutral shard generations
		// visible at admission. A later build or dirty source cannot change this
		// worker's meaning; publication revalidates the request separately.
		pendingRoutes_.push_back({created.handle, {}, 0U, 0U, std::move(request), world_->PublishedRoot(), publishedShards_,
			portalGraph_.AcquireSnapshot()});
		contractTestWorkAvailable_.store(true, std::memory_order_release);
		return created.handle;
	}

	NavigationStoreReadResult<NavigationRequestRecord> NavigationSystem::ReadRequest(
		NavigationRequestHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex_);
		return requestStore_ ? requestStore_->Read(handle) : NavigationStoreReadResult<NavigationRequestRecord>{};
	}

	NavigationRouteHandle NavigationSystem::PublishedRouteForRequest(NavigationRequestHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!requestStore_ || requestStore_->Status(handle) != NavigationHandleStatus::Alive
			|| handle.slotIndex >= requestRoutes_.size()) return {};
		return requestRoutes_[handle.slotIndex];
	}

	Ground::GroundRouteQuery::RetainedRoute NavigationSystem::ReadRoute(NavigationRouteHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!routeStore_ || routeStore_->Status(handle) != NavigationHandleStatus::Alive
			|| handle.slotIndex >= publishedRouteData_.size()) return {};
		return publishedRouteData_[handle.slotIndex];
	}

	bool NavigationSystem::CancelRequest(NavigationRequestHandle handle) {
		std::lock_guard<std::mutex> lock(mutex_);
		if(!requestStore_) return false;
		const bool cancelled = requestStore_->Cancel(handle);
		if(cancelled) {
			std::erase_if(pendingRoutes_, [handle](const PendingRoute& pending) { return pending.handle == handle; });
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
		if(!requestStore_) return false;
		const auto request = requestStore_->Read(handle);
		if(!request.record || request.record->lifecycle != NavigationRequestLifecycle::Terminal)
			return false;
		if(handle.slotIndex < requestRoutes_.size()) {
			const NavigationRouteHandle route = requestRoutes_[handle.slotIndex];
			if(route.IsValid() && routeStore_) (void)routeStore_->Release(route);
			requestRoutes_[handle.slotIndex] = {};
			if(route.IsValid() && route.slotIndex < publishedRouteData_.size())
				publishedRouteData_[route.slotIndex] = {};
		}
		return requestStore_->Release(handle);
	}

	NavigationAgentHandle NavigationSystem::RegisterAgentLocked(
		NavigationOwnerHandle owner, NavigationLocation currentPosition, NavigationProfile profile) {
		if(!owner.IsValid() || !profile.Validate().accepted
			|| !ValidateNavigationLocation(currentPosition).Succeeded()
			|| currentPosition.world != owner.world) return {};
		if(!initialized_.load(std::memory_order_acquire) || !agentStore_ || !world_
			|| world_->Handle() != owner.world) return {};
		NavigationAgentRecord record{};
		record.createdTick = lastProtectedBoundaryGeneration_;
		record.profile = std::move(profile);
		record.currentPosition = currentPosition;
		record.destination = currentPosition;
		record.poseGeneration = 1U;
		return agentStore_->Create(owner, std::move(record)).handle;
	}

	NavigationAgentHandle NavigationSystem::RegisterAgent(
		NavigationOwnerHandle owner, NavigationLocation currentPosition, NavigationProfile profile) {
		std::lock_guard<std::mutex> lock(mutex_);
		return RegisterAgentLocked(owner, std::move(currentPosition), std::move(profile));
	}

	NavigationSystem::DestinationCommandReceipt NavigationSystem::QueueSetDestination(
		NavigationAgentHandle agent, NavigationLocation destination, std::uint32_t routePolicy,
		std::uint64_t expectedIntentGeneration) {
		if(!agent.IsValid() || !ValidateNavigationLocation(destination).Succeeded()
			|| destination.world != agent.owner.world || routePolicy > 1U) {
			return {{}, NavigationHandleStatus::Invalid};
		}
		std::lock_guard<std::mutex> lock(mutex_);
		if(!initialized_.load(std::memory_order_acquire) || !agentStore_ || !commandStore_ || !world_)
			return {{}, NavigationHandleStatus::Invalid};
		if(world_->Handle() != agent.owner.world)
			return {{}, NavigationHandleStatus::StaleWorld};
		const auto agentRead = agentStore_->Read(agent);
		if(!agentRead.record) return {{}, agentRead.status};
		if(expectedIntentGeneration != 0U && agentRead.record->intentGeneration != expectedIntentGeneration)
			return {{}, NavigationHandleStatus::StaleSlot};
		NavigationCommandRecord record{};
		record.createdTick = lastProtectedBoundaryGeneration_;
		record.state = NavigationCommandState::Queued;
		record.lifecycle = NavigationRequestLifecycle::Queued;
		record.outcome = NavigationOutcome::Queued;
		const auto command = commandStore_->Create(agent.owner, std::move(record));
		if(!command.Succeeded()) return {{}, command.status};
		pendingDestinations_.push_back({command.handle, agent, destination, routePolicy, expectedIntentGeneration});
		contractTestWorkAvailable_.store(true, std::memory_order_release);
		return {command.handle, NavigationHandleStatus::Alive};
	}

	NavigationStoreReadResult<NavigationCommandRecord> NavigationSystem::ReadCommand(
		NavigationCommandHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex_);
		return commandStore_ ? commandStore_->Read(handle) : NavigationStoreReadResult<NavigationCommandRecord>{};
	}

	NavigationStoreReadResult<NavigationAgentRecord> NavigationSystem::ReadAgent(
		NavigationAgentHandle handle) const {
		std::lock_guard<std::mutex> lock(mutex_);
		return agentStore_ ? agentStore_->Read(handle) : NavigationStoreReadResult<NavigationAgentRecord>{};
	}

	NavigationAgentHandle NavigationSystem::ResolveObjectAgent(const TerraEngine::UUID& objectId) const {
		std::lock_guard<std::mutex> lock(mutex_);
		const auto found = objectAgents_.find(objectId);
		if(found == objectAgents_.end() || !agentStore_
			|| agentStore_->Status(found->second.agent) != NavigationHandleStatus::Alive) return {};
		return found->second.agent;
	}

	bool NavigationSystem::ReleaseAgentLocked(NavigationAgentHandle handle, NavigationReason reason) {
		if(!agentStore_) return false;
		const auto read = agentStore_->Read(handle);
		if(!read.record || read.status != NavigationHandleStatus::Alive) return false;
		const NavigationAgentRecord agent = *read.record;
		if(agent.activeRequest.IsValid() && requestStore_) {
			// This helper already owns mutex_. Calling the public cancellation entry
			// point would recursively lock it, so perform the same bounded state
			// transition directly at this protected boundary.
			(void)requestStore_->Cancel(agent.activeRequest);
			std::erase_if(pendingRoutes_, [&agent](const PendingRoute& pending) {
				return pending.handle == agent.activeRequest;
			});
		}
		if(agent.activeRoute.IsValid()) {
			if(invalidations_.size() >= settings_.maxAdmittedRequests) invalidations_.erase(invalidations_.begin());
			invalidations_.push_back({nextInvalidationCursor_++, handle.owner, agent.activeRoute,
				reason, NavigationOutcome::Stale, 0U, lastProtectedBoundaryGeneration_});
		}
		for(auto iterator = pendingDestinations_.begin(); iterator != pendingDestinations_.end();) {
			if(iterator->agent != handle) { ++iterator; continue; }
			if(commandStore_) (void)commandStore_->Update(iterator->command, [](NavigationCommandRecord& command) {
				command.state = NavigationCommandState::Rejected;
				command.lifecycle = NavigationRequestLifecycle::Terminal;
				command.outcome = NavigationOutcome::Stale;
				command.reason = NavigationReason::SceneReplaced;
			});
			iterator = pendingDestinations_.erase(iterator);
		}
		return agentStore_->Release(handle);
	}

	bool NavigationSystem::ReleaseAgent(NavigationAgentHandle handle) {
		std::lock_guard<std::mutex> lock(mutex_);
		const bool released = ReleaseAgentLocked(handle, NavigationReason::SceneReplaced);
		if(released) std::erase_if(objectAgents_, [handle](const auto& entry) { return entry.second.agent == handle; });
		return released;
	}

	void NavigationSystem::SynchronizeSceneObjectAgents(ObjectManager& objects) {
		std::lock_guard<std::mutex> lock(mutex_);
		SynchronizeSceneObjectAgentsLocked(objects);
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

	void NavigationSystem::SynchronizeSceneObjectAgentsLocked(ObjectManager& objects) {
		if(!initialized_.load(std::memory_order_acquire) || !world_ || !agentStore_) return;
		std::unordered_set<TerraEngine::UUID> observed;
		for(const Object& object : objects.SnapshotObjectsWithComponents<NavigationAgentComponent, Transform>()) {
			const TerraEngine::UUID objectId = objects.getUUID(object);
			NavigationAgentComponent* component = objects.GetComponent<NavigationAgentComponent>(object);
			Transform* transform = objects.GetComponent<Transform>(object);
			if(objectId == TerraEngine::EMPTY_UUID || component == nullptr || transform == nullptr) continue;
			auto componentGuard = component->acquireReadLock();
			auto transformGuard = transform->acquireReadLock();
			if(!component->enabled) continue;
			const NavigationProfileResolution resolvedProfile = profileResolver_.Resolve(*component);
			if(!resolvedProfile.accepted) continue;
			const Scene::WorldCoordinateResult normalized = Scene::NormalizeWorldPosition(transform->worldPose.position);
			if(!normalized.Succeeded()) continue;
			const NavigationLocation pose{world_->Handle(), normalized.position};
			if(!ValidateNavigationLocation(pose).Succeeded()) continue;
			observed.insert(objectId);
			const std::uint64_t lifetime = objects.getObjectLifetimeGeneration(object);
			auto bound = objectAgents_.find(objectId);
			const bool canReuse = bound != objectAgents_.end()
				&& bound->second.objectLifetimeGeneration == lifetime
				&& bound->second.profileId == resolvedProfile.profile.profileId
				&& bound->second.profileRevision == resolvedProfile.profile.revision
				&& agentStore_->Status(bound->second.agent) == NavigationHandleStatus::Alive;
			if(!canReuse) {
				if(bound != objectAgents_.end()) (void)ReleaseAgentLocked(bound->second.agent, NavigationReason::SceneReplaced);
				NavigationOwnerId ownerId{TerraEngine::MakeObjectStableHash(objectId)};
				if(!ownerId.IsValid()) ownerId.value = 1U;
				NavigationOwnerHandle owner{world_->Handle(), ownerId, lifetime == 0U ? 1U : lifetime};
				const NavigationAgentHandle agent = RegisterAgentLocked(owner, pose, resolvedProfile.profile);
				if(!agent.IsValid()) { objectAgents_.erase(objectId); continue; }
				const std::uint32_t authoredRoutePolicy = component->GetRoutePolicyValue();
				(void)agentStore_->Update(agent, [authoredRoutePolicy](NavigationAgentRecord& runtimeAgent) {
					runtimeAgent.routePolicy = authoredRoutePolicy;
				});
				objectAgents_[objectId] = {
					agent, lifetime, resolvedProfile.profile.profileId, resolvedProfile.profile.revision};
				continue;
			}
			(void)agentStore_->Update(bound->second.agent, [&pose](NavigationAgentRecord& agent) {
				if(agent.currentPosition != pose) {
					agent.currentPosition = pose;
					++agent.poseGeneration;
				}
			});
		}
		for(auto iterator = objectAgents_.begin(); iterator != objectAgents_.end();) {
			if(observed.contains(iterator->first)) { ++iterator; continue; }
			(void)ReleaseAgentLocked(iterator->second.agent, NavigationReason::SceneReplaced);
			iterator = objectAgents_.erase(iterator);
		}
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
		agentStore_ = std::make_unique<NavigationAgentStore>(settings_.maxActiveAgents);
		commandStore_ = std::make_unique<NavigationCommandStore>(settings_.maxAdmittedRequests);
		pendingGroundBuilds_.clear();
		preparedGroundBuilds_.clear();
		pendingDynamicChanges_.clear();
		pendingDynamicRebuilds_.clear();
		preparedDynamicRebuilds_.clear();
		dynamicSourceStages_.clear();
		pendingRoutes_.clear();
		preparedRoutes_.clear();
		publishedShards_.clear();
		publishedRouteData_.clear();
		requestRoutes_.clear();
		pendingDestinations_.clear();
		objectAgents_.clear();
		invalidations_.clear();
		nextInvalidationCursor_ = 1U;
		contractTestWorkAvailable_.store(false, std::memory_order_release);
		diagnostics_.Append({NavigationDiagnosticSeverity::Information,
			NavigationMeasurementReason::MeasurementUnavailable, nextGeneration, 0U,
			"navigation.owner_lifetime_revoked",
			std::string("Revoked navigation owners: ") + std::string(ToString(reason))});
	}

	Ground::GroundTraversalConnectionUpsertResult NavigationSystem::PublishConnection(
		NavigationOwnerHandle owner,
		const NavigationConnectionComponent& component,
		Ground::GroundTraversalConnectionStore::SupportCertificate certificate) {
		return connectionStore_.Upsert(owner, component, certificate);
	}

	Ground::GroundTraversalConnectionSnapshot NavigationSystem::PublishedConnections() const {
		return connectionStore_.AcquireSnapshot();
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
			for(const auto& [objectId, binding] : objectAgents_) {
				(void)objectId;
				if(!agentStore_) break;
				const auto agent = agentStore_->Read(binding.agent);
				if(!agent.record || !agent.record->activeRoute.IsValid()) continue;
				const NavigationRouteHandle route = agent.record->activeRoute;
				if(!routeStore_ || routeStore_->Status(route) != NavigationHandleStatus::Alive
					|| route.slotIndex >= publishedRouteData_.size()) continue;
				if(!RouteIntersectsLocalizedChange(publishedRouteData_[route.slotIndex], agent.record->profile,
					origin, intent, change)) continue;
				if(invalidations_.size() >= settings_.maxAdmittedRequests) invalidations_.erase(invalidations_.begin());
				invalidations_.push_back({nextInvalidationCursor_++, binding.agent.owner, route,
					NavigationReason::DirtyCoverage, NavigationOutcome::Stale, change.sourceRevision,
					lastProtectedBoundaryGeneration_});
				(void)agentStore_->Update(binding.agent, [route](NavigationAgentRecord& runtimeAgent) {
					if(runtimeAgent.activeRoute == route) runtimeAgent.activeRoute = {};
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
		// ObjectManager is read only at this declared protected boundary. The scan
		// copies pose/profile inputs into NavigationAgentStore; workers never retain
		// a Transform or NavigationAgentComponent pointer.
		SynchronizeSceneObjectAgentsLocked(ObjectManager::instance());
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
			result.reason = NavigationReason::MissingNeighbor;
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
			prepared.sourceIndex = std::move(groundBuild->sourceIndex);
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
			searchedRoute = PreparedRoute{route->handle, route->agent, route->intentGeneration, route->poseGeneration,
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
		// NavigationPublish is itself a protected commit. Refresh the copied poses
		// immediately before applying queued destinations so their route starts use
		// the newest scene-authoritative Transform state.
		SynchronizeSceneObjectAgentsLocked(ObjectManager::instance());
		// Destination changes become visible only at this protected publication
		// boundary. A command either replaces the whole active intent or records a
		// terminal rejection; it never leaves a half-updated agent record behind.
		while(!pendingDestinations_.empty()) {
			PendingDestination pending = std::move(pendingDestinations_.front());
			pendingDestinations_.pop_front();
			if(!commandStore_ || !agentStore_ || !requestStore_) continue;
			const auto agentRead = agentStore_->Read(pending.agent);
			const bool accepted = agentRead.record.has_value()
				&& (pending.expectedIntentGeneration == 0U
					|| agentRead.record->intentGeneration == pending.expectedIntentGeneration);
			if(!accepted) {
				(void)commandStore_->Update(pending.command, [](NavigationCommandRecord& command) {
					command.state = NavigationCommandState::Rejected;
					command.lifecycle = NavigationRequestLifecycle::Terminal;
					command.outcome = NavigationOutcome::Stale;
					command.reason = NavigationReason::SupersededRequest;
				});
				continue;
			}

			const NavigationAgentRecord previous = *agentRead.record;
			Ground::GroundRouteQuery::Request routeRequest{};
			routeRequest.owner = pending.agent.owner;
			routeRequest.start = previous.currentPosition;
			routeRequest.destination = pending.destination;
			routeRequest.profile = previous.profile;
			// Policy bit zero is complete-only; bit one allows the route query to
			// publish its documented bounded partial result when that is possible.
			routeRequest.allowPartial = pending.routePolicy != 0U;
			NavigationRequestRecord requestRecord{};
			requestRecord.createdTick = lastProtectedBoundaryGeneration_;
			requestRecord.intentGeneration = previous.intentGeneration + 1U;
			requestRecord.start = routeRequest.start;
			requestRecord.destination = routeRequest.destination;
			requestRecord.allowPartial = routeRequest.allowPartial;
			const auto request = requestStore_->Create(pending.agent.owner, std::move(requestRecord));
			if(!request.Succeeded()) {
				(void)commandStore_->Update(pending.command, [](NavigationCommandRecord& command) {
					command.state = NavigationCommandState::Rejected;
					command.lifecycle = NavigationRequestLifecycle::Terminal;
					command.outcome = NavigationOutcome::RejectedCapacity;
					command.reason = NavigationReason::OutputLimit;
				});
				continue;
			}
			if(requestRoutes_.size() <= request.handle.slotIndex) requestRoutes_.resize(request.handle.slotIndex + 1U);
			requestRoutes_[request.handle.slotIndex] = {};
			// Destination commands are admitted at the protected boundary, so capture
			// the same complete root/shard generations that were current with the new
			// intent. Worker search must never synthesize a route from endpoints alone.
			pendingRoutes_.push_back({request.handle, pending.agent, previous.intentGeneration + 1U,
				previous.poseGeneration, std::move(routeRequest),
				world_ ? world_->PublishedRoot() : nullptr, publishedShards_, portalGraph_.AcquireSnapshot()});
			(void)agentStore_->Update(pending.agent, [&pending, &request, &previous](NavigationAgentRecord& agent) {
				agent.intentGeneration = previous.intentGeneration + 1U;
				agent.destination = pending.destination;
				agent.routePolicy = pending.routePolicy;
				agent.activeRequest = request.handle;
				agent.activeRoute = {};
			});
			if(previous.activeRequest.IsValid()) (void)requestStore_->Cancel(previous.activeRequest);
			if(previous.activeRoute.IsValid()) {
				if(invalidations_.size() >= settings_.maxAdmittedRequests) invalidations_.erase(invalidations_.begin());
				invalidations_.push_back({nextInvalidationCursor_++, pending.agent.owner, previous.activeRoute,
					NavigationReason::SupersededRequest, NavigationOutcome::Stale, 0U, lastProtectedBoundaryGeneration_});
			}
			(void)commandStore_->Update(pending.command, [&request, &previous](NavigationCommandRecord& command) {
				command.state = NavigationCommandState::Applied;
				command.lifecycle = NavigationRequestLifecycle::Terminal;
				command.outcome = NavigationOutcome::Complete;
				command.reason = NavigationReason::None;
				command.resultingIntentGeneration = previous.intentGeneration + 1U;
				command.resultingRequest = request.handle;
			});
		}
		if(world_ && !preparedGroundBuilds_.empty()) {
			PreparedGroundBuild preparedGroundBuild = std::move(preparedGroundBuilds_.front());
			preparedGroundBuilds_.pop_front();
			if(preparedGroundBuild.accepted) {
				const auto current = world_->PublishedRoot();
				NavigationWorldRoot candidate = world_->CreateCandidate();
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
				prepared.result.outcome = NavigationOutcome::UnavailableCoverage;
				prepared.result.reason = NavigationReason::MissingNeighbor;
			}
			if(prepared.agent.IsValid()) {
				const auto agentRead = agentStore_ ? agentStore_->Read(prepared.agent)
					: NavigationStoreReadResult<NavigationAgentRecord>{};
				const bool sameIntent = agentRead.record.has_value()
					&& agentRead.record->intentGeneration == prepared.intentGeneration
					&& agentRead.record->activeRequest == prepared.handle;
				const bool samePose = sameIntent
					&& agentRead.record->poseGeneration == prepared.poseGeneration;
				if(!sameIntent || !samePose) {
					// The character changed intent or moved after this worker captured its
					// start. A route from the old pose must not snap back into gameplay.
					prepared.result.points.clear();
					prepared.result.outcome = NavigationOutcome::Stale;
					prepared.result.reason = sameIntent ? NavigationReason::InvalidStart
						: NavigationReason::SupersededRequest;
				}
			}
			const auto requestRead = requestStore_->Read(prepared.handle);
			// A cancellation keeps its slot allocated for deterministic reads, so the
			// handle can still be alive while the result is no longer publishable.
			if(!requestRead.record || requestRead.record->lifecycle == NavigationRequestLifecycle::Terminal)
				continue;
			NavigationRouteHandle routeHandle{};
			if(prepared.result.outcome == NavigationOutcome::Complete && routeStore_) {
				NavigationRouteRecord routeRecord{};
				routeRecord.request = prepared.handle;
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
			if(routeHandle.IsValid() && prepared.agent.IsValid() && agentStore_) {
				// Route and agent state publish as one protected operation. A late route
				// cannot replace a newer intent because the generation checks above have
				// already rejected it.
				(void)agentStore_->Update(prepared.agent, [&prepared, routeHandle](NavigationAgentRecord& agent) {
					if(agent.intentGeneration == prepared.intentGeneration
						&& agent.poseGeneration == prepared.poseGeneration
						&& agent.activeRequest == prepared.handle) agent.activeRoute = routeHandle;
				});
			}
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
