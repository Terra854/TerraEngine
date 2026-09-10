// WorldStreamingSystem.cpp - worker planning and protected residency swap.

#include "Scene/WorldStreamingSystem.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Core/Scheduling/WorkerSafety.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>

namespace TerraEngine::Scene
{
	namespace
	{
		[[nodiscard]] int PriorityValue(ObserverInterestPriority priority) { return static_cast<int>(priority); }
	}

	RegionInterestPlan BuildDeterministicRegionInterestPlan(const std::vector<ObserverInterest>& interests, const WorldRegionDirectory& directory, const PublishedWorldRegionResidency& current, WorldStreamingBudget budget, std::uint64_t generation)
	{
		RegionInterestPlan plan{};
		plan.generation = generation;
		if (budget.maximumResidentRegions == 0U) return plan;
		struct Candidate { WorldRegionKey key{}; int priority = 0; };
		std::vector<Candidate> candidates;
		for (const ObserverInterest& interest : interests)
		{
			if (interest.observerId.empty() || !std::isfinite(interest.radiusMetres) || interest.radiusMetres < 0.0) continue;
			WorldRegionKey center{};
			if (WorldPositionToRegionKey(interest.position, center) != WorldCoordinateError::None) continue;
			const std::int64_t radius = static_cast<std::int64_t>(std::ceil(interest.radiusMetres / kWorldRegionEdgeMetres));
			for (std::int64_t z = -radius; z <= radius; ++z) for (std::int64_t y = -radius; y <= radius; ++y) for (std::int64_t x = -radius; x <= radius; ++x)
			{
				WorldRegionKey key{ center.x + x, center.y + y, center.z + z };
				if (directory.Find(key)) candidates.push_back({ key, PriorityValue(interest.priority) });
			}
		}
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) { return left.priority != right.priority ? left.priority > right.priority : left.key < right.key; });
		std::unordered_set<WorldRegionKey, WorldRegionKeyHash> selected;
		for (const Candidate& candidate : candidates)
		{
			if (selected.insert(candidate.key).second) plan.requested.push_back(candidate.key);
			if (plan.requested.size() >= budget.maximumResidentRegions) break;
		}
		// Priority decides which keys fit the budget; the published result then
		// uses key order so worker completion order cannot affect a snapshot.
		std::sort(plan.requested.begin(), plan.requested.end());
		for (const auto& [key, snapshot] : current.residentRegions)
		{
			(void)snapshot;
			if (!std::binary_search(plan.requested.begin(), plan.requested.end(), key)) plan.retirements.push_back(key);
		}
		std::sort(plan.retirements.begin(), plan.retirements.end());
		return plan;
	}

	WorldStreamingSystem& WorldStreamingSystem::Instance() { static WorldStreamingSystem instance; return instance; }
	void WorldStreamingSystem::Initialize() { std::scoped_lock lock(mutex); observerInterests.clear(); queuedPublications.clear(); interestPlan = {}; residency = {}; initialized = true; }
	void WorldStreamingSystem::Update(float) {
		// Direct hosts use the same bounded planning functions as scheduler jobs.
		// Production frames normally invoke these through DeclareFrameWorkloads.
		PrepareInterestPlan();
		PrepareRetirementPlan();
	}
	void WorldStreamingSystem::Shutdown() { std::scoped_lock lock(mutex); initialized = false; observerInterests.clear(); queuedPublications.clear(); residency = {}; }
	std::string WorldStreamingSystem::SystemName() { return "WorldStreamingSystem"; }
	TerraEngine::Scheduling::SystemDeclaration WorldStreamingSystem::SchedulerDeclaration()
	{
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration{}; declaration.name = SystemName(); declaration.phase = "Streaming";
		declaration.reads = { kResourceWorldRegionDirectory, kResourceObserverInterests, kResourcePublishedWorldRegionResidency };
		declaration.writes = { kResourcePreparedWorldRegionResidency }; declaration.mainThread = false; declaration.usesDefaultMetadata = false; declaration.profilingLabel = "World Region Streaming";
		return declaration;
	}
	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> WorldStreamingSystem::DeclareFrameWorkloads(float)
	{
		using namespace TerraEngine::Scheduling;
		FrameWorkloadDeclaration workload = MakeWorkerPreparePhase("WorldRegionInterestPrepare", SystemName(), { kResourceWorldRegionDirectory, kResourceObserverInterests, kResourcePublishedWorldRegionResidency }, { kResourcePreparedWorldRegionResidency }, "World Region Interest Prepare");
		workload.work = [this](JobGraphContext&) { PrepareInterestPlan(); PrepareRetirementPlan(); };
		return { std::move(workload) };
	}
	void WorldStreamingSystem::SetBudget(WorldStreamingBudget nextBudget) { std::scoped_lock lock(mutex); budget = nextBudget; }
	void WorldStreamingSystem::SubmitObserverInterest(ObserverInterest interest) {
		std::scoped_lock lock(mutex);
		if(observerInterests.size() < 4096U) observerInterests.push_back(std::move(interest));
	}
	void WorldStreamingSystem::SubmitPreparedRegion(PreparedRegionPublication publication) {
		std::scoped_lock lock(mutex);
		if(queuedPublications.size() < budget.maximumPreparedRegions) queuedPublications.push_back(std::move(publication));
	}
	RegionInterestPlan WorldStreamingSystem::CurrentInterestPlan() const { std::scoped_lock lock(mutex); return interestPlan; }
	PublishedWorldRegionResidency WorldStreamingSystem::CurrentResidency() const { std::scoped_lock lock(mutex); return residency; }
	void WorldStreamingSystem::PrepareInterestPlan()
	{
		std::scoped_lock lock(mutex);
		if (!initialized) return;
		interestPlan = BuildDeterministicRegionInterestPlan(observerInterests, directory, residency, budget, interestPlan.generation + 1U);
		observerInterests.clear();
	}
	void WorldStreamingSystem::PrepareRetirementPlan() { /* retirement candidates are derived with the same immutable interest plan. */ }
	bool WorldStreamingSystem::PublishAtFrameEnd(ResourceManager& resources, std::uint64_t frameId)
	{
		if (TerraEngine::Scheduling::IsWorkerSafetyScopeActive()) return false;
		const auto now = std::chrono::steady_clock::now().time_since_epoch();
		const std::uint64_t nowNanoseconds = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
		(void)directory.ExpireLeases(frameId, nowNanoseconds, 4096U);
		std::scoped_lock lock(mutex);
		if (!initialized) return false;
		if (queuedPublications.empty() && interestPlan.retirements.empty()) return false;
		PublishedWorldRegionResidency next = residency;
		next.invalidatedRegions.clear();
		for (const PreparedRegionPublication& publication : queuedPublications)
		{
			const std::optional<WorldRegionDirectoryEntry> entry = directory.Find(publication.key);
			if (publication.cancelled || !publication.snapshot || !entry || entry->revision != publication.expectedDirectoryRevision || entry->state != WorldRegionState::Prepared
				|| publication.snapshot->key != publication.key || publication.snapshot->directoryRevision != entry->revision || publication.snapshot->contentHash != entry->contentHash) return false;
		}
		for (const WorldRegionKey& key : interestPlan.retirements)
		{
			const auto entry = directory.Find(key);
			if (entry && (!entry->leases.empty() || entry->state != WorldRegionState::Resident)) return false;
		}
		for (const WorldRegionKey& key : interestPlan.retirements)
		{
			if (const auto entry = directory.Find(key); entry && entry->leases.empty()) {
				next.invalidatedRegions.push_back(key);
				next.residentRegions.erase(key);
			}
		}
		std::sort(next.invalidatedRegions.begin(), next.invalidatedRegions.end());
		for (const PreparedRegionPublication& publication : queuedPublications)
		{
			next.residentRegions[publication.key] = publication.snapshot;
		}
		next.generation = residency.generation + 1U;
		next.residentObjectCount = 0U; next.coarseProxies.clear();
		for (const auto& [key, snapshot] : next.residentRegions) { (void)key; next.residentObjectCount += static_cast<std::uint32_t>(snapshot->objects.size()); next.coarseProxies.insert(next.coarseProxies.end(), snapshot->coarseProxies.begin(), snapshot->coarseProxies.end()); }
		std::sort(next.coarseProxies.begin(), next.coarseProxies.end(), [](const auto& left, const auto& right) { return TerraEngine::ToHexString(left.objectId) < TerraEngine::ToHexString(right.objectId); });
		std::vector<TerraEngine::Resource::SceneRegionAssetNeed> assetNeeds;
		for (const PreparedRegionPublication& publication : queuedPublications)
		{
			for (const WorldRegionObjectRecord& object : publication.snapshot->objects) for (const TerraEngine::UUID& assetId : object.assetIds)
			{
				assetNeeds.push_back({ assetId, TerraEngine::Resource::StreamableAssetType::Mesh, object.objectId,
					std::to_string(publication.key.x) + ":" + std::to_string(publication.key.y) + ":" + std::to_string(publication.key.z), 0 });
			}
			(void)directory.Transition(publication.key, WorldRegionState::Resident);
		}
		for (const WorldRegionKey& key : interestPlan.retirements)
		{
			if (directory.Find(key))
			{
				(void)directory.Transition(key, WorldRegionState::EvictionPending);
				(void)directory.Transition(key, WorldRegionState::Unloaded);
			}
		}
		resources.PublishSceneRegionAssetNeeds(assetNeeds, frameId);
		residency = std::move(next);
		queuedPublications.clear();
		interestPlan.retirements.clear();
		return true;
	}
}
