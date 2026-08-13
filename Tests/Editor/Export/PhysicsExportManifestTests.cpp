#include "TestHarness.h"

#include "ExportEvidence.h"
#include "ExportTypes.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace TerraEngine::EditorTests {
	namespace {
		class TempPhysicsExportDirectory {
		public:
			TempPhysicsExportDirectory()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "PhysicsExportManifest") {
				std::error_code ignored;
				std::filesystem::remove_all(root_, ignored);
				std::filesystem::create_directories(root_);
			}

			~TempPhysicsExportDirectory() {
				std::error_code ignored;
				std::filesystem::remove_all(root_, ignored);
			}

			std::filesystem::path Write(const std::filesystem::path& relative, const std::string& contents) const {
				const std::filesystem::path destination = root_ / relative;
				std::filesystem::create_directories(destination.parent_path());
				std::ofstream output(destination, std::ios::binary | std::ios::trunc);
				output << contents;
				return destination;
			}

			std::filesystem::path root_;
		};

		std::string ReadText(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			std::ostringstream output;
			output << input.rdbuf();
			return output.str();
		}

		nlohmann::json MakePhysicsManifest() {
			return {
				{"physics", {
					{"schemaVersion", 1},
					{"provider", {{"identifier", "Jolt"}, {"version", "5.6.1"}, {"revision", "2e28006e4a367624bb512bf2c37ca99a93d9d1ea"}}},
					{"cookFormatVersion", 1},
					{"settings", {{"packagePath", "ProjectSettings/PhysicsSettings.json"}, {"schemaVersion", 1}, {"contentHash", "settings-hash"}}},
					{"layersHash", "layers-hash"}, {"defaultMaterialGuid", "11111111111111111111111111111111"},
					{"materials", nlohmann::json::array({{{"guid", "11111111111111111111111111111111"}, {"packagePath", "Physics/Default.tephysmat"}, {"checksum", "material-hash"}, {"size", 12}}})},
					{"colliders", nlohmann::json::array({{{"guid", "22222222222222222222222222222222"}, {"packagePath", "Physics/Level.tepcoll"}, {"checksum", "collider-hash"}, {"size", 24}, {"sourceProvenanceHash", "source-hash"}, {"recipeHash", "recipe-hash"}}})},
					{"validation", {{"fixtureSchemaVersion", 1}, {"status", "validated"}}}
				}}
			};
		}
	}

	void RegisterPhysicsExportManifestTests(TestRegistry& registry) {
		registry.Add("Editor.PhysicsExport.ManifestUsesAppendOnlyPhysicsAssetTypes", [](TestContext& context) {
			context.ExpectEqual(4u, static_cast<unsigned int>(AssetType::PhysicsMaterial), "Physics material must append after the original serialized asset types.");
			context.ExpectEqual(5u, static_cast<unsigned int>(AssetType::CookedCollider), "Cooked collider must append after physics material without renumbering prior types.");
			const PhysicsExportManifest manifest;
			context.ExpectEqual(std::string("Jolt"), manifest.providerIdentifier, "The exported physics provider identifier must remain engine-facing.");
			context.ExpectEqual(std::string("5.6.1"), manifest.providerVersion, "The manifest must record the approved Jolt 5.6.1 provider version.");
			context.ExpectEqual(std::string("2e28006e4a367624bb512bf2c37ca99a93d9d1ea"), manifest.providerRevision, "The manifest must record the exact approved Jolt revision.");
		});

		registry.Add("Editor.PhysicsExport.InventoryClassifiesRuntimePhysicsPayloads", [](TestContext& context) {
			TempPhysicsExportDirectory temporary;
			temporary.Write("Package/ProjectSettings/PhysicsSettings.json", "{}");
			temporary.Write("Package/Physics/Default.tephysmat", "physics material");
			temporary.Write("Package/Physics/Level.tepcoll", "cooked collider");
			temporary.Write("Package/Physics/Source.tecollider", "editor only recipe");
			std::vector<ExportContentInventoryEntry> inventory;
			std::string error;
			context.Expect(BuildExportContentInventory(temporary.root_ / "Package", inventory, error), "The physics package fixture should produce an inventory.");
			auto find = [&inventory](const std::string& path) { return std::find_if(inventory.begin(), inventory.end(), [&path](const auto& entry) { return entry.logicalPath == path; }); };
			context.Expect(find("ProjectSettings/PhysicsSettings.json") != inventory.end() && find("ProjectSettings/PhysicsSettings.json")->assetType == "physics_settings", "Resolved physics settings must be identified in inventory evidence.");
			context.Expect(find("Physics/Default.tephysmat") != inventory.end() && find("Physics/Default.tephysmat")->assetType == "physics_material", "Physics materials must be identified in inventory evidence.");
			context.Expect(find("Physics/Level.tepcoll") != inventory.end() && find("Physics/Level.tepcoll")->assetType == "cooked_collider", "Cooked colliders must be identified in inventory evidence.");
			context.Expect(find("Physics/Source.tecollider") != inventory.end() && find("Physics/Source.tecollider")->assetType == "other", "Source recipes are not runtime physics asset types and staging must exclude them.");
		});

		registry.Add("Editor.PhysicsExport.InventoryDoesNotInventEditorCacheOrPreferencePayloads", [](TestContext& context) {
			TempPhysicsExportDirectory temporary;
			const std::filesystem::path projectRoot = temporary.root_ / "Project";
			const std::filesystem::path packageInput = projectRoot / "PackageInput";
			temporary.Write("Project/PackageInput/Scenes/Runtime.tescene", R"({"objects":[]})");
			// These sibling editor artifacts intentionally remain outside package input.
			// The inventory must be rooted in explicit runtime staging, never a broad
			// project walk that could turn disposable caches into gameplay state.
			temporary.Write("Project/Library/DerivedScenes/Runtime.dscn", "derived scene cache");
			temporary.Write("Project/EditorPreferences/SceneView.json", "editor marker preference");

			std::vector<ExportContentInventoryEntry> inventory;
			std::string error;
			context.Expect(BuildExportContentInventory(packageInput, inventory, error), "The isolated runtime staging root should produce export inventory evidence.");
			const auto contains = [&inventory](const std::string& path) {
				return std::any_of(inventory.begin(), inventory.end(), [&path](const ExportContentInventoryEntry& entry) {
					return entry.logicalPath == path;
				});
			};
			context.Expect(contains("Scenes/Runtime.tescene"), "Authored runtime scenes must remain visible to export inventory.");
			context.Expect(!contains("Library/DerivedScenes/Runtime.dscn"), "Derived scene cache artifacts must not become authored runtime inventory.");
			context.Expect(!contains("EditorPreferences/SceneView.json"), "Editor marker preferences must not become authored runtime inventory.");
		});

		registry.Add("Editor.PhysicsExport.SixEvidenceFilesCarryPhysicsProvenance", [](TestContext& context) {
			TempPhysicsExportDirectory temporary;
			ExportRequest request;
			request.runId = "physics-export-evidence";
			request.evidenceDirectory = temporary.root_ / "Evidence";
			ExportResult result;
			result.runId = request.runId;
			result.success = true;
			result.validationResult.passed = true;
			ExportEvidenceInputs inputs;
			inputs.resolvedManifestJson = MakePhysicsManifest().dump(2);
			inputs.progressJsonLines = {R"({"type":"progress","schemaVersion":1,"stage":"asset_staging"})"};
			inputs.packageValidationReport = "Final: PASS\n";
			std::string error;
			context.Expect(WriteExportEvidenceBundle(request, result, inputs, error), "Physics export evidence should write all durable files.");
			const std::vector<std::string> files = {"export-result.json", "resolved-manifest.json", "content-inventory.json", "validation-report.json", "progress.jsonl", "package-validation.txt"};
			for(const std::string& name : files) {
				const std::string contents = ReadText(request.evidenceDirectory / name);
				context.Expect(contents.find("2e28006e4a367624bb512bf2c37ca99a93d9d1ea") != std::string::npos, "Every physics evidence output should expose the exact provider provenance.");
			}
			const nlohmann::json inventory = nlohmann::json::parse(ReadText(request.evidenceDirectory / "content-inventory.json"));
			context.Expect(inventory.at("physics").at("present").get<bool>(), "Inventory evidence must retain physics validation provenance even when the file list is empty.");
		});
	}
}
