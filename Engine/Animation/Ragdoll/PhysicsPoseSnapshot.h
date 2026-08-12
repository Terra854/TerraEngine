// PhysicsPoseSnapshot.h - Immutable copied physics results consumed by animation.
#pragma once
#include "Animation/AnimationTypes.h"
#include "Animation/Assets/AnimationAssetTypes.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <numeric>
#include <span>
#include <tuple>
#include <vector>
namespace TerraEngine::Animation {
	inline constexpr std::size_t kRagdollSnapshotMaximumBodies = 256U;
	struct RagdollGenerationStamp {
		AnimationGeneration worldGeneration = 0U, profileGeneration = 0U, skeletonGeneration = 0U, publicationGeneration = 0U;
		AnimationFixedTick fixedStep = 0;
		// A generation stamp names one sealed simulation result. The fixed step is
		// part of the identity so a later extraction can never masquerade as the
		// current physics result for an earlier animation evaluation.
		constexpr bool IsValid() const noexcept {
			return worldGeneration != 0U && profileGeneration != 0U
				&& skeletonGeneration != 0U && publicationGeneration != 0U && fixedStep > 0;
		}
		auto operator<=>(const RagdollGenerationStamp&) const = default;
	};
	struct PhysicsPoseBodySnapshot {
		std::uint64_t bodyId = 0U; std::uint64_t stableJointId = 0U; AnimationTransform modelPose{};
		AnimationFloat3 linearVelocity{}; AnimationFloat3 angularVelocity{}; bool sleeping = false;
		// This is the provider body-slot generation copied at extraction time. A
		// default first generation keeps value construction convenient, while an
		// explicit zero remains invalid and is rejected before publication.
		AnimationGeneration bodyGeneration = 1U;
		auto operator<=>(const PhysicsPoseBodySnapshot&) const = default;
	};
	struct PhysicsPoseSnapshot {
		RagdollGenerationStamp generation{}; std::vector<PhysicsPoseBodySnapshot> bodies;
		bool completeCurrentStep = false;
	};
	struct PhysicsPoseSnapshotRecord {
		std::uint64_t ragdollId = 0U;
		std::shared_ptr<const PhysicsPoseSnapshot> snapshot;
	};

	// Expected body generations are deliberately a copied, typed validation
	// input. The animation layer never receives a mutable provider body or Jolt
	// pointer merely to decide whether a published pose is current.
	struct PhysicsPoseExpectedBodyGeneration {
		std::uint64_t bodyId = 0U;
		AnimationGeneration bodyGeneration = 0U;
		auto operator<=>(const PhysicsPoseExpectedBodyGeneration&) const = default;
	};

	// The two index builders keep validation bounded without allocating a hash map
	// on the fixed-step publication path. Callers keep their source arrays immutable;
	// only these stack-local indexes are sorted to compare stable identities.
	inline void BuildPhysicsPoseBodyOrder(
		std::span<const PhysicsPoseBodySnapshot> bodies,
		std::array<std::size_t, kRagdollSnapshotMaximumBodies>& order) noexcept {
		std::iota(order.begin(), order.end(), 0U);
		std::sort(order.begin(), order.begin() + bodies.size(), [&bodies](std::size_t left, std::size_t right) {
			return std::tie(bodies[left].bodyId, bodies[left].stableJointId)
				< std::tie(bodies[right].bodyId, bodies[right].stableJointId);
		});
	}

	inline void BuildPhysicsPoseExpectedOrder(
		std::span<const PhysicsPoseExpectedBodyGeneration> bodies,
		std::array<std::size_t, kRagdollSnapshotMaximumBodies>& order) noexcept {
		std::iota(order.begin(), order.end(), 0U);
		std::sort(order.begin(), order.begin() + bodies.size(), [&bodies](std::size_t left, std::size_t right) {
			return bodies[left].bodyId < bodies[right].bodyId;
		});
	}

	inline AnimationStatus ValidatePhysicsPoseSnapshot(const PhysicsPoseSnapshot& value, const RagdollGenerationStamp& expected) noexcept {
		if(!value.completeCurrentStep || !value.generation.IsValid() || value.generation != expected
			|| value.bodies.empty() || value.bodies.size() > kRagdollSnapshotMaximumBodies) return AnimationStatus::StaleHandle;
		std::array<std::size_t, kRagdollSnapshotMaximumBodies> order{};
		BuildPhysicsPoseBodyOrder(value.bodies, order);
		for(std::size_t index = 0U; index < value.bodies.size(); ++index) {
			const PhysicsPoseBodySnapshot& body = value.bodies[order[index]];
			if(body.bodyId == 0U || body.stableJointId == 0U || body.bodyGeneration == 0U
				|| !IsFinite(body.modelPose) || !IsFinite(body.linearVelocity) || !IsFinite(body.angularVelocity)) return AnimationStatus::InvalidArgument;
			if(index != 0U) {
				const PhysicsPoseBodySnapshot& previous = value.bodies[order[index - 1U]];
				if(previous.bodyId == body.bodyId || previous.stableJointId == body.stableJointId) return AnimationStatus::InvalidArgument;
			}
		}
		return AnimationStatus::Ok;
	}

	inline AnimationStatus ValidatePhysicsPoseSnapshot(
		const PhysicsPoseSnapshot& value,
		const RagdollGenerationStamp& expected,
		std::span<const PhysicsPoseExpectedBodyGeneration> expectedBodies) noexcept {
		const AnimationStatus basic = ValidatePhysicsPoseSnapshot(value, expected);
		if(basic != AnimationStatus::Ok) return basic;
		if(expectedBodies.empty() || expectedBodies.size() != value.bodies.size()) return AnimationStatus::StaleHandle;
		std::array<std::size_t, kRagdollSnapshotMaximumBodies> poseOrder{};
		std::array<std::size_t, kRagdollSnapshotMaximumBodies> expectedOrder{};
		BuildPhysicsPoseBodyOrder(value.bodies, poseOrder);
		BuildPhysicsPoseExpectedOrder(expectedBodies, expectedOrder);
		for(std::size_t index = 0U; index < value.bodies.size(); ++index) {
			const PhysicsPoseBodySnapshot& body = value.bodies[poseOrder[index]];
			const PhysicsPoseExpectedBodyGeneration& expectedBody = expectedBodies[expectedOrder[index]];
			if(expectedBody.bodyId == 0U || expectedBody.bodyGeneration == 0U
				|| (index != 0U && expectedBodies[expectedOrder[index - 1U]].bodyId == expectedBody.bodyId)
				|| expectedBody.bodyId != body.bodyId || expectedBody.bodyGeneration != body.bodyGeneration) return AnimationStatus::StaleHandle;
		}
		return AnimationStatus::Ok;
	}
}
