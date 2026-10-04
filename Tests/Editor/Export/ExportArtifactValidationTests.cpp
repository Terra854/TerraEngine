#include "TestHarness.h"

#include "ExportEvidence.h"
#include "ExportValidator.h"
#include "ExportPipeline.h"
#include "ExportPackageValidator.h"
#include "ExportRuntimeLayout.h"
#include "Runtime/ArtifactValidation.h"
#include "Runtime/ExportStartupCoordinator.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Utilities/PlatformUtils.h"
#include "Physics/Assets/PhysicsMaterial.h"
#include "TEPAK/PackWriter.h"
#include "Tests/Engine/Runtime/ArtifactValidationTestCorpus.h"
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>
#include "Physics/Assets/CookedColliderAsset.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		// Every test owns a fresh directory; cleanup cannot touch an authored project.
		class ArtifactWorkspace final {
		public:
			ArtifactWorkspace() {
				static std::atomic<unsigned> sequence{0};
				base_ = std::filesystem::absolute(std::filesystem::temp_directory_path()
					/ "TerraEngineArtifactTests").lexically_normal();
				root_ = base_ / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
					+ "-" + std::to_string(sequence.fetch_add(1)));
				std::filesystem::create_directories(root_);
			}
			~ArtifactWorkspace() {
				// Verify the exact absolute disposable boundary before recursive removal.
				if(root_.parent_path() == base_) {
					std::error_code ignored;
					std::filesystem::remove_all(root_, ignored);
					std::filesystem::remove(base_ / (root_.filename().string() + ".tepak"), ignored);
				}
			}
			const std::filesystem::path& Root() const { return root_; }
			void Write(const std::filesystem::path& relative, std::span<const std::byte> bytes) const {
				const auto path = root_ / relative;
				std::filesystem::create_directories(path.parent_path());
				std::ofstream output(path, std::ios::binary);
				output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			}
			void Write(const std::filesystem::path& relative, std::string_view text) const {
				Write(relative, std::as_bytes(std::span(text.data(), text.size())));
			}
		private:
			std::filesystem::path base_;
			std::filesystem::path root_;
		};

		std::vector<std::byte> CookedWordPayload() {
			Physics::CookedColliderEnvelope envelope;
			envelope.assetId[0] = 1; envelope.sourceAssetId[0] = 2;
			envelope.sourceContentHash = 3; envelope.recipeHash = 4;
			envelope.vertexCount = 3; envelope.triangleCount = 1; envelope.partCount = 1;
			envelope.bounds.minimum = {-1, -1, -1}; envelope.bounds.maximum = {1, 1, 1};
			const std::string_view payload = "legitimate opaque Jolt payload: PlAcEhOlDeRDoor";
			Physics::CookedColliderDiagnostics diagnostics;
			const auto payloadBytes = std::as_bytes(std::span(payload.data(), payload.size()));
			const auto asset = Physics::CookedColliderAsset::Create(envelope,
				std::vector<std::byte>(payloadBytes.begin(), payloadBytes.end()), diagnostics);
			return asset ? asset->Serialize() : std::vector<std::byte>{};
		}
		Runtime::ArtifactValidationResult Native(const Tests::ArtifactCorpus::Buffer& bytes, Runtime::NativeImageTarget target) {
			return Runtime::ValidateArtifact({bytes, Runtime::ArtifactKind::NativeGameplay, target, "Managed/Assembly-CSharp"});
		}
		ExportRuntimeLayout InstalledLayout() {
			// Require real installed runtime assets instead of weakening validation
			// for fabricated font/shader/launcher payloads. CMake install is prerequisite.
			const auto suffix = GetExportExecutableSuffix(GetHostExportPlatform());
			for(auto root = PlatformUtils::GetExecutablePath().parent_path(); !root.empty(); root = root.parent_path()) {
				for(const auto& candidate : {root / "Editor", root / "Build" / "Editor"}) {
					const auto executable = candidate / ("TerraEngine-Export" + suffix);
					if(!std::filesystem::is_regular_file(executable)) continue;
					ExportRuntimeLayout layout; std::string error;
					if(DiscoverExportRuntimeLayout(executable, layout, error) && ValidateExportRuntimeLayout(layout, GetHostExportPlatform(), error)) return layout;
				}
				if(root == root.parent_path()) break;
			}
			throw std::runtime_error("Artifact gate tests require the installed Editor runtime closure.");
		}
		std::string Hash(std::span<const std::byte> bytes) {
			// Reuse the engine's FNV payload checksum; no separate test hash algorithm.
			std::ostringstream output;
			output << std::hex << std::setfill('0') << std::setw(16) << Physics::CookedColliderAsset::ComputePayloadChecksum(bytes);
			return output.str();
		}
		std::span<const std::byte> ByteView(std::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); }
		nlohmann::json Settings() {
			const auto settings = Physics::PhysicsSettings::Defaults();
			nlohmann::json layers = nlohmann::json::array();
			for(const auto& layer : settings.layers) layers.push_back({{"id", layer.id}, {"name", "PlaceholderDoor" + std::to_string(layer.id)},
				{"collisionMask", layer.collisionMask}, {"broadPhaseGroup", layer.broadPhaseGroup}, {"enabled", layer.enabled}});
			return {{"schemaVersion", settings.schemaVersion}, {"gravity", {0, -9.81, 0}}, {"layers", layers}};
		}
		nlohmann::json WritePackage(ArtifactWorkspace& workspace, const Tests::ArtifactCorpus::Buffer& managed) {
			const auto layout = InstalledLayout();
			std::filesystem::copy(layout.shaderDirectory, workspace.Root() / "Shaders", std::filesystem::copy_options::recursive);
			std::filesystem::copy(layout.fontDirectory, workspace.Root() / "Fonts", std::filesystem::copy_options::recursive);
			workspace.Write("Scenes/Startup.tescene", R"({"format":"TerraSceneUnboundedV1","name":"PlaceholderDoor","objects":[]})");
			const auto settings = Settings(); const auto settingsText = settings.dump(); const auto layers = settings["layers"].dump();
			workspace.Write("ProjectSettings/PhysicsSettings.json", settingsText);
			UUID materialId{}; materialId[0] = 7;
			auto material = Physics::PhysicsMaterial::Defaults(materialId); material.surfaceTag = "pLaCeHoLdErStone";
			const auto materialText = material.ToJson().dump(); workspace.Write("Physics/Surface.tephysmat", materialText);
			const auto collider = CookedWordPayload(); workspace.Write("Physics/Door.tepcoll", collider);
			workspace.Write("Managed/Assembly-CSharp.dll", managed);
			UUID colliderId{}; colliderId[0] = 1;
			return {
				{"startupScene", "Scenes/Startup.tescene"},
				{"rendererRecovery", {{"mode", "failClosed"}, {"fatalMessage", "restart"}, {"fullApplicationRestart", false}}},
				{"physics", {{"schemaVersion", 1}, {"provider", {{"identifier", "Jolt"}, {"version", "5.6.1"}, {"revision", "2e28006e4a367624bb512bf2c37ca99a93d9d1ea"}}},
					{"cookFormatVersion", 1}, {"settings", {{"packagePath", "ProjectSettings/PhysicsSettings.json"}, {"schemaVersion", 1}, {"contentHash", Hash(ByteView(settingsText))}}},
					{"layersHash", Hash(ByteView(layers))}, {"defaultMaterialGuid", ToHexString(materialId)}, {"materials", nlohmann::json::array({{{"guid", ToHexString(materialId)}, {"packagePath", "Physics/Surface.tephysmat"}, {"size", materialText.size()}, {"checksum", Hash(ByteView(materialText))}}})},
					{"colliders", nlohmann::json::array({{{"guid", ToHexString(colliderId)}, {"packagePath", "Physics/Door.tepcoll"}, {"size", collider.size()}, {"checksum", Hash(collider)}, {"sourceProvenanceHash", "source"}, {"recipeHash", "recipe"}}})},
					{"validation", {{"status", "validated"}, {"fixtureSchemaVersion", 1}}}}}
			};
		}
		std::filesystem::path Pack(ArtifactWorkspace& workspace, const nlohmann::json& manifest) {
			workspace.Write("game.json", manifest.dump());
			const auto path = workspace.Root().parent_path() / (workspace.Root().filename().string() + ".tepak");
			tepak::PackagingRequest request; request.inputRoot = workspace.Root(); request.outputPackPath = path;
			request.enableCompression = false; request.enableDictionaryTraining = false; request.workerCount = 1;
			request.overwriteExisting = true; (void)tepak::PackWriter::Create(request);
			return path;
		}
		Tests::ArtifactCorpus::Buffer HostImage() {
			return Runtime::HostNativeImageTarget() == Runtime::NativeImageTarget::WindowsX64 ? Tests::ArtifactCorpus::Pe() : Tests::ArtifactCorpus::Elf();
		}

	}

	void RegisterExportArtifactValidationTests(TestRegistry& registry) {
		registry.Add("Editor.ExportArtifactValidation.AuthoredWordAccepted", [](TestContext& context) {
			ArtifactWorkspace workspace;
			workspace.Write("Scenes/Startup.tescene", R"({"format":"TerraSceneUnboundedV1","name":"PlaceholderDoor","objects":[]})");
			workspace.Write("Notes/ordinary.txt", "A placeholder is an ordinary authored word.");
			workspace.Write("Notes/ordinary-array.json", R"(["placeholder",{"name":"PlaceholderDoor"}])");
			std::vector<ExportContentInventoryEntry> inventory;
			std::string error;
			context.Expect(BuildExportContentInventory(workspace.Root(), inventory, error), "Readable authored content must produce inventory evidence.");
			context.Expect(inventory.size() == 3 && std::none_of(inventory.begin(), inventory.end(),
				[](const auto& entry) { return entry.placeholderDetected; }),
				"Ordinary words in valid scene names and text must not become explicit-stub evidence.");
		});
		registry.Add("Editor.ExportArtifactValidation.CookedWordAccepted", [](TestContext& context) {
			ArtifactWorkspace workspace;
			const auto bytes = CookedWordPayload();
			context.Expect(!bytes.empty(), "The corpus must use the production collider envelope serializer.");
			workspace.Write("Assets/Colliders/PlaceholderDoor.tecollider", R"({"schemaVersion":1})");
			workspace.Write("Build/Assets/Colliders/PlaceholderDoor.tepcoll", bytes);
			BuildProfile profile; profile.name = "Artifact Corpus";
			profile.enableManagedCompilation = false;
			profile.startupScene = "Scenes/Startup.tescene";
			workspace.Write("Assets/Scenes/Startup.tescene", R"({"format":"TerraSceneUnboundedV1","objects":[]})");
			const auto result = ExportValidator{}.ValidateAll(profile, workspace.Root());
			context.Expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
				[](const auto& diagnostic) { return diagnostic.category == "PhysicsCollider"
					&& diagnostic.severity == ExportDiagnosticSeverity::Error; }),
				"A codec-valid opaque cooked payload must not fail because its bytes contain an ordinary word.");
		});
		registry.Add("Editor.ExportArtifactValidation.PeBootstrapAccepted", [](TestContext& context) {
			for(const std::string_view name : {"InitManagedRuntimeV4", "InitManagedRuntimeV3", "InitManagedRuntime"})
				context.Expect(Native(Tests::ArtifactCorpus::Pe(name), Runtime::NativeImageTarget::WindowsX64).IsValid(), "Valid PE bootstrap and ordinary runtime words must pass the shared gameplay gate.");
		});
		registry.Add("Editor.ExportArtifactValidation.ElfBootstrapAccepted", [](TestContext& context) {
			for(const std::string_view name : {"InitManagedRuntimeV4", "InitManagedRuntimeV3", "InitManagedRuntime"})
				context.Expect(Native(Tests::ArtifactCorpus::Elf(name), Runtime::NativeImageTarget::LinuxX64).IsValid(), "Valid ELF bootstrap and ordinary runtime words must pass the shared gameplay gate.");
		});
		registry.Add("Editor.ExportArtifactValidation.NativeBoundsRejected", [](TestContext& context) {
			auto pe = Tests::ArtifactCorpus::Pe(); Tests::ArtifactCorpus::Put(pe, 0x80 + 20, 0xffff, 2);
			context.Expect(!Native(pe, Runtime::NativeImageTarget::WindowsX64).IsValid(), "PE optional header cannot extend beyond the supplied bytes.");
			auto elf = Tests::ArtifactCorpus::Elf(); Tests::ArtifactCorpus::Put(elf, 32, ~std::uint64_t{0}, 8);
			context.Expect(!Native(elf, Runtime::NativeImageTarget::LinuxX64).IsValid(), "ELF program-table arithmetic must fail closed.");
		});
		registry.Add("Editor.ExportArtifactValidation.WrongArchitectureRejected", [](TestContext& context) {
			auto pe = Tests::ArtifactCorpus::Pe(); Tests::ArtifactCorpus::Put(pe, 0x84, 0xaa64, 2);
			context.Expect(!Native(pe, Runtime::NativeImageTarget::WindowsX64).IsValid(), "PE ARM64 cannot satisfy x64 export contract.");
			auto elf = Tests::ArtifactCorpus::Elf(); Tests::ArtifactCorpus::Put(elf, 18, 183, 2);
			context.Expect(!Native(elf, Runtime::NativeImageTarget::LinuxX64).IsValid(), "ELF ARM64 cannot satisfy x64 export contract.");
		});
		registry.Add("Editor.ExportArtifactValidation.MissingBootstrapRejected", [](TestContext& context) {
			context.Expect(!Native(Tests::ArtifactCorpus::Pe("OrdinaryExport"), Runtime::NativeImageTarget::WindowsX64).IsValid(), "PE gameplay without a managed initialization contract must fail.");
			context.Expect(!Native(Tests::ArtifactCorpus::Elf("OrdinaryExport"), Runtime::NativeImageTarget::LinuxX64).IsValid(), "ELF gameplay without a managed initialization contract must fail.");
		});
		registry.Add("Editor.ExportArtifactValidation.UnsupportedTargetExplicit", [](TestContext& context) {
			const auto result = Native(Tests::ArtifactCorpus::Pe(), Runtime::NativeImageTarget::Unsupported);
			context.Expect(result.disposition == Runtime::ArtifactDisposition::Unsupported && !result.code.empty(), "Unknown image targets must be explicit rather than implicitly accepted.");
		});
		registry.Add("Editor.ExportArtifactValidation.TextStubStructurallyRejected", [](TestContext& context) {
			for(const std::string_view text : {"placeholder", "unfinished payload with no special words"}) {
				for(const auto kind : {Runtime::ArtifactKind::NativeGameplay, Runtime::ArtifactKind::Mesh, Runtime::ArtifactKind::Texture, Runtime::ArtifactKind::Material, Runtime::ArtifactKind::CookedCollider}) {
					const auto result = Runtime::ValidateArtifact({ByteView(text), kind, Runtime::HostNativeImageTarget(), "required artifact"});
					context.Expect(!result.IsValid() && !result.IsExplicitStub(), "Text cannot satisfy binary artifact structure; ordinary wording is irrelevant.");
				}
			}
			for(const std::string_view scene : {R"({"objects":[]})", R"({"format":"legacy","objects":[]})", R"({"format":"TerraSceneUnboundedV1","objects":{}})"}) {
				const auto result = Runtime::ValidateArtifact({ByteView(scene), Runtime::ArtifactKind::Scene, Runtime::HostNativeImageTarget(), "Scenes/Invalid.tescene"});
				context.Expect(!result.IsValid() && result.code == "artifact.scene.envelope", "Required scene structure must agree with the production format gate.");
			}

			ArtifactWorkspace workspace; workspace.Write("Meshes/Invalid.temf", "unfinished mesh");
			std::vector<ExportContentInventoryEntry> inventory; std::string error;
			context.Expect(!BuildExportContentInventory(workspace.Root(), inventory, error), "Staging evidence must independently reject malformed required formats.");
		});
		registry.Add("Editor.ExportArtifactValidation.StagingEvidenceAgreement", [](TestContext& context) {
			ArtifactWorkspace workspace;
			workspace.Write("Project/game.json", R"({"name":"PlaceholderDoor"})");
			workspace.Write("Project/Assets/Scenes/Startup.tescene", R"({"format":"TerraSceneUnboundedV1","name":"PlaceholderDoor","objects":[]})");
			workspace.Write("Project/Assets/UI/PlaceholderDoor.teui", R"({"root":{"text":"pLaCeHoLdEr"}})");
			ExportRequest request; request.projectRoot = workspace.Root() / "Project";
			request.profile.name = "PlaceholderDoor"; request.profile.id = "artifact-corpus";
			request.profile.platform = GetHostExportPlatform(); request.profile.enableManagedCompilation = false;
			request.profile.startupScene = "Scenes/Startup.tescene"; request.profile.compressionPreset = CompressionPreset::None;
			request.finalOutputDirectory = workspace.Root() / "Output"; request.evidenceDirectory = workspace.Root() / "Evidence";
			request.runId = "artifact-corpus"; request.runtimeLayout = InstalledLayout();
			const auto result = ExportPipeline{}.Execute(request);
			context.Expect(result.success, "The actual staging/package/publication path must accept legal authored words: " + result.message);
			std::ifstream input(request.evidenceDirectory / "content-inventory.json");
			const auto evidence = nlohmann::json::parse(input);
			context.Expect(!evidence["entries"].empty(), "Successful staging must retain durable content identities.");
			for(const auto& entry : evidence["entries"]) context.Expect(entry.contains("placeholderDetected") && !entry["placeholderDetected"].get<bool>(), "Compatible schema flags must not classify ordinary words as explicit stubs.");
		});
		registry.Add("Editor.ExportArtifactValidation.PackagePolicyAgreement", [](TestContext& context) {
			ArtifactWorkspace workspace;
			auto manifest = WritePackage(workspace, HostImage());
			const auto package = Pack(workspace, manifest);
			const auto report = InspectExportPackage(package, GetHostExportPlatform());
			context.Expect(report.passed, "Valid scene/settings/material/cooked/native word corpus must pass production package inspection: " + report.summaryText);
			manifest["physics"]["colliders"][0]["checksum"] = "ffffffffffffffff";
			context.Expect(!InspectExportPackage(Pack(workspace, manifest), GetHostExportPlatform()).passed, "Accepting legal words must preserve manifest checksum rejection.");
		});
		registry.Add("Editor.ExportArtifactValidation.RuntimeStartupAgreement", [](TestContext& context) {
			ArtifactWorkspace workspace; const auto manifest = WritePackage(workspace, HostImage());
			const auto package = Pack(workspace, manifest); auto& mount = ContentMount::Instance();
			struct Unmount { ~Unmount() { ContentMount::Instance().Unmount(); } } unmount;
			context.Expect(mount.Mount(package), "Runtime corpus TEPAK must mount before inspection.");
			const auto physics = Runtime::LoadPackagedPhysicsRuntime(mount);
			context.Expect(physics.loaded && physics.managedBindingsLoaded, "The actual mounted runtime loader must accept legal words in all typed payloads: " + physics.diagnostic);
			Runtime::ExportStartupCoordinatorInput input; input.exportRuntime = true; input.contentMounted = true; input.metadataLoaded = true;
			input.startupSceneConfigured = true; input.startupSceneLoaded = true; input.packagedPhysics = physics;
			input.requiredSystems = {{"Scripting", true, true, "Real bridge readiness is supplied by the caller"}};
			context.Expect(Runtime::CoordinateExportStartup(input).CanEnterRuntime(), "Valid structural content plus ready required systems must permit runtime entry.");
			input.requiredSystems[0].initialized = false;
			context.Expect(!Runtime::CoordinateExportStartup(input).CanEnterRuntime(), "Native structure cannot bypass actual required bridge readiness.");
			mount.Unmount(); workspace.Write("Managed/Assembly-CSharp.dll", "unfinished gameplay image");
			context.Expect(mount.Mount(Pack(workspace, manifest)), "Structural-negative corpus must remain a readable archive.");
			context.Expect(!Runtime::LoadPackagedPhysicsRuntime(mount).loaded, "Startup must reject text gameplay even without any guessed stub marker.");
		});

	}
}
