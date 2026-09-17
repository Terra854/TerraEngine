// NavigationExportTests.cpp - checked source-free navigation packaging.

#include "TestHarness.h"

#include "NavigationExportDependencies.h"
#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Utilities/UUID.h"

#include <filesystem>
#include <fstream>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::filesystem::path MakeRoot()
		{
			const auto root = std::filesystem::temp_directory_path()
				/ ("terra-navigation-export-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
			std::filesystem::create_directories(root / "Assets");
			return root;
		}

		bool WriteBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
		{
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if(!output) return false;
			output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			return output.good();
		}
	}

	void RegisterNavigationExportTests(TestRegistry& registry)
	{
		registry.Add("Editor.NavigationExport.DiscoversCompanionsCachesAndNotice", [](TestContext& context) {
			const auto root = MakeRoot();
			TerraEngine::Navigation::NavigationGeometryDocument geometry{};
			geometry.kind = TerraEngine::Navigation::NavigationGeometryPayloadKind::TriangleMesh;
			geometry.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
			geometry.triangles = {{0U, 1U, 2U}};
			const auto encoded = TerraEngine::Navigation::EncodeNavigationGeometry(geometry);
			context.Expect(encoded.accepted && WriteBytes(root / "Assets" / "ground.tenavgeom", encoded.bytes),
				"The export fixture must contain one real checked geometry companion.");
			context.Expect(WriteBytes(root / "Assets" / "ground.tenav", {std::byte{1}, std::byte{2}}),
				"The export fixture must contain one optional cache candidate.");

			const auto closure = TerraEngine::Export::NavigationExportDependencyResolver::Resolve(root);
			context.Expect(closure.Complete() && closure.sourceFree && closure.dependencies.size() == 2U,
				"Export must discover required geometry and optional cache dependencies.");
			std::string error;
			context.Expect(TerraEngine::Export::NavigationExportDependencyResolver::WriteRuntimeManifest(
				closure, root, error), "A complete closure must write its source-free runtime manifest and notice.");
			std::ifstream manifest(root / "navigation" / "runtime-manifest.json", std::ios::binary);
			const std::string text((std::istreambuf_iterator<char>(manifest)), {});
			manifest.close();
			context.Expect(text.find("Recast Navigation") != std::string::npos
				&& text.find("ground.tenavgeom") != std::string::npos,
				"The archive input manifest must retain dependency and third-party notice provenance.");
			std::filesystem::remove_all(root);
		});

		registry.Add("Editor.NavigationExport.AuthoredSourceFilesAreNotRuntimeDependencies", [](TestContext& context) {
			const auto root = MakeRoot();
			std::ofstream(root / "Assets" / "broken.tenavsource") << "authored source";
			const auto closure = TerraEngine::Export::NavigationExportDependencyResolver::Resolve(root);
			context.Expect(closure.Complete() && closure.sourceFree && closure.dependencies.empty(),
				"Authored navigation source files must not participate in runtime export dependency discovery.");
			std::string error;
			context.Expect(TerraEngine::Export::NavigationExportDependencyResolver::WriteRuntimeManifest(
				closure, root, error), "Source-free export must write a valid runtime manifest.");
			std::filesystem::remove_all(root);
		});

		registry.Add("Editor.NavigationExport.SourceFreeProjectRemainsValid", [](TestContext& context) {
			const auto root = MakeRoot();
			const auto closure = TerraEngine::Export::NavigationExportDependencyResolver::Resolve(root);
			context.Expect(closure.Complete() && closure.sourceFree && closure.dependencies.empty(),
				"A project without navigation sources must export and initialize without fabricated terrain or geometry.");
			std::filesystem::remove_all(root);
		});
	}
}
