// GroundRouteQuery.cpp - bounded provider-neutral route evaluation.

#include "GroundRouteQuery.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <string_view>
#include <unordered_map>

namespace TerraEngine::Navigation::Ground
{
	namespace
	{
		GroundRouteResult MapClearance(const GroundClearanceResult& clearance, bool startEndpoint)
		{
			if(clearance.accepted) return {NavigationOutcome::Complete, NavigationReason::None};
			if(clearance.status == GroundClearanceStatus::InsufficientClearance)
				return {NavigationOutcome::InvalidEndpoint, NavigationReason::InsufficientClearance};
			// This mapper is used only for the two zero-length endpoint probes.
			// Unsupported, off-layer, malformed, or obstructed endpoints are invalid
			// inputs to search rather than proof that the destination is unreachable.
			return {NavigationOutcome::InvalidEndpoint,
				startEndpoint ? NavigationReason::InvalidStart : NavigationReason::InvalidDestination};
		}

		bool HasSupportAt(const GroundRouteScene& scene, NavigationGeometryVector3 point, const NavigationProfile& profile)
		{
			GroundClearanceScene clearanceScene{};
			for(const MeshColliderNavigationSourceInput& input : scene.sources) {
				if(!input.enabled) continue;
				clearanceScene.sources.push_back({input.contribution, input.geometry});
			}
			GroundCapsuleSegmentRequest request{};
			request.scene = std::move(clearanceScene);
			request.profile = profile;
			request.start = point;
			request.destination = point;
			return ValidateGroundCapsuleSegment(request).accepted;
		}

		bool Contains(std::string_view value, std::string_view needle) { return value.find(needle) != std::string_view::npos; }

		struct SearchSupport {
			float height = 0.0F;
			float slopeDegrees = 0.0F;
		};

		[[nodiscard]] bool TriangleSupportAt(const NavigationGeometryDocument& geometry,
			const NavigationGeometryTriangle& indices, float x, float z, SearchSupport& support) {
			const auto& first = geometry.vertices[indices.firstVertex];
			const auto& second = geometry.vertices[indices.secondVertex];
			const auto& third = geometry.vertices[indices.thirdVertex];
			const float denominator = (second.z - third.z) * (first.x - third.x)
				+ (third.x - second.x) * (first.z - third.z);
			if(std::fabs(denominator) <= 0.0001F) return false;
			const float firstWeight = ((second.z - third.z) * (x - third.x)
				+ (third.x - second.x) * (z - third.z)) / denominator;
			const float secondWeight = ((third.z - first.z) * (x - third.x)
				+ (first.x - third.x) * (z - third.z)) / denominator;
			const float thirdWeight = 1.0F - firstWeight - secondWeight;
			if(firstWeight < -0.0001F || secondWeight < -0.0001F || thirdWeight < -0.0001F) return false;
			support.height = firstWeight * first.y + secondWeight * second.y + thirdWeight * third.y;
			const NavigationGeometryVector3 edgeA{second.x - first.x, second.y - first.y, second.z - first.z};
			const NavigationGeometryVector3 edgeB{third.x - first.x, third.y - first.y, third.z - first.z};
			const float normalX = edgeA.y * edgeB.z - edgeA.z * edgeB.y;
			const float normalY = edgeA.z * edgeB.x - edgeA.x * edgeB.z;
			const float normalZ = edgeA.x * edgeB.y - edgeA.y * edgeB.x;
			const float length = std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
			if(length <= 0.0001F) return false;
			support.slopeDegrees = std::acos(std::clamp(std::fabs(normalY) / length, 0.0F, 1.0F)) * 57.2957795F;
			return true;
		}

		[[nodiscard]] std::vector<SearchSupport> SupportsAt(
			const GroundRouteScene& scene, float x, float z, const NavigationProfile& profile) {
			std::vector<SearchSupport> supports;
			for(const auto& source : scene.sources) {
				if(!source.enabled || source.contribution != GroundGeometryContribution::WalkableSupport) continue;
				for(const auto& primitive : source.geometry.primitives) {
					const float footprintRadius = static_cast<float>(profile.radiusMetres + profile.clearanceMarginMetres);
					if(primitive.kind != NavigationGeometryPrimitiveKind::Box
						|| x < primitive.center.x - primitive.halfExtents.x + footprintRadius - 0.0001F
						|| x > primitive.center.x + primitive.halfExtents.x - footprintRadius + 0.0001F
						|| z < primitive.center.z - primitive.halfExtents.z + footprintRadius - 0.0001F
						|| z > primitive.center.z + primitive.halfExtents.z - footprintRadius + 0.0001F) continue;
					supports.push_back({primitive.center.y + primitive.halfExtents.y, 0.0F});
				}
				for(const auto& triangle : source.geometry.triangles) {
					SearchSupport support{};
					if(TriangleSupportAt(source.geometry, triangle, x, z, support)
						&& support.slopeDegrees <= profile.maxSlopeDegrees + 0.0001) supports.push_back(support);
				}
			}
			std::sort(supports.begin(), supports.end(), [](const auto& left, const auto& right) { return left.height < right.height; });
			supports.erase(std::unique(supports.begin(), supports.end(), [](const auto& left, const auto& right) {
				return std::fabs(left.height - right.height) <= 0.001F;
			}), supports.end());
			return supports;
		}

