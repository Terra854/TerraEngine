// WorldStreamingSystem.cpp - worker planning and protected residency swap.

#include "Scene/WorldStreamingSystem.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Core/Scheduling/WorkerSafety.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <unordered_map>

namespace TerraEngine::Scene
{
	namespace
	{
		[[nodiscard]] int PriorityValue(ObserverInterestPriority priority) { return static_cast<int>(priority); }
	}

	RegionInterestPlan BuildDeterministicRegionInterestPlan(const std::vector<ObserverInterest>& interests, const WorldRegionDirectory& directory, const PublishedWorldRegionResidency& current, WorldStreamingBudget budget, std::uint64_t generation)
	{
		return BuildDeterministicRegionInterestPlan(interests, directory, current, budget, generation, nullptr);
	}
	RegionInterestPlan BuildDeterministicRegionInterestPlan(const std::vector<ObserverInterest>& interests, const WorldRegionDirectory& directory, const PublishedWorldRegionResidency& current, WorldStreamingBudget budget, std::uint64_t generation, const RegionInterestPlanningObserver* observer)
	{
		RegionInterestPlan plan{};
		plan.generation = generation;
		if (budget.maximumResidentRegions == 0U) return plan;
		struct Candidate { WorldRegionKey key{}; int priority = 0; };
		std::unordered_map<WorldRegionKey, int, WorldRegionKeyHash> candidatePriorities;
		std::optional<std::vector<WorldRegionDirectoryEntry>> sparseEntries;
		const auto increment = [](std::uint64_t& value) {
			if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
		};
		const auto reject = [&](std::size_t index, RegionInterestPlanningRejectionReason reason) {
			increment(plan.diagnostics.rejectedCount);
			if (plan.diagnostics.recentRejections.size() == 64u)
				plan.diagnostics.recentRejections.erase(plan.diagnostics.recentRejections.begin());
			plan.diagnostics.recentRejections.push_back({index, reason});
		};
		const auto addCandidate = [&](const WorldRegionKey& key, int priority) {
			auto [entry, inserted] = candidatePriorities.try_emplace(key, priority);
			if (!inserted) entry->second = std::max(entry->second, priority);
		};
		// Unsigned subtraction preserves the full [0,UINT64_MAX] distance even
		// when signed keys straddle INT64_MIN/MAX; no signed difference overflows.
		const auto distance = [](std::int64_t left, std::int64_t right) -> std::uint64_t {
			return left >= right ? static_cast<std::uint64_t>(left) - static_cast<std::uint64_t>(right)
				: static_cast<std::uint64_t>(right) - static_cast<std::uint64_t>(left);
		};
		const auto checkedAdd = [](std::int64_t value, std::int64_t delta, std::int64_t& result) {
			if ((delta > 0 && value > (std::numeric_limits<std::int64_t>::max)() - delta)
				|| (delta < 0 && value < (std::numeric_limits<std::int64_t>::min)() - delta)) return false;
			result = value + delta;
			return true;
		};
		for (std::size_t index = 0u; index < interests.size(); ++index)
		{
			const ObserverInterest& interest = interests[index];
			if (interest.observerId.empty()) { reject(index, RegionInterestPlanningRejectionReason::InvalidObserver); continue; }
			if (!std::isfinite(interest.radiusMetres) || interest.radiusMetres < 0.0)
			{ reject(index, RegionInterestPlanningRejectionReason::InvalidRadius); continue; }
			WorldRegionKey center{};
			if (WorldPositionToRegionKey(interest.position, center) != WorldCoordinateError::None)
			{ reject(index, RegionInterestPlanningRejectionReason::InvalidPosition); continue; }
			const double observedRadius = std::ceil(interest.radiusMetres / kWorldRegionEdgeMetres);
			// INT64_MAX rounds up to2^63 as double; the exclusive exact bound
			// rejects before narrowing and also leaves -radius representable.
			if (!std::isfinite(observedRadius) || observedRadius < 0.0 || observedRadius >= 0x1p63)
			{ reject(index, RegionInterestPlanningRejectionReason::RadiusUnrepresentable); continue; }
			if (observer && observer->beforeRadiusNarrowing) observer->beforeRadiusNarrowing(observer->context, observedRadius);
			const auto radius = static_cast<std::int64_t>(observedRadius);
			const auto unsignedRadius = static_cast<std::uint64_t>(radius);
			const std::uint64_t side = 2u * unsignedRadius + 1u;
			const auto directorySize = directory.Size();
			if (directorySize == 0u) continue;
			// Division checks choose the cheaper strategy without ever computing
			// an overflowing cubic work estimate. Radius zero remains a single
			// lookup even when the directory contains millions of unrelated keys.
			const bool useCube = side <= directorySize && side <= directorySize / side
				&& side <= (directorySize / side) / side;
			if (useCube)
			{
				increment(plan.diagnostics.cubeInterests);
				for (std::uint64_t z = 0u; z < side; ++z)
					for (std::uint64_t y = 0u; y < side; ++y)
						for (std::uint64_t x = 0u; x < side; ++x)
						{
							WorldRegionKey key{};
							if (!checkedAdd(center.x, static_cast<std::int64_t>(x) - radius, key.x)
								|| !checkedAdd(center.y, static_cast<std::int64_t>(y) - radius, key.y)
								|| !checkedAdd(center.z, static_cast<std::int64_t>(z) - radius, key.z)) continue;
							increment(plan.diagnostics.directoryExaminations);
							if (observer && observer->beforeDirectoryLookup) observer->beforeDirectoryLookup(observer->context, key);
							if (directory.Find(key)) addCandidate(key, PriorityValue(interest.priority));
						}
			}
			else
			{
				increment(plan.diagnostics.sparseInterests);
				// Copy once on demand; normal small-radius planning pays no full
				// directory copy. Candidate ownership is bounded to unique keys.
				if (!sparseEntries) sparseEntries = directory.Entries();
				for (const auto& entry : *sparseEntries)
				{
					increment(plan.diagnostics.directoryExaminations);
					if (observer && observer->beforeDirectoryLookup) observer->beforeDirectoryLookup(observer->context, entry.key);
					if (distance(center.x, entry.key.x) <= unsignedRadius
						&& distance(center.y, entry.key.y) <= unsignedRadius
						&& distance(center.z, entry.key.z) <= unsignedRadius)
						addCandidate(entry.key, PriorityValue(interest.priority));
				}
			}
		}
		std::vector<Candidate> candidates;
		candidates.reserve(candidatePriorities.size());
		for (const auto& [key, priority] : candidatePriorities) candidates.push_back({key, priority});
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
	void WorldStreamingSystem::Initialize() { std::scoped_lock lock(mutex); observerInterests.clear(); queuedPublications.clear(); interestPlan = {}; residency = {}; publicationDiagnostics = {}; initialized = true; }
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
		declaration.writes = { kResourcePreparedWorldRegionResidency };
		declaration.mainThread = false;
		// The scheduled item only prepares typed values. Live publication is a
		// separate coordinator operation at the existing MainEngine Frame End edge.
		declaration.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		declaration.boundaryClassification = HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Reads system-owned observer interests and residency under the system lock, uses locked directory lookups or snapshots, and prepares bounded exact-key interest/retirement candidates and copied diagnostics without publishing live residency.";
		declaration.protectedCommit = "At the existing coordinator Frame End boundary, expires directory leases, validates prepared identities/revisions and retirement leases, writes directory residency transitions and ResourceManager region asset needs, then swaps the complete PublishedWorldRegionResidency snapshot. Rejected batches preserve the prior residency; invalid queued work retires for later progress.";
		declaration.protectedReason = "Directory transitions, resource residency ownership and published snapshot visibility remain protected Frame End writes; workers only prepare candidate values.";
		declaration.usesDefaultMetadata = false;
		declaration.profilingLabel = "World Region Streaming";
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
	WorldStreamingPublicationDiagnostics WorldStreamingSystem::GetPublicationDiagnostics() const { std::scoped_lock lock(mutex); return publicationDiagnostics; }
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
		// Reject this boundary atomically, but retire permanently invalid prepared
		// output so it cannot hold a queue slot or block later retirement forever.
		// Valid entries keep their order and wait for the next protected boundary.
		const auto rejectedBegin = std::remove_if(queuedPublications.begin(), queuedPublications.end(), [&](const PreparedRegionPublication& publication) {
			const std::optional<WorldRegionDirectoryEntry> entry = directory.Find(publication.key);
			if (publication.cancelled || !publication.snapshot || !entry || entry->revision != publication.expectedDirectoryRevision || entry->state != WorldRegionState::Prepared
				|| publication.snapshot->key != publication.key || publication.snapshot->directoryRevision != entry->revision || publication.snapshot->contentHash != entry->contentHash) {
				// Report the rejected observation without failing a newer directory
				// revision or publishing any part of this rejected residency batch.
				std::string reason = publication.cancelled ? "cancelled prepared region"
					: !publication.snapshot ? "missing prepared region snapshot"
					: !entry ? "missing region directory entry"
					: entry->revision != publication.expectedDirectoryRevision ? "stale region directory revision"
					: entry->state != WorldRegionState::Prepared ? "region is not prepared"
					: "prepared region metadata mismatch";
				if (publicationDiagnostics.rejectedCount != std::numeric_limits<std::uint64_t>::max()) ++publicationDiagnostics.rejectedCount;
				constexpr std::size_t maximumReceipts = 64U;
				if (publicationDiagnostics.recentRejections.size() == maximumReceipts) publicationDiagnostics.recentRejections.erase(publicationDiagnostics.recentRejections.begin());
				// Retire only the rejected revision's preparation. A newer directory
				// owner must remain untouched, and Failed permits a fresh request.
				if (entry && entry->revision == publication.expectedDirectoryRevision
					&& entry->state == WorldRegionState::Prepared)
					(void)directory.Transition(publication.key, WorldRegionState::Failed, reason);
				publicationDiagnostics.recentRejections.push_back({publication.key, publication.expectedDirectoryRevision, entry ? entry->revision : 0U, std::move(reason)});
				return true;
			}
			return false;
		});
		if (rejectedBegin != queuedPublications.end()) {
			queuedPublications.erase(rejectedBegin, queuedPublications.end());
			return false;
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
