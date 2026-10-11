// Small real-file fixtures verify installed identity without an editor, renderer,
// Engine DLL or fabricated GPU acceptance. Known SHA256 vectors are independent
// of the implementation and pin the portable aggregate encoding.
#include "../TestHarness.h"
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyInstallIdentity.h"
#include <windows.h>
#include <nlohmann/json.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace Editor::Automation;
		namespace fs = std::filesystem;
		using Json = nlohmann::json;
		constexpr const char* kHash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
		constexpr const char* kSealPath = "Tests/Tools/RendererEfficiency/renderer-efficiency-artifacts.json";
		struct Fixture
		{
			fs::path root;
			Json seal;
			Fixture()
			{
				static std::atomic_uint64_t sequence{0};
				root = fs::temp_directory_path() / ("TerraEngine-InstallIdentity-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++sequence));
				seal = {{"schemaVersion", 1}, {"revision", std::string(40, 'a')}, {"sourceDirty", true}, {"artifacts", Json::array()}};
				// Deliberately unsorted rows prove aggregation does not inherit seal order.
				for(const char* path : {"Tests/Fixtures/RendererEfficiency/workloads.json", "Editor/TerraEngine.dll",
					"Intermediate/InstalledShaders/RelWithDebInfo/Default.task.tspf", "Editor/Assets/Shaders/Default.tspf",
					"Editor/Shaders/Lighting/lighting.spv", "Editor/TerraEngine-Editor.exe"})
				{
					Write(path, "abc"); seal["artifacts"].push_back({{"path", path}, {"sha256", kHash}, {"bytes", 3}});
				}
				Save();
			}
			~Fixture()
			{
				// Only this fixture's direct temporary child can be recursively removed.
				std::error_code ignored;
				const auto actual = fs::weakly_canonical(root, ignored), temporary = fs::weakly_canonical(fs::temp_directory_path(), ignored);
				if(!ignored && actual.parent_path() == temporary && actual.filename().string().starts_with("TerraEngine-InstallIdentity-")) fs::remove_all(actual, ignored);
			}
			void Write(const fs::path& path, const std::string& bytes)
			{
				fs::create_directories((root / path).parent_path()); std::ofstream file(root / path, std::ios::binary | std::ios::trunc); file << bytes;
				if(!file.good()) throw std::runtime_error("Identity test fixture write failed.");
			}
			void Save() { Write(kSealPath, seal.dump()); }
			void Reject(TestContext& context)
			{
				RendererEfficiencyInstallIdentity output{fs::path("preserved-root"), "preserved-revision", "binary", "shader", "asset", "closure"}; std::string error;
				context.Expect(!VerifyRendererEfficiencyInstallIdentity(root, output, error), "Invalid actual install closure must be rejected.");
				context.Expect(!error.empty(), "Rejection must explain the invalid identity.");
				context.Expect(output.installRoot == "preserved-root" && output.revision == "preserved-revision" && output.installedBinaryHash == "binary"
					&& output.installedShaderClosureHash == "shader" && output.installedAssetClosureHash == "asset" && output.installedClosureHash == "closure", "Rejected verification must leave all output identity fields unchanged.");
			}
		};
	}
	void RegisterRendererEfficiencyInstallIdentityTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.Editor.InstallIdentity.KnownActualBytesAndOrdinalClosure", [](TestContext& context) {
			Fixture fixture; RendererEfficiencyInstallIdentity output; std::string error;
			const bool valid = VerifyRendererEfficiencyInstallIdentity(fixture.root, output, error); context.Expect(valid, error);
			context.ExpectEqual(std::string("f2b9774c107ea789e4eefb99a461c0b62726a83756571424b7e7c2683114a234"), output.installedBinaryHash, "Binary closure must include both actual binary rows.");
			context.ExpectEqual(std::string("d4c905c5713a12eecde411f9bd107dd44bcb0ab468d24c241c794827678f0452"), output.installedShaderClosureHash, "Shader closure must include compiled and asset shader dependencies.");
			context.ExpectEqual(std::string("18c5aa4c35ada518c010fd22e8ee4c690f1fac218c9e9a4419d9c389410fcab5"), output.installedAssetClosureHash, "Asset closure must include fixture and runtime assets.");
			context.ExpectEqual(std::string(40, 'a'), output.revision, "Dirty seals preserve actual revision plus independent byte identity.");
			const auto first = output;
			// Seal bytes identify the complete receipt, while subset identities stay
			// stable when only seal row ordering changes.
			std::reverse(fixture.seal["artifacts"].begin(), fixture.seal["artifacts"].end()); fixture.Save();
			const bool reorderedValid = VerifyRendererEfficiencyInstallIdentity(fixture.root, output, error); context.Expect(reorderedValid, error);
			context.Expect(output.installedClosureHash != first.installedClosureHash && output.installedBinaryHash == first.installedBinaryHash
				&& output.installedShaderClosureHash == first.installedShaderClosureHash && output.installedAssetClosureHash == first.installedAssetClosureHash, "Seal SHA must reflect actual bytes without changing sorted subset identity.");
		});
		registry.Add("Renderer.Efficiency.Editor.InstallIdentity.ChangedBytesSameLength", [](TestContext& context) { Fixture fixture; fixture.Write("Editor/TerraEngine.dll", "abd"); fixture.Reject(context); });
		registry.Add("Renderer.Efficiency.Editor.InstallIdentity.StreamsAcrossChunkBoundary", [](TestContext& context) {
			Fixture fixture; fixture.Write("Editor/TerraEngine.dll", std::string(65537, 'a'));
			fixture.seal["artifacts"][1]["sha256"] = "008ffc88d3c96a9f307524eb361e47c5222a887fc45fa0c1fb8d429c5c23b430";
			fixture.seal["artifacts"][1]["bytes"] = 65537; fixture.Save(); RendererEfficiencyInstallIdentity output; std::string error;
			const bool valid = VerifyRendererEfficiencyInstallIdentity(fixture.root, output, error); context.Expect(valid, error);
			context.ExpectEqual(std::string("fa70f6b81ecffdac9735ee4e8805ef03a661fbb079442d4a26e65cd89b6dbba2"), output.installedBinaryHash, "All streamed chunks and exact byte count must enter the binary identity.");
		});
		registry.Add("Renderer.Efficiency.Editor.InstallIdentity.ChangedFileLength", [](TestContext& context) { Fixture fixture; fixture.Write("Editor/TerraEngine.dll", "abcd"); fixture.Reject(context); });
		registry.Add("Renderer.Efficiency.Editor.InstallIdentity.DuplicateWindowsPathAlias", [](TestContext& context) {
			Fixture fixture; fixture.seal["artifacts"].push_back({{"path", "editor/TERRAENGINE.dll"}, {"sha256", kHash}, {"bytes", 3}}); fixture.Save(); fixture.Reject(context);
		});
		registry.Add("Renderer.Efficiency.Editor.InstallIdentity.TraversalAndAbsolutePaths", [](TestContext& context) {
			Fixture fixture;
			for(const auto* invalid : {"../outside", "Editor/../TerraEngine.dll", "C:/outside", "Editor\\TerraEngine.dll", "Editor/TerraEngine.dll.", "Editor//TerraEngine.dll"}) {
				fixture.seal["artifacts"][0]["path"] = invalid; fixture.Save(); fixture.Reject(context);
			}
		});
		registry.Add("Renderer.Efficiency.Editor.InstallIdentity.MissingRequiredClosures", [](TestContext& context) {
			Fixture fixture; const auto original = fixture.seal;
			for(unsigned subset = 0; subset < 3; ++subset) {
				fixture.seal = original; auto& rows = fixture.seal["artifacts"];
				for(auto row = rows.begin(); row != rows.end();) {
					const auto path = row->at("path").get<std::string>();
					const bool remove = subset == 0 ? path == "Editor/TerraEngine.dll" : subset == 1 ? path.find("Shaders/") != std::string::npos : path.starts_with("Tests/Fixtures/") || path.starts_with("Editor/Assets/");
					if(remove) row = rows.erase(row); else ++row;
				}
				fixture.Save(); fixture.Reject(context);
			}
		});
		registry.Add("Renderer.Efficiency.Editor.InstallIdentity.MalformedSealAndBounds", [](TestContext& context) {
			Fixture fixture; const auto original = fixture.seal;
			fixture.seal["revision"] = "short"; fixture.Save(); fixture.Reject(context);
			fixture.seal = original; fixture.seal["artifacts"][0]["sha256"] = std::string(64, 'z'); fixture.Save(); fixture.Reject(context);
			fixture.seal = original; fixture.seal["artifacts"][0]["bytes"] = -1; fixture.Save(); fixture.Reject(context);
			fixture.seal = original; fixture.seal["artifacts"][0]["bytes"] = 16ULL * 1024 * 1024 * 1024 + 1; fixture.Save(); fixture.Reject(context);
			fixture.seal = original; fixture.seal["artifacts"] = Json::array();
			for(unsigned index = 0; index <= 65536; ++index) fixture.seal["artifacts"].push_back(original["artifacts"][0]);
			fixture.Save(); fixture.Reject(context);
			fixture.Write(kSealPath, std::string(16 * 1024 * 1024 + 1, ' ')); fixture.Reject(context);
		});
		registry.Add("Renderer.Efficiency.Editor.InstallIdentity.MissingDeclaredFile", [](TestContext& context) {
			Fixture fixture; fs::remove(fixture.root / "Editor/TerraEngine.dll"); fixture.Reject(context);
		});
		registry.Add("Renderer.Efficiency.Editor.InstallIdentity.RejectsSymlink", [](TestContext& context) {
			Fixture fixture; fs::remove(fixture.root / "Editor/TerraEngine.dll");
			fixture.Write("actual.dll", "abc");
			// Developer Mode permits unprivileged link creation; this additional check
			// is skipped when the host cannot create links; the separate missing-file
			// case above always runs.
			if(!CreateSymbolicLinkW((fixture.root / "Editor/TerraEngine.dll").c_str(), (fixture.root / "actual.dll").c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) context.Skip("Host does not permit fixture symlink creation.");
			fixture.Reject(context);
		});
		registry.Add("Renderer.Efficiency.Editor.InstallIdentity.RunningNonEditorCannotClaimInstalledIdentity", [](TestContext& context) {
			RendererEfficiencyInstallIdentity output; output.revision = "preserved"; std::string error;
			context.Expect(!PrepareRendererEfficiencyInstallIdentity(output, error) && !error.empty() && output.revision == "preserved", "A test executable must not claim the installed editor's root.");
		});
	}
}
