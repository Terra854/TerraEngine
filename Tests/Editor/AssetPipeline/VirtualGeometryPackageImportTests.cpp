#include "TestHarness.h"

#include "AssetImportResult.h"
#include "FbxTestHelpers.h"
#include "ModelPackageImporter.h"
#include "Resource/ResourceManager.h"
#include "Renderer/RHI/Vulkan/VulkanShadowPagePreparation.h"
#include "ThreadPool/ThreadPool.h"
#include "../../Engine/Runtime/CorrectnessTestSupport.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <fastgltf/base64.hpp>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::filesystem::path MakeVirtualGeometryPackageTempRoot(std::string_view testName)
		{
			const std::filesystem::path root = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryPackageImportTests" / std::string(testName);
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root);
			return root;
		}

		nlohmann::json LoadBaseGltfFixture()
		{
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path fixturePath = sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf";
			std::ifstream input(fixturePath);
			return nlohmann::json::parse(input);
		}

		std::filesystem::path WriteGltfFixture(const std::filesystem::path& tempRoot, std::string_view fileName, const nlohmann::json& document)
		{
			const std::filesystem::path fixturePath = tempRoot / std::string(fileName);
			std::ofstream output(fixturePath);
			output << document.dump(2);
			return fixturePath;
		}

		TerraEngine::AssetPipeline::ModelPackageImportOptions BuildPackageOptions(
			const std::filesystem::path& tempRoot,
			const std::filesystem::path& fixturePath,
			const std::filesystem::path& sourceRelativePath,
			const TerraEngine::UUID& packageGuid)
		{
			TerraEngine::AssetPipeline::ModelPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath = sourceRelativePath;
			options.packageGuid = packageGuid;
			options.forceReimport = true;
			return options;
		}

		bool HasDiagnosticCode(const TerraEngine::AssetPipeline::ModelPackageImportResult& result, std::string_view code)
		{
			for (const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic : result.diagnostics)
			{
				if (diagnostic.code == code)
				{
					return true;
				}
			}
			return false;
		}

		std::string ReadTextFile(const std::filesystem::path& path)
		{
			std::ifstream input(path);
			return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		}

		nlohmann::json LoadHardeningFixtureManifest()
		{
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			std::ifstream input(sourceRoot / "Tests/Fixtures/VirtualGeometry/hardening-fixtures.json");
			return nlohmann::json::parse(input);
		}

		bool HasCoverageTag(const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh, std::string_view expectedTag)
		{
			return std::find(mesh.virtualGeometryCoverageTags.begin(), mesh.virtualGeometryCoverageTags.end(), std::string(expectedTag)) != mesh.virtualGeometryCoverageTags.end();
		}

		// Keep the logical glTF child and two authored material sections fixed while
		// changing only tessellation or position. The normal importer cooks every byte.
		nlohmann::json WriteSelectedGeometryGrid(const std::filesystem::path& root, unsigned divisions, float displacement)
		{
			std::vector<std::byte> bytes;
			const auto append = [&bytes]<typename T>(T value) {
				const auto offset = bytes.size(); bytes.resize(offset + sizeof(T));
				std::memcpy(bytes.data() + offset, &value, sizeof(T));
			};
			const unsigned vertices = (divisions + 1) * (divisions + 1);
			for(unsigned y = 0; y <= divisions; ++y) for(unsigned x = 0; x <= divisions; ++x) {
				append(float(x) / divisions * 4 - 2 + displacement); append(float(y) / divisions * 4); append(0.F);
			}
			const auto normals = bytes.size();
			for(unsigned i = 0; i < vertices; ++i) { append(0.F); append(0.F); append(1.F); }
			const auto uvs = bytes.size();
			for(unsigned y = 0; y <= divisions; ++y) for(unsigned x = 0; x <= divisions; ++x) { append(float(x) / divisions); append(float(y) / divisions); }
			std::array<std::size_t, 2> offsets{}; std::array<unsigned, 2> counts{};
			for(unsigned section = 0; section < 2; ++section) {
				offsets[section] = bytes.size();
				for(unsigned y = 0; y < divisions; ++y) for(unsigned x = 0; x < divisions; ++x) {
					if(x * 2 / divisions != section) continue;
					const unsigned a = y * (divisions + 1) + x, b = a + 1, c = a + divisions + 1, d = c + 1;
					for(unsigned index : {a, b, c, b, d, c}) append(static_cast<std::uint16_t>(index));
					counts[section] += 6;
				}
			}
			std::ofstream buffer(root / "grid.bin", std::ios::binary);
			buffer.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); buffer.close();
			using Json = nlohmann::json;
			Json document = {{"asset", {{"version", "2.0"}}}, {"buffers", Json::array({{{"uri", "grid.bin"}, {"byteLength", bytes.size()}}})},
				{"bufferViews", Json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", normals}},
					{{"buffer", 0}, {"byteOffset", normals}, {"byteLength", uvs - normals}}, {{"buffer", 0}, {"byteOffset", uvs}, {"byteLength", offsets[0] - uvs}}})},
				{"accessors", Json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", vertices}, {"type", "VEC3"}, {"min", {-2 + displacement, 0, 0}}, {"max", {2 + displacement, 4, 0}}},
					{{"bufferView", 1}, {"componentType", 5126}, {"count", vertices}, {"type", "VEC3"}}, {{"bufferView", 2}, {"componentType", 5126}, {"count", vertices}, {"type", "VEC2"}}})},
				{"materials", Json::array({{{"name", "SelectedGeometryMaterial0"}, {"doubleSided", true}}, {{"name", "SelectedGeometryMaterial1"}, {"doubleSided", true}}})},
				{"nodes", Json::array({{{"name", "SelectedGeometry"}, {"mesh", 0}}})}, {"scenes", Json::array({{{"nodes", {0}}}})}, {"scene", 0}};
			Json primitives = Json::array();
			for(unsigned section = 0; section < 2; ++section) {
				document["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", offsets[section]}, {"byteLength", counts[section] * sizeof(std::uint16_t)}});
				document["accessors"].push_back({{"bufferView", 3 + section}, {"componentType", 5123}, {"count", counts[section]}, {"type", "SCALAR"}});
				primitives.push_back({{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}}, {"indices", 3 + section}, {"material", section}});
			}
			document["meshes"] = Json::array({{{"name", "SelectedGeometry"}, {"primitives", primitives}}});
			return document;
		}
	}

	void RegisterVirtualGeometryPackageImportTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.Editor.PackageSiblingOwnsAcceptedNativePublication", [](TestContext& context) {
			TerraEngine::Tests::Correctness::DisposableFixturePath fixture;
			const auto root = fixture.Root();
			const auto source = root / "hero.gltf";
			std::filesystem::copy_file(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Tests/Fixtures/Animation/ImportValidation/Assets/Models/ImportedHero.gltf", source);
			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(BuildPackageOptions(root, source, "Models/hero.gltf", TerraEngine::GenerateUUID()));
			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "The committed embedded glTF must recook through the current native importer.");
			const auto& children = result.animationManifest.subAssets;
			const auto mesh = std::find_if(children.begin(), children.end(), [](const auto& child) { return child.kind == TerraEngine::AssetPipeline::AnimationImportSubAssetKind::DeformableMesh; });
			const auto morph = std::find_if(children.begin(), children.end(), [](const auto& child) { return child.kind == TerraEngine::AssetPipeline::AnimationImportSubAssetKind::MorphTarget; });
			context.Expect(mesh != children.end() && morph != children.end() && mesh->guid != morph->guid && mesh->compiledPath == morph->compiledPath, "The real importer must publish distinct durable children sharing one actual cooked TEMF.");
			if (mesh == children.end() || morph == children.end()) return;
			// Cache isolation and a real loader worker keep the test independent of
			// previous imported packages. The retained owner outlives every reload.
			auto& resources = ResourceManager::Instance();
			ThreadPool::instance().shutdown(); resources.BeginRequestSession(); resources.ClearCaches(); ThreadPool::instance().initialize(1U);
			struct Cleanup { ResourceManager& resources; ~Cleanup() { ThreadPool::instance().shutdown(); resources.ClearCaches(); resources.BeginRequestSession(); } } cleanup{resources};
			const auto cooked = root / "Build" / mesh->compiledPath;
			resources.RegisterAssetPath(mesh->guid, cooked, AssetType::Mesh);
			resources.RegisterAssetPath(morph->guid, cooked, AssetType::Mesh);
			const auto before = resources.GetRendererResourcePublicationGeneration();
			context.Expect(!resources.AcquireMeshSnapshot(mesh->guid) && resources.GetRendererResourcePublicationGeneration() == before, "Registration alone must not publish, load, or manufacture an alias owner.");
			const auto future = resources.RequestStreamableLoad(mesh->guid, AssetType::Mesh, TerraEngine::Resource::StreamingNeedType::Visible, 1U, "owned-package-child", TerraEngine::GenerateUUID());
			context.Expect(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready, "The real native async load must settle within a bounded deadline.");
			const auto acceptedId = future.get();
			const auto owner = resources.AcquireMeshSnapshot(acceptedId);
			context.Expect(acceptedId != TerraEngine::EMPTY_UUID && owner && owner->publicationGeneration > 0U, "An actual accepted cooked publication is required before any alias proof.");
			context.Expect(resources.AcquireMeshSnapshot(mesh->guid) == owner, "The sibling must share the actual successfully accepted immutable payload.");
			const auto generation = resources.GetRendererResourcePublicationGeneration();
			const auto canonicalBounds = resources.GetMeshLocalBounds(acceptedId);
			const auto aliasBounds = resources.GetMeshLocalBounds(mesh->guid);
			const auto morphBounds = resources.GetMeshLocalBounds(morph->guid);
			context.Expect(canonicalBounds.min != glm::vec3(-1.0F) || canonicalBounds.max != glm::vec3(1.0F), "The actual imported deformation envelope must differ from the unloaded unit fallback.");
			context.Expect(aliasBounds.min == canonicalBounds.min && aliasBounds.max == canonicalBounds.max
				&& morphBounds.min == canonicalBounds.min && morphBounds.max == canonicalBounds.max,
				"Every currently accepted package child must query the canonical payload's exact published spatial envelope.");
			context.Expect(resources.GetRendererResourcePublicationGeneration() == generation, "Alias bounds lookup must not load, insert a payload, or advance publication clocks.");
			context.Expect(resources.GetMesh(mesh->guid) == owner.get(), "The real legacy alias accessor must retain the same published owner and cache its old bounds.");
			const auto retainedStorage = owner->storage;
			const auto retainedPublication = owner->publicationGeneration;
			// Recook genuine source morph geometry, keeping the package and child identities.
			// This makes a stale exact alias-bounds entry observably wrong after reload.
			auto document = nlohmann::json::parse(ReadTextFile(source));
			const auto uri = document["buffers"][0]["uri"].get<std::string>();
			const auto decoded = fastgltf::base64::decode(std::string_view(uri).substr(uri.find(',') + 1U));
			std::vector<std::uint8_t> bytes(decoded.begin(), decoded.end());
			context.Expect(bytes.size() >= 276U, "The committed fixture must contain its real float3 morph displacements.");
			// Report a broken fixture before accessing its displacement stream.
			if (bytes.size() < 276U) return;
			// Expand the first morph displacement, keeping base mesh topology and child identity unchanged.
			float expandedDisplacement = -5.0F;
			std::memcpy(bytes.data() + 240U, &expandedDisplacement, sizeof(expandedDisplacement));
			{ std::ofstream output(root / "changed.bin", std::ios::binary); output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); }
			document["buffers"][0]["uri"] = "changed.bin";
			WriteGltfFixture(root, "hero.gltf", document);
			const auto recooked = TerraEngine::AssetPipeline::ImportModelPackage(BuildPackageOptions(root, source, "Models/hero.gltf", result.packageGuid));
			context.Expect(recooked.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Changed real morph geometry must produce another valid native package generation.");
			const auto recookedMesh = std::find_if(recooked.animationManifest.subAssets.begin(), recooked.animationManifest.subAssets.end(), [&](const auto& child) { return child.guid == mesh->guid; });
			context.Expect(recookedMesh != recooked.animationManifest.subAssets.end() && recookedMesh->compiledPath == mesh->compiledPath, "The real recook must preserve the authored child identity and native path.");
			const auto reload = resources.ReloadMeshAsync(acceptedId);
			context.Expect(reload.wait_for(std::chrono::seconds(10)) == std::future_status::ready && reload.get() == acceptedId, "The canonical actual mesh reload must accept the changed cooked bytes.");
			const auto freshOwner = resources.AcquireMeshSnapshot(mesh->guid);
			const auto freshBounds = resources.GetMeshLocalBounds(acceptedId);
			context.Expect(freshOwner && freshOwner != owner && freshOwner->publicationGeneration > owner->publicationGeneration
				&& (freshBounds.min != canonicalBounds.min || freshBounds.max != canonicalBounds.max), "A real different deformation envelope and owner must be published before comparing alias bounds.");
			context.Expect(owner->storage == retainedStorage && owner->publicationGeneration == retainedPublication
				&& freshOwner->storage != retainedStorage, "Reload must replace actual cooked bytes while a retained owner preserves its original bytes and publication.");
			const auto freshAliasBounds = resources.GetMeshLocalBounds(mesh->guid);
			context.Expect(freshAliasBounds.min == freshBounds.min && freshAliasBounds.max == freshBounds.max,
				"A legacy exact alias entry must not mask the current accepted canonical deformation envelope.");
			// Native re-registration breaks the old association immediately; it may
			// not silently borrow the previous path's still-retained owner.
			resources.RegisterAssetPath(mesh->guid, root / "different.temf", AssetType::Mesh);
			context.Expect(!resources.AcquireMeshSnapshot(mesh->guid) && owner->publicationGeneration > 0U, "Changed registration must reject a stale alias without invalidating an existing shared lease.");
			const auto rejectedBounds = resources.GetMeshLocalBounds(mesh->guid);
			context.Expect(rejectedBounds.min == glm::vec3(-1.0F) && rejectedBounds.max == glm::vec3(1.0F), "An invalid registered native association must not expose cached legacy alias bounds.");
		});

		registry.Add("Renderer.Efficiency.Editor.DensePageImportPreservesIdentity", [](TestContext& context) {
			const auto root = MakeVirtualGeometryPackageTempRoot("DensePageIdentity");
			const auto packageGuid = TerraEngine::GenerateUUID();
			const auto expected = TerraEngine::AssetPipeline::BuildStableGeneratedModelUuid(packageGuid, "gltf-mesh", 0);
			std::array<std::string, 4> sourceBytes, cookedBytes;
			std::vector<TerraEngine::UUID> materials;
			// The original owner grid remains direct. Page validation first cooks a
			// dense baseline, changes its positions, then restores the exact original.
			const std::array<unsigned, 4> divisions{32, 64, 64, 32};
			for(unsigned stage = 0; stage < divisions.size(); ++stage) {
				const auto document = WriteSelectedGeometryGrid(root, divisions[stage], stage == 2 ? 0.5F : 0.F);
				const auto source = WriteGltfFixture(root, "grid.gltf", document);
				const auto result = TerraEngine::AssetPipeline::ImportModelPackage(BuildPackageOptions(root, source, "Models/RendererEfficiency_grid.gltf", packageGuid));
				context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed && result.generatedMeshes.size() == 1, "Each native recook must publish exactly one usable mesh.");
				const auto& child = result.generatedMeshes.front();
				context.Expect(child.guid == expected && child.displayName == "SelectedGeometry" && child.sourcePath == "Models/RendererEfficiency_grid.gltf", "Source topology changes must preserve the exact package child identity.");
				context.ExpectEqual(2u * divisions[stage] * divisions[stage], child.virtualGeometrySourceTriangles, "The two sections partition one grid; source triangles must describe the entire mesh.");
				const auto packageSource = root / "Assets/Models/RendererEfficiency_grid.gltf";
				context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(packageSource, result), "Actual importer identity metadata must persist.");
				TerraEngine::AssetPipeline::ModelPackageImportResult persisted;
				context.Expect(TerraEngine::AssetPipeline::LoadModelPackageImportResult(packageSource, persisted)
					&& persisted.packageGuid == packageGuid && persisted.generatedMeshes.size() == 1 && persisted.generatedMeshes.front().guid == expected,
					"Every persisted native package/child identity must round-trip unchanged.");
				context.Expect(result.generatedMaterials.size() == 2 && result.materialAssignments.size() == 2, "Both authored material slots must survive every recook.");
				for(unsigned slot = 0; slot < 2; ++slot) {
					if(stage == 0) materials.push_back(result.generatedMaterials[slot].guid);
					context.Expect(result.generatedMaterials[slot].guid == materials[slot] && result.materialAssignments[slot].meshGuid == expected && result.materialAssignments[slot].materialGuid == materials[slot], "Material order and mesh/material joins must remain stable.");
				}
				const auto cooked = root / "Build" / child.compiledPath;
				sourceBytes[stage] = document.dump(); cookedBytes[stage] = ReadTextFile(cooked);
				const auto evidence = root / ("stage" + std::to_string(stage)); std::filesystem::create_directories(evidence);
				std::filesystem::copy_file(source, evidence / "grid.gltf", std::filesystem::copy_options::overwrite_existing);
				std::filesystem::copy_file(root / "grid.bin", evidence / "grid.bin", std::filesystem::copy_options::overwrite_existing);
				std::filesystem::copy_file(cooked, evidence / "grid.temf", std::filesystem::copy_options::overwrite_existing);
				// LoadMesh gives each isolated cooked-byte probe a fresh cache key.
				// Authored child identity is proven above from importer and persisted metadata.
				ResourceManager& manager = ResourceManager::Instance(); manager.ClearCaches();
				Mesh* mesh = manager.GetMesh(manager.LoadMesh(cooked));
				context.Expect(mesh != nullptr, "The actual native TEMF must load.");
				const bool dense = stage == 1 || stage == 2;
				context.Expect(mesh->hasVirtualGeometry == dense, "Only the dense baseline and displaced variant must publish actual virtual pages.");
				context.Expect(mesh->virtualGeometryDetailLevels.size() >= 2, "Every cook must retain selectable native LOD ranges.");
				if(dense) {
					context.Expect(!mesh->virtualGeometryPages.empty() && !mesh->virtualGeometryClusters.empty() && !mesh->virtualGeometryClusterPageLinks.empty(), "Dense native cook must retain page, cluster and join records.");
					const auto& lod = mesh->virtualGeometryDetailLevels.front();
					TerraEngine::Renderer::Vulkan::ShadowPageCutRequest request{expected, std::shared_ptr<const Mesh>(mesh, [](const Mesh*) {}), mesh->publicationGeneration, lod.meshletOffset, lod.meshletCount, lod.lodIndex};
					const auto cut = TerraEngine::Renderer::Vulkan::BuildShadowAuthoredPageCut(request);
					context.Expect(cut.complete && !cut.segments.empty(), "Actual cooked joins must authorize complete authored shadow-page coverage.");
				}
			}
			context.Expect(sourceBytes[1] != sourceBytes[2] && cookedBytes[1] != cookedBytes[2], "The displaced dense variant must change real source and cooked bytes.");
			context.Expect(sourceBytes[0] == sourceBytes[3] && cookedBytes[0] == cookedBytes[3], "Restoration must recover the original source and deterministic cooked bytes.");
		});
		registry.Add("VirtualGeometry.Editor.PackageImport.HardeningFixtureManifestCoversRequiredCategories", [](TestContext& context) {
			const nlohmann::json manifest = LoadHardeningFixtureManifest();
			context.ExpectEqual(1, manifest.value("schemaVersion", 0), "Hardening fixture manifest should use schema version 1.");

			std::set<std::string> categories;
			std::set<std::string> packageFormats;
			for (const nlohmann::json& format : manifest.value("packageFormats", nlohmann::json::array()))
			{
				packageFormats.insert(format.get<std::string>());
			}
			for (const nlohmann::json& entry : manifest.value("requiredCategories", nlohmann::json::array()))
			{
				context.Expect(entry.contains("id") && entry["id"].is_string(), "Every hardening fixture entry should have a stable id.");
				context.Expect(entry.contains("category") && entry["category"].is_string(), "Every hardening fixture entry should have a category.");
				context.Expect(entry.value("requiredForCleanCheckout", false), "Required hardening fixtures should be clean-checkout safe.");
				context.Expect(!entry.value("expectedDiagnostics", nlohmann::json::array()).empty(), "Each hardening fixture should name expected diagnostics.");
				context.Expect(!entry.value("expectedRenderOutcomes", nlohmann::json::array()).empty(), "Each hardening fixture should name expected render outcomes.");
				context.Expect(!entry.value("coverageTags", nlohmann::json::array()).empty(), "Each hardening fixture should name coverage tags consumed by import/export diagnostics.");
				categories.insert(entry["category"].get<std::string>());
			}

			const std::vector<std::string> requiredCategories = {
				"tiny",
				"dense",
				"thin",
				"curved",
				"hard-surface",
				"multi-material-package",
				"streaming-pressure",
				"multi-view",
				"export"
			};
			for (const std::string& category : requiredCategories)
			{
				context.Expect(categories.contains(category), "The hardening fixture manifest should cover every broad virtual geometry validation category.");
			}
			context.Expect(packageFormats.contains("gltf"), "Package coverage should include text glTF fixtures.");
			context.Expect(packageFormats.contains("glb"), "Package coverage should include binary GLB fixtures.");
			context.Expect(packageFormats.contains("fbx"), "Package coverage should include FBX fixtures.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.CooksMultipleGeneratedMeshOutputs", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("MultipleMeshes");
			nlohmann::json document = LoadBaseGltfFixture();

			// Duplicate the fixture mesh through a second source mesh and node.
			// Each generated child must now own a matching compiled TEMF so a
			// component that references one child cannot accidentally select or
			// render virtual-geometry clusters from a sibling mesh.
			nlohmann::json secondMesh = document["meshes"][0];
			document["meshes"][0]["name"] = "Triangle_A";
			secondMesh["name"] = "Triangle_B";
			document["meshes"].push_back(secondMesh);
			document["nodes"] = nlohmann::json::array({
				{ { "name", "Node_A" }, { "mesh", 0 } },
				{ { "name", "Node_B" }, { "mesh", 1 } }
			});
			document["scenes"][0]["nodes"] = nlohmann::json::array({ 0, 1 });

			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "virtual-multi-mesh.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/virtual-multi-mesh.gltf", packageGuid);
			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(options);

			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Multi-mesh package import should produce usable static geometry.");
			context.ExpectEqual(static_cast<std::size_t>(2), result.generatedMeshes.size(), "Each source mesh should remain visible as a generated package mesh.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.generatedPrefabs.size(), "Package import should generate one configured prefab for the imported object.");
			context.Expect(result.generatedMeshes[0].compiledPath != result.generatedMeshes[1].compiledPath, "Generated mesh children should not share one compiled package TEMF.");
			for (const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : result.generatedMeshes)
			{
				context.Expect(mesh.compiledPath.parent_path().filename().string().find("_Generated") != std::string::npos, "Generated child TEMFs should live beside other package child artifacts.");
				context.Expect(mesh.virtualGeometryRequested, "Package mesh records should state that virtual geometry was requested.");
				context.Expect(!mesh.virtualGeometryCooked, "A triangle package mesh must not claim virtual-page cooking.");
				context.ExpectEqual(std::string("Skipped"), mesh.virtualGeometryStatus, "A triangle package mesh must publish the canonical direct-meshlet route.");
				context.ExpectEqual(1u, mesh.virtualGeometryDetailLevels, "Direct meshlets must publish their selectable resident LOD0 range.");
				context.Expect(mesh.virtualGeometryMeshlets > 0u, "Cooked package mesh should expose a meshlet count.");
				context.Expect(std::any_of(mesh.virtualGeometryWarnings.begin(), mesh.virtualGeometryWarnings.end(), [](const std::string& warning) {
					return warning.find("direct-resident") != std::string::npos;
				}), "Direct meshlet metadata must explain why no virtual pages were published.");
			}
			TerraEngine::AssetPipeline::GeneratedModelMeshAsset refreshedMesh =
				result.generatedMeshes.front();
			refreshedMesh.virtualGeometryTargets.assign(
				refreshedMesh.virtualGeometryDetailLevels + 2u,
				{});
			for(auto& staleTarget : refreshedMesh.virtualGeometryTargets)
			{
				staleTarget.status = "Generated";
			}
			std::vector<TerraEngine::AssetPipeline::ModelImportDiagnostic>
				refreshDiagnostics;
			TerraEngine::AssetPipeline::
				PopulateGeneratedMeshVirtualGeometryMetadata(
					options.buildRoot / refreshedMesh.compiledPath,
					refreshedMesh,
					refreshDiagnostics);
			const std::size_t publishedGeneratedTargetCount =
				static_cast<std::size_t>(std::count_if(
					refreshedMesh.virtualGeometryTargets.begin(),
					refreshedMesh.virtualGeometryTargets.end(),
					[](const auto& target) {
						return target.status == "Generated";
					}));
			context.ExpectEqual(
				static_cast<std::size_t>(refreshedMesh.virtualGeometryDetailLevels - 1u),
				publishedGeneratedTargetCount,
				"Final TEMF inspection must replace stale inventory rows; direct LOD0 is renderable without pretending it came from an adaptive target.");
			const std::filesystem::path generatedPrefabPath = options.assetRoot / result.generatedPrefabs.front().sourcePath;
			context.Expect(std::filesystem::exists(generatedPrefabPath), "Generated package prefab should be written beside package child assets.");
			const nlohmann::json prefabJson = nlohmann::json::parse(ReadTextFile(generatedPrefabPath));
			const nlohmann::json prefabObjects = prefabJson.value("objects", nlohmann::json::array());
			context.ExpectEqual(static_cast<std::size_t>(1), prefabObjects.size(), "Generated prefab should contain one import root object.");
			const nlohmann::json prefabChildren = prefabObjects.empty() ? nlohmann::json::array() : prefabObjects.front().value("children", nlohmann::json::array());
			context.ExpectEqual(result.generatedMeshes.size(), prefabChildren.size(), "Generated prefab should create one render child per generated mesh.");
			if (!prefabChildren.empty())
			{
				const nlohmann::json meshRender = prefabChildren.front().value("components", nlohmann::json::object()).value("MeshRenderComponent", nlohmann::json::object());
				context.ExpectEqual(TerraEngine::ToHexString(result.generatedMeshes.front().guid), meshRender.value("meshId", std::string()), "Generated prefab child should reference the generated mesh GUID.");
				context.Expect(!meshRender.value("materialId", std::string()).empty(), "Generated prefab child should preserve available material assignment data.");
			}

			const nlohmann::json sidecar = TerraEngine::AssetPipeline::ToJson(result);
			context.ExpectEqual(static_cast<std::size_t>(1), sidecar.value("generatedPrefabs", nlohmann::json::array()).size(), "Sidecar JSON should include generated prefab records.");
			context.Expect(sidecar["generatedMeshes"][0]["virtualGeometry"].is_object(), "Sidecar JSON should persist virtual geometry metadata for generated meshes.");
			context.ExpectEqual(std::string("Skipped"), sidecar["generatedMeshes"][0]["virtualGeometry"].value("status", std::string()), "Sidecar JSON should preserve the direct-meshlet route.");
			const nlohmann::json targets = sidecar["generatedMeshes"][0]["virtualGeometry"].value("targets", nlohmann::json::array());
			context.Expect(targets.empty(), "Direct meshlets must not retain virtual-geometry target diagnostics.");
			const nlohmann::json coneSafety = sidecar["generatedMeshes"][0]["virtualGeometry"].value("coneSafety", nlohmann::json::object());
			context.Expect(!coneSafety.value("metadataValid", false), "Direct meshlets must not claim virtual cone-safety metadata.");
			context.ExpectEqual(0u, coneSafety.value("disabledClusterCount", 0u), "Direct meshlets must not retain virtual cone-safety counters.");
			context.Expect(coneSafety.value("disabledReasons", nlohmann::json::array()).empty(), "Direct meshlets must not retain virtual cone-safety reasons.");

			const std::filesystem::path sourceAssetPath = tempRoot / "Assets/Models/virtual-multi-mesh.gltf";
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, result), "Virtual geometry package sidecar should save.");
			TerraEngine::AssetPipeline::ModelPackageImportResult loaded;
			context.Expect(TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, loaded), "Virtual geometry package sidecar should reload.");
			context.ExpectEqual(static_cast<std::size_t>(1), loaded.generatedPrefabs.size(), "Generated prefab records should round-trip through the sidecar loader.");
			context.ExpectEqual(std::string("Skipped"), loaded.generatedMeshes.front().virtualGeometryStatus, "The direct-meshlet route should round-trip through package metadata.");
			context.Expect(!loaded.generatedMeshes.front().virtualGeometryCooked, "Package metadata must not relabel direct meshlets as virtual-page output.");
			context.Expect(!loaded.generatedMeshes.front().virtualGeometryConeSafetySummary.metadataValid, "Direct-route sidecars must keep virtual cone-safety metadata absent.");
			context.ExpectEqual(0u, loaded.generatedMeshes.front().virtualGeometryConeSafetySummary.disabledClusterCount, "Direct-route sidecars must keep virtual cone-safety counts empty.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.PreservesStableGeneratedIdentifiers", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("StableIdentifiers");
			const nlohmann::json document = LoadBaseGltfFixture();
			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "stable.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/stable.gltf", packageGuid);

			const auto first = TerraEngine::AssetPipeline::ImportModelPackage(options);
			const auto second = TerraEngine::AssetPipeline::ImportModelPackage(options);

			context.Expect(first.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "First package import should succeed.");
			context.Expect(second.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Second package import should succeed.");
			context.Expect(!first.generatedMeshes.empty() && !second.generatedMeshes.empty(), "Both imports should produce generated mesh records.");
			context.Expect(!first.generatedMaterials.empty() && !second.generatedMaterials.empty(), "Both imports should produce generated material records.");
			context.Expect(!first.materialAssignments.empty() && !second.materialAssignments.empty(), "Both imports should preserve material assignments.");

			const std::string expectedMeshGuid = TerraEngine::ToHexString(TerraEngine::AssetPipeline::BuildStableGeneratedModelUuid(packageGuid, "gltf-mesh", 0u));
			context.ExpectEqual(expectedMeshGuid, TerraEngine::ToHexString(first.generatedMeshes.front().guid), "Generated mesh GUID should match the stable package child ID.");
			context.ExpectEqual(TerraEngine::ToHexString(first.generatedMeshes.front().guid), TerraEngine::ToHexString(second.generatedMeshes.front().guid), "Generated mesh GUID should remain stable across reimport.");
			context.ExpectEqual(TerraEngine::ToHexString(first.generatedMaterials.front().guid), TerraEngine::ToHexString(second.generatedMaterials.front().guid), "Generated material GUID should remain stable across reimport.");
			context.ExpectEqual(TerraEngine::ToHexString(first.materialAssignments.front().meshGuid), TerraEngine::ToHexString(second.materialAssignments.front().meshGuid), "Material slot mesh reference should remain stable across reimport.");
			context.Expect(TerraEngine::ToHexString(first.generatedMeshes.front().guid) != TerraEngine::ToHexString(packageGuid), "Generated mesh should keep a package-child ID instead of being overwritten by the package GUID.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.GeneratedChildOwnsCurrentGpuBytes", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("GeneratedChildGpuBytes");
			const nlohmann::json document = LoadBaseGltfFixture();
			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "child-gpu-bytes.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/child-gpu-bytes.gltf", packageGuid);
			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(options);

			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Package import should produce generated children for residency accounting.");
			context.Expect(!result.generatedMeshes.empty(), "Package import should expose the generated mesh child id.");
			if (result.generatedMeshes.empty())
			{
				return;
			}

			ResourceManager& resourceManager = ResourceManager::Instance();
			resourceManager.SetStreamingDiagnosticsEnabled(true);
			const TerraEngine::UUID childMeshId = result.generatedMeshes.front().guid;
			const std::filesystem::path parentPath = tempRoot / "Assets/Models/child-gpu-bytes.gltf";
			const std::filesystem::path childPath = tempRoot / "Build" / result.generatedMeshes.front().compiledPath;
			resourceManager.RegisterStreamableAsset(packageGuid, AssetType::Mesh, parentPath);
			resourceManager.RegisterStreamableAsset(childMeshId, AssetType::Mesh, childPath);
			resourceManager.MarkResourceUploadedToGpu(childMeshId, AssetType::Mesh, 77u, 4096u, "generated package child upload");

			const TerraEngine::Resource::StreamingDiagnosticsSummary summary = resourceManager.GetStreamingDiagnosticsSummary();
			const auto parentRecord = std::find_if(summary.records.begin(), summary.records.end(), [packageGuid](const TerraEngine::Resource::StreamingDiagnosticRecord& record) {
				return record.assetId == packageGuid;
			});
			const auto childRecord = std::find_if(summary.records.begin(), summary.records.end(), [childMeshId](const TerraEngine::Resource::StreamingDiagnosticRecord& record) {
				return record.assetId == childMeshId;
			});

			context.Expect(parentRecord != summary.records.end(), "The package parent should exist only as a logical diagnostics row before UI filtering.");
			context.Expect(childRecord != summary.records.end(), "The generated mesh child should own the current GPU-byte row.");
			if (parentRecord != summary.records.end())
			{
				context.Expect(parentRecord->exactGpuBytes.status == TerraEngine::Resource::ExactResidencyByteStatus::ZeroByState, "Package parents must not borrow generated child GPU bytes.");
				context.ExpectEqual(0ull, parentRecord->exactGpuBytes.bytes, "Package parent current GPU bytes should stay zero unless it owns an allocation.");
			}
			if (childRecord != summary.records.end())
			{
				context.Expect(childRecord->exactGpuBytes.status == TerraEngine::Resource::ExactResidencyByteStatus::Exact, "Generated child current GPU bytes should be exact.");
				context.ExpectEqual(4096ull, childRecord->exactGpuBytes.bytes, "Generated child current GPU bytes should match the backend upload.");
			}

			resourceManager.DeleteResource(packageGuid, AssetType::Mesh);
			resourceManager.DeleteResource(childMeshId, AssetType::Mesh);
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.ReportsMultiMaterialClusterCoverage", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("MultiMaterialCoverage");
			nlohmann::json document = LoadBaseGltfFixture();
			nlohmann::json secondMaterial = document["materials"][0];
			secondMaterial["name"] = "Mat_Blue";
			secondMaterial["pbrMetallicRoughness"]["baseColorFactor"] = nlohmann::json::array({ 0.05f, 0.15f, 0.9f, 1.0f });
			document["materials"].push_back(secondMaterial);
			nlohmann::json secondPrimitive = document["meshes"][0]["primitives"][0];
			secondPrimitive["material"] = 1;
			document["meshes"][0]["primitives"].push_back(secondPrimitive);

			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "multi-material.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/multi-material.gltf", packageGuid);
			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(options);

			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Multi-material package import should still produce static geometry.");
			context.ExpectEqual(static_cast<std::size_t>(1u), result.generatedMeshes.size(), "One source mesh should produce one generated mesh record.");
			context.ExpectEqual(static_cast<std::size_t>(2u), result.materialAssignments.size(), "Both authored primitives should produce material assignments.");
			const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh = result.generatedMeshes.front();
			context.Expect(!mesh.virtualGeometryCooked && mesh.virtualGeometryStatus == "Skipped", "A tiny multi-material fixture must retain its canonical direct-meshlet route.");
			context.ExpectEqual(2u, mesh.virtualGeometryClusterSummary.sourceMaterialAssignmentCount, "Cluster summary should remember the authored material assignment count.");
			context.Expect(mesh.virtualGeometryClusterSummary.materialSectionCount == 0u, "Direct meshlets must not synthesize virtual material-section tables.");

			const nlohmann::json sidecar = TerraEngine::AssetPipeline::ToJson(result);
			const nlohmann::json clusterSummary = sidecar["generatedMeshes"][0]["virtualGeometry"]["clusterSummary"];
			context.ExpectEqual(2u, clusterSummary.value("sourceMaterialAssignmentCount", 0u), "Sidecar JSON should persist source material assignment counts.");
			context.ExpectEqual(std::string("Skipped"), sidecar["generatedMeshes"][0]["virtualGeometry"].value("status", std::string()), "Sidecar JSON should preserve the direct-meshlet route.");

			const std::filesystem::path sourceAssetPath = tempRoot / "Assets/Models/multi-material.gltf";
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, result), "Multi-material package sidecar should save.");
			TerraEngine::AssetPipeline::ModelPackageImportResult loaded;
			context.Expect(TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, loaded), "Multi-material package sidecar should reload.");
			context.ExpectEqual(2u, loaded.generatedMeshes.front().virtualGeometryClusterSummary.sourceMaterialAssignmentCount, "Material assignment count should round-trip through the sidecar loader.");
			context.Expect(!loaded.generatedMeshes.front().virtualGeometryClusterSummary.materialCoverageValid, "Material coverage state should round-trip through the sidecar loader.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.PreservesGeneratedMeshOverrideSettings", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("GeneratedOverrideSettings");
			const nlohmann::json document = LoadBaseGltfFixture();
			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "override-settings.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/override-settings.gltf", packageGuid);
			options.hasVirtualGeometrySettings = true;
			options.virtualGeometrySettings.hasOverride = true;
			options.virtualGeometrySettings.settingsSource = "PerMeshOverride";
			options.virtualGeometrySettings.targetRatios = { 1.0f, 0.5f };
			options.virtualGeometrySettings.minimumGeneratedTriangles = 32u;

			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(options);
			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Package import with generated mesh override settings should succeed.");
			context.Expect(!result.generatedMeshes.empty(), "Package import should produce a generated mesh record.");
			context.Expect(result.generatedMeshes.front().virtualGeometrySettings.hasOverride, "Generated mesh record should preserve the override flag.");
			context.ExpectEqual(static_cast<std::size_t>(2u), result.generatedMeshes.front().virtualGeometrySettings.targetRatios.size(), "Generated mesh override ratios should be stored in the import result.");
			context.ExpectEqual(32u, result.generatedMeshes.front().virtualGeometrySettings.minimumGeneratedTriangles, "Generated mesh override thresholds should be stored in the import result.");

			const std::filesystem::path sourceAssetPath = tempRoot / "Assets/Models/override-settings.gltf";
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, result), "Override package sidecar should save.");
			TerraEngine::AssetPipeline::ModelPackageImportResult loaded;
			context.Expect(TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, loaded), "Override package sidecar should reload.");
			context.Expect(loaded.generatedMeshes.front().virtualGeometrySettings.hasOverride, "Generated mesh override flag should round-trip through the sidecar.");
			context.ExpectEqual(static_cast<std::size_t>(2u), loaded.generatedMeshes.front().virtualGeometrySettings.targetRatios.size(), "Generated mesh override ratios should round-trip through the sidecar.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.AppliesPercentageOverridesPerGeneratedMesh", [](TestContext& context) {
			const std::filesystem::path tempRoot =
				MakeVirtualGeometryPackageTempRoot("PerGeneratedMeshOverrides");
			nlohmann::json document = LoadBaseGltfFixture();
			nlohmann::json secondMesh = document["meshes"][0];
			secondMesh["name"] = "Threshold_B";
			document["meshes"][0]["name"] = "Threshold_A";
			document["meshes"].push_back(secondMesh);
			document["nodes"] = nlohmann::json::array({
				{ { "name", "Node_A" }, { "mesh", 0 } },
				{ { "name", "Node_B" }, { "mesh", 1 } }
			});
			document["scenes"][0]["nodes"] =
				nlohmann::json::array({0, 1});

			const std::filesystem::path fixturePath = WriteGltfFixture(
				tempRoot,
				"per-generated-mesh-overrides.gltf",
				document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			auto options = BuildPackageOptions(
				tempRoot,
				fixturePath,
				"Models/per-generated-mesh-overrides.gltf",
				packageGuid);
			for(std::uint32_t meshIndex = 0U; meshIndex < 2U; ++meshIndex) {
				TerraEngine::AssetPipeline::
					ModelPackageMeshVirtualGeometryOverride override;
				override.meshGuid =
					TerraEngine::AssetPipeline::BuildStableGeneratedModelUuid(
						packageGuid,
						"gltf-mesh",
						meshIndex);
				override.settings.hasOverride = true;
				override.settings.settingsSource = "PerMeshOverride";
				override.settings.targetRatios = {1.0F, 0.5F};
				override.settings.minimumUsefulTriangleReduction =
					meshIndex == 0U ? 0.10F : 0.80F;
				override.settings.minimumUsefulMeshletReduction =
					meshIndex == 0U ? 0.20F : 0.90F;
				options.virtualGeometryMeshOverrides.push_back(
					std::move(override));
			}

			const auto result =
				TerraEngine::AssetPipeline::ImportModelPackage(options);
			context.Expect(result.status
					!= TerraEngine::AssetPipeline::ModelImportStatus::Failed,
				"A package with independent child thresholds should import.");
			context.ExpectEqual(
				static_cast<std::size_t>(2U),
				result.generatedMeshes.size(),
				"Both generated children should remain present.");
			if(result.generatedMeshes.size() == 2U) {
				context.ExpectEqual(
					0.10F,
					result.generatedMeshes[0].virtualGeometrySettings
						.minimumUsefulTriangleReduction,
					"The first generated mesh should keep its own triangle percentage.");
				context.ExpectEqual(
					0.90F,
					result.generatedMeshes[1].virtualGeometrySettings
						.minimumUsefulMeshletReduction,
					"The second generated mesh should keep its own meshlet percentage.");
			}
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.UnsupportedContentStaysNonBlocking", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("UnsupportedContent");
			nlohmann::json document = LoadBaseGltfFixture();
			document["extras"] = {
				{ "animations", true },
				{ "skins", true },
				{ "cameras", true },
				{ "lights", true },
				{ "constraints", true }
			};

			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "unsupported.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/unsupported.gltf", packageGuid);
			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(options);

			context.Expect(result.status == TerraEngine::AssetPipeline::ModelImportStatus::SucceededWithWarnings, "Unsupported authoring data should warn without blocking static mesh output.");
			context.Expect(!result.generatedMeshes.empty(), "Usable static geometry should still be generated when unsupported authoring data is ignored.");
			const std::vector<std::string> expectedCodes = {
				"UnsupportedCameraIgnored",
				"UnsupportedLightIgnored",
				"UnsupportedConstraintIgnored"
			};
			for (const std::string& code : expectedCodes)
			{
				context.Expect(HasDiagnosticCode(result, code), "Unsupported package content should be reported through diagnostics.");
			}
			context.Expect(!HasDiagnosticCode(result, "UnsupportedAnimationIgnored")
				&& !HasDiagnosticCode(result, "UnsupportedSkinIgnored"),
				"Animation and skin records are supported package content and must not be downgraded by the format-neutral dispatcher.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.RejectsCorruptSidecarWithoutMutatingLoadedResult", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("RejectedSidecar");
			const nlohmann::json document = LoadBaseGltfFixture();
			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "atomicity.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/atomicity.gltf", packageGuid);
			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(options);

			const std::filesystem::path sourceAssetPath = tempRoot / "Assets/Models/atomicity.gltf";
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, result), "Initial package sidecar should publish through the atomic writer.");
			const std::filesystem::path sidecarPath = TerraEngine::AssetPipeline::GetImportResultPath(sourceAssetPath);
			const std::string goodSidecar = ReadTextFile(sidecarPath);

			{
				std::ofstream corrupt(sidecarPath, std::ios::trunc);
				corrupt << "{ this is not valid package JSON";
			}

			TerraEngine::AssetPipeline::ModelPackageImportResult loaded;
			loaded.status = TerraEngine::AssetPipeline::ModelImportStatus::Succeeded;
			loaded.packageGuid = packageGuid;
			context.Expect(!TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, loaded), "Corrupt package sidecar should be rejected.");
			context.Expect(loaded.status == TerraEngine::AssetPipeline::ModelImportStatus::Succeeded, "Rejected sidecar reads should not partially mutate the caller's previous result.");
			context.ExpectEqual(TerraEngine::ToHexString(packageGuid), TerraEngine::ToHexString(loaded.packageGuid), "Rejected sidecar reads should preserve the previous package GUID.");

			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, result), "Atomic writer should recover from a corrupt sidecar by publishing a complete replacement.");
			context.ExpectEqual(goodSidecar, ReadTextFile(sidecarPath), "Recovered sidecar should match the last complete import result.");
			context.Expect(!std::filesystem::exists(sidecarPath.string() + ".tmp"), "Atomic sidecar writer should not leave staged files after success.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.PublishesCoverageTagsForGltfAndFbxPackages", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("CoverageTags");
			nlohmann::json document = LoadBaseGltfFixture();
			nlohmann::json secondMaterial = document["materials"][0];
			secondMaterial["name"] = "Coverage_Mat_B";
			document["materials"].push_back(secondMaterial);
			nlohmann::json secondPrimitive = document["meshes"][0]["primitives"][0];
			secondPrimitive["material"] = 1;
			document["meshes"][0]["primitives"].push_back(secondPrimitive);

			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "coverage-tags.gltf", document);
			const TerraEngine::UUID gltfPackageGuid = TerraEngine::GenerateUUID();
			const auto gltfOptions = BuildPackageOptions(tempRoot, fixturePath, "Models/coverage-tags.gltf", gltfPackageGuid);
			const auto gltfResult = TerraEngine::AssetPipeline::ImportModelPackage(gltfOptions);
			context.Expect(gltfResult.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Coverage-tag glTF package import should succeed.");
			context.Expect(!gltfResult.generatedMeshes.empty(), "Coverage-tag glTF package import should produce a generated mesh.");
			const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& gltfMesh = gltfResult.generatedMeshes.front();
			context.Expect(HasCoverageTag(gltfMesh, "package-gltf"), "glTF package meshes should publish the package-gltf coverage tag.");
			context.Expect(HasCoverageTag(gltfMesh, "package-static-geometry"), "Package meshes should publish the static geometry coverage tag.");
			context.Expect(HasCoverageTag(gltfMesh, "multi-material-package"), "Multi-material package meshes should publish the multi-material coverage tag.");
			context.Expect(gltfMesh.virtualGeometryStatus == "Skipped", "The tiny glTF coverage fixture must remain direct-resident.");

			const nlohmann::json sidecar = TerraEngine::AssetPipeline::ToJson(gltfResult);
			const nlohmann::json tags = sidecar["generatedMeshes"][0]["virtualGeometry"].value("coverageTags", nlohmann::json::array());
			bool sidecarHasMultiMaterialTag = false;
			for (const nlohmann::json& tag : tags)
			{
				sidecarHasMultiMaterialTag = sidecarHasMultiMaterialTag || (tag.is_string() && tag.get<std::string>() == "multi-material-package");
			}
			context.Expect(sidecarHasMultiMaterialTag, "Coverage tags should persist into generated package sidecar JSON.");

			const std::filesystem::path sourceAssetPath = tempRoot / "Assets/Models/coverage-tags.gltf";
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, gltfResult), "Coverage-tag glTF package sidecar should save.");
			TerraEngine::AssetPipeline::ModelPackageImportResult loadedGltf;
			context.Expect(TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, loadedGltf), "Coverage-tag glTF package sidecar should reload.");
			context.Expect(HasCoverageTag(loadedGltf.generatedMeshes.front(), "package-gltf"), "Coverage tags should round-trip through the package sidecar loader.");

			const auto fbxResult = ImportFbxFixture("box.fbx");
			context.Expect(fbxResult.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Coverage-tag FBX package import should succeed.");
			context.Expect(!fbxResult.generatedMeshes.empty(), "Coverage-tag FBX package import should produce a generated mesh.");
			context.Expect(HasCoverageTag(fbxResult.generatedMeshes.front(), "package-fbx"), "FBX package meshes should publish the package-fbx coverage tag.");
			context.Expect(HasCoverageTag(fbxResult.generatedMeshes.front(), "package-static-geometry"), "FBX package meshes should publish the static geometry coverage tag.");
			context.Expect(fbxResult.generatedMeshes.front().virtualGeometryStatus == "Skipped", "The small FBX coverage fixture must remain direct-resident.");
		});
	}
}
