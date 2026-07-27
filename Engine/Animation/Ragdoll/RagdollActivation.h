// RagdollActivation.h - All-or-nothing animation-to-physics handoff preparation.
#pragma once
#include "Animation/Assets/RagdollProfileAsset.h"
#include "Animation/Ragdoll/PhysicsPoseSnapshot.h"
namespace TerraEngine::Animation {
	struct RagdollActivationBody {
		std::uint64_t bodyId = 0U;
		std::uint64_t stableJointId = 0U;
		// The source record is a complete animation-space handoff. Applying the
		// profile's joint-to-body transform here produces the provider world pose
		// once, so all bodies enter physics from one sealed animation generation.
		AnimationTransform worldPose{};
		AnimationFloat3 linearVelocity{};
		AnimationFloat3 angularVelocity{};
		auto operator<=>(const RagdollActivationBody&) const = default;
	};
	struct RagdollActivationPlan {
		RagdollGenerationStamp generation{};
		std::vector<RagdollActivationBody> bodies;
	};
	TERRAENGINE_API AnimationStatus PrepareRagdollActivation(const PhysicsPoseSnapshot& completeAnimationPose, const RagdollGenerationStamp& expected, RagdollActivationPlan& output) noexcept;
	TERRAENGINE_API AnimationStatus PrepareRagdollActivation(
		const PhysicsPoseSnapshot& completeAnimationPose,
		const RagdollProfileAsset& profile,
		const RagdollGenerationStamp& expected,
		RagdollActivationPlan& output) noexcept;
	// When two complete animation publications are available, derive body-origin
	// motion from their mapped world poses. This preserves both object motion and
	// joint-to-body offsets instead of starting a moving character at zero velocity.
	TERRAENGINE_API AnimationStatus PrepareRagdollActivation(
		const PhysicsPoseSnapshot& completeAnimationPose,
		const PhysicsPoseSnapshot& previousAnimationPose,
		float elapsedSeconds,
		const RagdollProfileAsset& profile,
		const RagdollGenerationStamp& expected,
		RagdollActivationPlan& output) noexcept;
}
