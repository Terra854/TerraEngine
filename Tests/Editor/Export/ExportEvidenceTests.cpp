#include "TestHarness.h"

#include "ExportEvidence.h"
#include "Physics/Assets/PhysicsMaterial.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		class TempExportEvidenceDirectory {
		public:
			TempExportEvidenceDirectory()
				: root_(std::filesystem::temp_directory_path()
					/ "TerraEngineEditorTests" / "ExportEvidence") {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
				std::filesystem::create_directories(root_);
			}

			~TempExportEvidenceDirectory() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] const std::filesystem::path& Root() const {
				return root_;
			}

			std::filesystem::path WriteText(
				const std::filesystem::path& relativePath,
				const std::string& contents) const
			{
				const std::filesystem::path path = root_ / relativePath;
				std::filesystem::create_directories(path.parent_path());
				std::ofstream output(path, std::ios::binary | std::ios::trunc);
				output << contents;
				return path;
			}

		private:
			std::filesystem::path root_;
		};

		std::string ReadText(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		ExportRequest MakeEvidenceRequest(const std::filesystem::path& root) {
			ExportRequest request;
			request.runId = "evidence-contract";
			request.invocationKind = ExportInvocationKind::HeadlessCli;
			request.profile.id = "evidence-profile";
			request.projectRoot = root / "Project";
			request.finalOutputDirectory = root / "Published";
			request.evidenceDirectory = root / "Evidence";
			return request;
		}

		ExportResult MakeFailureResult(const ExportRequest& request) {
			ExportResult result;
			result.runId = request.runId;
			result.success = false;
			result.invocationKind = request.invocationKind;
			result.stage = ExportStage::AssetStaging;
			result.failureCategory = ExportFailureCategory::AssetStagingFailed;
			result.exitCode = ExitCodeForFailure(result.failureCategory);
			result.message = "Required staged asset was rejected.";
			result.profileId = request.profile.id;
			result.projectRoot = request.projectRoot;
			result.outputDirectory = request.finalOutputDirectory;
			result.evidenceDirectory = request.evidenceDirectory;
			result.validationResult.passed = false;
			result.validationResult.errorCount = 2;

			ExportDiagnostic laterDiagnostic;
			laterDiagnostic.severity = ExportDiagnosticSeverity::Error;
			laterDiagnostic.stage = "validation";
			laterDiagnostic.category = "Profile";
			laterDiagnostic.code = "export.profile.invalid";
			laterDiagnostic.filePath = "BuildProfiles/baseline.json";
			laterDiagnostic.message = "Profile is invalid.";

			ExportDiagnostic earlierDiagnostic;
			earlierDiagnostic.severity = ExportDiagnosticSeverity::Error;
			earlierDiagnostic.stage = "asset_staging";
			earlierDiagnostic.category = "Assets";
			earlierDiagnostic.code = "export.asset.placeholder";
			earlierDiagnostic.filePath = "Meshes/Baseline.temf";
			earlierDiagnostic.message = "Placeholder marker found.";
			result.diagnostics = {laterDiagnostic, earlierDiagnostic};
			return result;
		}
	}

	void RegisterExportEvidenceTests(TestRegistry& registry) {
		registry.Add("Editor.ExportEvidence.InventoryIsSortedTypedAndPlaceholderAware", [](TestContext& context) {
			TempExportEvidenceDirectory tempDirectory;
			const std::filesystem::path packageInput = tempDirectory.Root() / "PackageInput";
			tempDirectory.WriteText("PackageInput/Zeta/Raw.txt", "ordinary source bytes");
			TerraEngine::UUID materialId{}; materialId[0] = 1;
			auto material = TerraEngine::Physics::PhysicsMaterial::Defaults(materialId);
			material.surfaceTag = "binary-prefix-PLACEHOLDER-binary-suffix";
			tempDirectory.WriteText("PackageInput/Physics/Baseline.tephysmat", material.ToJson().dump());
			tempDirectory.WriteText("PackageInput/game.json", R"({"startupScene":"Scenes/Startup.tescene"})");

			std::vector<ExportContentInventoryEntry> entries;
			std::string error;
			context.Expect(BuildExportContentInventory(packageInput, entries, error), "A readable package-input directory should produce deterministic inventory entries.");
			context.ExpectEqual(3u, static_cast<unsigned int>(entries.size()), "Every staged regular file should appear once in the inventory.");
			context.Expect(std::is_sorted(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
				return left.logicalPath < right.logicalPath;
			}), "Inventory paths should be lexically sorted before serialization.");

			const auto mesh = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
				return entry.logicalPath == "Physics/Baseline.tephysmat";
			});
			context.Expect(mesh != entries.end(), "The runtime material should keep its normalized logical path.");
			context.ExpectEqual(std::string("physics_material"), mesh->assetType, "TEPHYSMAT files should retain their typed identity.");
			context.ExpectEqual(std::string("compiled"), mesh->sourceKind, "Runtime physics materials should be identified as compiled payloads.");
			context.Expect(!mesh->placeholderDetected, "Ordinary material text must not be classified as explicit producer stub evidence.");
			context.Expect(!mesh->contentHash.empty(), "Every inventory entry should carry a deterministic content identity.");
		});

		registry.Add("Editor.ExportEvidence.ResultSchemaAndDiagnosticsRemainStable", [](TestContext& context) {
			TempExportEvidenceDirectory tempDirectory;
			const ExportRequest request = MakeEvidenceRequest(tempDirectory.Root());
			const ExportResult result = MakeFailureResult(request);
			const nlohmann::json document = nlohmann::json::parse(SerializeExportResultRecord(result));

			context.ExpectEqual(std::string("result"), document.at("type").get<std::string>(), "Serialized final records should identify the result type.");
			context.ExpectEqual(1u, document.at("schemaVersion").get<unsigned int>(), "Export evidence should remain on schema version 1.");
			context.ExpectEqual(std::string("asset_staging"), document.at("stage").get<std::string>(), "The stable failed stage should be machine-readable.");
			context.ExpectEqual(std::string("asset_staging_failed"), document.at("failureCategory").get<std::string>(), "The stable failure category should be machine-readable.");
			context.ExpectEqual(6, document.at("exitCode").get<int>(), "Asset staging failures should keep exit code 6.");
			context.ExpectEqual(2u, static_cast<unsigned int>(document.at("diagnostics").size()), "Every structured diagnostic should be retained.");
			context.ExpectEqual(std::string("asset_staging"), document.at("diagnostics").at(0).at("stage").get<std::string>(), "Diagnostics should be sorted by stable stage before presentation wording.");
			context.ExpectEqual(std::string("export.asset.placeholder"), document.at("diagnostics").at(0).at("code").get<std::string>(), "Stable diagnostic codes should survive serialization.");
		});

		registry.Add("Editor.ExportEvidence.PartialFailureWritesInspectableSixFileBundle", [](TestContext& context) {
			TempExportEvidenceDirectory tempDirectory;
			const ExportRequest request = MakeEvidenceRequest(tempDirectory.Root());
			const ExportResult result = MakeFailureResult(request);
			ExportEvidenceInputs inputs;
			inputs.progressJsonLines.push_back(R"({"type":"progress","schemaVersion":1,"runId":"evidence-contract","stage":"asset_staging","progress":0.4,"message":"Staging"})");
			std::string error;

			context.Expect(WriteExportEvidenceBundle(request, result, inputs, error), "A failed run with a resolved evidence directory should still write inspectable partial evidence.");
			const std::vector<std::string> expectedFiles = {
				"export-result.json",
				"resolved-manifest.json",
				"content-inventory.json",
				"validation-report.json",
				"progress.jsonl",
				"package-validation.txt"
			};
			for(const std::string& fileName : expectedFiles) {
				context.Expect(std::filesystem::is_regular_file(request.evidenceDirectory / fileName), "Partial evidence should retain every stable bundle filename.");
			}

			const nlohmann::json inventory = nlohmann::json::parse(ReadText(request.evidenceDirectory / "content-inventory.json"));
			context.ExpectEqual(0u, inventory.at("entryCount").get<unsigned int>(), "A pre-inventory failure should explicitly record an empty inventory.");
			context.Expect(ReadText(request.evidenceDirectory / "package-validation.txt").find("Final: FAIL") != std::string::npos, "A package stage that did not run must never look successful.");
			context.ExpectEqual(std::string("{}\n"), ReadText(request.evidenceDirectory / "resolved-manifest.json"), "A missing staged manifest should have an explicit empty partial record.");

			std::istringstream progressStream(ReadText(request.evidenceDirectory / "progress.jsonl"));
			std::string line;
			unsigned int lineCount = 0;
			while(std::getline(progressStream, line)) {
				if(line.empty()) continue;
				const nlohmann::json parsedLine = nlohmann::json::parse(line);
				context.Expect(parsedLine.is_object(), "Every progress evidence line should be a JSON object.");
				++lineCount;
			}
			context.ExpectEqual(2u, lineCount, "Partial progress evidence should contain prior progress followed by exactly one final result.");
		});

		registry.Add("Editor.ExportEvidence.InventorySerializationPreservesSortedOrder", [](TestContext& context) {
			TempExportEvidenceDirectory tempDirectory;
			const ExportRequest request = MakeEvidenceRequest(tempDirectory.Root());
			ExportResult result = MakeFailureResult(request);
			ExportEvidenceInputs inputs;
			inputs.inventory = {
				{"z/file.tetf", "texture", "compiled", 3u, "bbbb", true, false},
				{"a/file.temf", "mesh", "compiled", 2u, "aaaa", true, false}
			};
			std::string error;

			context.Expect(WriteExportEvidenceBundle(request, result, inputs, error), "Evidence serialization should accept inventory prepared in any order.");
			const nlohmann::json inventory = nlohmann::json::parse(ReadText(request.evidenceDirectory / "content-inventory.json"));
			context.ExpectEqual(std::string("a/file.temf"), inventory.at("entries").at(0).at("logicalPath").get<std::string>(), "Serialized inventory entries should be lexically sorted.");
			context.ExpectEqual(std::string("z/file.tetf"), inventory.at("entries").at(1).at("logicalPath").get<std::string>(), "Sorted inventory should be deterministic across entry discovery order.");
		});

		registry.Add("Editor.ExportEvidence.InterruptedWritePreservesBlockingPath", [](TestContext& context) {
			TempExportEvidenceDirectory tempDirectory;
			ExportRequest request = MakeEvidenceRequest(tempDirectory.Root());
			request.evidenceDirectory = tempDirectory.WriteText("blocked-evidence", "previous diagnostic marker");
			const ExportResult result = MakeFailureResult(request);
			ExportEvidenceInputs inputs;
			std::string error;

			context.Expect(!WriteExportEvidenceBundle(request, result, inputs, error), "An evidence path occupied by a regular file should fail cleanly.");
			context.Expect(!error.empty(), "An interrupted evidence write should return an actionable filesystem reason.");
			context.ExpectEqual(std::string("previous diagnostic marker"), ReadText(request.evidenceDirectory), "Failure must not overwrite the blocking path or invent a successful bundle.");
		});

		registry.Add("Editor.ExportEvidence.MissingInputAndInventoryParityFailExplicitly", [](TestContext& context) {
			TempExportEvidenceDirectory tempDirectory;
			std::vector<ExportContentInventoryEntry> entries;
			std::string error;
			context.Expect(!BuildExportContentInventory(tempDirectory.Root() / "Missing", entries, error), "A missing package-input directory should not be mistaken for an empty successful inventory.");
			context.Expect(!error.empty(), "Missing input should include the affected directory in a human-readable error.");

			std::vector<ExportContentInventoryEntry> left = {
				{"Assets/One.temf", "mesh", "compiled", 4u, "hash-one", true, false}
			};
			std::vector<ExportContentInventoryEntry> right = left;
			right.front().contentHash = "hash-two";
			context.Expect(!EquivalentExportContentInventories(left, right), "Parity should reject same-path entries whose bytes differ.");
		});
	}
}
