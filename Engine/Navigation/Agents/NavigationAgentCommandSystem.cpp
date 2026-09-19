// NavigationAgentCommandSystem.cpp - declared early protected command boundary.

#include "NavigationAgentCommandSystem.h"

#include "Core/Scheduling/JobGraph.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "NavigationAgentSystem.h"

namespace TerraEngine::Navigation {
	NavigationAgentCommandSystem& NavigationAgentCommandSystem::Instance() {
		static NavigationAgentCommandSystem system;
		return system;
	}

	void NavigationAgentCommandSystem::Initialize() {
		// NavigationAgentSystem owns the queue and its runtime storage. This system
		// owns only scheduling/lifecycle participation, so initialization is idempotent.
		initialized_ = true;
	}

	void NavigationAgentCommandSystem::Update(float) {
		if(!initialized_) return;
		// Compatibility execution preserves the declared protected ordering because
		// MainEngine registers this system before NavigationSystem.
		NavigationAgentSystem::Instance().CaptureCommandAdmission();
	}

	void NavigationAgentCommandSystem::Shutdown() {
		initialized_ = false;
	}

	std::string NavigationAgentCommandSystem::SystemName() {
		return "NavigationAgentCommandSystem";
	}

	TerraEngine::Scheduling::SystemDeclaration NavigationAgentCommandSystem::SchedulerDeclaration() {
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration;
		declaration.name = SystemName();
		declaration.phase = "Navigation";
		declaration.before = {"NavigationSystem"};
		declaration.reads = {kResourceNavigationAgentCommands, kResourceNavigationAgentState,
			kResourceNavigationWorldRoot, kResourceNavigationResults,
			kResourceNavigationRouteValidity, "ObjectManager"};
		declaration.writes = {kResourceNavigationAgentCommands, kResourceNavigationAgentState,
			kResourceNavigationRequestQueue, kResourceNavigationResults,
			kResourceNavigationRouteValidity};
		declaration.mainThread = true;
		declaration.requiresFlushedSceneMutations = true;
		declaration.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
		declaration.boundaryClassification = HardBoundaryClassification::Protected;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "None; command admission validates live attachment and target state.";
		declaration.protectedCommit = "Drain copied agent commands, update generation-qualified runtime intent, and admit fresh destinations into Navigation's request queue.";
		declaration.protectedReason = "Object/component attachment, target pose, receipt completion, and route admission require one protected coordinator boundary.";
		declaration.profilingLabel = "NavigationAgentCommands";
		declaration.omitWhenNoFrameWorkloads = true;
		declaration.usesDefaultMetadata = false;
		return declaration;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration>
	NavigationAgentCommandSystem::DeclareFrameWorkloads(float) {
		using namespace TerraEngine::Scheduling;
		if(!initialized_) return {};
		auto capture = MakeProtectedCommitPhase("NavigationAgentCommandCapture", SystemName(),
			{kResourceNavigationAgentCommands, kResourceNavigationAgentState,
				kResourceNavigationWorldRoot, kResourceNavigationResults,
				kResourceNavigationRouteValidity, "ObjectManager"},
			{kResourceNavigationAgentCommands, kResourceNavigationAgentState,
				kResourceNavigationRequestQueue, kResourceNavigationResults,
				kResourceNavigationRouteValidity},
			"NavigationAgentCommandCapture");
		capture.phase = "Navigation";
		capture.stableDescriptorId = "NavigationAgentCommandSystem/Navigation/CommandCapture";
		capture.hardBoundary = true;
		capture.protectedCommit = "Admit copied fresh destinations before Navigation captures one bounded route packet.";
		capture.work = [](JobGraphContext&) {
			NavigationAgentSystem::Instance().CaptureCommandAdmission();
		};
		return {std::move(capture)};
	}
}
