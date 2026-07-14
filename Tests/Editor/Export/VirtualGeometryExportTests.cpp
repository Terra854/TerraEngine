#include "TestHarness.h"

#include "ExportPackageValidator.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "TEPAK/PackWriter.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
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

			// Package validation requires every export fixture to name and stage a
			// startup scene, even when this test exercises virtual-geometry data only.
			// The empty object list is a valid minimal serialized-scene payload and
			// deliberately keeps physics optional for these focused tests.
			workspace.WriteText("Scenes/Startup.tescene", R"({"objects":[]})");
		}

		std::string ReadTextFile(const std::filesystem::path& path)
		{
			// This regression test checks the production exporter writer rather
			// than the hand-authored test fixture, so read the source file as a
			// plain text contract and assert the required metadata is present.
			std::ifstream stream(path, std::ios::binary);
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			return buffer.str();
		}

		enum class VirtualGeometryTemfVariant
		{
			Valid,
			StaleGeneration,
			MissingAdaptiveTargets,
			MissingFallbackCoverage,
			MissingPageData,
			MissingConeSafety
		};

		template <typename RecordType>
		std::uint32_t AppendRecord(std::vector<std::uint8_t>& bytes, const RecordType& record)
		{
			const std::uint32_t offset = static_cast<std::uint32_t>(bytes.size());
			const auto* first = reinterpret_cast<const std::uint8_t*>(&record);
			bytes.insert(bytes.end(), first, first + sizeof(RecordType));
			return offset;
		}

		std::vector<std::uint8_t> MakeVirtualGeometryTemf(VirtualGeometryTemfVariant variant)
		{
			std::vector<std::uint8_t> bytes(sizeof(MeshHeader) + sizeof(VirtualGeometryHeader), 0u);
			MeshHeader header{};
			const std::uint32_t magic = kMeshMagicValue;
			std::memcpy(header.magic, &magic, sizeof(magic));
			header.version = kMeshCurrentVersion;
			header.headerSize = sizeof(MeshHeader);
			header.meshletCount = 1u;
			header.meshletFlags = kMeshletHeaderFlagHasVirtualGeometry;
			header.virtualGeometryHeaderOffset = sizeof(MeshHeader);
			header.virtualGeometryHeaderSize = sizeof(VirtualGeometryHeader);
			header.virtualGeometryPayloadGeneration = variant == VirtualGeometryTemfVariant::StaleGeneration ? 999u : kVirtualGeometryPayloadGeneration;

			VirtualGeometryHeader virtualHeader{};
			virtualHeader.payloadGeneration = variant == VirtualGeometryTemfVariant::StaleGeneration ? 999u : kVirtualGeometryPayloadGeneration;
			virtualHeader.headerSize = sizeof(VirtualGeometryHeader);
			virtualHeader.detailLevelCount = 1u;
			virtualHeader.materialSectionCount = 1u;
			virtualHeader.sectionRangeCount = 1u;
			virtualHeader.cullableGroupCount = 1u;
			virtualHeader.pageCount = variant == VirtualGeometryTemfVariant::MissingPageData ? 0u : 1u;
			virtualHeader.requestedTargetCount = 1u;
			virtualHeader.clusterCount = 1u;
			virtualHeader.clusterPageLinkCount = variant == VirtualGeometryTemfVariant::MissingPageData ? 0u : 1u;
			virtualHeader.validationFlagCount = 1u;
			virtualHeader.coneSafetyCount = variant == VirtualGeometryTemfVariant::MissingConeSafety ? 0u : 1u;
			virtualHeader.flags = kVirtualGeometryValidationFlagAdaptiveTargetsPresent
				| kVirtualGeometryValidationFlagFallbackCoverageValidated
				| kVirtualGeometryValidationFlagClusterHierarchyValidated
				| kVirtualGeometryValidationFlagPageLinksValidated
				| kVirtualGeometryValidationFlagConeSafetyMetadataValidated;
			if (variant == VirtualGeometryTemfVariant::MissingAdaptiveTargets)
			{
				virtualHeader.flags &= ~kVirtualGeometryValidationFlagAdaptiveTargetsPresent;
			}
			if (variant == VirtualGeometryTemfVariant::MissingFallbackCoverage)
			{
				virtualHeader.flags &= ~kVirtualGeometryValidationFlagFallbackCoverageValidated;
			}
			if (variant == VirtualGeometryTemfVariant::MissingConeSafety)
			{
				virtualHeader.flags &= ~kVirtualGeometryValidationFlagConeSafetyMetadataValidated;
			}
			virtualHeader.sourceTriangleCount = 12u;
			virtualHeader.sourceVertexCount = 8u;
			VirtualGeometryDetailLevelRecord detail{};
			detail.lodIndex = 0u;
			detail.targetStatus = static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Generated);
			detail.meshletCount = 1u;
			detail.vertexCount = 3u;
			detail.primitiveCount = 1u;
			detail.sectionRangeCount = 1u;
			detail.pageRangeCount = virtualHeader.pageCount;
			detail.targetRatio = 1.0f;
			virtualHeader.detailLevelTableOffset = AppendRecord(bytes, detail);

			VirtualGeometryMaterialSectionRecord section{};
			section.sectionId = 0u;
			section.sourceTriangleCount = 1u;
			virtualHeader.materialSectionTableOffset = AppendRecord(bytes, section);

			VirtualGeometrySectionRangeRecord range{};
			range.lodIndex = 0u;
			range.meshletCount = 1u;
			range.primitiveCount = 1u;
			virtualHeader.sectionRangeTableOffset = AppendRecord(bytes, range);

			VirtualGeometryCullableGroupRecord group{};
			group.vertexCount = 3u;
			group.primitiveCount = 1u;
			group.boundsRadius = 1.0f;
			virtualHeader.cullableGroupTableOffset = AppendRecord(bytes, group);

			VirtualGeometryRequestedTargetRecord target{};
			target.status = static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Generated);
			target.desiredTriangleCount = 12u;
			target.desiredMeshletCount = 1u;
			target.requestedRatio = 1.0f;
			virtualHeader.requestedTargetTableOffset = AppendRecord(bytes, target);

			VirtualGeometryClusterRecord cluster{};
			cluster.parentClusterId = kVirtualGeometryInvalidIndex;
			cluster.firstChildClusterId = kVirtualGeometryInvalidIndex;
			cluster.groupCount = 1u;
			cluster.fallbackClusterId = 0u;
			cluster.boundsRadius = 1.0f;
			virtualHeader.clusterTableOffset = AppendRecord(bytes, cluster);

			std::uint32_t linkOffset = 0u;
			if (variant != VirtualGeometryTemfVariant::MissingPageData)
			{
				VirtualGeometryClusterPageLinkRecord link{};
				link.groupCount = 1u;
				linkOffset = AppendRecord(bytes, link);
				virtualHeader.clusterPageLinkTableOffset = linkOffset;
			}

			if (variant != VirtualGeometryTemfVariant::MissingConeSafety)
			{
				VirtualGeometryConeSafetyRecord cone{};
				cone.groupCount = 1u;
				cone.materialSidedness = 0u;
				cone.windingState = 0u;
				cone.alphaRisk = 0u;
				cone.thinShellRisk = 0u;
				cone.flags = kVirtualGeometryConeSafetyFlagConeValid | kVirtualGeometryConeSafetyFlagEligible;
				virtualHeader.coneSafetyTableOffset = AppendRecord(bytes, cone);
			}

			VirtualGeometryValidationFlagRecord validation{};
			validation.flags = virtualHeader.flags;
			virtualHeader.validationFlagTableOffset = AppendRecord(bytes, validation);

			if (variant != VirtualGeometryTemfVariant::MissingPageData)
			{
				VirtualGeometryPageRecord page{};
				page.lodCount = 1u;
				page.groupCount = 1u;
				page.clusterPageLinkOffset = linkOffset;
				page.clusterPageLinkCount = 1u;
				page.byteSize = 128u;
				virtualHeader.pageTableOffset = AppendRecord(bytes, page);
			}

			std::memcpy(bytes.data(), &header, sizeof(header));
			std::memcpy(bytes.data() + sizeof(MeshHeader), &virtualHeader, sizeof(virtualHeader));
			return bytes;
		}

		void WriteVirtualGeometryManifest(const TempVirtualGeometryExportWorkspace& workspace, bool includeCoverage = true)
		{
			// Export validation now requires the runtime package to declare which
			// hardening fixture categories it covers. Keep the manifest assembled
			// as a string so tests can omit the coverage block for one negative case.
			const std::string coverageBlock = includeCoverage
				? "\t\t\"coverage\": {\n"
					"\t\t\t\"fixtureManifest\": \"Tests/Fixtures/VirtualGeometry/hardening-fixtures.json\",\n"
					"\t\t\t\"requiredCategories\": [\"export\", \"streaming-pressure\", \"multi-view\"]\n"
					"\t\t},\n"
				: "";
			const std::string manifest =
				std::string("{\n") +
				"\t\"name\": \"Virtual Geometry Export Test\",\n"
				"\t\"startupScene\": \"Scenes/Startup.tescene\",\n"
				"\t\"rendererRecovery\": {\n"
				"\t\t\"validationEnabled\": false,\n"
				"\t\t\"diagnosticsEnabled\": true,\n"
				"\t\t\"mode\": \"failClosed\",\n"
				"\t\t\"fatalMessage\": \"Graphics device lost. Restart the application to restore rendering.\",\n"
				"\t\t\"fullApplicationRestart\": false\n"
				"\t},\n"
				"\t\"virtualGeometry\": {\n"
				"\t\t\"quality\": \"medium\",\n"
				"\t\t\"diagnosticsEnabled\": true,\n"
				"\t\t\"geometryBudgetBytes\": 268435456,\n"
				"\t\t\"lodErrorScale\": 1.0,\n"
				"\t\t\"maxLodDrop\": 0,\n"
				"\t\t\"hiZOcclusionEnabled\": true,\n"
				"\t\t\"preloadBias\": 1.0,\n"
				+ coverageBlock +
				"\t\t\"requiredMetadata\": [\"adaptiveTargets\", \"fallbackCoverage\", \"clusterHierarchy\", \"pageLinks\", \"coneSafety\"],\n"
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
				"}\n";
			workspace.WriteText("game.json", manifest);
		}
	}

	void RegisterVirtualGeometryExportTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.Export.PipelineDeclaresConeSafetyMetadata", [](TestContext& context) {
			const std::filesystem::path pipelinePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor" / "Export" / "ExportPipeline.cpp";
			const std::string pipelineSource = ReadTextFile(pipelinePath);
			const std::size_t metadataIndex = pipelineSource.find("\"requiredMetadata\"");
			const std::size_t coneSafetyIndex = metadataIndex == std::string::npos ? std::string::npos : pipelineSource.find("\"coneSafety\"", metadataIndex);
			const std::size_t coverageIndex = pipelineSource.find("\"coverage\"");
			const std::size_t fixtureManifestIndex = coverageIndex == std::string::npos ? std::string::npos : pipelineSource.find("\"fixtureManifest\"", coverageIndex);
			const std::size_t requiredCategoriesIndex = coverageIndex == std::string::npos ? std::string::npos : pipelineSource.find("\"requiredCategories\"", coverageIndex);
			const std::size_t exportCategoryIndex = requiredCategoriesIndex == std::string::npos ? std::string::npos : pipelineSource.find("\"export\"", requiredCategoriesIndex);

			context.Expect(!pipelineSource.empty(), "The export pipeline source should be readable by the editor test target.");
			context.Expect(metadataIndex != std::string::npos, "The export pipeline should still write virtual geometry requiredMetadata.");
			context.Expect(coneSafetyIndex != std::string::npos, "The export pipeline must declare coneSafety so runtime export validation accepts current virtual geometry packages.");
			context.Expect(coverageIndex != std::string::npos, "The export pipeline must declare virtual geometry hardening coverage metadata.");
			context.Expect(fixtureManifestIndex != std::string::npos, "The export pipeline coverage metadata must include the hardening fixture manifest.");
			context.Expect(requiredCategoriesIndex != std::string::npos, "The export pipeline coverage metadata must include requiredCategories.");
			context.Expect(exportCategoryIndex != std::string::npos, "The export pipeline coverage metadata must include the export category required by package validation.");
		});

		registry.Add("VirtualGeometry.Editor.Export.PipelineStagesModelPackageChildren", [](TestContext& context) {
			const std::filesystem::path pipelinePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor" / "Export" / "ExportPipeline.cpp";
			const std::string pipelineSource = ReadTextFile(pipelinePath);
			const bool readsPackageSidecar = pipelineSource.find("LoadModelPackageImportResult") != std::string::npos;
			const bool stagesPackageChildren = pipelineSource.find("stageModelPackageChildren") != std::string::npos;
			const bool treatsPackageParentAsLooseMesh = pipelineSource.find("importer == \"MeshImporter\" || IsModelPackageImporterName(importer)") != std::string::npos;

			context.Expect(!pipelineSource.empty(), "The export pipeline source should be readable by the editor test target.");
			context.Expect(readsPackageSidecar, "The export pipeline must read GLTF/FBX package sidecars before writing runtime asset paths.");
			context.Expect(stagesPackageChildren, "The export pipeline must stage generated model-package children instead of package parent containers.");
			context.Expect(!treatsPackageParentAsLooseMesh, "The export pipeline must not classify GLTF/FBX package parents as loose compiled mesh assets.");
		});

		registry.Add("VirtualGeometry.Editor.Export.RenderDiagnosticsComeFromBuildProfile", [](TestContext& context) {
			const std::filesystem::path pipelinePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor" / "Export" / "ExportPipeline.cpp";
			const std::string pipelineSource = ReadTextFile(pipelinePath);
			const std::size_t resolverIndex = pipelineSource.find("ResolveRenderProfileManifest(const BuildProfile& profile");
			const std::size_t validationProfileFlagIndex = resolverIndex == std::string::npos ? std::string::npos : pipelineSource.find("profile.enableVulkanValidationLayers", resolverIndex);
			const std::size_t debugProfileFlagIndex = resolverIndex == std::string::npos ? std::string::npos : pipelineSource.find("profile.enableRenderDebugDiagnostics", resolverIndex);
			const std::size_t validationRenderSettingsOverrideIndex = pipelineSource.find("(*renderSettings)[\"validationEnabled\"] = profile.enableVulkanValidationLayers");
			const std::size_t debugRenderSettingsOverrideIndex = pipelineSource.find("(*renderSettings)[\"debugEnabled\"] = profile.enableRenderDebugDiagnostics");
			const std::size_t editorValidationSettingIndex = resolverIndex == std::string::npos ? std::string::npos : pipelineSource.find("savedSettings.value(\"validationEnabled\"", resolverIndex);
			const std::size_t editorDebugSettingIndex = resolverIndex == std::string::npos ? std::string::npos : pipelineSource.find("savedSettings.value(\"debugEnabled\"", resolverIndex);

			context.Expect(!pipelineSource.empty(), "The export pipeline source should be readable by the editor test target.");
			context.Expect(resolverIndex != std::string::npos, "Export render-profile manifest creation should receive the selected build profile.");
			context.Expect(validationProfileFlagIndex != std::string::npos, "Exported Vulkan validation must be controlled by the selected build profile.");
			context.Expect(debugProfileFlagIndex != std::string::npos, "Exported render debug diagnostics must be controlled by the selected build profile.");
			context.Expect(validationRenderSettingsOverrideIndex != std::string::npos, "Exported exact render settings must mirror the build profile validation toggle.");
			context.Expect(debugRenderSettingsOverrideIndex != std::string::npos, "Exported exact render settings must mirror the build profile debug diagnostics toggle.");
			context.Expect(editorValidationSettingIndex == std::string::npos, "Exported Vulkan validation must not inherit the editor Preferences toggle.");
			context.Expect(editorDebugSettingIndex == std::string::npos, "Exported render debug diagnostics must not inherit the editor Preferences toggle.");
		});

		registry.Add("VirtualGeometry.Editor.Export.ValidatesPackagedTemfPayloads", [](TestContext& context) {
			TempVirtualGeometryExportWorkspace workspace;
			WriteRequiredRuntimeEntries(workspace);
			WriteVirtualGeometryManifest(workspace);
			workspace.WriteBytes("Meshes/Rock.temf", MakeVirtualGeometryTemf(VirtualGeometryTemfVariant::Valid));
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), ExportPlatform::Windows, error);

			context.Expect(valid, error.empty() ? "Virtual geometry export package should validate." : error);
			context.Expect(error.empty(), "Successful virtual geometry export validation should not leave an error string.");
		});

		registry.Add("VirtualGeometry.Editor.Export.RequiresHardeningCoverageDeclaration", [](TestContext& context) {
			TempVirtualGeometryExportWorkspace workspace;
			WriteRequiredRuntimeEntries(workspace);
			WriteVirtualGeometryManifest(workspace, false);
			workspace.WriteBytes("Meshes/Rock.temf", MakeVirtualGeometryTemf(VirtualGeometryTemfVariant::Valid));
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), ExportPlatform::Windows, error);

			context.Expect(!valid, "Virtual geometry export validation should require broad hardening coverage metadata.");
			context.Expect(error.find("coverage") != std::string::npos, "The validation error should name the missing coverage declaration.");
		});

		registry.Add("VirtualGeometry.Editor.Export.RejectsStaleTemfPayloads", [](TestContext& context) {
			TempVirtualGeometryExportWorkspace workspace;
			WriteRequiredRuntimeEntries(workspace);
			WriteVirtualGeometryManifest(workspace);
			workspace.WriteBytes("Meshes/Rock.temf", MakeVirtualGeometryTemf(VirtualGeometryTemfVariant::StaleGeneration));
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
			workspace.WriteBytes("Meshes/Rock.temf", MakeVirtualGeometryTemf(VirtualGeometryTemfVariant::Valid));
			std::filesystem::remove(workspace.InputRoot() / "Shaders" / "Default.task.tspf");
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), ExportPlatform::Windows, error);

			context.Expect(!valid, "Virtual geometry export validation should fail without the mesh shader task payload.");
			context.Expect(error.find("default.task.tspf") != std::string::npos, "The validation error should name the missing task shader.");
		});

		registry.Add("VirtualGeometry.Editor.Export.RejectsMissingAdaptiveCookMetadata", [](TestContext& context) {
			TempVirtualGeometryExportWorkspace workspace;
			WriteRequiredRuntimeEntries(workspace);
			WriteVirtualGeometryManifest(workspace);
			workspace.WriteBytes("Meshes/Rock.temf", MakeVirtualGeometryTemf(VirtualGeometryTemfVariant::MissingAdaptiveTargets));
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), ExportPlatform::Windows, error);

			context.Expect(!valid, "Export validation should reject virtual geometry payloads without adaptive target validation.");
			context.Expect(error.find("adaptive") != std::string::npos, "The validation error should explain the missing adaptive cook metadata.");
		});

		registry.Add("VirtualGeometry.Editor.Export.RejectsMissingFallbackCoverage", [](TestContext& context) {
			TempVirtualGeometryExportWorkspace workspace;
			WriteRequiredRuntimeEntries(workspace);
			WriteVirtualGeometryManifest(workspace);
			workspace.WriteBytes("Meshes/Rock.temf", MakeVirtualGeometryTemf(VirtualGeometryTemfVariant::MissingFallbackCoverage));
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), ExportPlatform::Windows, error);

			context.Expect(!valid, "Export validation should reject virtual geometry payloads without fallback coverage validation.");
			context.Expect(error.find("fallback") != std::string::npos, "The validation error should explain the missing fallback coverage.");
		});

		registry.Add("VirtualGeometry.Editor.Export.RejectsMissingPageData", [](TestContext& context) {
			TempVirtualGeometryExportWorkspace workspace;
			WriteRequiredRuntimeEntries(workspace);
			WriteVirtualGeometryManifest(workspace);
			workspace.WriteBytes("Meshes/Rock.temf", MakeVirtualGeometryTemf(VirtualGeometryTemfVariant::MissingPageData));
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), ExportPlatform::Windows, error);

			context.Expect(!valid, "Export validation should reject virtual geometry payloads without page metadata.");
			context.Expect(error.find("page") != std::string::npos, "The validation error should explain the missing page metadata.");
		});

		registry.Add("VirtualGeometry.Editor.Export.US6.RejectsMissingConeSafetyMetadata", [](TestContext& context) {
			TempVirtualGeometryExportWorkspace workspace;
			WriteRequiredRuntimeEntries(workspace);
			WriteVirtualGeometryManifest(workspace);
			workspace.WriteBytes("Meshes/Rock.temf", MakeVirtualGeometryTemf(VirtualGeometryTemfVariant::MissingConeSafety));
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), ExportPlatform::Windows, error);

			context.Expect(!valid, "Export validation should reject virtual geometry payloads without cone safety metadata.");
			context.Expect(error.find("cone") != std::string::npos, "The validation error should explain the missing cone safety metadata.");
		});
	}
}
