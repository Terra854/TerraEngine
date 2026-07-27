// AnimationGraphInstance.cpp - Copy-on-publish program and inspection snapshot owner.

#include "Animation/Runtime/AnimationGraphInstance.h"

#include <atomic>
#include <new>

namespace TerraEngine::Animation {
namespace {
AnimationStatus BuildInspectionSnapshot(const EvaluationProgram &program, AnimationGeneration generation,
	const StateMachineSnapshot &stateMachine, std::shared_ptr<const AnimationGraphInspectionSnapshot> &output) {
	try {
		auto inspection = std::make_shared<AnimationGraphInspectionSnapshot>();
		inspection->programGeneration = generation;
		inspection->programStableHash = program.stableHash;
		inspection->stateMachine = stateMachine;
		output = std::shared_ptr<const AnimationGraphInspectionSnapshot>(std::move(inspection));
		return AnimationStatus::Ok;
	} catch (const std::bad_alloc &) {
		return AnimationStatus::InternalError;
	}
}

bool IsRestorableSnapshot(const EvaluationProgram &program, const AnimationGraphInspectionSnapshot &inspection,
	AnimationGeneration expectedGeneration) {
	return inspection.programGeneration == expectedGeneration && inspection.programStableHash == program.stableHash &&
		ValidateStateMachineSnapshot(program, inspection.stateMachine) == AnimationStatus::Ok;
}
} // namespace

AnimationStatus AnimationGraphInstance::Publish(const EvaluationProgram &program, AnimationGeneration generation) {
	if (generation == 0U || generation <= generation_) {
		return AnimationStatus::StaleHandle;
	}
	const AnimationStatus programStatus = ValidateStateMachineProgram(program);
	if (programStatus != AnimationStatus::Ok) {
		return programStatus == AnimationStatus::InternalError ? AnimationStatus::InternalError : AnimationStatus::StaleHandle;
	}
	try {
		// Copy and allocate the reader-visible object first. The final move/store
		// sequence cannot allocate, so a failed publication preserves all state.
		EvaluationProgram stagedProgram = program;
		StateMachineSnapshot stagedState{};
		std::shared_ptr<const AnimationGraphInspectionSnapshot> stagedInspection;
		const AnimationStatus inspectionStatus = BuildInspectionSnapshot(stagedProgram, generation, stagedState, stagedInspection);
		if (inspectionStatus != AnimationStatus::Ok) {
			return inspectionStatus;
		}
		program_ = std::move(stagedProgram);
		generation_ = generation;
		snapshot_ = std::move(stagedState);
		std::atomic_store_explicit(&inspectionSnapshot_, std::move(stagedInspection), std::memory_order_release);
		return AnimationStatus::Ok;
	} catch (const std::bad_alloc &) {
		return AnimationStatus::InternalError;
	}
}

AnimationStatus AnimationGraphInstance::Step(const StateMachineStepInput &input) {
	if (generation_ == 0U) {
		return AnimationStatus::NotReady;
	}
	StateMachineSnapshot staged;
	const AnimationStatus status = StepStateMachineTrusted(program_, snapshot_, input, staged);
	if (status != AnimationStatus::Ok) {
		return status;
	}
	std::shared_ptr<const AnimationGraphInspectionSnapshot> stagedInspection;
	const AnimationStatus inspectionStatus = BuildInspectionSnapshot(program_, generation_, staged, stagedInspection);
	if (inspectionStatus != AnimationStatus::Ok) {
		return inspectionStatus;
	}
	snapshot_ = std::move(staged);
	std::atomic_store_explicit(&inspectionSnapshot_, std::move(stagedInspection), std::memory_order_release);
	return AnimationStatus::Ok;
}

AnimationStatus AnimationGraphInstance::Restore(const AnimationGraphInspectionSnapshot &inspection) {
	if (generation_ == 0U) {
		return AnimationStatus::NotReady;
	}
	if (!IsRestorableSnapshot(program_, inspection, generation_)) {
		return AnimationStatus::Rejected;
	}
	try {
		StateMachineSnapshot staged = inspection.stateMachine;
		std::shared_ptr<const AnimationGraphInspectionSnapshot> stagedInspection;
		const AnimationStatus inspectionStatus = BuildInspectionSnapshot(program_, generation_, staged, stagedInspection);
		if (inspectionStatus != AnimationStatus::Ok) {
			return inspectionStatus;
		}
		snapshot_ = std::move(staged);
		std::atomic_store_explicit(&inspectionSnapshot_, std::move(stagedInspection), std::memory_order_release);
		return AnimationStatus::Ok;
	} catch (const std::bad_alloc &) {
		return AnimationStatus::InternalError;
	}
}

StateMachineSnapshot AnimationGraphInstance::Snapshot() const {
	const std::shared_ptr<const AnimationGraphInspectionSnapshot> inspection = InspectionSnapshot();
	return inspection != nullptr ? inspection->stateMachine : StateMachineSnapshot{};
}

std::shared_ptr<const AnimationGraphInspectionSnapshot> AnimationGraphInstance::InspectionSnapshot() const {
	return std::atomic_load_explicit(&inspectionSnapshot_, std::memory_order_acquire);
}
} // namespace TerraEngine::Animation
