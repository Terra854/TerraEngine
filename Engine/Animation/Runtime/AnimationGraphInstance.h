// AnimationGraphInstance.h - Worker-owned graph state with immutable inspection.

#pragma once

#include "Animation/Graph/StateMachineEvaluator.h"

#include <memory>

namespace TerraEngine::Animation {
struct AnimationGraphInspectionSnapshot {
	AnimationGeneration programGeneration = 0U;
	std::uint64_t programStableHash = 0U;
	StateMachineSnapshot stateMachine;
	auto operator<=>(const AnimationGraphInspectionSnapshot &) const = default;
};

class AnimationGraphInstance {
public:
	// Publish and Step are owned by one scheduler worker.  Readers must use the
	// immutable inspection pointer and never observe the mutable working copy.
	TERRAENGINE_API AnimationStatus Publish(const EvaluationProgram &program, AnimationGeneration generation);
	TERRAENGINE_API AnimationStatus Step(const StateMachineStepInput &input);
	TERRAENGINE_API AnimationStatus Restore(const AnimationGraphInspectionSnapshot &inspection);

	TERRAENGINE_API StateMachineSnapshot Snapshot() const;
	TERRAENGINE_API std::shared_ptr<const AnimationGraphInspectionSnapshot> InspectionSnapshot() const;

private:
	EvaluationProgram program_{};
	AnimationGeneration generation_ = 0U;
	StateMachineSnapshot snapshot_{};
	std::shared_ptr<const AnimationGraphInspectionSnapshot> inspectionSnapshot_{};
};
} // namespace TerraEngine::Animation
