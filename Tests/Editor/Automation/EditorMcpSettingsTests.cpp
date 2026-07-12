#include "../TestHarness.h"

#include "EditorAutomationService.h"
#include "EditorPreferences.h"
#include "ProjectSettings.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		std::optional<AutomationCapabilitySnapshot> FindSettingCapability(
			const EditorAutomationService& service,
			std::string_view name)
		{
			return service.GetRegistry().Find(name, 1, service.BuildAutomationStateSnapshot());
		}

		bool SchemaHasProperty(const nlohmann::json& schema, std::string_view property)
		{
			return schema.contains("properties")
				&& schema["properties"].is_object()
				&& schema["properties"].contains(std::string(property));
		}
	}

	void RegisterEditorMcpSettingsTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.Settings.DiscoverySeparatesProjectAndEditorScopes", [](TestContext& context) {
			EditorAutomationService service;
			const std::array<std::string_view, 4> operations{
				"settings.project.get",
				"settings.project.set",
				"settings.editor.get",
				"settings.editor.set"
			};

			for (const std::string_view operation : operations)
			{
				const auto capability = FindSettingCapability(service, operation);
				context.Expect(capability.has_value(), std::string(operation) + " should be registered by the settings provider.");
				if (capability.has_value())
					context.ExpectEqual(std::string("settings"), capability->descriptor.category, "Settings tools should share a stable category.");
			}
		});

		registry.Add("Editor.MCP.Settings.AllowlistAndTypeContractsAreDiscoverable", [](TestContext& context) {
			EditorAutomationService service;
			for (const std::string_view operation : {"settings.project.get", "settings.project.set", "settings.editor.get", "settings.editor.set"})
			{
				const auto capability = FindSettingCapability(service, operation);
				context.Expect(capability.has_value(), std::string(operation) + " should be discoverable.");
				if (!capability.has_value())
					continue;

				context.Expect(SchemaHasProperty(capability->descriptor.inputSchema, "key"), "Every setting operation should select an allowlisted semantic key.");
				context.Expect(SchemaHasProperty(capability->descriptor.resultSchema, "allowedKeys"), "Settings results should disclose the scope's typed allowlist.");
				if (operation.ends_with(".set"))
				{
					context.Expect(SchemaHasProperty(capability->descriptor.inputSchema, "value"), "Setting writes should validate a typed value.");
					context.Expect(SchemaHasProperty(capability->descriptor.resultSchema, "persisted"), "Setting writes should report whether persistence completed.");
				}
			}
		});

		registry.Add("Editor.MCP.Settings.GlobalWritesRequireHighImpactApproval", [](TestContext& context) {
			EditorAutomationService service;
			const auto projectSet = FindSettingCapability(service, "settings.project.set");
			const auto editorSet = FindSettingCapability(service, "settings.editor.set");
			context.Expect(projectSet.has_value() && editorSet.has_value(), "Both setting write scopes should be registered.");

			if (projectSet.has_value())
			{
				context.Expect(projectSet->descriptor.permission == AutomationPermission::ProjectWrite, "Project settings should require project-write permission.");
				context.Expect(projectSet->descriptor.approvalPolicy == AutomationApprovalPolicy::None, "Ordinary allowlisted project settings should not require global approval.");
				context.Expect(projectSet->descriptor.availability.requiresProject, "Project settings should be unavailable without a project.");
			}
			if (editorSet.has_value())
			{
				context.Expect(editorSet->descriptor.permission == AutomationPermission::GlobalWrite, "Editor preferences should require global-write permission.");
				context.Expect(editorSet->descriptor.approvalPolicy == AutomationApprovalPolicy::HighImpact, "Editor preference changes must require high-impact approval.");
			}
		});

		registry.Add("Editor.MCP.Settings.ForbiddenFieldsCannotUseArbitraryObjectWrites", [](TestContext& context) {
			EditorAutomationService service;
			const auto projectSet = FindSettingCapability(service, "settings.project.set");
			const auto editorSet = FindSettingCapability(service, "settings.editor.set");
			if (projectSet.has_value())
			{
				context.Expect(!SchemaHasProperty(projectSet->descriptor.inputSchema, "path"), "Project settings must not expose arbitrary settings-file paths.");
				context.Expect(!SchemaHasProperty(projectSet->descriptor.inputSchema, "object"), "Project settings must not accept an unrestricted replacement object.");
			}
			if (editorSet.has_value())
			{
				context.Expect(!SchemaHasProperty(editorSet->descriptor.inputSchema, "path"), "Editor settings must not expose arbitrary preference-file paths.");
				context.Expect(!SchemaHasProperty(editorSet->descriptor.inputSchema, "object"), "Editor settings must not accept an unrestricted replacement object.");
			}
		});

		registry.Add("Editor.MCP.Settings.TypedAdaptersRejectWrongTypesWithoutPartialMutation", [](TestContext& context) {
			ProjectSettings project;
			project.projectDisplayName = "Before";
			std::string error;
			context.Expect(!TerraEngine::Editor::Settings::WriteProjectAutomationSetting(project, "projectDisplayName", 42, error), "Project setting adapters should reject the wrong JSON type.");
			context.ExpectEqual(std::string("Before"), project.projectDisplayName, "Rejected project setting writes must leave the typed object unchanged.");

			EditorPreferences editor;
			editor.autoBuildScripts = true;
			error.clear();
			context.Expect(!TerraEngine::Editor::Settings::WriteEditorAutomationSetting(editor, "autoBuildScripts", "false", error), "Editor setting adapters should reject string values for boolean preferences.");
			context.Expect(editor.autoBuildScripts, "Rejected editor setting writes must leave the typed object unchanged.");
			context.Expect(TerraEngine::Editor::Settings::WriteEditorAutomationSetting(editor, "autoBuildScripts", false, error), "An allowlisted boolean editor setting should update through the typed adapter.");
			context.Expect(!editor.autoBuildScripts, "A valid typed editor setting update should be observable before persistence.");
		});
	}
}
