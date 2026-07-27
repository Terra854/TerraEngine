// ModelPackageImporter.cpp - Shared detection and dispatch for model packages.
//
// This file is deliberately small: it centralizes extension/importer-name policy
// while the format-specific files own parsing details and diagnostics.

#include "ModelPackageImporter.h"

#include "FbxPackageImporter.h"
#include "GltfPackageImporter.h"

#include "Animation/AnimationTypes.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		/// Returns a native I/O spelling that bypasses the legacy MAX_PATH parser
		/// without changing the logical path stored in manifests or diagnostics.
		/// Windows extended-length paths also keep this library safe when a tool
		/// hosts it without declaring longPathAware in its application manifest.
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

		struct StagedPublicationFile
		{
			std::filesystem::path stagedPath;
			std::filesystem::path livePath;
			std::filesystem::path backupPath;
			bool hadPrevious = false;
			bool published = false;
		};

		bool ImportCancellationRequested(const ModelPackageImportOptions& options)
		{
			return options.cancellationRequested && options.cancellationRequested();
		}

		std::string BuildPublicationToken(const ModelPackageImportOptions& options)
		{
			static std::atomic<std::uint64_t> sequence{0u};
			const std::uint64_t ordinal = sequence.fetch_add(1u, std::memory_order_relaxed);
			std::uint64_t token = static_cast<std::uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			// Mix the package bytes and a process-local ordinal into one compact token.
			// Staging folders are internal and short names avoid MAX_PATH exhaustion
			// while still separating concurrent imports of the same source package.
			for(const std::uint8_t byte : options.packageGuid)
			{
				token ^= byte;
				token *= 1099511628211ull;
			}
			token ^= ordinal + 0x9e3779b97f4a7c15ull + (token << 6u) + (token >> 2u);
			std::ostringstream stream;
			stream << std::hex << std::setfill('0') << std::setw(16) << token;
			return stream.str();
		}

		std::filesystem::path ManifestRelativePath(const TerraEngine::UUID& packageGuid)
		{
			return std::filesystem::path(".terra-animation-manifests")
				/ (TerraEngine::ToHexString(packageGuid) + ".json");
		}

		std::uint64_t ReadPriorPublishedGeneration(
			const ModelPackageImportOptions& options,
			bool& hasLastKnownGood)
		{
			hasLastKnownGood = false;
			std::ifstream input(NativeIoPath(options.buildRoot / ManifestRelativePath(options.packageGuid)));
			if(!input) return 0u;
			try
			{
				nlohmann::json document;
				input >> document;
				hasLastKnownGood = true;
				return document.value("publishedGeneration", std::uint64_t{0u});
			}
			catch(const std::exception&)
			{
				// An unreadable marker is not trusted as a last-known-good generation.
				return 0u;
			}
		}

		std::string HashFileHex(const std::filesystem::path& path)
		{
			std::ifstream input(NativeIoPath(path), std::ios::binary);
			if(!input) return {};
			std::uint64_t hash = 1469598103934665603ull;
			char buffer[16384];
			while(input)
			{
				input.read(buffer, sizeof(buffer));
				for(std::streamsize index = 0; index < input.gcount(); ++index)
				{
					hash ^= static_cast<std::uint8_t>(buffer[index]);
					hash *= 1099511628211ull;
				}
			}
			std::ostringstream stream;
			stream << std::hex << std::setfill('0') << std::setw(16) << hash;
			return stream.str();
		}

		std::string HashTextHex(std::string_view text)
		{
			std::uint64_t hash = 1469598103934665603ull;
			for(const char character : text)
			{
				hash ^= static_cast<std::uint8_t>(character);
				hash *= 1099511628211ull;
			}
			std::ostringstream stream;
			stream << std::hex << std::setfill('0') << std::setw(16) << hash;
			return stream.str();
		}

		void AddManifestSubAssetIfMissing(
			AnimationImportManifest& manifest,
			GeneratedAnimationSubAsset subAsset)
		{
			const bool exists = std::any_of(manifest.subAssets.begin(), manifest.subAssets.end(), [&](const auto& current) {
				return current.guid == subAsset.guid
					|| (current.kind == subAsset.kind && current.semanticKey == subAsset.semanticKey);
			});
			if(!exists) manifest.subAssets.push_back(std::move(subAsset));
		}

		bool IsPrefabAnimationBindingKind(AnimationImportSubAssetKind kind)
		{
			return kind == AnimationImportSubAssetKind::Skeleton
				|| kind == AnimationImportSubAssetKind::AnimationClip
				|| kind == AnimationImportSubAssetKind::DeformableMesh
				|| kind == AnimationImportSubAssetKind::MorphTarget;
		}

		void AddRequiredManifestDependency(
			GeneratedAnimationSubAsset& owner,
			const TerraEngine::UUID& dependencyGuid,
			std::uint64_t compatibilityFingerprint)
		{
			if(dependencyGuid == TerraEngine::EMPTY_UUID) return;
			const auto existing = std::find_if(owner.dependencies.begin(), owner.dependencies.end(), [&](const auto& dependency) {
				return dependency.assetGuid == dependencyGuid;
			});
			if(existing != owner.dependencies.end())
			{
				// A format-neutral mesh edge may already exist with no fingerprint.
				// Upgrade it when the typed animation child carries stronger evidence.
				existing->compatibilityFingerprint = compatibilityFingerprint;
				existing->required = true;
				return;
			}
			owner.dependencies.push_back({dependencyGuid, compatibilityFingerprint, true});
		}

		void AppendFormatNeutralManifestChildren(ModelPackageImportResult& result)
		{
			for(const GeneratedModelMeshAsset& mesh : result.generatedMeshes)
			{
				const bool hasFormatSpecificChild = std::any_of(
					result.animationManifest.subAssets.begin(), result.animationManifest.subAssets.end(),
					[&](const GeneratedAnimationSubAsset& existing) { return existing.guid == mesh.guid; });
				if(hasFormatSpecificChild) continue;
				GeneratedAnimationSubAsset child;
				child.guid = mesh.guid;
				child.kind = AnimationImportSubAssetKind::Mesh;
				child.semanticKey = mesh.sourceNodePath.empty()
					? "mesh/" + std::to_string(mesh.sourceMeshIndex)
					: "mesh/" + mesh.sourceNodePath;
				child.displayName = mesh.displayName;
				child.sourcePath = mesh.sourcePath;
				child.compiledPath = mesh.compiledPath;
				AddManifestSubAssetIfMissing(result.animationManifest, std::move(child));
			}
			for(const GeneratedModelMaterialAsset& material : result.generatedMaterials)
			{
				GeneratedAnimationSubAsset child;
				child.guid = material.guid;
				child.kind = AnimationImportSubAssetKind::Material;
				child.semanticKey = "material/" + std::to_string(material.sourceMaterialIndex.value_or(0u));
				child.displayName = material.displayName;
				child.sourcePath = material.sourcePath;
				child.compiledPath = material.compiledPath;
				AddManifestSubAssetIfMissing(result.animationManifest, std::move(child));
			}
			for(const GeneratedModelTextureAsset& texture : result.generatedTextures)
			{
				GeneratedAnimationSubAsset child;
				child.guid = texture.guid;
				child.kind = AnimationImportSubAssetKind::Texture;
				child.semanticKey = "texture/" + std::to_string(texture.sourceTextureIndex.value_or(
					texture.sourceImageIndex.value_or(0u)));
				child.displayName = texture.displayName;
				child.sourcePath = texture.sourcePath;
				child.compiledPath = texture.compiledPath;
				AddManifestSubAssetIfMissing(result.animationManifest, std::move(child));
			}
			for(const GeneratedModelPrefabAsset& prefab : result.generatedPrefabs)
			{
				GeneratedAnimationSubAsset child;
				child.guid = prefab.guid;
				child.kind = AnimationImportSubAssetKind::Prefab;
				child.semanticKey = "prefab/package-root";
				child.displayName = prefab.displayName;
				child.sourcePath = prefab.sourcePath;
				for(const TerraEngine::UUID& meshGuid : prefab.meshGuids)
				{
					AddRequiredManifestDependency(child, meshGuid, 0u);
				}
				for(const TerraEngine::UUID& materialGuid : prefab.materialGuids)
				{
					AddRequiredManifestDependency(child, materialGuid, 0u);
				}
				for(const GeneratedAnimationSubAsset& binding : result.animationManifest.subAssets)
				{
					if(!IsPrefabAnimationBindingKind(binding.kind)) continue;
					AddRequiredManifestDependency(child, binding.guid, binding.compatibilityFingerprint);
				}
				AddManifestSubAssetIfMissing(result.animationManifest, std::move(child));
			}
		}

		bool ValidateManifestIdentity(const AnimationImportManifest& manifest)
		{
			if(manifest.packageGuid == TerraEngine::EMPTY_UUID || manifest.sourceHashHex.empty()
				|| manifest.importerName.empty()) return false;
			std::set<TerraEngine::UUID> guids;
			std::set<std::pair<AnimationImportSubAssetKind, std::string>> semanticKeys;
			for(const GeneratedAnimationSubAsset& subAsset : manifest.subAssets)
			{
				if(subAsset.guid == TerraEngine::EMPTY_UUID || subAsset.semanticKey.empty()
					|| !guids.insert(subAsset.guid).second
					|| !semanticKeys.emplace(subAsset.kind, subAsset.semanticKey).second)
				{
					return false;
				}
			}
			return true;
		}

		void FinalizeManifest(
			const ModelPackageImportOptions& liveOptions,
			const ModelPackageImportOptions& stagedOptions,
			std::uint64_t priorGeneration,
			ModelPackageImportResult& result)
		{
			AnimationImportManifest& manifest = result.animationManifest;
			manifest.packageGuid = result.packageGuid;
			manifest.sourceHashHex = result.package.sourceHashHex;
			manifest.importerName = GetModelPackageImporterName(liveOptions.sourcePath);
			manifest.importerVersion = GetModelPackageImporterVersion(liveOptions.sourcePath);
			manifest.settingsFingerprintHex = HashTextHex(ToJson(liveOptions.virtualGeometrySettings).dump());
			manifest.lastKnownGoodGeneration = priorGeneration;
			manifest.candidateGeneration = priorGeneration + 1u;
			manifest.publishedGeneration = manifest.candidateGeneration;
			manifest.cancelled = false;
			manifest.publishedAtomically = true;
			manifest.preservedLastKnownGood = false;
			AppendFormatNeutralManifestChildren(result);
			for(GeneratedAnimationSubAsset& subAsset : manifest.subAssets)
			{
				const std::filesystem::path compiled = stagedOptions.buildRoot / subAsset.compiledPath;
				const std::filesystem::path source = stagedOptions.assetRoot / subAsset.sourcePath;
				subAsset.contentHashHex = std::filesystem::is_regular_file(NativeIoPath(compiled))
					? HashFileHex(compiled)
					: std::filesystem::is_regular_file(NativeIoPath(source)) ? HashFileHex(source) : subAsset.contentHashHex;
			}
			CanonicalizeAnimationImportManifest(manifest);
			manifest.dependencyFingerprintHex = BuildAnimationImportDependencyFingerprint(manifest);
		}

		void CollectStagedFiles(
			const std::filesystem::path& stagedRoot,
			const std::filesystem::path& liveRoot,
			std::string_view token,
			std::vector<StagedPublicationFile>& files)
		{
			std::error_code error;
			const std::filesystem::path nativeStagedRoot = NativeIoPath(stagedRoot);
			if(!std::filesystem::exists(nativeStagedRoot, error)) return;
			for(std::filesystem::recursive_directory_iterator iterator(nativeStagedRoot, error), end;
				iterator != end && !error; iterator.increment(error))
			{
				if(!iterator->is_regular_file(error)) continue;
				const std::filesystem::path relative = std::filesystem::relative(iterator->path(), nativeStagedRoot, error);
				if(error) break;
				StagedPublicationFile file;
				// Keep publication bookkeeping prefix-free. NativeIoPath is applied
				// only while touching disk so manifest paths remain portable.
				file.stagedPath = stagedRoot / relative;
				file.livePath = liveRoot / relative;
				// Keep the sibling backup name compact because generated clip names can
				// already approach the legacy Windows path limit during atomic publication.
				file.backupPath = file.livePath.string() + ".ap-" + std::string(token);
				files.push_back(std::move(file));
			}
		}

		bool PublishStagedFiles(std::vector<StagedPublicationFile>& files)
		{
			std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
				// The generation manifest is the visibility boundary. Publishing it
				// after every asset/build payload prevents filesystem observers from
				// treating a still-incomplete candidate as the current generation.
				const bool leftIsManifest = left.livePath.parent_path().filename() == ".terra-animation-manifests";
				const bool rightIsManifest = right.livePath.parent_path().filename() == ".terra-animation-manifests";
				if(leftIsManifest != rightIsManifest) return !leftIsManifest;
				return left.livePath.generic_string() < right.livePath.generic_string();
			});
			for(std::size_t index = 1u; index < files.size(); ++index)
			{
				if(files[index - 1u].livePath == files[index].livePath) return false;
			}
			for(std::size_t index = 0u; index < files.size(); ++index)
			{
				StagedPublicationFile& file = files[index];
				std::error_code error;
				std::filesystem::create_directories(NativeIoPath(file.livePath.parent_path()), error);
				if(error) goto rollback;
				std::filesystem::remove(NativeIoPath(file.backupPath), error);
				error.clear();
				file.hadPrevious = std::filesystem::exists(NativeIoPath(file.livePath), error);
				error.clear();
				if(file.hadPrevious)
				{
					std::filesystem::rename(NativeIoPath(file.livePath), NativeIoPath(file.backupPath), error);
					if(error) goto rollback;
				}
				std::filesystem::rename(NativeIoPath(file.stagedPath), NativeIoPath(file.livePath), error);
				if(error)
				{
					if(file.hadPrevious)
					{
						std::error_code restoreError;
						std::filesystem::rename(NativeIoPath(file.backupPath), NativeIoPath(file.livePath), restoreError);
					}
					goto rollback;
				}
				file.published = true;
				continue;

			rollback:
				for(std::size_t rollbackIndex = index + 1u; rollbackIndex-- > 0u;)
				{
					StagedPublicationFile& published = files[rollbackIndex];
					if(!published.published) continue;
					std::error_code rollbackError;
					std::filesystem::remove(NativeIoPath(published.livePath), rollbackError);
					if(published.hadPrevious)
					{
						rollbackError.clear();
						std::filesystem::rename(
							NativeIoPath(published.backupPath),
							NativeIoPath(published.livePath),
							rollbackError);
					}
					published.published = false;
				}
				return false;
			}
			for(StagedPublicationFile& file : files)
			{
				if(!file.hadPrevious) continue;
				std::error_code cleanupError;
				std::filesystem::remove(NativeIoPath(file.backupPath), cleanupError);
			}
			return true;
		}
		std::string NormalizeExtension(const std::filesystem::path& filePath)
		{
			std::string ext = filePath.extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return ext;
		}

		bool SourceTextContains(const std::filesystem::path& filePath, std::string_view token)
		{
			const std::string ext = NormalizeExtension(filePath);
			if (ext != ".gltf")
			{
				return false;
			}

			std::ifstream in(NativeIoPath(filePath));
			if (!in)
			{
				return false;
			}

			const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			return text.find(token) != std::string::npos;
		}

		bool HasDiagnosticCode(const ModelPackageImportResult& result, std::string_view code)
		{
			for (const ModelImportDiagnostic& diagnostic : result.diagnostics)
			{
				if (diagnostic.code == code)
				{
					return true;
				}
			}
			return false;
		}

		void AddDiagnosticIfSourceContains(
			const ModelPackageImportOptions& options,
			ModelPackageImportResult& result,
			std::string_view token,
			ModelDiagnosticSeverity severity,
			std::string code,
			std::string source,
			std::string message,
			std::string fallback)
		{
			if (SourceTextContains(options.sourcePath, token) && !HasDiagnosticCode(result, code))
			{
				result.diagnostics.push_back({
					severity,
					std::move(code),
					std::move(source),
					std::move(message),
					std::move(fallback)
				});
			}
		}

		void AddFormatNeutralUnsupportedDiagnostics(const ModelPackageImportOptions& options, ModelPackageImportResult& result)
		{
			// Some authoring formats expose data that is not part of TerraEngine's
			// static package import contract yet. Report that content once, but
			// keep usable static meshes available to the editor and automation.
			AddDiagnosticIfSourceContains(options, result, "\"cameras\"", ModelDiagnosticSeverity::Warning, "UnsupportedCameraIgnored", "cameras", "Camera data was detected in the source package but is not imported for static assets.", "Renderable meshes were imported.");
			AddDiagnosticIfSourceContains(options, result, "\"lights\"", ModelDiagnosticSeverity::Warning, "UnsupportedLightIgnored", "lights", "Light data was detected in the source package but is not imported for static assets.", "Renderable meshes were imported.");
			AddDiagnosticIfSourceContains(options, result, "\"constraints\"", ModelDiagnosticSeverity::Warning, "UnsupportedConstraintIgnored", "constraints", "Constraint data was detected in the source package but is not imported for static assets.", "Renderable meshes were imported.");
		}

		void PopulateMissingVirtualGeometryMetadata(const ModelPackageImportOptions& options, ModelPackageImportResult& result)
		{
			for (GeneratedModelMeshAsset& mesh : result.generatedMeshes)
			{
				if (!mesh.compiledPath.empty() && mesh.virtualGeometryStatus == "Unknown")
				{
					PopulateGeneratedMeshVirtualGeometryMetadata(options.buildRoot / mesh.compiledPath, mesh, result.diagnostics);
				}
			}
		}

		std::optional<TerraEngine::UUID> FindMaterialForMesh(const ModelPackageImportResult& result, const TerraEngine::UUID& meshGuid)
		{
			for (const ModelMaterialAssignment& assignment : result.materialAssignments)
			{
				if (assignment.meshGuid == meshGuid && assignment.materialGuid != TerraEngine::EMPTY_UUID)
				{
					return assignment.materialGuid;
				}
			}
			return std::nullopt;
		}

		nlohmann::json MakeIdentityTransformJson()
		{
			return {
				{ "Position", { 0.0f, 0.0f, 0.0f } },
				{ "Rotation", { 1.0f, 0.0f, 0.0f, 0.0f } },
				{ "Scale", { 1.0f, 1.0f, 1.0f } }
			};
		}

		struct ModelPackagePrefabAnimationBindings
		{
			TerraEngine::UUID primarySkeletonGuid = TerraEngine::EMPTY_UUID;
			TerraEngine::UUID defaultClipGuid = TerraEngine::EMPTY_UUID;
			std::vector<TerraEngine::UUID> skeletonGuids;
			std::vector<TerraEngine::UUID> clipGuids;
			std::vector<TerraEngine::UUID> deformableMeshGuids;
			std::vector<TerraEngine::UUID> morphTargetGuids;
		};

		std::vector<const GeneratedAnimationSubAsset*> FindSortedManifestChildren(
			const AnimationImportManifest& manifest,
			AnimationImportSubAssetKind kind)
		{
			std::vector<const GeneratedAnimationSubAsset*> children;
			for(const GeneratedAnimationSubAsset& child : manifest.subAssets)
			{
				if(child.kind == kind && child.guid != TerraEngine::EMPTY_UUID) children.push_back(&child);
			}
			std::sort(children.begin(), children.end(), [](const auto* left, const auto* right) {
				if(left->semanticKey != right->semanticKey) return left->semanticKey < right->semanticKey;
				return left->guid < right->guid;
			});
			return children;
		}

		ModelPackagePrefabAnimationBindings BuildPrefabAnimationBindings(const AnimationImportManifest& manifest)
		{
			ModelPackagePrefabAnimationBindings bindings;
			const auto skeletons = FindSortedManifestChildren(manifest, AnimationImportSubAssetKind::Skeleton);
			const auto clips = FindSortedManifestChildren(manifest, AnimationImportSubAssetKind::AnimationClip);
			const auto deformableMeshes = FindSortedManifestChildren(manifest, AnimationImportSubAssetKind::DeformableMesh);
			const auto morphTargets = FindSortedManifestChildren(manifest, AnimationImportSubAssetKind::MorphTarget);
			for(const auto* skeleton : skeletons) bindings.skeletonGuids.push_back(skeleton->guid);
			for(const auto* clip : clips) bindings.clipGuids.push_back(clip->guid);
			for(const auto* mesh : deformableMeshes) bindings.deformableMeshGuids.push_back(mesh->guid);
			for(const auto* morph : morphTargets) bindings.morphTargetGuids.push_back(morph->guid);

			if(!bindings.skeletonGuids.empty()) bindings.primarySkeletonGuid = bindings.skeletonGuids.front();
			if(bindings.primarySkeletonGuid != TerraEngine::EMPTY_UUID)
			{
				// Pick the first semantic clip that explicitly targets the primary
				// skeleton. This remains stable when exporter arrays are reordered and
				// never defaults the prefab to a clip from a different skin.
				const auto compatibleClip = std::find_if(clips.begin(), clips.end(), [&](const auto* clip) {
					return std::any_of(clip->dependencies.begin(), clip->dependencies.end(), [&](const auto& dependency) {
						return dependency.assetGuid == bindings.primarySkeletonGuid;
					});
				});
				if(compatibleClip != clips.end()) bindings.defaultClipGuid = (*compatibleClip)->guid;
			}
			else if(!clips.empty())
			{
				// Node-only animation packages have no skeleton compatibility edge.
				// They still receive a deterministic preview clip selected by semantic key.
				bindings.defaultClipGuid = clips.front()->guid;
			}
			return bindings;
		}

		TerraEngine::UUID FindDefaultClipForSkeleton(
			const AnimationImportManifest& manifest,
			const TerraEngine::UUID& skeletonGuid)
		{
			if (skeletonGuid == TerraEngine::EMPTY_UUID) return TerraEngine::EMPTY_UUID;
			const auto clips = FindSortedManifestChildren(manifest, AnimationImportSubAssetKind::AnimationClip);
			const auto clip = std::find_if(clips.begin(), clips.end(), [&](const GeneratedAnimationSubAsset* candidate) {
				return std::any_of(candidate->dependencies.begin(), candidate->dependencies.end(), [&](const AnimationImportDependency& dependency) {
					return dependency.assetGuid == skeletonGuid;
				});
			});
			return clip == clips.end() ? TerraEngine::EMPTY_UUID : (*clip)->guid;
		}

		nlohmann::json UuidListToJson(const std::vector<TerraEngine::UUID>& values)
		{
			nlohmann::json array = nlohmann::json::array();
			for(const TerraEngine::UUID& value : values) array.push_back(TerraEngine::ToHexString(value));
			return array;
		}

		nlohmann::json MakePrefabAnimationBindingsJson(const ModelPackagePrefabAnimationBindings& bindings)
		{
			// This editor-facing metadata deliberately names clips separately from
			// controllerAssetId. Import does not invent a controller; it supplies the
			// stable assets needed for preview or a later controller authoring step.
			return {
				{ "schemaVersion", 1u },
				{ "primarySkeletonAssetId", TerraEngine::ToHexString(bindings.primarySkeletonGuid) },
				{ "skeletonAssetIds", UuidListToJson(bindings.skeletonGuids) },
				{ "defaultClipAssetId", TerraEngine::ToHexString(bindings.defaultClipGuid) },
				{ "clipAssetIds", UuidListToJson(bindings.clipGuids) },
				{ "deformableMeshAssetIds", UuidListToJson(bindings.deformableMeshGuids) },
				{ "morphTargetAssetIds", UuidListToJson(bindings.morphTargetGuids) }
			};
		}

		nlohmann::json MakeImportedAnimatorComponentJson(
			const TerraEngine::UUID& skeletonGuid,
			const TerraEngine::UUID& defaultClipGuid)
		{
			// Match AnimatorComponent's reflected authored defaults exactly. Runtime
			// handles and evaluated state remain owned by AnimationSystem and are never
			// serialized into an imported prefab.
			return {
				{ "active", true },
				{ "schemaVersion", TerraEngine::Animation::kAnimatorComponentSchemaVersion },
				{ "controllerAssetId", TerraEngine::ToHexString(TerraEngine::EMPTY_UUID) },
				{ "defaultClipAssetId", TerraEngine::ToHexString(defaultClipGuid) },
				{ "loopDefaultClip", true },
				{ "playAutomatically", true },
				{ "defaultClipStartFrame", 0U },
				{ "skeletonOverrideAssetId", TerraEngine::ToHexString(skeletonGuid) },
				{ "avatarOverrideAssetId", TerraEngine::ToHexString(TerraEngine::EMPTY_UUID) },
				{ "retargetProfileOverrideAssetId", TerraEngine::ToHexString(TerraEngine::EMPTY_UUID) },
				{ "ragdollProfileOverrideAssetId", TerraEngine::ToHexString(TerraEngine::EMPTY_UUID) },
				{ "motionDatabaseOverrideAssetId", TerraEngine::ToHexString(TerraEngine::EMPTY_UUID) },
				{ "requestedMode", static_cast<std::uint32_t>(TerraEngine::Animation::AnimationExecutionPolicy::Auto) },
				{ "rootMotionPolicy", static_cast<std::uint32_t>(TerraEngine::Animation::AnimatorRootMotionPolicy::ExtractOnly) },
				{ "updatePolicy", static_cast<std::uint32_t>(TerraEngine::Animation::AnimatorUpdatePolicy::EveryFrame) },
				{ "timePolicy", static_cast<std::uint32_t>(TerraEngine::Animation::AnimatorTimePolicy::Scaled) },
				{ "initialParameters", "{}" },
				{ "eventPolicy", static_cast<std::uint32_t>(TerraEngine::Animation::AnimatorEventPolicy::Dispatch) },
				{ "cullingPolicy", static_cast<std::uint32_t>(TerraEngine::Animation::AnimatorCullingPolicy::Automatic) },
				{ "diagnosticOverrides", 0u }
			};
		}

		nlohmann::json MakeModelPackagePrefabObjectJson(
			const std::string& objectName,
			const TerraEngine::UUID& objectGuid,
			const TerraEngine::UUID& meshGuid,
			const TerraEngine::UUID& materialGuid,
			const TerraEngine::UUID& skeletonGuid = TerraEngine::EMPTY_UUID,
			const TerraEngine::UUID& defaultClipGuid = TerraEngine::EMPTY_UUID)
		{
			// The importer writes prefab JSON directly because it runs outside a
			// live ObjectManager scene. Keep the shape identical to Serializer so
			// SceneManager can open the generated prefab without a special path.
			nlohmann::json object;
			object["name"] = objectName;
			object["tag"] = "Untagged";
			object["layer"] = "Default";
			object["active"] = true;
			object["isStatic"] = false;
			object["uuid"] = TerraEngine::ToHexString(objectGuid);
			object["components"] = {
				{ "Transform", MakeIdentityTransformJson() },
				{ "MeshRenderComponent", {
					{ "meshId", TerraEngine::ToHexString(meshGuid) },
					{ "materialId", TerraEngine::ToHexString(materialGuid) },
					{ "spatialObjectId", TerraEngine::ToHexString(TerraEngine::EMPTY_UUID) },
					{ "taskShaderCullingEnabled", true }
				} }
			};
			if (skeletonGuid != TerraEngine::EMPTY_UUID)
			{
				// The nearest animator owns this renderable. This is required for
				// independently-instanced morph nodes, whose palette must evaluate the
				// exact synthetic skeleton that contains the source node transform.
				object["components"]["AnimatorComponent"] =
					MakeImportedAnimatorComponentJson(skeletonGuid, defaultClipGuid);
			}
			return object;
		}

		void GenerateModelPackagePrefab(const ModelPackageImportOptions& options, ModelPackageImportResult& result)
		{
			if (result.generatedMeshes.empty() || result.package.importRoot.empty())
			{
				return;
			}

			const std::string prefabName = options.sourcePath.stem().string().empty()
				? std::string("ImportedModel")
				: options.sourcePath.stem().string();
			GeneratedModelPrefabAsset prefab{};
			prefab.guid = BuildStableGeneratedModelUuid(result.packageGuid, "model-prefab", 0u);
			prefab.displayName = prefabName;
			prefab.sourcePath = result.package.importRoot / (prefabName + ".teprefab");

			nlohmann::json prefabDocument;
			prefabDocument["name"] = prefab.displayName;
			prefabDocument["objects"] = nlohmann::json::array();
			const ModelPackagePrefabAnimationBindings animationBindings =
				BuildPrefabAnimationBindings(result.animationManifest);
			const bool hasAnimationBindings = !animationBindings.skeletonGuids.empty()
				|| !animationBindings.clipGuids.empty()
				|| !animationBindings.deformableMeshGuids.empty()
				|| !animationBindings.morphTargetGuids.empty();
			if(hasAnimationBindings)
			{
				prefabDocument["animationBindings"] = MakePrefabAnimationBindingsJson(animationBindings);
			}

			nlohmann::json root;
			root["name"] = prefab.displayName;
			root["tag"] = "Untagged";
			root["layer"] = "Default";
			root["active"] = true;
			root["isStatic"] = false;
			root["uuid"] = TerraEngine::ToHexString(BuildStableGeneratedModelUuid(result.packageGuid, "model-prefab-root", 0u));
			root["components"] = {
				{ "Transform", MakeIdentityTransformJson() }
			};
			if(animationBindings.primarySkeletonGuid != TerraEngine::EMPTY_UUID)
			{
				root["components"]["AnimatorComponent"] =
					MakeImportedAnimatorComponentJson(
						animationBindings.primarySkeletonGuid,
						animationBindings.defaultClipGuid);
			}
			root["children"] = nlohmann::json::array();

			for (std::size_t meshIndex = 0u; meshIndex < result.generatedMeshes.size(); ++meshIndex)
			{
				const GeneratedModelMeshAsset& mesh = result.generatedMeshes[meshIndex];
				const std::optional<TerraEngine::UUID> materialGuid = FindMaterialForMesh(result, mesh.guid);
				const TerraEngine::UUID objectGuid = BuildStableGeneratedModelUuid(result.packageGuid, "model-prefab-mesh-object", meshIndex);
				const bool needsDedicatedAnimator = mesh.animationSkeletonGuid != TerraEngine::EMPTY_UUID
					&& mesh.animationSkeletonGuid != animationBindings.primarySkeletonGuid;
				root["children"].push_back(MakeModelPackagePrefabObjectJson(
					mesh.displayName.empty() ? ("Mesh_" + std::to_string(meshIndex)) : mesh.displayName,
					objectGuid,
					mesh.guid,
					materialGuid.value_or(TerraEngine::EMPTY_UUID),
					needsDedicatedAnimator ? mesh.animationSkeletonGuid : TerraEngine::EMPTY_UUID,
					needsDedicatedAnimator
						? FindDefaultClipForSkeleton(result.animationManifest, mesh.animationSkeletonGuid)
						: TerraEngine::EMPTY_UUID));
				prefab.meshGuids.push_back(mesh.guid);
				if (materialGuid.has_value())
				{
					prefab.materialGuids.push_back(*materialGuid);
				}
			}

			prefabDocument["objects"].push_back(std::move(root));

			const std::filesystem::path prefabPath = options.assetRoot / prefab.sourcePath;
			std::error_code directoryError;
			std::filesystem::create_directories(NativeIoPath(prefabPath.parent_path()), directoryError);
			if (directoryError)
			{
				result.diagnostics.push_back({ ModelDiagnosticSeverity::Warning, "GeneratedPrefabWriteFailed", prefab.sourcePath.generic_string(), "Failed to create the generated prefab directory.", "Meshes and materials were still imported." });
				return;
			}

			std::ofstream output(NativeIoPath(prefabPath), std::ios::binary | std::ios::trunc);
			if (!output)
			{
				result.diagnostics.push_back({ ModelDiagnosticSeverity::Warning, "GeneratedPrefabWriteFailed", prefab.sourcePath.generic_string(), "Failed to write the generated prefab asset.", "Meshes and materials were still imported." });
				return;
			}
			output << std::setw(4) << prefabDocument << '\n';
			output.close();
			if (!output)
			{
				result.diagnostics.push_back({ ModelDiagnosticSeverity::Warning, "GeneratedPrefabWriteFailed", prefab.sourcePath.generic_string(), "Generated prefab output failed while closing.", "Meshes and materials were still imported." });
				return;
			}

			result.generatedPrefabs.clear();
			result.generatedPrefabs.push_back(std::move(prefab));
		}

		void PromoteStatusFromDiagnostics(ModelPackageImportResult& result)
		{
			bool hasError = false;
			bool hasWarning = false;
			for (const ModelImportDiagnostic& diagnostic : result.diagnostics)
			{
				hasError = hasError || diagnostic.severity == ModelDiagnosticSeverity::Error;
				hasWarning = hasWarning || diagnostic.severity == ModelDiagnosticSeverity::Warning;
			}

			if (hasError || result.generatedMeshes.empty())
			{
				result.status = ModelImportStatus::Failed;
			}
			else if (hasWarning)
			{
				result.status = ModelImportStatus::SucceededWithWarnings;
			}
			else
			{
				result.status = ModelImportStatus::Succeeded;
			}
		}
	}

	bool IsFbxPackageSourceExtension(const std::filesystem::path& filePath)
	{
		return NormalizeExtension(filePath) == ".fbx";
	}

	bool IsModelPackageSourceExtension(const std::filesystem::path& filePath)
	{
		return IsGltfPackageSourceExtension(filePath) || IsFbxPackageSourceExtension(filePath);
	}

	const char* GetModelPackageImporterName(const std::filesystem::path& filePath)
	{
		if (IsFbxPackageSourceExtension(filePath))
		{
			return "FbxPackageImporter";
		}
		if (IsGltfPackageSourceExtension(filePath))
		{
			return "GltfPackageImporter";
		}
		return "";
	}

	const char* GetModelPackageImporterVersion(const std::filesystem::path& filePath)
	{
		// Keep cache compatibility independent from compiled TEMF/animation file
		// versions. Only importer recipe changes belong here.
		if (IsFbxPackageSourceExtension(filePath))
		{
			return "4";
		}
		if (IsGltfPackageSourceExtension(filePath))
		{
			return "1";
		}
		return "";
	}

	bool IsModelPackageImporterName(std::string_view importerName)
	{
		return importerName == "GltfPackageImporter" || importerName == "FbxPackageImporter";
	}

	ModelPackageImportResult ImportModelPackage(const ModelPackageImportOptions& options)
	{
		ModelPackageImportResult result;
		result.packageGuid = options.packageGuid;
		result.package.sourcePath = options.sourceRelativePath;
		bool hasLastKnownGood = false;
		const std::uint64_t priorGeneration = ReadPriorPublishedGeneration(options, hasLastKnownGood);
		const auto rejectCandidate = [&](std::string code, std::string message, bool cancelled) {
			result.status = ModelImportStatus::Failed;
			result.animationManifest.packageGuid = options.packageGuid;
			result.animationManifest.sourceHashHex = result.package.sourceHashHex;
			result.animationManifest.importerName = GetModelPackageImporterName(options.sourcePath);
			result.animationManifest.candidateGeneration = priorGeneration + 1u;
			result.animationManifest.publishedGeneration = 0u;
			result.animationManifest.lastKnownGoodGeneration = priorGeneration;
			result.animationManifest.cancelled = cancelled;
			result.animationManifest.publishedAtomically = false;
			result.animationManifest.preservedLastKnownGood = hasLastKnownGood;
			result.diagnostics.push_back({
				cancelled ? ModelDiagnosticSeverity::Warning : ModelDiagnosticSeverity::Error,
				std::move(code), options.sourceRelativePath.generic_string(), std::move(message),
				hasLastKnownGood ? "The previous complete package generation remains active."
					: "No package generation was published."
			});
		};

		if(options.packageGuid == TerraEngine::EMPTY_UUID || options.assetRoot.empty()
			|| options.buildRoot.empty() || !IsModelPackageSourceExtension(options.sourcePath))
		{
			rejectCandidate("InvalidModelPackageImportOptions", "Package identity, roots, or source extension are invalid.", false);
			return result;
		}
		if(ImportCancellationRequested(options))
		{
			rejectCandidate("ModelPackageImportCancelled", "Import was cancelled before preparation began.", true);
			return result;
		}

		const std::string token = BuildPublicationToken(options);
		// Keep private candidates beside their destination volume for atomic rename,
		// but use one compact directory component so deeply nested Windows projects
		// do not exhaust the legacy path budget before publication.
		const std::filesystem::path assetStagingRoot = options.assetRoot / (".ta-" + token);
		const std::filesystem::path buildStagingRoot = options.buildRoot / (".tb-" + token);
		const auto cleanupStaging = [&]() {
			std::error_code cleanupError;
			std::filesystem::remove_all(NativeIoPath(assetStagingRoot), cleanupError);
			cleanupError.clear();
			std::filesystem::remove_all(NativeIoPath(buildStagingRoot), cleanupError);
		};
		cleanupStaging();
		std::error_code stagingError;
		std::filesystem::create_directories(NativeIoPath(assetStagingRoot), stagingError);
		if(!stagingError) std::filesystem::create_directories(NativeIoPath(buildStagingRoot), stagingError);
		if(stagingError)
		{
			rejectCandidate("ModelPackageStagingUnavailable", "The isolated package staging directories could not be created.", false);
			cleanupStaging();
			return result;
		}

		ModelPackageImportOptions stagedOptions = options;
		stagedOptions.assetRoot = assetStagingRoot;
		stagedOptions.buildRoot = buildStagingRoot;
		if(IsFbxPackageSourceExtension(options.sourcePath)) result = ImportFbxPackage(stagedOptions);
		else result = ImportGltfPackage(stagedOptions);

		AddFormatNeutralUnsupportedDiagnostics(stagedOptions, result);
		PopulateMissingVirtualGeometryMetadata(stagedOptions, result);
		GenerateModelPackagePrefab(stagedOptions, result);
		PromoteStatusFromDiagnostics(result);
		if(ImportCancellationRequested(options))
		{
			rejectCandidate("ModelPackageImportCancelled", "Import was cancelled before its protected publication boundary.", true);
			cleanupStaging();
			return result;
		}
		if(result.status == ModelImportStatus::Failed)
		{
			result.animationManifest.packageGuid = options.packageGuid;
			result.animationManifest.sourceHashHex = result.package.sourceHashHex;
			result.animationManifest.importerName = GetModelPackageImporterName(options.sourcePath);
			result.animationManifest.candidateGeneration = priorGeneration + 1u;
			result.animationManifest.publishedGeneration = 0u;
			result.animationManifest.lastKnownGoodGeneration = priorGeneration;
			result.animationManifest.cancelled = false;
			result.animationManifest.publishedAtomically = false;
			result.animationManifest.preservedLastKnownGood = hasLastKnownGood;
			result.diagnostics.push_back({
				ModelDiagnosticSeverity::Info,
				"ModelPackageCandidateRejected",
				options.sourceRelativePath.generic_string(),
				"The candidate package generation failed validation and was not published.",
				hasLastKnownGood ? "The previous complete package generation remains active."
					: "Fix the preceding diagnostics and retry the import."
			});
			cleanupStaging();
			return result;
		}

		FinalizeManifest(options, stagedOptions, priorGeneration, result);
		if(!ValidateManifestIdentity(result.animationManifest))
		{
			rejectCandidate("AnimationImportManifestInvalid", "Generated package children contain duplicate or incomplete stable identities.", false);
			cleanupStaging();
			return result;
		}
		std::string dependencyFailure;
		if(!ValidateAnimationImportManifestDependencies(result.animationManifest, &dependencyFailure))
		{
			rejectCandidate(
				"AnimationImportManifestDependencyInvalid",
				"Generated package dependency validation failed: " + dependencyFailure,
				false);
			cleanupStaging();
			return result;
		}

		const std::filesystem::path stagedManifestPath = buildStagingRoot
			/ ManifestRelativePath(options.packageGuid);
		std::filesystem::create_directories(NativeIoPath(stagedManifestPath.parent_path()), stagingError);
		std::ofstream manifestOutput(NativeIoPath(stagedManifestPath), std::ios::binary | std::ios::trunc);
		if(!stagingError && manifestOutput)
		{
			manifestOutput << std::setw(4) << ToJson(result.animationManifest) << '\n';
			manifestOutput.close();
		}
		if(stagingError || !manifestOutput)
		{
			rejectCandidate("AnimationImportManifestWriteFailed", "The staged package manifest could not be written completely.", false);
			cleanupStaging();
			return result;
		}
		if(ImportCancellationRequested(options))
		{
			rejectCandidate("ModelPackageImportCancelled", "Import was cancelled immediately before publication.", true);
			cleanupStaging();
			return result;
		}

		std::vector<StagedPublicationFile> publicationFiles;
		CollectStagedFiles(assetStagingRoot, options.assetRoot, token, publicationFiles);
		CollectStagedFiles(buildStagingRoot, options.buildRoot, token, publicationFiles);
		if(publicationFiles.empty() || !PublishStagedFiles(publicationFiles))
		{
			rejectCandidate("AnimationImportPublicationFailed", "The complete staged package generation could not be published transactionally.", false);
			cleanupStaging();
			return result;
		}
		cleanupStaging();
		return result;
	}
}
