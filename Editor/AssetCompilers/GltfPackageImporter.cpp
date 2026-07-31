// GltfPackageImporter.cpp - fastgltf-backed package import implementation.
//
// The current implementation establishes the package-aware import contract:
// fastgltf parses the source package, Terra records meshes/materials/textures,
// writes generated material assets, and emits diagnostics for unsupported data.
// Later tasks reuse this entry point for raw geometry and image-byte extraction.

#include "GltfPackageImporter.h"

#include "AnimationClipCompiler.h"
#include "MaterialAssetWriter.h"
#include "MeshCompiler.h"
#include "TextureCompiler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <nlohmann/json.hpp>

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		std::string NormalizeExtension(const std::filesystem::path& filePath)
		{
			std::string ext = filePath.extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return ext;
		}

		std::string SanitizeAssetName(std::string value, std::string_view fallback)
		{
			if (value.empty())
			{
				value = std::string(fallback);
			}

			for (char& ch : value)
			{
				const bool ok = std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-';
				if (!ok)
				{
					ch = '_';
				}
			}
			return value.empty() ? std::string(fallback) : value;
		}

		VirtualGeometryAdaptiveCompileSettings ToCompileSettings(const GeneratedModelMeshVirtualGeometrySettings& settings)
		{
			VirtualGeometryAdaptiveCompileSettings compileSettings;
			compileSettings.targetRatios = settings.targetRatios;
			compileSettings.minimumSourceTriangles = settings.minimumSourceTriangles;
			compileSettings.minimumGeneratedTriangles = settings.minimumGeneratedTriangles;
			compileSettings.minimumMeshletsPerGeneratedLevel = settings.minimumMeshletsPerGeneratedLevel;
			compileSettings.minimumUsefulTriangleReduction = settings.minimumUsefulTriangleReduction;
			compileSettings.minimumUsefulMeshletReduction = settings.minimumUsefulMeshletReduction;
			compileSettings.maximumGeometricError = settings.maximumGeometricError;
			compileSettings.settingsSource = settings.hasOverride ? "PerMeshOverride" : settings.settingsSource;
			return compileSettings;
		}

		GeneratedModelMeshVirtualGeometrySettings ToGeneratedSettings(const VirtualGeometryAdaptiveCompileSettings& settings, bool hasOverride)
		{
			GeneratedModelMeshVirtualGeometrySettings generatedSettings;
			generatedSettings.hasOverride = hasOverride;
			generatedSettings.targetRatios = settings.targetRatios;
			generatedSettings.minimumSourceTriangles = settings.minimumSourceTriangles;
			generatedSettings.minimumGeneratedTriangles = settings.minimumGeneratedTriangles;
			generatedSettings.minimumMeshletsPerGeneratedLevel = settings.minimumMeshletsPerGeneratedLevel;
			generatedSettings.minimumUsefulTriangleReduction = settings.minimumUsefulTriangleReduction;
			generatedSettings.minimumUsefulMeshletReduction = settings.minimumUsefulMeshletReduction;
			generatedSettings.maximumGeometricError = settings.maximumGeometricError;
			generatedSettings.settingsSource = hasOverride ? "PerMeshOverride" : settings.settingsSource;
			return generatedSettings;
		}

		const GeneratedModelMeshVirtualGeometrySettings*
			FindMeshVirtualGeometrySettings(
				const GltfPackageImportOptions& options,
				const TerraEngine::UUID& meshGuid)
		{
			for (const ModelPackageMeshVirtualGeometryOverride& override :
				options.virtualGeometryMeshOverrides)
			{
				if (override.meshGuid == meshGuid)
				{
					return &override.settings;
				}
			}
			return options.hasVirtualGeometrySettings
				? &options.virtualGeometrySettings
				: nullptr;
		}

		void PopulateVirtualGeometryTargetDiagnostics(const MeshCompileReport& report, GeneratedModelMeshAsset& mesh)
		{
			mesh.virtualGeometryTargets.clear();
			for (const VirtualGeometryRequestedTargetCompileReport& requested : report.virtualGeometry.requestedTargets)
			{
				GeneratedModelMeshVirtualGeometryTarget target{};
				target.targetIndex = requested.targetIndex;
				target.requestedRatio = requested.requestedRatio;
				target.status = requested.status;
				target.desiredTriangleCount = requested.desiredTriangleCount;
				target.desiredMeshletCount = requested.desiredMeshletCount;
				target.triangleCount = requested.desiredTriangleCount;
				target.meshletCount = requested.desiredMeshletCount;
				target.actualRatio = requested.requestedRatio;

				for (const VirtualGeometryGeneratedLevelCompileReport& generated : report.virtualGeometry.generatedLevels)
				{
					if (generated.requestedTargetIndex == requested.targetIndex)
					{
						target.status = "Generated";
						target.actualRatio = generated.actualRatio;
						target.actualTriangleCount = generated.triangleCount;
						target.actualMeshletCount = generated.meshletCount;
						target.triangleCount = target.actualTriangleCount;
						target.meshletCount = target.actualMeshletCount;
						target.geometricError = generated.geometricError;
						target.nearestGeneratedLevelIndex = generated.generatedLevelIndex;
						break;
					}
				}
				for (const VirtualGeometrySkippedLevelCompileReport& skipped : report.virtualGeometry.skippedLevels)
				{
					if (skipped.targetIndex == requested.targetIndex)
					{
						target.status = skipped.status;
						target.reason = skipped.reason;
						target.message = skipped.message;
						target.nearestGeneratedLevelIndex = skipped.nearestGeneratedLevelIndex;
						target.actualRatio = skipped.actualCandidateRatio > 0.0f ? skipped.actualCandidateRatio : target.actualRatio;
						target.actualTriangleCount = skipped.candidateTriangleCount;
						target.actualMeshletCount = skipped.candidateMeshletCount;
						target.triangleCount = target.actualTriangleCount > 0u ? target.actualTriangleCount : target.triangleCount;
						target.meshletCount = target.actualMeshletCount > 0u ? target.actualMeshletCount : target.meshletCount;
						break;
					}
				}
				mesh.virtualGeometryTargets.push_back(std::move(target));
			}
		}

		std::uint32_t CountMaterialAssignmentsForMesh(const ModelPackageImportResult& result, const TerraEngine::UUID& meshGuid)
		{
			return static_cast<std::uint32_t>(std::count_if(
				result.materialAssignments.begin(),
				result.materialAssignments.end(),
				[&meshGuid](const ModelMaterialAssignment& assignment) {
					return assignment.meshGuid == meshGuid;
				}));
		}

		void PopulatePackageMaterialCoverageDiagnostics(GeneratedModelMeshAsset& mesh, std::uint32_t sourceMaterialAssignmentCount)
		{
			mesh.virtualGeometryClusterSummary.sourceMaterialAssignmentCount = sourceMaterialAssignmentCount;
			if (sourceMaterialAssignmentCount == 0u || mesh.virtualGeometryClusterSummary.materialSectionCount == 0u)
			{
				mesh.virtualGeometryClusterSummary.materialCoverageValid = sourceMaterialAssignmentCount <= 1u;
				return;
			}

			const bool sectionCountCanRepresentAssignments =
				mesh.virtualGeometryClusterSummary.materialSectionCount >= sourceMaterialAssignmentCount
				&& mesh.virtualGeometryClusterSummary.clusterMaterialSectionCount >= sourceMaterialAssignmentCount;
			mesh.virtualGeometryClusterSummary.materialCoverageValid =
				mesh.virtualGeometryClusterSummary.materialCoverageValid
				&& sectionCountCanRepresentAssignments;
			if (!mesh.virtualGeometryClusterSummary.materialCoverageValid)
			{
				mesh.virtualGeometryWarnings.push_back(
					"Virtual geometry material coverage is diagnostic-only for this package mesh: authored material assignments exceed cooked VG material sections.");
			}
		}

		void AddCoverageTag(GeneratedModelMeshAsset& mesh, std::string_view tag)
		{
			const std::string tagText(tag);
			if (std::find(mesh.virtualGeometryCoverageTags.begin(), mesh.virtualGeometryCoverageTags.end(), tagText) == mesh.virtualGeometryCoverageTags.end())
			{
				mesh.virtualGeometryCoverageTags.push_back(tagText);
			}
		}

		struct ExtractedGltfMeshGeometry
		{
			// Each generated mesh child owns the raw vertices and indices that
			// will be cooked into its own TEMF. Keeping this per-child storage
			// prevents package-level cluster cuts from being applied to a child
			// GUID such as camera_uv or tripod_uv.
			std::vector<RawMeshVertex> vertices;
			std::vector<std::uint32_t> indices;

			bool HasTriangles() const
			{
				return !vertices.empty() && !indices.empty() && (indices.size() % 3u) == 0u;
			}
		};

		std::filesystem::path BuildGeneratedMeshCompiledPath(
			const std::filesystem::path& importRoot,
			std::string_view displayName,
			std::size_t meshIndex,
			std::vector<std::string>& usedFileNames)
		{
			const std::string sanitizedName = SanitizeAssetName(std::string(displayName), "Mesh_" + std::to_string(meshIndex));
			std::string fileName = sanitizedName + ".temf";
			if (std::find(usedFileNames.begin(), usedFileNames.end(), fileName) != usedFileNames.end())
			{
				fileName = sanitizedName + "_" + std::to_string(meshIndex) + ".temf";
			}
			usedFileNames.push_back(fileName);
			return importRoot / fileName;
		}

		void PopulateVirtualGeometryCoverageTags(GeneratedModelMeshAsset& mesh, ModelPackageKind packageKind, std::uint32_t sourceMaterialAssignmentCount)
		{
			// These tags are coverage metadata for tests and tooling. They do not
			// enable rendering paths; the cooked TEMF metadata remains the
			// authoritative runtime contract.
			mesh.virtualGeometryCoverageTags.clear();
			AddCoverageTag(mesh, "package-static-geometry");
			AddCoverageTag(mesh, packageKind == ModelPackageKind::GltfBinary ? "package-glb" : "package-gltf");
			if (!mesh.virtualGeometryTargets.empty())
			{
				AddCoverageTag(mesh, "adaptive-targets");
			}
			if (mesh.virtualGeometryStatus == "Skipped")
			{
				AddCoverageTag(mesh, "tiny-mesh-skip");
			}
			if (mesh.virtualGeometryCooked)
			{
				AddCoverageTag(mesh, "package-imported");
			}
			if (mesh.virtualGeometryDetailLevels >= 3u || mesh.virtualGeometrySourceTriangles >= 512u)
			{
				AddCoverageTag(mesh, "dense-cluster-selection");
			}
			if (mesh.virtualGeometryClusterSummary.clusterCount > 0u)
			{
				AddCoverageTag(mesh, "cluster-hierarchy");
			}
			if (mesh.virtualGeometryClusterSummary.fallbackCoverageValid)
			{
				AddCoverageTag(mesh, "fallback-coverage");
			}
			if (mesh.virtualGeometryClusterSummary.pageLinkCount > 0u)
			{
				AddCoverageTag(mesh, "page-links");
			}
			if (mesh.virtualGeometryConeSafetySummary.metadataValid)
			{
				AddCoverageTag(mesh, "cone-safety-metadata");
			}
			if (mesh.virtualGeometryConeSafetySummary.disabledClusterCount > 0u)
			{
				AddCoverageTag(mesh, "cone-culling-disabled");
			}
			if (mesh.virtualGeometryConeSafetySummary.eligibleClusterCount > 0u)
			{
				AddCoverageTag(mesh, "cone-culling-eligible");
			}
			if (sourceMaterialAssignmentCount > 1u)
			{
				AddCoverageTag(mesh, "multi-material-package");
			}
			if (sourceMaterialAssignmentCount > 1u && !mesh.virtualGeometryClusterSummary.materialCoverageValid)
			{
				AddCoverageTag(mesh, "material-coverage-review");
			}
		}

		std::string ComputeFileHashHex(const std::filesystem::path& filePath)
		{
			std::ifstream in(filePath, std::ios::binary);
			if (!in)
			{
				return {};
			}

			std::uint64_t hash = 1469598103934665603ull;
			constexpr std::uint64_t prime = 1099511628211ull;
			std::array<char, 4096> buffer{};
			while (in)
			{
				in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const std::streamsize readCount = in.gcount();
				for (std::streamsize index = 0; index < readCount; ++index)
				{
					hash ^= static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(index)]);
					hash *= prime;
				}
			}

			std::ostringstream out;
			out << std::hex << hash;
			return out.str();
		}

		TerraEngine::UUID StableChildUuid(const TerraEngine::UUID& packageGuid, std::string_view role, std::size_t index)
		{
			return BuildStableGeneratedModelUuid(packageGuid, role, index);
		}

		bool SourceTextContains(const std::filesystem::path& filePath, std::string_view token)
		{
			if (NormalizeExtension(filePath) != ".gltf")
			{
				return false;
			}
			std::ifstream in(filePath);
			if (!in)
			{
				return false;
			}
			const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			return text.find(token) != std::string::npos;
		}

		void AddDiagnostic(
			GltfImportResult& result,
			GltfDiagnosticSeverity severity,
			std::string code,
			std::string source,
			std::string message,
			std::string fallback = {})
		{
			result.diagnostics.push_back({ severity, std::move(code), std::move(source), std::move(message), std::move(fallback) });
		}

		void ReportProgress(const GltfPackageImportOptions& options, float progress, std::string_view stage, std::string_view message)
		{
			if (options.progressCallback)
			{
				options.progressCallback(std::clamp(progress, 0.0f, 1.0f), stage, message);
			}
		}

		std::string BuildPackageInventoryMessage(std::size_t meshCount, std::size_t textureCount, std::size_t materialCount)
		{
			std::ostringstream message;
			message << "Discovered " << meshCount << " mesh" << (meshCount == 1u ? "" : "es")
				<< ", " << textureCount << " texture" << (textureCount == 1u ? "" : "s")
				<< ", and " << materialCount << " material" << (materialCount == 1u ? "" : "s") << ".";
			return message.str();
		}

		std::string BuildIndexedStage(std::string_view itemName, std::size_t currentIndex, std::size_t totalCount, std::string_view stage)
		{
			std::ostringstream label;
			label << itemName << " " << currentIndex << "/" << std::max<std::size_t>(totalCount, 1u);
			if (!stage.empty())
			{
				label << " - " << stage;
			}
			return label.str();
		}

		std::string BuildIndexedMessage(std::string_view itemName, std::size_t currentIndex, std::size_t totalCount, std::string_view displayName, std::string_view message)
		{
			std::ostringstream text;
			text << itemName << " " << currentIndex << " of " << std::max<std::size_t>(totalCount, 1u);
			if (!displayName.empty())
			{
				text << " (" << displayName << ")";
			}
			if (!message.empty())
			{
				text << ": " << message;
			}
			return text.str();
		}

		auto MakeNestedProgress(const GltfPackageImportOptions& options, float start, float end)
		{
			return [&options, start, end](float progress, std::string_view stage, std::string_view message) {
				ReportProgress(options, start + (end - start) * std::clamp(progress, 0.0f, 1.0f), stage, message);
			};
		}

		auto MakeIndexedNestedProgress(const GltfPackageImportOptions& options, float start, float end, std::size_t currentIndex, std::size_t totalCount, std::string displayName)
		{
			return [&options, start, end, currentIndex, totalCount, displayName = std::move(displayName)](float progress, std::string_view stage, std::string_view message) {
				ReportProgress(
					options,
					start + (end - start) * std::clamp(progress, 0.0f, 1.0f),
					BuildIndexedStage("Texture", currentIndex, totalCount, stage),
					BuildIndexedMessage("Texture", currentIndex, totalCount, displayName, message));
			};
		}

		void PromoteStatusFromDiagnostics(GltfImportResult& result)
		{
			bool hasError = false;
			bool hasWarning = false;
			for (const GltfImportDiagnostic& diagnostic : result.diagnostics)
			{
				hasError = hasError || diagnostic.severity == GltfDiagnosticSeverity::Error;
				hasWarning = hasWarning || diagnostic.severity == GltfDiagnosticSeverity::Warning;
			}

			if (hasError || result.generatedMeshes.empty())
			{
				result.status = GltfImportStatus::Failed;
			}
			else if (hasWarning)
			{
				result.status = GltfImportStatus::SucceededWithWarnings;
			}
			else
			{
				result.status = GltfImportStatus::Succeeded;
			}
		}

		TextureSemantic ToMaterialSemantic(GltfTextureUsage usage)
		{
			switch (usage)
			{
			case GltfTextureUsage::BaseColor: return TextureSemantic::Base;
			case GltfTextureUsage::Normal: return TextureSemantic::Normal;
			case GltfTextureUsage::MetallicRoughness: return TextureSemantic::MetallicRoughness;
			case GltfTextureUsage::Occlusion: return TextureSemantic::Occlusion;
			case GltfTextureUsage::Emissive: return TextureSemantic::Emissive;
			case GltfTextureUsage::Unknown: return TextureSemantic::Base;
			}
			return TextureSemantic::Base;
		}

		GltfTextureSemantic ToTextureSemantic(GltfTextureUsage usage)
		{
			return (usage == GltfTextureUsage::BaseColor || usage == GltfTextureUsage::Emissive)
				? GltfTextureSemantic::Color
				: GltfTextureSemantic::Data;
		}

		TextureFileSemantic ToTextureFileSemantic(GltfTextureUsage usage)
		{
			return (usage == GltfTextureUsage::BaseColor || usage == GltfTextureUsage::Emissive)
				? TextureFileSemantic::Color
				: TextureFileSemantic::Data;
		}

		std::string ExtensionForMimeType(fastgltf::MimeType mimeType)
		{
			switch (mimeType)
			{
			case fastgltf::MimeType::JPEG: return ".jpg";
			case fastgltf::MimeType::PNG: return ".png";
			case fastgltf::MimeType::WEBP: return ".webp";
			case fastgltf::MimeType::KTX2: return ".ktx2";
			case fastgltf::MimeType::DDS: return ".dds";
			case fastgltf::MimeType::GltfBuffer:
			case fastgltf::MimeType::OctetStream:
			case fastgltf::MimeType::None:
				return ".png";
			}
			return ".png";
		}

		std::vector<std::uint8_t> BytesFromSpan(fastgltf::span<const std::byte> bytes)
		{
			std::vector<std::uint8_t> result(bytes.size());
			for (std::size_t index = 0; index < bytes.size(); ++index)
			{
				result[index] = static_cast<std::uint8_t>(bytes[index]);
			}
			return result;
		}

		bool ExtractImageBytes(
			const fastgltf::Asset& asset,
			const fastgltf::Image& image,
			const std::filesystem::path& packageDirectory,
			std::vector<std::uint8_t>& outBytes,
			std::string& outExtension)
		{
			outBytes.clear();
			outExtension = ".png";

			return std::visit(fastgltf::visitor {
				[&](const fastgltf::sources::Array& array) {
					outExtension = ExtensionForMimeType(array.mimeType);
					outBytes.resize(array.bytes.size());
					for (std::size_t index = 0; index < array.bytes.size(); ++index)
					{
						outBytes[index] = static_cast<std::uint8_t>(array.bytes[index]);
					}
					return true;
				},
				[&](const fastgltf::sources::Vector& vector) {
					outExtension = ExtensionForMimeType(vector.mimeType);
					outBytes.resize(vector.bytes.size());
					for (std::size_t index = 0; index < vector.bytes.size(); ++index)
					{
						outBytes[index] = static_cast<std::uint8_t>(vector.bytes[index]);
					}
					return true;
				},
				[&](const fastgltf::sources::ByteView& view) {
					outExtension = ExtensionForMimeType(view.mimeType);
					outBytes = BytesFromSpan(view.bytes);
					return true;
				},
				[&](const fastgltf::sources::BufferView& view) {
					fastgltf::DefaultBufferDataAdapter adapter;
					outExtension = ExtensionForMimeType(view.mimeType);
					outBytes = BytesFromSpan(adapter(asset, view.bufferViewIndex));
					return true;
				},
				[&](const fastgltf::sources::URI& uri) {
					outExtension = ExtensionForMimeType(uri.mimeType);
					if (uri.uri.isLocalPath())
					{
						const std::filesystem::path imagePath = packageDirectory / uri.uri.fspath();
						std::ifstream in(imagePath, std::ios::binary);
						if (!in)
						{
							return false;
						}
						outExtension = imagePath.extension().string().empty() ? outExtension : imagePath.extension().string();
						outBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
						return !outBytes.empty();
					}
					return false;
				},
				[&](const auto&) {
					return false;
				}
			}, image.data);
		}

		std::optional<std::size_t> ResolveImageIndex(const fastgltf::Asset& asset, std::size_t textureIndex)
		{
			if (textureIndex >= asset.textures.size())
			{
				return std::nullopt;
			}

			const fastgltf::Texture& texture = asset.textures[textureIndex];
			if (texture.imageIndex.has_value())
			{
				return texture.imageIndex.value();
			}
			if (texture.basisuImageIndex.has_value())
			{
				return texture.basisuImageIndex.value();
			}
			if (texture.webpImageIndex.has_value())
			{
				return texture.webpImageIndex.value();
			}
			return std::nullopt;
		}

		std::filesystem::path ImageSourcePath(const fastgltf::Image& image)
		{
			if (const auto* uri = std::get_if<fastgltf::sources::URI>(&image.data))
			{
				if (uri->uri.isLocalPath())
				{
					return uri->uri.fspath();
				}
			}
			return {};
		}

		void AddTextureRecord(
			const fastgltf::Asset& asset,
			GltfImportResult& result,
			std::size_t textureIndex,
			GltfTextureUsage usage,
			const TerraEngine::UUID& packageGuid,
			const std::filesystem::path& packageFolderRelative,
			const std::filesystem::path& importRoot)
		{
			const std::optional<std::size_t> imageIndex = ResolveImageIndex(asset, textureIndex);
			GeneratedGltfTextureAsset texture{};
			texture.guid = StableChildUuid(packageGuid, "gltf-texture", textureIndex);
			texture.sourceTextureIndex = textureIndex;
			texture.sourceImageIndex = imageIndex;
			texture.usage = usage;
			texture.semantic = ToTextureSemantic(usage);
			texture.displayName = "Texture_" + std::to_string(textureIndex);

			if (imageIndex.has_value() && *imageIndex < asset.images.size())
			{
				const fastgltf::Image& image = asset.images[*imageIndex];
				texture.displayName = SanitizeAssetName(std::string(image.name), texture.displayName);
				const std::filesystem::path imagePath = ImageSourcePath(image);
				if (!imagePath.empty())
				{
					texture.sourcePath = packageFolderRelative / imagePath;
				}
			}
			if (texture.sourcePath.empty())
			{
				texture.sourcePath = importRoot / (texture.displayName + ".png");
			}
			texture.compiledPath = importRoot / (texture.displayName + ".tetf");

			result.generatedTextures.push_back(std::move(texture));
		}

		const GeneratedGltfTextureAsset* FindTextureBySourceTexture(const GltfImportResult& result, std::size_t textureIndex)
		{
			for (const GeneratedGltfTextureAsset& texture : result.generatedTextures)
			{
				if (texture.sourceTextureIndex.has_value() && *texture.sourceTextureIndex == textureIndex)
				{
					return &texture;
				}
			}
			return nullptr;
		}

		void AddMaterialTextureBinding(
			const GltfImportResult& result,
			MaterialAssetWriteDesc& desc,
			GeneratedGltfMaterialAsset& material,
			std::size_t textureIndex,
			GltfTextureUsage usage)
		{
			const GeneratedGltfTextureAsset* texture = FindTextureBySourceTexture(result, textureIndex);
			if (texture == nullptr)
			{
				return;
			}

			TextureBinding binding{};
			binding.semantic = ToMaterialSemantic(usage);
			binding.textureUuid = texture->guid;
			binding.samplerFilter = SamplerFilter::Trilinear;
			binding.addressU = SamplerAddressMode::Repeat;
			binding.addressV = SamplerAddressMode::Repeat;
			binding.addressW = SamplerAddressMode::Repeat;
			binding.uvSetIndex = 0;
			binding.uvScale[0] = 1.0f;
			binding.uvScale[1] = 1.0f;
			binding.uvOffset[0] = 0.0f;
			binding.uvOffset[1] = 0.0f;
			desc.textureBindings.push_back(binding);
			material.textureGuids.push_back(texture->guid);
		}

		bool ExtractPrimitiveGeometry(
			const fastgltf::Asset& asset,
			const fastgltf::Primitive& primitive,
			std::vector<RawMeshVertex>& outVertices,
			std::vector<std::uint32_t>& outIndices)
		{
			outVertices.clear();
			outIndices.clear();

			if (primitive.type != fastgltf::PrimitiveType::Triangles)
			{
				return false;
			}

			const auto positionIt = primitive.findAttribute("POSITION");
			if (positionIt == primitive.attributes.end() || !primitive.indicesAccessor.has_value())
			{
				return false;
			}

			const fastgltf::Accessor& positionAccessor = asset.accessors[positionIt->accessorIndex];
			outVertices.resize(positionAccessor.count);
			fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, positionAccessor, [&](fastgltf::math::fvec3 position, std::size_t index) {
				outVertices[index].position = {
					static_cast<float>(position.x()),
					static_cast<float>(position.y()),
					static_cast<float>(position.z())
				};
			});

			if (const auto normalIt = primitive.findAttribute("NORMAL"); normalIt != primitive.attributes.end())
			{
				const fastgltf::Accessor& normalAccessor = asset.accessors[normalIt->accessorIndex];
				fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, normalAccessor, [&](fastgltf::math::fvec3 normal, std::size_t index) {
					if (index < outVertices.size())
					{
						outVertices[index].normal = {
							static_cast<float>(normal.x()),
							static_cast<float>(normal.y()),
							static_cast<float>(normal.z())
						};
					}
				});
			}

			if (const auto texcoordIt = primitive.findAttribute("TEXCOORD_0"); texcoordIt != primitive.attributes.end())
			{
				const fastgltf::Accessor& texcoordAccessor = asset.accessors[texcoordIt->accessorIndex];
				fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(asset, texcoordAccessor, [&](fastgltf::math::fvec2 uv, std::size_t index) {
					if (index < outVertices.size())
					{
						outVertices[index].uv = {
							static_cast<float>(uv.x()),
							static_cast<float>(uv.y())
						};
					}
				});
			}

			const fastgltf::Accessor& indexAccessor = asset.accessors[*primitive.indicesAccessor];
			outIndices.resize(indexAccessor.count);
			fastgltf::copyFromAccessor<std::uint32_t>(asset, indexAccessor, outIndices.data());
			return !outVertices.empty() && !outIndices.empty() && (outIndices.size() % 3u) == 0u;
		}

		bool MeshRequiresDeformableCook(const fastgltf::Asset& asset, std::size_t meshIndex)
		{
			if (meshIndex >= asset.meshes.size()) return false;
			for (const fastgltf::Node& node : asset.nodes)
			{
				if (node.meshIndex.has_value() && *node.meshIndex == meshIndex && node.skinIndex.has_value())
				{
					return true;
				}
			}
			for (const fastgltf::Primitive& primitive : asset.meshes[meshIndex].primitives)
			{
				if (!primitive.targets.empty()) return true;
				for (const fastgltf::Attribute& attribute : primitive.attributes)
				{
					if (std::string_view(attribute.name).starts_with("JOINTS_")
						|| std::string_view(attribute.name).starts_with("WEIGHTS_"))
					{
						return true;
					}
				}
			}
			return false;
		}

		std::string CompactPrivateTransactionToken(const TerraEngine::UUID& packageGuid)
		{
			// Hash every identity byte rather than truncating the GUID. The fixed-width
			// 64-bit result keeps private paths short while retaining deterministic
			// separation between package generations that differ in any UUID byte.
			constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
			constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
			std::uint64_t hash = kFnvOffset;
			for(const std::uint8_t byte : packageGuid)
			{
				hash ^= byte;
				hash *= kFnvPrime;
			}
			std::array<char, 16U> digits{};
			std::array<char, 16U> encoded{};
			const auto conversion = std::to_chars(encoded.data(), encoded.data() + encoded.size(), hash, 16);
			const std::size_t count = static_cast<std::size_t>(conversion.ptr - encoded.data());
			std::fill(digits.begin(), digits.end(), '0');
			std::copy(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(count),
				digits.end() - static_cast<std::ptrdiff_t>(count));
			return std::string(digits.begin(), digits.end());
		}

		std::filesystem::path StaticDeformableCandidatePath(
			const GltfPackageImportOptions& options,
			const std::filesystem::path& compiledPath)
		{
			const std::filesystem::path finalPath = options.buildRoot / compiledPath;
			// This filename remains private until the complete deformable package is
			// ready to publish. Keep its uniqueness token compact because the outer
			// model-package transaction already adds a private directory component;
			// spelling out a second 32-digit GUID can cross the legacy Windows path
			// limit in otherwise ordinary nested projects.
			const std::string packageToken = CompactPrivateTransactionToken(options.packageGuid);
			return finalPath.parent_path() / (finalPath.filename().string()
				+ ".sc-" + packageToken);
		}

		void ExpandBounds(GeneratedGltfMeshAsset& mesh, const std::vector<RawMeshVertex>& vertices, bool& initialized)
		{
			for (const RawMeshVertex& vertex : vertices)
			{
				if (!initialized)
				{
					mesh.localBoundsMin = vertex.position;
					mesh.localBoundsMax = vertex.position;
					initialized = true;
					continue;
				}
				for (std::size_t component = 0; component < 3u; ++component)
				{
					mesh.localBoundsMin[component] = std::min(mesh.localBoundsMin[component], vertex.position[component]);
					mesh.localBoundsMax[component] = std::max(mesh.localBoundsMax[component], vertex.position[component]);
				}
			}
		}

		// Animation child identities are derived from semantic source paths rather
		// than glTF array positions. Reordering unrelated source records therefore
		// cannot silently change a joint, socket, morph, skeleton, or clip identity.
		std::uint64_t StableSemanticHash(std::string_view value, std::uint64_t seed = 1469598103934665603ULL)
		{
			constexpr std::uint64_t prime = 1099511628211ULL;
			std::uint64_t hash = seed;
			for (const char character : value)
			{
				hash ^= static_cast<std::uint8_t>(character);
				hash *= prime;
			}
			return hash == 0U ? 1U : hash;
		}

		void ExtendSemanticHash(std::uint64_t& hash, std::span<const std::byte> bytes)
		{
			constexpr std::uint64_t prime = 1099511628211ULL;
			for (const std::byte byte : bytes)
			{
				hash ^= std::to_integer<std::uint8_t>(byte);
				hash *= prime;
			}
			if (hash == 0U)
			{
				hash = 1U;
			}
		}

		template<typename Value>
		void ExtendSemanticHash(std::uint64_t& hash, const Value& value)
		{
			ExtendSemanticHash(hash, std::as_bytes(std::span<const Value>(&value, 1U)));
		}

		std::string HexHash(std::uint64_t value)
		{
			std::ostringstream stream;
			stream << std::hex << value;
			return stream.str();
		}

		std::string NormalizeSemanticToken(std::string_view source, std::string_view fallback)
		{
			std::string result;
			result.reserve(source.size());
			bool priorSeparator = false;
			for (const char sourceCharacter : source)
			{
				const unsigned char character = static_cast<unsigned char>(sourceCharacter);
				if (std::isalnum(character))
				{
					result.push_back(static_cast<char>(std::tolower(character)));
					priorSeparator = false;
				}
				else if (!priorSeparator && !result.empty())
				{
					result.push_back('_');
					priorSeparator = true;
				}
			}
			while (!result.empty() && result.back() == '_')
			{
				result.pop_back();
			}
			return result.empty() ? std::string(fallback) : result;
		}

		struct ParsedGltfAnimationEvent
		{
			double timeSeconds = 0.0;
			std::string name;
			std::string payload;
			Animation::AnimationEventPayloadType payloadType = Animation::AnimationEventPayloadType::None;
		};

		struct ParsedGltfMetadata
		{
			GltfCoordinateConversion coordinateConversion;
			std::vector<std::string> jointRoles;
			std::vector<std::string> socketRoles;
			std::vector<std::vector<std::vector<std::string>>> morphTargetNames;
			std::vector<std::vector<ParsedGltfAnimationEvent>> animationEvents;
		};

		Animation::SkeletonSourceAxis ParseSourceAxis(
			std::string_view value,
			Animation::SkeletonSourceAxis fallback)
		{
			std::string normalized;
			for (const char character : value)
			{
				if (!std::isspace(static_cast<unsigned char>(character)))
				{
					normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
				}
			}
			if (normalized == "X" || normalized == "+X") return Animation::SkeletonSourceAxis::PositiveX;
			if (normalized == "Y" || normalized == "+Y") return Animation::SkeletonSourceAxis::PositiveY;
			if (normalized == "Z" || normalized == "+Z") return Animation::SkeletonSourceAxis::PositiveZ;
			if (normalized == "-X") return Animation::SkeletonSourceAxis::NegativeX;
			if (normalized == "-Y") return Animation::SkeletonSourceAxis::NegativeY;
			if (normalized == "-Z") return Animation::SkeletonSourceAxis::NegativeZ;
			return fallback;
		}

		ParsedGltfMetadata ReadGltfMetadata(const std::filesystem::path& sourcePath)
		{
			ParsedGltfMetadata metadata;
			if (NormalizeExtension(sourcePath) != ".gltf")
			{
				return metadata;
			}

			try
			{
				std::ifstream input(sourcePath);
				const nlohmann::json document = nlohmann::json::parse(input);
				const nlohmann::json& assetExtras = document.value("asset", nlohmann::json::object()).value("extras", nlohmann::json::object());
				metadata.coordinateConversion.sourceUnitScaleMeters = assetExtras.value("sourceUnitScale", 1.0F);
				metadata.coordinateConversion.sourceUpAxis = ParseSourceAxis(
					assetExtras.value("sourceUpAxis", std::string("Y")), Animation::SkeletonSourceAxis::PositiveY);
				metadata.coordinateConversion.sourceForwardAxis = ParseSourceAxis(
					assetExtras.value("sourceForwardAxis", std::string("Z")), Animation::SkeletonSourceAxis::PositiveZ);
				const std::string handedness = assetExtras.value("sourceHandedness", std::string("Right"));
				metadata.coordinateConversion.sourceHandedness = NormalizeSemanticToken(handedness, "right") == "left"
					? GltfSourceHandedness::Left
					: GltfSourceHandedness::Right;

				const nlohmann::json& nodes = document.value("nodes", nlohmann::json::array());
				metadata.jointRoles.resize(nodes.size());
				metadata.socketRoles.resize(nodes.size());
				for (std::size_t nodeIndex = 0U; nodeIndex < nodes.size(); ++nodeIndex)
				{
					const nlohmann::json& extras = nodes[nodeIndex].value("extras", nlohmann::json::object());
					if (extras.contains("terraEngineSemantic") && extras["terraEngineSemantic"].is_string())
					{
						metadata.jointRoles[nodeIndex] = extras["terraEngineSemantic"].get<std::string>();
					}
					if (extras.contains("terraEngineSocket") && extras["terraEngineSocket"].is_string())
					{
						metadata.socketRoles[nodeIndex] = extras["terraEngineSocket"].get<std::string>();
					}
				}

				const nlohmann::json& meshes = document.value("meshes", nlohmann::json::array());
				metadata.morphTargetNames.resize(meshes.size());
				for (std::size_t meshIndex = 0U; meshIndex < meshes.size(); ++meshIndex)
				{
					const nlohmann::json& primitives = meshes[meshIndex].value("primitives", nlohmann::json::array());
					metadata.morphTargetNames[meshIndex].resize(primitives.size());
					const nlohmann::json& targetNames = meshes[meshIndex].value("extras", nlohmann::json::object()).value("targetNames", nlohmann::json::array());
					for (std::size_t primitiveIndex = 0U; primitiveIndex < primitives.size(); ++primitiveIndex)
					{
						const std::size_t targetCount = primitives[primitiveIndex].value("targets", nlohmann::json::array()).size();
						metadata.morphTargetNames[meshIndex][primitiveIndex].resize(targetCount);
						for (std::size_t targetIndex = 0U; targetIndex < targetCount; ++targetIndex)
						{
							if (targetIndex < targetNames.size() && targetNames[targetIndex].is_string())
							{
								metadata.morphTargetNames[meshIndex][primitiveIndex][targetIndex] = targetNames[targetIndex].get<std::string>();
							}
						}
					}
				}

				const nlohmann::json& animations = document.value("animations", nlohmann::json::array());
				metadata.animationEvents.resize(animations.size());
				for (std::size_t animationIndex = 0U; animationIndex < animations.size(); ++animationIndex)
				{
					const nlohmann::json& events = animations[animationIndex].value("extras", nlohmann::json::object()).value("events", nlohmann::json::array());
					for (const nlohmann::json& event : events)
					{
						if (!event.is_object() || !event.contains("time") || !event["time"].is_number())
						{
							continue;
						}
						ParsedGltfAnimationEvent parsed;
						parsed.timeSeconds = event["time"].get<double>();
						parsed.name = event.value("name", std::string("Event"));
						if (event.contains("payload"))
						{
							const nlohmann::json& payload = event["payload"];
							if (payload.is_string())
							{
								parsed.payloadType = Animation::AnimationEventPayloadType::String;
								parsed.payload = payload.get<std::string>();
							}
							else if (payload.is_boolean())
							{
								parsed.payloadType = Animation::AnimationEventPayloadType::Boolean;
								parsed.payload = payload.get<bool>() ? "true" : "false";
							}
							else if (payload.is_number_integer())
							{
								parsed.payloadType = Animation::AnimationEventPayloadType::Integer;
								parsed.payload = std::to_string(payload.get<std::int64_t>());
							}
							else if (payload.is_number())
							{
								parsed.payloadType = Animation::AnimationEventPayloadType::Float;
								parsed.payload = payload.dump();
							}
						}
						metadata.animationEvents[animationIndex].push_back(std::move(parsed));
					}
				}
			}
			catch (const std::exception&)
			{
				// fastgltf owns source validation. Optional Terra extras are ignored when
				// the secondary metadata view cannot be read, keeping GLB/text parity.
			}
			return metadata;
		}

		Animation::AnimationFloat3 AxisVector(Animation::SkeletonSourceAxis axis)
		{
			switch (axis)
			{
			case Animation::SkeletonSourceAxis::PositiveX: return { 1.0F, 0.0F, 0.0F };
			case Animation::SkeletonSourceAxis::PositiveY: return { 0.0F, 1.0F, 0.0F };
			case Animation::SkeletonSourceAxis::PositiveZ: return { 0.0F, 0.0F, 1.0F };
			case Animation::SkeletonSourceAxis::NegativeX: return { -1.0F, 0.0F, 0.0F };
			case Animation::SkeletonSourceAxis::NegativeY: return { 0.0F, -1.0F, 0.0F };
			case Animation::SkeletonSourceAxis::NegativeZ: return { 0.0F, 0.0F, -1.0F };
			}
			return { 0.0F, 1.0F, 0.0F };
		}

		Animation::AnimationFloat3 Cross(Animation::AnimationFloat3 left, Animation::AnimationFloat3 right)
		{
			return {
				left.y * right.z - left.z * right.y,
				left.z * right.x - left.x * right.z,
				left.x * right.y - left.y * right.x
			};
		}

		float Dot(Animation::AnimationFloat3 left, Animation::AnimationFloat3 right)
		{
			return left.x * right.x + left.y * right.y + left.z * right.z;
		}

		struct GltfCoordinateConverter
		{
			float basis[3U][3U]{};
			float unitScaleMeters = 1.0F;

			explicit GltfCoordinateConverter(const GltfCoordinateConversion& conversion)
			{
				unitScaleMeters = std::isfinite(conversion.sourceUnitScaleMeters) && conversion.sourceUnitScaleMeters > 0.0F
					? conversion.sourceUnitScaleMeters
					: 1.0F;
				const Animation::AnimationFloat3 up = AxisVector(conversion.sourceUpAxis);
				const Animation::AnimationFloat3 forward = AxisVector(conversion.sourceForwardAxis);
				Animation::AnimationFloat3 right = Cross(up, forward);
				if (conversion.sourceHandedness == GltfSourceHandedness::Left)
				{
					right = { -right.x, -right.y, -right.z };
				}
				if (Dot(right, right) < 0.5F || std::abs(Dot(up, forward)) > 0.01F)
				{
					right = { 1.0F, 0.0F, 0.0F };
				}
				basis[0U][0U] = right.x; basis[0U][1U] = right.y; basis[0U][2U] = right.z;
				basis[1U][0U] = up.x; basis[1U][1U] = up.y; basis[1U][2U] = up.z;
				basis[2U][0U] = forward.x; basis[2U][1U] = forward.y; basis[2U][2U] = forward.z;
			}

			Animation::AnimationFloat3 Vector(Animation::AnimationFloat3 value, bool applyUnits) const
			{
				const float scale = applyUnits ? unitScaleMeters : 1.0F;
				return {
					scale * (basis[0U][0U] * value.x + basis[0U][1U] * value.y + basis[0U][2U] * value.z),
					scale * (basis[1U][0U] * value.x + basis[1U][1U] * value.y + basis[1U][2U] * value.z),
					scale * (basis[2U][0U] * value.x + basis[2U][1U] * value.y + basis[2U][2U] * value.z)
				};
			}

			Animation::AnimationQuaternion Rotation(Animation::AnimationQuaternion source) const
			{
				const float lengthSquared = source.x * source.x + source.y * source.y + source.z * source.z + source.w * source.w;
				if (!std::isfinite(lengthSquared) || lengthSquared <= std::numeric_limits<float>::epsilon())
				{
					return {};
				}
				const float reciprocal = 1.0F / std::sqrt(lengthSquared);
				source.x *= reciprocal; source.y *= reciprocal; source.z *= reciprocal; source.w *= reciprocal;
				const float x = source.x; const float y = source.y; const float z = source.z; const float w = source.w;
				const float sourceMatrix[3U][3U] = {
					{ 1.0F - 2.0F * (y * y + z * z), 2.0F * (x * y - z * w), 2.0F * (x * z + y * w) },
					{ 2.0F * (x * y + z * w), 1.0F - 2.0F * (x * x + z * z), 2.0F * (y * z - x * w) },
					{ 2.0F * (x * z - y * w), 2.0F * (y * z + x * w), 1.0F - 2.0F * (x * x + y * y) }
				};
				float converted[3U][3U]{};
				for (std::size_t row = 0U; row < 3U; ++row)
				{
					for (std::size_t column = 0U; column < 3U; ++column)
					{
						for (std::size_t left = 0U; left < 3U; ++left)
						{
							for (std::size_t right = 0U; right < 3U; ++right)
							{
								converted[row][column] += basis[row][left] * sourceMatrix[left][right] * basis[column][right];
							}
						}
					}
				}

				Animation::AnimationQuaternion result;
				const float trace = converted[0U][0U] + converted[1U][1U] + converted[2U][2U];
				if (trace > 0.0F)
				{
					const float scale = std::sqrt(trace + 1.0F) * 2.0F;
					result.w = 0.25F * scale;
					result.x = (converted[2U][1U] - converted[1U][2U]) / scale;
					result.y = (converted[0U][2U] - converted[2U][0U]) / scale;
					result.z = (converted[1U][0U] - converted[0U][1U]) / scale;
				}
				else if (converted[0U][0U] > converted[1U][1U] && converted[0U][0U] > converted[2U][2U])
				{
					const float scale = std::sqrt(1.0F + converted[0U][0U] - converted[1U][1U] - converted[2U][2U]) * 2.0F;
					result.w = (converted[2U][1U] - converted[1U][2U]) / scale;
					result.x = 0.25F * scale;
					result.y = (converted[0U][1U] + converted[1U][0U]) / scale;
					result.z = (converted[0U][2U] + converted[2U][0U]) / scale;
				}
				else if (converted[1U][1U] > converted[2U][2U])
				{
					const float scale = std::sqrt(1.0F + converted[1U][1U] - converted[0U][0U] - converted[2U][2U]) * 2.0F;
					result.w = (converted[0U][2U] - converted[2U][0U]) / scale;
					result.x = (converted[0U][1U] + converted[1U][0U]) / scale;
					result.y = 0.25F * scale;
					result.z = (converted[1U][2U] + converted[2U][1U]) / scale;
				}
				else
				{
					const float scale = std::sqrt(1.0F + converted[2U][2U] - converted[0U][0U] - converted[1U][1U]) * 2.0F;
					result.w = (converted[1U][0U] - converted[0U][1U]) / scale;
					result.x = (converted[0U][2U] + converted[2U][0U]) / scale;
					result.y = (converted[1U][2U] + converted[2U][1U]) / scale;
					result.z = 0.25F * scale;
				}
				const float resultLength = std::sqrt(result.x * result.x + result.y * result.y + result.z * result.z + result.w * result.w);
				if (resultLength > std::numeric_limits<float>::epsilon())
				{
					result.x /= resultLength; result.y /= resultLength; result.z /= resultLength; result.w /= resultLength;
				}
				return result;
			}

			Animation::AnimationTransform Transform(const fastgltf::TRS& source) const
			{
				Animation::AnimationTransform result;
				result.translation = Vector({ source.translation.x(), source.translation.y(), source.translation.z() }, true);
				result.rotation = Rotation({ source.rotation.x(), source.rotation.y(), source.rotation.z(), source.rotation.w() });
				const Animation::AnimationFloat3 convertedScale = Vector({ source.scale.x(), source.scale.y(), source.scale.z() }, false);
				result.scale = { std::abs(convertedScale.x), std::abs(convertedScale.y), std::abs(convertedScale.z) };
				return result;
			}
		};

		Animation::AnimationMatrix4x4 IdentityAnimationMatrix()
		{
			Animation::AnimationMatrix4x4 result;
			result.values[0U] = 1.0F;
			result.values[5U] = 1.0F;
			result.values[10U] = 1.0F;
			result.values[15U] = 1.0F;
			return result;
		}

		Animation::AnimationMatrix4x4 ConvertMatrix(
			const fastgltf::math::fmat4x4& source,
			const GltfCoordinateConverter& converter)
		{
			float sourceRows[4U][4U]{};
			float conversion[4U][4U]{};
			float inverseConversion[4U][4U]{};
			for (std::size_t row = 0U; row < 4U; ++row)
			{
				for (std::size_t column = 0U; column < 4U; ++column)
				{
					sourceRows[row][column] = source[column][row];
				}
			}
			for (std::size_t row = 0U; row < 3U; ++row)
			{
				for (std::size_t column = 0U; column < 3U; ++column)
				{
					conversion[row][column] = converter.basis[row][column] * converter.unitScaleMeters;
					inverseConversion[row][column] = converter.basis[column][row] / converter.unitScaleMeters;
				}
			}
			conversion[3U][3U] = 1.0F;
			inverseConversion[3U][3U] = 1.0F;
			float intermediate[4U][4U]{};
			float converted[4U][4U]{};
			for (std::size_t row = 0U; row < 4U; ++row)
			{
				for (std::size_t column = 0U; column < 4U; ++column)
				{
					for (std::size_t index = 0U; index < 4U; ++index)
					{
						intermediate[row][column] += conversion[row][index] * sourceRows[index][column];
					}
				}
			}
			for (std::size_t row = 0U; row < 4U; ++row)
			{
				for (std::size_t column = 0U; column < 4U; ++column)
				{
					for (std::size_t index = 0U; index < 4U; ++index)
					{
						converted[row][column] += intermediate[row][index] * inverseConversion[index][column];
					}
				}
			}
			Animation::AnimationMatrix4x4 result;
			for (std::size_t row = 0U; row < 4U; ++row)
			{
				for (std::size_t column = 0U; column < 4U; ++column)
				{
					result.values[column * 4U + row] = converted[row][column];
				}
			}
			return result;
		}

		std::vector<std::optional<std::size_t>> BuildNodeParents(
			const fastgltf::Asset& asset,
			GltfImportResult& diagnostics)
		{
			std::vector<std::optional<std::size_t>> parents(asset.nodes.size());
			for (std::size_t parentIndex = 0U; parentIndex < asset.nodes.size(); ++parentIndex)
			{
				for (const std::size_t childIndex : asset.nodes[parentIndex].children)
				{
					if (childIndex >= asset.nodes.size())
					{
						AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "InvalidNodeChild", "nodes/" + std::to_string(parentIndex), "A node child index is outside the parsed node table.");
						continue;
					}
					if (parents[childIndex].has_value() && *parents[childIndex] != parentIndex)
					{
						AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "MultipleNodeParents", "nodes/" + std::to_string(childIndex), "A glTF node is referenced by more than one parent; a stable hierarchy cannot be published.");
						continue;
					}
					parents[childIndex] = parentIndex;
				}
			}
			return parents;
		}

		std::vector<GltfImportedNode> BuildImportedNodes(
			const fastgltf::Asset& asset,
			const ParsedGltfMetadata& metadata,
			const GltfCoordinateConverter& converter,
			const std::vector<std::optional<std::size_t>>& parents,
			GltfImportResult& diagnostics)
		{
			std::vector<std::string> tokens(asset.nodes.size());
			std::vector<std::uint64_t> signatures(asset.nodes.size());
			for (std::size_t nodeIndex = 0U; nodeIndex < asset.nodes.size(); ++nodeIndex)
			{
				const fastgltf::Node& node = asset.nodes[nodeIndex];
				tokens[nodeIndex] = NormalizeSemanticToken(node.name, "unnamed_node");
				std::uint64_t signature = StableSemanticHash(tokens[nodeIndex]);
				if (node.meshIndex.has_value()) ExtendSemanticHash(signature, *node.meshIndex);
				if (node.skinIndex.has_value()) ExtendSemanticHash(signature, *node.skinIndex);
				if (const auto* transform = std::get_if<fastgltf::TRS>(&node.transform))
				{
					for (const float component : {
						transform->translation.x(), transform->translation.y(), transform->translation.z(),
						transform->rotation.x(), transform->rotation.y(), transform->rotation.z(), transform->rotation.w(),
						transform->scale.x(), transform->scale.y(), transform->scale.z() })
					{
						ExtendSemanticHash(signature, component);
					}
				}
				signatures[nodeIndex] = signature;
			}

			std::map<std::pair<std::optional<std::size_t>, std::string>, std::vector<std::size_t>> siblingNames;
			for (std::size_t nodeIndex = 0U; nodeIndex < asset.nodes.size(); ++nodeIndex)
			{
				siblingNames[{ parents[nodeIndex], tokens[nodeIndex] }].push_back(nodeIndex);
			}
			for (const auto& [key, siblings] : siblingNames)
			{
				(void)key;
				if (siblings.size() <= 1U) continue;
				std::unordered_set<std::uint64_t> usedSignatures;
				for (const std::size_t nodeIndex : siblings)
				{
					if (!usedSignatures.insert(signatures[nodeIndex]).second)
					{
						ExtendSemanticHash(signatures[nodeIndex], nodeIndex);
						AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Warning, "AmbiguousNodeSemanticPath", "nodes/" + std::to_string(nodeIndex), "Identical sibling nodes required a source-index tie-breaker because their semantic content was indistinguishable.", "Give duplicate siblings distinct source names to preserve identity through arbitrary reordering.");
					}
					tokens[nodeIndex] += "_" + HexHash(signatures[nodeIndex]);
				}
			}

			std::vector<std::string> paths(asset.nodes.size());
			std::vector<std::uint8_t> state(asset.nodes.size(), 0U);
			std::function<std::string(std::size_t)> buildPath = [&](std::size_t nodeIndex) -> std::string {
				if (state[nodeIndex] == 2U) return paths[nodeIndex];
				if (state[nodeIndex] == 1U)
				{
					AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "NodeHierarchyCycle", "nodes/" + std::to_string(nodeIndex), "The glTF node hierarchy contains a cycle and cannot define a skeleton.");
					return "cycle/" + tokens[nodeIndex];
				}
				state[nodeIndex] = 1U;
				paths[nodeIndex] = parents[nodeIndex].has_value()
					? buildPath(*parents[nodeIndex]) + "/" + tokens[nodeIndex]
					: "root/" + tokens[nodeIndex];
				state[nodeIndex] = 2U;
				return paths[nodeIndex];
			};

			std::vector<GltfImportedNode> result(asset.nodes.size());
			for (std::size_t nodeIndex = 0U; nodeIndex < asset.nodes.size(); ++nodeIndex)
			{
				const fastgltf::Node& source = asset.nodes[nodeIndex];
				GltfImportedNode& target = result[nodeIndex];
				target.sourceNodeIndex = nodeIndex;
				target.parentNodeIndex = parents[nodeIndex];
				target.displayName = source.name.empty() ? "Node_" + HexHash(signatures[nodeIndex]) : std::string(source.name);
				target.normalizedSemanticPath = buildPath(nodeIndex);
				target.stableId = StableSemanticHash(target.normalizedSemanticPath);
				const std::string authoredRole = nodeIndex < metadata.jointRoles.size()
					? metadata.jointRoles[nodeIndex] : std::string{};
				target.jointRole = NormalizeSemanticToken(
					authoredRole.empty() ? target.displayName : authoredRole, "joint");
				if (const auto* transform = std::get_if<fastgltf::TRS>(&source.transform))
				{
					target.localTransform = converter.Transform(*transform);
				}
				const std::string explicitSocket = nodeIndex < metadata.socketRoles.size() ? metadata.socketRoles[nodeIndex] : std::string{};
				const std::string normalizedName = NormalizeSemanticToken(source.name, "node");
				target.namedSocket = !explicitSocket.empty() || normalizedName.find("socket") != std::string::npos;
				target.socketRole = !explicitSocket.empty() ? explicitSocket : (target.namedSocket ? target.displayName : std::string{});
			}
			return result;
		}

		using MorphTargetIdMap = std::map<std::pair<std::size_t, std::size_t>, std::vector<std::uint64_t>>;

		std::uint32_t StableMorphTargetId(std::string_view semanticKey)
		{
			const std::uint64_t hash = StableSemanticHash(semanticKey);
			const std::uint32_t folded = static_cast<std::uint32_t>(hash) ^ static_cast<std::uint32_t>(hash >> 32U);
			return folded == 0U ? 1U : folded;
		}

		std::uint64_t HashAccessorSemanticContent(const fastgltf::Asset& asset, std::size_t accessorIndex, std::uint64_t hash)
		{
			if (accessorIndex >= asset.accessors.size()) return hash;
			const fastgltf::Accessor& accessor = asset.accessors[accessorIndex];
			ExtendSemanticHash(hash, accessor.type);
			ExtendSemanticHash(hash, accessor.componentType);
			ExtendSemanticHash(hash, accessor.count);
			switch (accessor.type)
			{
			case fastgltf::AccessorType::Scalar:
				fastgltf::iterateAccessor<float>(asset, accessor, [&](float value) { ExtendSemanticHash(hash, value); });
				break;
			case fastgltf::AccessorType::Vec2:
				fastgltf::iterateAccessor<fastgltf::math::fvec2>(asset, accessor, [&](const auto& value) {
					ExtendSemanticHash(hash, value.x()); ExtendSemanticHash(hash, value.y());
				});
				break;
			case fastgltf::AccessorType::Vec3:
				fastgltf::iterateAccessor<fastgltf::math::fvec3>(asset, accessor, [&](const auto& value) {
					ExtendSemanticHash(hash, value.x()); ExtendSemanticHash(hash, value.y()); ExtendSemanticHash(hash, value.z());
				});
				break;
			case fastgltf::AccessorType::Vec4:
				fastgltf::iterateAccessor<fastgltf::math::fvec4>(asset, accessor, [&](const auto& value) {
					ExtendSemanticHash(hash, value.x()); ExtendSemanticHash(hash, value.y()); ExtendSemanticHash(hash, value.z()); ExtendSemanticHash(hash, value.w());
				});
				break;
			case fastgltf::AccessorType::Mat2:
			case fastgltf::AccessorType::Mat3:
			case fastgltf::AccessorType::Mat4:
			case fastgltf::AccessorType::Invalid:
				break;
			}
			return hash;
		}

		std::uint64_t PrimitiveSemanticHash(const fastgltf::Asset& asset, const fastgltf::Primitive& primitive)
		{
			std::vector<std::pair<std::string, std::size_t>> attributes;
			for (const fastgltf::Attribute& attribute : primitive.attributes)
			{
				attributes.emplace_back(std::string(attribute.name), attribute.accessorIndex);
			}
			std::sort(attributes.begin(), attributes.end());
			std::uint64_t hash = StableSemanticHash("primitive");
			for (const auto& [name, accessor] : attributes)
			{
				ExtendSemanticHash(hash, std::as_bytes(std::span(name.data(), name.size())));
				hash = HashAccessorSemanticContent(asset, accessor, hash);
			}
			if (primitive.indicesAccessor.has_value()) hash = HashAccessorSemanticContent(asset, *primitive.indicesAccessor, hash);
			return hash;
		}

		MorphTargetIdMap BuildImportedMorphTargets(
			const fastgltf::Asset& asset,
			const ParsedGltfMetadata& metadata,
			GltfAnimatedPackageData& animation,
			GltfImportResult& diagnostics)
		{
			MorphTargetIdMap targetIds;
			for (std::size_t meshIndex = 0U; meshIndex < asset.meshes.size(); ++meshIndex)
			{
				const fastgltf::Mesh& mesh = asset.meshes[meshIndex];
				const std::string meshToken = NormalizeSemanticToken(mesh.name, "unnamed_mesh");
				for (std::size_t primitiveIndex = 0U; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
				{
					const fastgltf::Primitive& primitive = mesh.primitives[primitiveIndex];
					const std::string primitiveToken = HexHash(PrimitiveSemanticHash(asset, primitive));
					for (std::size_t targetIndex = 0U; targetIndex < primitive.targets.size(); ++targetIndex)
					{
						std::uint64_t targetContentHash = StableSemanticHash("morph-target");
						std::vector<std::pair<std::string, std::size_t>> targetAttributes;
						for (const fastgltf::Attribute& attribute : primitive.targets[targetIndex])
						{
							targetAttributes.emplace_back(std::string(attribute.name), attribute.accessorIndex);
						}
						std::sort(targetAttributes.begin(), targetAttributes.end());
						for (const auto& [name, accessor] : targetAttributes)
						{
							ExtendSemanticHash(targetContentHash, std::as_bytes(std::span(name.data(), name.size())));
							targetContentHash = HashAccessorSemanticContent(asset, accessor, targetContentHash);
						}
						std::string displayName;
						if (meshIndex < metadata.morphTargetNames.size()
							&& primitiveIndex < metadata.morphTargetNames[meshIndex].size()
							&& targetIndex < metadata.morphTargetNames[meshIndex][primitiveIndex].size())
						{
							displayName = metadata.morphTargetNames[meshIndex][primitiveIndex][targetIndex];
						}
						if (displayName.empty())
						{
							displayName = "Morph_" + HexHash(targetContentHash);
							AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Info, "UnnamedMorphTarget", "meshes/" + std::to_string(meshIndex), "An unnamed morph target received a deterministic structural label.", "Author extras.targetNames to keep a human-readable identity across structural edits.");
						}
						const std::string semanticKey = "mesh/" + meshToken
							+ "/primitive/" + primitiveToken
							+ "/morph/" + NormalizeSemanticToken(displayName, "morph")
							+ "/" + HexHash(targetContentHash);
						GltfImportedMorphTarget target;
						target.sourceMeshIndex = meshIndex;
						target.sourcePrimitiveIndex = primitiveIndex;
						target.sourceTargetIndex = targetIndex;
						target.stableId = StableMorphTargetId(semanticKey);
						target.displayName = displayName;
						target.semanticKey = semanticKey;
						target.defaultWeight = targetIndex < mesh.weights.size()
							? static_cast<float>(mesh.weights[targetIndex])
							: 0.0F;
						targetIds[{ meshIndex, targetIndex }].push_back(target.stableId);
						animation.morphTargets.push_back(std::move(target));
					}
				}
			}
			return targetIds;
		}

		struct GltfSkeletonContext
		{
			std::size_t generatedSkeletonIndex = 0U;
			std::unordered_set<std::size_t> sourceNodes;
			std::unordered_map<std::size_t, std::uint32_t> sourceNodeToCookedJoint;
			std::optional<std::size_t> sourceRootNode;
			std::string semanticKey;
		};

		std::unordered_map<std::size_t, Animation::AnimationMatrix4x4> ReadInverseBindMatrices(
			const fastgltf::Asset& asset,
			const fastgltf::Skin& skin,
			const GltfCoordinateConverter& converter,
			GltfImportResult& diagnostics,
			std::size_t skinIndex)
		{
			std::unordered_map<std::size_t, Animation::AnimationMatrix4x4> matrices;
			if (!skin.inverseBindMatrices.has_value() || *skin.inverseBindMatrices >= asset.accessors.size())
			{
				AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Warning, "MissingInverseBindMatrices", "skins/" + std::to_string(skinIndex), "The skin has no valid inverse-bind accessor; identity matrices were retained as an explicit fallback.", "Re-export inverse bind matrices for deformation-correct playback.");
				return matrices;
			}

			const fastgltf::Accessor& accessor = asset.accessors[*skin.inverseBindMatrices];
			if (accessor.type != fastgltf::AccessorType::Mat4 || accessor.count < skin.joints.size())
			{
				AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Warning, "InvalidInverseBindAccessor", "skins/" + std::to_string(skinIndex), "The inverse-bind accessor is not a MAT4 table large enough for every skin joint; identity fallback matrices were retained.", "Re-export the skin with one MAT4 inverse bind per joint.");
				return matrices;
			}

			fastgltf::iterateAccessorWithIndex<fastgltf::math::fmat4x4>(asset, accessor,
				[&](const fastgltf::math::fmat4x4& matrix, std::size_t matrixIndex) {
					if (matrixIndex < skin.joints.size())
					{
						matrices[skin.joints[matrixIndex]] = ConvertMatrix(matrix, converter);
					}
				});
			return matrices;
		}

		std::optional<GltfSkeletonContext> BuildSkeletonContext(
			const fastgltf::Asset& asset,
			const std::vector<GltfImportedNode>& nodes,
			const std::vector<std::optional<std::size_t>>& parents,
			const GltfCoordinateConversion& coordinateConversion,
			const GltfCoordinateConverter& converter,
			const GltfPackageImportOptions& options,
			std::optional<std::size_t> skinIndex,
			std::span<const std::size_t> seedNodes,
			std::string displayName,
			std::string semanticKey,
			GltfAnimatedPackageData& animation,
			GltfImportResult& diagnostics)
		{
			std::unordered_set<std::size_t> included;
			std::unordered_set<std::size_t> deforming;
			if (skinIndex.has_value())
			{
				if (*skinIndex >= asset.skins.size()) return std::nullopt;
				for (const std::size_t joint : asset.skins[*skinIndex].joints)
				{
					if (joint < nodes.size())
					{
						included.insert(joint);
						deforming.insert(joint);
					}
					else
					{
						AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "InvalidSkinJoint", "skins/" + std::to_string(*skinIndex), "A skin joint index is outside the parsed node table.");
					}
				}
				if (asset.skins[*skinIndex].skeleton.has_value() && *asset.skins[*skinIndex].skeleton < nodes.size())
				{
					included.insert(*asset.skins[*skinIndex].skeleton);
				}
			}
			for (const std::size_t nodeIndex : seedNodes)
			{
				if (nodeIndex < nodes.size()) included.insert(nodeIndex);
			}
			if (included.empty())
			{
				AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Warning, "EmptySkeleton", semanticKey, "A skin or animated hierarchy contained no valid source nodes and was not published.");
				return std::nullopt;
			}

			// Include every ancestor between a deforming/animated node and its scene
			// root. Helper joints keep inherited transforms intact and make the cooked
			// hierarchy independently evaluable without the source glTF at runtime.
			std::vector<std::size_t> closure(included.begin(), included.end());
			for (const std::size_t nodeIndex : closure)
			{
				std::optional<std::size_t> parent = parents[nodeIndex];
				std::size_t guard = 0U;
				while (parent.has_value() && guard++ <= nodes.size())
				{
					included.insert(*parent);
					parent = parents[*parent];
				}
			}

			std::vector<std::size_t> roots;
			for (const std::size_t nodeIndex : included)
			{
				if (!parents[nodeIndex].has_value() || !included.contains(*parents[nodeIndex]))
				{
					roots.push_back(nodeIndex);
				}
			}
			std::sort(roots.begin(), roots.end(), [&](std::size_t left, std::size_t right) {
				return nodes[left].normalizedSemanticPath < nodes[right].normalizedSemanticPath;
			});
			if (roots.empty())
			{
				AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "SkeletonHierarchyCycle", semanticKey, "The selected skeleton hierarchy has no acyclic root and was not published.");
				return std::nullopt;
			}

			std::vector<std::size_t> ordered;
			std::function<void(std::size_t)> appendSubtree = [&](std::size_t nodeIndex) {
				ordered.push_back(nodeIndex);
				std::vector<std::size_t> children;
				for (const std::size_t child : asset.nodes[nodeIndex].children)
				{
					if (included.contains(child)) children.push_back(child);
				}
				std::sort(children.begin(), children.end(), [&](std::size_t left, std::size_t right) {
					return nodes[left].normalizedSemanticPath < nodes[right].normalizedSemanticPath;
				});
				for (const std::size_t child : children) appendSubtree(child);
			};
			for (const std::size_t root : roots) appendSubtree(root);

			std::unordered_map<std::size_t, Animation::AnimationMatrix4x4> inverseBinds;
			if (skinIndex.has_value())
			{
				inverseBinds = ReadInverseBindMatrices(asset, asset.skins[*skinIndex], converter, diagnostics, *skinIndex);
			}

			GltfGeneratedSkeleton generated;
			generated.sourceSkinIndex = skinIndex.value_or(std::numeric_limits<std::size_t>::max());
			generated.displayName = displayName;
			generated.semanticKey = semanticKey;
			generated.guid = BuildStableAnimationSubAssetUuid(options.packageGuid, AnimationImportSubAssetKind::Skeleton, semanticKey);
			generated.sourcePath = options.sourceRelativePath;
			const std::string fileName = SanitizeAssetName(displayName, "Skeleton") + "_" + HexHash(StableSemanticHash(semanticKey)).substr(0U, 8U) + ".teskel";
			generated.compiledPath = std::filesystem::path("Animation") / fileName;
			generated.asset.assetId = { generated.guid };
			generated.asset.source.unitScaleMeters = coordinateConversion.sourceUnitScaleMeters;
			generated.asset.source.upAxis = coordinateConversion.sourceUpAxis;
			generated.asset.source.forwardAxis = coordinateConversion.sourceForwardAxis;
			generated.asset.source.sourcePackagePath = options.sourceRelativePath.generic_string();

			const bool needsSyntheticRoot = roots.size() > 1U;
			std::unordered_map<std::size_t, std::uint32_t> cookedIndices;
			if (needsSyntheticRoot)
			{
				Animation::SkeletonJoint synthetic;
				synthetic.stableId = StableSemanticHash(semanticKey + "/synthetic_root");
				synthetic.parentIndex = -1;
				synthetic.displayName = "ImportedSkeletonRoot";
				synthetic.normalizedSourcePath = semanticKey + "/synthetic_root";
				generated.asset.joints.push_back(std::move(synthetic));
				generated.asset.inverseBindMatrices.push_back(IdentityAnimationMatrix());
			}
			for (const std::size_t nodeIndex : ordered)
			{
				Animation::SkeletonJoint joint;
				joint.stableId = nodes[nodeIndex].stableId;
				joint.localBindTransform = nodes[nodeIndex].localTransform;
				joint.deformsMesh = deforming.contains(nodeIndex);
				joint.displayName = nodes[nodeIndex].displayName;
				joint.normalizedSourcePath = nodes[nodeIndex].normalizedSemanticPath;
				// Explicit author semantics survive compatible reimport independently of
				// source array order. A normalized display-name fallback still gives FBX-
				// and glTF-authored rigs a stable retarget role without using node indices.
				joint.semanticRole = nodes[nodeIndex].jointRole;
				if (parents[nodeIndex].has_value() && cookedIndices.contains(*parents[nodeIndex]))
				{
					joint.parentIndex = static_cast<std::int32_t>(cookedIndices[*parents[nodeIndex]]);
				}
				else
				{
					joint.parentIndex = needsSyntheticRoot ? 0 : -1;
				}
				cookedIndices[nodeIndex] = static_cast<std::uint32_t>(generated.asset.joints.size());
				generated.asset.joints.push_back(std::move(joint));
				const auto inverseBind = inverseBinds.find(nodeIndex);
				generated.asset.inverseBindMatrices.push_back(inverseBind != inverseBinds.end()
					? inverseBind->second
					: IdentityAnimationMatrix());
			}
			generated.asset.rootJointIndex = 0U;
			if (!needsSyntheticRoot && roots.size() == 1U)
			{
				generated.asset.rootJointIndex = cookedIndices[roots.front()];
			}

			std::unordered_set<std::string> socketRoles;
			for (std::size_t socketNodeIndex = 0U; socketNodeIndex < nodes.size(); ++socketNodeIndex)
			{
				if (!nodes[socketNodeIndex].namedSocket) continue;
				std::optional<std::size_t> jointNode = parents[socketNodeIndex];
				while (jointNode.has_value() && !cookedIndices.contains(*jointNode))
				{
					jointNode = parents[*jointNode];
				}
				if (!jointNode.has_value()) continue;
				Animation::SkeletonSocket socket;
				socket.stableId = nodes[socketNodeIndex].stableId;
				socket.jointStableId = nodes[*jointNode].stableId;
				socket.localTransform = nodes[socketNodeIndex].localTransform;
				socket.displayName = nodes[socketNodeIndex].displayName;
				socket.semanticRole = NormalizeSemanticToken(nodes[socketNodeIndex].socketRole, "socket");
				if (!socketRoles.insert(socket.semanticRole).second)
				{
					socket.semanticRole += "_" + HexHash(socket.stableId);
					socketRoles.insert(socket.semanticRole);
				}
				generated.asset.sockets.push_back(std::move(socket));
			}

			std::uint64_t compatibility = StableSemanticHash(semanticKey);
			std::uint64_t referencePose = StableSemanticHash("reference-pose/" + semanticKey);
			for (const Animation::SkeletonJoint& joint : generated.asset.joints)
			{
				ExtendSemanticHash(compatibility, joint.stableId);
				ExtendSemanticHash(compatibility, joint.parentIndex);
				const std::uint64_t semanticRoleHash = StableSemanticHash(joint.semanticRole);
				ExtendSemanticHash(compatibility, semanticRoleHash);
				ExtendSemanticHash(referencePose, joint.localBindTransform);
			}
			for (const Animation::SkeletonSocket& socket : generated.asset.sockets)
			{
				ExtendSemanticHash(compatibility, socket.stableId);
				ExtendSemanticHash(compatibility, socket.jointStableId);
				const std::uint64_t socketRoleHash = StableSemanticHash(socket.semanticRole);
				ExtendSemanticHash(compatibility, socketRoleHash);
			}
			for (const Animation::AnimationMatrix4x4& inverseBind : generated.asset.inverseBindMatrices)
			{
				ExtendSemanticHash(compatibility, inverseBind);
			}
			generated.asset.compatibilityFingerprint = compatibility;
			generated.asset.referencePoseHash = referencePose;

			GltfSkeletonContext context;
			context.generatedSkeletonIndex = animation.skeletons.size();
			context.sourceNodes = std::move(included);
			context.sourceNodeToCookedJoint = std::move(cookedIndices);
			context.sourceRootNode = !needsSyntheticRoot && roots.size() == 1U
				? std::optional<std::size_t>(roots.front())
				: std::nullopt;
			context.semanticKey = semanticKey;
			animation.skeletons.push_back(std::move(generated));
			return context;
		}

		Animation::AnimationInterpolation ToAnimationInterpolation(fastgltf::AnimationInterpolation interpolation)
		{
			switch (interpolation)
			{
			case fastgltf::AnimationInterpolation::Step: return Animation::AnimationInterpolation::Step;
			case fastgltf::AnimationInterpolation::Linear: return Animation::AnimationInterpolation::Linear;
			case fastgltf::AnimationInterpolation::CubicSpline: return Animation::AnimationInterpolation::Cubic;
			}
			return Animation::AnimationInterpolation::Linear;
		}

		bool ReadScalarAccessor(
			const fastgltf::Asset& asset,
			std::size_t accessorIndex,
			std::vector<float>& values)
		{
			values.clear();
			if (accessorIndex >= asset.accessors.size()) return false;
			const fastgltf::Accessor& accessor = asset.accessors[accessorIndex];
			if (accessor.type != fastgltf::AccessorType::Scalar) return false;
			values.resize(accessor.count);
			fastgltf::iterateAccessorWithIndex<float>(asset, accessor, [&](float value, std::size_t index) {
				values[index] = value;
			});
			return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
		}

		bool ReadVector3Accessor(
			const fastgltf::Asset& asset,
			std::size_t accessorIndex,
			std::vector<Animation::AnimationFloat3>& values)
		{
			values.clear();
			if (accessorIndex >= asset.accessors.size()) return false;
			const fastgltf::Accessor& accessor = asset.accessors[accessorIndex];
			if (accessor.type != fastgltf::AccessorType::Vec3) return false;
			values.resize(accessor.count);
			fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, accessor,
				[&](const fastgltf::math::fvec3& value, std::size_t index) {
					values[index] = { value.x(), value.y(), value.z() };
				});
			return std::all_of(values.begin(), values.end(), [](const auto& value) { return Animation::IsFinite(value); });
		}

		// glTF morph attributes are always FLOAT VEC3 deltas. Keeping this stricter
		// than generic vector readers prevents normalized integers or malformed counts
		// from silently changing deformation during the coordinate conversion.
		bool ReadMorphDeltaAccessor(
			const fastgltf::Asset& asset,
			std::size_t accessorIndex,
			std::size_t expectedCount,
			std::vector<Animation::AnimationFloat3>& values)
		{
			values.clear();
			if (accessorIndex >= asset.accessors.size()) return false;
			const fastgltf::Accessor& accessor = asset.accessors[accessorIndex];
			if (accessor.type != fastgltf::AccessorType::Vec3
				|| accessor.componentType != fastgltf::ComponentType::Float
				|| accessor.count != expectedCount)
			{
				return false;
			}
			values.resize(accessor.count);
			std::size_t decodedCount = 0U;
			bool indexInRange = true;
			fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, accessor,
				[&](const fastgltf::math::fvec3& value, std::size_t index) {
					if (index >= values.size())
					{
						indexInRange = false;
						return;
					}
					values[index] = { value.x(), value.y(), value.z() };
					++decodedCount;
				});
			return indexInRange && decodedCount == values.size()
				&& std::all_of(values.begin(), values.end(), [](const Animation::AnimationFloat3& value) {
				return Animation::IsFinite(value);
			});
		}

		bool ReadQuaternionAccessor(
			const fastgltf::Asset& asset,
			std::size_t accessorIndex,
			std::vector<Animation::AnimationQuaternion>& values)
		{
			values.clear();
			if (accessorIndex >= asset.accessors.size()) return false;
			const fastgltf::Accessor& accessor = asset.accessors[accessorIndex];
			if (accessor.type != fastgltf::AccessorType::Vec4) return false;
			values.resize(accessor.count);
			fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(asset, accessor,
				[&](const fastgltf::math::fvec4& value, std::size_t index) {
					values[index] = { value.x(), value.y(), value.z(), value.w() };
				});
			return std::all_of(values.begin(), values.end(), [](const auto& value) { return Animation::IsFinite(value); });
		}

		Animation::AnimationFixedTick SecondsToAnimationTicks(float seconds)
		{
			if (!std::isfinite(seconds) || seconds <= 0.0F) return 0;
			const double scaled = static_cast<double>(seconds) * static_cast<double>(Animation::kAnimationFixedTicksPerSecond);
			if (scaled >= static_cast<double>(std::numeric_limits<Animation::AnimationFixedTick>::max()))
			{
				return std::numeric_limits<Animation::AnimationFixedTick>::max();
			}
			return static_cast<Animation::AnimationFixedTick>(std::llround(scaled));
		}

		void AppendOrReplaceSourceKey(AnimationSourceTrack& track, AnimationSourceKey key)
		{
			if (!track.keys.empty() && track.keys.back().tick == key.tick)
			{
				// Fixed 60 Hz time is the engine-wide deterministic contract. When an
				// authored sample rate is denser, the later source sample deterministically
				// owns that tick instead of creating an invalid duplicate timestamp.
				track.keys.back() = key;
				return;
			}
			track.keys.push_back(key);
		}

		float QuaternionYaw(Animation::AnimationQuaternion rotation)
		{
			return std::atan2(
				2.0F * (rotation.w * rotation.y + rotation.x * rotation.z),
				1.0F - 2.0F * (rotation.y * rotation.y + rotation.z * rotation.z));
		}

		struct GltfClipBuildData
		{
			std::vector<AnimationSourceTrack> compilerTracks;
			std::vector<GltfImportedAnimationTrack> sourceTracks;
			Animation::AnimationFixedTick durationTicks = 0;
			std::set<std::pair<Animation::AnimationClipTrackKind, std::uint64_t>> uniqueTracks;
		};

		bool AddVectorAnimationChannel(
			const fastgltf::Asset& asset,
			const fastgltf::AnimationChannel& channel,
			const fastgltf::AnimationSampler& sampler,
			std::size_t channelIndex,
			std::span<const float> times,
			const std::vector<GltfImportedNode>& nodes,
			const GltfCoordinateConverter& converter,
			GltfClipBuildData& build)
		{
			std::vector<Animation::AnimationFloat3> values;
			if (!ReadVector3Accessor(asset, sampler.outputAccessor, values)) return false;
			const std::size_t multiplier = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline ? 3U : 1U;
			if (!channel.nodeIndex.has_value() || values.size() != times.size() * multiplier) return false;
			const std::size_t nodeIndex = *channel.nodeIndex;
			const Animation::AnimationClipTrackKind kind = channel.path == fastgltf::AnimationPath::Scale
				? Animation::AnimationClipTrackKind::Scale
				: Animation::AnimationClipTrackKind::Translation;
			const std::uint64_t targetId = nodes[nodeIndex].stableId;
			if (!build.uniqueTracks.emplace(kind, targetId).second) return false;

			GltfImportedAnimationTrack sourceTrack;
			sourceTrack.sourceChannelIndex = channelIndex;
			sourceTrack.sourceNodeIndex = nodeIndex;
			sourceTrack.stableTargetId = targetId;
			sourceTrack.kind = kind;
			sourceTrack.interpolation = ToAnimationInterpolation(sampler.interpolation);
			sourceTrack.componentCount = static_cast<std::uint32_t>(3U * multiplier);
			sourceTrack.timesSeconds.assign(times.begin(), times.end());
			sourceTrack.values.reserve(values.size() * 3U);
			for (const auto& value : values)
			{
				sourceTrack.values.push_back(value.x);
				sourceTrack.values.push_back(value.y);
				sourceTrack.values.push_back(value.z);
			}

			AnimationSourceTrack compilerTrack;
			compilerTrack.stableTargetId = targetId;
			compilerTrack.kind = kind;
			compilerTrack.interpolation = sourceTrack.interpolation;
			const std::size_t centerOffset = multiplier == 3U ? 1U : 0U;
			for (std::size_t keyIndex = 0U; keyIndex < times.size(); ++keyIndex)
			{
				const Animation::AnimationFixedTick tick = SecondsToAnimationTicks(times[keyIndex]);
				Animation::AnimationFloat3 value = values[keyIndex * multiplier + centerOffset];
				value = kind == Animation::AnimationClipTrackKind::Translation
					? converter.Vector(value, true)
					: converter.Vector(value, false);
				if (kind == Animation::AnimationClipTrackKind::Scale)
				{
					value = { std::abs(value.x), std::abs(value.y), std::abs(value.z) };
				}
				AnimationSourceKey key;
				key.tick = tick;
				key.vectorValue = value;
				AppendOrReplaceSourceKey(compilerTrack, key);
				build.durationTicks = std::max(build.durationTicks, tick);
			}
			build.sourceTracks.push_back(std::move(sourceTrack));
			build.compilerTracks.push_back(std::move(compilerTrack));
			return true;
		}

		bool AddRotationAnimationChannel(
			const fastgltf::Asset& asset,
			const fastgltf::AnimationChannel& channel,
			const fastgltf::AnimationSampler& sampler,
			std::size_t channelIndex,
			std::span<const float> times,
			const std::vector<GltfImportedNode>& nodes,
			const GltfCoordinateConverter& converter,
			GltfClipBuildData& build)
		{
			std::vector<Animation::AnimationQuaternion> values;
			if (!ReadQuaternionAccessor(asset, sampler.outputAccessor, values)) return false;
			const std::size_t multiplier = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline ? 3U : 1U;
			if (!channel.nodeIndex.has_value() || values.size() != times.size() * multiplier) return false;
			const std::size_t nodeIndex = *channel.nodeIndex;
			const std::uint64_t targetId = nodes[nodeIndex].stableId;
			if (!build.uniqueTracks.emplace(Animation::AnimationClipTrackKind::Rotation, targetId).second) return false;

			GltfImportedAnimationTrack sourceTrack;
			sourceTrack.sourceChannelIndex = channelIndex;
			sourceTrack.sourceNodeIndex = nodeIndex;
			sourceTrack.stableTargetId = targetId;
			sourceTrack.kind = Animation::AnimationClipTrackKind::Rotation;
			sourceTrack.interpolation = ToAnimationInterpolation(sampler.interpolation);
			sourceTrack.componentCount = static_cast<std::uint32_t>(4U * multiplier);
			sourceTrack.timesSeconds.assign(times.begin(), times.end());
			sourceTrack.values.reserve(values.size() * 4U);
			for (const auto& value : values)
			{
				sourceTrack.values.push_back(value.x);
				sourceTrack.values.push_back(value.y);
				sourceTrack.values.push_back(value.z);
				sourceTrack.values.push_back(value.w);
			}

			AnimationSourceTrack compilerTrack;
			compilerTrack.stableTargetId = targetId;
			compilerTrack.kind = Animation::AnimationClipTrackKind::Rotation;
			compilerTrack.interpolation = sourceTrack.interpolation;
			const std::size_t centerOffset = multiplier == 3U ? 1U : 0U;
			for (std::size_t keyIndex = 0U; keyIndex < times.size(); ++keyIndex)
			{
				const Animation::AnimationFixedTick tick = SecondsToAnimationTicks(times[keyIndex]);
				AnimationSourceKey key;
				key.tick = tick;
				key.rotationValue = converter.Rotation(values[keyIndex * multiplier + centerOffset]);
				AppendOrReplaceSourceKey(compilerTrack, key);
				build.durationTicks = std::max(build.durationTicks, tick);
			}
			build.sourceTracks.push_back(std::move(sourceTrack));
			build.compilerTracks.push_back(std::move(compilerTrack));
			return true;
		}

		bool AddMorphAnimationChannel(
			const fastgltf::Asset& asset,
			const fastgltf::AnimationChannel& channel,
			const fastgltf::AnimationSampler& sampler,
			std::size_t channelIndex,
			std::span<const float> times,
			const std::vector<GltfImportedNode>& nodes,
			const MorphTargetIdMap& morphTargetIds,
			GltfClipBuildData& build)
		{
			std::vector<float> values;
			if (!ReadScalarAccessor(asset, sampler.outputAccessor, values)
				|| !channel.nodeIndex.has_value() || !asset.nodes[*channel.nodeIndex].meshIndex.has_value())
			{
				return false;
			}
			const std::size_t nodeIndex = *channel.nodeIndex;
			const std::size_t meshIndex = *asset.nodes[nodeIndex].meshIndex;
			const std::size_t multiplier = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline ? 3U : 1U;
			if (times.empty() || values.size() % (times.size() * multiplier) != 0U) return false;
			const std::size_t morphCount = values.size() / (times.size() * multiplier);
			if (morphCount == 0U) return false;

			GltfImportedAnimationTrack sourceTrack;
			sourceTrack.sourceChannelIndex = channelIndex;
			sourceTrack.sourceNodeIndex = nodeIndex;
			sourceTrack.stableTargetId = nodes[nodeIndex].stableId;
			sourceTrack.kind = Animation::AnimationClipTrackKind::MorphWeight;
			sourceTrack.interpolation = ToAnimationInterpolation(sampler.interpolation);
			sourceTrack.componentCount = static_cast<std::uint32_t>(morphCount * multiplier);
			sourceTrack.timesSeconds.assign(times.begin(), times.end());
			sourceTrack.values = values;
			build.sourceTracks.push_back(std::move(sourceTrack));

			const std::size_t centerBlock = multiplier == 3U ? morphCount : 0U;
			for (std::size_t targetIndex = 0U; targetIndex < morphCount; ++targetIndex)
			{
				const auto ids = morphTargetIds.find({ meshIndex, targetIndex });
				if (ids == morphTargetIds.end()) continue;
				for (const std::uint64_t targetId : ids->second)
				{
					if (!build.uniqueTracks.emplace(Animation::AnimationClipTrackKind::MorphWeight, targetId).second) continue;
					AnimationSourceTrack compilerTrack;
					compilerTrack.stableTargetId = targetId;
					compilerTrack.kind = Animation::AnimationClipTrackKind::MorphWeight;
					compilerTrack.interpolation = ToAnimationInterpolation(sampler.interpolation);
					for (std::size_t keyIndex = 0U; keyIndex < times.size(); ++keyIndex)
					{
						const Animation::AnimationFixedTick tick = SecondsToAnimationTicks(times[keyIndex]);
						AnimationSourceKey key;
						key.tick = tick;
						key.scalarValue = values[keyIndex * morphCount * multiplier + centerBlock + targetIndex];
						AppendOrReplaceSourceKey(compilerTrack, key);
						build.durationTicks = std::max(build.durationTicks, tick);
					}
					build.compilerTracks.push_back(std::move(compilerTrack));
				}
			}
			return true;
		}

		void AddRootMotionTracks(GltfClipBuildData& build, std::uint64_t rootTargetId)
		{
			const auto translation = std::find_if(build.compilerTracks.begin(), build.compilerTracks.end(),
				[rootTargetId](const AnimationSourceTrack& track) {
					return track.stableTargetId == rootTargetId
						&& track.kind == Animation::AnimationClipTrackKind::Translation;
				});
			const auto rotation = std::find_if(build.compilerTracks.begin(), build.compilerTracks.end(),
				[rootTargetId](const AnimationSourceTrack& track) {
					return track.stableTargetId == rootTargetId
						&& track.kind == Animation::AnimationClipTrackKind::Rotation;
				});
			if (translation == build.compilerTracks.end() || rotation == build.compilerTracks.end())
			{
				return;
			}

			AnimationSourceTrack rootTranslation = *translation;
			rootTranslation.kind = Animation::AnimationClipTrackKind::RootTranslation;
			AnimationSourceTrack rootYaw;
			rootYaw.stableTargetId = rootTargetId;
			rootYaw.kind = Animation::AnimationClipTrackKind::RootYaw;
			rootYaw.interpolation = rotation->interpolation;
			for (const AnimationSourceKey& rotationKey : rotation->keys)
			{
				AnimationSourceKey yawKey;
				yawKey.tick = rotationKey.tick;
				yawKey.scalarValue = QuaternionYaw(rotationKey.rotationValue);
				rootYaw.keys.push_back(yawKey);
			}
			build.uniqueTracks.emplace(rootTranslation.kind, rootTargetId);
			build.uniqueTracks.emplace(rootYaw.kind, rootTargetId);
			build.compilerTracks.push_back(std::move(rootTranslation));
			build.compilerTracks.push_back(std::move(rootYaw));
		}

		std::string BuildAnimationSemanticKey(
			const fastgltf::Animation& animation,
			const std::vector<GltfImportedNode>& nodes,
			const GltfSkeletonContext& skeleton)
		{
			std::vector<std::string> channelKeys;
			for (const fastgltf::AnimationChannel& channel : animation.channels)
			{
				if (!channel.nodeIndex.has_value() || *channel.nodeIndex >= nodes.size()) continue;
				channelKeys.push_back(nodes[*channel.nodeIndex].normalizedSemanticPath
					+ "/" + std::to_string(static_cast<std::uint8_t>(channel.path)));
			}
			std::sort(channelKeys.begin(), channelKeys.end());
			std::string signature;
			for (const std::string& key : channelKeys) signature += key + ";";
			return "animation/" + NormalizeSemanticToken(animation.name, "unnamed_clip")
				+ "/" + HexHash(StableSemanticHash(signature))
				+ "/for/" + skeleton.semanticKey;
		}

		void BuildAnimationClips(
			const fastgltf::Asset& asset,
			const ParsedGltfMetadata& metadata,
			const GltfCoordinateConverter& converter,
			const GltfPackageImportOptions& options,
			const std::vector<GltfImportedNode>& nodes,
			const MorphTargetIdMap& morphTargetIds,
			const std::vector<GltfSkeletonContext>& skeletons,
			GltfAnimatedPackageData& animationData,
			GltfImportResult& diagnostics)
		{
			for (std::size_t animationIndex = 0U; animationIndex < asset.animations.size(); ++animationIndex)
			{
				const fastgltf::Animation& sourceAnimation = asset.animations[animationIndex];
				if (sourceAnimation.channels.empty())
				{
					AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Info, "AnimationHasNoChannels", "animations/" + std::to_string(animationIndex), "Animation metadata was preserved, but the source animation contains no channels to cook.");
					continue;
				}

				for (const GltfSkeletonContext& skeletonContext : skeletons)
				{
					GltfClipBuildData build;
					for (std::size_t channelIndex = 0U; channelIndex < sourceAnimation.channels.size(); ++channelIndex)
					{
						const fastgltf::AnimationChannel& channel = sourceAnimation.channels[channelIndex];
						if (!channel.nodeIndex.has_value() || *channel.nodeIndex >= nodes.size()
							|| !skeletonContext.sourceNodes.contains(*channel.nodeIndex))
						{
							continue;
						}
						if (channel.samplerIndex >= sourceAnimation.samplers.size())
						{
							AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Warning, "InvalidAnimationSampler", "animations/" + std::to_string(animationIndex) + "/channels/" + std::to_string(channelIndex), "An animation channel references a sampler outside its animation and was skipped.");
							continue;
						}
						const fastgltf::AnimationSampler& sampler = sourceAnimation.samplers[channel.samplerIndex];
						std::vector<float> times;
						if (!ReadScalarAccessor(asset, sampler.inputAccessor, times) || times.empty()
							|| times.front() < 0.0F
							|| !std::is_sorted(times.begin(), times.end(), std::less<float>{}))
						{
							AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Warning, "InvalidAnimationTimes", "animations/" + std::to_string(animationIndex) + "/channels/" + std::to_string(channelIndex), "An animation channel has missing, non-finite, negative, or unsorted timestamps and was skipped.");
							continue;
						}
						bool imported = false;
						switch (channel.path)
						{
						case fastgltf::AnimationPath::Translation:
						case fastgltf::AnimationPath::Scale:
							imported = AddVectorAnimationChannel(asset, channel, sampler, channelIndex, times, nodes, converter, build);
							break;
						case fastgltf::AnimationPath::Rotation:
							imported = AddRotationAnimationChannel(asset, channel, sampler, channelIndex, times, nodes, converter, build);
							break;
						case fastgltf::AnimationPath::Weights:
							imported = AddMorphAnimationChannel(asset, channel, sampler, channelIndex, times, nodes, morphTargetIds, build);
							break;
						}
						if (!imported)
						{
							AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Warning, "AnimationChannelRejected", "animations/" + std::to_string(animationIndex) + "/channels/" + std::to_string(channelIndex), "An animation output accessor, target, or duplicate channel is inconsistent with the glTF channel contract and was skipped.", "Re-export the channel with matching scalar/vector output counts and a valid target node.");
						}
					}

					if (build.compilerTracks.empty()) continue;
					const GltfGeneratedSkeleton& skeleton = animationData.skeletons[skeletonContext.generatedSkeletonIndex];
					if (skeletonContext.sourceRootNode.has_value())
					{
						AddRootMotionTracks(build, nodes[*skeletonContext.sourceRootNode].stableId);
					}
					const std::string semanticKey = BuildAnimationSemanticKey(sourceAnimation, nodes, skeletonContext);
					const std::string displayName = sourceAnimation.name.empty()
						? "Animation_" + HexHash(StableSemanticHash(semanticKey)).substr(0U, 8U)
						: std::string(sourceAnimation.name);
					const TerraEngine::UUID clipGuid = BuildStableAnimationSubAssetUuid(
						options.packageGuid, AnimationImportSubAssetKind::AnimationClip, semanticKey);

					AnimationClipCompileInput input;
					input.assetId = { clipGuid };
					input.skeletonAssetId = skeleton.asset.assetId;
					input.compatibilityFingerprint = skeleton.asset.compatibilityFingerprint;
					input.durationTicks = std::max<Animation::AnimationFixedTick>(1, build.durationTicks);
					input.sampleRate = Animation::kAnimationFixedTicksPerSecond;
					input.segmentDurationTicks = 30;
					input.tracks = build.compilerTracks;
					if (animationIndex < metadata.animationEvents.size())
					{
						for (const ParsedGltfAnimationEvent& sourceEvent : metadata.animationEvents[animationIndex])
						{
							Animation::AnimationClipEvent event;
							event.tick = SecondsToAnimationTicks(static_cast<float>(sourceEvent.timeSeconds));
							input.durationTicks = std::max(input.durationTicks, event.tick);
							event.stableEventId = StableSemanticHash(semanticKey + "/event/"
								+ NormalizeSemanticToken(sourceEvent.name, "event") + "/"
								+ std::to_string(event.tick) + "/" + sourceEvent.payload);
							event.payloadType = sourceEvent.payloadType;
							event.payload = sourceEvent.payload;
							input.events.push_back(std::move(event));
						}
					}

					GltfGeneratedAnimationClip generated;
					generated.guid = clipGuid;
					generated.skeletonGuid = skeleton.guid;
					generated.sourceAnimationIndex = animationIndex;
					generated.displayName = displayName;
					generated.semanticKey = semanticKey;
					generated.sourcePath = options.sourceRelativePath;
					generated.compiledPath = std::filesystem::path("Animation")
						/ (SanitizeAssetName(displayName, "Animation") + "_"
							+ HexHash(StableSemanticHash(semanticKey)).substr(0U, 8U) + ".teanim");
					generated.sourceTracks = std::move(build.sourceTracks);
					AnimationClipCompileReport report;
					const Animation::AnimationStatus status = CompileAnimationClip(
						input, AnimationClipCompilerSettings{}, generated.asset, report);
					if (status != Animation::AnimationStatus::Ok)
					{
						AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "AnimationClipCompileFailed", semanticKey,
							"The normalized glTF clip failed deterministic segmented compilation: " + report.diagnostic,
							"Inspect channel timestamps, finite values, skeleton compatibility, and configured animation limits.");
						continue;
					}
					animationData.animationClips.push_back(std::move(generated));
				}
			}
		}

		std::vector<GltfSkeletonContext> BuildAnimationSkeletons(
			const fastgltf::Asset& asset,
			const std::vector<GltfImportedNode>& nodes,
			const std::vector<std::optional<std::size_t>>& parents,
			const ParsedGltfMetadata& metadata,
			const GltfCoordinateConverter& converter,
			const GltfPackageImportOptions& options,
			GltfAnimatedPackageData& animation,
			GltfImportResult& diagnostics)
		{
			std::vector<GltfSkeletonContext> contexts;
			for (std::size_t skinIndex = 0U; skinIndex < asset.skins.size(); ++skinIndex)
			{
				const fastgltf::Skin& skin = asset.skins[skinIndex];
				std::vector<std::string> jointPaths;
				for (const std::size_t joint : skin.joints)
				{
					if (joint < nodes.size()) jointPaths.push_back(nodes[joint].normalizedSemanticPath);
				}
				std::sort(jointPaths.begin(), jointPaths.end());
				std::string signature;
				for (const std::string& path : jointPaths) signature += path + ";";
				const std::string displayName = skin.name.empty()
					? "Skeleton_" + HexHash(StableSemanticHash(signature)).substr(0U, 8U)
					: std::string(skin.name);
				const std::string semanticKey = "skeleton/skin/" + NormalizeSemanticToken(displayName, "skin")
					+ "/" + HexHash(StableSemanticHash(signature));
				const std::array<std::size_t, 0U> noAdditionalSeeds{};
				if (auto context = BuildSkeletonContext(asset, nodes, parents, metadata.coordinateConversion,
					converter, options, skinIndex, noAdditionalSeeds, displayName, semanticKey, animation, diagnostics))
				{
					contexts.push_back(std::move(*context));
				}
			}

			std::set<std::size_t> uncoveredAnimatedNodes;
			for (const fastgltf::Animation& sourceAnimation : asset.animations)
			{
				for (const fastgltf::AnimationChannel& channel : sourceAnimation.channels)
				{
					if (!channel.nodeIndex.has_value() || *channel.nodeIndex >= nodes.size()) continue;
					const bool covered = std::any_of(contexts.begin(), contexts.end(), [&](const GltfSkeletonContext& context) {
						return context.sourceNodes.contains(*channel.nodeIndex);
					});
					if (!covered) uncoveredAnimatedNodes.insert(*channel.nodeIndex);
				}
			}
			// A morph-only mesh still needs an evaluated node even when its clip only
			// animates weights (or when it has no clip at all). The shared deformable
			// payload stores one regular palette influence, so keeping this node in a
			// synthetic scene skeleton makes its authored bind TRS source-independent.
			for (std::size_t nodeIndex = 0U; nodeIndex < asset.nodes.size(); ++nodeIndex)
			{
				const fastgltf::Node& node = asset.nodes[nodeIndex];
				if (!node.meshIndex.has_value() || node.skinIndex.has_value()
					|| *node.meshIndex >= asset.meshes.size())
				{
					continue;
				}
				const bool hasMorphTarget = std::any_of(
					asset.meshes[*node.meshIndex].primitives.begin(),
					asset.meshes[*node.meshIndex].primitives.end(),
					[](const fastgltf::Primitive& primitive) { return !primitive.targets.empty(); });
				if (!hasMorphTarget) continue;
				const bool covered = std::any_of(contexts.begin(), contexts.end(), [&](const GltfSkeletonContext& context) {
					return context.sourceNodes.contains(nodeIndex);
				});
				if (!covered) uncoveredAnimatedNodes.insert(nodeIndex);
			}
			if (!uncoveredAnimatedNodes.empty())
			{
				const std::vector<std::size_t> seeds(uncoveredAnimatedNodes.begin(), uncoveredAnimatedNodes.end());
				std::string signature;
				for (const std::size_t node : seeds) signature += nodes[node].normalizedSemanticPath + ";";
				const std::string semanticKey = "skeleton/animated_scene/" + HexHash(StableSemanticHash(signature));
				if (auto context = BuildSkeletonContext(asset, nodes, parents, metadata.coordinateConversion,
					converter, options, std::nullopt, seeds, "AnimatedSceneSkeleton", semanticKey, animation, diagnostics))
				{
					contexts.push_back(std::move(*context));
				}
			}
			return contexts;
		}

		struct PreparedDeformableMesh
		{
			std::size_t sourceMeshIndex = 0U;
			std::size_t generatedMeshIndex = 0U;
			TerraEngine::UUID meshGuid = TerraEngine::EMPTY_UUID;
			TerraEngine::UUID skeletonGuid = TerraEngine::EMPTY_UUID;
			std::uint64_t compatibilityFingerprint = 0U;
			std::filesystem::path finalPath;
			std::filesystem::path compiledPath;
			std::filesystem::path candidatePath;
			std::string semanticKey;
			std::string contentHashHex;
			std::vector<std::string> cookedMorphSemanticKeys;
		};

		std::optional<std::uint32_t> AttributeSetOrdinal(std::string_view name, std::string_view prefix)
		{
			if (!name.starts_with(prefix) || name.size() == prefix.size()) return std::nullopt;
			std::uint32_t value = 0U;
			const char* begin = name.data() + prefix.size();
			const char* end = name.data() + name.size();
			const auto [cursor, error] = std::from_chars(begin, end, value);
			return error == std::errc{} && cursor == end ? std::optional<std::uint32_t>(value) : std::nullopt;
		}

		bool ReadJointAccessor(
			const fastgltf::Asset& asset,
			std::size_t accessorIndex,
			std::size_t expectedCount,
			std::vector<std::array<std::uint32_t, 4U>>& values)
		{
			values.clear();
			if (accessorIndex >= asset.accessors.size()) return false;
			const fastgltf::Accessor& accessor = asset.accessors[accessorIndex];
			if (accessor.type != fastgltf::AccessorType::Vec4 || accessor.count != expectedCount
				|| (accessor.componentType != fastgltf::ComponentType::UnsignedByte
					&& accessor.componentType != fastgltf::ComponentType::UnsignedShort
					&& accessor.componentType != fastgltf::ComponentType::UnsignedInt))
			{
				return false;
			}
			values.resize(accessor.count);
			fastgltf::iterateAccessorWithIndex<fastgltf::math::u32vec4>(asset, accessor,
				[&](const fastgltf::math::u32vec4& value, std::size_t index) {
					values[index] = { value.x(), value.y(), value.z(), value.w() };
				});
			return true;
		}

		bool ReadWeightAccessor(
			const fastgltf::Asset& asset,
			std::size_t accessorIndex,
			std::size_t expectedCount,
			std::vector<std::array<float, 4U>>& values)
		{
			values.clear();
			if (accessorIndex >= asset.accessors.size()) return false;
			const fastgltf::Accessor& accessor = asset.accessors[accessorIndex];
			const bool componentSupported = accessor.componentType == fastgltf::ComponentType::Float
				|| accessor.componentType == fastgltf::ComponentType::UnsignedByte
				|| accessor.componentType == fastgltf::ComponentType::UnsignedShort;
			const bool normalizationValid = accessor.componentType == fastgltf::ComponentType::Float || accessor.normalized;
			if (accessor.type != fastgltf::AccessorType::Vec4 || accessor.count != expectedCount
				|| !componentSupported || !normalizationValid)
			{
				return false;
			}
			values.resize(accessor.count);
			bool finite = true;
			fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(asset, accessor,
				[&](const fastgltf::math::fvec4& value, std::size_t index) {
					values[index] = { value.x(), value.y(), value.z(), value.w() };
					for (const float component : values[index]) finite = finite && std::isfinite(component) && component >= 0.0F;
				});
			return finite;
		}

		bool DecodePrimitiveInfluences(
			const fastgltf::Asset& asset,
			const fastgltf::Primitive& primitive,
			const fastgltf::Skin* skin,
			const GltfSkeletonContext* skeletonContext,
			std::vector<SkinnedMeshCookVertex>& vertices,
			std::string& failure)
		{
			std::map<std::uint32_t, std::size_t> jointAccessors;
			std::map<std::uint32_t, std::size_t> weightAccessors;
			for (const fastgltf::Attribute& attribute : primitive.attributes)
			{
				if (const auto ordinal = AttributeSetOrdinal(attribute.name, "JOINTS_"); ordinal.has_value())
				{
					jointAccessors[*ordinal] = attribute.accessorIndex;
				}
				if (const auto ordinal = AttributeSetOrdinal(attribute.name, "WEIGHTS_"); ordinal.has_value())
				{
					weightAccessors[*ordinal] = attribute.accessorIndex;
				}
			}
			if (jointAccessors.empty() && weightAccessors.empty()) return true;
			if (skin == nullptr || skeletonContext == nullptr)
			{
				failure = "Skin attributes are present, but no source node binds this mesh to a valid skin.";
				return false;
			}
			if (jointAccessors.size() != weightAccessors.size())
			{
				failure = "JOINTS_n and WEIGHTS_n attribute sets are not paired one-to-one.";
				return false;
			}
			for (const auto& [setOrdinal, jointAccessor] : jointAccessors)
			{
				const auto weightAccessor = weightAccessors.find(setOrdinal);
				if (weightAccessor == weightAccessors.end())
				{
					failure = "A JOINTS_n set has no matching WEIGHTS_n set.";
					return false;
				}
				std::vector<std::array<std::uint32_t, 4U>> joints;
				std::vector<std::array<float, 4U>> weights;
				if (!ReadJointAccessor(asset, jointAccessor, vertices.size(), joints)
					|| !ReadWeightAccessor(asset, weightAccessor->second, vertices.size(), weights))
				{
					failure = "A JOINTS_n or WEIGHTS_n accessor has an unsupported type, count, normalization, or non-finite value.";
					return false;
				}
				for (std::size_t vertexIndex = 0U; vertexIndex < vertices.size(); ++vertexIndex)
				{
					for (std::size_t influenceIndex = 0U; influenceIndex < 4U; ++influenceIndex)
					{
						const float weight = weights[vertexIndex][influenceIndex];
						if (!(weight > 0.0F)) continue;
						const std::uint32_t skinJointOrdinal = joints[vertexIndex][influenceIndex];
						if (skinJointOrdinal >= skin->joints.size())
						{
							failure = "A positive skin weight references a joint ordinal outside the skin joint table.";
							return false;
						}
						const std::size_t sourceNode = skin->joints[skinJointOrdinal];
						const auto cookedJoint = skeletonContext->sourceNodeToCookedJoint.find(sourceNode);
						if (cookedJoint == skeletonContext->sourceNodeToCookedJoint.end())
						{
							failure = "A weighted source joint is absent from the cooked skeleton closure.";
							return false;
						}
						vertices[vertexIndex].influences.push_back({ cookedJoint->second, weight });
					}
				}
			}
			return true;
		}

		RawMeshVertex ConvertDeformableBaseVertex(
			const RawMeshVertex& source,
			const GltfCoordinateConverter& converter)
		{
			RawMeshVertex result = source;
			const Animation::AnimationFloat3 position = converter.Vector(
				{ source.position[0U], source.position[1U], source.position[2U] }, true);
			Animation::AnimationFloat3 normal = converter.Vector(
				{ source.normal[0U], source.normal[1U], source.normal[2U] }, false);
			const float normalLength = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
			if (normalLength > std::numeric_limits<float>::epsilon())
			{
				normal = { normal.x / normalLength, normal.y / normalLength, normal.z / normalLength };
			}
			result.position = { position.x, position.y, position.z };
			result.normal = { normal.x, normal.y, normal.z };
			return result;
		}

		float CoordinateBasisDeterminant(const GltfCoordinateConverter& converter)
		{
			return converter.basis[0U][0U] * (converter.basis[1U][1U] * converter.basis[2U][2U] - converter.basis[1U][2U] * converter.basis[2U][1U])
				- converter.basis[0U][1U] * (converter.basis[1U][0U] * converter.basis[2U][2U] - converter.basis[1U][2U] * converter.basis[2U][0U])
				+ converter.basis[0U][2U] * (converter.basis[1U][0U] * converter.basis[2U][1U] - converter.basis[1U][1U] * converter.basis[2U][0U]);
		}

		const GltfImportedMorphTarget* FindImportedMorphTarget(
			const GltfAnimatedPackageData& animation,
			std::size_t meshIndex,
			std::size_t primitiveIndex,
			std::size_t targetIndex)
		{
			const auto target = std::find_if(animation.morphTargets.begin(), animation.morphTargets.end(),
				[&](const GltfImportedMorphTarget& candidate) {
					return candidate.sourceMeshIndex == meshIndex
						&& candidate.sourcePrimitiveIndex == primitiveIndex
						&& candidate.sourceTargetIndex == targetIndex;
				});
			return target == animation.morphTargets.end() ? nullptr : &*target;
		}

		bool BuildDeformableCookInput(
			const fastgltf::Asset& asset,
			std::size_t meshIndex,
			const fastgltf::Skin* skin,
			const GltfSkeletonContext* skeletonContext,
			std::optional<std::uint32_t> morphOnlySourceJoint,
			const GltfCoordinateConverter& converter,
			const GltfAnimatedPackageData& animation,
			SkinnedMeshCookInput& input,
			std::vector<std::string>& cookedMorphSemanticKeys,
			std::string& failure)
		{
			if (meshIndex >= asset.meshes.size()) return false;
			const fastgltf::Mesh& mesh = asset.meshes[meshIndex];
			const bool reverseWinding = CoordinateBasisDeterminant(converter) < 0.0F;
			std::unordered_map<std::uint32_t, std::string> morphIdOwners;
			for (std::size_t primitiveIndex = 0U; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
			{
				const fastgltf::Primitive& primitive = mesh.primitives[primitiveIndex];
				std::vector<RawMeshVertex> rawVertices;
				std::vector<std::uint32_t> rawIndices;
				if (!ExtractPrimitiveGeometry(asset, primitive, rawVertices, rawIndices))
				{
					const bool hasSkinAttributes = std::any_of(primitive.attributes.begin(), primitive.attributes.end(), [](const fastgltf::Attribute& attribute) {
						return std::string_view(attribute.name).starts_with("JOINTS_")
							|| std::string_view(attribute.name).starts_with("WEIGHTS_");
					});
					if (!primitive.targets.empty() || hasSkinAttributes)
					{
						failure = "A deformable primitive could not be decoded as indexed triangles.";
						return false;
					}
					continue;
				}
				std::vector<SkinnedMeshCookVertex> primitiveVertices(rawVertices.size());
				for (std::size_t vertexIndex = 0U; vertexIndex < rawVertices.size(); ++vertexIndex)
				{
					primitiveVertices[vertexIndex].baseVertex = ConvertDeformableBaseVertex(rawVertices[vertexIndex], converter);
					// Morph-only geometry has no authored JOINTS_n stream, but the shared
					// deformable payload still evaluates through a palette. Bind it to its source
					// node so bind and animated TRS are applied exactly once. Skinned meshes
					// keep their authored weights and inverse binds instead.
					if (skin == nullptr && morphOnlySourceJoint.has_value())
					{
						primitiveVertices[vertexIndex].influences.push_back({ *morphOnlySourceJoint, 1.0F });
					}
				}
				if (!DecodePrimitiveInfluences(asset, primitive, skin, skeletonContext, primitiveVertices, failure)) return false;

				const std::uint32_t vertexOffset = static_cast<std::uint32_t>(input.vertices.size());
				input.vertices.insert(input.vertices.end(), primitiveVertices.begin(), primitiveVertices.end());
				for (SkinnedMeshMorphTarget& existingMorph : input.morphTargets)
				{
					existingMorph.positionDeltas.resize(input.vertices.size(), { 0.0F, 0.0F, 0.0F });
					if (!existingMorph.normalDeltas.empty())
					{
						existingMorph.normalDeltas.resize(input.vertices.size(), { 0.0F, 0.0F, 0.0F });
					}
					if (!existingMorph.tangentDeltas.empty())
					{
						existingMorph.tangentDeltas.resize(input.vertices.size(), { 0.0F, 0.0F, 0.0F });
					}
				}
				for (std::size_t index = 0U; index < rawIndices.size(); index += 3U)
				{
					input.indices.push_back(vertexOffset + rawIndices[index]);
					input.indices.push_back(vertexOffset + rawIndices[index + (reverseWinding ? 2U : 1U)]);
					input.indices.push_back(vertexOffset + rawIndices[index + (reverseWinding ? 1U : 2U)]);
				}

				for (std::size_t targetIndex = 0U; targetIndex < primitive.targets.size(); ++targetIndex)
				{
					const GltfImportedMorphTarget* imported = FindImportedMorphTarget(animation, meshIndex, primitiveIndex, targetIndex);
					if (imported == nullptr)
					{
						failure = "A decoded morph target has no importer-owned semantic identity record.";
						return false;
					}
					const std::uint32_t stableTargetId = static_cast<std::uint32_t>(imported->stableId);
					const auto owner = morphIdOwners.find(stableTargetId);
					if (owner != morphIdOwners.end() && owner->second != imported->semanticKey)
					{
						failure = "Two semantic morph targets collide in the TEMF 32-bit stable target identifier space.";
						return false;
					}
					morphIdOwners[stableTargetId] = imported->semanticKey;
					auto existingMorph = std::find_if(input.morphTargets.begin(), input.morphTargets.end(), [stableTargetId](const SkinnedMeshMorphTarget& candidate) {
						return candidate.stableTargetId == stableTargetId;
					});
					if (existingMorph == input.morphTargets.end())
					{
						SkinnedMeshMorphTarget morph;
						morph.stableTargetId = stableTargetId;
						morph.positionDeltas.resize(input.vertices.size(), { 0.0F, 0.0F, 0.0F });
						input.morphTargets.push_back(std::move(morph));
						existingMorph = input.morphTargets.end() - 1;
					}
					const auto decodeChannel = [&](std::string_view semantic,
						auto& destination,
						bool applyUnits) -> bool {
						const auto channel = primitive.findTargetAttribute(targetIndex, semantic);
						if (channel == primitive.targets[targetIndex].end()) return true;
						std::vector<Animation::AnimationFloat3> deltas;
						if (!ReadMorphDeltaAccessor(asset, channel->accessorIndex, rawVertices.size(), deltas))
						{
							failure = "A morph " + std::string(semantic)
								+ " accessor does not contain one finite FLOAT VEC3 delta per primitive vertex.";
							return false;
						}
						if (destination.empty())
						{
							destination.resize(input.vertices.size(), { 0.0F, 0.0F, 0.0F });
						}
						for (std::size_t vertexIndex = 0U; vertexIndex < deltas.size(); ++vertexIndex)
						{
							// Position deltas use source units. Normal and tangent deltas are
							// direction differences, so they change basis but never scale or normalize.
							const Animation::AnimationFloat3 converted = converter.Vector(deltas[vertexIndex], applyUnits);
							destination[static_cast<std::size_t>(vertexOffset) + vertexIndex] = {
								converted.x, converted.y, converted.z
							};
						}
						return true;
					};
					if (!decodeChannel("POSITION", existingMorph->positionDeltas, true)
						|| !decodeChannel("NORMAL", existingMorph->normalDeltas, false)
						|| !decodeChannel("TANGENT", existingMorph->tangentDeltas, false))
					{
						return false;
					}
					if (std::find(cookedMorphSemanticKeys.begin(), cookedMorphSemanticKeys.end(), imported->semanticKey) == cookedMorphSemanticKeys.end())
					{
						cookedMorphSemanticKeys.push_back(imported->semanticKey);
					}
				}
			}
			// Stable target identifiers, rather than the incidental primitive traversal
			// order, define the baked target order consumed by the TEMF writer.
			std::sort(input.morphTargets.begin(), input.morphTargets.end(), [](const SkinnedMeshMorphTarget& left, const SkinnedMeshMorphTarget& right) {
				return left.stableTargetId < right.stableTargetId;
			});
			return !input.vertices.empty() && !input.indices.empty();
		}

		void CleanupPreparedDeformableCandidates(std::span<const PreparedDeformableMesh> candidates)
		{
			std::error_code ignored;
			for (const PreparedDeformableMesh& candidate : candidates)
			{
				std::filesystem::remove(candidate.candidatePath, ignored);
				ignored.clear();
			}
		}

		void CleanupStaticDeformableCandidates(
			const fastgltf::Asset& asset,
			const GltfPackageImportOptions& options,
			const GltfImportResult& result)
		{
			std::error_code ignored;
			for (const GeneratedGltfMeshAsset& mesh : result.generatedMeshes)
			{
				if (MeshRequiresDeformableCook(asset, mesh.sourceMeshIndex))
				{
					std::filesystem::remove(StaticDeformableCandidatePath(options, mesh.compiledPath), ignored);
					ignored.clear();
				}
			}
		}

		bool PrepareDeformableMeshes(
			const fastgltf::Asset& asset,
			const GltfCoordinateConverter& converter,
			const GltfPackageImportOptions& options,
			const std::vector<GltfSkeletonContext>& skeletonContexts,
			GltfAnimatedPackageData& animation,
			GltfImportResult& result,
			std::vector<PreparedDeformableMesh>& candidates)
		{
			// A source mesh may be instanced by several nodes. Morph-only instances
			// cannot share one identity palette because each node supplies its own
			// placement transform, so keep the source node in the binding identity.
			// Skinned bindings intentionally retain the existing mesh/skin identity:
			// their inverse-bind table already normalizes mesh placement.
			using DeformableBinding = std::tuple<std::size_t, std::optional<std::size_t>, std::optional<std::size_t>>;
			std::set<DeformableBinding> uniqueBindings;
			for (std::size_t nodeIndex = 0U; nodeIndex < asset.nodes.size(); ++nodeIndex)
			{
				const fastgltf::Node& node = asset.nodes[nodeIndex];
				if (node.meshIndex.has_value() && node.skinIndex.has_value())
				{
					uniqueBindings.emplace(*node.meshIndex, node.skinIndex, std::nullopt);
				}
				else if (node.meshIndex.has_value() && *node.meshIndex < asset.meshes.size())
				{
					const bool hasMorphs = std::any_of(asset.meshes[*node.meshIndex].primitives.begin(),
						asset.meshes[*node.meshIndex].primitives.end(), [](const fastgltf::Primitive& primitive) {
							return !primitive.targets.empty();
						});
					if (hasMorphs) uniqueBindings.emplace(*node.meshIndex, std::nullopt, nodeIndex);
				}
			}
			for (std::size_t meshIndex = 0U; meshIndex < asset.meshes.size(); ++meshIndex)
			{
				const bool hasMorphs = std::any_of(asset.meshes[meshIndex].primitives.begin(), asset.meshes[meshIndex].primitives.end(), [](const fastgltf::Primitive& primitive) {
					return !primitive.targets.empty();
				});
				const bool hasBinding = std::any_of(uniqueBindings.begin(), uniqueBindings.end(), [meshIndex](const DeformableBinding& binding) {
					return std::get<0U>(binding) == meshIndex;
				});
				if ((hasMorphs || MeshRequiresDeformableCook(asset, meshIndex)) && !hasBinding)
				{
					// Preserve the legacy unplaced fallback for malformed/unused source
					// meshes. Valid placed morph meshes always take the node-bound path.
					uniqueBindings.emplace(meshIndex, std::nullopt, std::nullopt);
				}
			}

			for (const auto& [meshIndex, skinIndex, sourceNodeIndex] : uniqueBindings)
			{
				if (meshIndex >= asset.meshes.size()) continue;
				const GltfSkeletonContext* skeletonContext = nullptr;
				const GltfGeneratedSkeleton* skeleton = nullptr;
				const fastgltf::Skin* skin = nullptr;
				std::optional<std::uint32_t> morphOnlySourceJoint;
				if (skinIndex.has_value())
				{
					if (*skinIndex >= asset.skins.size())
					{
						AddDiagnostic(result, GltfDiagnosticSeverity::Error, "DeformableSkinMissing", "meshes/" + std::to_string(meshIndex), "A mesh node references a skin outside the parsed skin table.");
						CleanupPreparedDeformableCandidates(candidates);
						return false;
					}
					skin = &asset.skins[*skinIndex];
					const auto context = std::find_if(skeletonContexts.begin(), skeletonContexts.end(), [&](const GltfSkeletonContext& candidate) {
						return animation.skeletons[candidate.generatedSkeletonIndex].sourceSkinIndex == *skinIndex;
					});
					if (context == skeletonContexts.end())
					{
						AddDiagnostic(result, GltfDiagnosticSeverity::Error, "DeformableSkeletonMissing", "meshes/" + std::to_string(meshIndex), "A skinned mesh has no validated cooked skeleton context.");
						CleanupPreparedDeformableCandidates(candidates);
						return false;
					}
					skeletonContext = &*context;
					skeleton = &animation.skeletons[context->generatedSkeletonIndex];
				}
				else if (sourceNodeIndex.has_value())
				{
					const auto context = std::find_if(skeletonContexts.begin(), skeletonContexts.end(), [&](const GltfSkeletonContext& candidate) {
						return candidate.sourceNodes.contains(*sourceNodeIndex)
							&& candidate.sourceNodeToCookedJoint.contains(*sourceNodeIndex);
					});
					if (context == skeletonContexts.end())
					{
						AddDiagnostic(result, GltfDiagnosticSeverity::Error, "MorphOnlyNodeSkeletonMissing",
							"nodes/" + std::to_string(*sourceNodeIndex),
							"A morph-only mesh node has no cooked synthetic skeleton context for its authored transform.");
						CleanupPreparedDeformableCandidates(candidates);
						return false;
					}
					skeletonContext = &*context;
					skeleton = &animation.skeletons[context->generatedSkeletonIndex];
					morphOnlySourceJoint = context->sourceNodeToCookedJoint.at(*sourceNodeIndex);
				}

				SkinnedMeshCookInput input;
				std::vector<std::string> cookedMorphs;
				std::string extractionFailure;
				if (!BuildDeformableCookInput(asset, meshIndex, skin, skeletonContext, morphOnlySourceJoint, converter,
					animation, input, cookedMorphs, extractionFailure))
				{
					AddDiagnostic(result, GltfDiagnosticSeverity::Error, "DeformableMeshDecodeFailed", "meshes/" + std::to_string(meshIndex),
						"The skinned/morphed primitive data could not be normalized: " + extractionFailure,
						"Verify indexed triangle topology, paired JOINTS_n/WEIGHTS_n accessors, skin joint ordinals, and morph POSITION counts.");
					CleanupPreparedDeformableCandidates(candidates);
					return false;
				}

				const fastgltf::Mesh& sourceMesh = asset.meshes[meshIndex];
				std::string meshSignature;
				for (const fastgltf::Primitive& primitive : sourceMesh.primitives) meshSignature += HexHash(PrimitiveSemanticHash(asset, primitive)) + ";";
				// Node array indices are only diagnostic metadata: reordering source nodes
				// must not churn a generated mesh GUID. The normalized path is built from
				// glTF hierarchy/name semantics and is the durable identity instead.
				const std::string sourceNodeKey = sourceNodeIndex.has_value()
					&& *sourceNodeIndex < animation.nodes.size()
					? animation.nodes[*sourceNodeIndex].normalizedSemanticPath
					: std::string{};
				const std::string bindingKey = skeleton != nullptr
					? skeleton->semanticKey + (!sourceNodeKey.empty()
						? "/node/" + sourceNodeKey : std::string{})
					: "morph_only";
				const std::string semanticKey = "deformable_mesh/"
					+ NormalizeSemanticToken(sourceMesh.name, "unnamed_mesh") + "/"
					+ HexHash(StableSemanticHash(meshSignature)) + "/" + bindingKey;
				const TerraEngine::UUID meshGuid = BuildStableAnimationSubAssetUuid(
					options.packageGuid, AnimationImportSubAssetKind::DeformableMesh, semanticKey);

				const auto original = std::find_if(result.generatedMeshes.begin(), result.generatedMeshes.end(), [meshIndex](const GeneratedGltfMeshAsset& mesh) {
					return mesh.sourceMeshIndex == meshIndex;
				});
				if (original == result.generatedMeshes.end())
				{
					AddDiagnostic(result, GltfDiagnosticSeverity::Error, "DeformableGeneratedMeshMissing", "meshes/" + std::to_string(meshIndex), "The static inventory has no generated mesh record to receive deformable payload data.");
					CleanupPreparedDeformableCandidates(candidates);
					return false;
				}
				const std::size_t priorBindingCount = static_cast<std::size_t>(std::count_if(candidates.begin(), candidates.end(), [meshIndex](const PreparedDeformableMesh& candidate) {
					return candidate.sourceMeshIndex == meshIndex;
				}));
				std::size_t generatedMeshIndex = static_cast<std::size_t>(std::distance(result.generatedMeshes.begin(), original));
				const TerraEngine::UUID originalGuid = original->guid;
				if (priorBindingCount == 0U)
				{
					result.generatedMeshes[generatedMeshIndex].guid = meshGuid;
					for (GltfMaterialAssignment& assignment : result.materialAssignments)
					{
						if (assignment.meshGuid == originalGuid) assignment.meshGuid = meshGuid;
					}
				}
				else
				{
					const TerraEngine::UUID primaryBindingGuid = result.generatedMeshes[generatedMeshIndex].guid;
					GeneratedGltfMeshAsset duplicate = *original;
					duplicate.guid = meshGuid;
					duplicate.displayName += "_" + HexHash(StableSemanticHash(bindingKey)).substr(0U, 8U);
					duplicate.compiledPath = duplicate.compiledPath.parent_path()
						/ (duplicate.compiledPath.stem().string() + "_" + HexHash(StableSemanticHash(bindingKey)).substr(0U, 8U) + ".temf");
					generatedMeshIndex = result.generatedMeshes.size();
					result.generatedMeshes.push_back(duplicate);
					std::vector<GltfMaterialAssignment> duplicateAssignments;
					for (const GltfMaterialAssignment& assignment : result.materialAssignments)
					{
						if (assignment.meshGuid == primaryBindingGuid)
						{
							GltfMaterialAssignment copied = assignment;
							copied.meshGuid = meshGuid;
							duplicateAssignments.push_back(std::move(copied));
						}
					}
					result.materialAssignments.insert(result.materialAssignments.end(), duplicateAssignments.begin(), duplicateAssignments.end());
				}

				PreparedDeformableMesh prepared;
				prepared.sourceMeshIndex = meshIndex;
				prepared.generatedMeshIndex = generatedMeshIndex;
				prepared.meshGuid = meshGuid;
				prepared.semanticKey = semanticKey;
				prepared.compiledPath = result.generatedMeshes[generatedMeshIndex].compiledPath;
				prepared.finalPath = options.buildRoot / prepared.compiledPath;
				prepared.candidatePath = prepared.finalPath.parent_path()
					/ (prepared.finalPath.filename().string() + ".dc-"
						+ HexHash(StableSemanticHash(bindingKey)));
				prepared.cookedMorphSemanticKeys = std::move(cookedMorphs);
				if (skeleton != nullptr)
				{
					prepared.skeletonGuid = skeleton->guid;
					prepared.compatibilityFingerprint = skeleton->asset.compatibilityFingerprint;
					// This durable association lets the prefab select the exact synthetic
					// context when one glTF package contains several morph-only instances.
					// It is deliberately not used as a second mesh transform.
					result.generatedMeshes[generatedMeshIndex].animationSkeletonGuid = skeleton->guid;
				}
				std::error_code ignored;
				std::filesystem::remove(prepared.candidatePath, ignored);
				SkinnedMeshCookSettings settings;
				settings.sourceSkeletonJointCount = skeleton != nullptr
					? static_cast<std::uint32_t>(skeleton->asset.joints.size())
					: 1U;
				settings.skeletonCompatibilityFingerprint = skeleton != nullptr
					? skeleton->asset.compatibilityFingerprint
					: 0U;
				const GeneratedModelMeshVirtualGeometrySettings* meshSettings =
					FindMeshVirtualGeometrySettings(
						options,
						result.generatedMeshes[generatedMeshIndex].guid);
				if (meshSettings != nullptr)
				{
					// This deformable cook replaces the temporary static TEMF.
					// Carry the child recipe forward so its LOD chain survives.
					settings.virtualGeometrySettings =
						ToCompileSettings(*meshSettings);
				}
				SkinnedMeshCookReport report;
				if (!CompileSkinnedMeshFromRaw(options.sourceRelativePath, prepared.candidatePath, input, &report, settings))
				{
					AddDiagnostic(result, GltfDiagnosticSeverity::Error,
						report.diagnosticCode.empty() ? "DeformableMeshCookFailed" : report.diagnosticCode,
						prepared.compiledPath.generic_string(), report.diagnosticMessage,
						report.recommendedCorrection);
					CleanupPreparedDeformableCandidates(candidates);
					std::filesystem::remove(prepared.candidatePath, ignored);
					return false;
				}
				GeneratedGltfMeshAsset& cookedMesh = result.generatedMeshes[generatedMeshIndex];
				bool boundsInitialized = false;
				for (const SkinnedMeshCookVertex& vertex : input.vertices)
				{
					const std::array<float, 3U>& position = vertex.baseVertex.position;
					if (!boundsInitialized)
					{
						cookedMesh.localBoundsMin = position;
						cookedMesh.localBoundsMax = position;
						boundsInitialized = true;
					}
					else
					{
						for (std::size_t axis = 0U; axis < 3U; ++axis)
						{
							cookedMesh.localBoundsMin[axis] = std::min(cookedMesh.localBoundsMin[axis], position[axis]);
							cookedMesh.localBoundsMax[axis] = std::max(cookedMesh.localBoundsMax[axis], position[axis]);
						}
					}
				}
				PopulateGeneratedMeshVirtualGeometryMetadata(prepared.candidatePath, cookedMesh, result.diagnostics);
				if (skin == nullptr && morphOnlySourceJoint.has_value())
				{
					AddDiagnostic(result, GltfDiagnosticSeverity::Info, "MorphOnlyNodeBoundDeformableCooked", prepared.compiledPath.generic_string(), "A morph-only primitive was bound to its source-node skeleton joint so constant and animated glTF node transforms remain visible through the shared TEMF payload.");
				}
				else if (skin == nullptr)
				{
					AddDiagnostic(result, GltfDiagnosticSeverity::Info, "MorphOnlyDeformableCooked", prepared.compiledPath.generic_string(), "A morph-only primitive was cooked with the TEMF identity-skinning record required by the current shared deformable payload contract.");
				}
				else if (report.zeroWeightVertexCount == input.vertices.size())
				{
					AddDiagnostic(result, GltfDiagnosticSeverity::Info, "SkinHasNoWeightedVertices", prepared.compiledPath.generic_string(), "The mesh is bound to a skin but contains no positive retained influences; identity deformation was encoded without discarding the skeleton dependency.");
				}
				prepared.contentHashHex = ComputeFileHashHex(prepared.candidatePath);
				candidates.push_back(std::move(prepared));
			}
			CleanupStaticDeformableCandidates(asset, options, result);
			return true;
		}

		std::string HashBytes(std::span<const std::uint8_t> bytes)
		{
			std::uint64_t hash = 1469598103934665603ULL;
			ExtendSemanticHash(hash, std::as_bytes(bytes));
			return HexHash(hash);
		}

		struct PendingAnimationArtifact
		{
			std::filesystem::path fileName;
			std::vector<std::uint8_t> bytes;
		};

		bool WriteAnimationArtifact(
			const std::filesystem::path& path,
			std::span<const std::uint8_t> bytes)
		{
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if (!output) return false;
			if (!bytes.empty())
			{
				output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			}
			output.flush();
			return static_cast<bool>(output);
		}

		bool PublishAnimationArtifactsAtomically(
			const GltfPackageImportOptions& options,
			GltfAnimatedPackageData& animation,
			GltfImportResult& diagnostics,
			std::unordered_map<TerraEngine::UUID, std::string>& contentHashes,
			std::span<const PreparedDeformableMesh> deformableMeshes)
		{
			std::vector<PendingAnimationArtifact> artifacts;
			for (const GltfGeneratedSkeleton& skeleton : animation.skeletons)
			{
				PendingAnimationArtifact artifact;
				artifact.fileName = skeleton.compiledPath.filename();
				const Animation::AnimationStatus status = Animation::SerializeSkeletonAsset(skeleton.asset, artifact.bytes);
				if (status != Animation::AnimationStatus::Ok)
				{
					CleanupPreparedDeformableCandidates(deformableMeshes);
					AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "SkeletonSerializeFailed", skeleton.semanticKey,
						"The cooked skeleton failed checked serialization with status " + std::string(Animation::ToString(status)) + ".",
						"Inspect topology, normalized transforms, inverse binds, sockets, and frozen limits.");
					return false;
				}
				contentHashes[skeleton.guid] = HashBytes(artifact.bytes);
				artifacts.push_back(std::move(artifact));
			}
			for (const GltfGeneratedAnimationClip& clip : animation.animationClips)
			{
				PendingAnimationArtifact artifact;
				artifact.fileName = clip.compiledPath.filename();
				const Animation::AnimationStatus status = Animation::SerializeAnimationClipAsset(clip.asset, artifact.bytes);
				if (status != Animation::AnimationStatus::Ok)
				{
					CleanupPreparedDeformableCandidates(deformableMeshes);
					AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "AnimationClipSerializeFailed", clip.semanticKey,
						"The cooked animation clip failed checked serialization with status " + std::string(Animation::ToString(status)) + ".",
						"Inspect segmented ranges, codec metadata, events, and skeleton compatibility.");
					return false;
				}
				contentHashes[clip.guid] = HashBytes(artifact.bytes);
				artifacts.push_back(std::move(artifact));
			}
			if (artifacts.empty() && deformableMeshes.empty())
			{
				animation.publishedAtomically = true;
				return true;
			}

			const std::filesystem::path finalRoot = options.buildRoot / animation.publishedAnimationRoot;
			// The outer package transaction already isolates this generation. A
			// compact 64-bit token still prevents sibling collision while preserving
			// enough Windows path budget for generated skeleton and clip filenames.
			const std::string publicationId = CompactPrivateTransactionToken(options.packageGuid);
			const std::filesystem::path stagingRoot = finalRoot.parent_path()
				/ (finalRoot.filename().string() + ".s-" + publicationId);
			const std::filesystem::path backupRoot = finalRoot.parent_path()
				/ (finalRoot.filename().string() + ".b-" + publicationId);
			std::error_code error;
			if (!artifacts.empty())
			{
				std::filesystem::create_directories(finalRoot.parent_path(), error);
				if (error)
				{
					CleanupPreparedDeformableCandidates(deformableMeshes);
					AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "AnimationPublicationDirectoryFailed", finalRoot.generic_string(), "The animation publication parent directory could not be created: " + error.message());
					return false;
				}
				std::filesystem::remove_all(stagingRoot, error);
				error.clear();
				std::filesystem::create_directories(stagingRoot, error);
				if (error)
				{
					CleanupPreparedDeformableCandidates(deformableMeshes);
					AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "AnimationStagingFailed", stagingRoot.generic_string(), "The animation staging directory could not be created: " + error.message());
					return false;
				}
				for (const PendingAnimationArtifact& artifact : artifacts)
				{
					if (!WriteAnimationArtifact(stagingRoot / artifact.fileName, artifact.bytes))
					{
						std::filesystem::remove_all(stagingRoot, error);
						CleanupPreparedDeformableCandidates(deformableMeshes);
						AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "AnimationArtifactWriteFailed", artifact.fileName.generic_string(), "A staged animation artifact could not be written; the last-known-good generation remains unchanged.");
						return false;
					}
				}
			}

			struct MeshCommitState
			{
				const PreparedDeformableMesh* mesh = nullptr;
				std::filesystem::path backupPath;
				bool hadPublishedGeneration = false;
				bool committed = false;
			};
			std::vector<MeshCommitState> meshStates;
			meshStates.reserve(deformableMeshes.size());
			const auto rollbackMeshes = [&]() {
				std::error_code rollbackError;
				for (auto state = meshStates.rbegin(); state != meshStates.rend(); ++state)
				{
					if (state->committed) std::filesystem::remove(state->mesh->finalPath, rollbackError);
					rollbackError.clear();
					if (state->hadPublishedGeneration && std::filesystem::exists(state->backupPath, rollbackError))
					{
						rollbackError.clear();
						std::filesystem::rename(state->backupPath, state->mesh->finalPath, rollbackError);
					}
					rollbackError.clear();
				}
			};

			for (std::size_t meshIndex = 0U; meshIndex < deformableMeshes.size(); ++meshIndex)
			{
				const PreparedDeformableMesh& mesh = deformableMeshes[meshIndex];
				MeshCommitState state;
				state.mesh = &mesh;
				state.backupPath = mesh.finalPath.parent_path() / (mesh.finalPath.filename().string()
					+ ".b-" + publicationId + "-" + std::to_string(meshIndex));
				std::filesystem::create_directories(mesh.finalPath.parent_path(), error);
				if (error || !std::filesystem::exists(mesh.candidatePath))
				{
					rollbackMeshes();
					std::filesystem::remove_all(stagingRoot, error);
					CleanupPreparedDeformableCandidates(deformableMeshes);
					AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "DeformablePublicationCandidateMissing", mesh.compiledPath.generic_string(), "A validated deformable TEMF candidate disappeared before the protected publication boundary.");
					return false;
				}
				error.clear();
				std::filesystem::remove(state.backupPath, error);
				error.clear();
				state.hadPublishedGeneration = std::filesystem::exists(mesh.finalPath, error) && !error;
				error.clear();
				if (state.hadPublishedGeneration)
				{
					std::filesystem::rename(mesh.finalPath, state.backupPath, error);
					if (error)
					{
						rollbackMeshes();
						std::filesystem::remove_all(stagingRoot, error);
						CleanupPreparedDeformableCandidates(deformableMeshes);
						AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "DeformablePublicationBackupFailed", mesh.compiledPath.generic_string(), "The last-known-good TEMF could not be moved aside for transactional publication.");
						return false;
					}
				}
				meshStates.push_back(state);
				std::filesystem::rename(mesh.candidatePath, mesh.finalPath, error);
				if (error)
				{
					rollbackMeshes();
					std::filesystem::remove_all(stagingRoot, error);
					CleanupPreparedDeformableCandidates(deformableMeshes);
					AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "DeformablePublicationCommitFailed", mesh.compiledPath.generic_string(), "The validated deformable TEMF could not be committed; prior mesh generations were restored.");
					return false;
				}
				meshStates.back().committed = true;
			}

			bool hadPublishedAnimationGeneration = false;
			if (!artifacts.empty())
			{
				hadPublishedAnimationGeneration = std::filesystem::exists(finalRoot, error) && !error;
				error.clear();
				std::filesystem::remove_all(backupRoot, error);
				error.clear();
				if (hadPublishedAnimationGeneration)
				{
					std::filesystem::rename(finalRoot, backupRoot, error);
					if (error)
					{
						rollbackMeshes();
						std::filesystem::remove_all(stagingRoot, error);
						AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "AnimationPublicationBackupFailed", finalRoot.generic_string(), "The last-known-good animation directory could not be moved aside; committed mesh candidates were rolled back.");
						return false;
					}
				}
				std::filesystem::rename(stagingRoot, finalRoot, error);
				if (error)
				{
					const std::string publicationError = error.message();
					error.clear();
					if (hadPublishedAnimationGeneration) std::filesystem::rename(backupRoot, finalRoot, error);
					rollbackMeshes();
					std::filesystem::remove_all(stagingRoot, error);
					AddDiagnostic(diagnostics, GltfDiagnosticSeverity::Error, "AnimationPublicationCommitFailed", finalRoot.generic_string(), "The staged animation generation could not be transactionally published: " + publicationError, "Prior animation and mesh generations were restored when available.");
					return false;
				}
				std::filesystem::remove_all(backupRoot, error);
			}
			for (const MeshCommitState& state : meshStates)
			{
				std::filesystem::remove(state.backupPath, error);
				error.clear();
			}
			animation.publishedAtomically = true;
			return true;
		}

		void PopulateAnimationManifest(
			const GltfPackageImportOptions& options,
			const GltfAnimatedPackageData& animation,
			const std::unordered_map<TerraEngine::UUID, std::string>& contentHashes,
			std::span<const PreparedDeformableMesh> deformableMeshes,
			GltfImportResult& result)
		{
			AnimationImportManifest& manifest = result.animationManifest;
			manifest.packageGuid = options.packageGuid;
			manifest.sourceHashHex = result.package.sourceHashHex;
			manifest.importerName = "fastgltf-animation";
			manifest.importerVersion = "1";
			std::uint64_t settingsHash = StableSemanticHash("gltf-animation-settings-v1");
			ExtendSemanticHash(settingsHash, animation.coordinateConversion.sourceUnitScaleMeters);
			ExtendSemanticHash(settingsHash, animation.coordinateConversion.sourceUpAxis);
			ExtendSemanticHash(settingsHash, animation.coordinateConversion.sourceForwardAxis);
			ExtendSemanticHash(settingsHash, animation.coordinateConversion.sourceHandedness);
			manifest.settingsFingerprintHex = HexHash(settingsHash);
			manifest.candidateGeneration = 1U;
			manifest.publishedGeneration = animation.publishedAtomically ? 1U : 0U;
			manifest.lastKnownGoodGeneration = animation.publishedAtomically ? 1U : 0U;
			manifest.publishedAtomically = animation.publishedAtomically;
			manifest.preservedLastKnownGood = !animation.publishedAtomically;

			for (const GltfGeneratedSkeleton& skeleton : animation.skeletons)
			{
				GeneratedAnimationSubAsset record;
				record.guid = skeleton.guid;
				record.kind = AnimationImportSubAssetKind::Skeleton;
				record.semanticKey = skeleton.semanticKey;
				record.displayName = skeleton.displayName;
				record.sourcePath = skeleton.sourcePath;
				record.compiledPath = skeleton.compiledPath;
				record.compatibilityFingerprint = skeleton.asset.compatibilityFingerprint;
				if (const auto hash = contentHashes.find(skeleton.guid); hash != contentHashes.end()) record.contentHashHex = hash->second;
				manifest.subAssets.push_back(std::move(record));
			}
			for (const GltfGeneratedAnimationClip& clip : animation.animationClips)
			{
				GeneratedAnimationSubAsset record;
				record.guid = clip.guid;
				record.kind = AnimationImportSubAssetKind::AnimationClip;
				record.semanticKey = clip.semanticKey;
				record.displayName = clip.displayName;
				record.sourcePath = clip.sourcePath;
				record.compiledPath = clip.compiledPath;
				record.compatibilityFingerprint = clip.asset.compatibilityFingerprint;
				record.dependencies.push_back({ clip.skeletonGuid, clip.asset.compatibilityFingerprint, true });
				if (const auto hash = contentHashes.find(clip.guid); hash != contentHashes.end()) record.contentHashHex = hash->second;
				manifest.subAssets.push_back(std::move(record));
			}
			for (const PreparedDeformableMesh& mesh : deformableMeshes)
			{
				GeneratedAnimationSubAsset record;
				record.guid = mesh.meshGuid;
				record.kind = AnimationImportSubAssetKind::DeformableMesh;
				record.semanticKey = mesh.semanticKey;
				record.displayName = mesh.generatedMeshIndex < result.generatedMeshes.size()
					? result.generatedMeshes[mesh.generatedMeshIndex].displayName
					: "DeformableMesh";
				record.sourcePath = options.sourceRelativePath;
				record.compiledPath = mesh.compiledPath;
				record.compatibilityFingerprint = mesh.compatibilityFingerprint;
				record.contentHashHex = mesh.contentHashHex;
				if (mesh.skeletonGuid != TerraEngine::EMPTY_UUID)
				{
					record.dependencies.push_back({ mesh.skeletonGuid, mesh.compatibilityFingerprint, true });
				}
				manifest.subAssets.push_back(std::move(record));

				for (const std::string& morphSemanticKey : mesh.cookedMorphSemanticKeys)
				{
					const auto morph = std::find_if(animation.morphTargets.begin(), animation.morphTargets.end(), [&](const GltfImportedMorphTarget& candidate) {
						return candidate.semanticKey == morphSemanticKey;
					});
					if (morph == animation.morphTargets.end()) continue;
					const std::string payloadSemanticKey = morph->semanticKey + "/payload/" + mesh.semanticKey;
					GeneratedAnimationSubAsset morphRecord;
					morphRecord.guid = BuildStableAnimationSubAssetUuid(options.packageGuid, AnimationImportSubAssetKind::MorphTarget, payloadSemanticKey);
					morphRecord.kind = AnimationImportSubAssetKind::MorphTarget;
					morphRecord.semanticKey = payloadSemanticKey;
					morphRecord.displayName = morph->displayName;
					morphRecord.sourcePath = options.sourceRelativePath;
					morphRecord.compiledPath = mesh.compiledPath;
					morphRecord.compatibilityFingerprint = mesh.compatibilityFingerprint;
					morphRecord.contentHashHex = mesh.contentHashHex;
					morphRecord.dependencies.push_back({ mesh.meshGuid, mesh.compatibilityFingerprint, true });
					if (mesh.skeletonGuid != TerraEngine::EMPTY_UUID)
					{
						morphRecord.dependencies.push_back({ mesh.skeletonGuid, mesh.compatibilityFingerprint, true });
					}
					manifest.subAssets.push_back(std::move(morphRecord));
				}
			}
			CanonicalizeAnimationImportManifest(manifest);
			manifest.dependencyFingerprintHex = BuildAnimationImportDependencyFingerprint(manifest);
		}

		void RemoveSupersededAnimationDiagnostics(GltfImportResult& result)
		{
			result.diagnostics.erase(std::remove_if(result.diagnostics.begin(), result.diagnostics.end(), [](const GltfImportDiagnostic& diagnostic) {
				return diagnostic.code == "UnsupportedAnimationIgnored" || diagnostic.code == "UnsupportedSkinIgnored";
			}), result.diagnostics.end());
		}
	}

	bool IsGltfPackageSourceExtension(const std::filesystem::path& filePath)
	{
		const std::string ext = NormalizeExtension(filePath);
		return ext == ".gltf" || ext == ".glb";
	}

	GltfImportResult ImportStaticGltfPackage(const GltfPackageImportOptions& options)
	{
		GltfImportResult result{};
		result.packageGuid = options.packageGuid;
		result.package.sourcePath = options.sourceRelativePath;
		result.package.packageKind = NormalizeExtension(options.sourcePath) == ".glb" ? GltfPackageKind::GltfBinary : GltfPackageKind::GltfText;
		result.package.sourceHashHex = ComputeFileHashHex(options.sourcePath);
		// Generated children follow the logical project-relative source name. The
		// physical source may be a cache or test fixture with a different basename.
		result.package.importRoot = options.sourceRelativePath.parent_path()
			/ (options.sourceRelativePath.stem().string() + "_Generated");

		ReportProgress(options, 0.04f, "Reading", "Opening glTF source data.");
		auto gltfFile = fastgltf::GltfDataBuffer::FromPath(options.sourcePath);
		if (!gltfFile)
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Error, "SourceOpenFailed", options.sourceRelativePath.generic_string(), std::string("Failed to open glTF package: ") + std::string(fastgltf::getErrorMessage(gltfFile.error())));
			PromoteStatusFromDiagnostics(result);
			return result;
		}

		fastgltf::Parser parser(
			fastgltf::Extensions::KHR_materials_unlit |
			fastgltf::Extensions::KHR_texture_transform |
			fastgltf::Extensions::KHR_lights_punctual);

		constexpr fastgltf::Options parserOptions =
			fastgltf::Options::DontRequireValidAssetMember |
			fastgltf::Options::AllowDouble |
			fastgltf::Options::LoadExternalBuffers |
			fastgltf::Options::LoadGLBBuffers |
			fastgltf::Options::GenerateMeshIndices |
			fastgltf::Options::DecomposeNodeMatrices;

		ReportProgress(options, 0.10f, "Parsing", "Parsing glTF scene, buffers, and package metadata.");
		auto parsed = parser.loadGltf(gltfFile.get(), options.sourcePath.parent_path(), parserOptions);
		if (parsed.error() != fastgltf::Error::None)
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Error, "ParseFailed", options.sourceRelativePath.generic_string(), std::string("Failed to parse glTF package: ") + std::string(fastgltf::getErrorMessage(parsed.error())));
			PromoteStatusFromDiagnostics(result);
			return result;
		}

		fastgltf::Asset& asset = parsed.get();
		ReportProgress(
			options,
			0.14f,
			"Inventory",
			BuildPackageInventoryMessage(asset.meshes.size(), asset.textures.size(), asset.materials.size()));
		if (!asset.animations.empty())
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Info, "UnsupportedAnimationIgnored", "animations", "Animation data was not imported for this static asset import.", "Static geometry was imported.");
		}
		if (!asset.skins.empty())
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "UnsupportedSkinIgnored", "skins", "Skinning data was not imported for this static asset import.", "Static mesh geometry was imported without skinning.");
		}
		if (!asset.cameras.empty())
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "UnsupportedCameraIgnored", "cameras", "Camera data was not imported for this static asset import.", "Renderable meshes were imported.");
		}
		else if (SourceTextContains(options.sourcePath, "\"cameras\""))
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "UnsupportedCameraIgnored", "cameras", "Camera data was detected in the source package but is not imported for static assets.", "Renderable meshes were imported.");
		}
		if (!asset.lights.empty())
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "UnsupportedLightIgnored", "lights", "Light data was not imported for this static asset import.", "Renderable meshes were imported.");
		}
		else if (SourceTextContains(options.sourcePath, "\"lights\""))
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "UnsupportedLightIgnored", "lights", "Light data was detected in the source package but is not imported for static assets.", "Renderable meshes were imported.");
		}

		const std::filesystem::path packageFolderRelative = options.sourceRelativePath.parent_path();
		const float materialStart = 0.18f;
		const float materialEnd = 0.38f;
		for (std::size_t materialIndex = 0; materialIndex < asset.materials.size(); ++materialIndex)
		{
			const float fraction = asset.materials.empty() ? 1.0f : static_cast<float>(materialIndex) / static_cast<float>(asset.materials.size());
			ReportProgress(options,
				materialStart + (materialEnd - materialStart) * fraction,
				"Materials",
				"Generating Terra material assets.");
			const fastgltf::Material& sourceMaterial = asset.materials[materialIndex];
			const std::string materialName = SanitizeAssetName(std::string(sourceMaterial.name), "Material_" + std::to_string(materialIndex));
			GeneratedGltfMaterialAsset material{};
			material.guid = StableChildUuid(options.packageGuid, "gltf-material", materialIndex);
			material.sourceMaterialIndex = materialIndex;
			material.displayName = materialName;
			material.sourcePath = result.package.importRoot / (materialName + ".tmat");
			material.compiledPath = material.sourcePath;
			material.baseColorFactor = {
				static_cast<float>(sourceMaterial.pbrData.baseColorFactor.x()),
				static_cast<float>(sourceMaterial.pbrData.baseColorFactor.y()),
				static_cast<float>(sourceMaterial.pbrData.baseColorFactor.z()),
				static_cast<float>(sourceMaterial.pbrData.baseColorFactor.w())
			};
			material.emissiveFactor = {
				static_cast<float>(sourceMaterial.emissiveFactor.x() * sourceMaterial.emissiveStrength),
				static_cast<float>(sourceMaterial.emissiveFactor.y() * sourceMaterial.emissiveStrength),
				static_cast<float>(sourceMaterial.emissiveFactor.z() * sourceMaterial.emissiveStrength)
			};
			material.metallicFactor = static_cast<float>(sourceMaterial.pbrData.metallicFactor);
			material.roughnessFactor = static_cast<float>(sourceMaterial.pbrData.roughnessFactor);
			material.doubleSided = sourceMaterial.doubleSided;
			if (sourceMaterial.normalTexture.has_value())
			{
				material.normalScale = static_cast<float>(sourceMaterial.normalTexture->scale);
			}
			if (sourceMaterial.occlusionTexture.has_value())
			{
				material.occlusionStrength = static_cast<float>(sourceMaterial.occlusionTexture->strength);
			}

			if (sourceMaterial.pbrData.baseColorTexture.has_value())
			{
				AddTextureRecord(asset, result, sourceMaterial.pbrData.baseColorTexture->textureIndex, GltfTextureUsage::BaseColor, options.packageGuid, packageFolderRelative, result.package.importRoot);
			}
			if (sourceMaterial.pbrData.metallicRoughnessTexture.has_value())
			{
				AddTextureRecord(asset, result, sourceMaterial.pbrData.metallicRoughnessTexture->textureIndex, GltfTextureUsage::MetallicRoughness, options.packageGuid, packageFolderRelative, result.package.importRoot);
			}
			if (sourceMaterial.normalTexture.has_value())
			{
				AddTextureRecord(asset, result, sourceMaterial.normalTexture->textureIndex, GltfTextureUsage::Normal, options.packageGuid, packageFolderRelative, result.package.importRoot);
			}
			if (sourceMaterial.occlusionTexture.has_value())
			{
				AddTextureRecord(asset, result, sourceMaterial.occlusionTexture->textureIndex, GltfTextureUsage::Occlusion, options.packageGuid, packageFolderRelative, result.package.importRoot);
			}
			if (sourceMaterial.emissiveTexture.has_value())
			{
				AddTextureRecord(asset, result, sourceMaterial.emissiveTexture->textureIndex, GltfTextureUsage::Emissive, options.packageGuid, packageFolderRelative, result.package.importRoot);
			}

			MaterialAssetWriteDesc materialDesc{};
			materialDesc.header = CreateDefaultMaterialHeader(options.shaderCollectionId);
			materialDesc.header.baseColorFactor[0] = material.baseColorFactor[0];
			materialDesc.header.baseColorFactor[1] = material.baseColorFactor[1];
			materialDesc.header.baseColorFactor[2] = material.baseColorFactor[2];
			materialDesc.header.baseColorFactor[3] = material.baseColorFactor[3];
			materialDesc.header.emissiveFactor[0] = material.emissiveFactor[0];
			materialDesc.header.emissiveFactor[1] = material.emissiveFactor[1];
			materialDesc.header.emissiveFactor[2] = material.emissiveFactor[2];
			materialDesc.header.metallicFactor = material.metallicFactor;
			materialDesc.header.roughnessFactor = material.roughnessFactor;
			materialDesc.header.normalScale = material.normalScale;
			materialDesc.header.occlusionStrength = material.occlusionStrength;
			if (material.doubleSided)
			{
				materialDesc.header.flags |= static_cast<std::uint32_t>(DoubleSided);
			}

			if (sourceMaterial.pbrData.baseColorTexture.has_value())
			{
				AddMaterialTextureBinding(result, materialDesc, material, sourceMaterial.pbrData.baseColorTexture->textureIndex, GltfTextureUsage::BaseColor);
			}
			if (sourceMaterial.normalTexture.has_value())
			{
				AddMaterialTextureBinding(result, materialDesc, material, sourceMaterial.normalTexture->textureIndex, GltfTextureUsage::Normal);
			}
			if (sourceMaterial.pbrData.metallicRoughnessTexture.has_value())
			{
				AddMaterialTextureBinding(result, materialDesc, material, sourceMaterial.pbrData.metallicRoughnessTexture->textureIndex, GltfTextureUsage::MetallicRoughness);
			}
			if (sourceMaterial.occlusionTexture.has_value())
			{
				AddMaterialTextureBinding(result, materialDesc, material, sourceMaterial.occlusionTexture->textureIndex, GltfTextureUsage::Occlusion);
			}
			if (sourceMaterial.emissiveTexture.has_value())
			{
				AddMaterialTextureBinding(result, materialDesc, material, sourceMaterial.emissiveTexture->textureIndex, GltfTextureUsage::Emissive);
			}

			const std::filesystem::path materialSourceOutputPath = options.assetRoot / material.sourcePath;
			const std::filesystem::path materialCompiledOutputPath = options.buildRoot / material.compiledPath;
			if (!WriteMaterialAsset(materialSourceOutputPath, materialDesc))
			{
				AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "GeneratedMaterialWriteFailed", material.sourcePath.generic_string(), "Failed to write generated Terra material.", "The source material remains visible in diagnostics only.");
			}
			if (!WriteMaterialAsset(materialCompiledOutputPath, materialDesc))
			{
				AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "GeneratedMaterialCompileFailed", material.compiledPath.generic_string(), "Failed to write generated Terra material to the build cache.", "The material source file remains available for reimport diagnostics.");
			}

			result.generatedMaterials.push_back(std::move(material));
		}
		ReportProgress(options, materialEnd, "Materials", "Generated Terra material assets.");

		const float textureStart = 0.38f;
		const float textureEnd = 0.56f;
		const std::size_t textureCount = result.generatedTextures.size();
		ReportProgress(
			options,
			textureStart,
			"Textures",
			"Compiling " + std::to_string(textureCount) + " referenced package texture" + (textureCount == 1u ? "." : "s."));
		std::size_t textureOrdinal = 0u;
		for (GeneratedGltfTextureAsset& texture : result.generatedTextures)
		{
			const float fraction = textureCount == 0u ? 1.0f : static_cast<float>(textureOrdinal) / static_cast<float>(textureCount);
			const std::size_t currentTextureIndex = textureOrdinal + 1u;
			ReportProgress(options,
				textureStart + (textureEnd - textureStart) * fraction,
				BuildIndexedStage("Texture", currentTextureIndex, textureCount, "Textures"),
				BuildIndexedMessage("Texture", currentTextureIndex, textureCount, texture.displayName, "Extracting and compiling package texture."));
			if (!texture.sourceImageIndex.has_value() || *texture.sourceImageIndex >= asset.images.size())
			{
				AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "MissingTextureImage", texture.displayName, "Texture does not reference a valid glTF image.", "The generated material keeps scalar PBR factors.");
				++textureOrdinal;
				continue;
			}

			const fastgltf::Image& image = asset.images[*texture.sourceImageIndex];
			std::vector<std::uint8_t> imageBytes;
			std::string imageExtension;
			const bool hasImageBytes = ExtractImageBytes(asset, image, options.sourcePath.parent_path(), imageBytes, imageExtension);
			const std::filesystem::path textureOutputPath = options.buildRoot / texture.compiledPath;
			const float nestedStart = textureStart + (textureEnd - textureStart) * fraction;
			const float nestedEnd = textureStart + (textureEnd - textureStart) * (textureCount == 0u ? 1.0f : static_cast<float>(textureOrdinal + 1u) / static_cast<float>(textureCount));
			const TextureCompileProgressCallback textureProgress = MakeIndexedNestedProgress(options, nestedStart, nestedEnd, currentTextureIndex, textureCount, texture.displayName);
			const bool compiled = hasImageBytes
				? CompileTextureFromBytes(imageBytes, imageExtension, textureOutputPath, ToTextureFileSemantic(texture.usage), 1, true, TextureFilterType::Lanczos, textureProgress)
				: (!texture.sourcePath.empty() && CompileTexture(options.assetRoot / texture.sourcePath, textureOutputPath, ToTextureFileSemantic(texture.usage), 1, true, TextureFilterType::Lanczos, textureProgress));
			if (!compiled)
			{
				AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "TextureCompileFailed", texture.displayName, "Failed to compile a package texture.", "The generated material keeps the texture binding for reimport diagnostics.");
			}
			++textureOrdinal;
		}
		ReportProgress(options, textureEnd, "Textures", "Package texture compilation finished.");

		std::vector<ExtractedGltfMeshGeometry> meshGeometries;
		meshGeometries.reserve(asset.meshes.size());
		std::vector<std::string> usedMeshOutputNames;
		usedMeshOutputNames.reserve(asset.meshes.size());
		const float meshStart = 0.56f;
		const float meshEnd = 0.78f;
		for (std::size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex)
		{
			const float fraction = asset.meshes.empty() ? 1.0f : static_cast<float>(meshIndex) / static_cast<float>(asset.meshes.size());
			const fastgltf::Mesh& sourceMesh = asset.meshes[meshIndex];
			const std::string meshDisplayName = SanitizeAssetName(std::string(sourceMesh.name), "Mesh_" + std::to_string(meshIndex));
			ReportProgress(options,
				meshStart + (meshEnd - meshStart) * fraction,
				BuildIndexedStage("Mesh", meshIndex + 1u, asset.meshes.size(), "Meshes"),
				BuildIndexedMessage("Mesh", meshIndex + 1u, asset.meshes.size(), meshDisplayName, "Extracting static mesh geometry."));
			GeneratedGltfMeshAsset mesh{};
			mesh.guid = StableChildUuid(options.packageGuid, "gltf-mesh", meshIndex);
			mesh.sourceMeshIndex = meshIndex;
			mesh.sourcePath = options.sourceRelativePath;
			mesh.displayName = meshDisplayName;
			mesh.compiledPath = BuildGeneratedMeshCompiledPath(result.package.importRoot, mesh.displayName, meshIndex, usedMeshOutputNames);
			bool boundsInitialized = false;
			ExtractedGltfMeshGeometry meshGeometry{};

			for (std::size_t primitiveIndex = 0; primitiveIndex < sourceMesh.primitives.size(); ++primitiveIndex)
			{
				const fastgltf::Primitive& primitive = sourceMesh.primitives[primitiveIndex];
				std::vector<RawMeshVertex> primitiveVertices;
				std::vector<std::uint32_t> primitiveIndices;
				if (ExtractPrimitiveGeometry(asset, primitive, primitiveVertices, primitiveIndices))
				{
					const std::uint32_t vertexOffset = static_cast<std::uint32_t>(meshGeometry.vertices.size());
					meshGeometry.vertices.insert(meshGeometry.vertices.end(), primitiveVertices.begin(), primitiveVertices.end());
					for (std::uint32_t index : primitiveIndices)
					{
						meshGeometry.indices.push_back(vertexOffset + index);
					}
					ExpandBounds(mesh, primitiveVertices, boundsInitialized);
				}
				else
				{
					AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "PrimitiveGeometrySkipped", mesh.displayName + "_Primitive" + std::to_string(primitiveIndex), "Primitive geometry could not be extracted as static triangles.", "Other compatible primitives continue importing.");
				}

				GltfMaterialAssignment assignment{};
				assignment.meshGuid = mesh.guid;
				assignment.sourceMeshIndex = meshIndex;
				assignment.sourcePrimitiveIndex = primitiveIndex;
				assignment.slotName = mesh.displayName + "_Primitive" + std::to_string(primitiveIndex);
				if (primitive.materialIndex.has_value() && *primitive.materialIndex < result.generatedMaterials.size())
				{
					assignment.materialGuid = result.generatedMaterials[*primitive.materialIndex].guid;
				}
				else
				{
					AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "MissingPrimitiveMaterial", assignment.slotName, "Primitive has no valid material reference.", "A default material will be used when the mesh is placed.");
				}
				result.materialAssignments.push_back(std::move(assignment));
			}
			result.generatedMeshes.push_back(mesh);
			meshGeometries.push_back(std::move(meshGeometry));
		}
		ReportProgress(options, meshEnd, "Meshes", "Static mesh geometry extracted.");

		if (result.generatedMeshes.empty())
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Error, "NoRenderableGeometry", options.sourceRelativePath.generic_string(), "No renderable static mesh geometry was found in the package.");
		}
		else if (std::none_of(meshGeometries.begin(), meshGeometries.end(), [](const ExtractedGltfMeshGeometry& geometry) {
			return geometry.HasTriangles();
		}))
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Error, "NoRenderableGeometry", options.sourceRelativePath.generic_string(), "No compatible static triangle geometry was extracted from the package.");
		}
		else
		{
			VirtualGeometryConeSafetyCookSettings coneSafetySettings{};
			coneSafetySettings.proveOneSidedOpaqueClusters = false;
			coneSafetySettings.disabledReason = "glTF generated meshes wait for per-cluster material sidedness proof before cone culling is enabled.";
			const float compileStart = 0.80f;
			const float compileEnd = 0.90f;
			const std::size_t compileCount = result.generatedMeshes.size();
			for (std::size_t meshIndex = 0u; meshIndex < result.generatedMeshes.size(); ++meshIndex)
			{
				GeneratedGltfMeshAsset& mesh = result.generatedMeshes[meshIndex];
				const ExtractedGltfMeshGeometry& geometry = meshGeometries[meshIndex];
				const float fraction = compileCount == 0u ? 1.0f : static_cast<float>(meshIndex) / static_cast<float>(compileCount);
				const float nextFraction = compileCount == 0u ? 1.0f : static_cast<float>(meshIndex + 1u) / static_cast<float>(compileCount);
				const float nestedStart = compileStart + (compileEnd - compileStart) * fraction;
				const float nestedEnd = compileStart + (compileEnd - compileStart) * nextFraction;

				ReportProgress(options,
					nestedStart,
					BuildIndexedStage("Mesh", meshIndex + 1u, compileCount, "Compiling"),
					BuildIndexedMessage("Mesh", meshIndex + 1u, compileCount, mesh.displayName, "Building child TEMF and virtual geometry data."));
				if (!geometry.HasTriangles())
				{
					AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "GeneratedMeshGeometrySkipped", mesh.displayName, "Generated mesh child has no compatible static triangle geometry.", "Other generated meshes continue importing.");
					continue;
				}

				MeshCompileReport compileReport{};
				const bool requiresDeformableCook = MeshRequiresDeformableCook(asset, mesh.sourceMeshIndex);
				const GeneratedModelMeshVirtualGeometrySettings* resolvedSettings =
					FindMeshVirtualGeometrySettings(options, mesh.guid);
				const VirtualGeometryAdaptiveCompileSettings compileSettings =
					resolvedSettings != nullptr
						? ToCompileSettings(*resolvedSettings)
						: MakeDefaultVirtualGeometryAdaptiveCompileSettings();
				const VirtualGeometryAdaptiveCompileSettings* compileSettingsPtr =
					resolvedSettings != nullptr ? &compileSettings : nullptr;
				const std::filesystem::path compiledMeshOutputPath = requiresDeformableCook
					? StaticDeformableCandidatePath(options, mesh.compiledPath)
					: options.buildRoot / mesh.compiledPath;
				if (!CompileMeshFromRaw(
					mesh.sourcePath,
					compiledMeshOutputPath,
					geometry.vertices,
					geometry.indices,
					true,
					&compileReport,
					60.0f,
					1e-6f,
					compileSettingsPtr,
					MakeNestedProgress(options, nestedStart, nestedEnd),
					&coneSafetySettings))
				{
					AddDiagnostic(result, GltfDiagnosticSeverity::Error, "MeshCompileFailed", mesh.compiledPath.generic_string(), "Extracted glTF mesh geometry failed Terra mesh compilation.");
					continue;
				}

				mesh.virtualGeometrySettings = ToGeneratedSettings(
					compileReport.virtualGeometry.adaptiveSettings,
					resolvedSettings != nullptr);
				PopulateVirtualGeometryTargetDiagnostics(compileReport, mesh);
				PopulateGeneratedMeshVirtualGeometryMetadata(compiledMeshOutputPath, mesh, result.diagnostics);
				// Direct-resident TEMFPRD1 meshes intentionally have no virtual
				// hierarchy, but their automation record still needs the source
				// triangle fact for truthful import evidence. Recover it from the
				// immutable extracted index stream when the canonical runtime
				// metadata does not publish that advisory summary.
				if(mesh.virtualGeometrySourceTriangles == 0u
					&& !geometry.indices.empty())
				{
					mesh.virtualGeometrySourceTriangles =
						static_cast<std::uint32_t>(geometry.indices.size() / 3u);
				}
				const std::uint32_t materialAssignmentCount = CountMaterialAssignmentsForMesh(result, mesh.guid);
				PopulatePackageMaterialCoverageDiagnostics(mesh, materialAssignmentCount);
				PopulateVirtualGeometryCoverageTags(mesh, result.package.packageKind, materialAssignmentCount);
			}
			ReportProgress(options, compileEnd, "Mesh Metadata", "Generated mesh metadata updated.");
		}

		ReportProgress(options, 0.94f, "Diagnostics", "Finalizing package diagnostics.");
		PromoteStatusFromDiagnostics(result);
		return result;
	}

	GltfAnimatedImportResult ImportGltfPackageWithAnimation(const GltfPackageImportOptions& options)
	{
		GltfAnimatedImportResult result;
		result.package = ImportStaticGltfPackage(options);
		result.animation.publishedAnimationRoot = result.package.package.importRoot / "Animation";
		if (result.package.package.sourceHashHex.empty())
		{
			// The legacy path already produced the actionable open/parse diagnostic.
			// A second parse cannot add animation evidence when no source bytes exist.
			return result;
		}

		ReportProgress(options, 0.95F, "Animation", "Parsing skeletal hierarchy, clips, morph targets, and source semantics.");
		auto gltfFile = fastgltf::GltfDataBuffer::FromPath(options.sourcePath);
		if (!gltfFile)
		{
			AddDiagnostic(result.package, GltfDiagnosticSeverity::Error, "AnimationSourceOpenFailed", options.sourceRelativePath.generic_string(), "The animation pass could not reopen the glTF package after static import.");
			PromoteStatusFromDiagnostics(result.package);
			return result;
		}

		fastgltf::Parser parser(
			fastgltf::Extensions::KHR_materials_unlit |
			fastgltf::Extensions::KHR_texture_transform |
			fastgltf::Extensions::KHR_lights_punctual);
		constexpr fastgltf::Options parserOptions =
			fastgltf::Options::DontRequireValidAssetMember |
			fastgltf::Options::AllowDouble |
			fastgltf::Options::LoadExternalBuffers |
			fastgltf::Options::LoadGLBBuffers |
			fastgltf::Options::GenerateMeshIndices |
			fastgltf::Options::DecomposeNodeMatrices;
		auto parsed = parser.loadGltf(gltfFile.get(), options.sourcePath.parent_path(), parserOptions);
		if (parsed.error() != fastgltf::Error::None)
		{
			AddDiagnostic(result.package, GltfDiagnosticSeverity::Error, "AnimationParseFailed", options.sourceRelativePath.generic_string(), std::string("The animation pass could not parse the package: ") + std::string(fastgltf::getErrorMessage(parsed.error())));
			PromoteStatusFromDiagnostics(result.package);
			return result;
		}

		fastgltf::Asset& asset = parsed.get();
		ParsedGltfMetadata metadata = ReadGltfMetadata(options.sourcePath);
		if (!std::isfinite(metadata.coordinateConversion.sourceUnitScaleMeters)
			|| metadata.coordinateConversion.sourceUnitScaleMeters <= 0.0F)
		{
			AddDiagnostic(result.package, GltfDiagnosticSeverity::Warning, "InvalidSourceUnitScale", "asset/extras/sourceUnitScale", "The declared source unit scale is non-finite or non-positive; one meter per source unit was used.");
			metadata.coordinateConversion.sourceUnitScaleMeters = 1.0F;
		}
		const Animation::AnimationFloat3 up = AxisVector(metadata.coordinateConversion.sourceUpAxis);
		const Animation::AnimationFloat3 forward = AxisVector(metadata.coordinateConversion.sourceForwardAxis);
		if (std::abs(Dot(up, forward)) > 0.01F)
		{
			AddDiagnostic(result.package, GltfDiagnosticSeverity::Warning, "InvalidSourceAxes", "asset/extras", "Source up and forward axes are parallel; a perpendicular project-forward axis was selected.");
			metadata.coordinateConversion.sourceForwardAxis = metadata.coordinateConversion.sourceUpAxis == Animation::SkeletonSourceAxis::PositiveZ
				|| metadata.coordinateConversion.sourceUpAxis == Animation::SkeletonSourceAxis::NegativeZ
				? Animation::SkeletonSourceAxis::PositiveY
				: Animation::SkeletonSourceAxis::PositiveZ;
		}
		metadata.coordinateConversion.appliedExactlyOnce = true;
		result.animation.coordinateConversion = metadata.coordinateConversion;
		const GltfCoordinateConverter converter(metadata.coordinateConversion);

		const std::vector<std::optional<std::size_t>> parents = BuildNodeParents(asset, result.package);
		result.animation.nodes = BuildImportedNodes(asset, metadata, converter, parents, result.package);
		const MorphTargetIdMap morphTargetIds = BuildImportedMorphTargets(asset, metadata, result.animation, result.package);
		const std::vector<GltfSkeletonContext> skeletonContexts = BuildAnimationSkeletons(
			asset, result.animation.nodes, parents, metadata, converter, options, result.animation, result.package);
		BuildAnimationClips(asset, metadata, converter, options, result.animation.nodes,
			morphTargetIds, skeletonContexts, result.animation, result.package);

		// Public paths include the package-generated root, while staging uses only
		// the leaf filename beneath one atomic Animation directory.
		for (GltfGeneratedSkeleton& skeleton : result.animation.skeletons)
		{
			skeleton.compiledPath = result.package.package.importRoot / skeleton.compiledPath;
		}
		for (GltfGeneratedAnimationClip& clip : result.animation.animationClips)
		{
			clip.compiledPath = result.package.package.importRoot / clip.compiledPath;
		}

		RemoveSupersededAnimationDiagnostics(result.package);
		std::vector<PreparedDeformableMesh> deformableMeshes;
		const bool deformablePrepared = PrepareDeformableMeshes(
			asset, converter, options, skeletonContexts, result.animation, result.package, deformableMeshes);
		CleanupStaticDeformableCandidates(asset, options, result.package);
		if (!deformablePrepared)
		{
			deformableMeshes.clear();
		}
		const bool hasPublicationBlockingError = std::any_of(
			result.package.diagnostics.begin(), result.package.diagnostics.end(), [](const GltfImportDiagnostic& diagnostic) {
				return diagnostic.severity == GltfDiagnosticSeverity::Error;
			});
		std::unordered_map<TerraEngine::UUID, std::string> contentHashes;
		const bool published = deformablePrepared && !hasPublicationBlockingError
			&& PublishAnimationArtifactsAtomically(options, result.animation, result.package, contentHashes, deformableMeshes);
		if (hasPublicationBlockingError)
		{
			CleanupPreparedDeformableCandidates(deformableMeshes);
		}
		PopulateAnimationManifest(options, result.animation, contentHashes, deformableMeshes, result.package);
		if (!published)
		{
			result.package.animationManifest.preservedLastKnownGood = true;
		}
		if (!asset.skins.empty() || !asset.animations.empty() || !result.animation.morphTargets.empty())
		{
			std::ostringstream summary;
			summary << "Preserved " << result.animation.skeletons.size() << " skeleton"
				<< (result.animation.skeletons.size() == 1U ? "" : "s") << ", "
				<< result.animation.animationClips.size() << " clip"
				<< (result.animation.animationClips.size() == 1U ? "" : "s") << ", and "
				<< result.animation.morphTargets.size() << " morph target"
				<< (result.animation.morphTargets.size() == 1U ? "" : "s") << ".";
			AddDiagnostic(result.package, GltfDiagnosticSeverity::Info, "AnimatedPackageImported", "animation", summary.str());
		}
		ReportProgress(options, 1.0F, "Animation", "Animated glTF package import finished.");
		PromoteStatusFromDiagnostics(result.package);
		return result;
	}

	GltfImportResult ImportGltfPackage(const GltfPackageImportOptions& options)
	{
		return ImportGltfPackageWithAnimation(options).package;
	}
}
