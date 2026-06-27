#include "../TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"
#include "Scene/RuntimeLayout.h"

namespace TerraEngine::Tests {
	void RegisterRenderRuntimeLayoutTests(TestRegistry& registry)
	{
		registry.Add("Engine.RuntimeLayout.Render.RecordKindsFormatDeterministically", [](TestContext& context) {
			context.ExpectEqual(std::string("camera"), std::string(TerraEngine::Scene::ToString(TerraEngine::Scene::RuntimeLayoutRecordKind::Camera)), "Camera record kind should be stable.");
			context.ExpectEqual(std::string("renderable"), std::string(TerraEngine::Scene::ToString(TerraEngine::Scene::RuntimeLayoutRecordKind::Renderable)), "Renderable record kind should be stable.");
			context.ExpectEqual(std::string("light"), std::string(TerraEngine::Scene::ToString(TerraEngine::Scene::RuntimeLayoutRecordKind::Light)), "Light record kind should be stable.");
		});

		registry.Add("Engine.RuntimeLayout.Render.DiagnosticsCarryRecordCountsAndRejections", [](TestContext& context) {
			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			summary.frameId = 7;
			summary.runtimeLayoutRenderRecordCount = 12;
			summary.runtimeLayoutRejectedRecordCount = 2;
			summary.runtimeLayoutRejectionReasons = { "missing-resource", "inactive" };

			context.ExpectEqual(static_cast<std::size_t>(12), summary.runtimeLayoutRenderRecordCount, "Render diagnostics should expose accepted compact record count.");
			context.ExpectEqual(static_cast<std::size_t>(2), summary.runtimeLayoutRejectedRecordCount, "Render diagnostics should expose rejected compact record count.");
			context.ExpectEqual(static_cast<std::size_t>(2), summary.runtimeLayoutRejectionReasons.size(), "Render diagnostics should preserve stable rejection reasons.");
		});
	}
}
