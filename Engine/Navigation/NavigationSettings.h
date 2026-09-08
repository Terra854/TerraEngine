// NavigationSettings.h - validated ground profiles and bounded world limits.
//
// These values describe engine semantics before a worker receives geometry or
// a query. They intentionally contain no provider configuration objects.

#pragma once

#include "NavigationTypes.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace TerraEngine::Navigation {
	inline constexpr std::size_t kNavigationAreaCount = 32U;
	inline constexpr std::size_t kNavigationMiB = 1024U * 1024U;

	enum class NavigationProfileValidationCode : std::uint8_t {
		None,
		InvalidId,
		InvalidRevision,
		InvalidRadius,
		InvalidHeight,
		InvalidStepHeight,
		InvalidSlope,
		InvalidClearanceMargin,
		InvalidResolution,
		InvalidAreaPenalty
	};

	struct NavigationProfileValidation {
		bool accepted = false;
		NavigationProfileValidationCode code = NavigationProfileValidationCode::InvalidId;

		[[nodiscard]] constexpr bool Succeeded() const noexcept { return accepted; }
	};

	// Profiles are value data captured into immutable build/query snapshots. The
	// ground character is always an upright capsule; navigation never selects a
	// different locomotion mode for it.
	struct NavigationProfile {
		std::uint64_t profileId = 1U;
		NavigationGeneration revision = 1U;
		double radiusMetres = 0.4;
		double heightMetres = 1.8;
		double stepHeightMetres = 0.35;
		double maxSlopeDegrees = 45.0;
		double clearanceMarginMetres = 0.01;
		double horizontalResolutionMetres = 0.1;
		double verticalResolutionMetres = 0.05;
		std::uint32_t excludedAreasMask = 0U;
		std::array<double, kNavigationAreaCount> areaPenalties{};

		[[nodiscard]] static NavigationProfile Defaults() noexcept;
		[[nodiscard]] NavigationProfileValidation Validate() const noexcept;
		[[nodiscard]] double AreaTraversalMultiplier(std::size_t areaIndex) const noexcept;
	};

	enum class NavigationSettingsValidationCode : std::uint8_t {
		None,
		InvalidProfileBuildCapacity,
		InvalidAgentCapacity,
		InvalidRequestCapacity,
		InvalidSearchNodeCapacity,
		InvalidRoutePointCapacity,
		InvalidMemoryBudget,
		InvalidConcurrencyLimit,
		InvalidRetentionLimit
	};

	struct NavigationSettingsValidation {
		bool accepted = false;
		NavigationSettingsValidationCode code = NavigationSettingsValidationCode::InvalidRequestCapacity;

		[[nodiscard]] constexpr bool Succeeded() const noexcept { return accepted; }
	};

	// Frozen defaults are the documented reference profile. Projects may choose
	// lower safe limits, but zero, overflow-prone, and unbounded settings reject
	// before a scene mutation or worker admission is attempted.
	struct NavigationSettings {
		std::uint32_t maxActiveProfileBuilds = 16U;
		std::uint32_t maxActiveAgents = 4096U;
		std::uint32_t maxAdmittedRequests = 1024U;
		std::uint32_t maxConcurrentBuilds = 4U;
		std::uint32_t maxConcurrentSearches = 8U;
		std::uint32_t maxDetourNodesPerQuery = 32768U;
		std::uint32_t maxEngineNodesPerRequest = 65536U;
		std::uint32_t maxRoutePoints = 4096U;
		std::uint32_t workerExpansionQuantum = 1024U;
		std::uint64_t maxGeometryBytes = 256U * kNavigationMiB;
		std::uint64_t maxPublishedAndRetiredBytes = 512U * kNavigationMiB;
		std::uint64_t maxBuildScratchBytes = 256U * kNavigationMiB;
		std::uint64_t maxRequestAndRouteBytes = 64U * kNavigationMiB;
		std::uint64_t maxDiagnosticBytes = 4U * kNavigationMiB;
		std::uint64_t defaultRequestDeadlineMilliseconds = 5000U;
		std::uint64_t maximumRequestLifetimeMilliseconds = 30000U;
		std::uint64_t defaultRouteRetentionMilliseconds = 30000U;
		std::uint64_t terminalRequestRetentionMilliseconds = 10000U;

		[[nodiscard]] static NavigationSettings Defaults() noexcept;
		[[nodiscard]] NavigationSettingsValidation Validate() const noexcept;
	};

	// This counter is the small atomic admission primitive used before a request
	// record is constructed. A failed compare-and-swap leaves all prior admitted
	// work untouched, giving callers an all-or-nothing capacity decision.
	class NavigationAdmissionCounter {
	public:
		explicit NavigationAdmissionCounter(std::uint32_t capacity) noexcept;

		[[nodiscard]] bool TryReserve(std::uint32_t count = 1U) noexcept;
		void Release(std::uint32_t count = 1U) noexcept;
		[[nodiscard]] std::uint32_t ReservedCount() const noexcept;
		[[nodiscard]] std::uint32_t Capacity() const noexcept;

	private:
		std::uint32_t capacity_ = 0U;
		std::atomic<std::uint32_t> reservedCount_{0U};
	};

	TERRAENGINE_API std::string_view ToString(NavigationProfileValidationCode code) noexcept;
	TERRAENGINE_API std::string_view ToString(NavigationSettingsValidationCode code) noexcept;
}
