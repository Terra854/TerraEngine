#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "MeshCompiler.h"
#include "Resource/ResourceManager.h"
#include "Resource/VirtualGeometryCodec.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
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

	RawMeshFixture MakeDisconnectedPanelMesh(
		std::uint32_t panelCount,
		std::uint32_t quadsPerSide)
	{
		RawMeshFixture fixture;
		// Imported environments commonly contain many disconnected props and
		// hard-surface panels. Build each panel with unique vertices so every
		// island has a real open border and exercises the whole-mesh fallback
		// instead of accidentally sharing topology with its neighbor.
		for (std::uint32_t panel = 0u; panel < panelCount; ++panel)
		{
			const std::uint32_t vertexBase = static_cast<std::uint32_t>(fixture.vertices.size());
			const std::uint32_t verticesPerSide = quadsPerSide + 1u;
			const float panelOffsetX = static_cast<float>(panel % 4u) * 3.0f;
			const float panelOffsetY = static_cast<float>(panel / 4u) * 3.0f;
			for (std::uint32_t y = 0u; y < verticesPerSide; ++y)
			{
				for (std::uint32_t x = 0u; x < verticesPerSide; ++x)
				{
					const float fx = static_cast<float>(x) / static_cast<float>(quadsPerSide);
					const float fy = static_cast<float>(y) / static_cast<float>(quadsPerSide);
					RawMeshVertex vertex{};
					vertex.position = {
						panelOffsetX + fx * 2.0f,
						panelOffsetY + fy * 2.0f,
						0.03f * std::sin((fx + static_cast<float>(panel)) * 9.0f) * std::cos(fy * 7.0f)
					};
					vertex.normal = { 0.0f, 0.0f, 1.0f };
					vertex.uv = { fx, fy };
					fixture.vertices.push_back(vertex);
				}
			}

			const auto VertexIndex = [vertexBase, verticesPerSide](std::uint32_t x, std::uint32_t y) {
				return vertexBase + y * verticesPerSide + x;
			};
			for (std::uint32_t y = 0u; y < quadsPerSide; ++y)
			{
				for (std::uint32_t x = 0u; x < quadsPerSide; ++x)
				{
					const std::uint32_t a = VertexIndex(x, y);
					const std::uint32_t b = VertexIndex(x + 1u, y);
					const std::uint32_t c = VertexIndex(x, y + 1u);
					const std::uint32_t d = VertexIndex(x + 1u, y + 1u);
					fixture.indices.insert(fixture.indices.end(), { a, c, b, b, c, d });
				}
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

	bool IsStrictlyDescending(const std::vector<float>& values)
	{
		for (std::size_t index = 1u; index < values.size(); ++index)
		{
			if (!(values[index] < values[index - 1u]))
			{
				return false;
			}
		}
		return true;
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
		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.DefaultAdaptiveHalfOctaveTargetsAreRecorded", [](TestContext& context) {
			const RawMeshFixture fixture = MakeGridMesh(48u);
			const std::filesystem::path output = MakeTempOutputPath("AdaptiveHalfOctaveGrid");
			MeshCompileReport report{};

			context.Expect(CompileMeshFromRaw("VirtualGeometry/AdaptiveHalfOctaveGrid.obj", output, fixture.vertices, fixture.indices, true, &report), "Adaptive half-octave fixture should compile.");
			context.Expect(report.virtualGeometry.status == VirtualGeometryCompileStatus::Cooked, "Large fixture should cook adaptive virtual geometry.");
			context.ExpectEqual(static_cast<std::size_t>(8u), report.virtualGeometry.adaptiveSettings.targetRatios.size(), "Default adaptive cook settings should expose eight half-octave ratios.");
			context.Expect(std::abs(report.virtualGeometry.adaptiveSettings.targetRatios[1] - 0.70710677f) < 0.0001f, "Second default target should be a half-octave ratio.");
			context.ExpectEqual(0.0f, report.virtualGeometry.adaptiveSettings.minimumUsefulTriangleReduction, "Default imports should not require a minimum triangle-reduction percentage.");
			context.ExpectEqual(0.0f, report.virtualGeometry.adaptiveSettings.minimumUsefulMeshletReduction, "Default imports should not require a minimum meshlet-reduction percentage.");
			context.ExpectEqual(1u, report.virtualGeometry.adaptiveSettings.minimumSourceTriangles, "Default imports should attempt valid simplification for every nonempty mesh.");
			context.ExpectEqual(report.virtualGeometry.adaptiveSettings.targetRatios.size(), report.virtualGeometry.requestedTargets.size(), "Every requested target should be represented in cook diagnostics.");
			context.Expect(report.virtualGeometry.generatedLevels.size() >= 4u, "Large fixture should generate smoother intermediate detail levels.");

			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr, "Adaptive half-octave fixture should load for record inspection.");
			context.ExpectEqual(report.virtualGeometry.requestedTargets.size(), mesh->virtualGeometryRequestedTargets.size(), "TEMF should serialize every requested adaptive target.");
			context.Expect(mesh->virtualGeometryDetailLevels.size() >= 4u, "Loaded TEMF should contain the generated adaptive detail levels.");
			context.Expect(mesh->virtualGeometryDetailLevels[1].requestedTargetIndex >= 1u, "Generated detail should retain the requested target index that produced it.");
			bool verifiedCoarseSpacingFloor = false;
			for (std::size_t detailIndex = 1u; detailIndex < mesh->virtualGeometryDetailLevels.size(); ++detailIndex)
			{
				const VirtualGeometryDetailLevelRecord& detail = mesh->virtualGeometryDetailLevels[detailIndex];
				const VirtualGeometryDetailLevelRecord& previousDetail = mesh->virtualGeometryDetailLevels[detailIndex - 1u];
				const float retainedRatio = std::clamp(detail.targetRatio, 0.001f, 1.0f);
				const float expectedSpacingFloor = std::clamp(((1.0f - retainedRatio) / retainedRatio) * 0.0075f, 0.0f, 0.1f);
				context.Expect(detail.geometricError + 0.00001f >= expectedSpacingFloor, "Cooked error metadata should space progressively coarser LOD transitions instead of saturating at one threshold.");
				const float expectedAdjacentSpacingFloor = std::min(previousDetail.geometricError * 1.5f, 0.05f);
				context.Expect(detail.geometricError + 0.00001f >= expectedAdjacentSpacingFloor, "Adjacent cooked LODs should keep their screen-error transitions spatially separated even when simplification returns the same error.");
				context.Expect(detail.primitiveCount < previousDetail.primitiveCount || detail.meshletCount < previousDetail.meshletCount, "Every emitted LOD should reduce triangles or meshlets; exact regional duplicates belong in merged-target diagnostics.");
				verifiedCoarseSpacingFloor = verifiedCoarseSpacingFloor || retainedRatio <= 0.5f;
			}
			context.Expect(verifiedCoarseSpacingFloor, "Transition-spacing regression should inspect at least one half-resolution-or-coarser detail.");
			context.ExpectEqual(mesh->virtualGeometryDetailLevels.size(), report.virtualGeometry.generatedLevels.size(), "Inspector generated-level rows should match the distinct runtime LOD count after duplicate regional candidates are merged.");
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.GeneratesLodChainForHighDetailMesh", [](TestContext& context) {
			const RawMeshFixture fixture = MakeGridMesh(48u);
			const std::filesystem::path output = MakeTempOutputPath("HighDetailGrid");
			MeshCompileReport report{};

			context.Expect(CompileMeshFromRaw("VirtualGeometry/HighDetailGrid.obj", output, fixture.vertices, fixture.indices, true, &report), "High-detail virtual geometry fixture should compile.");
			context.Expect(report.virtualGeometry.status == VirtualGeometryCompileStatus::Cooked, "High-detail meshes should cook a virtual geometry payload.");
			context.ExpectEqual(static_cast<std::uint32_t>(fixture.indices.size() / 3u), report.virtualGeometry.sourceTriangleCount, "Cook report should preserve the source triangle count.");
			context.Expect(report.virtualGeometry.detailLevelCount >= 2u, "High-detail meshes should produce more than one detail level.");
			context.Expect(report.virtualGeometry.cullableGroupCount > 0u, "Cooked virtual geometry should expose cullable groups for the renderer.");
			context.Expect(report.virtualGeometry.clusters.size() >= report.virtualGeometry.detailLevelCount, "Cook report should expose at least one spatial cluster per generated detail level.");
			context.Expect(report.virtualGeometry.clusters.size() > report.virtualGeometry.detailLevelCount, "High-detail fixtures should split dense LODs into multiple spatial clusters.");
			context.Expect(report.virtualGeometry.selectedFallbackAvailable != 0u, "Cooked virtual geometry should keep a fallback detail available.");

			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr, "Compiled virtual geometry TEMF should load through ResourceManager.");
			context.Expect(mesh->hasVirtualGeometry, "Loaded mesh should advertise virtual geometry metadata.");
			context.Expect((mesh->runtimeLayout.meshletFlags & kMeshletHeaderFlagHasVirtualGeometry) != 0u, "TEMF header should carry the virtual geometry flag.");
			context.Expect(!mesh->virtualGeometryPageStreamingReady,
				"A static owner-stream cook must not masquerade as page-local deformable hierarchy data.");
			context.Expect(mesh->virtualGeometryOwnerFallbackReason.find("do not provide complete hierarchy page identities") != std::string::npos,
				"A page-identity mismatch should explain why the validated owning-mesh payload is active.");
			context.Expect(std::all_of(
				mesh->virtualGeometryPhysicalPageStates.begin(),
				mesh->virtualGeometryPhysicalPageStates.end(),
				[](std::uint32_t state) {
					return state == static_cast<std::uint32_t>(::GeometryPageState::Resident)
						|| state == static_cast<std::uint32_t>(::GeometryPageState::Pinned);
				}), "Owning-mesh fallback should keep every decoded hierarchy page selectable without fake streaming requests.");
			context.ExpectEqual(static_cast<std::size_t>(report.virtualGeometry.detailLevelCount), mesh->virtualGeometryDetailLevels.size(), "Loaded detail table should match the cook report.");
			context.Expect(mesh->virtualGeometryDetailLevels.back().primitiveCount < mesh->virtualGeometryDetailLevels.front().primitiveCount, "Last detail level should use fewer primitives than the source detail.");
			context.Expect(mesh->virtualGeometryPages.size() > mesh->virtualGeometryDetailLevels.size(), "Dense detail levels should split into bounded spatial physical pages.");
			context.ExpectEqual(mesh->virtualGeometryClusters.size(), mesh->virtualGeometryPages.size(), "Every independently streamable page should own one hierarchy cluster.");
			context.ExpectEqual(mesh->virtualGeometryClusters.size(), mesh->virtualGeometryClusterPageLinks.size(), "Every cooked spatial cluster should carry explicit cluster-page coverage.");
			context.Expect((mesh->virtualGeometryHeader.flags & kVirtualGeometryValidationFlagClusterCoverageValidated) != 0u, "Regional virtual geometry cooks should prove cluster coverage before enabling sparse mixed-detail cuts.");
			context.Expect(std::all_of(report.virtualGeometry.clusters.begin(), report.virtualGeometry.clusters.end(), [](const VirtualGeometryClusterCompileReport& cluster) {
				return cluster.materialSectionId == 0u;
			}), "Current raw mesh compiler diagnostics should preserve the default material section on every cluster.");
			context.Expect(std::any_of(mesh->virtualGeometryValidationFlags.begin(), mesh->virtualGeometryValidationFlags.end(), [](const VirtualGeometryValidationFlagRecord& flag) {
				return (flag.flags & kVirtualGeometryValidationFlagClusterCoverageValidated) != 0u;
			}), "Validation records should mirror the cluster coverage proof bit.");
			const auto fallbackPage = std::find_if(mesh->virtualGeometryPages.begin(), mesh->virtualGeometryPages.end(), [](const VirtualGeometryPageRecord& page) {
				return page.fallbackPageId == page.pageId;
			});
			context.Expect(fallbackPage != mesh->virtualGeometryPages.end(), "Cooked pages should declare explicit resident spatial fallback roots.");
			if (fallbackPage != mesh->virtualGeometryPages.end())
			{
				context.ExpectEqual(mesh->virtualGeometryDetailLevels.back().lodIndex, fallbackPage->firstLodIndex, "The coarsest generated detail should be the protected fallback page.");
				context.Expect(fallbackPage->clusterPageLinkCount >= 1u, "Fallback page should declare its spatial cluster-page links.");
				context.Expect(fallbackPage->byteSize > 0u, "Fallback page metadata should carry a non-zero byte estimate for budget planning.");
			}

			bool sawParentChildLink = false;
			for (const VirtualGeometryClusterRecord& parent : mesh->virtualGeometryClusters)
			{
				if (parent.childCount == 0u)
				{
					continue;
				}
				sawParentChildLink = true;
				for (std::uint32_t childOffset = 0u; childOffset < parent.childCount; ++childOffset)
				{
					const VirtualGeometryClusterRecord& child = mesh->virtualGeometryClusters[parent.firstChildClusterId + childOffset];
					context.ExpectEqual(parent.clusterId, child.parentClusterId, "Coverage-linked child clusters should point back to their parent.");
					context.ExpectEqual(parent.clusterId, child.fallbackClusterId, "A child cluster should fall back to the coarser parent that covers the same source region.");
					context.Expect(parent.lodIndex == child.lodIndex + 1u, "Coverage hierarchy should only link adjacent LOD levels.");
				}
			}
			context.Expect(sawParentChildLink, "High-detail fixtures should publish at least one source-coverage parent/child link.");
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.DisconnectedBordersStillGenerateUsefulLods", [](TestContext& context) {
			const RawMeshFixture fixture = MakeDisconnectedPanelMesh(16u, 12u);
			const std::filesystem::path output = MakeTempOutputPath("DisconnectedBorderPanels");
			MeshCompileReport report{};

			context.Expect(CompileMeshFromRaw(
				"VirtualGeometry/DisconnectedBorderPanels.obj",
				output,
				fixture.vertices,
				fixture.indices,
				true,
				&report), "Disconnected panel fixture should compile through the adaptive route.");
			context.Expect(report.virtualGeometry.detailLevelCount >= 4u,
				"Open borders must not freeze a whole environment mesh at one nearly identical LOD.");
			context.Expect(!report.virtualGeometry.generatedLevels.empty(),
				"Disconnected panel cook should publish generated-level diagnostics.");
			if (!report.virtualGeometry.generatedLevels.empty())
			{
				const auto& coarsest = report.virtualGeometry.generatedLevels.back();
				const std::uint32_t sourceTriangles = static_cast<std::uint32_t>(fixture.indices.size() / 3u);
				context.Expect(coarsest.triangleCount < (sourceTriangles * 3u) / 4u,
					"The coarsest disconnected-panel LOD should remove at least one quarter of source triangles.");
				context.Expect(coarsest.geometricError <= report.virtualGeometry.adaptiveSettings.maximumGeometricError,
					"Permissive fallback output must remain inside the authored geometric-error ceiling.");
			}
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.WritesCanonicalProductionDirectory", [](TestContext& context) {
			// Virtual-page assertions require enough source triangles to select the
			// virtual representation. Small fixtures intentionally use direct meshlets.
			const RawMeshFixture fixture = MakeGridMesh(48u);
			const std::filesystem::path output = MakeTempOutputPath("CanonicalProductionDirectory");
			MeshCompileReport report{};
			context.Expect(CompileMeshFromRaw("VirtualGeometry/CanonicalProductionDirectory.obj", output,
				fixture.vertices, fixture.indices, true, &report),
				"Canonical production fixture should compile.");

			std::ifstream file(output, std::ios::binary);
			const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
			const auto validation = TerraEngine::Resource::ValidateTemfProductionFile(bytes, {});
			context.Expect(validation.IsComplete(),
				"A virtual-geometry cook must publish a complete TEMFPRD1 directory instead of an optional sidecar.");
			if (!validation.IsComplete()) return;
			context.Expect(validation.directory.header.formatContractId == kTemfProductionContractId
				&& (validation.directory.header.representationFlags & kTemfRepresentationVirtualGeometry) != 0u
				&& !validation.directory.blocks.empty(),
				"The canonical root must identify TEMFPRD1, virtual geometry, and descriptor-owned blocks.");
			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr && mesh->hasVirtualGeometry
				&& !mesh->virtualGeometryPages.empty()
				&& !mesh->virtualGeometryClusters.empty(),
				"The production decoder must reconstruct virtual hierarchy records without a legacy append payload.");
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.PerMeshMinimumSourceOverrideCanSkipSmallMeshes", [](TestContext& context) {
			const RawMeshFixture fixture = MakeSmallMesh();
			const std::filesystem::path output = MakeTempOutputPath("SmallQuad");
			MeshCompileReport report{};
			VirtualGeometryAdaptiveCompileSettings settings =
				MakeDefaultVirtualGeometryAdaptiveCompileSettings();
			settings.minimumSourceTriangles = 512u;
			settings.settingsSource = "PerMeshOverride";

			context.Expect(CompileMeshFromRaw("VirtualGeometry/SmallQuad.obj", output, fixture.vertices, fixture.indices, true, &report, 60.0f, 1e-6f, &settings), "Small virtual geometry fixture should still compile with a per-mesh threshold.");
			context.Expect(report.virtualGeometry.status == VirtualGeometryCompileStatus::Skipped, "An explicit minimum-source override should report an intentional simplification skip.");
			context.ExpectEqual(1u, report.virtualGeometry.detailLevelCount, "Skipped simplification should still publish the fallback detail.");
			context.Expect(report.virtualGeometry.selectedFallbackAvailable != 0u, "Small meshes should remain renderable through their fallback detail.");
			context.Expect(HasWarningContaining(report.virtualGeometry, "skipped"), "Small mesh cook report should explain that simplification was skipped.");
			context.Expect(!report.virtualGeometry.skippedLevels.empty(), "Small mesh cook report should record skipped adaptive target reasons.");
			context.Expect(report.virtualGeometry.skippedLevels.front().reason == "MeshTooSmall", "Small mesh skip reason should be explicit.");
			context.Expect(report.virtualGeometry.skippedLevels.front().message.find("2 triangles") != std::string::npos, "Small mesh skip message should include the actual source triangle count.");
			context.Expect(report.virtualGeometry.skippedLevels.front().message.find("minimum source threshold") != std::string::npos, "Small mesh skip message should name the threshold that prevented extra LODs.");

			std::ifstream file(output, std::ios::binary);
			const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
			const auto validation = TerraEngine::Resource::ValidateTemfProductionFile(bytes, {});
			context.Expect(validation.IsComplete()
				&& (validation.directory.header.representationFlags & kTemfRepresentationDirectMeshlets) != 0u
				&& (validation.directory.header.representationFlags & kTemfRepresentationVirtualGeometry) == 0u,
				"A tiny mesh must publish the canonical direct-meshlet TEMF route.");
			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr, "Direct-resident small-mesh TEMF should still load.");
			context.Expect(!mesh->hasVirtualGeometry, "Small meshes must not expose virtual pages to the renderer path.");
			context.ExpectEqual(1u, static_cast<std::uint32_t>(mesh->virtualGeometryDetailLevels.size()),
				"Direct-resident meshes must retain the lightweight LOD0 range without publishing virtual pages.");
			if (!mesh->virtualGeometryDetailLevels.empty())
			{
				context.ExpectEqual(0u, mesh->virtualGeometryDetailLevels.front().meshletOffset,
					"Direct-resident LOD0 must begin at the owner meshlet allocation.");
				context.ExpectEqual(mesh->runtimeLayout.meshletCount,
					mesh->virtualGeometryDetailLevels.front().meshletCount,
					"A one-level direct cook must cover the complete owner meshlet allocation.");
			}
			context.Expect(mesh->virtualGeometrySkippedDetails.empty(), "Direct-resident TEMF must not serialize virtual adaptive-detail records.");
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.DirectResidentMeshRetainsGeneratedLodRanges", [](TestContext& context) {
			const RawMeshFixture fixture = MakeGridMesh(24u);
			const std::filesystem::path output = MakeTempOutputPath("DirectResidentLodGrid");
			MeshCompileReport report{};

			context.Expect(CompileMeshFromRaw(
				"VirtualGeometry/DirectResidentLodGrid.obj",
				output,
				fixture.vertices,
				fixture.indices,
				true,
				&report),
				"A low-poly direct-resident grid should compile with automatic LODs.");
			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr && !mesh->hasVirtualGeometry,
				"The sub-4096-triangle fixture must stay on the direct-resident route.");
			if (mesh == nullptr)
			{
				return;
			}
			context.Expect(mesh->virtualGeometryDetailLevels.size() >= 2u,
				"A reducible direct-resident mesh must retain more than one selectable LOD range.");
			std::uint32_t expectedOffset = 0u;
			for (const VirtualGeometryDetailLevelRecord& detail : mesh->virtualGeometryDetailLevels)
			{
				context.ExpectEqual(expectedOffset, detail.meshletOffset,
					"Direct-resident LOD ranges must remain contiguous in the owner allocation.");
				expectedOffset += detail.meshletCount;
			}
			context.ExpectEqual(mesh->runtimeLayout.meshletCount, expectedOffset,
				"Direct-resident LOD ranges must cover the packed allocation exactly once.");
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.SanitizesUnsafeAdaptiveSettings", [](TestContext& context) {
			VirtualGeometryAdaptiveCompileSettings settings;
			settings.targetRatios = { 1.0f, std::numeric_limits<float>::quiet_NaN(), 0.9f, 0.5f, 0.5f, -0.25f, 0.25f };
			settings.minimumSourceTriangles = 0u;
			settings.minimumGeneratedTriangles = 0u;
			settings.minimumMeshletsPerGeneratedLevel = 0u;
			settings.minimumUsefulTriangleReduction = -5.0f;
			settings.minimumUsefulMeshletReduction = 5.0f;
			settings.maximumGeometricError = std::numeric_limits<float>::quiet_NaN();

			const VirtualGeometryAdaptiveCompileSettings sanitized = SanitizeVirtualGeometryAdaptiveCompileSettings(settings);

			context.Expect(!sanitized.targetRatios.empty(), "Sanitized settings should keep at least LOD0.");
			context.ExpectEqual(1.0f, sanitized.targetRatios.front(), "Sanitized ratios should begin with the source detail.");
			context.Expect(IsStrictlyDescending(sanitized.targetRatios), "Sanitized ratios should be strictly descending after LOD0.");
			context.Expect(sanitized.minimumSourceTriangles >= 1u, "Minimum source triangles should clamp to a usable value.");
			context.Expect(sanitized.minimumGeneratedTriangles >= 1u, "Minimum generated triangles should clamp to a usable value.");
			context.Expect(sanitized.minimumMeshletsPerGeneratedLevel >= 1u, "Minimum generated meshlets should clamp to a usable value.");
			context.Expect(sanitized.minimumUsefulTriangleReduction >= 0.0f && sanitized.minimumUsefulTriangleReduction <= 0.95f, "Triangle reduction threshold should clamp to a safe range.");
			context.Expect(sanitized.minimumUsefulMeshletReduction >= 0.0f && sanitized.minimumUsefulMeshletReduction <= 0.95f, "Meshlet reduction threshold should clamp to a safe range.");
			context.Expect(std::isfinite(sanitized.maximumGeometricError), "Maximum geometric error should recover from non-finite input.");
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.ReportsGeneratedAndSkippedAdaptiveTargets", [](TestContext& context) {
			const RawMeshFixture fixture = MakeGridMesh(48u);
			const std::filesystem::path output = MakeTempOutputPath("AdaptiveDiagnosticCoverage");
			MeshCompileReport report{};
			VirtualGeometryAdaptiveCompileSettings settings = MakeDefaultVirtualGeometryAdaptiveCompileSettings();
			settings.targetRatios = { 1.0f, 0.95f, 0.5f, 0.04f };
			settings.minimumGeneratedTriangles = 128u;
			settings.minimumUsefulTriangleReduction = 0.2f;

			context.Expect(CompileMeshFromRaw("VirtualGeometry/AdaptiveDiagnosticCoverage.obj", output, fixture.vertices, fixture.indices, true, &report, 60.0f, 1e-6f, &settings), "Diagnostic coverage fixture should compile.");
			context.ExpectEqual(settings.targetRatios.size(), report.virtualGeometry.requestedTargets.size(), "Every requested diagnostic target should be reported.");
			context.Expect(!report.virtualGeometry.generatedLevels.empty(), "Diagnostics should include at least the source generated level.");
			context.Expect(!report.virtualGeometry.skippedLevels.empty(), "Diagnostics should include non-generated target reasons.");

			bool sawGenerated = false;
			for (const VirtualGeometryRequestedTargetCompileReport& target : report.virtualGeometry.requestedTargets)
			{
				sawGenerated = sawGenerated || target.status == "Generated";
			}
			context.Expect(sawGenerated, "Requested-target diagnostics should identify generated levels.");
			context.Expect(!report.virtualGeometry.skippedLevels.front().reason.empty(), "Skipped diagnostics should explain why the target did not become a level.");
			for (const VirtualGeometrySkippedLevelCompileReport& skipped : report.virtualGeometry.skippedLevels)
			{
				if (skipped.reason == "InsufficientTriangleReduction" || skipped.reason == "InsufficientMeshletReduction")
				{
					context.Expect(skipped.candidateTriangleCount > 0u, "Merged adaptive targets should report the actual candidate triangle count.");
					context.Expect(skipped.candidateMeshletCount > 0u, "Merged adaptive targets should report the actual candidate meshlet count.");
					context.Expect(skipped.actualCandidateRatio > 0.0f, "Merged adaptive targets should report the candidate ratio.");
					context.Expect(skipped.message.find("useful threshold") != std::string::npos, "Merged adaptive targets should explain the useful-reduction threshold.");
				}
			}

			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr, "Adaptive diagnostic fixture should load so report counts can be compared to emitted TEMF records.");
			if (mesh != nullptr)
			{
				context.ExpectEqual(mesh->virtualGeometryDetailLevels.size(), report.virtualGeometry.generatedLevels.size(), "Generated report rows should mirror the emitted detail-level table.");
				for (const VirtualGeometryDetailLevelRecord& detail : mesh->virtualGeometryDetailLevels)
				{
					const auto generated = std::find_if(
						report.virtualGeometry.generatedLevels.begin(),
						report.virtualGeometry.generatedLevels.end(),
						[&detail](const VirtualGeometryGeneratedLevelCompileReport& candidate) {
							return candidate.lodIndex == detail.lodIndex
								&& candidate.requestedTargetIndex == detail.requestedTargetIndex;
						});
					context.Expect(generated != report.virtualGeometry.generatedLevels.end(), "Every emitted adaptive detail should have a generated diagnostic row.");
					if (generated == report.virtualGeometry.generatedLevels.end())
					{
						continue;
					}

					// Regional cluster cooking may rebuild the meshlet payload,
					// so diagnostics must match the final TEMF data rather than
					// the earlier simplification candidate estimate.
					context.ExpectEqual(detail.primitiveCount, generated->triangleCount, "Generated diagnostic triangle count should match the emitted detail.");
					context.ExpectEqual(detail.meshletCount, generated->meshletCount, "Generated diagnostic meshlet count should match the emitted detail.");
					const float emittedRatio = report.virtualGeometry.sourceTriangleCount > 0u
						? static_cast<float>(detail.primitiveCount) / static_cast<float>(report.virtualGeometry.sourceTriangleCount)
						: detail.targetRatio;
					context.Expect(std::abs(emittedRatio - generated->actualRatio) < 0.0001f, "Generated diagnostic ratio should describe the emitted detail.");
				}
			}
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.ValidatesCullableGroupBounds", [](TestContext& context) {
			const RawMeshFixture fixture = MakeGridMesh(48u);
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

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.CooksConeSafetyRecordsForClusters", [](TestContext& context) {
			const RawMeshFixture fixture = MakeGridMesh(48u);
			const std::filesystem::path output = MakeTempOutputPath("ConeSafetyGrid");
			MeshCompileReport report{};

			context.Expect(CompileMeshFromRaw("VirtualGeometry/ConeSafetyGrid.obj", output, fixture.vertices, fixture.indices, true, &report), "Cone safety fixture should compile.");
			context.Expect(report.virtualGeometry.coneSafetyRecordCount > 0u, "Cook report should count cone safety records.");
			context.ExpectEqual(report.virtualGeometry.coneSafetyRecordCount, report.virtualGeometry.clusters.size(), "Cooker should emit one cone safety record per cluster.");
			context.ExpectEqual(report.virtualGeometry.coneSafetyRecordCount, report.virtualGeometry.coneEligibleClusterCount, "Direct raw one-sided opaque fixtures should be eligible for cone culling.");

			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr, "Cone safety fixture should load for TEMF inspection.");
			context.Expect(mesh != nullptr && mesh->virtualGeometryConeSafety.size() == mesh->virtualGeometryClusters.size(), "Loaded TEMF should preserve one cone safety record per cluster.");
			context.Expect(mesh != nullptr && (mesh->virtualGeometryHeader.flags & kVirtualGeometryValidationFlagConeSafetyMetadataValidated) != 0u, "TEMF header should publish the cone safety validation flag.");
			if (mesh != nullptr)
			{
				context.Expect(std::all_of(mesh->virtualGeometryConeSafety.begin(), mesh->virtualGeometryConeSafety.end(), [](const VirtualGeometryConeSafetyRecord& record) {
					return (record.flags & kVirtualGeometryConeSafetyFlagConeValid) != 0u
						&& (record.flags & kVirtualGeometryConeSafetyFlagEligible) != 0u
						&& record.materialSidedness == 0u
						&& record.windingState == 0u
						&& record.alphaRisk == 0u
						&& record.thinShellRisk == 0u;
				}), "Raw mesh cone safety records should prove one-sided opaque low-risk culling.");
			}
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.ConservativeCookDisablesConeSafety", [](TestContext& context) {
			const RawMeshFixture fixture = MakeGridMesh(48u);
			const std::filesystem::path output = MakeTempOutputPath("ConeSafetyConservativeGrid");
			MeshCompileReport report{};
			VirtualGeometryConeSafetyCookSettings coneSettings;
			coneSettings.proveOneSidedOpaqueClusters = false;
			coneSettings.disabledReason = "test importer has not proven material sidedness";

			context.Expect(CompileMeshFromRaw("VirtualGeometry/ConeSafetyConservativeGrid.obj", output, fixture.vertices, fixture.indices, true, &report, 60.0f, 1e-6f, nullptr, {}, &coneSettings), "Conservative cone safety fixture should compile.");
			context.Expect(report.virtualGeometry.coneSafetyRecordCount > 0u, "Conservative cook should still emit cone safety records for diagnostics.");
			context.ExpectEqual(0u, report.virtualGeometry.coneEligibleClusterCount, "Conservative metadata should not arm cone culling.");

			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr, "Conservative cone fixture should load.");
			if (mesh != nullptr)
			{
				context.Expect(std::all_of(mesh->virtualGeometryConeSafety.begin(), mesh->virtualGeometryConeSafety.end(), [](const VirtualGeometryConeSafetyRecord& record) {
					return (record.flags & kVirtualGeometryConeSafetyFlagEligible) == 0u
						&& record.disabledReasonStringSize > 0u;
				}), "Disabled cone safety records should keep visible disabled reasons.");
			}
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.SectionRangesRemainContinuousAcrossLods", [](TestContext& context) {
			const RawMeshFixture fixture = MakeGridMesh(48u);
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
				context.Expect(detail.targetStatus == static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Generated), "Accepted adaptive detail should be marked generated.");
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
			const RawMeshFixture fixture = MakeGridMesh(48u);
			const std::filesystem::path output = MakeTempOutputPath("VisualErrorGrid");
			MeshCompileReport report{};
			VirtualGeometryAdaptiveCompileSettings settings =
				MakeDefaultVirtualGeometryAdaptiveCompileSettings();
			// This regression validates an explicit quality budget. Keep the
			// fixture above the production VG route threshold while asking the
			// cooker to reject any candidate beyond five-percent object error.
			settings.maximumGeometricError = 0.05f;

			context.Expect(CompileMeshFromRaw(
				"VirtualGeometry/VisualErrorGrid.obj",
				output,
				fixture.vertices,
				fixture.indices,
				true,
				&report,
				60.0f,
				1e-6f,
				&settings),
				"Visual-error fixture should compile.");
			Mesh* mesh = LoadCompiledMeshForInspection(output);
			context.Expect(mesh != nullptr, "Visual-error fixture should load.");
			context.Expect(mesh->virtualGeometryDetailLevels.size() >= 2u, "Visual-error fixture should produce multiple LODs.");

			float previousTargetRatio = 2.0f;
			float previousGeometricError = 0.0f;
			std::uint32_t previousPrimitiveCount = mesh->virtualGeometryDetailLevels.front().primitiveCount + 1u;
			for (const VirtualGeometryDetailLevelRecord& detail : mesh->virtualGeometryDetailLevels)
			{
				context.Expect(std::isfinite(detail.geometricError) && detail.geometricError >= previousGeometricError, "Geometric error should be finite and monotonic across LODs.");
				context.Expect(detail.geometricError <= 0.05f, "The quality tolerance should reject overly aggressive simplification error.");
				context.Expect(std::isfinite(detail.targetRatio) && detail.targetRatio > 0.0f && detail.targetRatio <= previousTargetRatio, "Target ratios should move toward lower-detail representations.");
				context.Expect(detail.primitiveCount < previousPrimitiveCount, "Each later LOD should reduce primitive count.");
				previousTargetRatio = detail.targetRatio;
				previousGeometricError = detail.geometricError;
				previousPrimitiveCount = detail.primitiveCount;
			}
		});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.MeshCompiler.CleanCheckoutLargeMeshCookIsDeterministic", [](TestContext& context) {
			const RawMeshFixture fixture = MakeDeterministicLargeStressMesh(48u, 4u);
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
			context.Expect(mesh->virtualGeometryPages.size() > mesh->virtualGeometryDetailLevels.size(), "The physical-page cook should spatially subdivide dense detail levels.");
			context.ExpectEqual(mesh->virtualGeometryClusters.size(), mesh->virtualGeometryPages.size(), "Deterministic spatial pages should preserve one cluster identity per page.");
		});
	}
}
