#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Project/ManagedReadinessManifest.h"
#include "Editor/TerraEngine-Editor/Project/ScriptWorkspaceService.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
	std::filesystem::path CreateTemporaryRoot(const char* suffix)
	{
		const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
		const std::filesystem::path root = std::filesystem::temp_directory_path()
			/ (std::string("TerraEngineManagedReadiness_") + suffix + "_" + std::to_string(tick));
		std::filesystem::create_directories(root);
		return root;
	}

	void WriteFile(const std::filesystem::path& path, const std::string& contents)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << contents;
	}

	ManagedReadinessInputs CreateInputs()
	{
		return {
			"source-1234",
			"workspace-1234",
			"Release",
			"bridge-1234",
			"analyzer-1234",
			"TerraEngine.ManagedRuntimeAbi.v1"
		};
	}
}

namespace TerraEngine::EditorTests
{
	void RegisterManagedReadinessTests(TestRegistry& registry)
	{
		registry.Add("Editor.ManagedReadiness.ManifestPersistsEveryManagedInputAndOutputHash", [](TestContext& context) {
			const std::filesystem::path root = CreateTemporaryRoot("persist");
			const std::filesystem::path output = root / "Assembly-CSharp.dll";
			const std::filesystem::path manifestPath = ManagedReadinessManifest::GetPath(root);
			WriteFile(output, "assembly-v1");

			ManagedReadinessManifest manifest;
			manifest.inputs = CreateInputs();
			manifest.outputFingerprint = ManagedReadinessManifest::FingerprintFile(output);
			std::string error;
			context.Expect(manifest.PersistAtomically(manifestPath, &error), "A complete managed readiness manifest should be atomically persisted: " + error);
			context.Expect(ManagedReadinessManifest::Validate(manifestPath, CreateInputs(), output, nullptr, &error) == ManagedReadinessValidation::Valid,
				"Source, workspace, profile, bridge, analyzer, runtime ABI, and output hashes should validate together: " + error);
			std::filesystem::remove_all(root);
		});

		registry.Add("Editor.ManagedReadiness.ManifestRejectsStaleCorruptAndChangedOutput", [](TestContext& context) {
			const std::filesystem::path root = CreateTemporaryRoot("recovery");
			const std::filesystem::path output = root / "Assembly-CSharp.dll";
			const std::filesystem::path manifestPath = ManagedReadinessManifest::GetPath(root);
			WriteFile(output, "assembly-v1");
			ManagedReadinessManifest manifest;
			manifest.inputs = CreateInputs();
			manifest.outputFingerprint = ManagedReadinessManifest::FingerprintFile(output);
			std::string error;
			context.Expect(manifest.PersistAtomically(manifestPath, &error), "Readiness fixture should persist: " + error);

			ManagedReadinessInputs staleInputs = CreateInputs();
			staleInputs.sourceFingerprint = "source-v2";
			context.Expect(ManagedReadinessManifest::Validate(manifestPath, staleInputs, output, nullptr, &error) == ManagedReadinessValidation::Stale,
				"Changed managed sources should force a rebuild instead of a warm load.");

			WriteFile(output, "assembly-v2");
			context.Expect(ManagedReadinessManifest::Validate(manifestPath, CreateInputs(), output, nullptr, &error) == ManagedReadinessValidation::OutputHashMismatch,
				"A changed managed output DLL should never be direct-loaded through an old manifest.");

			WriteFile(manifestPath, "not-json");
			context.Expect(ManagedReadinessManifest::Validate(manifestPath, CreateInputs(), output, nullptr, &error) == ManagedReadinessValidation::Corrupt,
				"Corrupt derived readiness metadata should fall back to a normal explicit build.");
			std::filesystem::remove_all(root);
		});

		registry.Add("Editor.ManagedReadiness.FingerprintsAreStableAndDistinguishOutputBytes", [](TestContext& context) {
			const std::filesystem::path root = CreateTemporaryRoot("fingerprint");
			const std::filesystem::path output = root / "Assembly-CSharp.dll";
			WriteFile(output, "assembly-v1");
			const std::string first = ManagedReadinessManifest::FingerprintFile(output);
			const std::string repeat = ManagedReadinessManifest::FingerprintFile(output);
			WriteFile(output, "assembly-v2");
			const std::string changed = ManagedReadinessManifest::FingerprintFile(output);
			context.Expect(!first.empty(), "A readable managed output should have a stable cache fingerprint.");
			context.ExpectEqual(first, repeat, "Fingerprinting unchanged managed output should be deterministic.");
			context.Expect(first != changed, "Different managed output bytes must invalidate warm readiness.");
			std::filesystem::remove_all(root);
		});

		registry.Add("Editor.ManagedReadiness.ReferenceCopiesAreConditionalOnValidatedBytes", [](TestContext& context) {
			const std::filesystem::path root = CreateTemporaryRoot("references");
			const std::filesystem::path source = root / "source.dll";
			const std::filesystem::path target = root / "target.dll";
			WriteFile(source, "bridge-v1");
			WriteFile(target, "bridge-v1");
			context.Expect(!ScriptWorkspaceService::RequiresManagedReferenceCopy(source, target),
				"Matching validated bridge bytes should skip a redundant workspace reference copy.");
			WriteFile(target, "bridge-v0");
			context.Expect(ScriptWorkspaceService::RequiresManagedReferenceCopy(source, target),
				"A stale bridge copy should be replaced before managed readiness can be reused.");
			std::filesystem::remove_all(root);
		});
	}
}
