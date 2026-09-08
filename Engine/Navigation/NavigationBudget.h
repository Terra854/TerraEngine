// NavigationBudget.h -- Thread-safe RAII accounting for bounded navigation memory.

#pragma once

#include "Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Navigation
{
	enum class NavigationBudgetCategory : std::uint8_t
	{
		Geometry,
		PublishedNavigation,
		BuildScratch,
		RequestRoute,
		ProfileBuild,
		Diagnostic,
		Lease
	};

	struct NavigationBudgetLimits
	{
		std::size_t geometryBytes = 256U * 1024U * 1024U;
		std::size_t publishedNavigationBytes = 512U * 1024U * 1024U;
		std::size_t buildScratchBytes = 256U * 1024U * 1024U;
		std::size_t requestRouteBytes = 64U * 1024U * 1024U;
		std::size_t profileBuildCount = 16U;
		std::size_t diagnosticBytes = 4U * 1024U * 1024U;
		std::size_t leaseBytes = 1024U * 1024U;
	};

	class NavigationBudget;

	class TERRAENGINE_API NavigationBudgetReservation final
	{
	public:
		NavigationBudgetReservation() = default;
		NavigationBudgetReservation(const NavigationBudgetReservation&) = delete;
		NavigationBudgetReservation& operator=(const NavigationBudgetReservation&) = delete;
		NavigationBudgetReservation(NavigationBudgetReservation&& other) noexcept;
		NavigationBudgetReservation& operator=(NavigationBudgetReservation&& other) noexcept;
		~NavigationBudgetReservation();

		bool accepted = false;
		std::string reason;
		[[nodiscard]] std::size_t Bytes() const noexcept;
		[[nodiscard]] NavigationBudgetCategory Category() const noexcept;
		void Release() noexcept;

	private:
		friend class NavigationBudget;
		struct State;
		explicit NavigationBudgetReservation(std::shared_ptr<State> state, NavigationBudgetCategory category,
			std::size_t bytes, std::string allocationKey);
		std::shared_ptr<State> state;
		NavigationBudgetCategory category = NavigationBudgetCategory::Geometry;
		std::size_t bytes = 0U;
		std::string allocationKey;
	};

	class TERRAENGINE_API NavigationBudget final
	{
	public:
		explicit NavigationBudget(NavigationBudgetLimits limits = {});
		[[nodiscard]] NavigationBudgetReservation TryReserve(NavigationBudgetCategory category,
			std::size_t bytes, std::string purpose);
		// Shared source/BVH buffers are charged once by a stable allocation key.
		// Additional callers receive a RAII lease without consuming capacity again.
		[[nodiscard]] NavigationBudgetReservation TryReserveShared(NavigationBudgetCategory category,
			std::size_t bytes, std::string allocationKey);
		[[nodiscard]] std::size_t UsedBytes(NavigationBudgetCategory category) const;
		[[nodiscard]] std::size_t LimitBytes(NavigationBudgetCategory category) const;
		[[nodiscard]] std::vector<std::string> RejectionReasons() const;

	private:
		std::shared_ptr<NavigationBudgetReservation::State> state;
	};
}
