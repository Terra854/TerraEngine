#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "AssetDatabase.h"
#include "EditorContext.h"
#include "ProjectManager.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		class AssetOrganizationFixture
		{
		public:
			explicit AssetOrganizationFixture(std::string name)
				: workspace(std::filesystem::temp_directory_path() / "TerraEngineMcpAssetOrganization" / std::move(name)),
				projectRoot(workspace / "Project")
			{
				std::error_code ignored;
				std::filesystem::remove_all(workspace, ignored);
				std::filesystem::create_directories(workspace);
				context.projectManager = std::make_shared<ProjectManager>();
				service.SetEditorContext(&context);
				// Opening a workspace is a native editor responsibility; the fixture
				// establishes that precondition before exercising MCP asset tools.
				projectReady = context.projectManager->CreateProject(projectRoot, "AssetOrganization", ProjectTemplate::Blank);
				if (projectReady)
				{
					// Asset organization must exercise the same indexed project view as
					// the editor instead of treating files as an unrestricted filesystem.
					context.assetDatabase = std::shared_ptr<AssetDatabase>(&AssetDatabase::Instance(), [](AssetDatabase*) {});
					context.assetDatabase->Initialize(projectRoot);
				}
			}

			~AssetOrganizationFixture()
			{
				AssetDatabase::Instance().Shutdown();
				std::error_code ignored;
				std::filesystem::remove_all(workspace, ignored);
			}

			void WriteAsset(const std::filesystem::path& relativePath, std::string_view contents = "asset") const
			{
				const std::filesystem::path absolutePath = projectRoot / relativePath;
				std::filesystem::create_directories(absolutePath.parent_path());
				std::ofstream output(absolutePath, std::ios::binary | std::ios::trunc);
				output << contents;
				output.close();
				// Tests publish files after project creation, so refresh the editor-owned
				// index before asking automation to resolve their stable identities.
				if (context.assetDatabase)
				{
					context.assetDatabase->ScanProject();
				}
			}

			std::filesystem::path workspace;
			std::filesystem::path projectRoot;
			EditorContext context;
			EditorAutomationService service;
			bool projectReady = false;
		};

		std::string ErrorCategory(const nlohmann::json& result)
		{
			return result.value("error", nlohmann::json::object()).value("category", std::string());
		}
	}

	void RegisterEditorMcpAssetOrganizationTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.AssetOrganization.InspectPublishesCompleteImpact", [](TestContext& context) {
			AssetOrganizationFixture fixture("InspectImpact");
			context.Expect(fixture.projectReady, "Asset impact fixture should create its project.");
			fixture.WriteAsset("Assets/Models/Source.txt");
			fixture.WriteAsset("Assets/Models/Source_Generated/Mesh.bin");

			const nlohmann::json inspected = fixture.service.HandleTool(
				"asset.inspect",
				{{"asset", "Assets/Models/Source.txt"}, {"operation", "move"}, {"destination", "Assets/Environment/Source.txt"}},
				WriteMode::ReadOnly,
				"asset-impact-inspect");
			context.Expect(inspected.value("ok", false), "Asset inspection should return a preflight impact report.");
			const nlohmann::json impact = inspected["data"].value("impact", nlohmann::json::object());
			context.Expect(!impact.value("fingerprint", std::string()).empty(), "Impact reports need a stable fingerprint for approval binding.");
			context.Expect(impact.contains("dependents") && impact["dependents"].is_array(), "Impact reports should enumerate project-owned dependents.");
			context.Expect(impact.contains("generatedChildren") && impact["generatedChildren"].is_array(), "Impact reports should enumerate generated child assets.");
			context.Expect(impact["generatedChildren"].dump().find("Source_Generated/Mesh.bin") != std::string::npos, "Generated children should identify the concrete output that must move with its owner.");
			context.Expect(impact.contains("repairPlan") && impact["repairPlan"].is_array(), "Impact reports should describe reference repairs before publication.");
			context.Expect(impact.contains("rollbackJournalEstimate"), "Impact reports should expose rollback-journal cost before mutation.");
		});

		registry.Add("Editor.MCP.AssetOrganization.RejectsTraversalAbsoluteAndCanonicalAliases", [](TestContext& context) {
			AssetOrganizationFixture fixture("PathSafety");
			context.Expect(fixture.projectReady, "Asset path-safety fixture should create its project.");
			fixture.WriteAsset("Assets/Source.txt");

			const nlohmann::json traversal = fixture.service.HandleTool(
				"asset.move",
				{{"source", "Assets/Source.txt"}, {"destination", "../Outside.txt"}},
				WriteMode::WritesEnabled,
				"asset-move-traversal");
			context.Expect(!traversal.value("ok", true), "Asset moves must reject traversal outside the project asset root.");
			context.ExpectEqual(std::string("validation"), ErrorCategory(traversal), "Traversal should be a validation error.");

			const nlohmann::json absolute = fixture.service.HandleTool(
				"asset.rename",
				{{"asset", "Assets/Source.txt"}, {"destination", (fixture.workspace / "Outside.txt").string()}},
				WriteMode::WritesEnabled,
				"asset-rename-absolute");
			context.Expect(!absolute.value("ok", true), "Asset destinations must remain project relative.");
			context.ExpectEqual(std::string("validation"), ErrorCategory(absolute), "Absolute destinations should be validation errors.");

			const nlohmann::json alias = fixture.service.HandleTool(
				"asset.move",
				{{"source", "Assets/Source.txt"}, {"destination", "Assets/Alias/../../Outside.txt"}},
				WriteMode::WritesEnabled,
				"asset-move-alias");
			context.Expect(!alias.value("ok", true), "Canonical containment must reject aliases that lexically begin inside Assets but resolve outside it.");
			context.Expect(std::filesystem::exists(fixture.projectRoot / "Assets/Source.txt"), "Rejected aliases must not mutate the source asset.");
		});

		registry.Add("Editor.MCP.AssetOrganization.MoveRenameDuplicatePreserveIdentityAndReferences", [](TestContext& context) {
			AssetOrganizationFixture fixture("Lifecycle");
			context.Expect(fixture.projectReady, "Asset lifecycle fixture should create its project.");
			fixture.WriteAsset("Assets/Items/Source.txt");

			const nlohmann::json moved = fixture.service.HandleTool(
				"asset.move",
				{{"source", "Assets/Items/Source.txt"}, {"destination", "Assets/Archive/Source.txt"}},
				WriteMode::WritesEnabled,
				"asset-move");
			context.Expect(moved.value("ok", false), "A contained asset move should publish atomically.");
			const std::string stableId = moved["data"].value("assetId", std::string());
			context.Expect(!stableId.empty(), "Move results should identify the stable asset identity.");
			context.Expect(moved["data"].contains("repairedReferences"), "Move results should report every repaired project reference.");

			const nlohmann::json renamed = fixture.service.HandleTool(
				"asset.rename",
				{{"asset", "Assets/Archive/Source.txt"}, {"name", "Renamed.txt"}},
				WriteMode::WritesEnabled,
				"asset-rename");
			context.Expect(renamed.value("ok", false), "A valid rename should publish through the asset boundary.");
			context.ExpectEqual(stableId, renamed["data"].value("assetId", std::string()), "Move and rename must preserve source identity.");

			const nlohmann::json duplicated = fixture.service.HandleTool(
				"asset.duplicate",
				{{"source", "Assets/Archive/Renamed.txt"}, {"destination", "Assets/Archive/RenamedCopy.txt"}},
				WriteMode::WritesEnabled,
				"asset-duplicate");
			context.Expect(duplicated.value("ok", false), "A valid asset duplicate should succeed.");
			context.Expect(duplicated["data"].value("assetId", std::string()) != stableId, "Duplicated assets require a new stable identity.");
		});

		registry.Add("Editor.MCP.AssetOrganization.ConflictRollsBackEveryPublishedFile", [](TestContext& context) {
			AssetOrganizationFixture fixture("Rollback");
			context.Expect(fixture.projectReady, "Asset rollback fixture should create its project.");
			fixture.WriteAsset("Assets/Source.txt", "source-before");
			fixture.WriteAsset("Assets/Destination.txt", "destination-before");

			const nlohmann::json result = fixture.service.HandleTool(
				"asset.move",
				{{"source", "Assets/Source.txt"}, {"destination", "Assets/Destination.txt"}, {"replace", false}},
				WriteMode::WritesEnabled,
				"asset-move-conflict");
			context.Expect(!result.value("ok", true), "A destination conflict should reject the complete asset transaction.");
			context.Expect(ErrorCategory(result) == "conflict" || ErrorCategory(result) == "validation", "Destination collision should use a machine-readable rejection category.");
			context.Expect(std::filesystem::exists(fixture.projectRoot / "Assets/Source.txt"), "Rollback must preserve the original source.");
			context.Expect(std::filesystem::exists(fixture.projectRoot / "Assets/Destination.txt"), "Rollback must preserve the pre-existing destination.");
			context.Expect(result.value("data", nlohmann::json::object()).contains("rollback"), "Rejected publication should report rollback-journal disposition.");
		});

		registry.Add("Editor.MCP.AssetOrganization.ApprovalFingerprintCannotAuthorizeChangedImpact", [](TestContext& context) {
			AssetOrganizationFixture fixture("ApprovalFingerprint");
			context.Expect(fixture.projectReady, "Asset approval fixture should create its project.");
			fixture.WriteAsset("Assets/Source.txt");

			const nlohmann::json prepared = fixture.service.HandleTool(
				"asset.inspect",
				{{"asset", "Assets/Source.txt"}, {"operation", "move"}, {"destination", "Assets/First.txt"}},
				WriteMode::ReadOnly,
				"asset-approval-preflight");
			context.Expect(prepared.value("ok", false), "High-impact asset work should provide a reviewable preflight.");
			const std::string fingerprint = prepared["data"]["impact"].value("fingerprint", std::string());
			context.Expect(!fingerprint.empty(), "Approval preflight should publish its normalized impact fingerprint.");

			const nlohmann::json changed = fixture.service.HandleTool(
				"asset.move",
				{{"source", "Assets/Source.txt"}, {"destination", "Assets/Changed.txt"}, {"approvedImpactFingerprint", fingerprint}},
				WriteMode::WritesEnabled,
				"asset-approval-substitution");
			context.Expect(!changed.value("ok", true), "An approval fingerprint must not authorize different normalized arguments.");
			context.Expect(ErrorCategory(changed) == "approval_required" || ErrorCategory(changed) == "conflict", "Changed impact should require renewed approval or report a fingerprint conflict.");
			context.Expect(std::filesystem::exists(fixture.projectRoot / "Assets/Source.txt"), "Fingerprint rejection must leave the source untouched.");
		});
	}
}
