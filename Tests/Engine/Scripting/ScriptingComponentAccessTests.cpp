#include "../TestHarness.h"

#include "Components/ScriptComponent.h"
#include "Components/Transform.h"
#include "Core/Object/Component.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Scripting/ScriptingSystem.h"
#include "ScriptingRuntimeTestFixtures.h"

namespace TerraEngine::Tests {
	void RegisterScriptingComponentAccessTests(TestRegistry& registry)
	{
		registry.Add("Scripting.ComponentAccess.ManagedCallbacksAreWorkerScheduled", [](TestContext& context) {
			const ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
			context.Expect(scriptingSystem.ManagedCallbacksRunProtected(), "Managed callbacks should remain behind the protected boundary.");
			context.Expect(!scriptingSystem.ManagedCallbacksRequireSchedulerAnnotations(), "C# scripts should not require author-side scheduler annotations.");
		});

		registry.Add("Scripting.ComponentAccess.SchedulerDeclarationReportsProtectedManagedBoundary", [](TestContext& context) {
			const ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
			const TerraEngine::Scheduling::SystemDeclaration declaration = const_cast<ScriptingSystem&>(scriptingSystem).SchedulerDeclaration();
			TerraEngine::Scheduling::FrameWorkloadDeclaration workload;
			workload.workloadName = declaration.name;
			workload.ownerSystem = declaration.name;
			workload.phase = declaration.phase;
			workload.reads = declaration.reads;
			workload.writes = declaration.writes;
			workload.workerEligible = !declaration.mainThread;
			workload.mainThreadRequired = declaration.mainThread;
			workload.protectedReason = scriptingSystem.GetSchedulerBoundarySummary();

			const TerraEngine::Scheduling::FrameExecutionPlan plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ workload }, "scripting-protected-boundary");

			context.Expect(plan.valid, "Scripting protected declaration should validate as a frame workload.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.protectedCount, "Scripting managed boundary should remain protected in migration summaries.");
			context.Expect(plan.formatted.find("C# gameplay callbacks") != std::string::npos, "Scripting protected fallback diagnostics should explain the managed boundary.");
		});

		registry.Add("Scripting.ComponentAccess.ComponentWrapperGenerationChangesOnStructureEdit", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			Object object = ObjectManager::instance().createObject("Cache Target");
			const std::uint64_t beforeGeneration = ObjectManager::instance().getObjectComponentGeneration(object);

			ObjectManager::instance().AddComponent<ScriptComponent>(object.EntityId());
			ObjectManager::instance().notifyObjectStructureChanged(object);
			const std::uint64_t afterGeneration = ObjectManager::instance().getObjectComponentGeneration(object);

			context.Expect(afterGeneration > beforeGeneration, "Managed component wrapper caches should have a native generation that changes after structural component edits.");
		});

		registry.Add("Scripting.ComponentAccess.ObjectNameCacheGenerationChangesOnRename", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			Object object = ObjectManager::instance().createObject("Cache Target");
			const std::uint64_t beforeGeneration = ObjectManager::instance().getObjectMetadataGeneration(object);

			ObjectManager::instance().setObjectName(object, "Renamed Cache Target");
			const std::uint64_t afterGeneration = ObjectManager::instance().getObjectMetadataGeneration(object);

			context.Expect(afterGeneration > beforeGeneration, "Managed Object.Name caches should invalidate when native object names change.");
		});

		registry.Add("Scripting.ComponentAccess.BindingPresenceGenerationChangesOnScriptBindingEdit", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			Object object = ObjectManager::instance().createObject("Binding Target");
			const std::uint64_t beforeGeneration = ObjectManager::instance().getObjectBindingGeneration(object);

			ScriptComponent& component = *ObjectManager::instance().AddComponent<ScriptComponent>(object.EntityId());
			component.AddBinding(TerraEngine::EMPTY_UUID, "Game.BindingProbe");
			ObjectManager::instance().notifyObjectStructureChanged(object);
			const std::uint64_t afterGeneration = ObjectManager::instance().getObjectBindingGeneration(object);

			context.Expect(afterGeneration > beforeGeneration, "Managed HasComponent caches for script bindings should invalidate when bindings change.");
		});

		registry.Add("Scripting.ComponentAccess.FrameReadViewCapturesLookupGeneration", [](TestContext& context) {
			ScriptingRuntimeTestScope scope;
			CreateScriptedRuntimeObject("Reader", "Game.Reader");

			const ScriptingSystem::ScriptUpdateWorkload workload = ScriptingSystem::Instance().BuildScriptUpdateWorkload(0.016f, false);
			const ScriptingSystem::ScriptFrameReadView readView = ScriptingSystem::Instance().BuildScriptFrameReadView(workload);

			context.ExpectEqual(workload.readViewVersion, readView.readViewVersion, "Read views should carry the workload generation used by workers.");
			context.ExpectEqual(ObjectManager::instance().GetSpatialRevision(), readView.lookupIndexGeneration, "Read views should record the lookup cache generation visible to scripts.");
			context.ExpectEqual(static_cast<std::size_t>(1), readView.scriptRecords.size(), "Read views should expose the script record workers are about to execute.");
			context.Expect(readView.scriptRecords.front().componentGeneration != 0, "Read views should carry component generations for managed wrapper caches.");
			context.Expect(readView.scriptRecords.front().bindingGeneration != 0, "Read views should carry binding generations for managed HasComponent caches.");
		});
	}
}
