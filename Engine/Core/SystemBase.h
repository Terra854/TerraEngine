// SystemBase.h — Base interface for all modular engine subsystems.
//
// This file defines the SystemsBase class, which provides the standard lifecycle
// hooks (Initialize, Update, Shutdown) required for any engine system. By
// inheriting from this class, subsystems can be registered with and managed by
// the MainEngine loop in a consistent manner.

#pragma once

#include "Export.h"
#include "Scene/SceneMutationQueue.h"
#include "Scheduling/SystemScheduler.h"
#include <string>
#include <utility>
#include <vector>

class ObjectManager;

class TERRAENGINE_API SystemsBase {
public:
	SystemsBase() {};
	virtual ~SystemsBase() = default;

	// System Initialization
	virtual void Initialize() = 0;

	// System Update with Delta Time
	virtual void Update(float deltaTime) = 0;

	// System freeing resources
	virtual void Shutdown() = 0;

	// Systems can report a terminal condition without teaching MainEngine about
	// subsystem-specific states. The engine checks this only after the protected
	// frame has completed, so implementations must not mutate shared frame data.
	virtual bool RequestsEngineShutdown() const {
		return false;
	}

	// For debugging, getting System Name
	// SystemName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual std::string SystemName() {
		return "Unnamed system (If you are seeing this, you need to implement SystemName() inside the system)";
	};

	// SchedulerDeclaration returns conservative metadata when a system has not
	// been given an explicit phase/resource contract yet. Keeping the fallback
	// main-thread and PostFrame-bound preserves current single-threaded behavior
	// while diagnostics tell contributors which systems still need real metadata.
	virtual TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() {
		TerraEngine::Scheduling::SystemDeclaration declaration;
		declaration.name = SystemName();
		declaration.phase = "PostFrame";
		declaration.mainThread = true;
		declaration.profilingLabel = declaration.name;
		declaration.usesDefaultMetadata = true;
		return declaration;
	}

	// DeclareFrameWorkloads lets a system split its frame work into smaller
	// snapshot, worker-preparation, and protected-commit phases. Returning an
	// empty list keeps the legacy behavior where the scheduler wraps Update().
	virtual std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float) {
		return {};
	}

	// Systems submit scene-owned object/component mutations through the shared
	// transaction queue instead of mutating live state from worker code. The
	// queue performs protected validation and commit at the approved flush point.
	void SubmitSceneTransaction(TerraEngine::Scene::Transaction transaction) {
		TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(std::move(transaction));
	}

	TerraEngine::Scene::TransactionResult PreflightSceneTransaction(const TerraEngine::Scene::Transaction& transaction, ObjectManager& objects) const {
		return TerraEngine::Scene::SceneMutationQueue::Instance().PreflightTransaction(transaction, objects);
	}
};
