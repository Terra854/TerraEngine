#include "../TestHarness.h"

#include "Components/SkyComponent.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "EditorAutomationService.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Physics/PhysicsDiagnostics.h"

#include <memory>
#include <vector>

namespace TerraEngine::EditorTests {
	void RegisterFullParallelFrameDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Editor.FullParallelFrame.DiagnosticsExposeExecutionMode", [](TestContext& context) {
			TerraEngine::Scheduling::FrameWorkloadDeclaration workload;
			workload.workloadName = "EditorReflectionPreflight";
			workload.ownerSystem = "EditorReflection";
			workload.phase = "Editor";
			workload.reads = { "EditorSelectionSnapshot" };
			workload.workerEligible = true;
			workload.mainThreadRequired = false;
			workload.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare;

			const auto plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ workload }, "editor-parallel-diagnostics");

			context.Expect(plan.valid, "Editor worker-preflight diagnostics should validate.");
			context.Expect(plan.formatted.find("mode=parallel") != std::string::npos, "Editor diagnostics should expose parallel execution mode.");
			context.Expect(plan.formatted.find("workerUtilization=") != std::string::npos, "Editor diagnostics should include worker utilization field.");
		});

		registry.Add("Editor.FullParallelFrame.ReflectionPreflightValidatesBeforeProtectedCommit", [](TestContext& context) {
			ObjectManager::instance().clear();
			SkyComponent::registerType();
			ObjectManager& objects = ObjectManager::instance();
			Object object = objects.createObject("Reflection Target");
			(void)objects.AddComponent<SkyComponent>(object);

			TerraEngine::Editor::Automation::EditorAutomationService service;
			const auto preflight = service.PreflightReflectionMutation({
				{"object", objects.getObjectName(object)},
				{"component", "SkyComponent"},
				{"property", "sourceAsset"},
				{"value", "Textures/Sky/WorkerSafe.tetf"}
			});

			context.Expect(preflight.valid, "Reflection mutation preflight should validate target/component/property/payload data.");
			context.ExpectEqual(std::string("protected"), preflight.classification, "Reflection mutation commit should stay protected.");
			context.Expect(preflight.safeSubwork.find("validation") != std::string::npos, "Preflight should name validation as the worker-safe subwork.");
			context.Expect(preflight.protectedCommit.find("undo stack") != std::string::npos, "Protected commit should name editor undo/dirty side effects.");
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.FullParallelFrame.ReflectionMutationAppliesOnlyThroughProtectedHandler", [](TestContext& context) {
			ObjectManager::instance().clear();
			SkyComponent::registerType();
			ObjectManager& objects = ObjectManager::instance();
			Object object = objects.createObject("Reflection Commit");
			(void)objects.AddComponent<SkyComponent>(object);

			TerraEngine::Editor::Automation::EditorAutomationService service;
			const nlohmann::json result = service.HandleTool(
				"component.set_property",
				{
					{"object", objects.getObjectName(object)},
					{"component", "SkyComponent"},
					{"property", "sourceAsset"},
					{"value", "Textures/Sky/ProtectedCommit.tetf"}
				},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"reflection-protected");

			context.Expect(result.value("ok", false), "Protected reflection handler should apply validated property edits.");
			const SkyComponent* component = objects.GetComponent<SkyComponent>(object);
			context.Expect(component != nullptr, "Edited object should still own the sky component.");
			context.ExpectEqual(std::string("Textures/Sky/ProtectedCommit.tetf"), component->sourceAsset, "Protected reflection handler should update the reflected property.");
			ObjectManager::instance().clear();
		});

		registry.Add("Editor.FullParallelFrame.InactiveDiagnosticsDoNotConsumeForeignDebugPackets", [](TestContext& context) {
			// Packet selection is the cross-window boundary: an inactive diagnostics
			// view must neither request provider work nor expose another world's data.
			TerraEngine::Physics::PhysicsDebugPacketHistory history{2U};
			const TerraEngine::Physics::PhysicsWorldHandle activeWorld{{700U}, 1U};
			const TerraEngine::Physics::PhysicsWorldHandle inactiveWorld{{701U}, 1U};
			auto packet = std::make_shared<TerraEngine::Physics::PhysicsDebugPacket>();
			packet->world = activeWorld;
			packet->producedTick = 3U;
			packet->expiryTick = 8U;
			packet->lines = std::make_shared<std::vector<TerraEngine::Physics::PhysicsDebugLine>>(4U);
			history.Publish(std::move(packet));

			context.Expect(history.Select(inactiveWorld, 3U).empty(),
				"An inactive or unrelated editor window must not consume debug packets published by the active physics world.");
			TerraEngine::Physics::PhysicsDebugDisplaySelection disabled;
			disabled.showLines = false;
			disabled.showTriangles = false;
			disabled.showContacts = false;
			disabled.showConstraints = false;
			disabled.showCharacterSupport = false;
			disabled.showRecentQueries = false;
			const auto selected = history.Select(activeWorld, 3U, disabled);
			context.Expect(selected.size() == 1U && selected.front()->lines && selected.front()->lines->size() == 4U,
				"Disabling diagnostics must change only presentation selection; it must not mutate or republish the immutable physics packet.");
		});
	}
}