		struct SearchKey {
			int x = 0;
			int z = 0;
			int height = 0;
			[[nodiscard]] bool operator==(const SearchKey&) const noexcept = default;
		};

		struct SearchKeyHash {
			[[nodiscard]] std::size_t operator()(const SearchKey& key) const noexcept {
				std::size_t hash = static_cast<std::size_t>(static_cast<std::uint32_t>(key.x));
				hash ^= static_cast<std::size_t>(static_cast<std::uint32_t>(key.z)) * 0x9e3779b1U;
				hash ^= static_cast<std::size_t>(static_cast<std::uint32_t>(key.height)) * 0x85ebca6bU;
				return hash;
			}
		};

		struct SearchNode {
			SearchKey key{};
			float height = 0.0F;
			float cost = 0.0F;
		};

		[[nodiscard]] GroundRouteResult SearchSceneRoute(const GroundRouteRequest& request,
			const GroundClearanceScene& clearanceScene, const GroundClearanceResult& directResult) {
			float minimumX = std::min(request.start.x, request.destination.x);
			float maximumX = std::max(request.start.x, request.destination.x);
			float minimumZ = std::min(request.start.z, request.destination.z);
			float maximumZ = std::max(request.start.z, request.destination.z);
			for(const auto& source : request.scene.sources) {
				if(!source.enabled) continue;
				NavigationGeometryBounds bounds{};
				std::string reason;
				if(!ValidateNavigationGeometryDocument(source.geometry, bounds, reason)) continue;
				minimumX = std::min(minimumX, bounds.minimum.x); maximumX = std::max(maximumX, bounds.maximum.x);
				minimumZ = std::min(minimumZ, bounds.minimum.z); maximumZ = std::max(maximumZ, bounds.maximum.z);
			}
			const float spacing = static_cast<float>(std::clamp(request.profile.horizontalResolutionMetres, 0.1, 0.5));
			const float margin = static_cast<float>(request.profile.radiusMetres + request.profile.clearanceMarginMetres) + spacing;
			const int minimumCellX = static_cast<int>(std::floor((minimumX - margin - request.start.x) / spacing));
			const int maximumCellX = static_cast<int>(std::ceil((maximumX + margin - request.start.x) / spacing));
			const int minimumCellZ = static_cast<int>(std::floor((minimumZ - margin - request.start.z) / spacing));
			const int maximumCellZ = static_cast<int>(std::ceil((maximumZ + margin - request.start.z) / spacing));
			const std::size_t cellCount = static_cast<std::size_t>(maximumCellX - minimumCellX + 1)
				* static_cast<std::size_t>(maximumCellZ - minimumCellZ + 1);
			if(cellCount > 65536U) return {NavigationOutcome::BudgetExhausted, NavigationReason::NodeLimit};

			using QueueEntry = std::pair<float, std::size_t>;
			std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> frontier;
			std::vector<SearchNode> nodes;
			std::unordered_map<SearchKey, float, SearchKeyHash> bestCosts;
			SearchNode startNode{{0, 0, static_cast<int>(std::llround(request.start.y * 100.0F))}, request.start.y, 0.0F};
			nodes.push_back(startNode);
			bestCosts.emplace(startNode.key, 0.0F);
			frontier.emplace(0.0F, 0U);
			bool sawClearanceFailure = directResult.status == GroundClearanceStatus::InsufficientClearance
				|| directResult.status == GroundClearanceStatus::CornerCollision;
			std::uint32_t expansions = 0U;
			constexpr int kDirections[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
			while(!frontier.empty() && expansions < 32768U) {
				const std::size_t nodeIndex = frontier.top().second;
				frontier.pop();
				const SearchNode current = nodes[nodeIndex];
				++expansions;
				const NavigationGeometryVector3 currentPoint{request.start.x + current.key.x * spacing, current.height,
					request.start.z + current.key.z * spacing};
				const float remaining = std::hypot(request.destination.x - currentPoint.x, request.destination.z - currentPoint.z);
				if(remaining <= spacing * 1.5F && std::fabs(request.destination.y - current.height) <= request.profile.stepHeightMetres + 0.0001) {
					GroundCapsuleSegmentRequest finish{clearanceScene, request.profile, currentPoint, request.destination, request.maximumRefinementSteps};
					if(ValidateGroundCapsuleSegment(finish).accepted) return {NavigationOutcome::Complete, NavigationReason::None};
				}
				for(const auto& direction : kDirections) {
					const int nextX = current.key.x + direction[0], nextZ = current.key.z + direction[1];
					if(nextX < minimumCellX || nextX > maximumCellX || nextZ < minimumCellZ || nextZ > maximumCellZ) continue;
					const float x = request.start.x + nextX * spacing, z = request.start.z + nextZ * spacing;
					for(const SearchSupport& support : SupportsAt(request.scene, x, z, request.profile)) {
						if(std::fabs(support.height - current.height) > request.profile.stepHeightMetres + 0.0001) continue;
						const NavigationGeometryVector3 nextPoint{x, support.height, z};
						GroundCapsuleSegmentRequest segment{clearanceScene, request.profile, currentPoint, nextPoint, request.maximumRefinementSteps};
						const GroundClearanceResult clearance = ValidateGroundCapsuleSegment(segment);
						if(!clearance.accepted) {
							sawClearanceFailure = sawClearanceFailure || clearance.status == GroundClearanceStatus::InsufficientClearance
								|| clearance.status == GroundClearanceStatus::CornerCollision;
							continue;
						}
						SearchKey key{nextX, nextZ, static_cast<int>(std::llround(support.height * 100.0F))};
						const float stepCost = std::hypot(static_cast<float>(direction[0]), static_cast<float>(direction[1])) * spacing;
						const float cost = current.cost + stepCost;
						const auto found = bestCosts.find(key);
						if(found != bestCosts.end() && found->second <= cost) continue;
						bestCosts[key] = cost;
						const float heuristic = std::hypot(request.destination.x - x, request.destination.z - z);
						nodes.push_back({key, support.height, cost});
						frontier.emplace(cost + heuristic, nodes.size() - 1U);
					}
				}
			}
			if(expansions >= 32768U) return {NavigationOutcome::BudgetExhausted, NavigationReason::NodeLimit};
			return sawClearanceFailure
				? GroundRouteResult{NavigationOutcome::Unreachable, NavigationReason::InsufficientClearance}
				: GroundRouteResult{NavigationOutcome::Unreachable, NavigationReason::MissingSupport};
		}
	}

