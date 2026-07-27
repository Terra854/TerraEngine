// AnimationExportTests.cpp - real staged-closure and TEPAK validation regressions.

#include "TestHarness.h"

#include "AnimationExportDependencies.h"
#include "AssetImportResult.h"
#include "File Formats/TerraEngineAnimationFile.h"
#include "TEPAK/PackWriter.h"
#include "validation/AnimationPackValidator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace Animation;

		struct ExportWorkspace final
		{
			ExportWorkspace()
			{
				const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
				root = std::filesystem::temp_directory_path()
					/ ("terra-animation-export-" + std::to_string(nonce));
				assets = root / "Assets";
				staged = root / "Staged";
				archive = root / "content.tepak";
				std::filesystem::create_directories(assets);
				std::filesystem::create_directories(staged);
			}

			~ExportWorkspace()
			{
				std::error_code error;
				std::filesystem::remove_all(root, error);
			}

			std::filesystem::path root;
			std::filesystem::path assets;
			std::filesystem::path staged;
			std::filesystem::path archive;
		};

		AnimationGuid MakeGuid(std::uint8_t seed)
		{
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index)
			{
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		std::vector<std::uint8_t> MakeCookedAsset(
			AnimationGuid guid,
			std::span<const AnimationDependencyEntry> dependencies = {},
			std::uint8_t metadataMarker = 4U)
		{
			AnimationAssetWriter writer;
			for(const AnimationDependencyEntry& dependency : dependencies)
			{
				(void)writer.AddDependency(dependency);
			}
			const std::array<std::uint8_t, 4U> metadata{{1U, 2U, 3U, metadataMarker}};
			(void)writer.AddChunk(kAnimationChunkMetadata, false, metadata);
			AnimationAssetEnvelope envelope;
			envelope.assetId = guid;
			envelope.compatibilityFingerprint = 0x8844U;
			std::vector<std::uint8_t> bytes;
			if(writer.Finish(envelope, bytes) != AnimationStatus::Ok)
			{
				return {};
			}
			return bytes;
		}

		bool WriteBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
		{
			std::filesystem::create_directories(path.parent_path());
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			return output.good();
		}

		std::string ImporterFileHash(std::span<const std::uint8_t> bytes)
		{
			// This is the stable payload-hash contract used by the glTF, FBX, and
			// model-package importers. The export regression intentionally computes
			// it independently so a validator seed mismatch cannot hide in a helper.
			std::uint64_t hash = 1469598103934665603ULL;
			for(const std::uint8_t byte : bytes)
			{
				hash ^= byte;
				hash *= 1099511628211ULL;
			}
			std::ostringstream output;
			output << std::hex << std::setfill('0') << std::setw(16) << hash;
			return output.str();
		}

		void StageRequiredShaders(const std::filesystem::path& staged)
		{
			const std::array<std::uint8_t, 4U> shaderMarker{{0x03U, 0x02U, 0x23U, 0x07U}};
			for(const std::string& path : Export::RequiredAnimationShaderPaths())
			{
				(void)WriteBytes(staged / path, shaderMarker);
			}
		}

		void Pack(const ExportWorkspace& workspace)
		{
			tepak::PackagingRequest request;
			request.inputRoot = workspace.staged;
			request.outputPackPath = workspace.archive;
			request.workerCount = 1U;
			request.enableDictionaryTraining = false;
			request.enableCompression = false;
			request.overwriteExisting = true;
			(void)tepak::PackWriter::Create(request);
		}

		bool SaveGeneratedPrefabSidecar(
			const ExportWorkspace& workspace,
			AnimationGuid packageGuid,
			AnimationGuid prefabGuid,
			const std::filesystem::path& runtimePath,
			std::string contentHashHex = {})
		{
			using namespace TerraEngine::AssetPipeline;
			ModelPackageImportResult result;
			result.status = ModelImportStatus::Succeeded;
			result.packageGuid = packageGuid.value;
			result.animationManifest.packageGuid = packageGuid.value;
			result.animationManifest.sourceHashHex = "mixed-case-source";
			result.animationManifest.importerName = "GltfPackageImporter";
			result.animationManifest.settingsFingerprintHex = "mixed-case-settings";
			result.animationManifest.candidateGeneration = 1U;
			result.animationManifest.publishedGeneration = 1U;
			result.animationManifest.lastKnownGoodGeneration = 1U;
			result.animationManifest.publishedAtomically = true;

			GeneratedAnimationSubAsset prefab;
			prefab.guid = prefabGuid.value;
			prefab.kind = AnimationImportSubAssetKind::Prefab;
			prefab.semanticKey = "prefab:hero";
			prefab.displayName = "Hero";
			prefab.sourcePath = runtimePath;
			prefab.contentHashHex = std::move(contentHashHex);
			result.animationManifest.subAssets.push_back(std::move(prefab));
			CanonicalizeAnimationImportManifest(result.animationManifest);
			result.animationManifest.dependencyFingerprintHex =
				BuildAnimationImportDependencyFingerprint(result.animationManifest);

			const std::filesystem::path sourcePath =
				workspace.assets / "Models" / "Hero.gltf";
			return SaveModelPackageImportResult(sourcePath, result);
		}

		bool SaveGeneratedSkeletonSidecar(
			const ExportWorkspace& workspace,
			AnimationGuid packageGuid,
			AnimationGuid skeletonGuid,
			const std::filesystem::path& runtimePath,
			std::string contentHashHex)
		{
			using namespace TerraEngine::AssetPipeline;
			ModelPackageImportResult result;
			result.status = ModelImportStatus::Succeeded;
			result.packageGuid = packageGuid.value;
			result.animationManifest.packageGuid = packageGuid.value;
			result.animationManifest.sourceHashHex = "skeleton-source";
			result.animationManifest.importerName = "GltfPackageImporter";
			result.animationManifest.settingsFingerprintHex = "skeleton-settings";
			result.animationManifest.candidateGeneration = 1U;
			result.animationManifest.publishedGeneration = 1U;
			result.animationManifest.lastKnownGoodGeneration = 1U;
			result.animationManifest.publishedAtomically = true;

			GeneratedAnimationSubAsset skeleton;
			skeleton.guid = skeletonGuid.value;
			skeleton.kind = AnimationImportSubAssetKind::Skeleton;
			skeleton.semanticKey = "skeleton:hero";
			skeleton.displayName = "Hero";
			skeleton.sourcePath = "Models/Hero.gltf";
			skeleton.compiledPath = runtimePath;
			skeleton.compatibilityFingerprint = 0x8844U;
			skeleton.contentHashHex = std::move(contentHashHex);
			result.animationManifest.subAssets.push_back(std::move(skeleton));
			CanonicalizeAnimationImportManifest(result.animationManifest);
			result.animationManifest.dependencyFingerprintHex =
				BuildAnimationImportDependencyFingerprint(result.animationManifest);

			return SaveModelPackageImportResult(
				workspace.assets / "Models" / "Hero.gltf", result);
		}
	}

	void RegisterAnimationExportTests(TestRegistry& registry)
	{
		registry.Add("Animation.Export.CookedRuntimeFixtureBuildsAllClassesWithExpectedHashes", [](TestContext& context) {
			const std::filesystem::path manifestPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Tests/Fixtures/AnimationExportRuntime/cooked-assets.json";
			std::ifstream input(manifestPath);
			nlohmann::json manifest;
			input >> manifest;
			context.Expect(input.good() || input.eof(),
				"The executable cooked animation fixture manifest must be readable.");
			const auto& assets = manifest.at("assets");
			context.Expect(assets.size() == 7U,
				"The executable fixture must cover every append-only animation class.");
			context.Expect(!Export::AnimationAssetTypeCode("future-animation-class").has_value(),
				"Unknown future classes must retain the generic archive fallback instead of aliasing a known type.");
			for (const nlohmann::json& asset : assets)
			{
				std::vector<AnimationDependencyEntry> dependencies;
				if (asset.contains("dependencySeed"))
				{
					dependencies.push_back({MakeGuid(asset.at("dependencySeed").get<std::uint8_t>()),
						asset.at("dependencyType").get<std::uint32_t>(), 1U});
				}
				const std::vector<std::uint8_t> cooked = MakeCookedAsset(
					MakeGuid(asset.at("seed").get<std::uint8_t>()), dependencies);
				AnimationAssetReader reader;
				std::uint64_t fingerprint = 0U;
				const bool parsed = reader.Parse(cooked) == AnimationStatus::Ok;
				if (parsed)
				{
					std::memcpy(&fingerprint, reader.Envelope().contentHash.data(), sizeof(fingerprint));
				}
				const std::uint64_t expected = std::stoull(
					asset.at("expectedContentFingerprint").get<std::string>(), nullptr, 16);
				context.Expect(parsed && fingerprint == expected
					&& Export::AnimationAssetClassForPath(asset.at("path").get<std::string>())
						== asset.at("class").get<std::string>(),
					"Fixture cook output must preserve its declared class and exact validated content hash.");
			}
		});

		registry.Add("Animation.Export.TransitiveClosureAndFailClosedEvidence", [](TestContext& context) {
			ExportWorkspace workspace;
			struct CookedClassFixture
			{
				const char* path;
				AnimationGuid guid;
			};
			const std::array<CookedClassFixture, 7U> fixtures{{
				{"characters/hero.teskel", MakeGuid(10U)},
				{"characters/walk.teanim", MakeGuid(40U)},
				{"controllers/hero.teanimctrl", MakeGuid(70U)},
				{"masks/upper.teanimmask", MakeGuid(90U)},
				{"retarget/humanoid.teretarget", MakeGuid(110U)},
				{"motion/locomotion.temotiondb", MakeGuid(130U)},
				{"ragdoll/hero.teragdoll", MakeGuid(150U)}
			}};
			const AnimationDependencyEntry skeletonDependency{fixtures[0].guid, 6U, 1U};
			const std::array<AnimationDependencyEntry, 1U> dependencies{{skeletonDependency}};
			bool stagedAllClasses = true;
			for (std::size_t index = 0U; index < fixtures.size(); ++index)
			{
				stagedAllClasses = WriteBytes(workspace.staged / fixtures[index].path,
					index == 1U ? MakeCookedAsset(fixtures[index].guid, dependencies)
						: MakeCookedAsset(fixtures[index].guid)) && stagedAllClasses;
			}
			context.Expect(stagedAllClasses,
				"The fixture must stage a valid envelope for every cooked animation class.");
			StageRequiredShaders(workspace.staged);

			const Export::AnimationExportClosure closure =
				Export::AnimationExportDependencyResolver::Resolve({workspace.assets, workspace.staged});
			context.Expect(closure.Complete() && closure.assets.size() == fixtures.size(),
				"All seven classes and the clip-to-skeleton edge must resolve from cooked identities.");
			context.Expect(Export::AnimationPackValidator::Validate(closure, 1U, true).accepted,
				"A complete supported closure should pass staging preflight.");
			context.Expect(!Export::AnimationPackValidator::Validate(closure, 2U, true).accepted,
				"Unsupported cooked major versions must fail before export publication.");

			std::string manifestError;
			context.Expect(Export::AnimationExportDependencyResolver::WriteRuntimeManifests(
				closure, workspace.staged, manifestError),
				"A complete closure must produce a sanitized runtime manifest: " + manifestError);
			Pack(workspace);
			const Export::AnimationPackValidationReport archiveReport =
				Export::AnimationPackValidator::ValidateArchive(workspace.archive);
			context.Expect(archiveReport.accepted
				&& archiveReport.checkedAnimationAssets == fixtures.size()
				&& archiveReport.checkedDependencyEdges == 1U,
				"The final TEPAK validator must reconstruct all seven classes and their dependency edge.");
		});

		registry.Add("Animation.Export.GeneratedPrefabUsesCanonicalPhysicalPath", [](TestContext& context) {
			ExportWorkspace workspace;
			const AnimationGuid packageGuid = MakeGuid(21U);
			const AnimationGuid prefabGuid = MakeGuid(51U);
			const std::filesystem::path authoredRuntimePath =
				"Models/Hero_Generated/Hero.teprefab";
			const std::array<std::uint8_t, 14U> prefabBytes{{
				'{', '"', 'o', 'b', 'j', 'e', 'c', 't', 's', '"', ':', '[', ']', '}'}};
			const std::vector<std::uint8_t> skeletonBytes =
				MakeCookedAsset(MakeGuid(91U));
			context.Expect(
				WriteBytes(workspace.staged / authoredRuntimePath, prefabBytes)
					&& WriteBytes(
						workspace.staged / "Models/Hero_Generated/Hero.teskel",
						skeletonBytes)
					&& SaveGeneratedPrefabSidecar(
						workspace,
						packageGuid,
						prefabGuid,
						authoredRuntimePath,
						"0000000000000000"),
				"The generated-prefab fixture must publish its staged document and sidecar.");
			StageRequiredShaders(workspace.staged);

			const Export::AnimationExportClosure closure =
				Export::AnimationExportDependencyResolver::Resolve(
					{workspace.assets, workspace.staged});
			const auto prefab = std::find_if(
				closure.assets.begin(), closure.assets.end(), [](const auto& asset) {
					return asset.path.ends_with(".teprefab");
				});
			context.Expect(
				closure.Complete() && prefab != closure.assets.end()
					&& prefab->generatedPackageAsset
					&& !prefab->cookedAnimationEnvelope
					&& prefab->assetType == Export::kUnknownAnimationExportAssetType
					&& prefab->path == "models/hero_generated/hero.teprefab"
					&& prefab->byteSize == prefabBytes.size()
					&& !prefab->fileHash.empty(),
				"A generated prefab must retain its dependency identity while reading "
				"the original-case physical file on case-sensitive hosts, even after "
				"export intentionally rewrites its staged document bytes.");

			std::string manifestError;
			context.Expect(
				Export::AnimationExportDependencyResolver::WriteRuntimeManifests(
					closure, workspace.staged, manifestError),
				"The generated prefab should produce source-free dependency manifests: "
					+ manifestError);
			Pack(workspace);
			context.Expect(
				Export::AnimationPackValidator::ValidateArchive(workspace.archive).accepted,
				"The archive validator must accept the prefab's intentionally untyped "
				"dependency-document record alongside cooked animation assets with "
				"legitimately empty optional manifest fields.");
		});

		registry.Add("Animation.Export.GeneratedManifestHashMatchesImporterContract", [](TestContext& context) {
			ExportWorkspace workspace;
			const AnimationGuid packageGuid = MakeGuid(31U);
			const AnimationGuid skeletonGuid = MakeGuid(61U);
			const std::filesystem::path runtimePath = "Models/Hero_Generated/Hero.teskel";
			const std::vector<std::uint8_t> cookedBytes = MakeCookedAsset(skeletonGuid);
			context.Expect(
				WriteBytes(workspace.staged / runtimePath, cookedBytes)
					&& SaveGeneratedSkeletonSidecar(
						workspace,
						packageGuid,
						skeletonGuid,
						runtimePath,
						ImporterFileHash(cookedBytes)),
				"The fixture must publish a generated payload with its importer-authored hash.");

			const Export::AnimationExportClosure matching =
				Export::AnimationExportDependencyResolver::Resolve(
					{workspace.assets, workspace.staged});
			context.Expect(
				std::none_of(matching.issues.begin(), matching.issues.end(), [](const auto& issue) {
					return issue.code == "animation.export.manifest_hash_stale";
				}),
				"A staged payload matching the importer hash contract must not be rejected as stale.");

			const std::vector<std::uint8_t> changedBytes =
				MakeCookedAsset(skeletonGuid, {}, 5U);
			context.Expect(
				WriteBytes(workspace.staged / runtimePath, changedBytes),
				"The fixture must replace the staged payload without republishing its sidecar.");
			const Export::AnimationExportClosure stale =
				Export::AnimationExportDependencyResolver::Resolve(
					{workspace.assets, workspace.staged});
			context.Expect(
				std::any_of(stale.issues.begin(), stale.issues.end(), [](const auto& issue) {
					return issue.code == "animation.export.manifest_hash_stale";
				}),
				"A changed staged payload must still be rejected until its import manifest is republished.");
		});

		registry.Add("Animation.Export.MixedCaseShaderTreeResolvesCanonically", [](TestContext& context) {
			ExportWorkspace workspace;
			const std::filesystem::path skeletonPath =
				workspace.staged / "Characters" / "Hero.teskel";
			context.Expect(
				WriteBytes(skeletonPath, MakeCookedAsset(MakeGuid(81U))),
				"The mixed-case shader fixture must stage a valid animation root.");
			const std::array<std::uint8_t, 4U> shaderMarker{{0x03U, 0x02U, 0x23U, 0x07U}};
			for(const std::string& logicalPath : Export::RequiredAnimationShaderPaths())
			{
				const std::filesystem::path actualPath =
					workspace.staged / "Shaders" / "Animation"
					/ std::filesystem::path(logicalPath).filename();
				context.Expect(
					WriteBytes(actualPath, shaderMarker),
					"Every required shader must be staged beneath the mixed-case tree.");
			}

			const Export::AnimationExportClosure closure =
				Export::AnimationExportDependencyResolver::Resolve(
					{workspace.assets, workspace.staged});
			context.Expect(
				closure.Complete() && closure.HasAnimationAssets(),
				"Canonical shader lookup must not depend on host filesystem casing.");
		});

		registry.Add("Animation.Export.MissingCorruptShaderAndSourceEntriesFailClosed", [](TestContext& context) {
			ExportWorkspace workspace;
			const AnimationGuid skeletonGuid = MakeGuid(70U);
			const AnimationGuid clipGuid = MakeGuid(90U);
			const std::array<AnimationDependencyEntry, 1U> dependencies{{
				AnimationDependencyEntry{skeletonGuid, 6U, 1U}}};
			context.Expect(WriteBytes(
				workspace.staged / "characters/walk.teanim",
				MakeCookedAsset(clipGuid, dependencies)),
				"The missing-dependency fixture must stage its owner asset.");
			StageRequiredShaders(workspace.staged);
			const Export::AnimationExportClosure missing =
				Export::AnimationExportDependencyResolver::Resolve({workspace.assets, workspace.staged});
			context.Expect(!missing.Complete(),
				"A required skeleton absent from staging must reject the entire closure.");

			std::vector<std::uint8_t> corrupt = MakeCookedAsset(skeletonGuid);
			corrupt.back() ^= 0x1U;
			context.Expect(WriteBytes(workspace.staged / "characters/hero.teskel", corrupt),
				"The corrupt fixture must be written completely.");
			const Export::AnimationExportClosure corruptClosure =
				Export::AnimationExportDependencyResolver::Resolve({workspace.assets, workspace.staged});
			context.Expect(!corruptClosure.Complete(),
				"A cooked content-hash mismatch must preserve the last good export by rejecting staging.");

			context.Expect(WriteBytes(
				workspace.staged / "characters/hero.teskel", MakeCookedAsset(skeletonGuid)),
				"The valid replacement fixture must be staged.");
			std::filesystem::remove(workspace.staged / Export::RequiredAnimationShaderPaths().front());
			const Export::AnimationExportClosure missingShader =
				Export::AnimationExportDependencyResolver::Resolve({workspace.assets, workspace.staged});
			context.Expect(!missingShader.Complete(),
				"An archive missing a required animation shader variant must fail before packaging.");

			StageRequiredShaders(workspace.staged);
			const Export::AnimationExportClosure complete =
				Export::AnimationExportDependencyResolver::Resolve({workspace.assets, workspace.staged});
			std::string manifestError;
			context.Expect(Export::AnimationExportDependencyResolver::WriteRuntimeManifests(
				complete, workspace.staged, manifestError),
				"The repaired fixture must produce its manifest.");
			const std::array<std::uint8_t, 3U> sourceMarker{{'F', 'B', 'X'}};
			context.Expect(WriteBytes(workspace.staged / "source/hero.fbx", sourceMarker),
				"The forbidden-source fixture must enter the archive input.");
			Pack(workspace);
			context.Expect(!Export::AnimationPackValidator::ValidateArchive(workspace.archive).accepted,
				"Deep validation must reject editor/source-only files even when closure is otherwise complete.");
		});
	}
}
