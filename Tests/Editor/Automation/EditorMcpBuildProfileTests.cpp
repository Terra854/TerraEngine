#include "../TestHarness.h"

#include "EditorAutomationService.h"
#include "BuildProfile.h"
#include "EditorContext.h"
#include "ProjectManager.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		// Build-profile tests use discovery as the public contract. The owning
		// service may change internally without forcing MCP clients to know which
		// window currently presents profiles.
		std::optional<AutomationCapabilitySnapshot> FindProfileCapability(
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

	void RegisterEditorMcpBuildProfileTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.BuildProfile.DiscoveryCoversProfileLifecycle", [](TestContext& context) {
			EditorAutomationService service;
			const std::array<std::string_view, 6> operations{
				"build_profile.list",
				"build_profile.get",
				"build_profile.create",
				"build_profile.update",
				"build_profile.duplicate",
				"build_profile.prepare_delete"
			};

			for (const std::string_view operation : operations)
			{
				const auto capability = FindProfileCapability(service, operation);
				context.Expect(capability.has_value(), std::string(operation) + " should be registered by the build-profile provider.");
				if (capability.has_value())
				{
					context.ExpectEqual(std::string("build_profile"), capability->descriptor.category, "Build-profile operations should share a stable discovery category.");
					context.Expect(capability->descriptor.availability.requiresProject, "Build-profile operations should require an open project.");
				}
			}
		});

		registry.Add("Editor.MCP.BuildProfile.PermissionsAndDeleteApprovalAreExplicit", [](TestContext& context) {
			EditorAutomationService service;
			for (const std::string_view operation : {"build_profile.list", "build_profile.get"})
			{
				const auto capability = FindProfileCapability(service, operation);
				context.Expect(capability.has_value(), std::string(operation) + " should be discoverable.");
				if (capability.has_value())
				{
					context.Expect(capability->descriptor.permission == AutomationPermission::Read, "Profile inspection must remain available in read-only mode.");
					context.Expect(capability->descriptor.mutationMode == MutationMode::ReadOnly, "Profile inspection must not be classified as mutation.");
				}
			}

			for (const std::string_view operation : {"build_profile.create", "build_profile.update", "build_profile.duplicate"})
			{
				const auto capability = FindProfileCapability(service, operation);
				context.Expect(capability.has_value(), std::string(operation) + " should be discoverable.");
				if (capability.has_value())
				{
					context.Expect(capability->descriptor.permission == AutomationPermission::ProjectWrite, "Profile changes require project-write permission.");
					context.Expect(capability->descriptor.execution.protectedCommit, "Profile changes must publish through a protected editor boundary.");
				}
			}

			const auto prepareDelete = FindProfileCapability(service, "build_profile.prepare_delete");
			context.Expect(prepareDelete.has_value(), "Profile deletion should be represented by an approval preflight.");
			if (prepareDelete.has_value())
			{
				context.Expect(prepareDelete->descriptor.approvalPolicy == AutomationApprovalPolicy::Destructive, "Profile deletion must require destructive approval.");
				context.Expect(prepareDelete->descriptor.permission == AutomationPermission::ProjectWrite, "Profile deletion must require project-write permission.");
			}
		});

		registry.Add("Editor.MCP.BuildProfile.SchemasRequireStableIdentityAndValidation", [](TestContext& context) {
			EditorAutomationService service;
			const auto get = FindProfileCapability(service, "build_profile.get");
			const auto create = FindProfileCapability(service, "build_profile.create");
			const auto update = FindProfileCapability(service, "build_profile.update");
			const auto duplicate = FindProfileCapability(service, "build_profile.duplicate");

			context.Expect(get.has_value() && create.has_value() && update.has_value() && duplicate.has_value(), "Profile lifecycle schemas should all be registered.");
			if (get.has_value())
				context.Expect(SchemaHasProperty(get->descriptor.inputSchema, "profileId"), "Profile lookup should use a stable profile identifier.");
			if (create.has_value())
			{
				context.Expect(SchemaHasProperty(create->descriptor.inputSchema, "name"), "Profile creation should require a display name.");
				context.Expect(SchemaHasProperty(create->descriptor.resultSchema, "validation"), "Profile creation should return structured validation results.");
			}
			if (update.has_value())
			{
				context.Expect(SchemaHasProperty(update->descriptor.inputSchema, "profileId"), "Profile updates should identify the existing profile.");
				context.Expect(SchemaHasProperty(update->descriptor.inputSchema, "changes"), "Profile updates should carry typed changes rather than replacing arbitrary JSON files.");
			}
			if (duplicate.has_value())
				context.Expect(SchemaHasProperty(duplicate->descriptor.resultSchema, "profileId"), "Duplication should return the new profile identity.");
		});

		registry.Add("Editor.MCP.BuildProfile.WindowIndependentCollectionOperationsPersistIdentity", [](TestContext& context) {
			const std::filesystem::path directory = std::filesystem::temp_directory_path() / "TerraEngineMcpBuildProfileOwnership";
			std::error_code ignored;
			std::filesystem::remove_all(directory, ignored);

			BuildProfileCollectionResult created = CreateBuildProfileDraft("Automation Profile");
			context.Expect(created.succeeded && created.profile.has_value(), "The reusable collection service should create a validated draft without a window.");
			if (!created.profile.has_value()) return;
			BuildProfileCollectionResult saved = SaveBuildProfileToCollection(directory, *created.profile);
			context.Expect(saved.succeeded && saved.profile.has_value(), "The reusable collection service should persist the validated draft.");
			if (!saved.profile.has_value()) return;
			const BuildProfileCollectionResult found = FindBuildProfileInCollection(directory, saved.profile->id);
			context.Expect(found.succeeded && found.profile.has_value(), "The persisted profile should be found by stable identity.");
			const BuildProfileCollectionResult removed = RemoveBuildProfileFromCollection(directory, saved.profile->id);
			context.Expect(removed.succeeded, "The reusable collection service should remove an approved profile by stable identity.");
			std::filesystem::remove_all(directory, ignored);
		});

		registry.Add("Editor.MCP.BuildProfile.StartupSceneUsesAssetsRelativeNativePath", [](TestContext& context) {
			const std::filesystem::path workspace = std::filesystem::temp_directory_path() / "TerraEngineMcpBuildProfileStartupScene";
			const std::filesystem::path project = workspace / "Project";
			std::error_code ignored;
			std::filesystem::remove_all(workspace, ignored);

			EditorContext editorContext;
			editorContext.projectManager = std::make_shared<ProjectManager>();
			context.Expect(editorContext.projectManager->CreateProject(project, "Project", ProjectTemplate::Blank),
				"The native project owner should create the build-profile test workspace.");
			std::filesystem::create_directories(project / "Assets" / "Scenes");
			// Only path ownership is under test here. Export validation separately
			// verifies the scene payload, so a minimal fixture keeps this test focused.
			std::ofstream(project / "Assets" / "Scenes" / "Main.tescene") << "{}";

			EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			const nlohmann::json created = service.HandleTool(
				"build_profile.create", {{"name", "Startup Scene"}}, WriteMode::WritesEnabled, "profile-create");
			context.Expect(created.value("ok", false), "Build-profile creation should succeed in an owner-opened project.");
			const std::string profileId = created["data"].value("profileId", std::string{});

			const nlohmann::json updated = service.HandleTool(
				"build_profile.update",
				{{"profileId", profileId}, {"changes", {{"startupScene", "Assets/Scenes/Main.tescene"}}}},
				WriteMode::WritesEnabled,
				"profile-update-startup-scene");
			context.Expect(updated.value("ok", false), "The scene-tool Assets prefix should be accepted at the MCP boundary.");
			if (updated.value("ok", false))
			{
				context.ExpectEqual(std::string("Scenes/Main.tescene"), updated["data"]["profile"].value("startupScene", std::string{}),
					"Persisted native profiles must store startup scenes relative to Assets.");
			}

			const nlohmann::json missing = service.HandleTool(
				"build_profile.update",
				{{"profileId", profileId}, {"changes", {{"startupScene", "Assets/Scenes/Missing.tescene"}}}},
				WriteMode::WritesEnabled,
				"profile-update-missing-scene");
			context.Expect(!missing.value("ok", true), "A missing startup scene must be rejected before profile persistence.");
			context.ExpectEqual(std::string("validation"), missing["error"].value("category", std::string{}),
				"A missing startup scene should report a validation failure.");

			editorContext.projectManager->CloseProject();
			std::filesystem::remove_all(workspace, ignored);
		});
	}
}
