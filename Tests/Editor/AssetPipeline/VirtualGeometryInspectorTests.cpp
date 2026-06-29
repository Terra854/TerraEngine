#include "TestHarness.h"

#include "AssetDatabase.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::filesystem::path MakeVirtualGeometryInspectorProjectRoot()
		{
			const std::filesystem::path root = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryInspectorTests";
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root / "Assets");
			std::filesystem::create_directories(root / "Build" / "Assets");
			return root;
		}

		void WriteMeshHeader(const std::filesystem::path& path, bool cookedVirtualGeometry)
		{
			MeshHeader header{};
			std::memcpy(header.magic, "TEMF", 4);
			header.version = kMeshCurrentVersion;
			header.headerSize = sizeof(MeshHeader);
			header.meshletCount = 12u;
			if (cookedVirtualGeometry)
			{
				header.meshletFlags = kMeshletHeaderFlagHasVirtualGeometry;
				header.virtualGeometryHeaderOffset = sizeof(MeshHeader);
				header.virtualGeometryHeaderSize = sizeof(VirtualGeometryHeader);
				header.virtualGeometryPayloadGeneration = kVirtualGeometryPayloadGeneration;
			}
			else
			{
				header.meshletFlags = kMeshletHeaderFlagHasVirtualGeometry;
				header.virtualGeometryHeaderOffset = 0u;
				header.virtualGeometryHeaderSize = 0u;
				header.virtualGeometryPayloadGeneration = 0u;
			}

			std::ofstream output(path, std::ios::binary);
			output.write(reinterpret_cast<const char*>(&header), sizeof(header));
			if (cookedVirtualGeometry)
			{
				VirtualGeometryHeader virtualHeader{};
				virtualHeader.payloadGeneration = kVirtualGeometryPayloadGeneration;
				virtualHeader.headerSize = sizeof(VirtualGeometryHeader);
				virtualHeader.detailLevelCount = 3u;
				virtualHeader.cullableGroupCount = 12u;
				virtualHeader.sourceTriangleCount = 2048u;
				output.write(reinterpret_cast<const char*>(&virtualHeader), sizeof(virtualHeader));
			}
		}

		std::string ReadBinaryFile(const std::filesystem::path& path)
		{
			std::ifstream input(path, std::ios::binary);
			return std::string(
				std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>());
		}
	}

	void RegisterVirtualGeometryInspectorTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.Inspector.ReadsCookedMetadata", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			const std::filesystem::path compiledPath = AssetDatabase::Instance().GetBuildRoot() / "mesh.temf";
			WriteMeshHeader(compiledPath, true);

			AssetMetadata metadata{};
			metadata.compiledPath = "mesh.temf";
			VirtualGeometryAssetMetadata virtualGeometry{};
			const bool readable = AssetDatabase::Instance().TryGetVirtualGeometryMetadata(metadata, virtualGeometry);

			context.Expect(readable, "Inspector metadata accessor should read a valid compiled TEMF header.");
			context.Expect(virtualGeometry.hasVirtualGeometry, "Cooked virtual geometry should be reported as present.");
			context.Expect(!virtualGeometry.needsRebuild, "Current payload generation should not request a rebuild.");
			context.ExpectEqual(3u, virtualGeometry.detailLevelCount, "Inspector metadata should expose the cooked detail count.");
			context.ExpectEqual(2048u, virtualGeometry.sourceTriangleCount, "Inspector metadata should expose source triangle count.");
			context.Expect(virtualGeometry.warnings.empty(), "Current cooked metadata should not surface warning rows.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.Inspector.SurfacesRebuildWarnings", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			const std::filesystem::path compiledPath = AssetDatabase::Instance().GetBuildRoot() / "stale.temf";
			WriteMeshHeader(compiledPath, false);

			AssetMetadata metadata{};
			metadata.compiledPath = "stale.temf";
			VirtualGeometryAssetMetadata virtualGeometry{};
			const bool readable = AssetDatabase::Instance().TryGetVirtualGeometryMetadata(metadata, virtualGeometry);

			context.Expect(readable, "Stale virtual geometry headers should remain readable for inspector guidance.");
			context.Expect(virtualGeometry.hasVirtualGeometry, "Stale virtual geometry flags should still be visible to the inspector.");
			context.Expect(virtualGeometry.needsRebuild, "Incomplete virtual geometry metadata should request a rebuild.");
			context.Expect(!virtualGeometry.warnings.empty(), "Stale metadata should surface a visible warning.");
			context.Expect(virtualGeometry.rebuildGuidance.find("Rebuild") != std::string::npos, "Stale metadata should include explicit rebuild guidance for the inspector.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.AssetDatabase.PreservesCompiledMeshOnFailedRebuild", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			const std::filesystem::path sourcePath = AssetDatabase::Instance().GetAssetRoot() / "broken.obj";
			const std::filesystem::path compiledPath = AssetDatabase::Instance().GetBuildRoot() / "broken.temf";
			std::ofstream source(sourcePath, std::ios::trunc);
			source << "this is not a valid obj mesh";
			source.close();
			std::ofstream compiled(compiledPath, std::ios::binary | std::ios::trunc);
			compiled << "previous usable compiled mesh";
			compiled.close();
			const std::string before = ReadBinaryFile(compiledPath);

			AssetDatabase::Instance().ImportAsset(sourcePath, true);
			const std::string after = ReadBinaryFile(compiledPath);

			context.ExpectEqual(before, after, "Failed mesh rebuilds should leave the previous compiled artifact untouched.");
			context.Expect(!std::filesystem::exists(compiledPath.string() + ".previous"), "Successful rollback should not leave a publish backup beside the compiled mesh.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});
	}
}
