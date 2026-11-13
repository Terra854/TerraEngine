// WorldStreamingSystem.h - task-prepared sparse region residency publication.

#pragma once

#include "Core/SystemBase.h"
#include "Scene/WorldRegion.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class ResourceManager;

namespace TerraEngine::Scene
{
	struct WorldStreamingReviewTestAccess;
	struct WorldRegionPublicationRejection
	{
		WorldRegionKey key{};
		std::uint64_t observedDirectoryRevision = 0;
		std::uint64_t currentDirectoryRevision = 0;
		std::string reason;
	};
	struct WorldStreamingPublicationDiagnostics
	{
		std::uint64_t rejectedCount = 0;
		std::vector<WorldRegionPublicationRejection> recentRejections;
	};
	enum class ObserverInterestKind : std::uint8_t { Render, Physics, Editor, Automation, SelectionPin, ExplicitPreload };
	enum class ObserverInterestPriority : std::uint8_t { Background, Normal, Important, Critical };

	struct ObserverInterest
	{
		std::string observerId;
		ObserverInterestKind kind = ObserverInterestKind::Render;
		WorldPosition position{};
		glm::dvec3 velocityMetres{};
		double radiusMetres = 0.0;
		ObserverInterestPriority priority = ObserverInterestPriority::Normal;
		std::uint64_t leaseGeneration = 0;
	};

	struct WorldStreamingBudget { std::uint32_t maximumResidentRegions = 64; std::uint32_t maximumPreparedRegions = 16; };
	// Synchronous caller-owned observation; never retained in the system.
	struct RegionInterestPlanningObserver
	{
		void* context = nullptr;
		void (*beforeRadiusNarrowing)(void*, double radiusRegions) = nullptr;
		void (*beforeDirectoryLookup)(void*, const WorldRegionKey&) = nullptr;
	};
	// Value-only diagnostics are copied with the prepared plan. Recent entries
	// are capped at64 and reference the caller's interest ordinal, not a retained
	// arbitrary-length observer string or callback.
	enum class RegionInterestPlanningRejectionReason : std::uint8_t
	{
		InvalidObserver, InvalidRadius, InvalidPosition, RadiusUnrepresentable
	};
	struct RegionInterestPlanningRejection
	{
		std::size_t interestIndex = 0u;
		RegionInterestPlanningRejectionReason reason = RegionInterestPlanningRejectionReason::InvalidRadius;
	};
	struct RegionInterestPlanningDiagnostics
	{
		std::uint64_t directoryExaminations = 0u;
		std::uint64_t cubeInterests = 0u;
		std::uint64_t sparseInterests = 0u;
		std::uint64_t rejectedCount = 0u;
		std::vector<RegionInterestPlanningRejection> recentRejections;
	};
	struct RegionInterestPlan
	{
		std::uint64_t generation = 0;
		std::vector<WorldRegionKey> requested;
		std::vector<WorldRegionKey> retirements;
		RegionInterestPlanningDiagnostics diagnostics;
	};
	struct PreparedRegionPublication
	{
		WorldRegionKey key{};
		std::uint64_t expectedDirectoryRevision = 0;
		std::uint64_t sourceRequestGeneration = 0;
		bool cancelled = false;
		std::string rejectionReason;
		std::shared_ptr<const WorldRegionSnapshot> snapshot;
	};

	class TERRAENGINE_API WorldStreamingSystem final : public SystemsBase
	{
	public:
		static WorldStreamingSystem& Instance();
		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

		[[nodiscard]] WorldRegionDirectory& Directory() { return directory; }
		void SetBudget(WorldStreamingBudget nextBudget);
		void SubmitObserverInterest(ObserverInterest interest);
		void SubmitPreparedRegion(PreparedRegionPublication publication);
		[[nodiscard]] RegionInterestPlan CurrentInterestPlan() const;
		[[nodiscard]] PublishedWorldRegionResidency CurrentResidency() const;
		// Copied, bounded rejection evidence never mutates a newer directory entry.
		[[nodiscard]] WorldStreamingPublicationDiagnostics GetPublicationDiagnostics() const;
		// The caller invokes this only at the declared Frame End boundary. It
		// either installs every validated prepared region as one new generation or
		// leaves the last immutable residency snapshot unchanged.
		[[nodiscard]] bool PublishAtFrameEnd(ResourceManager& resources, std::uint64_t frameId);

	private:
		// Test definitions live outside the engine; the class layout never depends
		// on a test-only preprocessor definition across the shared-library boundary.
		friend struct WorldStreamingReviewTestAccess;
		WorldStreamingSystem() = default;
		void PrepareInterestPlan();
		void PrepareRetirementPlan();

		mutable std::mutex mutex;
		WorldRegionDirectory directory;
		WorldStreamingBudget budget{};
		std::vector<ObserverInterest> observerInterests;
		std::vector<PreparedRegionPublication> queuedPublications;
		RegionInterestPlan interestPlan{};
		PublishedWorldRegionResidency residency{};
		WorldStreamingPublicationDiagnostics publicationDiagnostics{};
		bool initialized = false;
	};

	[[nodiscard]] TERRAENGINE_API RegionInterestPlan BuildDeterministicRegionInterestPlan(
		const std::vector<ObserverInterest>& interests,
		const WorldRegionDirectory& directory,
		const PublishedWorldRegionResidency& current,
		WorldStreamingBudget budget,
		std::uint64_t generation);
	[[nodiscard]] TERRAENGINE_API RegionInterestPlan BuildDeterministicRegionInterestPlan(
		const std::vector<ObserverInterest>& interests, const WorldRegionDirectory& directory,
		const PublishedWorldRegionResidency& current, WorldStreamingBudget budget,
		std::uint64_t generation, const RegionInterestPlanningObserver* observer);
}
