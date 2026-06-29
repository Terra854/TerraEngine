#include "TestHarness.h"

#include "ExportPackageValidator.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "TEPAK/PackWriter.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		class TempVirtualGeometryExportWorkspace
		{
		public:
			TempVirtualGeometryExportWorkspace()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "VirtualGeometryExport")
			{
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(InputRoot());
				std::filesystem::create_directories(OutputRoot());
			}

			~TempVirtualGeometryExportWorkspace()
			{
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] std::filesystem::path InputRoot() const
			{
				return root_ / "Input";
			}

			[[nodiscard]] std::filesystem::path OutputRoot() const
			{
				return root_ / "Output";
			}

			[[nodiscard]] std::filesystem::path PackPath() const
			{
				return OutputRoot() / "VirtualGeometry.tepak";
			}

			void WriteText(const std::filesystem::path& relativePath, const std::string& contents) const
			{
				const std::filesystem::path fullPath = InputRoot() / relativePath;
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream stream(fullPath, std::ios::binary);
				stream << contents;
			}

			void WriteBytes(const std::filesystem::path& relativePath, const std::vector<std::uint8_t>& bytes) const
			{
				const std::filesystem::path fullPath = InputRoot() / relativePath;
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream stream(fullPath, std::ios::binary);
				stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			}

			void CreatePack() const
			{
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

		void WriteRequiredRuntimeEntries(const TempVirtualGeometryExportWorkspace& workspace)
		{
			const std::vector<std::string> requiredFiles = {
				"Fonts/Roboto-Medium.ttf",
				"Shaders/Default.task.tspf",
				"Shaders/Default.mesh.tspf",
				"Shaders/Default.frag.tspf",
				"Shaders/Editor/SceneViewGrid.vert.spv",
				"Shaders/Editor/SceneViewGrid.frag.spv",
				"Shaders/Sky/SkyBackground.task.tspf",
				"Shaders/Sky/SkyBackground.mesh.tspf",
				"Shaders/Sky/SkyBackground.frag.tspf",
				"Shaders/UI/UI_Instanced.vert.spv",
				"Shaders/UI/UI_Instanced.frag.spv",
				"Shaders/UI/UI_Static.vert.spv",
				"Shaders/UI/UI_Static.frag.spv",
				"Shaders/UI/UI_SDF_Text.vert.spv",
				"Shaders/UI/UI_SDF_Text.frag.spv",
				"Shaders/Lighting/ClusterDepthMask.comp.spv",
				"Shaders/Lighting/ClusterGrid.comp.spv",
				"Shaders/Lighting/ClusterLightCount.comp.spv",
				"Shaders/Lighting/ClusterLightFill.comp.spv",
				"Shaders/Lighting/ClusterLightPrefix.comp.spv",
				"Shaders/Lighting/LightCull.comp.spv",
				"Shaders/PostProcess/HiZGenerate.comp.spv",
				"Shaders/PostProcess/VRSGenerate.comp.spv",
				"Shaders/PostProcess/GTAO.comp.spv",
				"Shaders/PostProcess/GTAOBlur.comp.spv",
			};

			for (const std::string& file : requiredFiles)
			{
				workspace.WriteText(file, "required runtime payload");
			}
		}

		std::vector<std::uint8_t> MakeVirtualGeometryTemf(bool currentPayload)
		{
			std::vector<std::uint8_t> bytes(sizeof(MeshHeader) + sizeof(VirtualGeometryHeader), 0u);
			MeshHeader header{};
			const std::uint32_t magic = kMeshMagicValue;
			std::memcpy(header.magic, &magic, sizeof(magic));
			header.version = kMeshCurrentVersion;
			header.headerSize = sizeof(MeshHeader);
			header.meshletFlags = kMeshletHeaderFlagHasVirtualGeometry;
			header.virtualGeometryHeaderOffset = sizeof(MeshHeader);
			header.virtualGeometryHeaderSize = sizeof(VirtualGeometryHeader);
			header.virtualGeometryPayloadGeneration = currentPayload ? kVirtualGeometryPayloadGeneration : 999u;
			std::memcpy(bytes.data(), &header, sizeof(header));

			VirtualGeometryHeader virtualHeader{};
			virtualHeader.payloadGeneration = currentPayload ? kVirtualGeometryPayloadGeneration : 999u;
			virtualHeader.headerSize = sizeof(VirtualGeometryHeader);
			virtualHeader.detailLevelCount = 1u;
			virtualHeader.sourceTriangleCount = 12u;
			virtualHeader.sourceVertexCount = 8u;
			std::memcpy(bytes.data() + sizeof(MeshHeader), &virtualHeader, sizeof(virtualHeader));
			return bytes;
		}

		void WriteVirtualGeometryManifest(const TempVirtualGeometryExportWorkspace& workspace)
		{
			workspace.WriteText(
				"game.json",
				"{\n"
				"\t\"name\": \"Virtual Geometry Export Test\",\n"
				"\t\"virtualGeometry\": {\n"
				"\t\t\"quality\": \"medium\",\n"
				"\t\t\"diagnosticsEnabled\": true,\n"
				"\t\t\"requiredShaders\": [\n"
				"\t\t\t\"Shaders/Default.task.tspf\",\n"
				"\t\t\t\"Shaders/Default.mesh.tspf\",\n"
				"\t\t\t\"Shaders/Default.frag.tspf\",\n"
				"\t\t\t\"Shaders/PostProcess/HiZGenerate.comp.spv\"\n"
				"\t\t]\n"
				"\t},\n"
				"\t\"assets\": [\n"
				"\t\t{ \"guid\": \"00000000000000000000000000000002\", \"path\": \"Meshes/Rock.temf\", \"type\": 0, \"virtualGeometry\": { \"required\": true } }\n"
				"\t]\n"
				"}\n");
		}
	}

	void RegisterVirtualGeometryExportTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.Export.ValidatesPackagedTemfPayloads", [](TestContext& context) {
			TempVirtualGeometryExportWorkspace workspace;
			WriteRequiredRuntimeEntries(workspace);
			WriteVirtualGeometryManifest(workspace);
			workspace.WriteBytes("Meshes/Rock.temf", MakeVirtualGeometryTemf(true));
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), ExportPlatform::Windows, error);

			context.Expect(valid, error.empty() ? "Virtual geometry export package should validate." : error);
			context.Expect(error.empty(), "Successful virtual geometry export validation should not leave an error string.");
		});

		registry.Add("VirtualGeometry.Editor.Export.RejectsStaleTemfPayloads", [](TestContext& context) {
			TempVirtualGeometryExportWorkspace workspace;
			WriteRequiredRuntimeEntries(workspace);
			WriteVirtualGeometryManifest(workspace);
			workspace.WriteBytes("Meshes/Rock.temf", MakeVirtualGeometryTemf(false));
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), ExportPlatform::Windows, error);

			context.Expect(!valid, "Stale virtual geometry TEMF payloads should fail export validation.");
			context.Expect(error.find("stale") != std::string::npos || error.find("current") != std::string::npos, "The validation error should explain that the virtual geometry payload must be current.");
		});

		registry.Add("VirtualGeometry.Editor.Export.RequiresMeshShaderResources", [](TestContext& context) {
			TempVirtualGeometryExportWorkspace workspace;
			WriteRequiredRuntimeEntries(workspace);
			WriteVirtualGeometryManifest(workspace);
			workspace.WriteBytes("Meshes/Rock.temf", MakeVirtualGeometryTemf(true));
			std::filesystem::remove(workspace.InputRoot() / "Shaders" / "Default.task.tspf");
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), ExportPlatform::Windows, error);

			context.Expect(!valid, "Virtual geometry export validation should fail without the mesh shader task payload.");
			context.Expect(error.find("default.task.tspf") != std::string::npos, "The validation error should name the missing task shader.");
		});
	}
}
