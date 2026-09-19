// NavigationAgentCommandSystem.h - early protected admission for agent commands.
//
// This small coordinator exists separately from NavigationAgentSystem because a
// fresh destination must reach NavigationSystem's capture in the same frame,
// while guidance still has to consume Navigation's published results afterward.

#pragma once

#include "Core/SystemBase.h"

namespace TerraEngine::Navigation {
	class TERRAENGINE_API NavigationAgentCommandSystem final : public SystemsBase {
	public:
		static NavigationAgentCommandSystem& Instance();

		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

	private:
		NavigationAgentCommandSystem() = default;
		bool initialized_ = false;
	};
}
