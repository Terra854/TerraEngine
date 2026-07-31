#include "../TestHarness.h"

#include "EditorAutomationService.h"

namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryCapabilityAutomationTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Editor.Automation.CapabilityEvidenceQueryIsReadOnly",
			[](TestContext &context)
			{
				TerraEngine::Editor::Automation::EditorAutomationService service;
				const auto *capability = service.FindCapability("renderer.virtualGeometry.capabilityEvidence");
				context.Expect(capability != nullptr, "Automation discovery must expose the virtual-geometry capability/evidence query.");
				if (capability == nullptr)
				{
					return;
				}
				context.Expect(capability->mutationMode == TerraEngine::Editor::Automation::MutationMode::ReadOnly,
					"Capability/timing evidence is an immutable renderer query.");
				context.Expect(!capability->mutatesState && !capability->requiresProject && !capability->requiresScene,
					"Renderer evidence queries must remain usable without authoring mutations.");
			});

		registry.Add("VirtualGeometry.Editor.Automation.CapabilityEvidenceRejectsInvalidView",
			[](TestContext &context)
			{
				TerraEngine::Editor::Automation::EditorAutomationService service;
				const nlohmann::json result = service.HandleTool("renderer.virtualGeometry.capabilityEvidence",
					{ { "viewType", "Shadow" } },
					TerraEngine::Editor::Automation::WriteMode::ReadOnly,
					"vg-capability-invalid-view");
				context.Expect(!result.value("ok", true), "Unsupported view selectors must fail before a renderer query is attempted.");
				context.ExpectEqual(std::string("validation"),
					result["error"].value("category", std::string()),
					"Invalid selectors must return the standard validation envelope.");
			});

		registry.Add("VirtualGeometry.Editor.Automation.CapabilityEvidenceReportsMissingRenderer",
			[](TestContext &context)
			{
				TerraEngine::Editor::Automation::EditorAutomationService service;
				const nlohmann::json result = service.HandleTool("renderer.virtualGeometry.capabilityEvidence",
					{ { "viewType", "Game" } },
					TerraEngine::Editor::Automation::WriteMode::ReadOnly,
					"vg-capability-no-renderer");
				context.Expect(!result.value("ok", true), "A query without a live renderer must not fabricate an empty success record.");
				context.ExpectEqual(std::string("missing_target"),
					result["error"].value("category", std::string()),
					"Missing live renderer state must use the standard missing-target envelope.");
			});

		registry.Add("VirtualGeometry.Editor.Automation.ValidationRoutesAreExplicitAndFailClosed",
			[](TestContext &context)
			{
				TerraEngine::Editor::Automation::EditorAutomationService service;
				const auto *setRoute = service.FindCapability("renderer.virtualGeometry.validationRoute.set");
				const auto *resetRoute = service.FindCapability("renderer.virtualGeometry.validationRoute.reset");
				context.Expect(setRoute != nullptr && resetRoute != nullptr
					&& setRoute->mutationMode == TerraEngine::Editor::Automation::MutationMode::EditorSelection
					&& resetRoute->mutationMode == TerraEngine::Editor::Automation::MutationMode::EditorSelection,
					"Route controls must be transient editor state rather than project-authored renderer settings.");
				const nlohmann::json invalid = service.HandleTool(
					"renderer.virtualGeometry.validationRoute.set",
					{{"route", "Fallback"}},
					TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
					"vg-route-invalid");
				context.Expect(!invalid.value("ok", true)
					&& invalid["error"].value("category", std::string()) == "validation",
					"Fallback and fake route names must be rejected before renderer state changes.");
				const nlohmann::json unavailable = service.HandleTool(
					"renderer.virtualGeometry.validationRoute.set",
					{{"route", "Hardware"}},
					TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
					"vg-route-no-renderer");
				context.Expect(!unavailable.value("ok", true)
					&& unavailable["error"].value("category", std::string()) == "missing_target",
					"A hardware route request without the installed Vulkan renderer must fail closed.");
			});

		registry.Add("VirtualGeometry.Editor.Automation.AuthoritativePickHasAsyncContract",
			[](TestContext &context)
			{
				TerraEngine::Editor::Automation::EditorAutomationService service;
				const auto *pick = service.FindCapability("renderer.virtualGeometry.authoritativePick");
				context.Expect(pick != nullptr
					&& pick->mutationMode == TerraEngine::Editor::Automation::MutationMode::ReadOnly,
					"Authoritative pick polling must remain a read-only automation capability.");
				const nlohmann::json malformed = service.HandleTool(
					"renderer.virtualGeometry.authoritativePick",
					{{"pixel", nlohmann::json::array({1u})}},
					TerraEngine::Editor::Automation::WriteMode::ReadOnly,
					"vg-pick-malformed");
				context.Expect(!malformed.value("ok", true)
					&& malformed["error"].value("category", std::string()) == "validation",
					"Identity pick requests need an exact two-component pixel before querying Vulkan.");
			});
	}
} // namespace TerraEngine::EditorTests
