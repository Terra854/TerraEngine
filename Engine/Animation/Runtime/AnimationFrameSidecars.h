// AnimationFrameSidecars.h - Immutable sparse outputs for one authority frame.
//
// Common animation state belongs in authority rows.  This file intentionally
// keeps uncommon specialist work in ordered sidecars, so ordinary animators do
// not pay storage or traversal costs for events, physics, or diagnostics.

#pragma once

#include "Animation/Runtime/PoseBuffer.h"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	struct AnimationSlotGenerationKey;

	// The key is repeated with every sparse value.  Consumers can therefore
	// reject a recycled animator slot without reopening live runtime storage.
	struct AnimationSidecarKey {
		std::uint32_t slot = kInvalidAnimatorInstanceSlot;
		AnimationGeneration lifetimeGeneration = 0U;
		std::uint64_t sessionGeneration = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept
		{
			return slot != kInvalidAnimatorInstanceSlot && lifetimeGeneration != 0U
				&& sessionGeneration != 0U;
		}

		auto operator<=>(const AnimationSidecarKey&) const = default;
	};

	// A compact opaque result describes specialist work whose full typed payload
	// remains owned by its existing subsystem.  This avoids placing a wide union
	// in every authority row while preserving deterministic ordering/provenance.
	struct AnimationSpecialistRecord {
		AnimationSidecarKey key{};
		std::uint64_t authorityGeneration = 0U;
		std::uint64_t correlationId = 0U;
		AnimationStatus status = AnimationStatus::NotReady;
		std::uint32_t kind = 0U;
		std::uint32_t flags = 0U;

		[[nodiscard]] bool IsValidFor(std::uint64_t generation) const noexcept
		{
			return key.IsValid() && authorityGeneration == generation;
		}
	};

	struct AnimationSidecarDiagnostics {
		std::uint32_t evaluatedRows = 0U;
		std::uint32_t fallbackRows = 0U;
		std::uint32_t rejectedRows = 0U;
		std::uint32_t reserved = 0U;
	};

	// All vectors are built and sorted before sealing.  Published roots expose a
	// shared const instance only, making the sidecar lifetime match its authority.
	struct AnimationFrameSidecars {
		std::vector<AnimationEventRecord> events;
		std::vector<AnimationRootMotionDelta> rootMotion;
		std::vector<AnimationSpecialistRecord> callbacks;
		std::vector<AnimationSpecialistRecord> physics;
		std::vector<AnimationSpecialistRecord> attachments;
		std::vector<AnimationSpecialistRecord> procedural;
		std::vector<AnimationSpecialistRecord> rollback;
		std::vector<AnimationSpecialistRecord> completions;
		std::vector<AnimationSpecialistRecord> gpuCandidates;
		std::vector<AnimationSpecialistRecord> diagnostics;
		AnimationSidecarDiagnostics summary{};
		// Production compact workers validate and order their disjoint sidecars
		// before this mutable owner reaches SealAnimationFrameResult. A matching
		// generation avoids a second serial crowd scan; external inputs leave zero.
		std::uint64_t workerValidatedAuthorityGeneration = 0U;

		// Deterministic ordering is established once during sealing.  Managed and
		// native consumers subsequently enumerate exactly their participating span.
		[[nodiscard]] bool ValidateAndOrder(std::uint64_t authorityGeneration) noexcept;
	};

	[[nodiscard]] constexpr AnimationSidecarKey ToAnimationSidecarKey(
		AnimatorInstanceHandle handle, std::uint64_t sessionGeneration) noexcept
	{
		return {handle.slotIndex, handle.generation, sessionGeneration};
	}
}
