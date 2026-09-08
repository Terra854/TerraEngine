// NavigationBudget.cpp -- Atomic bounded accounting with RAII release.

#include "Navigation/NavigationBudget.h"

#include <array>
#include <mutex>
#include <unordered_map>

namespace TerraEngine::Navigation
{
	namespace
	{
		constexpr std::size_t kCategoryCount = 7U;
		[[nodiscard]] constexpr std::size_t CategoryIndex(NavigationBudgetCategory category) noexcept
		{
			return static_cast<std::size_t>(category);
		}
		[[nodiscard]] std::size_t LimitFor(const NavigationBudgetLimits& limits, NavigationBudgetCategory category) noexcept
		{
			switch(category) {
			case NavigationBudgetCategory::Geometry: return limits.geometryBytes;
			case NavigationBudgetCategory::PublishedNavigation: return limits.publishedNavigationBytes;
			case NavigationBudgetCategory::BuildScratch: return limits.buildScratchBytes;
			case NavigationBudgetCategory::RequestRoute: return limits.requestRouteBytes;
			case NavigationBudgetCategory::ProfileBuild: return limits.profileBuildCount;
			case NavigationBudgetCategory::Diagnostic: return limits.diagnosticBytes;
			case NavigationBudgetCategory::Lease: return limits.leaseBytes;
			}
			return 0U;
		}
	}

	struct NavigationBudgetReservation::State
	{
		struct SharedAllocation { NavigationBudgetCategory category{}; std::size_t bytes = 0U; std::size_t references = 0U; };
		NavigationBudgetLimits limits{};
		std::array<std::size_t, kCategoryCount> used{};
		std::unordered_map<std::string, SharedAllocation> sharedAllocations;
		std::vector<std::string> rejectionReasons;
		mutable std::mutex mutex;

		void Release(NavigationBudgetCategory category, std::size_t bytes, const std::string& allocationKey) noexcept
		{
			std::scoped_lock lock(mutex);
			if(!allocationKey.empty()) {
				auto found = sharedAllocations.find(allocationKey);
				if(found == sharedAllocations.end()) return;
				if(--found->second.references != 0U) return;
				used[CategoryIndex(category)] -= found->second.bytes;
				sharedAllocations.erase(found);
				return;
			}
			used[CategoryIndex(category)] -= bytes;
		}
	};

	NavigationBudgetReservation::NavigationBudgetReservation(std::shared_ptr<State> reservationState,
		NavigationBudgetCategory reservationCategory, std::size_t reservationBytes, std::string key)
		: accepted(true), state(std::move(reservationState)), category(reservationCategory), bytes(reservationBytes), allocationKey(std::move(key)) {}
	NavigationBudgetReservation::NavigationBudgetReservation(NavigationBudgetReservation&& other) noexcept
		: accepted(other.accepted), reason(std::move(other.reason)), state(std::move(other.state)), category(other.category), bytes(other.bytes), allocationKey(std::move(other.allocationKey))
	{
		other.accepted = false;
		other.bytes = 0U;
	}
	NavigationBudgetReservation& NavigationBudgetReservation::operator=(NavigationBudgetReservation&& other) noexcept
	{
		if(this == &other) return *this;
		Release();
		accepted = other.accepted;
		reason = std::move(other.reason);
		state = std::move(other.state);
		category = other.category;
		bytes = other.bytes;
		allocationKey = std::move(other.allocationKey);
		other.accepted = false;
		other.bytes = 0U;
		return *this;
	}
	NavigationBudgetReservation::~NavigationBudgetReservation() { Release(); }
	std::size_t NavigationBudgetReservation::Bytes() const noexcept { return bytes; }
	NavigationBudgetCategory NavigationBudgetReservation::Category() const noexcept { return category; }
	void NavigationBudgetReservation::Release() noexcept
	{
		if(!accepted || !state) return;
		state->Release(category, bytes, allocationKey);
		accepted = false;
		bytes = 0U;
		allocationKey.clear();
		state.reset();
	}

	NavigationBudget::NavigationBudget(NavigationBudgetLimits limits)
		: state(std::make_shared<NavigationBudgetReservation::State>())
	{
		state->limits = limits;
	}

	NavigationBudgetReservation NavigationBudget::TryReserve(NavigationBudgetCategory category, std::size_t bytes, std::string purpose)
	{
		std::scoped_lock lock(state->mutex);
		const std::size_t limit = LimitFor(state->limits, category);
		const std::size_t used = state->used[CategoryIndex(category)];
		if(bytes > limit || used > limit - bytes) {
			state->rejectionReasons.push_back("GeometryBudgetExceeded");
			if(state->rejectionReasons.size() > 64U) state->rejectionReasons.erase(state->rejectionReasons.begin());
			NavigationBudgetReservation rejected{};
			rejected.reason = "GeometryBudgetExceeded";
			return rejected;
		}
		state->used[CategoryIndex(category)] += bytes;
		return NavigationBudgetReservation(state, category, bytes, {});
	}

	NavigationBudgetReservation NavigationBudget::TryReserveShared(NavigationBudgetCategory category, std::size_t bytes, std::string allocationKey)
	{
		if(allocationKey.empty()) return TryReserve(category, bytes, "unnamed shared allocation");
		std::scoped_lock lock(state->mutex);
		if(auto found = state->sharedAllocations.find(allocationKey); found != state->sharedAllocations.end()) {
			if(found->second.category != category || found->second.bytes != bytes) {
				NavigationBudgetReservation rejected{};
				rejected.reason = "SharedAllocationIdentityMismatch";
				return rejected;
			}
			++found->second.references;
			return NavigationBudgetReservation(state, category, bytes, std::move(allocationKey));
		}
		const std::size_t limit = LimitFor(state->limits, category);
		const std::size_t used = state->used[CategoryIndex(category)];
		if(bytes > limit || used > limit - bytes) {
			state->rejectionReasons.push_back("GeometryBudgetExceeded");
			NavigationBudgetReservation rejected{};
			rejected.reason = "GeometryBudgetExceeded";
			return rejected;
		}
		state->used[CategoryIndex(category)] += bytes;
		state->sharedAllocations.emplace(allocationKey, NavigationBudgetReservation::State::SharedAllocation{ category, bytes, 1U });
		return NavigationBudgetReservation(state, category, bytes, std::move(allocationKey));
	}

	std::size_t NavigationBudget::UsedBytes(NavigationBudgetCategory category) const
	{
		std::scoped_lock lock(state->mutex);
		return state->used[CategoryIndex(category)];
	}
	std::size_t NavigationBudget::LimitBytes(NavigationBudgetCategory category) const
	{
		std::scoped_lock lock(state->mutex);
		return LimitFor(state->limits, category);
	}
	std::vector<std::string> NavigationBudget::RejectionReasons() const
	{
		std::scoped_lock lock(state->mutex);
		return state->rejectionReasons;
	}
}
