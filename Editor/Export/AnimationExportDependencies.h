// AnimationExportDependencies.h - cooked animation dependency discovery.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Export
{
	inline constexpr std::uint32_t kAnimationExportManifestSchemaVersion = 1U;
	inline constexpr std::uint32_t kAnimationExportCapabilityVersion = 1U;
	// AssetType::Mesh deliberately uses zero, so an out-of-band value is needed
	// while discovery has not yet learned a dependency's concrete runtime type.
	inline constexpr std::uint32_t kUnknownAnimationExportAssetType = 0xFFFFFFFFU;
	inline constexpr std::string_view kAnimationRuntimeManifestPath = "animation/runtime-manifest.json";

	// One edge combines the dependency encoded in a cooked animation envelope
	// with any stronger compatibility fingerprint recorded by an import manifest.
	struct AnimationExportDependency
	{
		std::string assetGuid;
		std::uint32_t expectedAssetType = kUnknownAnimationExportAssetType;
		std::uint32_t expectedGeneration = 0U;
		std::uint64_t expectedCompatibilityFingerprint = 0U;
		bool required = true;
		std::string resolvedPath;
		std::string resolvedAssetClass;
	};

	// Assets are keyed by stable GUID. Multiple identities may intentionally
	// share one payload, for example an embedded morph identity in a cooked mesh.
	struct AnimationExportAsset
	{
		std::string assetGuid;
		std::string path;
		std::string assetClass;
		std::uint32_t assetType = kUnknownAnimationExportAssetType;
		std::uint32_t generation = 1U;
		std::uint64_t byteSize = 0U;
		std::string fileHash;
		std::string envelopeContentHash;
		std::uint16_t formatMajor = 0U;
		std::uint16_t formatMinor = 0U;
		std::uint64_t compatibilityFingerprint = 0U;
		bool cookedAnimationEnvelope = false;
		bool generatedPackageAsset = false;
		std::vector<AnimationExportDependency> dependencies;
	};

	// Editor package sidecars are never copied verbatim because they contain
	// source paths. This record drives a small source-free runtime manifest.
	struct AnimationExportPackageManifest
	{
		std::string packageGuid;
		std::uint32_t schemaVersion = 1U;
		std::uint64_t publishedGeneration = 0U;
		std::string runtimePath;
		std::vector<std::string> assetGuids;
	};

	struct AnimationExportIssue
	{
		std::string code;
		std::string path;
		std::string dependencyGuid;
		std::string message;
	};

	struct AnimationExportClosure
	{
		std::vector<AnimationExportAsset> assets;
		std::vector<AnimationExportPackageManifest> packageManifests;
		std::vector<std::string> requiredRuntimePaths;
		std::vector<AnimationExportIssue> issues;

		[[nodiscard]] bool Complete() const noexcept { return issues.empty(); }
		[[nodiscard]] bool HasAnimationAssets() const noexcept;
	};

	struct AnimationExportDiscoveryRequest
	{
		// projectAssetsRoot contains editor sidecars and is read-only. stagedContentRoot
		// contains the exact runtime payload candidates that will enter TEPAK.
		std::filesystem::path projectAssetsRoot;
		std::filesystem::path stagedContentRoot;
	};

	[[nodiscard]] bool IsCookedAnimationPath(const std::filesystem::path& path);
	[[nodiscard]] std::string AnimationAssetClassForPath(const std::filesystem::path& path);
	[[nodiscard]] std::optional<std::uint32_t> AnimationAssetTypeCode(std::string_view assetClass);
	[[nodiscard]] const std::vector<std::string>& RequiredAnimationShaderPaths();
	[[nodiscard]] bool IsForbiddenAnimationRuntimePath(std::string_view logicalPath);

	// Discovery derives roots and edges from real staged bytes and generated
	// package sidecars. Callers cannot inject an already-resolved string graph.
	class AnimationExportDependencyResolver final
	{
	public:
		[[nodiscard]] static AnimationExportClosure Resolve(
			const AnimationExportDiscoveryRequest& request);

		// Writes only sanitized runtime manifests beneath stagedContentRoot. Source,
		// editor, and build-tree paths are intentionally absent from these files.
		[[nodiscard]] static bool WriteRuntimeManifests(
			const AnimationExportClosure& closure,
			const std::filesystem::path& stagedContentRoot,
			std::string& outError);
	};
}
