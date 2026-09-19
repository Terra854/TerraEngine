// NavigationAgentTypes.h - provider-neutral values shared by navigation agents.
//
// These records are deliberately copyable values. The agent system may pass them
// between protected scene boundaries and worker jobs without exposing ECS or a
// navigation provider's mutable state.

#pragma once

#include "Navigation/NavigationSettings.h"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace TerraEngine::Navigation {
	// Finite limits prevent command or diagnostic retention from becoming an
	// unbounded gameplay-memory sink. Runtime settings may choose lower values.
	inline constexpr std::size_t kNavigationAgentMaximumCommandCount = 8192U;
	inline constexpr std::size_t kNavigationAgentMaximumReceiptCount = 16384U;
	inline constexpr std::size_t kNavigationAgentMaximumRuntimeRecords = 4096U;
	inline constexpr std::size_t kNavigationAgentMaximumGuidanceSnapshots = 4096U;

	// An attachment generation distinguishes remove/re-add from an older agent
	// attached to the same persistent scene object. Runtime slot generation keeps
	// recycled bounded-store slots from reviving stale commands.
	struct TERRAENGINE_API NavigationAgentIdentity {
		NavigationWorldHandle world{};
		NavigationObjectIdentity object{};
		NavigationGeneration attachmentGeneration = 0U;
		NavigationSlotIndex runtimeSlot = kInvalidNavigationSlot;
		NavigationGeneration runtimeSlotGeneration = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept {
			return world.IsValid() && object.IsValid()
				&& attachmentGeneration != 0U && runtimeSlot != kInvalidNavigationSlot
				&& runtimeSlotGeneration != 0U;
		}
		[[nodiscard]] constexpr bool operator==(const NavigationAgentIdentity&) const = default;
	};

	enum class NavigationAgentState : std::uint8_t {
		Idle,
		Requesting,
		Following,
		Arrived,
		Stopped,
		Failed
	};

	enum class NavigationAgentCommandKind : std::uint8_t {
		SetLocationDestination,
		SetObjectDestination,
		Stop
	};

	enum class NavigationAgentReceiptState : std::uint8_t {
		Queued,
		Applied,
		Rejected,
		Superseded,
		Expired
	};

	// A command has all values needed for protected admission. It contains no
	// live object pointer, so a worker never resolves scene data on behalf of a
	// caller. The system assigns admittedSequence at its protected boundary.
	struct TERRAENGINE_API NavigationAgentCommand {
		NavigationAgentIdentity identity{};
		NavigationAgentCommandKind kind = NavigationAgentCommandKind::Stop;
		NavigationRequestedDestination destination{};
		NavigationGeneration expectedConfigurationRevision = 0U;
		NavigationGeneration expectedIntentSequence = 0U;
		std::uint64_t producerOrderingToken = 0U;
		std::uint64_t correlationId = 0U;
	};

	struct TERRAENGINE_API NavigationAgentReceipt {
		NavigationAgentIdentity identity{};
		NavigationAgentReceiptState state = NavigationAgentReceiptState::Rejected;
		NavigationOutcome outcome = NavigationOutcome::InvalidRequest;
		NavigationReason reason = NavigationReason::None;
		std::uint64_t correlationId = 0U;
		std::uint64_t admittedSequence = 0U;
	};

	// Guidance is advisory input for a controller. Navigation never fills speed
	// or velocity, which leaves all movement authority with gameplay systems.
	struct TERRAENGINE_API NavigationAgentGuidanceSnapshot {
		NavigationAgentIdentity identity{};
		NavigationGeneration configurationRevision = 0U;
		NavigationGeneration intentSequence = 0U;
		NavigationGeneration publicationGeneration = 0U;
		NavigationGeneration routeGeneration = 0U;
		NavigationObservationEvidence observation{};
		NavigationAgentState state = NavigationAgentState::Idle;
		NavigationOutcome outcome = NavigationOutcome::Queued;
		NavigationReason reason = NavigationReason::None;
		NavigationRequestedDestination destination{};
		NavigationLocation nextWaypoint{};
		glm::dvec3 suggestedDirection{0.0};
		double remainingDistanceMetres = 0.0;
		bool usable = false;

		[[nodiscard]] bool IsUsable() const noexcept;
	};

	struct TERRAENGINE_API NavigationAgentAuthoredSettings {
		NavigationProfile traversal = NavigationProfile::Defaults();
		double arrivalToleranceMetres = 0.10;
		double targetDisplacementThresholdMetres = 0.25;
		double minimumReplanIntervalSeconds = 0.50;

		[[nodiscard]] bool Validate() const noexcept;
	};

	TERRAENGINE_API std::string_view ToString(NavigationAgentState state) noexcept;
	TERRAENGINE_API std::string_view ToString(NavigationAgentCommandKind kind) noexcept;
	TERRAENGINE_API std::string_view ToString(NavigationAgentReceiptState state) noexcept;
}
