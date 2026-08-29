// TerrainStreamingSystem.cpp — explicit terrain streaming scheduler contract.

#include "TerrainStreamingSystem.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <tuple>

namespace TerraEngine::Terrain
{
namespace
{
	bool IsProtected(TerrainDemandSource source) noexcept
	{
		return source == TerrainDemandSource::ActiveEdit
			|| source == TerrainDemandSource::Selection
			|| source == TerrainDemandSource::Play
			|| source == TerrainDemandSource::ExplicitPin;
	}

	std::array<std::size_t, 4u> CountBytes(const std::vector<TerrainResidentArea>& areas)
	{
		std::array<std::size_t, 4u> bytes{};
		for (const TerrainResidentArea& area : areas)
		{
			bytes[0] += area.authoredBytes;
			bytes[1] += area.renderBytes;
			bytes[2] += area.controlBytes;
			bytes[3] += area.collisionBytes;
		}
		return bytes;
	}

	bool ExceedsBudget(const std::array<std::size_t, 4u>& bytes, const TerrainResidencyBudget& budget) noexcept
	{
		return bytes[0] > budget.authoredBytes || bytes[1] > budget.renderBytes
			|| bytes[2] > budget.controlBytes || bytes[3] > budget.collisionBytes;
	}

	bool SameArea(const TerrainResidentArea& resident, const TerrainResidencyDemand& demand) noexcept
	{
		return resident.assetId == demand.assetId && resident.area == demand.area;
	}
}

	TerrainResidencyPlan PrepareTerrainResidencyPlan(
		Generation observedRootGeneration,
		const TerrainResidencySnapshot& prior,
		std::vector<TerrainResidencyDemand> demands,
		TerrainResidencyBudget budget,
		const std::atomic_bool* cancellation,
		std::span<const TerrainAreaCoordinate> forcedRetirements)
	{
		TerrainResidencyPlan plan{};
		plan.observedRootGeneration = observedRootGeneration;
		plan.candidate = prior;
		plan.candidate.rootGeneration = observedRootGeneration;
		plan.candidate.budget = budget;
		if (observedRootGeneration == 0u)
		{
			plan.diagnostic = "Terrain residency planning requires a published root generation.";
			return plan;
		}
		if (cancellation != nullptr && cancellation->load(std::memory_order_acquire))
		{
			plan.cancelled = true;
			plan.diagnostic = "Terrain residency planning was cancelled before it prepared a candidate.";
			return plan;
		}

		// Removed or newly inactive crop coverage cannot remain visible merely
		// because an old view demand pinned it. The candidate drops it now; resource
		// owners release the old snapshot only after protected publication succeeds.
		for(const TerrainAreaCoordinate coordinate : forcedRetirements)
		{
			auto area = std::find_if(plan.candidate.areas.begin(), plan.candidate.areas.end(),
				[coordinate](const TerrainResidentArea& value) { return value.area == coordinate; });
			if(area == plan.candidate.areas.end()) continue;
			plan.evictAreas.push_back(coordinate);
			plan.candidate.areas.erase(area);
			++plan.candidate.evictions;
		}

		// Coalesce identical requests deterministically. A stronger source and the
		// largest byte estimate win so a worker cannot under-budget merged demand.
		std::sort(demands.begin(), demands.end(), [](const auto& left, const auto& right)
		{
			return std::tie(left.area.x, left.area.z, left.rootGeneration, left.priority)
				> std::tie(right.area.x, right.area.z, right.rootGeneration, right.priority);
		});
		std::vector<TerrainResidencyDemand> merged;
		for (const TerrainResidencyDemand& demand : demands)
		{
			if(std::find(forcedRetirements.begin(), forcedRetirements.end(), demand.area) != forcedRetirements.end())
			{
				++plan.candidate.cancelledDemands;
				continue;
			}
			if (demand.assetId == TerraEngine::EMPTY_UUID || demand.rootGeneration != observedRootGeneration)
			{
				++plan.candidate.cancelledDemands;
				continue;
			}
			auto existing = std::find_if(merged.begin(), merged.end(), [&demand](const auto& value)
			{
				return value.assetId == demand.assetId && value.area == demand.area;
			});
			if (existing == merged.end())
			{
				merged.push_back(demand);
				continue;
			}
			++plan.candidate.coalescedDemands;
			existing->priority = std::max(existing->priority, demand.priority);
			existing->authoredBytes = std::max(existing->authoredBytes, demand.authoredBytes);
			existing->renderBytes = std::max(existing->renderBytes, demand.renderBytes);
			existing->controlBytes = std::max(existing->controlBytes, demand.controlBytes);
			existing->collisionBytes = std::max(existing->collisionBytes, demand.collisionBytes);
			existing->regionalRoot = existing->regionalRoot || demand.regionalRoot;
			existing->seamDependency = existing->seamDependency || demand.seamDependency;
			if (IsProtected(demand.source)) existing->source = demand.source;
		}

		for (const TerrainResidencyDemand& demand : merged)
		{
			if (cancellation != nullptr && cancellation->load(std::memory_order_acquire))
			{
				plan.cancelled = true;
				plan.diagnostic = "Terrain residency planning was cancelled while coalescing requests.";
				return plan;
			}
			auto resident = std::find_if(plan.candidate.areas.begin(), plan.candidate.areas.end(), [&demand](const auto& value) { return SameArea(value, demand); });
			if (resident == plan.candidate.areas.end())
			{
				plan.candidate.areas.push_back({ demand.assetId, demand.area, observedRootGeneration, {}, demand.authoredBytes, demand.renderBytes, demand.controlBytes, demand.collisionBytes, demand.priority, IsProtected(demand.source) || demand.seamDependency, demand.regionalRoot });
				plan.loadAreas.push_back(demand.area);
			}
			else
			{
				resident->rootGeneration = observedRootGeneration;
				resident->priority = std::max(resident->priority, demand.priority);
				resident->protectedFromEviction = resident->protectedFromEviction || IsProtected(demand.source) || demand.seamDependency;
				resident->regionalRoot = resident->regionalRoot || demand.regionalRoot;
			}
		}

		// Evict only the least valuable unprotected detail. A regional root remains
		// when it is the last fallback, preventing an arbitrary-detail hole.
		auto bytes = CountBytes(plan.candidate.areas);
		while (ExceedsBudget(bytes, budget))
		{
			auto victim = plan.candidate.areas.end();
			for (auto it = plan.candidate.areas.begin(); it != plan.candidate.areas.end(); ++it)
			{
				if (it->protectedFromEviction) continue;
				const bool onlyRegionalRoot = it->regionalRoot && std::none_of(plan.candidate.areas.begin(), plan.candidate.areas.end(), [&it](const auto& other)
				{
					return &other != &*it && other.assetId == it->assetId && other.regionalRoot;
				});
				if (onlyRegionalRoot) continue;
				if (victim == plan.candidate.areas.end() || std::tie(it->priority, it->area.x, it->area.z) < std::tie(victim->priority, victim->area.x, victim->area.z)) victim = it;
			}
			if (victim == plan.candidate.areas.end())
			{
				plan.candidate.hasCoherentFallbackCoverage = false;
				plan.diagnostic = "Terrain residency budget is exceeded only by protected or sole regional-root areas.";
				break;
			}
			plan.evictAreas.push_back(victim->area);
			plan.candidate.areas.erase(victim);
			++plan.candidate.evictions;
			bytes = CountBytes(plan.candidate.areas);
		}
		plan.candidate.residentBytes = bytes;
		for (std::size_t index = 0; index < bytes.size(); ++index)
			plan.candidate.highWaterBytes[index] = std::max(prior.highWaterBytes[index], bytes[index]);
		plan.diagnostic = plan.diagnostic.empty() ? "Prepared a bounded immutable terrain residency candidate." : plan.diagnostic;
		return plan;
	}

