#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "MeshCompiler.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::EditorTests
{
namespace
{
	struct RawMeshFixture
	{
		std::vector<RawMeshVertex> vertices;
		std::vector<std::uint32_t> indices;
	};

	std::filesystem::path MakeTempOutputPath(std::string_view name)
	{
		const std::filesystem::path root = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryMeshCompilerTests";
		std::filesystem::create_directories(root);
		return root / std::filesystem::path(std::string(name) + ".temf");
	}

	RawMeshFixture MakeGridMesh(std::uint32_t quadsPerSide)
	{
		RawMeshFixture fixture;
		const std::uint32_t vertexCountPerSide = quadsPerSide + 1u;
		fixture.vertices.reserve(static_cast<std::size_t>(vertexCountPerSide) * static_cast<std::size_t>(vertexCountPerSide));
		fixture.indices.reserve(static_cast<std::size_t>(quadsPerSide) * static_cast<std::size_t>(quadsPerSide) * 6u);

		for (std::uint32_t y = 0; y < vertexCountPerSide; ++y)
		{
			for (std::uint32_t x = 0; x < vertexCountPerSide; ++x)
			{
				RawMeshVertex vertex{};
				const float fx = static_cast<float>(x) / static_cast<float>(quadsPerSide);
				const float fy = static_cast<float>(y) / static_cast<float>(quadsPerSide);
				vertex.position = { fx * 2.0f - 1.0f, fy * 2.0f - 1.0f, 0.05f * std::sin(fx * 12.0f) * std::cos(fy * 10.0f) };
				vertex.normal = { 0.0f, 0.0f, 1.0f };
				vertex.uv = { fx, fy };
				fixture.vertices.push_back(vertex);
			}
		}

		const auto VertexIndex = [vertexCountPerSide](std::uint32_t x, std::uint32_t y) {
			return y * vertexCountPerSide + x;
		};

		for (std::uint32_t y = 0; y < quadsPerSide; ++y)
		{
			for (std::uint32_t x = 0; x < quadsPerSide; ++x)
			{
				const std::uint32_t a = VertexIndex(x, y);
				const std::uint32_t b = VertexIndex(x + 1u, y);
				const std::uint32_t c = VertexIndex(x, y + 1u);
				const std::uint32_t d = VertexIndex(x + 1u, y + 1u);
				fixture.indices.insert(fixture.indices.end(), { a, c, b, b, c, d });
			}
		}

		return fixture;
	}

	RawMeshFixture MakeDeterministicLargeStressMesh(std::uint32_t quadsPerSide, std::uint32_t materialBandCount)
	{
		RawMeshFixture fixture = MakeGridMesh(quadsPerSide);
		if (materialBandCount > 1u)
		{
			for (std::size_t vertexIndex = 0; vertexIndex < fixture.vertices.size(); ++vertexIndex)
			{
				RawMeshVertex& vertex = fixture.vertices[vertexIndex];
				const std::uint32_t band = static_cast<std::uint32_t>(vertexIndex % materialBandCount);
				const float offset = static_cast<float>(band) * 0.0025f;
				vertex.position[2] += offset;
				vertex.uv[0] = std::fmod(vertex.uv[0] + offset, 1.0f);
			}
		}
		return fixture;
	}

	RawMeshFixture MakeSmallMesh()
	{
		RawMeshFixture fixture;
		fixture.vertices = {
			{ { -0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
			{ { 0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
			{ { -0.5f, 0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
			{ { 0.5f, 0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }
		};
		fixture.indices = { 0u, 2u, 1u, 1u, 2u, 3u };
		return fixture;
	}

	bool HasWarningContaining(const VirtualGeometryCompileReport& report, std::string_view needle)
	{
		return std::any_of(report.warnings.begin(), report.warnings.end(), [needle](const std::string& warning) {
			return warning.find(needle) != std::string::npos;
		});
	}

	Mesh* LoadCompiledMeshForInspection(const std::filesystem::path& output)
	{
		ResourceManager& manager = ResourceManager::Instance();
		manager.ClearCaches();
		const TerraEngine::UUID meshId = manager.LoadMesh(output);
		return manager.GetMesh(meshId);
	}
}

	void RegisterVirtualGeometryMeshCompilerTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.GeneratesLodChainForHighDetailMesh", [](TestContext& context) {
			const RawMeshFixture fixture = MakeGridMesh(24u);
			const std::filesystem::path output = MakeTempOutputPath("HighDetailGrid");
			MeshCompileReport report{};

			context.Expect(CompileMeshFromRaw("VirtualGeometry/HighDetailGrid.obj", output, fixture.vertices, fixture.indices, true, &report), "High-detail virtual geometry fixture should compile.");
			context.Expect(report.virtualGeometry.status == VirtualGeometryCompileStatus::Cooked, "High-detail meshes should cook a virtual geometry payload.");
			context.ExpectEqual(static_cast<std::uint32_t>(fixture.indices.size() / 3u), report.virtualGeometry.sourceTriangleCount, "Cook report should preserve the source triangle count.");
			context.Expect(report.virtualGeometry.detailLevelCount >= 2u, "High-detail meshes should produce more than one detail level.");
			context.Expect(report.virtualGeometry.cullableGroupCount > 0u, "Cooked virtual geometry should expose cullable groups for the renderer.");
			context.Expect(report.virtualGeometry.selectedFallbackAvailable != 0u, "Cooked virtual geometry should keep a fallback detail available.");

			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr, "Compiled virtual geometry TEMF should load through ResourceManager.");
			context.Expect(mesh->hasVirtualGeometry, "Loaded mesh should advertise virtual geometry metadata.");
			context.Expect((mesh->header.meshletFlags & kMeshletHeaderFlagHasVirtualGeometry) != 0u, "TEMF header should carry the virtual geometry flag.");
			context.ExpectEqual(static_cast<std::size_t>(report.virtualGeometry.detailLevelCount), mesh->virtualGeometryDetailLevels.size(), "Loaded detail table should match the cook report.");
			context.Expect(mesh->virtualGeometryDetailLevels.back().primitiveCount < mesh->virtualGeometryDetailLevels.front().primitiveCount, "Last detail level should use fewer primitives than the source detail.");
			context.ExpectEqual(mesh->virtualGeometryDetailLevels.size(), mesh->virtualGeometryPages.size(), "Cooked page table should publish one MVP page per detail level.");
			context.Expect(!mesh->virtualGeometryPages.empty() && mesh->virtualGeometryPages.front().fallbackPageId == mesh->virtualGeometryPages.front().pageId, "LOD0 page should be its own resident fallback.");
			context.Expect(mesh->virtualGeometryPages.front().byteSize > 0u, "Page metadata should carry a non-zero byte estimate for budget planning.");
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.SkipsSimplificationForSmallMeshes", [](TestContext& context) {
			const RawMeshFixture fixture = MakeSmallMesh();
			const std::filesystem::path output = MakeTempOutputPath("SmallQuad");
			MeshCompileReport report{};

			context.Expect(CompileMeshFromRaw("VirtualGeometry/SmallQuad.obj", output, fixture.vertices, fixture.indices, true, &report), "Small virtual geometry fixture should still compile.");
			context.Expect(report.virtualGeometry.status == VirtualGeometryCompileStatus::Skipped, "Small meshes should report an intentional simplification skip.");
			context.ExpectEqual(1u, report.virtualGeometry.detailLevelCount, "Skipped simplification should still publish the fallback detail.");
			context.Expect(report.virtualGeometry.selectedFallbackAvailable != 0u, "Small meshes should remain renderable through their fallback detail.");
			context.Expect(HasWarningContaining(report.virtualGeometry, "skipped"), "Small mesh cook report should explain that simplification was skipped.");

			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr, "Skipped virtual geometry TEMF should still load.");
			context.Expect(mesh->hasVirtualGeometry, "Small meshes should still carry one virtual geometry detail for the renderer path.");
			context.ExpectEqual(static_cast<std::size_t>(1), mesh->virtualGeometryDetailLevels.size(), "Loaded small mesh should contain one virtual geometry detail.");
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.ValidatesCullableGroupBounds", [](TestContext& context) {
			const RawMeshFixture fixture = MakeGridMesh(18u);
			const std::filesystem::path output = MakeTempOutputPath("CullableBoundsGrid");
			MeshCompileReport report{};

			context.Expect(CompileMeshFromRaw("VirtualGeometry/CullableBoundsGrid.obj", output, fixture.vertices, fixture.indices, true, &report), "Cullable-group fixture should compile.");
			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr, "Cullable-group fixture should load for inspection.");
			context.Expect(!mesh->virtualGeometryCullableGroups.empty(), "Virtual geometry payload should include cullable groups.");
			context.ExpectEqual(mesh->meshletBounds.size(), mesh->virtualGeometryCullableGroups.size(), "MVP cullable groups should mirror the emitted meshlet bounds.");

			for (const VirtualGeometryCullableGroupRecord& group : mesh->virtualGeometryCullableGroups)
			{
				context.Expect(group.lodIndex < mesh->virtualGeometryDetailLevels.size(), "Cullable group should reference a valid detail level.");
				context.Expect(std::isfinite(group.boundsCenter[0]) && std::isfinite(group.boundsCenter[1]) && std::isfinite(group.boundsCenter[2]), "Cullable group center should be finite.");
				context.Expect(std::isfinite(group.boundsRadius) && group.boundsRadius >= 0.0f, "Cullable group radius should be finite and non-negative.");
				context.Expect(group.primitiveCount > 0u, "Cullable group should describe at least one triangle primitive.");
			}
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.SectionRangesRemainContinuousAcrossLods", [](TestContext& context) {
			const RawMeshFixture fixture = MakeGridMesh(22u);
			const std::filesystem::path output = MakeTempOutputPath("SectionContinuityGrid");
			MeshCompileReport report{};

			context.Expect(CompileMeshFromRaw("VirtualGeometry/SectionContinuityGrid.obj", output, fixture.vertices, fixture.indices, true, &report), "Section continuity fixture should compile.");
			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr, "Section continuity fixture should load.");
			context.ExpectEqual(static_cast<std::size_t>(1), mesh->virtualGeometryMaterialSections.size(), "Current compiler path should preserve one default material section.");
			context.ExpectEqual(mesh->virtualGeometryDetailLevels.size(), static_cast<std::size_t>(mesh->virtualGeometryMaterialSections.front().perLodRangeCount), "Material section should publish one range per generated LOD.");

			std::vector<std::uint8_t> meshletCoverage(mesh->meshletRecords.size(), 0u);
			for (const VirtualGeometryDetailLevelRecord& detail : mesh->virtualGeometryDetailLevels)
			{
				context.ExpectEqual(1u, detail.sectionRangeCount, "Each MVP detail should have exactly one material section range.");
				const VirtualGeometrySectionRangeRecord& range = mesh->virtualGeometrySectionRanges[detail.sectionRangeOffset];
				context.ExpectEqual(detail.lodIndex, range.lodIndex, "Section range should belong to its owning LOD.");
				context.ExpectEqual(detail.meshletOffset, range.meshletOffset, "Section meshlet range should start at the detail meshlet offset.");
				context.ExpectEqual(detail.meshletCount, range.meshletCount, "Section meshlet range should cover the full MVP detail.");

				for (std::uint32_t meshlet = range.meshletOffset; meshlet < range.meshletOffset + range.meshletCount; ++meshlet)
				{
					context.Expect(meshlet < meshletCoverage.size(), "Section range should stay inside the meshlet table.");
					context.Expect(meshletCoverage[meshlet] == 0u, "Section ranges should not overlap meshlets.");
					meshletCoverage[meshlet] = 1u;
				}
			}
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.VisualErrorMetadataStaysWithinTolerance", [](TestContext& context) {
			const RawMeshFixture fixture = MakeGridMesh(28u);
			const std::filesystem::path output = MakeTempOutputPath("VisualErrorGrid");
			MeshCompileReport report{};

			context.Expect(CompileMeshFromRaw("VirtualGeometry/VisualErrorGrid.obj", output, fixture.vertices, fixture.indices, true, &report), "Visual-error fixture should compile.");
			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr, "Visual-error fixture should load.");
			context.Expect(mesh->virtualGeometryDetailLevels.size() >= 2u, "Visual-error fixture should produce multiple LODs.");

			float previousTargetRatio = 2.0f;
			float previousGeometricError = 0.0f;
			std::uint32_t previousPrimitiveCount = mesh->virtualGeometryDetailLevels.front().primitiveCount + 1u;
			for (const VirtualGeometryDetailLevelRecord& detail : mesh->virtualGeometryDetailLevels)
			{
				context.Expect(std::isfinite(detail.geometricError) && detail.geometricError >= previousGeometricError, "Geometric error should be finite and monotonic across LODs.");
				context.Expect(detail.geometricError <= 0.05f, "US2 tolerance should reject overly aggressive simplification error.");
				context.Expect(std::isfinite(detail.targetRatio) && detail.targetRatio > 0.0f && detail.targetRatio <= previousTargetRatio, "Target ratios should move toward lower-detail representations.");
				context.Expect(detail.primitiveCount < previousPrimitiveCount, "Each later LOD should reduce primitive count.");
				previousTargetRatio = detail.targetRatio;
				previousGeometricError = detail.geometricError;
				previousPrimitiveCount = detail.primitiveCount;
			}
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.CleanCheckoutLargeMeshCookIsDeterministic", [](TestContext& context) {
			const RawMeshFixture fixture = MakeDeterministicLargeStressMesh(36u, 4u);
			const std::filesystem::path firstOutput = MakeTempOutputPath("CleanCheckoutLargeMeshA");
			const std::filesystem::path secondOutput = MakeTempOutputPath("CleanCheckoutLargeMeshB");
			MeshCompileReport firstReport{};
			MeshCompileReport secondReport{};

			// The fixture is generated entirely in memory so contributors can run
			// this regression from a clean checkout without downloading a heavy
			// model. The two independent cooks should publish the same contract
			// counters even though the files are written to different paths.
			context.Expect(CompileMeshFromRaw("VirtualGeometry/CleanCheckoutLargeMesh.obj", firstOutput, fixture.vertices, fixture.indices, true, &firstReport), "First deterministic large fixture cook should succeed.");
			context.Expect(CompileMeshFromRaw("VirtualGeometry/CleanCheckoutLargeMesh.obj", secondOutput, fixture.vertices, fixture.indices, true, &secondReport), "Second deterministic large fixture cook should succeed.");
			context.Expect(firstReport.virtualGeometry.status == VirtualGeometryCompileStatus::Cooked, "Large clean-checkout fixture should cook virtual geometry.");
			context.Expect(secondReport.virtualGeometry.status == VirtualGeometryCompileStatus::Cooked, "Repeat clean-checkout fixture should cook virtual geometry.");
			context.ExpectEqual(firstReport.virtualGeometry.detailLevelCount, secondReport.virtualGeometry.detailLevelCount, "Deterministic fixture should generate the same detail count across cooks.");
			context.ExpectEqual(firstReport.virtualGeometry.cullableGroupCount, secondReport.virtualGeometry.cullableGroupCount, "Deterministic fixture should generate the same cullable group count across cooks.");
			context.ExpectEqual(firstReport.virtualGeometry.pageCount, secondReport.virtualGeometry.pageCount, "Deterministic fixture should generate the same page count across cooks.");
			context.Expect(firstReport.virtualGeometry.detailLevelCount >= 3u, "Large stress fixture should produce enough LODs for render stress tests.");

			Mesh* mesh = LoadCompiledMeshForInspection(firstOutput);
			context.Expect(mesh != nullptr, "Deterministic large fixture should load through ResourceManager.");
			context.Expect(mesh->hasVirtualGeometry, "Loaded deterministic fixture should advertise virtual geometry.");
			context.ExpectEqual(mesh->virtualGeometryDetailLevels.size(), mesh->virtualGeometryPages.size(), "Current MVP clean-checkout fixture should emit one page per detail level.");
		});
	}
}
