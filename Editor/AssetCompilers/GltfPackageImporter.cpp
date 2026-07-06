// GltfPackageImporter.cpp - fastgltf-backed package import implementation.
//
// The current implementation establishes the package-aware import contract:
// fastgltf parses the source package, Terra records meshes/materials/textures,
// writes generated material assets, and emits diagnostics for unsupported data.
// Later tasks reuse this entry point for raw geometry and image-byte extraction.

#include "GltfPackageImporter.h"

#include "MaterialAssetWriter.h"
#include "MeshCompiler.h"
#include "TextureCompiler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

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
	}

	bool IsGltfPackageSourceExtension(const std::filesystem::path& filePath)
	{
		const std::string ext = NormalizeExtension(filePath);
		return ext == ".gltf" || ext == ".glb";
	}

	GltfImportResult ImportGltfPackage(const GltfPackageImportOptions& options)
	{
		GltfImportResult result{};
		result.packageGuid = options.packageGuid;
		result.package.sourcePath = options.sourceRelativePath;
		result.package.packageKind = NormalizeExtension(options.sourcePath) == ".glb" ? GltfPackageKind::GltfBinary : GltfPackageKind::GltfText;
		result.package.sourceHashHex = ComputeFileHashHex(options.sourcePath);
		result.package.importRoot = options.sourceRelativePath.parent_path() / (options.sourcePath.stem().string() + "_Generated");

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
			const VirtualGeometryAdaptiveCompileSettings compileSettings = ToCompileSettings(options.virtualGeometrySettings);
			const VirtualGeometryAdaptiveCompileSettings* compileSettingsPtr = options.hasVirtualGeometrySettings ? &compileSettings : nullptr;
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
				if (!CompileMeshFromRaw(
					mesh.sourcePath,
					options.buildRoot / mesh.compiledPath,
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

				mesh.virtualGeometrySettings = ToGeneratedSettings(compileReport.virtualGeometry.adaptiveSettings, options.hasVirtualGeometrySettings);
				PopulateVirtualGeometryTargetDiagnostics(compileReport, mesh);
				PopulateGeneratedMeshVirtualGeometryMetadata(options.buildRoot / mesh.compiledPath, mesh, result.diagnostics);
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
}
