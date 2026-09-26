// LocomotionReplaySystem.h - validation-only replay at the movement boundary.
#pragma once

#include "Core/SystemBase.h"
#include "Physics/CharacterMotionTrace.h"
#include "Locomotion/LocomotionParameterTrace.h"

#include <memory>
#include <span>

namespace TerraEngine::Locomotion::Validation {
	struct ReplayPublicationResult {
		std::uint64_t frame = 0U;
		std::size_t accepted = 0U;
		std::size_t rejected = 0U;
		std::string failure;
	};

	// A control run omits the ordinary locomotion coordinator and feeds recorded
	// typed targets through the same final Physics lease/interval ingress. Callers
	// must remap complete identities to the current world before enqueueing.
	class TERRAENGINE_API LocomotionReplaySystem final : public SystemsBase {
	public:
		static LocomotionReplaySystem& Instance();
		void Initialize() override;
		void Shutdown() override;
		void Update(float deltaTime) override;
		std::string SystemName() override;
		Scheduling::SystemDeclaration SchedulerDeclaration() override;
		// Exactly one bounded frame may wait. A rejected frame never replaces the
		// previous pending frame; the harness treats any rejection as invalid timing.
		bool QueueFrame(std::uint64_t frame, std::span<const Physics::CharacterMotionTraceRecord> records,
			std::span<const LocomotionParameterTraceRecord> parameters = {});
		ReplayPublicationResult ReadResult() const;
		// Called only by the second declared protected phase, after Animation.
		void PublishRootIntervals();
	private:
		LocomotionReplaySystem();
		~LocomotionReplaySystem() override;
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
	class TERRAENGINE_API LocomotionReplayRootSystem final : public SystemsBase {
	public:
		static LocomotionReplayRootSystem& Instance();
		void Initialize() override {}
		void Shutdown() override {}
		void Update(float) override { LocomotionReplaySystem::Instance().PublishRootIntervals(); }
		std::string SystemName() override { return "LocomotionReplayRootValidationSystem"; }
		Scheduling::SystemDeclaration SchedulerDeclaration() override;
	};
}
