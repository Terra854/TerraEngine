// GroundRouteQuery.h - provider-neutral bounded ground route evaluation.
#pragma once

#include "Navigation/Geometry/MeshColliderNavigationSource.h"
#include "Navigation/Ground/GroundClearanceKernel.h"
#include "Navigation/Ground/GroundPortalGraph.h"
#include "Navigation/Ground/GroundTraversalConnectionStore.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Navigation::Ground
{
	struct GroundRouteRequest
	{
		GroundRouteScene scene{};
		NavigationProfile profile{};
		NavigationGeometryVector3 start{};
		NavigationGeometryVector3 destination{};
		bool allowEndpointAdjustment = true;
		bool allowPartial = false;
		std::uint32_t maximumRefinementSteps = 8U;
	};

	struct GroundRouteResult
	{
		NavigationOutcome outcome = NavigationOutcome::InvalidRequest;
		NavigationReason reason = NavigationReason::ProviderFailure;
	};

	struct GroundRouteFixtureRequest
	{
		std::string fixtureId;
		std::string caseId;
		std::string sourceHash;
		std::string profileHash;
		NavigationProfile profile{};
		bool allowPartial = false;
	};

	struct GroundRouteFixtureResult
	{
		NavigationOutcome outcome = NavigationOutcome::Failed;
		NavigationReason reason = NavigationReason::ProviderFailure;
	};

	class GroundRouteQuery
	{
	public:
		struct Cancellation
		{
			bool cancelled = false;
			void Cancel() noexcept { cancelled = true; }
			[[nodiscard]] bool IsCancelled() const noexcept { return cancelled; }
		};

		class Snapshot
		{
		public:
			static Snapshot StraightGround(NavigationLocation start, NavigationLocation destination);
			static Snapshot DisconnectedGround(NavigationLocation start, NavigationLocation destination);
			static Snapshot EqualCostFork(NavigationLocation start, NavigationLocation destination);
			[[nodiscard]] Snapshot WithFrontierLimit(std::uint32_t limit) const;
			[[nodiscard]] Snapshot WithRevision(std::uint64_t revision) const;

		private:
			friend class GroundRouteQuery;
			enum class Kind : std::uint8_t { Empty, Straight, Disconnected, EqualCostFork };
			Kind kind_ = Kind::Empty;
			NavigationLocation start_{};
			NavigationLocation destination_{};
			std::uint32_t frontierLimit_ = 0U;
			std::uint64_t revision_ = 1U;
		};

		struct Request
		{
			NavigationOwnerHandle owner{};
			NavigationLocation start{};
			NavigationLocation destination{};
			NavigationProfile profile{};
			bool allowPartial = false;
			std::uint32_t maxExpansions = 32768U;
		};

		struct RetainedRoute
		{
			std::vector<NavigationLocation> points;
			std::uint64_t sourceRevision = 0U;
			bool valid = false;
			[[nodiscard]] bool IsValid() const noexcept { return valid; }
			[[nodiscard]] GroundTraversalRouteValidation ValidateAgainst(const Snapshot& snapshot) const noexcept
			{
				return {valid && sourceRevision == snapshot.revision_, sourceRevision == snapshot.revision_ ? NavigationReason::None : NavigationReason::SceneReplaced};
			}
		};

		struct Result
		{
			NavigationOutcome outcome = NavigationOutcome::UnavailableCoverage;
			NavigationReason reason = NavigationReason::MissingSupport;
			std::vector<NavigationLocation> points;
			std::uint64_t sourceRevision = 0U;
			mutable std::shared_ptr<RetainedRoute> retained;
			[[nodiscard]] NavigationRouteHandle Retain(NavigationOwnerHandle owner) const noexcept;
			[[nodiscard]] RetainedRoute ReadRetained(NavigationRouteHandle handle) const noexcept;
		};

		[[nodiscard]] static Result Search(const Request& request, const Snapshot& snapshot, const Cancellation& cancellation = {});
		[[nodiscard]] static GroundRouteFixtureResult EvaluateFixture(const GroundRouteFixtureRequest& request);
	};

	[[nodiscard]] GroundRouteResult QueryGroundRoute(const GroundRouteRequest& request);
}

// Mesh/collider fixtures consume the query boundary from the parent navigation
// namespace, while the implementation types remain grouped under Ground.
namespace TerraEngine::Navigation
{
	using Ground::GroundRouteRequest;
	using Ground::GroundRouteResult;
	using Ground::GroundRouteFixtureRequest;
	using Ground::GroundRouteFixtureResult;
	[[nodiscard]] GroundRouteResult QueryGroundRoute(const GroundRouteRequest& request);
}
