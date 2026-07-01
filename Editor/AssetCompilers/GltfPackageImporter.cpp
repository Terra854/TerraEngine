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

		auto parsed = parser.loadGltf(gltfFile.get(), options.sourcePath.parent_path(), parserOptions);
		if (parsed.error() != fastgltf::Error::None)
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Error, "ParseFailed", options.sourceRelativePath.generic_string(), std::string("Failed to parse glTF package: ") + std::string(fastgltf::getErrorMessage(parsed.error())));
			PromoteStatusFromDiagnostics(result);
			return result;
		}

		fastgltf::Asset& asset = parsed.get();
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
		for (std::size_t materialIndex = 0; materialIndex < asset.materials.size(); ++materialIndex)
		{
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

		for (GeneratedGltfTextureAsset& texture : result.generatedTextures)
		{
			if (!texture.sourceImageIndex.has_value() || *texture.sourceImageIndex >= asset.images.size())
			{
				AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "MissingTextureImage", texture.displayName, "Texture does not reference a valid glTF image.", "The generated material keeps scalar PBR factors.");
				continue;
			}

			const fastgltf::Image& image = asset.images[*texture.sourceImageIndex];
			std::vector<std::uint8_t> imageBytes;
			std::string imageExtension;
			const bool hasImageBytes = ExtractImageBytes(asset, image, options.sourcePath.parent_path(), imageBytes, imageExtension);
			const std::filesystem::path textureOutputPath = options.buildRoot / texture.compiledPath;
			const bool compiled = hasImageBytes
				? CompileTextureFromBytes(imageBytes, imageExtension, textureOutputPath, ToTextureFileSemantic(texture.usage))
				: (!texture.sourcePath.empty() && CompileTexture(options.assetRoot / texture.sourcePath, textureOutputPath, ToTextureFileSemantic(texture.usage)));
			if (!compiled)
			{
				AddDiagnostic(result, GltfDiagnosticSeverity::Warning, "TextureCompileFailed", texture.displayName, "Failed to compile a package texture.", "The generated material keeps the texture binding for reimport diagnostics.");
			}
		}

		std::vector<RawMeshVertex> packageVertices;
		std::vector<std::uint32_t> packageIndices;
		for (std::size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex)
		{
			const fastgltf::Mesh& sourceMesh = asset.meshes[meshIndex];
			GeneratedGltfMeshAsset mesh{};
			mesh.guid = StableChildUuid(options.packageGuid, "gltf-mesh", meshIndex);
			mesh.sourceMeshIndex = meshIndex;
			mesh.sourcePath = options.sourceRelativePath;
			mesh.compiledPath = options.sourceRelativePath;
			mesh.compiledPath.replace_extension(".temf");
			mesh.displayName = SanitizeAssetName(std::string(sourceMesh.name), "Mesh_" + std::to_string(meshIndex));
			bool boundsInitialized = false;

			for (std::size_t primitiveIndex = 0; primitiveIndex < sourceMesh.primitives.size(); ++primitiveIndex)
			{
				const fastgltf::Primitive& primitive = sourceMesh.primitives[primitiveIndex];
				std::vector<RawMeshVertex> primitiveVertices;
				std::vector<std::uint32_t> primitiveIndices;
				if (ExtractPrimitiveGeometry(asset, primitive, primitiveVertices, primitiveIndices))
				{
					const std::uint32_t vertexOffset = static_cast<std::uint32_t>(packageVertices.size());
					packageVertices.insert(packageVertices.end(), primitiveVertices.begin(), primitiveVertices.end());
					for (std::uint32_t index : primitiveIndices)
					{
						packageIndices.push_back(vertexOffset + index);
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
		}

		if (result.generatedMeshes.empty())
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Error, "NoRenderableGeometry", options.sourceRelativePath.generic_string(), "No renderable static mesh geometry was found in the package.");
		}
		else if (packageVertices.empty() || packageIndices.empty())
		{
			AddDiagnostic(result, GltfDiagnosticSeverity::Error, "NoRenderableGeometry", options.sourceRelativePath.generic_string(), "No compatible static triangle geometry was extracted from the package.");
		}
		else
		{
			std::filesystem::path compiledPackagePath = options.sourceRelativePath;
			compiledPackagePath.replace_extension(".temf");
			MeshCompileReport compileReport{};
			const VirtualGeometryAdaptiveCompileSettings compileSettings = ToCompileSettings(options.virtualGeometrySettings);
			const VirtualGeometryAdaptiveCompileSettings* compileSettingsPtr = options.hasVirtualGeometrySettings ? &compileSettings : nullptr;
			if (!CompileMeshFromRaw(options.sourceRelativePath, options.buildRoot / compiledPackagePath, packageVertices, packageIndices, true, &compileReport, 60.0f, 1e-6f, compileSettingsPtr))
			{
				AddDiagnostic(result, GltfDiagnosticSeverity::Error, "MeshCompileFailed", compiledPackagePath.generic_string(), "Extracted glTF geometry failed Terra mesh compilation.");
			}
			for (GeneratedGltfMeshAsset& mesh : result.generatedMeshes)
			{
				mesh.virtualGeometrySettings = ToGeneratedSettings(compileReport.virtualGeometry.adaptiveSettings, options.hasVirtualGeometrySettings);
				PopulateVirtualGeometryTargetDiagnostics(compileReport, mesh);
				PopulateGeneratedMeshVirtualGeometryMetadata(options.buildRoot / compiledPackagePath, mesh, result.diagnostics);
			}
		}

		PromoteStatusFromDiagnostics(result);
		return result;
	}
}
