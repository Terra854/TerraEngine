// AnimationExportDependencies.cpp - envelope and import-manifest closure.

#include "AnimationExportDependencies.h"

#include "AssetImportResult.h"
#include "File Formats/TerraEngineAnimationFile.h"
#include "io/PathNormalizer.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace TerraEngine::Export
{
	namespace
	{
		using TerraEngine::Animation::AnimationAssetEnvelope;
		using TerraEngine::Animation::AnimationAssetReader;
		using TerraEngine::Animation::AnimationDependencyEntry;
		using TerraEngine::Animation::AnimationStatus;
		using TerraEngine::AssetPipeline::AnimationImportSubAssetKind;
		using TerraEngine::AssetPipeline::GeneratedAnimationSubAsset;
		using TerraEngine::AssetPipeline::ModelPackageImportResult;

		constexpr std::uint64_t kMaximumCookedAnimationBytes = 1024ULL * 1024ULL * 1024ULL;
		constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
		// Generated animation manifests are authored by the model importers with
		// TerraEngine's established stable-file-hash seed. Keep this separate from
		// the canonical FNV seed used by TEPAK package-manifest records.
		constexpr std::uint64_t kImportedPayloadFnvOffset = 1469598103934665603ULL;
		constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

		struct Catalog final
		{
			std::map<std::string, AnimationExportAsset> byGuid;
			std::map<std::string, std::string> importedPayloadHashByGuid;
			std::map<std::string, std::string> manifestPayloadHashByGuid;
			std::set<std::string> rootGuids;
			std::vector<AnimationExportPackageManifest> packageManifests;
			std::vector<AnimationExportIssue> issues;
		};

		// TEPAK logical paths are lowercase and slash-normalized, while the staged
		// filesystem keeps the author's original casing. Keep one checked lookup
		// table so Linux can open "Models/Hero.teprefab" through its canonical
		// "models/hero.teprefab" identity without manufacturing a second path.
		struct StagedContentIndex final
		{
			std::map<std::string, std::filesystem::path> physicalPathByLogicalPath;
		};

		std::string LowercaseAscii(std::string value)
		{
			for(char& character : value)
			{
				if(character >= 'A' && character <= 'Z')
				{
					character = static_cast<char>(character + ('a' - 'A'));
				}
			}
			return value;
		}

		std::string GuidToString(const TerraEngine::UUID& guid)
		{
			return LowercaseAscii(TerraEngine::ToHexString(guid));
		}

		std::string ContentHashToHex(const TerraEngine::Animation::AnimationContentHash& hash)
		{
			std::ostringstream output;
			output << std::hex << std::setfill('0');
			for(const std::uint8_t byte : hash)
			{
				output << std::setw(2) << static_cast<unsigned int>(byte);
			}
			return output.str();
		}

		std::string HashBytes(std::span<const std::uint8_t> bytes)
		{
			std::uint64_t hash = kFnvOffset;
			for(const std::uint8_t byte : bytes)
			{
				hash ^= byte;
				hash *= kFnvPrime;
			}
			std::ostringstream output;
			output << std::hex << std::setfill('0') << std::setw(16) << hash;
			return output.str();
		}

		std::string HashImportedPayloadBytes(std::span<const std::uint8_t> bytes)
		{
			std::uint64_t hash = kImportedPayloadFnvOffset;
			for(const std::uint8_t byte : bytes)
			{
				hash ^= byte;
				hash *= kFnvPrime;
			}
			std::ostringstream output;
			output << std::hex << std::setfill('0') << std::setw(16) << hash;
			return output.str();
		}

		bool ReadBoundedFile(
			const std::filesystem::path& path,
			std::uint64_t maximumBytes,
			std::vector<std::uint8_t>& outBytes)
		{
			std::error_code error;
			const std::uint64_t size = std::filesystem::file_size(path, error);
			if(error || size > maximumBytes || size > static_cast<std::uint64_t>(SIZE_MAX))
			{
				return false;
			}

			std::ifstream input(path, std::ios::binary);
			if(!input)
			{
				return false;
			}
			outBytes.resize(static_cast<std::size_t>(size));
			if(!outBytes.empty())
			{
				input.read(reinterpret_cast<char*>(outBytes.data()), static_cast<std::streamsize>(outBytes.size()));
			}
			return input.good() || input.eof();
		}

		std::string NormalizeManifestRuntimePath(const std::filesystem::path& path)
		{
			if(path.empty() || path.is_absolute())
			{
				return {};
			}

			std::string normalized = tepak::io::NormalizeLookupPath(path.generic_string());
			// Older sidecars sometimes persisted a project-relative compiled path.
			// Strip the known roots once so runtime references remain archive-relative.
			for(const std::string_view prefix : {"build/assets/", "assets/"})
			{
				if(normalized.starts_with(prefix))
				{
					normalized.erase(0U, prefix.size());
					break;
				}
			}
			return normalized;
		}

		std::uint32_t AssetTypeForGeneratedKind(AnimationImportSubAssetKind kind)
		{
			switch(kind)
			{
			case AnimationImportSubAssetKind::Skeleton:
				return 6U;
			case AnimationImportSubAssetKind::AnimationClip:
				return 7U;
			case AnimationImportSubAssetKind::DeformableMesh:
			case AnimationImportSubAssetKind::MorphTarget:
			case AnimationImportSubAssetKind::Mesh:
				return 0U;
			case AnimationImportSubAssetKind::Material:
				return 2U;
			case AnimationImportSubAssetKind::Texture:
				return 1U;
			case AnimationImportSubAssetKind::Prefab:
				return kUnknownAnimationExportAssetType;
			}
			return kUnknownAnimationExportAssetType;
		}

		std::uint32_t AssetTypeForPath(const std::filesystem::path& path)
		{
			const std::string assetClass = tepak::io::DetectAssetClass(path);
			if(const std::optional<std::uint32_t> animationType = AnimationAssetTypeCode(assetClass))
			{
				return *animationType;
			}
			const std::string extension = LowercaseAscii(path.extension().string());
			if(extension == ".temf" || assetClass == "mesh") return 0U;
			if(extension == ".tetf" || assetClass == "texture") return 1U;
			if(extension == ".temat") return 2U;
			if(assetClass == "shader") return 3U;
			if(extension == ".tephysmat") return 4U;
			if(extension == ".tepcoll") return 5U;
			return kUnknownAnimationExportAssetType;
		}

		void AddIssue(
			Catalog& catalog,
			std::string code,
			std::string path,
			std::string dependencyGuid,
			std::string message)
		{
			catalog.issues.push_back({
				std::move(code), std::move(path), std::move(dependencyGuid), std::move(message)});
		}

		StagedContentIndex BuildStagedContentIndex(
			const AnimationExportDiscoveryRequest& request,
			Catalog& catalog)
		{
			StagedContentIndex index;
			std::error_code error;
			if(!std::filesystem::is_directory(request.stagedContentRoot, error))
			{
				return index;
			}

			try
			{
				for(const std::filesystem::directory_entry& entry :
					std::filesystem::recursive_directory_iterator(request.stagedContentRoot))
				{
					if(!entry.is_regular_file())
					{
						continue;
					}
					const std::string logicalPath = tepak::io::NormalizeRelativePath(
						request.stagedContentRoot, entry.path());
					const auto [existing, inserted] =
						index.physicalPathByLogicalPath.emplace(logicalPath, entry.path());
					if(!inserted && existing->second != entry.path())
					{
						// Two physical files that collapse to one TEPAK key would be
						// platform-dependent. Reject them before either can win.
						AddIssue(catalog, "animation.export.path_collision", logicalPath, {},
							"Multiple staged files normalize to the same runtime path.");
					}
				}
			}
			catch(const std::exception& exception)
			{
				AddIssue(catalog, "animation.export.staging_index_failed", {}, {},
					std::string("The staged runtime file index could not be built: ")
						+ exception.what());
			}
			return index;
		}

		void MergeDependency(
			AnimationExportAsset& asset,
			AnimationExportDependency dependency)
		{
			const auto existing = std::find_if(asset.dependencies.begin(), asset.dependencies.end(),
				[&](const AnimationExportDependency& candidate) {
					return candidate.assetGuid == dependency.assetGuid;
				});
			if(existing == asset.dependencies.end())
			{
				asset.dependencies.push_back(std::move(dependency));
				return;
			}

			// Envelope values are authoritative for type/generation. Generated
			// manifests strengthen the edge with a compatibility fingerprint.
			if(existing->expectedAssetType == kUnknownAnimationExportAssetType)
			{
				existing->expectedAssetType = dependency.expectedAssetType;
			}
			if(existing->expectedGeneration == 0U)
			{
				existing->expectedGeneration = dependency.expectedGeneration;
			}
			if(existing->expectedCompatibilityFingerprint == 0U)
			{
				existing->expectedCompatibilityFingerprint = dependency.expectedCompatibilityFingerprint;
			}
			existing->required = existing->required || dependency.required;
		}

		AnimationExportAsset& UpsertAsset(Catalog& catalog, AnimationExportAsset candidate)
		{
			AnimationExportAsset& current = catalog.byGuid[candidate.assetGuid];
			if(current.assetGuid.empty())
			{
				current = std::move(candidate);
				return current;
			}

			if(current.path.empty()) current.path = std::move(candidate.path);
			if(current.assetClass.empty()) current.assetClass = std::move(candidate.assetClass);
			if(current.assetType == kUnknownAnimationExportAssetType) current.assetType = candidate.assetType;
			if(current.generation == 0U) current.generation = candidate.generation;
			if(current.byteSize == 0U) current.byteSize = candidate.byteSize;
			if(current.fileHash.empty()) current.fileHash = std::move(candidate.fileHash);
			if(current.envelopeContentHash.empty()) current.envelopeContentHash = std::move(candidate.envelopeContentHash);
			if(current.formatMajor == 0U) current.formatMajor = candidate.formatMajor;
			if(current.formatMinor == 0U) current.formatMinor = candidate.formatMinor;
			if(current.compatibilityFingerprint == 0U) current.compatibilityFingerprint = candidate.compatibilityFingerprint;
			current.cookedAnimationEnvelope = current.cookedAnimationEnvelope || candidate.cookedAnimationEnvelope;
			current.generatedPackageAsset = current.generatedPackageAsset || candidate.generatedPackageAsset;
			for(AnimationExportDependency& dependency : candidate.dependencies)
			{
				MergeDependency(current, std::move(dependency));
			}
			return current;
		}

		void DiscoverCookedAnimationAssets(
			const AnimationExportDiscoveryRequest& request,
			Catalog& catalog)
		{
			std::error_code error;
			if(!std::filesystem::is_directory(request.stagedContentRoot, error))
			{
				AddIssue(catalog, "animation.export.staging_missing", {}, {},
					"The staged content root does not exist or is not a directory.");
				return;
			}

			for(const std::filesystem::directory_entry& entry :
				std::filesystem::recursive_directory_iterator(request.stagedContentRoot))
			{
				if(!entry.is_regular_file() || !IsCookedAnimationPath(entry.path()))
				{
					continue;
				}

				const std::string logicalPath = tepak::io::NormalizeRelativePath(
					request.stagedContentRoot, entry.path());
				std::vector<std::uint8_t> bytes;
				if(!ReadBoundedFile(entry.path(), kMaximumCookedAnimationBytes, bytes))
				{
					AddIssue(catalog, "animation.export.asset_unreadable", logicalPath, {},
						"The cooked animation payload is unreadable or exceeds the bounded export limit.");
					continue;
				}

				AnimationAssetReader reader;
				const AnimationStatus status = reader.Parse(bytes);
				if(status != AnimationStatus::Ok)
				{
					AddIssue(catalog, "animation.export.asset_invalid", logicalPath, {},
						"The cooked animation envelope failed checked format/hash/range validation (status "
						+ std::to_string(static_cast<std::uint16_t>(status)) + ").");
					continue;
				}

				const AnimationAssetEnvelope& envelope = reader.Envelope();
				AnimationExportAsset asset;
				asset.assetGuid = GuidToString(envelope.assetId.value);
				asset.path = logicalPath;
				asset.assetClass = AnimationAssetClassForPath(entry.path());
				asset.assetType = AnimationAssetTypeCode(asset.assetClass).value_or(kUnknownAnimationExportAssetType);
				asset.byteSize = bytes.size();
				asset.fileHash = HashBytes(bytes);
				catalog.importedPayloadHashByGuid[asset.assetGuid] =
					HashImportedPayloadBytes(bytes);
				asset.envelopeContentHash = ContentHashToHex(envelope.contentHash);
				asset.formatMajor = envelope.majorVersion;
				asset.formatMinor = envelope.minorVersion;
				asset.compatibilityFingerprint = envelope.compatibilityFingerprint;
				asset.cookedAnimationEnvelope = true;
				for(const AnimationDependencyEntry& dependency : reader.Dependencies())
				{
					if(dependency.assetId.value == TerraEngine::EMPTY_UUID)
					{
						AddIssue(catalog, "animation.export.dependency_guid_missing", logicalPath, {},
							"A cooked animation dependency edge is missing its stable asset GUID.");
						continue;
					}
					asset.dependencies.push_back({
						GuidToString(dependency.assetId.value),
						dependency.expectedAssetType,
						dependency.expectedGeneration,
						0U,
						true,
						{},
						{}});
				}

				if(envelope.assetId.value == TerraEngine::EMPTY_UUID)
				{
					AddIssue(catalog, "animation.export.asset_guid_missing", logicalPath, {},
						"A cooked animation envelope does not contain a stable asset GUID.");
					continue;
				}
				UpsertAsset(catalog, std::move(asset));
				catalog.rootGuids.insert(GuidToString(envelope.assetId.value));
			}
		}

		bool IsImportResultSidecar(const std::filesystem::path& path)
		{
			return LowercaseAscii(path.filename().string()).ends_with(".import.json");
		}

		std::filesystem::path SourcePathForSidecar(const std::filesystem::path& sidecar)
		{
			std::string text = sidecar.string();
			constexpr std::string_view suffix = ".import.json";
			if(LowercaseAscii(text).ends_with(suffix))
			{
				text.resize(text.size() - suffix.size());
			}
			return std::filesystem::path(text);
		}

		std::string RuntimePathForGeneratedAsset(const GeneratedAnimationSubAsset& subAsset)
		{
			if(!subAsset.compiledPath.empty())
			{
				return NormalizeManifestRuntimePath(subAsset.compiledPath);
			}
			// Prefabs are authored runtime documents; other source package paths are
			// editor inputs and must never become a fallback runtime dependency.
			if(subAsset.kind == AnimationImportSubAssetKind::Prefab)
			{
				return NormalizeManifestRuntimePath(subAsset.sourcePath);
			}
			return {};
		}

		void AddGeneratedSubAsset(
			Catalog& catalog,
			const GeneratedAnimationSubAsset& subAsset,
			AnimationExportPackageManifest& packageManifest)
		{
			const std::string guid = GuidToString(subAsset.guid);
			const std::string runtimePath = RuntimePathForGeneratedAsset(subAsset);
			if(subAsset.guid == TerraEngine::EMPTY_UUID)
			{
				AddIssue(catalog, "animation.export.manifest_guid_missing", runtimePath, {},
					"A generated package asset is missing its stable GUID.");
				return;
			}
			if(runtimePath.empty() || IsForbiddenAnimationRuntimePath(runtimePath))
			{
				AddIssue(catalog, "animation.export.manifest_path_invalid", runtimePath, guid,
					"A generated package asset does not resolve to a source-free archive-relative runtime path.");
				return;
			}

			AnimationExportAsset candidate;
			candidate.assetGuid = guid;
			candidate.path = runtimePath;
			candidate.assetClass = tepak::io::DetectAssetClass(std::filesystem::path(runtimePath));
			candidate.assetType = AssetTypeForGeneratedKind(subAsset.kind);
			// Package generation is recorded on the sanitized package manifest. A
			// cooked dependency edge's generation remains the v1 runtime generation.
			candidate.generation = 1U;
			candidate.compatibilityFingerprint = subAsset.compatibilityFingerprint;
			candidate.generatedPackageAsset = true;
			for(const TerraEngine::AssetPipeline::AnimationImportDependency& dependency : subAsset.dependencies)
			{
				if(dependency.assetGuid == TerraEngine::EMPTY_UUID)
				{
					AddIssue(catalog, "animation.export.manifest_dependency_guid_missing", runtimePath, guid,
						"A generated package dependency is missing its stable asset GUID.");
					continue;
				}
				candidate.dependencies.push_back({
					GuidToString(dependency.assetGuid),
					kUnknownAnimationExportAssetType,
					0U,
					dependency.compatibilityFingerprint,
					dependency.required,
					{},
					{}});
			}

			AnimationExportAsset& merged = UpsertAsset(catalog, std::move(candidate));
			// Export deliberately rewrites staged prefab documents so managed
			// class names become deterministic runtime binding IDs. Their original
			// importer hash therefore cannot be compared with post-rewrite bytes;
			// immutable generated payloads remain covered by the strict check.
			if(subAsset.kind != AnimationImportSubAssetKind::Prefab
				&& !subAsset.contentHashHex.empty())
			{
				catalog.manifestPayloadHashByGuid[guid] =
					LowercaseAscii(subAsset.contentHashHex);
			}
			if(merged.path != runtimePath)
			{
				AddIssue(catalog, "animation.export.manifest_path_stale", runtimePath, guid,
					"The generated package manifest path disagrees with the staged cooked asset identity.");
			}
			if(merged.assetType != kUnknownAnimationExportAssetType
				&& merged.assetType != AssetTypeForGeneratedKind(subAsset.kind))
			{
				AddIssue(catalog, "animation.export.manifest_type_stale", runtimePath, guid,
					"The generated package manifest type disagrees with the staged cooked asset type.");
			}
			if(merged.cookedAnimationEnvelope
				&& subAsset.compatibilityFingerprint != 0U
				&& merged.compatibilityFingerprint != subAsset.compatibilityFingerprint)
			{
				AddIssue(catalog, "animation.export.manifest_fingerprint_stale", runtimePath, guid,
					"The generated package manifest compatibility fingerprint does not match the cooked envelope.");
			}

			packageManifest.assetGuids.push_back(guid);
			catalog.rootGuids.insert(guid);
		}

		void DiscoverGeneratedPackageManifests(
			const AnimationExportDiscoveryRequest& request,
			Catalog& catalog)
		{
			std::error_code error;
			if(!std::filesystem::is_directory(request.projectAssetsRoot, error))
			{
				return;
			}

			for(const std::filesystem::directory_entry& entry :
				std::filesystem::recursive_directory_iterator(request.projectAssetsRoot))
			{
				if(!entry.is_regular_file() || !IsImportResultSidecar(entry.path()))
				{
					continue;
				}

				ModelPackageImportResult importResult;
				const std::filesystem::path sourcePath = SourcePathForSidecar(entry.path());
				if(!TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourcePath, importResult))
				{
					AddIssue(catalog, "animation.export.manifest_unreadable",
						entry.path().filename().generic_string(), {},
						"A generated package import manifest could not be parsed.");
					continue;
				}
				if(importResult.animationManifest.subAssets.empty())
				{
					continue;
				}

				const TerraEngine::AssetPipeline::AnimationImportManifest& manifest = importResult.animationManifest;
				std::string dependencyError;
				const bool dependenciesValid = TerraEngine::AssetPipeline::ValidateAnimationImportManifestDependencies(
					manifest, &dependencyError);
				const std::string computedFingerprint =
					TerraEngine::AssetPipeline::BuildAnimationImportDependencyFingerprint(manifest);
				if(manifest.schemaVersion != 1U || manifest.packageGuid == TerraEngine::EMPTY_UUID
					|| manifest.cancelled || !manifest.publishedAtomically
					|| manifest.publishedGeneration == 0U
					|| manifest.candidateGeneration != manifest.publishedGeneration
					|| !dependenciesValid
					|| LowercaseAscii(manifest.dependencyFingerprintHex) != LowercaseAscii(computedFingerprint))
				{
					AddIssue(catalog, "animation.export.manifest_incompatible",
						entry.path().filename().generic_string(), {},
						dependencyError.empty()
							? "The generated animation package manifest is stale, cancelled, unsupported, or was not atomically published."
							: dependencyError);
					continue;
				}

				AnimationExportPackageManifest packageManifest;
				packageManifest.packageGuid = GuidToString(manifest.packageGuid);
				packageManifest.schemaVersion = manifest.schemaVersion;
				packageManifest.publishedGeneration = manifest.publishedGeneration;
				packageManifest.runtimePath = "animation/manifests/" + packageManifest.packageGuid + ".json";
				for(const GeneratedAnimationSubAsset& subAsset : manifest.subAssets)
				{
					AddGeneratedSubAsset(catalog, subAsset, packageManifest);
				}
				std::sort(packageManifest.assetGuids.begin(), packageManifest.assetGuids.end());
				packageManifest.assetGuids.erase(std::unique(
					packageManifest.assetGuids.begin(), packageManifest.assetGuids.end()),
					packageManifest.assetGuids.end());
				catalog.packageManifests.push_back(std::move(packageManifest));
			}
		}

		void DiscoverMetadataAssets(
			const AnimationExportDiscoveryRequest& request,
			Catalog& catalog)
		{
			std::error_code error;
			if(!std::filesystem::is_directory(request.projectAssetsRoot, error))
			{
				return;
			}

			for(const std::filesystem::directory_entry& entry :
				std::filesystem::recursive_directory_iterator(request.projectAssetsRoot))
			{
				if(!entry.is_regular_file() || LowercaseAscii(entry.path().extension().string()) != ".meta")
				{
					continue;
				}
				try
				{
					std::ifstream input(entry.path());
					const nlohmann::json document = nlohmann::json::parse(input);
					const std::string guid = LowercaseAscii(document.value("guid", std::string{}));
					const std::string runtimePath = NormalizeManifestRuntimePath(
						document.value("compiledPath", std::string{}));
					if(guid.empty() || runtimePath.empty() || IsForbiddenAnimationRuntimePath(runtimePath))
					{
						continue;
					}

					AnimationExportAsset candidate;
					candidate.assetGuid = guid;
					candidate.path = runtimePath;
					candidate.assetClass = tepak::io::DetectAssetClass(std::filesystem::path(runtimePath));
					candidate.assetType = AssetTypeForPath(std::filesystem::path(runtimePath));
					UpsertAsset(catalog, std::move(candidate));
				}
				catch(const std::exception&)
				{
					// Generic malformed metadata is handled by the normal export validator.
					// Only an animation closure that actually references it becomes missing.
				}
			}
		}

		void PopulateStagedFileEvidence(
			const StagedContentIndex& stagedContent,
			Catalog& catalog)
		{
			for(auto& [guid, asset] : catalog.byGuid)
			{
				if(asset.path.empty())
				{
					continue;
				}
				const std::string logicalPath =
					tepak::io::NormalizeLookupPath(asset.path);
				const auto stagedPath = stagedContent.physicalPathByLogicalPath.find(logicalPath);
				if(stagedPath == stagedContent.physicalPathByLogicalPath.end())
				{
					continue;
				}
				std::vector<std::uint8_t> bytes;
				if(!ReadBoundedFile(stagedPath->second, kMaximumCookedAnimationBytes, bytes))
				{
					continue;
				}
				if(asset.byteSize == 0U) asset.byteSize = bytes.size();
				if(asset.fileHash.empty()) asset.fileHash = HashBytes(bytes);
				catalog.importedPayloadHashByGuid[guid] =
					HashImportedPayloadBytes(bytes);
			}
		}

		void ValidateGeneratedManifestPayloadHashes(Catalog& catalog)
		{
			// Sidecars are discovered before generic staged-file evidence is filled,
			// so compare their importer-contract hashes only after both views exist.
			for(const auto& [guid, expectedHash] : catalog.manifestPayloadHashByGuid)
			{
				const auto actual = catalog.importedPayloadHashByGuid.find(guid);
				if(actual == catalog.importedPayloadHashByGuid.end()
					|| LowercaseAscii(actual->second) == expectedHash)
				{
					continue;
				}
				const auto asset = catalog.byGuid.find(guid);
				AddIssue(
					catalog,
					"animation.export.manifest_hash_stale",
					asset == catalog.byGuid.end() ? std::string{} : asset->second.path,
					guid,
					"The generated package manifest hash does not match the staged cooked payload.");
			}
		}

		void ResolveClosure(Catalog& catalog, AnimationExportClosure& closure)
		{
			std::set<std::string> pending = catalog.rootGuids;
			std::set<std::string> visited;
			while(!pending.empty())
			{
				const std::string guid = *pending.begin();
				pending.erase(pending.begin());
				if(!visited.insert(guid).second)
				{
					continue;
				}

				auto ownerIt = catalog.byGuid.find(guid);
				if(ownerIt == catalog.byGuid.end())
				{
					AddIssue(catalog, "animation.export.root_missing", {}, guid,
						"An animation package root does not resolve to a staged runtime asset.");
					continue;
				}
				AnimationExportAsset& owner = ownerIt->second;
				if(owner.path.empty() || owner.fileHash.empty() || owner.byteSize == 0U)
				{
					AddIssue(catalog, "animation.export.asset_missing", owner.path, guid,
						"An animation dependency identity does not resolve to a readable staged runtime payload.");
				}

				for(AnimationExportDependency& dependency : owner.dependencies)
				{
					auto targetIt = catalog.byGuid.find(dependency.assetGuid);
					if(targetIt == catalog.byGuid.end())
					{
						if(dependency.required)
						{
							AddIssue(catalog, "animation.export.dependency_missing", owner.path,
								dependency.assetGuid,
								"A required cooked or generated-manifest dependency is absent from staging.");
						}
						continue;
					}

					AnimationExportAsset& target = targetIt->second;
					dependency.resolvedPath = target.path;
					dependency.resolvedAssetClass = target.assetClass;
					if(dependency.expectedAssetType != kUnknownAnimationExportAssetType
						&& dependency.expectedAssetType != target.assetType)
					{
						AddIssue(catalog, "animation.export.dependency_type_mismatch", owner.path,
							dependency.assetGuid,
							"A dependency resolves to a different stable AssetType than the cooked edge requires.");
					}
					if(dependency.expectedGeneration != 0U
						&& dependency.expectedGeneration != target.generation)
					{
						AddIssue(catalog, "animation.export.dependency_generation_mismatch", owner.path,
							dependency.assetGuid,
							"A dependency resolves to a stale generated package generation.");
					}
					if(dependency.expectedCompatibilityFingerprint != 0U
						&& dependency.expectedCompatibilityFingerprint != target.compatibilityFingerprint)
					{
						AddIssue(catalog, "animation.export.dependency_fingerprint_mismatch", owner.path,
							dependency.assetGuid,
							"A dependency resolves to an incompatible cooked fingerprint.");
					}
					pending.insert(dependency.assetGuid);
				}
			}

			for(const std::string& guid : visited)
			{
				const auto found = catalog.byGuid.find(guid);
				if(found != catalog.byGuid.end())
				{
					AnimationExportAsset asset = found->second;
					std::sort(asset.dependencies.begin(), asset.dependencies.end(), [](const auto& left, const auto& right) {
						return std::tie(left.assetGuid, left.expectedAssetType, left.expectedGeneration)
							< std::tie(right.assetGuid, right.expectedAssetType, right.expectedGeneration);
					});
					closure.assets.push_back(std::move(asset));
				}
			}
			std::sort(closure.assets.begin(), closure.assets.end(), [](const auto& left, const auto& right) {
				return std::tie(left.path, left.assetGuid) < std::tie(right.path, right.assetGuid);
			});
		}

		nlohmann::json DependencyToJson(const AnimationExportDependency& dependency)
		{
			return {
				{"guid", dependency.assetGuid},
				{"expectedAssetType", dependency.expectedAssetType},
				{"expectedGeneration", dependency.expectedGeneration},
				{"expectedCompatibilityFingerprint", dependency.expectedCompatibilityFingerprint},
				{"required", dependency.required},
				{"path", dependency.resolvedPath},
				{"assetClass", dependency.resolvedAssetClass}
			};
		}

		nlohmann::json AssetToJson(const AnimationExportAsset& asset)
		{
			nlohmann::json dependencies = nlohmann::json::array();
			for(const AnimationExportDependency& dependency : asset.dependencies)
			{
				dependencies.push_back(DependencyToJson(dependency));
			}
			return {
				{"guid", asset.assetGuid},
				{"path", asset.path},
				{"assetClass", asset.assetClass},
				{"assetType", asset.assetType},
				{"generation", asset.generation},
				{"size", asset.byteSize},
				{"fileHash", asset.fileHash},
				{"envelopeContentHash", asset.envelopeContentHash},
				{"formatMajor", asset.formatMajor},
				{"formatMinor", asset.formatMinor},
				{"compatibilityFingerprint", asset.compatibilityFingerprint},
				{"cookedAnimationEnvelope", asset.cookedAnimationEnvelope},
				{"generatedPackageAsset", asset.generatedPackageAsset},
				{"dependencies", dependencies}
			};
		}

		bool WriteJsonFile(const std::filesystem::path& path, const nlohmann::json& document, std::string& outError)
		{
			std::error_code error;
			std::filesystem::create_directories(path.parent_path(), error);
			if(error)
			{
				outError = "Could not create animation runtime-manifest directory: " + error.message();
				return false;
			}
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if(!output)
			{
				outError = "Could not create animation runtime manifest: " + path.generic_string();
				return false;
			}
			output << document.dump(2) << '\n';
			if(!output.good())
			{
				outError = "Could not finish animation runtime manifest: " + path.generic_string();
				return false;
			}
			return true;
		}
	}

	bool AnimationExportClosure::HasAnimationAssets() const noexcept
	{
		return std::any_of(assets.begin(), assets.end(), [](const AnimationExportAsset& asset) {
			return asset.cookedAnimationEnvelope;
		});
	}

	bool IsCookedAnimationPath(const std::filesystem::path& path)
	{
		return !AnimationAssetClassForPath(path).empty();
	}

	std::string AnimationAssetClassForPath(const std::filesystem::path& path)
	{
		const std::string extension = LowercaseAscii(path.extension().string());
		if(extension == ".teskel") return "skeleton";
		if(extension == ".teanim") return "animation-clip";
		if(extension == ".teanimctrl") return "animation-controller";
		if(extension == ".teanimmask") return "animation-mask";
		if(extension == ".teretarget") return "retarget-profile";
		if(extension == ".temotiondb") return "motion-database";
		if(extension == ".teragdoll") return "ragdoll-profile";
		return {};
	}

	std::optional<std::uint32_t> AnimationAssetTypeCode(std::string_view assetClass)
	{
		if(assetClass == "skeleton") return 6U;
		if(assetClass == "animation-clip") return 7U;
		if(assetClass == "animation-controller") return 8U;
		if(assetClass == "animation-mask") return 9U;
		if(assetClass == "retarget-profile") return 10U;
		if(assetClass == "motion-database") return 11U;
		if(assetClass == "ragdoll-profile") return 12U;
		return std::nullopt;
	}

	const std::vector<std::string>& RequiredAnimationShaderPaths()
	{
		static const std::vector<std::string> paths = {
			"shaders/animation/animationsample.comp.spv",
			"shaders/animation/animationblend.comp.spv",
			"shaders/animation/animationhierarchy.comp.spv",
			"shaders/animation/animationprocedural.comp.spv",
			"shaders/animation/animationmorph.comp.spv",
			"shaders/animation/animationbounds.comp.spv",
			"shaders/animation/animationpalette.comp.spv",
			"shaders/animation/animationragdoll.comp.spv",
			"shaders/animation/motionmatchscore.comp.spv",
			"shaders/animation/motionmatchtopk.comp.spv"
		};
		return paths;
	}

	bool IsForbiddenAnimationRuntimePath(std::string_view logicalPath)
	{
		const std::string normalized = LowercaseAscii(tepak::io::NormalizeLookupPath(logicalPath));
		if(normalized.empty() || normalized.starts_with("source/")
			|| normalized.starts_with("editor/") || normalized.starts_with("build/")
			|| normalized.find("../") != std::string::npos || normalized == ".."
			|| normalized.find(':') != std::string::npos)
		{
			return true;
		}
		const std::filesystem::path path(normalized);
		const std::string extension = LowercaseAscii(path.extension().string());
		return extension == ".fbx" || extension == ".gltf" || extension == ".glb"
			|| extension == ".dae" || extension == ".blend" || extension == ".meta"
			|| normalized.ends_with(".import.json");
	}

	AnimationExportClosure AnimationExportDependencyResolver::Resolve(
		const AnimationExportDiscoveryRequest& request)
	{
		Catalog catalog;
		const StagedContentIndex stagedContent =
			BuildStagedContentIndex(request, catalog);
		DiscoverCookedAnimationAssets(request, catalog);
		DiscoverGeneratedPackageManifests(request, catalog);
		DiscoverMetadataAssets(request, catalog);
		PopulateStagedFileEvidence(stagedContent, catalog);
		ValidateGeneratedManifestPayloadHashes(catalog);

		AnimationExportClosure closure;
		ResolveClosure(catalog, closure);
		closure.packageManifests = std::move(catalog.packageManifests);
		closure.issues = std::move(catalog.issues);
		std::sort(closure.packageManifests.begin(), closure.packageManifests.end(), [](const auto& left, const auto& right) {
			return left.packageGuid < right.packageGuid;
		});
		if(closure.HasAnimationAssets())
		{
			closure.requiredRuntimePaths = RequiredAnimationShaderPaths();
			for(const std::string& path : closure.requiredRuntimePaths)
			{
				const std::string logicalPath =
					tepak::io::NormalizeLookupPath(path);
				if(!stagedContent.physicalPathByLogicalPath.contains(logicalPath))
				{
					closure.issues.push_back({
						"animation.export.shader_missing", path, {},
						"A required cooked animation shader variant is absent from export staging."});
				}
			}
		}
		return closure;
	}

	bool AnimationExportDependencyResolver::WriteRuntimeManifests(
		const AnimationExportClosure& closure,
		const std::filesystem::path& stagedContentRoot,
		std::string& outError)
	{
		outError.clear();
		if(!closure.Complete())
		{
			outError = closure.issues.empty()
				? "Animation export dependency closure is incomplete."
				: closure.issues.front().message;
			return false;
		}

		std::unordered_map<std::string, const AnimationExportAsset*> assetsByGuid;
		for(const AnimationExportAsset& asset : closure.assets)
		{
			assetsByGuid.emplace(asset.assetGuid, &asset);
		}

		nlohmann::json packageManifestRecords = nlohmann::json::array();
		std::vector<std::string> requiredPaths = closure.requiredRuntimePaths;
		for(const AnimationExportPackageManifest& package : closure.packageManifests)
		{
			nlohmann::json assets = nlohmann::json::array();
			for(const std::string& guid : package.assetGuids)
			{
				const auto found = assetsByGuid.find(guid);
				if(found == assetsByGuid.end())
				{
					outError = "A sanitized package manifest references an asset outside the resolved closure: " + guid;
					return false;
				}
				assets.push_back(AssetToJson(*found->second));
			}
			const nlohmann::json document = {
				{"schemaVersion", kAnimationExportManifestSchemaVersion},
				{"packageGuid", package.packageGuid},
				{"publishedGeneration", package.publishedGeneration},
				{"assets", assets}
			};
			const std::filesystem::path outputPath = stagedContentRoot / std::filesystem::path(package.runtimePath);
			if(!WriteJsonFile(outputPath, document, outError))
			{
				return false;
			}
			std::vector<std::uint8_t> bytes;
			if(!ReadBoundedFile(outputPath, kMaximumCookedAnimationBytes, bytes))
			{
				outError = "Could not hash a sanitized animation package manifest.";
				return false;
			}
			packageManifestRecords.push_back({
				{"packageGuid", package.packageGuid},
				{"publishedGeneration", package.publishedGeneration},
				{"path", package.runtimePath},
				{"size", bytes.size()},
				{"fileHash", HashBytes(bytes)}
			});
			requiredPaths.push_back(package.runtimePath);
		}

		std::sort(requiredPaths.begin(), requiredPaths.end());
		requiredPaths.erase(std::unique(requiredPaths.begin(), requiredPaths.end()), requiredPaths.end());
		nlohmann::json assets = nlohmann::json::array();
		std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> classCountsAndSizes;
		for(const AnimationExportAsset& asset : closure.assets)
		{
			assets.push_back(AssetToJson(asset));
			auto& summary = classCountsAndSizes[asset.assetClass];
			++summary.first;
			summary.second += asset.byteSize;
		}
		nlohmann::json classSummary = nlohmann::json::object();
		for(const auto& [assetClass, summary] : classCountsAndSizes)
		{
			classSummary[assetClass] = {{"count", summary.first}, {"bytes", summary.second}};
		}

		const bool hasAnimation = closure.HasAnimationAssets();
		const nlohmann::json aggregate = {
			{"schemaVersion", kAnimationExportManifestSchemaVersion},
			{"formatMajor", TerraEngine::Animation::kAnimationAssetMajorVersion},
			{"formatMinor", TerraEngine::Animation::kAnimationAssetMinorVersion},
			{"capabilities", {
				{"version", kAnimationExportCapabilityVersion},
				{"shaderVersion", kAnimationExportCapabilityVersion},
				{"cpuFallback", true},
				{"headless", true},
				{"gpuVisual", hasAnimation}
			}},
			{"assetCount", closure.assets.size()},
			{"classSummary", classSummary},
			{"assets", assets},
			{"packageManifests", packageManifestRecords},
			{"requiredRuntimeEntries", requiredPaths}
		};
		return WriteJsonFile(
			stagedContentRoot / std::filesystem::path(std::string(kAnimationRuntimeManifestPath)),
			aggregate,
			outError);
	}
}
