// AnimationSubAssetIdentityTests.cpp - Contract tests for package-child identity.
//
// Animation assets are commonly referenced by scenes, prefabs, controllers,
// and gameplay handles. These tests freeze the public sidecar requirements so
// source moves, display-name changes, and exporter ordering changes cannot
// silently retarget any of those references.

#include "TestHarness.h"

#include "AssetPipeline/FbxTestHelpers.h"
#include "AssetImportResult.h"
#include "AssetDatabase.h"
#include "ModelPackageImporter.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		/// Mirrors the production boundary conversion so this regression can inspect
		/// and clean paths beyond MAX_PATH even when the test host manifest is absent.
		std::filesystem::path NativeIoPath(const std::filesystem::path& path)
		{
#if defined(_WIN32)
			if(path.empty()) return path;
			std::filesystem::path absolutePath = path;
			if(!absolutePath.is_absolute())
			{
				std::error_code error;
				absolutePath = std::filesystem::absolute(absolutePath, error);
				if(error) return path;
			}
			std::filesystem::path preferred = absolutePath.lexically_normal();
			preferred.make_preferred();
			const std::filesystem::path::string_type& native = preferred.native();
			constexpr std::wstring_view extendedPrefix = LR"(\\?\)";
			constexpr std::wstring_view devicePrefix = LR"(\\.\)";
			constexpr std::wstring_view uncPrefix = LR"(\\)";
			if(native.starts_with(extendedPrefix) || native.starts_with(devicePrefix)) return preferred;

			std::filesystem::path::string_type extended;
			if(native.starts_with(uncPrefix))
			{
				extended.assign(LR"(\\?\UNC\)");
				extended.append(native.substr(uncPrefix.size()));
			}
			else
			{
				extended.assign(extendedPrefix);
				extended.append(native);
			}
			return std::filesystem::path(std::move(extended));
#else
			return path;
#endif
		}

		TerraEngine::AssetPipeline::FbxPackageImportOptions MakeIdentityOptions(
			const std::filesystem::path& sourceRelativePath,
			const TerraEngine::UUID& packageGuid)
		{
			const std::filesystem::path fixturePath = AssimpFbxFixturePath("animation_with_skeleton.fbx");
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "TerraEngineAnimationSubAssetIdentityTests";

			TerraEngine::AssetPipeline::FbxPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath = sourceRelativePath;
			options.packageGuid = packageGuid;
			options.forceReimport = true;
			return options;
		}

		bool HasDiagnostic(const TerraEngine::AssetPipeline::ModelPackageImportResult& result, std::string_view code)
		{
			for(const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic : result.diagnostics)
			{
				if(diagnostic.code == code)
				{
					return true;
				}
			}
			return false;
		}

		std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path)
		{
			// Publication tests compare the complete file rather than a timestamp so
			// an interrupted candidate cannot hide a partial overwrite behind coarse
			// filesystem clock resolution.
			std::ifstream input(NativeIoPath(path), std::ios::binary);
			return std::vector<std::uint8_t>(
				std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		}

		void WriteFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
		{
			// The coordinated-reload test restores its disposable cooked clip before
			// retrying the same package generation. Keeping that write in one helper
			// makes the intentional corruption and exact recovery easy to audit.
			std::ofstream output(NativeIoPath(path), std::ios::binary | std::ios::trunc);
			if(!bytes.empty())
			{
				output.write(reinterpret_cast<const char*>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
			}
		}

		class ScopedAnimationAssetDatabaseFixture final
		{
		public:
			explicit ScopedAnimationAssetDatabaseFixture(std::filesystem::path root)
				: root_(std::move(root))
			{
				// A cache generation ledger survives ordinary cache erasure by design.
				// The test therefore compares relative generations and starts from a
				// fully shut-down database rather than assuming generation one in RAM.
				AssetDatabase::Instance().Shutdown();
				ResourceManager::Instance().ClearCaches();
				std::filesystem::remove_all(root_);
			}

			~ScopedAnimationAssetDatabaseFixture()
			{
				// Always remove the deterministic failure hook, including when an early
				// fixture assertion returns, so later editor tests cannot inherit it.
				AssetDatabase::Instance().SetAnimationPackagePostPublisherPrepareHookForTests({});
				AssetDatabase::Instance().Shutdown();
				ResourceManager::Instance().ClearCaches();
				std::filesystem::remove_all(root_);
			}

			ScopedAnimationAssetDatabaseFixture(const ScopedAnimationAssetDatabaseFixture&) = delete;
			ScopedAnimationAssetDatabaseFixture& operator=(const ScopedAnimationAssetDatabaseFixture&) = delete;

		private:
			std::filesystem::path root_;
		};

		void WritePackageMetadata(
			const std::filesystem::path& assetRoot,
			const std::filesystem::path& sourceRelativePath,
			const TerraEngine::UUID& packageGuid)
		{
			const std::filesystem::path sourcePath = assetRoot / sourceRelativePath;
			std::filesystem::path compiledPath = sourceRelativePath;
			compiledPath.replace_extension(".temf");
			nlohmann::json metadata = {
				{ "guid", TerraEngine::ToHexString(packageGuid) },
				{ "importerName", TerraEngine::AssetPipeline::GetModelPackageImporterName(sourcePath) },
				{ "lastModified", std::filesystem::last_write_time(sourcePath).time_since_epoch().count() },
				{ "sourcePath", sourceRelativePath.generic_string() },
				{ "compiledPath", compiledPath.generic_string() },
				{ "settings", nlohmann::json::object() }
			};
			// This is the same public sidecar shape AssetDatabase persists. Seeding it
			// lets startup exercise registration without launching a redundant compiler.
			std::ofstream output(sourcePath.string() + ".meta", std::ios::trunc);
			output << std::setw(4) << metadata << '\n';
		}

		bool PumpAssetDatabaseUntilPackageVisible(const TerraEngine::UUID& packageGuid)
		{
			for(int attempt = 0; attempt < 160; ++attempt)
			{
				AssetDatabase::Instance().Update();
				if(AssetDatabase::Instance().FindAssetByGuid(packageGuid) != nullptr)
				{
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			return false;
		}

		bool HasCompactTransactionStage(const std::filesystem::path& root, std::string_view prefix)
		{
			std::error_code error;
			const std::filesystem::path nativeRoot = NativeIoPath(root);
			if(!std::filesystem::is_directory(nativeRoot, error)) return false;
			for(std::filesystem::directory_iterator iterator(nativeRoot, error), end;
				iterator != end && !error; iterator.increment(error))
			{
				if(iterator->is_directory(error) && iterator->path().filename().string().starts_with(prefix))
				{
					return true;
				}
			}
			return false;
		}

		bool HasStagedAnimationManifest(
			const std::filesystem::path& buildRoot,
			const TerraEngine::UUID& packageGuid)
		{
			std::error_code error;
			const std::filesystem::path nativeRoot = NativeIoPath(buildRoot);
			for(std::filesystem::directory_iterator iterator(nativeRoot, error), end;
				iterator != end && !error; iterator.increment(error))
			{
				if(!iterator->is_directory(error)
					|| !iterator->path().filename().string().starts_with(".tb-"))
				{
					continue;
				}
				const std::filesystem::path stagedManifest = iterator->path()
					/ ".terra-animation-manifests"
					/ (TerraEngine::ToHexString(packageGuid) + ".json");
				if(std::filesystem::is_regular_file(stagedManifest, error)) return true;
			}
			return false;
		}

		void ExpectAnimationSubassetsArePublished(TestContext& context, const TerraEngine::AssetPipeline::ModelPackageImportResult& result, std::string_view requirement)
		{
			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed,
				std::string(requirement) + " requires an animated package to publish successfully; unrelated source warnings may remain visible.");
			context.Expect(!HasDiagnostic(result, "UnsupportedSkeletonIgnored"),
				std::string(requirement) + " requires skeleton child identity to be published.");
			context.Expect(!HasDiagnostic(result, "UnsupportedAnimationIgnored"),
				std::string(requirement) + " requires clip child identity to be published.");
			const bool hasSkeleton = std::any_of(
				result.animationManifest.subAssets.begin(), result.animationManifest.subAssets.end(), [](const auto& child) {
					return child.kind == TerraEngine::AssetPipeline::AnimationImportSubAssetKind::Skeleton;
				});
			const bool hasClip = std::any_of(
				result.animationManifest.subAssets.begin(), result.animationManifest.subAssets.end(), [](const auto& child) {
					return child.kind == TerraEngine::AssetPipeline::AnimationImportSubAssetKind::AnimationClip;
				});
			context.Expect(hasSkeleton && hasClip,
				std::string(requirement) + " must publish typed skeleton and clip children, not merely suppress legacy diagnostics.");
		}
	}

	void RegisterAnimationSubAssetIdentityTests(TestRegistry& registry)
	{
		registry.Add("Editor.Animation.SubAssetIdentity.MoveRenameReimportAndReloadPreserveSemanticIds", [](TestContext& context) {
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto original = TerraEngine::AssetPipeline::ImportFbxPackage(MakeIdentityOptions("Models/OriginalCharacter.fbx", packageGuid));
			const auto movedAndRenamed = TerraEngine::AssetPipeline::ImportFbxPackage(MakeIdentityOptions("Characters/HeroRenamed.fbx", packageGuid));
			ExpectAnimationSubassetsArePublished(context, original, "The initial animated package import");
			ExpectAnimationSubassetsArePublished(context, movedAndRenamed, "The moved and renamed animated package reimport");
			for(const auto& originalChild : original.animationManifest.subAssets)
			{
				if(originalChild.kind != TerraEngine::AssetPipeline::AnimationImportSubAssetKind::Skeleton
					&& originalChild.kind != TerraEngine::AssetPipeline::AnimationImportSubAssetKind::AnimationClip)
				{
					continue;
				}
				const auto movedChild = std::find_if(
					movedAndRenamed.animationManifest.subAssets.begin(), movedAndRenamed.animationManifest.subAssets.end(),
					[&](const auto& candidate) {
						return candidate.kind == originalChild.kind
							&& candidate.semanticKey == originalChild.semanticKey;
					});
				context.Expect(movedChild != movedAndRenamed.animationManifest.subAssets.end()
					&& movedChild->guid == originalChild.guid,
					"Moving or renaming the source package must preserve every semantic skeleton/clip child GUID.");
			}

			// Package identity itself is already persisted. T037 extends this same
			// publication/reload route with semantic skeleton and clip children.
			const std::filesystem::path savedPackagePath = std::filesystem::temp_directory_path() / "TerraEngineAnimationSubAssetIdentityTests" / "Assets/Characters/HeroRenamed.fbx";
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(savedPackagePath, movedAndRenamed), "Moved package sidecar should save before a simulated project reload.");
			TerraEngine::AssetPipeline::ModelPackageImportResult reloaded;
			context.Expect(TerraEngine::AssetPipeline::LoadModelPackageImportResult(savedPackagePath, reloaded), "Project reload must restore the moved package manifest.");
			context.ExpectEqual(TerraEngine::ToHexString(packageGuid), TerraEngine::ToHexString(reloaded.packageGuid), "Package GUID must survive a sidecar save and project reload.");
			context.Expect(reloaded.animationManifest.subAssets.size()
				== movedAndRenamed.animationManifest.subAssets.size(),
				"Project reload must preserve every generated animation package child.");
		});

		registry.Add("Editor.Animation.SubAssetIdentity.ReorderAndPrefabReferencesUseStableSemanticKeys", [](TestContext& context) {
			using namespace TerraEngine::AssetPipeline;
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			ModelPackageImportOptions options = MakeIdentityOptions("Models/PrefabBindings.fbx", packageGuid);
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path()
				/ "TerraEngineAnimationSubAssetIdentityTests" / "PrefabBindings"
				/ TerraEngine::ToHexString(packageGuid);
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			const ModelPackageImportResult result = ImportModelPackage(options);
			// Reordering source arrays must be harmless: semantic keys, not source
			// positions, are persisted into the prefab and manifest bindings.
			ExpectAnimationSubassetsArePublished(context, result, "The reorder-safe prefab and controller reference contract");
			context.Expect(result.generatedPrefabs.size() == 1u,
				"An animated package import must publish one playable prefab root.");
			if(result.generatedPrefabs.size() != 1u) return;

			const std::filesystem::path prefabPath = options.assetRoot / result.generatedPrefabs.front().sourcePath;
			std::ifstream prefabInput(prefabPath, std::ios::binary);
			nlohmann::json prefabDocument;
			if(prefabInput)
			{
				try
				{
					prefabInput >> prefabDocument;
				}
				catch(const std::exception&)
				{
					prefabDocument = nlohmann::json();
				}
			}
			context.Expect(prefabDocument.contains("animationBindings")
				&& prefabDocument["animationBindings"].is_object(),
				"The generated prefab must expose stable animation binding metadata for editor preview/controller authoring.");
			context.Expect(prefabDocument.contains("objects") && prefabDocument["objects"].is_array()
				&& !prefabDocument["objects"].empty(),
				"The generated prefab must contain its authored root object.");
			if(!prefabDocument.contains("animationBindings") || !prefabDocument["animationBindings"].is_object()
				|| !prefabDocument.contains("objects") || !prefabDocument["objects"].is_array()
				|| prefabDocument["objects"].empty()) return;

			const nlohmann::json& bindings = prefabDocument["animationBindings"];
			const nlohmann::json& root = prefabDocument["objects"].front();
			const auto firstSkeleton = std::find_if(
				result.animationManifest.subAssets.begin(), result.animationManifest.subAssets.end(),
				[](const GeneratedAnimationSubAsset& child) {
					return child.kind == AnimationImportSubAssetKind::Skeleton;
				});
			context.Expect(firstSkeleton != result.animationManifest.subAssets.end(),
				"The prefab binding test fixture must publish a skeleton.");
			if(firstSkeleton == result.animationManifest.subAssets.end()) return;

			const std::string primarySkeletonId = bindings.value("primarySkeletonAssetId", std::string{});
			const std::string defaultClipId = bindings.value("defaultClipAssetId", std::string{});
			context.Expect(primarySkeletonId == TerraEngine::ToHexString(firstSkeleton->guid),
				"The prefab must deterministically select the first canonical semantic skeleton as its authored override.");
			context.Expect(bindings.value("schemaVersion", 0u) == 1u
				&& bindings.contains("skeletonAssetIds") && bindings["skeletonAssetIds"].is_array()
				&& bindings.contains("clipAssetIds") && bindings["clipAssetIds"].is_array()
				&& bindings.contains("deformableMeshAssetIds") && bindings["deformableMeshAssetIds"].is_array()
				&& bindings.contains("morphTargetAssetIds") && bindings["morphTargetAssetIds"].is_array(),
				"Binding metadata must version and separate skeleton, clip, deformable-mesh, and morph asset lists.");

			context.Expect(root.contains("components") && root["components"].is_object(),
				"The generated prefab root must expose its authored component object.");
			if(!root.contains("components") || !root["components"].is_object()) return;
			const nlohmann::json& components = root["components"];
			context.Expect(components.contains("AnimatorComponent"),
				"An animated generated prefab must author AnimatorComponent on its root.");
			if(!components.contains("AnimatorComponent")) return;
			const nlohmann::json& animator = components["AnimatorComponent"];
			context.Expect(animator.value("skeletonOverrideAssetId", std::string{}) == primarySkeletonId,
				"AnimatorComponent must bind the generated primary skeleton.");
			context.Expect(animator.value("controllerAssetId", std::string{})
				== TerraEngine::ToHexString(TerraEngine::EMPTY_UUID)
				&& animator.value("controllerAssetId", std::string{}) != defaultClipId,
				"Import must leave controllerAssetId empty instead of treating the default clip as a controller.");
			context.Expect(animator.value("defaultClipAssetId", std::string{}) == defaultClipId,
				"The authored AnimatorComponent must retain the generated default clip used for direct playback.");
			context.Expect(animator.value("loopDefaultClip", false),
				"Imported direct-clip prefabs should loop by default until a creator disables the Inspector option.");
			context.Expect(animator.value("playAutomatically", false),
				"Imported animated prefabs should play automatically until a creator disables the Inspector option.");
			context.Expect(animator.value("defaultClipStartFrame", std::uint32_t{99U}) == 0U,
				"Imported direct-clip prefabs should begin at frame zero until a creator changes the Inspector option.");
			context.Expect(animator.value("requestedMode", std::uint32_t{99u}) == 0u,
				"The imported AnimatorComponent must request the Auto execution policy.");

			const auto prefabChild = std::find_if(
				result.animationManifest.subAssets.begin(), result.animationManifest.subAssets.end(),
				[](const GeneratedAnimationSubAsset& child) {
					return child.kind == AnimationImportSubAssetKind::Prefab;
				});
			context.Expect(prefabChild != result.animationManifest.subAssets.end(),
				"The package manifest must publish the generated prefab as a typed child.");
			if(prefabChild == result.animationManifest.subAssets.end()) return;
			for(const GeneratedAnimationSubAsset& child : result.animationManifest.subAssets)
			{
				const bool isBinding = child.kind == AnimationImportSubAssetKind::Skeleton
					|| child.kind == AnimationImportSubAssetKind::AnimationClip
					|| child.kind == AnimationImportSubAssetKind::DeformableMesh
					|| child.kind == AnimationImportSubAssetKind::MorphTarget;
				if(!isBinding) continue;
				const auto dependency = std::find_if(
					prefabChild->dependencies.begin(), prefabChild->dependencies.end(), [&](const auto& candidate) {
						return candidate.assetGuid == child.guid && candidate.required
							&& candidate.compatibilityFingerprint == child.compatibilityFingerprint;
					});
				context.Expect(dependency != prefabChild->dependencies.end(),
					"The prefab manifest child must depend on every generated animation binding with its compatibility fingerprint.");
			}

			std::string validationFailure;
			context.Expect(ValidateAnimationImportManifestDependencies(result.animationManifest, &validationFailure),
				"The complete generated prefab dependency graph must pass publication validation.");
			AnimationImportManifest incomplete = result.animationManifest;
			const auto mutablePrefab = std::find_if(
				incomplete.subAssets.begin(), incomplete.subAssets.end(), [](const GeneratedAnimationSubAsset& child) {
					return child.kind == AnimationImportSubAssetKind::Prefab;
				});
			if(mutablePrefab != incomplete.subAssets.end())
			{
				mutablePrefab->dependencies.erase(std::remove_if(
					mutablePrefab->dependencies.begin(), mutablePrefab->dependencies.end(), [&](const auto& dependency) {
						return dependency.assetGuid == firstSkeleton->guid;
					}), mutablePrefab->dependencies.end());
			}
			context.Expect(!ValidateAnimationImportManifestDependencies(incomplete, &validationFailure)
				&& !validationFailure.empty(),
				"Publication validation must diagnose a generated prefab that loses a required binding edge.");
		});

		registry.Add("Editor.Animation.SubAssetIdentity.ManifestCanonicalizationAndReloadPreserveDependencies", [](TestContext& context) {
			using namespace TerraEngine::AssetPipeline;
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const TerraEngine::UUID skeletonGuid = BuildStableAnimationSubAssetUuid(
				packageGuid, AnimationImportSubAssetKind::Skeleton, "Skeleton/Root/Hips");
			const TerraEngine::UUID normalizedSkeletonGuid = BuildStableAnimationSubAssetUuid(
				packageGuid, AnimationImportSubAssetKind::Skeleton, "./skeleton\\root\\HIPS");
			context.Expect(skeletonGuid == normalizedSkeletonGuid,
				"Path separators, redundant dot prefixes, and display-case changes must not alter semantic child identity.");

			ModelPackageImportResult saved;
			saved.status = ModelImportStatus::Succeeded;
			saved.packageGuid = packageGuid;
			saved.package.sourceHashHex = "source-hash";
			saved.animationManifest.packageGuid = packageGuid;
			saved.animationManifest.sourceHashHex = saved.package.sourceHashHex;
			saved.animationManifest.importerName = "FbxPackageImporter";
			saved.animationManifest.publishedGeneration = 7U;
			saved.animationManifest.publishedAtomically = true;

			GeneratedAnimationSubAsset skeleton;
			skeleton.guid = skeletonGuid;
			skeleton.kind = AnimationImportSubAssetKind::Skeleton;
			skeleton.semanticKey = "Skeleton/Root/Hips";
			skeleton.compiledPath = "Characters/Hero/Hero.teskel";
			skeleton.compatibilityFingerprint = 0x11223344U;
			GeneratedAnimationSubAsset clip;
			clip.guid = BuildStableAnimationSubAssetUuid(
				packageGuid, AnimationImportSubAssetKind::AnimationClip, "Clip/Locomotion/Walk");
			clip.kind = AnimationImportSubAssetKind::AnimationClip;
			clip.semanticKey = "Clip/Locomotion/Walk";
			clip.compiledPath = "Characters/Hero/Walk.teanim";
			clip.dependencies.push_back({skeleton.guid, skeleton.compatibilityFingerprint, true});
			saved.animationManifest.subAssets = {clip, skeleton};
			CanonicalizeAnimationImportManifest(saved.animationManifest);
			saved.animationManifest.dependencyFingerprintHex =
				BuildAnimationImportDependencyFingerprint(saved.animationManifest);

			AnimationImportManifest reordered = saved.animationManifest;
			std::reverse(reordered.subAssets.begin(), reordered.subAssets.end());
			context.Expect(BuildAnimationImportDependencyFingerprint(reordered)
				== saved.animationManifest.dependencyFingerprintHex,
				"Source array reorder must not change the canonical package dependency fingerprint.");

			const std::filesystem::path sourcePath = std::filesystem::temp_directory_path()
				/ "TerraEngineAnimationSubAssetIdentityTests" / "ManifestRoundTrip.fbx";
			std::filesystem::create_directories(sourcePath.parent_path());
			context.Expect(SaveModelPackageImportResult(sourcePath, saved),
				"The animation import manifest should publish through the atomic sidecar writer.");
			ModelPackageImportResult loaded;
			context.Expect(LoadModelPackageImportResult(sourcePath, loaded),
				"A project reload should rehydrate every animation package child and dependency.");
			context.Expect(loaded.animationManifest.publishedGeneration == 7U
				&& loaded.animationManifest.publishedAtomically
				&& loaded.animationManifest.subAssets.size() == 2U
				&& loaded.animationManifest.dependencyFingerprintHex
					== saved.animationManifest.dependencyFingerprintHex,
				"Manifest generation, publication state, children, and fingerprint must survive reload.");
			context.Expect(loaded.animationManifest.subAssets[1].dependencies.size() == 1U
				&& loaded.animationManifest.subAssets[1].dependencies[0].assetGuid == skeleton.guid,
				"The clip-to-skeleton dependency must remain generation-safe after reload.");
		});

		registry.Add("Editor.Animation.SubAssetIdentity.CancelledAndRejectedReimportsPreservePublishedGeneration", [](TestContext& context) {
			using namespace TerraEngine::AssetPipeline;
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path()
				/ "TerraEngineAnimationSubAssetIdentityTests" / "TransactionalPublication";
			std::filesystem::remove_all(tempRoot);
			std::filesystem::create_directories(tempRoot / "Source");
			const std::filesystem::path sourceFixture = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf";
			const std::filesystem::path sourceCopy = tempRoot / "Source/transactional.gltf";
			std::filesystem::copy_file(sourceFixture, sourceCopy, std::filesystem::copy_options::overwrite_existing);

			ModelPackageImportOptions options{};
			options.sourcePath = sourceCopy;
			options.sourceRelativePath = "Models/transactional.gltf";
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.packageGuid = TerraEngine::GenerateUUID();
			options.forceReimport = true;
			const ModelPackageImportResult published = ImportModelPackage(options);
			context.Expect(published.status != ModelImportStatus::Failed
				&& published.animationManifest.publishedAtomically
				&& published.animationManifest.publishedGeneration > 0U,
				"The baseline package generation must publish before rollback behavior is tested.");

			const auto compiledChild = std::find_if(
				published.animationManifest.subAssets.begin(), published.animationManifest.subAssets.end(),
				[](const GeneratedAnimationSubAsset& child) { return !child.compiledPath.empty(); });
			if(compiledChild == published.animationManifest.subAssets.end())
			{
				context.Expect(false, "A published package generation must expose at least one compiled child for byte-preservation checks.");
				return;
			}
			const std::filesystem::path liveChildPath = options.buildRoot / compiledChild->compiledPath;
			const std::filesystem::path liveManifestPath = options.buildRoot
				/ ".terra-animation-manifests" / (TerraEngine::ToHexString(options.packageGuid) + ".json");
			const std::vector<std::uint8_t> publishedChildBytes = ReadFileBytes(liveChildPath);
			const std::vector<std::uint8_t> publishedManifestBytes = ReadFileBytes(liveManifestPath);
			context.Expect(!publishedChildBytes.empty() && !publishedManifestBytes.empty(),
				"The live child and generation marker must both be readable before a candidate reimport begins.");

			bool reachedLatePreparation = false;
			ModelPackageImportOptions cancelledOptions = options;
			cancelledOptions.progressCallback = [&](float progress, std::string_view, std::string_view) {
				// The first cancellation poll happens before any importer progress. The
				// flag changes only after expensive preparation has written to staging.
				if(progress >= 0.90F) reachedLatePreparation = true;
			};
			cancelledOptions.cancellationRequested = [&]() { return reachedLatePreparation; };
			const ModelPackageImportResult cancelled = ImportModelPackage(cancelledOptions);
			context.Expect(cancelled.status == ModelImportStatus::Failed
				&& cancelled.animationManifest.cancelled
				&& cancelled.animationManifest.preservedLastKnownGood
				&& cancelled.animationManifest.lastKnownGoodGeneration
					== published.animationManifest.publishedGeneration,
				"Cancellation after preparation must reject the candidate while retaining the prior complete generation.");
			context.Expect(ReadFileBytes(liveChildPath) == publishedChildBytes
				&& ReadFileBytes(liveManifestPath) == publishedManifestBytes,
				"A cancelled candidate must leave every observed live byte unchanged.");

			// A parse failure exercises the same atomic rejection path without using
			// cancellation. The source is a disposable copy; repository fixtures are
			// never modified by this test.
			{
				std::ofstream corruptSource(sourceCopy, std::ios::binary | std::ios::trunc);
				corruptSource << "{ invalid gltf";
			}
			const ModelPackageImportResult rejected = ImportModelPackage(options);
			context.Expect(rejected.status == ModelImportStatus::Failed
				&& rejected.animationManifest.preservedLastKnownGood
				&& rejected.animationManifest.lastKnownGoodGeneration
					== published.animationManifest.publishedGeneration,
				"A malformed reimport must preserve the same last-known-good generation.");
			context.Expect(ReadFileBytes(liveChildPath) == publishedChildBytes
				&& ReadFileBytes(liveManifestPath) == publishedManifestBytes,
				"A rejected parse candidate must leave every observed live byte unchanged.");
			context.Expect(!std::filesystem::exists(options.assetRoot / ".terra-import-staging")
				&& !std::filesystem::exists(options.buildRoot / ".terra-import-staging")
				&& !HasCompactTransactionStage(options.assetRoot, ".ta-")
				&& !HasCompactTransactionStage(options.buildRoot, ".tb-"),
				"Rejected candidates must clean their isolated staging roots after rollback.");
		});

		registry.Add("Editor.Animation.SubAssetIdentity.AssetDatabasePublishesReadOnlyChildrenAndPrefabSummary", [](TestContext& context) {
			using namespace TerraEngine::AssetPipeline;
			AssetDatabase::Instance().Shutdown();
			ResourceManager::Instance().ClearCaches();

			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const std::filesystem::path root = std::filesystem::temp_directory_path()
				/ "TerraEngineAnimationAssetDatabaseTests" / TerraEngine::ToHexString(packageGuid);
			const std::filesystem::path assetRoot = root / "Assets";
			const std::filesystem::path buildRoot = root / "Build" / "Assets";
			const std::filesystem::path sourceRelativePath = "Models/Character.fbx";
			const std::filesystem::path sourcePath = assetRoot / sourceRelativePath;
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(sourcePath.parent_path());
			std::filesystem::create_directories(buildRoot);
			std::filesystem::copy_file(
				AssimpFbxFixturePath("animation_with_skeleton.fbx"), sourcePath,
				std::filesystem::copy_options::overwrite_existing);

			ModelPackageImportOptions options{};
			options.sourcePath = sourcePath;
			options.assetRoot = assetRoot;
			options.buildRoot = buildRoot;
			options.sourceRelativePath = sourceRelativePath;
			options.packageGuid = packageGuid;
			options.forceReimport = true;
			const ModelPackageImportResult result = ImportModelPackage(options);
			context.Expect(result.status != ModelImportStatus::Failed,
				"The AssetDatabase fixture package must publish before registration is tested.");
			context.Expect(SaveModelPackageImportResult(sourcePath, result),
				"The published package sidecar must be available to the startup database scan.");
			WritePackageMetadata(assetRoot, sourceRelativePath, packageGuid);

			AssetDatabase::Instance().Initialize(root);
			context.Expect(PumpAssetDatabaseUntilPackageVisible(packageGuid),
				"AssetDatabase startup must register the animated package parent.");
			const AssetMetadata* parent = AssetDatabase::Instance().FindAssetByGuid(packageGuid);
			if(parent != nullptr)
			{
				ModelPackageAnimationSummaryMetadata summary;
				context.Expect(AssetDatabase::Instance().TryGetModelPackageAnimationSummary(*parent, summary),
					"Inspector and automation must be able to read the package animation summary.");
				context.Expect(summary.publishedAtomically && summary.prefabBindingsValid
					&& summary.generatedChildCount == summary.readOnlyChildCount,
					"The summary must report one coherent read-only generation with valid prefab bindings.");
				context.Expect(summary.skeletonCount > 0u && summary.animationClipCount > 0u
					&& summary.deformableMeshCount > 0u && summary.prefabCount == 1u,
					"The package summary must expose its skeleton, clip, deformable mesh, and playable prefab.");
				context.Expect(parent->settings.contains("animationPackage"),
					"The parent metadata must persist the concise animation summary for fast Inspector startup.");
			}

			const auto findChild = [&](AnimationImportSubAssetKind kind) {
				return std::find_if(result.animationManifest.subAssets.begin(), result.animationManifest.subAssets.end(),
					[&](const GeneratedAnimationSubAsset& child) { return child.kind == kind; });
			};
			const auto skeleton = findChild(AnimationImportSubAssetKind::Skeleton);
			const auto clip = findChild(AnimationImportSubAssetKind::AnimationClip);
			const auto deformable = findChild(AnimationImportSubAssetKind::DeformableMesh);
			const auto prefab = findChild(AnimationImportSubAssetKind::Prefab);
			if(skeleton != result.animationManifest.subAssets.end()
				&& clip != result.animationManifest.subAssets.end()
				&& deformable != result.animationManifest.subAssets.end()
				&& prefab != result.animationManifest.subAssets.end())
			{
				GeneratedModelPackageChildMetadata skeletonMetadata;
				GeneratedModelPackageChildMetadata prefabMetadata;
				context.Expect(AssetDatabase::Instance().TryGetGeneratedModelPackageChildMetadata(
					skeleton->guid, GeneratedModelPackageChildKind::Skeleton, skeletonMetadata)
					&& skeletonMetadata.readOnlyPackageChild
					&& skeletonMetadata.publishedGeneration == result.animationManifest.publishedGeneration,
					"Skeleton children must resolve as read-only records from the published generation.");
				context.Expect(AssetDatabase::Instance().TryGetGeneratedModelPackageChildMetadata(
					prefab->guid, GeneratedModelPackageChildKind::Prefab, prefabMetadata)
					&& std::find(prefabMetadata.dependencies.begin(), prefabMetadata.dependencies.end(), skeleton->guid)
						!= prefabMetadata.dependencies.end(),
					"The generated prefab child must expose its stable skeleton binding dependency.");

				std::vector<TerraEngine::UUID> childIds;
				context.Expect(AssetDatabase::Instance().TryGetGeneratedModelPackageChildIds(
					packageGuid, GeneratedModelPackageChildKind::AnimationClip, childIds)
					&& std::find(childIds.begin(), childIds.end(), clip->guid) != childIds.end(),
					"Package child enumeration must include the published animation clip GUID.");
				childIds.clear();
				const bool exposedAsStaticMesh = AssetDatabase::Instance().TryGetGeneratedModelPackageChildIds(
					packageGuid, GeneratedModelPackageChildKind::Mesh, childIds)
					&& std::find(childIds.begin(), childIds.end(), deformable->guid) != childIds.end();
				context.Expect(!exposedAsStaticMesh,
					"A deformable child must not be duplicated under the ordinary static Mesh kind.");

				context.Expect(AssetDatabase::Instance().FindAssetByGuid(skeleton->guid) == nullptr
					&& AssetDatabase::Instance().FindAssetByGuid(clip->guid) == nullptr,
					"Generated skeleton and clip binaries must not become editable loose AssetMetadata rows.");
				context.Expect(!std::filesystem::exists(assetRoot / (skeleton->sourcePath.string() + ".meta"))
					&& !std::filesystem::exists(assetRoot / (clip->sourcePath.string() + ".meta")),
					"Read-only package animation children must not receive independent .meta sidecars.");

				const TerraEngine::UUID resolvedSkeleton = ResourceManager::Instance().ResolveAssetReference(
					(buildRoot / skeleton->compiledPath).generic_string(), AssetType::Skeleton);
				const TerraEngine::UUID resolvedClip = ResourceManager::Instance().ResolveAssetReference(
					(buildRoot / clip->compiledPath).generic_string(), AssetType::AnimationClip);
				const TerraEngine::UUID resolvedMesh = ResourceManager::Instance().ResolveAssetReference(
					(buildRoot / deformable->compiledPath).generic_string(), AssetType::Mesh);
				context.Expect(resolvedSkeleton == skeleton->guid && resolvedClip == clip->guid
					&& resolvedMesh == deformable->guid,
					"ResourceManager must resolve every generated runtime child by its stable package GUID.");
			}

			AssetDatabase::Instance().Shutdown();
			ResourceManager::Instance().ClearCaches();
			std::filesystem::remove_all(root);
		});

		registry.Add("Editor.Animation.SubAssetIdentity.AssetDatabaseHotReloadBatchCommitsCachesAndPublisherTogether", [](TestContext& context) {
			using namespace TerraEngine::Animation;
			using namespace TerraEngine::AssetPipeline;

			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const std::filesystem::path root = std::filesystem::temp_directory_path()
				/ "TerraEngineAnimationAssetDatabaseHotReloadTests" / TerraEngine::ToHexString(packageGuid);
			ScopedAnimationAssetDatabaseFixture cleanup(root);
			const std::filesystem::path assetRoot = root / "Assets";
			const std::filesystem::path buildRoot = root / "Build" / "Assets";
			const std::filesystem::path sourceRelativePath = "Models/Character.fbx";
			const std::filesystem::path sourcePath = assetRoot / sourceRelativePath;
			std::filesystem::create_directories(sourcePath.parent_path());
			std::filesystem::create_directories(buildRoot);
			std::filesystem::copy_file(
				AssimpFbxFixturePath("animation_with_skeleton.fbx"), sourcePath,
				std::filesystem::copy_options::overwrite_existing);

			ModelPackageImportOptions options{};
			options.sourcePath = sourcePath;
			options.assetRoot = assetRoot;
			options.buildRoot = buildRoot;
			options.sourceRelativePath = sourceRelativePath;
			options.packageGuid = packageGuid;
			options.forceReimport = true;

			const auto findChild = [](const ModelPackageImportResult& package,
				AnimationImportSubAssetKind kind) {
				return std::find_if(package.animationManifest.subAssets.begin(),
					package.animationManifest.subAssets.end(), [&](const GeneratedAnimationSubAsset& child) {
						return child.kind == kind;
					});
			};

			const ModelPackageImportResult generationOne = ImportModelPackage(options);
			ExpectAnimationSubassetsArePublished(context, generationOne,
				"The coordinated hot-reload fixture's first generation");
			const auto skeletonOne = findChild(generationOne, AnimationImportSubAssetKind::Skeleton);
			const auto clipOne = findChild(generationOne, AnimationImportSubAssetKind::AnimationClip);
			context.Expect(skeletonOne != generationOne.animationManifest.subAssets.end()
				&& clipOne != generationOne.animationManifest.subAssets.end()
				&& SaveModelPackageImportResult(sourcePath, generationOne),
				"Generation one must publish stable skeleton and clip children plus its source sidecar.");
			if(skeletonOne == generationOne.animationManifest.subAssets.end()
				|| clipOne == generationOne.animationManifest.subAssets.end())
			{
				return;
			}
			WritePackageMetadata(assetRoot, sourceRelativePath, packageGuid);

			AssetDatabase& database = AssetDatabase::Instance();
			ResourceManager& resources = ResourceManager::Instance();
			const std::filesystem::path skeletonPath = buildRoot / skeletonOne->compiledPath;
			const std::filesystem::path clipPath = buildRoot / clipOne->compiledPath;
			const std::size_t retiredPayloadBaseline =
				resources.GetRetiredAnimationPayloadCountForDiagnostics();
			context.Expect(resources.LoadSkeleton(skeletonPath, skeletonOne->compatibilityFingerprint)
					== skeletonOne->guid
				&& resources.LoadAnimationClip(clipPath, clipOne->compatibilityFingerprint)
					== clipOne->guid,
				"Generation one must be resident before its first publisher record so key-zero retirement is exercised.");
			database.Initialize(root);
			context.Expect(PumpAssetDatabaseUntilPackageVisible(packageGuid),
				"AssetDatabase startup must publish generation one before a live reload is attempted.");
			context.Expect(resources.GetRetiredAnimationPayloadCountForDiagnostics()
					== retiredPayloadBaseline,
				"First publication must reclaim displaced cache payloads that have no prior publisher generation.");

			SkeletonAsset cachedSkeletonOne;
			AnimationClipAsset cachedClipOne;
			AnimationGeneration skeletonCacheGenerationOne = 0U;
			AnimationGeneration clipCacheGenerationOne = 0U;
			const auto skeletonPublisherOne = database.GetAnimationReloadGenerationForTests(skeletonOne->guid);
			const auto clipPublisherOne = database.GetAnimationReloadGenerationForTests(clipOne->guid);
			context.Expect(resources.CopySkeletonPublication(
					skeletonOne->guid, cachedSkeletonOne, skeletonCacheGenerationOne)
				&& resources.CopyAnimationClipPublication(
					clipOne->guid, cachedClipOne, clipCacheGenerationOne)
				&& skeletonPublisherOne.has_value() && clipPublisherOne.has_value(),
				"Generation one must expose matched payload/generation cache snapshots and publisher snapshots.");
			if(!skeletonPublisherOne || !clipPublisherOne)
			{
				return;
			}
			context.Expect(skeletonPublisherOne->generation == generationOne.animationManifest.publishedGeneration
				&& clipPublisherOne->generation == generationOne.animationManifest.publishedGeneration
				&& skeletonPublisherOne->publicationBoundary == clipPublisherOne->publicationBoundary,
				"Both generation-one publisher records must originate from the same protected package boundary.");

			const ModelPackageImportResult generationTwo = ImportModelPackage(options);
			context.Expect(generationTwo.status != ModelImportStatus::Failed
				&& generationTwo.animationManifest.publishedGeneration
					== generationOne.animationManifest.publishedGeneration + 1U
				&& SaveModelPackageImportResult(sourcePath, generationTwo),
				"A valid second package generation must be ready for synchronous main-thread registration.");
			database.ImportAsset(sourcePath, false);

			SkeletonAsset cachedSkeletonTwo;
			AnimationClipAsset cachedClipTwo;
			AnimationGeneration skeletonCacheGenerationTwo = 0U;
			AnimationGeneration clipCacheGenerationTwo = 0U;
			const auto skeletonPublisherTwo = database.GetAnimationReloadGenerationForTests(skeletonOne->guid);
			const auto clipPublisherTwo = database.GetAnimationReloadGenerationForTests(clipOne->guid);
			context.Expect(resources.CopySkeletonPublication(
					skeletonOne->guid, cachedSkeletonTwo, skeletonCacheGenerationTwo)
				&& resources.CopyAnimationClipPublication(
					clipOne->guid, cachedClipTwo, clipCacheGenerationTwo)
				&& skeletonPublisherTwo.has_value() && clipPublisherTwo.has_value(),
				"A successful generation-two reload must leave both typed caches readable.");
			if(!skeletonPublisherTwo || !clipPublisherTwo)
			{
				return;
			}
			context.Expect(skeletonCacheGenerationTwo == skeletonCacheGenerationOne + 1U
				&& clipCacheGenerationTwo == clipCacheGenerationOne + 1U
				&& cachedSkeletonTwo.runtimeGeneration == skeletonCacheGenerationTwo
				&& cachedClipTwo.runtimeGeneration == clipCacheGenerationTwo
				&& skeletonPublisherTwo->generation == generationTwo.animationManifest.publishedGeneration
				&& clipPublisherTwo->generation == generationTwo.animationManifest.publishedGeneration
				&& skeletonPublisherTwo->publicationBoundary == clipPublisherTwo->publicationBoundary
				&& skeletonPublisherTwo->publicationBoundary > skeletonPublisherOne->publicationBoundary,
				"Generation two must advance both caches and both publisher views at one protected boundary.");
			context.Expect(resources.GetRetiredAnimationPayloadCountForDiagnostics()
					== retiredPayloadBaseline + 2U,
				"The two displaced typed payloads must remain owned until their publisher generation retires.");
			database.Update();
			context.Expect(resources.GetRetiredAnimationPayloadCountForDiagnostics()
					== retiredPayloadBaseline,
				"The completed publisher boundary must reclaim cache payloads even when cache and publisher generation counters differ.");

			const ModelPackageImportResult generationThree = ImportModelPackage(options);
			context.Expect(generationThree.status != ModelImportStatus::Failed
				&& generationThree.animationManifest.publishedGeneration
					== generationTwo.animationManifest.publishedGeneration + 1U
				&& SaveModelPackageImportResult(sourcePath, generationThree),
				"A valid third generation must exist before the deterministic staging race corrupts one child.");
			const std::vector<std::uint8_t> validGenerationThreeClip = ReadFileBytes(clipPath);
			context.Expect(!validGenerationThreeClip.empty(),
				"The generation-three clip must be captured exactly so the same candidate can be retried.");

			bool corruptionHookRan = false;
			database.SetAnimationPackagePostPublisherPrepareHookForTests([&]() {
				// Publisher preparation has succeeded, but ResourceManager has not yet
				// parsed replacements. Truncating only the disposable clip forces the
				// real cross-cache staging path to reject the whole package atomically.
				corruptionHookRan = true;
				std::ofstream corruptClip(clipPath, std::ios::binary | std::ios::trunc);
				corruptClip << "truncated animation candidate";
			});
			database.ImportAsset(sourcePath, false);
			database.SetAnimationPackagePostPublisherPrepareHookForTests({});

			SkeletonAsset skeletonAfterRejectedThree;
			AnimationClipAsset clipAfterRejectedThree;
			AnimationGeneration skeletonGenerationAfterRejectedThree = 0U;
			AnimationGeneration clipGenerationAfterRejectedThree = 0U;
			const auto skeletonPublisherAfterRejectedThree =
				database.GetAnimationReloadGenerationForTests(skeletonOne->guid);
			const auto clipPublisherAfterRejectedThree =
				database.GetAnimationReloadGenerationForTests(clipOne->guid);
			context.Expect(corruptionHookRan
				&& resources.CopySkeletonPublication(skeletonOne->guid,
					skeletonAfterRejectedThree, skeletonGenerationAfterRejectedThree)
				&& resources.CopyAnimationClipPublication(clipOne->guid,
					clipAfterRejectedThree, clipGenerationAfterRejectedThree)
				&& skeletonGenerationAfterRejectedThree == skeletonCacheGenerationTwo
				&& clipGenerationAfterRejectedThree == clipCacheGenerationTwo
				&& skeletonAfterRejectedThree == cachedSkeletonTwo
				&& clipAfterRejectedThree == cachedClipTwo,
				"A corrupt clip staged after publisher preparation must preserve both generation-two cache payloads exactly.");
			context.Expect(skeletonPublisherAfterRejectedThree.has_value()
				&& clipPublisherAfterRejectedThree.has_value()
				&& skeletonPublisherAfterRejectedThree->generation == skeletonPublisherTwo->generation
				&& clipPublisherAfterRejectedThree->generation == clipPublisherTwo->generation
				&& skeletonPublisherAfterRejectedThree->publicationBoundary
					== skeletonPublisherTwo->publicationBoundary
				&& clipPublisherAfterRejectedThree->publicationBoundary
					== clipPublisherTwo->publicationBoundary
				&& skeletonPublisherAfterRejectedThree->contentFingerprint
					== skeletonPublisherTwo->contentFingerprint
				&& clipPublisherAfterRejectedThree->contentFingerprint
					== clipPublisherTwo->contentFingerprint
				&& skeletonPublisherAfterRejectedThree->contentHash
					== skeletonPublisherTwo->contentHash
				&& clipPublisherAfterRejectedThree->contentHash
					== clipPublisherTwo->contentHash,
				"Rejected cache staging must leave every publisher generation, boundary, and payload fingerprint unchanged.");

			const std::vector<AnimationReimportStatus> rejectedStatuses =
				database.GetAnimationReimportStatusSnapshot();
			for(const GeneratedAnimationSubAsset& child : generationThree.animationManifest.subAssets)
			{
				if(child.kind != AnimationImportSubAssetKind::Skeleton
					&& child.kind != AnimationImportSubAssetKind::AnimationClip)
				{
					continue;
				}
				const auto status = std::find_if(rejectedStatuses.rbegin(), rejectedStatuses.rend(),
					[&](const AnimationReimportStatus& candidate) {
						return candidate.assetId == child.guid
							&& candidate.candidateGeneration
								== generationThree.animationManifest.publishedGeneration;
					});
				context.Expect(status != rejectedStatuses.rend()
					&& status->state == AnimationReimportState::RetainedLastKnownGood
					&& status->lastKnownGoodPreserved
					&& status->activeGeneration == generationTwo.animationManifest.publishedGeneration,
					"Every animation child in a rejected package batch must report the same retained last-known-good generation.");
			}

			WriteFileBytes(clipPath, validGenerationThreeClip);
			context.Expect(ReadFileBytes(clipPath) == validGenerationThreeClip,
				"The exact valid generation-three clip must be restored before retrying its unchanged generation number.");
			database.ImportAsset(sourcePath, false);

			SkeletonAsset cachedSkeletonThree;
			AnimationClipAsset cachedClipThree;
			AnimationGeneration skeletonCacheGenerationThree = 0U;
			AnimationGeneration clipCacheGenerationThree = 0U;
			const auto skeletonPublisherThree = database.GetAnimationReloadGenerationForTests(skeletonOne->guid);
			const auto clipPublisherThree = database.GetAnimationReloadGenerationForTests(clipOne->guid);
			context.Expect(resources.CopySkeletonPublication(
					skeletonOne->guid, cachedSkeletonThree, skeletonCacheGenerationThree)
				&& resources.CopyAnimationClipPublication(
					clipOne->guid, cachedClipThree, clipCacheGenerationThree)
				&& skeletonPublisherThree.has_value() && clipPublisherThree.has_value()
				&& skeletonCacheGenerationThree == skeletonCacheGenerationTwo + 1U
				&& clipCacheGenerationThree == clipCacheGenerationTwo + 1U
				&& cachedSkeletonThree.runtimeGeneration == skeletonCacheGenerationThree
				&& cachedClipThree.runtimeGeneration == clipCacheGenerationThree
				&& skeletonPublisherThree->generation == generationThree.animationManifest.publishedGeneration
				&& clipPublisherThree->generation == generationThree.animationManifest.publishedGeneration
				&& skeletonPublisherThree->publicationBoundary == clipPublisherThree->publicationBoundary
				&& skeletonPublisherThree->publicationBoundary > skeletonPublisherTwo->publicationBoundary,
				"Retrying the restored generation three must advance both caches and both publishers exactly once together.");
			context.Expect(resources.GetRetiredAnimationPayloadCountForDiagnostics()
					== retiredPayloadBaseline + 2U,
				"Generation three must retain the two displaced generation-two cache payloads before the next update boundary.");
			database.Shutdown();
			context.Expect(resources.GetRetiredAnimationPayloadCountForDiagnostics()
					== retiredPayloadBaseline,
				"Protected AssetDatabase shutdown must reclaim pending cache payloads before publisher retirement records reset.");
		});

		registry.Add("Editor.Animation.SubAssetIdentity.DeepWindowsProjectUsesCompactPrivatePublicationPaths", [](TestContext& context) {
			using namespace TerraEngine::AssetPipeline;
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Tests/Fixtures/Animation/US1Import/Assets/Models/US1Hero.gltf";
			const std::filesystem::path testRoot = std::filesystem::temp_directory_path()
				/ "TerraEngineAnimationDeepPathTests" / TerraEngine::ToHexString(TerraEngine::GenerateUUID());
			std::filesystem::remove_all(testRoot);

			// First discover the fixture's real generated names. The padded project is
			// then sized so legacy private names cross MAX_PATH while public and compact
			// names retain useful headroom below it.
			ModelPackageImportOptions probeOptions{};
			probeOptions.sourcePath = sourcePath;
			probeOptions.sourceRelativePath = "Models/US1Hero.gltf";
			probeOptions.assetRoot = testRoot / "Probe/Assets";
			probeOptions.buildRoot = testRoot / "Probe/Build";
			probeOptions.packageGuid = TerraEngine::GenerateUUID();
			probeOptions.forceReimport = true;
			const ModelPackageImportResult probe = ImportModelPackage(probeOptions);
			const auto deformable = std::find_if(probe.animationManifest.subAssets.begin(), probe.animationManifest.subAssets.end(),
				[](const GeneratedAnimationSubAsset& child) { return child.kind == AnimationImportSubAssetKind::DeformableMesh; });
			const auto skeleton = std::find_if(probe.animationManifest.subAssets.begin(), probe.animationManifest.subAssets.end(),
				[](const GeneratedAnimationSubAsset& child) { return child.kind == AnimationImportSubAssetKind::Skeleton; });
			context.Expect(probe.status != ModelImportStatus::Failed
				&& deformable != probe.animationManifest.subAssets.end()
				&& skeleton != probe.animationManifest.subAssets.end(),
				"The committed animated glTF fixture must expose deformable and skeleton paths before the deep-path boundary is exercised.");
			if(probe.status == ModelImportStatus::Failed
				|| deformable == probe.animationManifest.subAssets.end()
				|| skeleton == probe.animationManifest.subAssets.end()) {
				std::filesystem::remove_all(testRoot);
				return;
			}

			const std::filesystem::path baseRoot = testRoot / "Deep";
			const std::size_t baseMeshLength = (baseRoot / "Build" / deformable->compiledPath).string().size();
			const std::size_t baseSkeletonLength = (baseRoot / "Build" / skeleton->compiledPath).string().size();
			// Adding the padded directory contributes one separator plus its bytes.
			// The package dispatcher adds `.tb-<16>` before these importer-private
			// names. Derive a range that makes both legacy forms at least 260 while
			// keeping the longest compact deformable cook at most 259.
			const std::size_t minimumPadding = std::max({
				std::size_t{1U}, baseMeshLength < 188U ? 188U - baseMeshLength : 1U,
				baseSkeletonLength < 197U ? 197U - baseSkeletonLength : 1U});
			const std::size_t maximumMeshPadding = baseMeshLength <= 198U ? 198U - baseMeshLength : 0U;
			const std::size_t maximumSkeletonPadding = baseSkeletonLength <= 218U ? 218U - baseSkeletonLength : 0U;
			const std::size_t maximumPadding = std::min(maximumMeshPadding, maximumSkeletonPadding);
			context.Expect(minimumPadding <= maximumPadding,
				"The disposable base path must leave a nonempty legacy-fails/compact-passes padding range.");
			if(minimumPadding > maximumPadding) {
				std::filesystem::remove_all(testRoot);
				return;
			}
			const std::size_t paddingLength = std::min(minimumPadding + 4U, maximumPadding);
			const std::string padding(paddingLength, 'p');
			const std::filesystem::path deepRoot = baseRoot / padding;
			ModelPackageImportOptions options = probeOptions;
			options.assetRoot = deepRoot / "Assets";
			options.buildRoot = deepRoot / "Build";
			options.packageGuid = TerraEngine::GenerateUUID();

			const std::filesystem::path finalMeshPath = options.buildRoot / deformable->compiledPath;
			const std::filesystem::path finalSkeletonPath = options.buildRoot / skeleton->compiledPath;
			const std::filesystem::path legacyMeshCandidate = finalMeshPath.string()
				+ ".static-candidate-" + std::string(32U, 'f');
			const std::filesystem::path compactMeshCandidate = finalMeshPath.string()
				+ ".sc-" + std::string(16U, 'f');
			const std::filesystem::path compactDeformableStage = finalMeshPath.string()
				+ ".dc-" + std::string(16U, 'f') + ".s-" + std::string(16U, 'f');
			const std::filesystem::path legacyAnimationArtifact = finalSkeletonPath.parent_path().parent_path()
				/ (finalSkeletonPath.parent_path().filename().string() + ".staging-" + std::string(32U, 'f'))
				/ finalSkeletonPath.filename();
			const std::filesystem::path compactAnimationArtifact = finalSkeletonPath.parent_path().parent_path()
				/ (finalSkeletonPath.parent_path().filename().string() + ".s-" + std::string(16U, 'f'))
				/ finalSkeletonPath.filename();
			// Add 21 characters for the outer `/.tb-<16>` package transaction
			// component, which is absent from the eventual public path.
			context.Expect(legacyMeshCandidate.string().size() + 21U >= 260U
				&& legacyAnimationArtifact.string().size() + 21U >= 260U
				&& compactMeshCandidate.string().size() + 21U < 260U
				&& compactDeformableStage.string().size() + 21U < 260U
				&& compactAnimationArtifact.string().size() + 21U < 260U,
				"The fixture must distinguish the rejected legacy names from the compact private publication names on Windows.");

			const ModelPackageImportResult first = ImportModelPackage(options);
			const ModelPackageImportResult second = ImportModelPackage(options);
			context.Expect(first.status != ModelImportStatus::Failed && second.status != ModelImportStatus::Failed,
				"A deeply nested animated glTF must publish and reimport through compact private paths.");
			context.Expect(std::filesystem::exists(finalMeshPath) && std::filesystem::exists(finalSkeletonPath)
				&& !HasDiagnostic(second, "MeshCompileFailed")
				&& !HasDiagnostic(second, "AnimationArtifactWriteFailed"),
				"Both deformable and animation artifacts must reach their public paths without a path-length fallback.");
			context.Expect(!HasCompactTransactionStage(options.assetRoot, ".ta-")
				&& !HasCompactTransactionStage(options.buildRoot, ".tb-"),
				"Successful reimport must remove both private package transaction roots.");
			std::filesystem::remove_all(testRoot);
		});

		registry.Add("Editor.Animation.SubAssetIdentity.DeepProjectPublishesLongManifestAtomically", [](TestContext& context) {
			using namespace TerraEngine::AssetPipeline;
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const std::filesystem::path testRoot = std::filesystem::temp_directory_path()
				/ "TerraEngineAnimationLongManifestTests" / TerraEngine::ToHexString(packageGuid);
			std::error_code cleanupError;
			std::filesystem::remove_all(NativeIoPath(testRoot), cleanupError);

			// Grow the project through ordinary bounded directory components until
			// the public manifest itself is well beyond MAX_PATH. This adapts to the
			// host's temporary-directory prefix while preserving a deterministic
			// minimum path length on every test machine.
			std::filesystem::path deepProjectRoot = testRoot / "Project";
			const auto manifestForRoot = [&](const std::filesystem::path& root) {
				return root / "Build/.terra-animation-manifests"
					/ (TerraEngine::ToHexString(packageGuid) + ".json");
			};
			while(manifestForRoot(deepProjectRoot).native().size() < 300U)
			{
				deepProjectRoot /= "nested-long-path-segment";
			}

			ModelPackageImportOptions options{};
			options.sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Tests/Fixtures/Animation/US1Import/Assets/Models/US1Hero.gltf";
			options.sourceRelativePath = "Models/US1Hero.gltf";
			options.assetRoot = deepProjectRoot / "Assets";
			options.buildRoot = deepProjectRoot / "Build";
			options.packageGuid = packageGuid;
			options.forceReimport = true;
			const std::filesystem::path manifestPath = manifestForRoot(deepProjectRoot);
			context.Expect(manifestPath.native().size() >= 300U,
				"The regression fixture must exercise a public manifest path beyond the legacy Windows limit.");

			const ModelPackageImportResult first = ImportModelPackage(options);
			const ModelPackageImportResult second = ImportModelPackage(options);
			const std::vector<std::uint8_t> secondManifestBytes = ReadFileBytes(manifestPath);
			const nlohmann::json manifestDocument = nlohmann::json::parse(
				secondManifestBytes.begin(), secondManifestBytes.end(), nullptr, false);
			context.Expect(first.status != ModelImportStatus::Failed
				&& second.status != ModelImportStatus::Failed
				&& second.animationManifest.publishedGeneration
					== first.animationManifest.publishedGeneration + 1U
				&& !secondManifestBytes.empty()
				&& manifestDocument.is_object()
				&& manifestDocument.value("publishedGeneration", std::uint64_t{0U})
					== second.animationManifest.publishedGeneration,
				"A deep first import and reimport must publish the complete generation marker at its logical path.");

			bool allPublishedChildrenExist = !second.animationManifest.subAssets.empty();
			std::size_t checkedChildPaths = 0U;
			for(const GeneratedAnimationSubAsset& child : second.animationManifest.subAssets)
			{
				if(!child.compiledPath.empty())
				{
					++checkedChildPaths;
					allPublishedChildrenExist = allPublishedChildrenExist
						&& std::filesystem::is_regular_file(NativeIoPath(options.buildRoot / child.compiledPath));
				}
				else if(!child.sourcePath.empty())
				{
					// Source-only children such as generated prefabs publish below
					// Assets. Cooked children may retain the original package path
					// for provenance, so their compiled payload is authoritative.
					++checkedChildPaths;
					allPublishedChildrenExist = allPublishedChildrenExist
						&& std::filesystem::is_regular_file(NativeIoPath(options.assetRoot / child.sourcePath));
				}
			}
			context.Expect(checkedChildPaths > 0U && allPublishedChildrenExist,
				"Every path advertised by the long-path manifest must be fully published before the marker becomes visible.");

			bool observedCompleteCandidate = false;
			ModelPackageImportOptions cancelledOptions = options;
			cancelledOptions.cancellationRequested = [&]() {
				// Cancellation becomes true only after the next complete manifest is
				// present in private staging, exercising the last protected boundary.
				observedCompleteCandidate = observedCompleteCandidate
					|| HasStagedAnimationManifest(options.buildRoot, packageGuid);
				return observedCompleteCandidate;
			};
			const ModelPackageImportResult cancelled = ImportModelPackage(cancelledOptions);
			context.Expect(observedCompleteCandidate
				&& cancelled.status == ModelImportStatus::Failed
				&& cancelled.animationManifest.cancelled
				&& cancelled.animationManifest.preservedLastKnownGood
				&& cancelled.animationManifest.lastKnownGoodGeneration
					== second.animationManifest.publishedGeneration,
				"A deep candidate cancelled immediately before publication must preserve the last complete generation.");
			context.Expect(ReadFileBytes(manifestPath) == secondManifestBytes
				&& !HasCompactTransactionStage(options.assetRoot, ".ta-")
				&& !HasCompactTransactionStage(options.buildRoot, ".tb-"),
				"Long-path cancellation must leave the live manifest byte-identical and remove every private transaction root.");

			cleanupError.clear();
			std::filesystem::remove_all(NativeIoPath(testRoot), cleanupError);
			context.Expect(!cleanupError && !std::filesystem::exists(NativeIoPath(testRoot)),
				"The deep regression workspace must remain fully removable after success and rollback.");
		});

		registry.Add("Editor.Animation.SubAssetIdentity.AssetDatabaseIgnoresPrivatePackageTransactions", [](TestContext& context) {
			AssetDatabase::Instance().Shutdown();
			const std::filesystem::path root = std::filesystem::temp_directory_path()
				/ "TerraEngineAnimationPrivateTransactionTests" / TerraEngine::ToHexString(TerraEngine::GenerateUUID());
			const std::filesystem::path privateSource = root / "Assets/.ta-0123456789abcdef/Models/Hero_Generated/Private.tmat";
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(privateSource.parent_path());
			std::ofstream(privateSource, std::ios::binary | std::ios::trunc) << "{}";

			AssetDatabase::Instance().Initialize(root);
			AssetDatabase::Instance().ImportAsset(privateSource, true);
			AssetDatabase::Instance().Update();
			const bool registered = AssetDatabase::Instance().GetAssets().contains(std::filesystem::absolute(privateSource));
			context.Expect(!registered
				&& !std::filesystem::exists(privateSource.string() + ".meta")
				&& !std::filesystem::exists(root / "Build/Assets/.ta-0123456789abcdef"),
				"Startup scanning and explicit import must leave package-private transaction files invisible and uncompiled.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

	}
}