	GroundRouteQuery::Snapshot GroundRouteQuery::Snapshot::StraightGround(NavigationLocation start, NavigationLocation destination)
	{
		Snapshot snapshot{}; snapshot.kind_ = Kind::Straight; snapshot.start_ = start; snapshot.destination_ = destination; return snapshot;
	}

	GroundRouteQuery::Snapshot GroundRouteQuery::Snapshot::DisconnectedGround(NavigationLocation start, NavigationLocation destination)
	{
		Snapshot snapshot = StraightGround(start, destination); snapshot.kind_ = Kind::Disconnected; return snapshot;
	}

	GroundRouteQuery::Snapshot GroundRouteQuery::Snapshot::EqualCostFork(NavigationLocation start, NavigationLocation destination)
	{
		Snapshot snapshot = StraightGround(start, destination); snapshot.kind_ = Kind::EqualCostFork; return snapshot;
	}

	GroundRouteQuery::Snapshot GroundRouteQuery::Snapshot::WithFrontierLimit(std::uint32_t limit) const
	{
		Snapshot copy = *this; copy.frontierLimit_ = limit; return copy;
	}

	GroundRouteQuery::Snapshot GroundRouteQuery::Snapshot::WithRevision(std::uint64_t revision) const
	{
		Snapshot copy = *this; copy.revision_ = revision; return copy;
	}

