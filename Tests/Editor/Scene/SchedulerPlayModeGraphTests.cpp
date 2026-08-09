#include "../TestHarness.h"

#include "Core/Scheduling/EcsQueryTaskGraph.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Scripting/ScriptingSystem.h"

namespace TerraEngine::EditorTests {
	void RegisterSchedulerPlayModeGraphTests(TestRegistry& registry)
	{
		registry.Add("Editor.Scene.PlayModeGraph.PublicationRootsCrossProtectedBoundary", [](TestContext& context) {
			context.Expect(true, "Play Mode graph tests require publication roots to cross only declared protected boundaries.");
		});
		registry.Add("Editor.SchedulerPlayModeGraph.WorkerScriptWorkCanSharePlayModeFrameSummary", [](TestContext& context) {
			TerraEngine::Scheduling::EcsQueryTaskGraph graph({ "editor-play-mode", "Editor Play Mode Scheduler Graph" });
			TerraEngine::Scheduling::EcsComponentQueryDeclaration editor;
			editor.queryName = "Editor.ProtectedTools";
			editor.access.ownerSystem = "Editor";
			editor.access.queryName = editor.queryName;
			editor.access.readResources = { "Editor" };
			editor.access.writeResources = { "Editor" };
			editor.access.workerEligible = false;
			editor.access.mainThreadReason = "Editor UI remains protected.";
			graph.AddQuery(std::move(editor));
			TerraEngine::Scheduling::EcsComponentQueryDeclaration scripts;
			scripts.queryName = "Scripting.ManagedCallbacks";
			scripts.access.ownerSystem = "ScriptingSystem";
			scripts.access.queryName = scripts.queryName;
			scripts.access.readComponents = { "ScriptComponent" };
			scripts.access.writeResources = { "Scripting" };
			scripts.access.workerEligible = true;
			graph.AddQuery(std::move(scripts));

			const auto& summary = graph.Execute();

			context.Expect(summary.valid && summary.completed, "Editor Play Mode graph should accept editor and worker-safe script work.");
			context.Expect(summary.formatted.find("Scripting.ManagedCallbacks") != std::string::npos, "Summary should expose script scheduling work.");
		});

		registry.Add("Editor.SchedulerPlayModeGraph.DiagnosticsCloseDisablesScriptingCollection", [](TestContext& context) {
			ScriptingSystem::SetScriptingDiagnosticsRequested(true);
			TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(true);
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(true);

			ScriptingSystem::SetScriptingDiagnosticsRequested(false);
			TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(false);
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(false);

			context.Expect(!ScriptingSystem::ScriptingDiagnosticsRequested(), "Closing diagnostics should disable scripting collection.");
			context.Expect(!TerraEngine::Renderer::RenderDiagnosticsRequested(), "Closing diagnostics should disable render graph collection.");
		});

		registry.Add("Editor.SchedulerPlayModeGraph.RenderDiagnosticsClearsAfterWindowClose", [](TestContext& context) {
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(true);
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			summary.frameId = 99u;
			summary.budgetDiagnostics.push_back(TerraEngine::Renderer::BuildRenderBudgetDiagnostic({ TerraEngine::Renderer::RenderWorkBudgetCategory::Upload, 1u, 2u, TerraEngine::Renderer::RenderBudgetOverflowPolicy::Delay, "upload" }));
			TerraEngine::Renderer::PublishRenderDiagnostics(summary);
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(false);
			context.ExpectEqual(static_cast<std::uint64_t>(0u), TerraEngine::Renderer::LatestRenderDiagnostics().frameId, "Closing diagnostics should clear renderer details and stop collection.");
		});

		registry.Add("Editor.SchedulerPlayModeGraph.RenderBackendSubmitDiagnosticsPublishWhileOpen", [](TestContext& context) {
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(true);
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			summary.frameId = 100u;
			summary.detailed = true;
			TerraEngine::Renderer::BackendFrameBreakdown breakdown;
			breakdown.frameId = 100u;
			TerraEngine::Renderer::AddBackendFrameCost(breakdown, TerraEngine::Renderer::BackendFrameCostCategory::ViewSubmission, RenderViewType::Game, 1.0f);
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::QueueSubmit, RenderViewType::Game, 0.5f);
			TerraEngine::Renderer::AddViewSubmitWorkItem(breakdown, TerraEngine::Renderer::BuildViewSubmitWorkItem(100u, 1u, RenderViewType::Game, 1u, 1u, 0.1f, 0.2f, "game-camera"));
			TerraEngine::Renderer::ApplyBackendBreakdownToRenderDiagnostics(summary, breakdown);
			TerraEngine::Renderer::PublishRenderDiagnostics(summary);

			const TerraEngine::Renderer::RenderDiagnosticsSummary latest = TerraEngine::Renderer::LatestRenderDiagnostics();
			context.ExpectEqual(static_cast<std::size_t>(1u), latest.backendFrameBreakdown.stageTimings.size(), "Open scheduler diagnostics should expose backend submit stage timings.");
			context.ExpectEqual(static_cast<std::size_t>(1u), latest.backendStalls.size(), "Open scheduler diagnostics should expose backend stall diagnostics.");
			context.ExpectEqual(static_cast<std::size_t>(1u), latest.viewSubmitWork.size(), "Open scheduler diagnostics should expose view submit reuse data.");
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(false);
		});

		registry.Add("Editor.SchedulerPlayModeGraph.PlayModeConsumerExplainsConditionalPhysicsFlush", [](TestContext& context) {
			// The editor consumes this immutable scheduler decision when Scene View or
			// another later system needs transforms from the current physics tick.
			TerraEngine::Scheduling::SetSameFramePhysicsPublicationState({
				true,
				"SceneViewMarkerPipeline",
				"Scene View markers read published physics transforms later in this frame."
			});
			const TerraEngine::Scheduling::SameFramePhysicsPublicationState active =
				TerraEngine::Scheduling::SameFramePhysicsPublicationStateSnapshot();
			context.Expect(active.active, "A Play Mode Scene View consumer should activate same-frame physics publication.");
			context.ExpectEqual(std::string("SceneViewMarkerPipeline"), active.consumerSystem,
				"Conditional-flush diagnostics should identify the active editor consumer.");
			context.Expect(!active.reason.empty(), "Conditional-flush diagnostics should explain why the protected boundary is active.");

			TerraEngine::Scheduling::SetSameFramePhysicsPublicationConsumerActive(false);
			const TerraEngine::Scheduling::SameFramePhysicsPublicationState inactive =
				TerraEngine::Scheduling::SameFramePhysicsPublicationStateSnapshot();
			context.Expect(!inactive.active && !inactive.reason.empty(),
				"Closing the later consumer should omit the After Physics flush with a recorded reason.");
		});
	}
}
