#include "NavigationSettings.h"

#include <cmath>
#include <limits>

namespace TerraEngine::Navigation {
	namespace {
		[[nodiscard]] bool IsFinite(double value) noexcept {
			return std::isfinite(value);
		}

		[[nodiscard]] bool HasPositiveBoundedMemory(std::uint64_t bytes) noexcept {
			// The top bit is reserved as a simple overflow guard for later additions
			// of charged allocations. The reference values are far below this limit.
			return bytes != 0U && bytes <= std::numeric_limits<std::uint64_t>::max() / 2U;
		}
	}

	NavigationProfile NavigationProfile::Defaults() noexcept {
		return {};
	}

	NavigationProfileValidation NavigationProfile::Validate() const noexcept {
		if(!IsFinite(radiusMetres) || radiusMetres <= 0.0) {
			return { false, NavigationProfileValidationCode::InvalidRadius };
		}
		if(!IsFinite(heightMetres) || heightMetres < radiusMetres * 2.0) {
			return { false, NavigationProfileValidationCode::InvalidHeight };
		}
		if(!IsFinite(stepHeightMetres) || stepHeightMetres < 0.0) {
			return { false, NavigationProfileValidationCode::InvalidStepHeight };
		}
		if(!IsFinite(maxSlopeDegrees) || maxSlopeDegrees < 0.0 || maxSlopeDegrees >= 90.0) {
			return { false, NavigationProfileValidationCode::InvalidSlope };
		}
		if(!IsFinite(clearanceMarginMetres) || clearanceMarginMetres < 0.0) {
			return { false, NavigationProfileValidationCode::InvalidClearanceMargin };
		}
		if(!IsFinite(horizontalResolutionMetres) || horizontalResolutionMetres <= 0.0
			|| !IsFinite(verticalResolutionMetres) || verticalResolutionMetres <= 0.0) {
			return { false, NavigationProfileValidationCode::InvalidResolution };
		}
		for(double penalty : areaPenalties) {
			if(!IsFinite(penalty) || penalty < 0.0) {
				return { false, NavigationProfileValidationCode::InvalidAreaPenalty };
			}
		}
		return { true, NavigationProfileValidationCode::None };
	}

	double NavigationProfile::AreaTraversalMultiplier(std::size_t areaIndex) const noexcept {
		if(areaIndex >= areaPenalties.size()) {
			return std::numeric_limits<double>::infinity();
		}
		return 1.0 + areaPenalties[areaIndex];
	}

	NavigationSettings NavigationSettings::Defaults() noexcept {
		return {};
	}

	NavigationSettingsValidation NavigationSettings::Validate() const noexcept {
		if(maxActiveProfileBuilds == 0U) {
			return { false, NavigationSettingsValidationCode::InvalidProfileBuildCapacity };
		}
		if(maxActiveAgents == 0U) {
			return { false, NavigationSettingsValidationCode::InvalidAgentCapacity };
		}
		if(maxAdmittedRequests == 0U) {
			return { false, NavigationSettingsValidationCode::InvalidRequestCapacity };
		}
		if(maxDetourNodesPerQuery == 0U || maxEngineNodesPerRequest == 0U || maxRoutePoints == 0U
			|| workerExpansionQuantum == 0U) {
			return { false, NavigationSettingsValidationCode::InvalidSearchNodeCapacity };
		}
		if(maxConcurrentBuilds == 0U || maxConcurrentSearches == 0U) {
			return { false, NavigationSettingsValidationCode::InvalidConcurrencyLimit };
		}
		if(!HasPositiveBoundedMemory(maxGeometryBytes)
			|| !HasPositiveBoundedMemory(maxPublishedAndRetiredBytes)
			|| !HasPositiveBoundedMemory(maxBuildScratchBytes)
			|| !HasPositiveBoundedMemory(maxRequestAndRouteBytes)
			|| !HasPositiveBoundedMemory(maxDiagnosticBytes)) {
			return { false, NavigationSettingsValidationCode::InvalidMemoryBudget };
		}
		if(defaultRequestDeadlineMilliseconds == 0U
			|| maximumRequestLifetimeMilliseconds < defaultRequestDeadlineMilliseconds
			|| defaultRouteRetentionMilliseconds == 0U
			|| terminalRequestRetentionMilliseconds == 0U) {
			return { false, NavigationSettingsValidationCode::InvalidRetentionLimit };
		}
		return { true, NavigationSettingsValidationCode::None };
	}