	bool PublishTerrainResidencyPlan(TerrainResidencySnapshot& inOutPublished, const TerrainResidencyPlan& plan, Generation currentRootGeneration)
	{
		// Publication is all-or-nothing: a stale root, cancellation, or incoherent
		// fallback leaves the complete old snapshot visible to every consumer.
		if (plan.cancelled || plan.observedRootGeneration == 0u || plan.observedRootGeneration != currentRootGeneration || !plan.candidate.hasCoherentFallbackCoverage) return false;
		inOutPublished = plan.candidate;
		inOutPublished.publicationGeneration = std::max(inOutPublished.publicationGeneration + 1u, currentRootGeneration);
		return true;
	}

	std::optional<std::vector<TerrainResidencyDemand>> BuildTerrainCoarseCoverageDemands(
		TerraEngine::UUID assetId, Generation rootGeneration,
		TerrainSampleBounds authoredBounds, std::size_t maximumAreas,
		std::size_t coarseRenderBytesPerArea)
	{
		if(assetId == TerraEngine::EMPTY_UUID || rootGeneration == 0u || maximumAreas == 0u) return std::nullopt;
		const auto coordinates = EnumerateTerrainAreas(authoredBounds, maximumAreas);
		if(!coordinates) return std::nullopt;
		std::vector<TerrainResidencyDemand> demands;
		demands.reserve(coordinates->size());
		for(const auto coordinate : *coordinates)
		{
			TerrainResidencyDemand demand{};
			demand.assetId = assetId;
			demand.area = coordinate;
			demand.rootGeneration = rootGeneration;
			demand.source = TerrainDemandSource::SceneView;
			demand.priority = 1u;
			demand.renderBytes = coarseRenderBytesPerArea;
			demand.regionalRoot = true;
			demands.push_back(demand);
		}
		return demands;
	}