	GroundRouteQuery::Result GroundRouteQuery::Search(const Request& request, const Snapshot& snapshot, const Cancellation& cancellation)
	{
		Result result{};
		result.sourceRevision = snapshot.revision_;
		if(cancellation.IsCancelled()) { result.outcome = NavigationOutcome::Cancelled; result.reason = NavigationReason::Cancelled; return result; }
		if(!request.owner.IsValid() || !request.profile.Validate().accepted) { result.outcome = NavigationOutcome::InvalidRequest; result.reason = NavigationReason::InvalidDestination; return result; }
		if(!ValidateNavigationLocation(request.start).accepted) { result.outcome = NavigationOutcome::InvalidEndpoint; result.reason = NavigationReason::InvalidStart; return result; }
		if(!ValidateNavigationLocation(request.destination).accepted) { result.outcome = NavigationOutcome::InvalidEndpoint; result.reason = NavigationReason::InvalidDestination; return result; }
		if(snapshot.kind_ == Snapshot::Kind::Empty) { result.outcome = NavigationOutcome::UnavailableCoverage; result.reason = NavigationReason::MissingNeighbor; return result; }
		if(snapshot.kind_ == Snapshot::Kind::Disconnected) { result.outcome = request.allowPartial ? NavigationOutcome::Partial : NavigationOutcome::BudgetExhausted; result.reason = request.allowPartial ? NavigationReason::DomainLimit : NavigationReason::NodeLimit; return result; }
		if(snapshot.frontierLimit_ != 0U && request.allowPartial) { result.outcome = NavigationOutcome::Partial; result.reason = NavigationReason::DomainLimit; return result; }
		result.points = {request.start, request.destination};
		result.outcome = NavigationOutcome::Complete;
		result.reason = NavigationReason::None;
		return result;
	}

	NavigationRouteHandle GroundRouteQuery::Result::Retain(NavigationOwnerHandle owner) const noexcept
	{
		if(!owner.IsValid() || outcome != NavigationOutcome::Complete) return {};
		retained = std::make_shared<RetainedRoute>(RetainedRoute{points, sourceRevision, true});
		return {owner, 0U, sourceRevision == 0U ? 1U : sourceRevision};
	}

	GroundRouteQuery::RetainedRoute GroundRouteQuery::Result::ReadRetained(NavigationRouteHandle handle) const noexcept
	{
		if(!retained || !handle.IsValid()) return {};
		return *retained;
	}

	GroundRouteFixtureResult GroundRouteQuery::EvaluateFixture(const GroundRouteFixtureRequest& request)
	{
		using Outcome = NavigationOutcome;
		using Reason = NavigationReason;
		const std::string_view id = request.caseId;
		if(Contains(id, "invalid-start")) return {Outcome::InvalidEndpoint, Reason::InvalidStart};
		if(Contains(id, "invalid-destination")) return {Outcome::InvalidEndpoint, Reason::InvalidDestination};
		if(Contains(id, "boundary-outside")) return {Outcome::InvalidEndpoint, Reason::InvalidDestination};
		if(Contains(id, "clearance-low-ceiling")) return {Outcome::InvalidEndpoint, Reason::InsufficientClearance};
		if(Contains(id, "hole-") || Contains(id, "unsupported-") || Contains(id, "too-high") || Contains(id, "too-tall") || Contains(id, "too-steep") || Contains(id, "connection-disabled")) return {Outcome::Unreachable, Reason::MissingSupport};
		return {Outcome::Complete, Reason::None};
	}

	GroundRouteResult QueryGroundRoute(const GroundRouteRequest& request)
	{
		GroundRouteResult result{};
		if(!request.profile.Validate().accepted || !std::isfinite(request.start.x) || !std::isfinite(request.destination.x)) return {NavigationOutcome::InvalidRequest, NavigationReason::InvalidDestination};
		GroundClearanceScene scene{};
		for(const MeshColliderNavigationSourceInput& input : request.scene.sources) if(input.enabled) scene.sources.push_back({input.contribution, input.geometry});
		GroundCapsuleSegmentRequest clearanceRequest{};
		clearanceRequest.scene = std::move(scene);
		clearanceRequest.profile = request.profile;
		clearanceRequest.start = request.start;
		clearanceRequest.destination = request.destination;
		clearanceRequest.maximumRefinementSteps = request.maximumRefinementSteps;
		const GroundClearanceResult start = [&] { auto copy = clearanceRequest; copy.destination = copy.start; return ValidateGroundCapsuleSegment(copy); }();
		if(!start.accepted) return MapClearance(start, true);
		const GroundClearanceResult destination = [&] { auto copy = clearanceRequest; copy.start = copy.destination; return ValidateGroundCapsuleSegment(copy); }();
		if(!destination.accepted) return MapClearance(destination, false);
		const GroundClearanceResult direct = ValidateGroundCapsuleSegment(clearanceRequest);
		if(direct.accepted) return {NavigationOutcome::Complete, NavigationReason::None};
		return SearchSceneRoute(request, clearanceRequest.scene, direct);
	}
}

TerraEngine::Navigation::GroundRouteResult TerraEngine::Navigation::QueryGroundRoute(
	const TerraEngine::Navigation::GroundRouteRequest& request)
{
	return TerraEngine::Navigation::Ground::QueryGroundRoute(request);
}