	NavigationAdmissionCounter::NavigationAdmissionCounter(std::uint32_t capacity) noexcept
		: capacity_(capacity) {
	}

	bool NavigationAdmissionCounter::TryReserve(std::uint32_t count) noexcept {
		if(count == 0U) {
			return true;
		}

		std::uint32_t observed = reservedCount_.load(std::memory_order_acquire);
		while(true) {
			if(observed > capacity_ || count > capacity_ - observed) {
				return false;
			}
			if(reservedCount_.compare_exchange_weak(
				observed,
				static_cast<std::uint32_t>(observed + count),
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
				return true;
			}
		}
	}

	void NavigationAdmissionCounter::Release(std::uint32_t count) noexcept {
		if(count == 0U) {
			return;
		}

		std::uint32_t observed = reservedCount_.load(std::memory_order_acquire);
		while(true) {
			const std::uint32_t next = count >= observed ? 0U : observed - count;
			if(reservedCount_.compare_exchange_weak(
				observed,
				next,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
				return;
			}
		}
	}

	std::uint32_t NavigationAdmissionCounter::ReservedCount() const noexcept {
		return reservedCount_.load(std::memory_order_acquire);
	}

	std::uint32_t NavigationAdmissionCounter::Capacity() const noexcept {
		return capacity_;
	}

	std::string_view ToString(NavigationProfileValidationCode code) noexcept {
		switch(code) {
		case NavigationProfileValidationCode::None: return "None";
		case NavigationProfileValidationCode::InvalidRadius: return "InvalidRadius";
		case NavigationProfileValidationCode::InvalidHeight: return "InvalidHeight";
		case NavigationProfileValidationCode::InvalidStepHeight: return "InvalidStepHeight";
		case NavigationProfileValidationCode::InvalidSlope: return "InvalidSlope";
		case NavigationProfileValidationCode::InvalidClearanceMargin: return "InvalidClearanceMargin";
		case NavigationProfileValidationCode::InvalidResolution: return "InvalidResolution";
		case NavigationProfileValidationCode::InvalidAreaPenalty: return "InvalidAreaPenalty";
		}
		return "UnknownNavigationProfileValidationCode";
	}

	std::string_view ToString(NavigationSettingsValidationCode code) noexcept {
		switch(code) {
		case NavigationSettingsValidationCode::None: return "None";
		case NavigationSettingsValidationCode::InvalidProfileBuildCapacity: return "InvalidProfileBuildCapacity";
		case NavigationSettingsValidationCode::InvalidAgentCapacity: return "InvalidAgentCapacity";
		case NavigationSettingsValidationCode::InvalidRequestCapacity: return "InvalidRequestCapacity";
		case NavigationSettingsValidationCode::InvalidSearchNodeCapacity: return "InvalidSearchNodeCapacity";
		case NavigationSettingsValidationCode::InvalidRoutePointCapacity: return "InvalidRoutePointCapacity";
		case NavigationSettingsValidationCode::InvalidMemoryBudget: return "InvalidMemoryBudget";
		case NavigationSettingsValidationCode::InvalidConcurrencyLimit: return "InvalidConcurrencyLimit";
		case NavigationSettingsValidationCode::InvalidRetentionLimit: return "InvalidRetentionLimit";
		}
		return "UnknownNavigationSettingsValidationCode";
	}
}