	TerrainStreamingSystem& TerrainStreamingSystem::Instance()
	{
		static TerrainStreamingSystem instance;
		return instance;
	}

	void TerrainStreamingSystem::Initialize()
	{
		initialized.store(true, std::memory_order_release);
	}

	void TerrainStreamingSystem::Update(float)
	{
		// Converted systems run their declared workloads. Keep Update empty so a
		// compatibility scheduler path cannot publish terrain state implicitly.
	}

	void TerrainStreamingSystem::Shutdown()
	{
		initialized.store(false, std::memory_order_release);
		std::scoped_lock lock(residencyMutex);
		pendingDemands.clear();
		pendingForcedRetirements.clear();
		forcedRetirementGeneration = 0u;
		preparedResidency.reset();
		publishedResidency = {};
	}

	std::string TerrainStreamingSystem::SystemName()
	{
		return "TerrainStreamingSystem";
	}

	TerraEngine::Scheduling::SystemDeclaration TerrainStreamingSystem::SchedulerDeclaration()
	{
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration;
		declaration.name = SystemName();
		// Streaming is render-facing in the engine's canonical phase graph. Its
		// protected publication must complete before derived cooks and render capture.
		declaration.phase = "Render";
		declaration.before = { "TerrainDerivedSystem", "VirtualGeometryStreamingSystem", "RenderSystem" };
		declaration.reads = { kResourceTerrainAssetRoots, kResourceTerrainStreamingDemands, kResourceTerrainPublishedResidency };
		declaration.writes = { kResourceTerrainPreparedResidency, kResourceTerrainPublishedResidency, kResourceTerrainDiagnostics };
		declaration.mainThread = false;
		declaration.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		declaration.boundaryClassification = HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Captures immutable terrain roots and demand packets, then prepares bounded load and eviction candidates without touching live residency.";
		declaration.protectedCommit = "Validates candidate root generations and atomically publishes a complete terrain residency snapshot; rejected candidates retain the previous snapshot.";
		declaration.protectedReason = declaration.protectedCommit;
		declaration.profilingLabel = "Terrain Streaming";
		declaration.usesDefaultMetadata = false;
		return declaration;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> TerrainStreamingSystem::DeclareFrameWorkloads(float)
	{
		using namespace TerraEngine::Scheduling;
		const bool active = initialized.load(std::memory_order_acquire);
		FrameWorkloadDeclaration prepare = MakeWorkerPreparePhase(
			"TerrainStreamingPrepare", SystemName(),
			{ kResourceTerrainAssetRoots, kResourceTerrainStreamingDemands, kResourceTerrainPublishedResidency },
			{ kResourceTerrainPreparedResidency }, "TerrainStreaming.Prepare");
		prepare.stableDescriptorId = "TerrainStreamingSystem/Render/TerrainStreamingPrepare";
		prepare.phase = "Render";
		prepare.activationPredicateKey = "terrain-streaming-lifecycle";
		prepare.boundaryClassification = HardBoundaryClassification::WorkerSafe;
		prepare.safeSubwork = "Value-only demand coalescing and bounded terrain area load/eviction planning.";
		prepare.work = [this](TerraEngine::Scheduling::JobGraphContext&)
		{
			std::vector<TerrainResidencyDemand> demands;
			TerrainResidencySnapshot prior;
			TerrainResidencyBudget capturedBudget;
			std::vector<TerrainAreaCoordinate> forcedRetirements;
			{
				std::scoped_lock lock(residencyMutex);
				demands.swap(pendingDemands);
				prior = publishedResidency;
				capturedBudget = budget;
				forcedRetirements = pendingForcedRetirements;
			}
			Generation observedGeneration = prior.rootGeneration;
			for(const TerrainResidencyDemand& demand : demands)
				observedGeneration = std::max(observedGeneration, demand.rootGeneration);
			TerrainResidencyPlan plan = PrepareTerrainResidencyPlan(
				observedGeneration, prior, std::move(demands), capturedBudget, nullptr, forcedRetirements);
			std::scoped_lock lock(residencyMutex);
			preparedResidency = std::move(plan);
		};

		FrameWorkloadDeclaration publish = MakeProtectedCommitPhase(
			"TerrainStreamingPublish", SystemName(),
			{ kResourceTerrainPreparedResidency, kResourceTerrainPublishedResidency },
			{ kResourceTerrainPublishedResidency, kResourceTerrainDiagnostics },
			"Residency publication swaps immutable terrain snapshots only after generation checks.",
			"TerrainStreaming.Publish");
		publish.stableDescriptorId = "TerrainStreamingSystem/Render/TerrainStreamingPublish";
		publish.phase = "Render";
		publish.activationPredicateKey = prepare.activationPredicateKey;
		publish.explicitDependencies = { prepare.stableDescriptorId };
		publish.boundaryClassification = HardBoundaryClassification::Protected;
		publish.hardBoundary = true;
		publish.protectedCommit = publish.protectedReason;
		publish.sameFrameVisibilityRequired = true;
		publish.work = [this](TerraEngine::Scheduling::JobGraphContext&)
		{
			std::scoped_lock lock(residencyMutex);
			if(!preparedResidency) return;
			Generation currentGeneration = std::max(
				std::max(publishedResidency.rootGeneration, preparedResidency->observedRootGeneration),
				forcedRetirementGeneration);
			for(const TerrainResidencyDemand& demand : pendingDemands)
				currentGeneration = std::max(currentGeneration, demand.rootGeneration);
			if(PublishTerrainResidencyPlan(publishedResidency, *preparedResidency, currentGeneration)
				&& currentGeneration >= forcedRetirementGeneration)
			{
				pendingForcedRetirements.clear();
				forcedRetirementGeneration = 0u;
			}
			preparedResidency.reset();
		};

		for(FrameWorkloadDeclaration* workload : { &prepare, &publish })
		{
			workload->active = active;
			workload->ready = active;
			workload->readinessReason = active ? std::string{} : "terrain streaming system is not initialized";
		}
		return { std::move(prepare), std::move(publish) };
	}

	void TerrainStreamingSystem::SubmitDemands(std::vector<TerrainResidencyDemand> demands)
	{
		std::scoped_lock lock(residencyMutex);
		pendingDemands.insert(pendingDemands.end(), std::make_move_iterator(demands.begin()), std::make_move_iterator(demands.end()));
	}

	bool TerrainStreamingSystem::SubmitCoarseCoverage(TerraEngine::UUID assetId,
		Generation rootGeneration, TerrainSampleBounds authoredBounds,
		std::size_t maximumAreas, std::size_t coarseRenderBytesPerArea)
	{
		auto demands = BuildTerrainCoarseCoverageDemands(assetId, rootGeneration,
			authoredBounds, maximumAreas, coarseRenderBytesPerArea);
		if(!demands) return false;
		SubmitDemands(std::move(*demands));
		return true;
	}

	void TerrainStreamingSystem::SubmitForcedRetirements(Generation rootGeneration,
		std::vector<TerrainAreaCoordinate> areas)
	{
		std::sort(areas.begin(), areas.end());
		areas.erase(std::unique(areas.begin(), areas.end()), areas.end());
		std::scoped_lock lock(residencyMutex);
		forcedRetirementGeneration = std::max(forcedRetirementGeneration, rootGeneration);
		pendingForcedRetirements.insert(pendingForcedRetirements.end(), areas.begin(), areas.end());
		std::sort(pendingForcedRetirements.begin(), pendingForcedRetirements.end());
		pendingForcedRetirements.erase(std::unique(pendingForcedRetirements.begin(), pendingForcedRetirements.end()), pendingForcedRetirements.end());
	}

	void TerrainStreamingSystem::SetBudget(TerrainResidencyBudget nextBudget)
	{
		std::scoped_lock lock(residencyMutex);
		budget = nextBudget;
	}

	TerrainResidencySnapshot TerrainStreamingSystem::ResidencySnapshot() const
	{
		std::scoped_lock lock(residencyMutex);
		return publishedResidency;
	}

	std::optional<TerrainResidencyPlan> TerrainStreamingSystem::PrepareSnapshot(Generation observedRootGeneration, const std::atomic_bool* cancellation)
	{
		std::scoped_lock lock(residencyMutex);
		if (!initialized.load(std::memory_order_acquire)) return std::nullopt;
		TerrainResidencyPlan plan = PrepareTerrainResidencyPlan(observedRootGeneration, publishedResidency,
			std::move(pendingDemands), budget, cancellation, pendingForcedRetirements);
		pendingDemands.clear();
		return plan;
	}

	bool TerrainStreamingSystem::PublishPreparedSnapshot(const TerrainResidencyPlan& plan, Generation currentRootGeneration)
	{
		std::scoped_lock lock(residencyMutex);
		const bool published = PublishTerrainResidencyPlan(publishedResidency, plan, currentRootGeneration);
		if(published && currentRootGeneration >= forcedRetirementGeneration)
		{
			pendingForcedRetirements.clear();
			forcedRetirementGeneration = 0u;
		}
		return published;
	}
}
