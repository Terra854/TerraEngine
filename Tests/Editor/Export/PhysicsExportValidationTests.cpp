#include "TestHarness.h"

// Physics export validation tests construct small self-contained TEPAK inputs.
// Each seeded failure represents data a creator can repair before a standalone
// runtime attempts to create a physics world.

#include "ExportPackageValidator.h"
#include "ExportValidator.h"
#include "Physics/Assets/CookedColliderAsset.h"
#include "TEPAK/PackWriter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		class PhysicsValidationWorkspace {
		public:
			PhysicsValidationWorkspace()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "PhysicsExportValidation") {
				std::error_code ignored;
				std::filesystem::remove_all(root_, ignored);
				std::filesystem::create_directories(InputRoot());
			}

			~PhysicsValidationWorkspace() {
				std::error_code ignored;
				std::filesystem::remove_all(root_, ignored);
			}

			std::filesystem::path InputRoot() const { return root_ / "Input"; }
			std::filesystem::path ProjectRoot() const { return root_ / "Project"; }
			std::filesystem::path PackPath() const { return root_ / "Game.tepak"; }

			void Write(const std::filesystem::path& relative, const std::vector<std::byte>& bytes) const {
				const std::filesystem::path path = InputRoot() / relative;
				std::filesystem::create_directories(path.parent_path());
				std::ofstream output(path, std::ios::binary | std::ios::trunc);
				output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			}

			void WriteText(const std::filesystem::path& relative, const std::string& text) const {
				Write(relative, std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())));
			}

			void CreatePack() const {
				tepak::PackagingRequest request;
				request.inputRoot = InputRoot();
				request.outputPackPath = PackPath();
				request.overwriteExisting = true;
				request.enableCompression = false;
				request.enableDictionaryTraining = false;
				(void)tepak::PackWriter::Create(request);
			}

		private:
			std::filesystem::path root_;
		};

		std::string Hash(const std::vector<std::byte>& bytes) {
			std::uint64_t hash = 14695981039346656037ULL;
			for(const std::byte value : bytes) {
				hash ^= std::to_integer<std::uint8_t>(value);
				hash *= 1099511628211ULL;
			}
			std::ostringstream output;
			output << std::hex << std::setfill('0') << std::setw(16) << hash;
			return output.str();
		}

		std::vector<std::byte> Bytes(std::string_view text) {
			std::vector<std::byte> bytes;
			bytes.reserve(text.size());
			for(const char value : text) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
			return bytes;
		}

		std::vector<std::byte> MakeCookedCollider() {
			TerraEngine::Physics::CookedColliderEnvelope envelope;
			envelope.assetId[0] = 1U;
			envelope.sourceAssetId[0] = 2U;
			envelope.sourceContentHash = 3U;
			envelope.recipeHash = 4U;
			envelope.vertexCount = 3U;
			envelope.triangleCount = 1U;
			envelope.partCount = 1U;
			envelope.bounds.minimum = {-1.0F, -1.0F, -1.0F};
			envelope.bounds.maximum = {1.0F, 1.0F, 1.0F};
			TerraEngine::Physics::CookedColliderDiagnostics diagnostics;
			const auto collider = TerraEngine::Physics::CookedColliderAsset::Create(envelope, Bytes("cooked-jolt-payload"), diagnostics);
			return collider ? collider->Serialize() : std::vector<std::byte>{};
		}

		std::vector<std::byte> MakeSettings() {
			nlohmann::json layers = nlohmann::json::array();
			for(std::uint32_t index = 0U; index < 32U; ++index) layers.push_back({{"id", index}, {"name", "Layer" + std::to_string(index)}});
			return Bytes(nlohmann::json{{"schemaVersion", 1}, {"gravity", {0, -9.81, 0}}, {"layers", layers}}.dump());
		}

		void WriteRuntimeEntries(const PhysicsValidationWorkspace& workspace) {
			static constexpr std::array<std::string_view, 31> kEntries = {
				"Fonts/Roboto-Medium.ttf", "Shaders/Default.task.tspf", "Shaders/Default.mesh.tspf", "Shaders/Default.frag.tspf",
				"Shaders/Default.ConventionalVisibilityIdentity.frag.spv", "Shaders/Editor/SceneViewGrid.vert.spv", "Shaders/Editor/SceneViewGrid.frag.spv", "Shaders/Sky/SkyBackground.task.tspf",
				"Shaders/Sky/SkyBackground.mesh.tspf", "Shaders/Sky/SkyBackground.frag.tspf", "Shaders/Terrain/TerrainVisibility.task.tspf",
				"Shaders/Terrain/TerrainVisibility.mesh.tspf", "Shaders/Terrain/TerrainVisibility.frag.tspf", "Shaders/Terrain/TerrainSurface.comp.tspf", "Shaders/UI/UI_Instanced.vert.spv",
				"Shaders/UI/UI_Instanced.frag.spv", "Shaders/UI/UI_Static.vert.spv", "Shaders/UI/UI_Static.frag.spv",
				"Shaders/UI/UI_SDF_Text.vert.spv", "Shaders/UI/UI_SDF_Text.frag.spv", "Shaders/Lighting/ClusterDepthMask.comp.spv",
				"Shaders/Lighting/ClusterGrid.comp.spv", "Shaders/Lighting/ClusterLightCount.comp.spv", "Shaders/Lighting/ClusterLightFill.comp.spv",
				"Shaders/Lighting/ClusterLightPrefix.comp.spv", "Shaders/Lighting/LightCull.comp.spv", "Shaders/PostProcess/HiZGenerate.comp.spv",
				"Shaders/PostProcess/VRSGenerate.comp.spv", "Shaders/PostProcess/GTAO.comp.spv", "Shaders/PostProcess/GTAOBlur.comp.spv", "Scenes/Startup.tescene"
			};
			for(const std::string_view entry : kEntries) {
				workspace.WriteText(std::string(entry), entry.ends_with(".tescene") ? R"({"format":"TerraSceneUnboundedV1","objects":[]})" : "runtime-support-payload");
			}
		}

		nlohmann::json MakePhysicsManifest(const std::vector<std::byte>& settings, const std::vector<std::byte>& collider) {
			const std::string settingsText(reinterpret_cast<const char*>(settings.data()), settings.size());
			const nlohmann::json settingsDocument = nlohmann::json::parse(settingsText);
			const std::vector<std::byte> layers = Bytes(settingsDocument.at("layers").dump());
			return {
				{"schemaVersion", 1},
				{"provider", {{"identifier", "Jolt"}, {"version", "5.6.1"}, {"revision", "2e28006e4a367624bb512bf2c37ca99a93d9d1ea"}}},
				{"cookFormatVersion", 1},
				{"settings", {{"packagePath", "ProjectSettings/PhysicsSettings.json"}, {"schemaVersion", 1}, {"contentHash", Hash(settings)}}},
				{"layersHash", Hash(layers)}, {"defaultMaterialGuid", ""}, {"materials", nlohmann::json::array()},
				{"colliders", nlohmann::json::array({{{"guid", "01000000000000000000000000000000"}, {"packagePath", "Physics/Level.tepcoll"}, {"checksum", Hash(collider)}, {"size", collider.size()}, {"sourceProvenanceHash", "source"}, {"recipeHash", "recipe"}}})},
				{"validation", {{"fixtureSchemaVersion", 1}, {"status", "validated"}}}
			};
		}

		void WritePhysicsPackage(const PhysicsValidationWorkspace& workspace, const nlohmann::json& physics, const std::vector<std::byte>& settings, const std::vector<std::byte>& collider) {
			WriteRuntimeEntries(workspace);
			workspace.Write("ProjectSettings/PhysicsSettings.json", settings);
			workspace.Write("Physics/Level.tepcoll", collider);
			workspace.WriteText("game.json", nlohmann::json{
				{"startupScene", "Scenes/Startup.tescene"},
				{"rendererRecovery", {{"mode", "failClosed"}, {"fatalMessage", "restart"}, {"fullApplicationRestart", false}}},
				{"physics", physics}
			}.dump());
			workspace.CreatePack();
		}
	}

	void RegisterPhysicsExportValidationTests(TestRegistry& registry) {
		registry.Add("Editor.PhysicsExport.ValidationRejectsMissingEmptyAndCorruptColliderDependencies", [](TestContext& context) {
			const std::vector<std::byte> settings = MakeSettings();
			const std::vector<std::byte> validCollider = MakeCookedCollider();
			for(const std::string& mode : {"missing", "empty", "corrupt"}) {
				PhysicsValidationWorkspace workspace;
				nlohmann::json physics = MakePhysicsManifest(settings, validCollider);
				std::vector<std::byte> collider = validCollider;
				if(mode == "missing") physics["colliders"][0]["packagePath"] = "Physics/Missing.tepcoll";
				if(mode == "empty") { collider.clear(); physics["colliders"][0]["checksum"] = Hash(collider); physics["colliders"][0]["size"] = 1U; }
				if(mode == "corrupt") { collider = Bytes("not-a-cooked-collider"); physics["colliders"][0]["checksum"] = Hash(collider); physics["colliders"][0]["size"] = collider.size(); }
				WritePhysicsPackage(workspace, physics, settings, collider);
				std::string error;
				context.Expect(!ValidateExportPackage(workspace.PackPath(), error), "Physics " + mode + " dependency must fail package validation.");
				context.Expect(!error.empty(), "Physics " + mode + " failure must include an actionable diagnostic.");
			}
		});

		registry.Add("Editor.PhysicsExport.ValidationRejectsPlaceholderProviderMismatchAndSourceOnlyColliders", [](TestContext& context) {
			const std::vector<std::byte> settings = MakeSettings();
			const std::vector<std::byte> validCollider = MakeCookedCollider();
			{
				PhysicsValidationWorkspace workspace;
				const std::vector<std::byte> placeholder = Bytes("PLACEHOLDER collider payload");
				WritePhysicsPackage(workspace, MakePhysicsManifest(settings, placeholder), settings, placeholder);
				std::string error;
				context.Expect(!ValidateExportPackage(workspace.PackPath(), error) && error.find("artifact.collider") != std::string::npos, "Text cannot satisfy a cooked collider envelope and must report the structural diagnostic.");
			}
			{
				PhysicsValidationWorkspace workspace;
				nlohmann::json physics = MakePhysicsManifest(settings, validCollider);
				physics["provider"]["revision"] = "wrong-revision";
				WritePhysicsPackage(workspace, physics, settings, validCollider);
				std::string error;
				context.Expect(!ValidateExportPackage(workspace.PackPath(), error) && error.find("2e28006e4a367624bb512bf2c37ca99a93d9d1ea") != std::string::npos, "Provider mismatch must name the approved Jolt revision.");
			}
			{
				PhysicsValidationWorkspace workspace;
				nlohmann::json physics = MakePhysicsManifest(settings, validCollider);
				WritePhysicsPackage(workspace, physics, settings, validCollider);
				workspace.WriteText("Physics/Recipe.tecollider", "editor-only recipe");
				workspace.CreatePack();
				std::string error;
				context.Expect(!ValidateExportPackage(workspace.PackPath(), error) && error.find("source-only") != std::string::npos, "Source-only recipes must never be accepted by a runtime package.");
			}
			{
				PhysicsValidationWorkspace workspace;
				const nlohmann::json physics = MakePhysicsManifest(settings, validCollider);
				WritePhysicsPackage(workspace, physics, settings, validCollider);
				// A scene reference is a dependency even when the recipe file itself
				// was omitted from the archive. This guards the staging rewrite path.
				workspace.WriteText("Scenes/Startup.tescene", R"({"format":"TerraSceneUnboundedV1","objects":[],"physicsExportFixture":{"sourceCollider":"Assets/Colliders/Startup.tecollider"}})");
				workspace.CreatePack();
				std::string error;
				context.Expect(!ValidateExportPackage(workspace.PackPath(), error) && error.find("source-only") != std::string::npos, "Startup scenes must not retain source-only collider dependencies.");
			}
			{
				PhysicsValidationWorkspace workspace;
				nlohmann::json physics = MakePhysicsManifest(settings, validCollider);
				physics["colliders"] = nlohmann::json::array();
				WritePhysicsPackage(workspace, physics, settings, validCollider);
				workspace.WriteText("Scenes/Startup.tescene", R"({"format":"TerraSceneUnboundedV1","objects":[],"physicsExportFixture":{}})");
				workspace.CreatePack();
				std::string error;
				context.Expect(!ValidateExportPackage(workspace.PackPath(), error) && error.find("cooked collider") != std::string::npos, "A physics parity fixture must declare at least one cooked collider dependency.");
			}
		});

		registry.Add("Editor.PhysicsExport.ValidationRejectsUnresolvedDefaultMaterial", [](TestContext& context) {
			PhysicsValidationWorkspace workspace;
			const std::vector<std::byte> settings = MakeSettings();
			const std::vector<std::byte> validCollider = MakeCookedCollider();
			nlohmann::json physics = MakePhysicsManifest(settings, validCollider);
			// No material is staged in this fixture, so a nonempty default GUID is
			// an actionable unresolved dependency rather than an optional fallback.
			physics["defaultMaterialGuid"] = "missing-default-material";
			WritePhysicsPackage(workspace, physics, settings, validCollider);
			std::string error;
			context.Expect(!ValidateExportPackage(workspace.PackPath(), error)
				&& error.find("defaultMaterialGuid") != std::string::npos,
				"An exported default material identity must resolve to the packaged material inventory.");
		});

		registry.Add("Editor.PhysicsExport.ValidationRejectsSourceOnlyRecipeBeforeStaging", [](TestContext& context) {
			PhysicsValidationWorkspace workspace;
			const std::filesystem::path recipe = workspace.ProjectRoot() / "Assets" / "Physics" / "Level.tecollider";
			std::filesystem::create_directories(recipe.parent_path());
			std::ofstream output(recipe, std::ios::binary | std::ios::trunc);
			output << "source recipe";
			output.close();
			BuildProfile profile;
			profile.name = "Physics validation";
			profile.id = "physics-validation";
			profile.startupScene = "Scenes/Startup.tescene";
			profile.enableManagedCompilation = false;
			const ExportValidationResult result = ExportValidator{}.ValidateAll(profile, workspace.ProjectRoot());
			const bool found = std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const ExportDiagnostic& diagnostic) {
				return diagnostic.category == "PhysicsCollider" && diagnostic.message.find("Source-only") != std::string::npos;
			});
			context.Expect(!result.passed && found, "Pre-staging validation must explain how to cook a source-only collider recipe.");
		});
	}
}
