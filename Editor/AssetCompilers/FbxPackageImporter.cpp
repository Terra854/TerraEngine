// FbxPackageImporter.cpp - Assimp-backed FBX package importer.
//
// The importer performs worker-safe preparation: it decodes Assimp's scene,
// writes generated material/texture/build outputs, and returns a sidecar result.
// The AssetDatabase remains responsible for the protected registration boundary
// that makes those outputs visible to the editor and ResourceManager.

#include "FbxPackageImporter.h"

#include "MaterialAssetWriter.h"
#include "MeshCompiler.h"
#include "TextureCompiler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/ProgressHandler.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		struct TextureImportRecord
		{
			GeneratedModelTextureAsset texture;
			bool hasImageBytes = false;
			std::vector<std::uint8_t> imageBytes;
			std::string imageExtension;
			std::filesystem::path absoluteSourcePath;
		};

		std::string NormalizeExtension(const std::filesystem::path& filePath)
		{
			std::string ext = filePath.extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
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

		void PopulateVirtualGeometryCoverageTags(GeneratedModelMeshAsset& mesh, std::uint32_t sourceMaterialAssignmentCount)
		{
			// Keep broad asset-coverage metadata with the generated mesh so the
			// package sidecar can prove FBX import coverage without loading a
			// large manual stress asset in every test run.
			mesh.virtualGeometryCoverageTags.clear();
			AddCoverageTag(mesh, "package-static-geometry");
			AddCoverageTag(mesh, "package-fbx");
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

		void AddDiagnostic(
			ModelPackageImportResult& result,
			ModelDiagnosticSeverity severity,
			std::string code,
			std::string source,
			std::string message,
			std::string fallback = {})
		{
			result.diagnostics.push_back({ severity, std::move(code), std::move(source), std::move(message), std::move(fallback) });
		}

		void ReportProgress(const FbxPackageImportOptions& options, float progress, std::string_view stage, std::string_view message)
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
				<< ", " << textureCount << " texture reference" << (textureCount == 1u ? "" : "s")
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

		class FbxAssimpProgressHandler final : public Assimp::ProgressHandler
		{
		public:
			explicit FbxAssimpProgressHandler(const FbxPackageImportOptions& options)
				: m_Options(options) {}

			bool Update(float percentage = -1.0f) override
			{
				try
				{
					const float normalized = percentage >= 0.0f ? std::clamp(percentage, 0.0f, 1.0f) : 0.0f;
					ReportProgress(m_Options, 0.12f + (0.34f - 0.12f) * normalized, "Parsing", "Parsing FBX scene with Assimp.");
					return true;
				}
				catch (...)
				{
					return false;
				}
			}

		private:
			const FbxPackageImportOptions& m_Options;
		};

		auto MakeNestedProgress(const FbxPackageImportOptions& options, float start, float end)
		{
			return [&options, start, end](float progress, std::string_view stage, std::string_view message) {
				ReportProgress(options, start + (end - start) * std::clamp(progress, 0.0f, 1.0f), stage, message);
			};
		}

		TextureCompileProgressCallback MakeIndexedTextureProgress(TextureCompileProgressCallback progressCallback, std::size_t currentIndex, std::size_t totalCount, std::string displayName)
		{
			return [progressCallback = std::move(progressCallback), currentIndex, totalCount, displayName = std::move(displayName)](float progress, std::string_view stage, std::string_view message) {
				if (progressCallback)
				{
					progressCallback(
						progress,
						BuildIndexedStage("Texture", currentIndex, totalCount, stage),
						BuildIndexedMessage("Texture", currentIndex, totalCount, displayName, message));
				}
			};
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

		TextureSemantic ToMaterialSemantic(ModelTextureUsage usage)
		{
			switch (usage)
			{
			case ModelTextureUsage::BaseColor: return TextureSemantic::Base;
			case ModelTextureUsage::Normal: return TextureSemantic::Normal;
			case ModelTextureUsage::MetallicRoughness: return TextureSemantic::MetallicRoughness;
			case ModelTextureUsage::Occlusion: return TextureSemantic::Occlusion;
			case ModelTextureUsage::Emissive: return TextureSemantic::Emissive;
			case ModelTextureUsage::Unknown: return TextureSemantic::Base;
			}
			return TextureSemantic::Base;
		}

		ModelTextureSemantic ToTextureSemantic(ModelTextureUsage usage)
		{
			return (usage == ModelTextureUsage::BaseColor || usage == ModelTextureUsage::Emissive)
				? ModelTextureSemantic::Color
				: ModelTextureSemantic::Data;
		}

		TextureFileSemantic ToTextureFileSemantic(ModelTextureUsage usage)
		{
			return (usage == ModelTextureUsage::BaseColor || usage == ModelTextureUsage::Emissive)
				? TextureFileSemantic::Color
				: TextureFileSemantic::Data;
		}

		std::string EmbeddedTextureExtension(const aiTexture& texture)
		{
			std::string hint = texture.achFormatHint;
			if (hint.empty())
			{
				return ".png";
			}
			std::transform(hint.begin(), hint.end(), hint.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return hint.front() == '.' ? hint : "." + hint;
		}

		bool TryMakeProjectRelative(const std::filesystem::path& root, const std::filesystem::path& path, std::filesystem::path& outRelative)
		{
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
			if (ec || relative.empty())
			{
				return false;
			}
			for (const auto& part : relative)
			{
				if (part == "..")
				{
					return false;
				}
			}
			outRelative = relative;
			return true;
		}

		aiVector3D TransformPoint(const aiMatrix4x4& transform, const aiVector3D& value)
		{
			return {
				transform.a1 * value.x + transform.a2 * value.y + transform.a3 * value.z + transform.a4,
				transform.b1 * value.x + transform.b2 * value.y + transform.b3 * value.z + transform.b4,
				transform.c1 * value.x + transform.c2 * value.y + transform.c3 * value.z + transform.c4
			};
		}

		aiVector3D TransformDirection(const aiMatrix4x4& transform, const aiVector3D& value)
		{
			aiVector3D direction{
				transform.a1 * value.x + transform.a2 * value.y + transform.a3 * value.z,
				transform.b1 * value.x + transform.b2 * value.y + transform.b3 * value.z,
				transform.c1 * value.x + transform.c2 * value.y + transform.c3 * value.z
			};
			direction.NormalizeSafe();
			return direction;
		}

		void ExpandBounds(GeneratedModelMeshAsset& mesh, const RawMeshVertex& vertex, bool& initialized)
		{
			if (!initialized)
			{
				mesh.localBoundsMin = vertex.position;
				mesh.localBoundsMax = vertex.position;
				initialized = true;
				return;
			}

			for (std::size_t component = 0; component < 3u; ++component)
			{
				mesh.localBoundsMin[component] = std::min(mesh.localBoundsMin[component], vertex.position[component]);
				mesh.localBoundsMax[component] = std::max(mesh.localBoundsMax[component], vertex.position[component]);
			}
		}

		std::string NodePath(const std::string& parent, const aiNode& node)
		{
			const std::string current = node.mName.C_Str();
			if (parent.empty())
			{
				return current.empty() ? std::string("Root") : current;
			}
			return parent + "/" + (current.empty() ? std::string("Node") : current);
		}

		void AddUnsupportedDiagnostics(const aiScene& scene, ModelPackageImportResult& result)
		{
			if (scene.HasAnimations())
			{
				AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "UnsupportedAnimationIgnored", "animations", "Animation data is not imported by this static FBX feature.", "Static mesh data was imported.");
			}
			if (scene.HasCameras())
			{
				AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "UnsupportedCameraIgnored", "cameras", "Camera data is not imported by this static FBX feature.", "Renderable meshes were imported.");
			}
			if (scene.HasLights())
			{
				AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "UnsupportedLightIgnored", "lights", "Light data is not imported by this static FBX feature.", "Renderable meshes were imported.");
			}
			for (unsigned int meshIndex = 0; meshIndex < scene.mNumMeshes; ++meshIndex)
			{
				const aiMesh* mesh = scene.mMeshes[meshIndex];
				if (mesh != nullptr && mesh->HasBones())
				{
					AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "UnsupportedSkeletonIgnored", mesh->mName.C_Str(), "Skeleton and skin weights are not imported by this static FBX feature.", "The bind-pose/static mesh geometry was imported.");
					return;
				}
			}
		}

		bool TryReadTextureReference(const aiMaterial& material, aiTextureType type, aiString& outPath)
		{
			return material.GetTextureCount(type) > 0u && material.GetTexture(type, 0u, &outPath) == AI_SUCCESS && outPath.length > 0u;
		}

		std::array<aiTextureType, 8> ImportedTextureTypes()
		{
			return {
				aiTextureType_BASE_COLOR,
				aiTextureType_DIFFUSE,
				aiTextureType_NORMALS,
				aiTextureType_HEIGHT,
				aiTextureType_METALNESS,
				aiTextureType_DIFFUSE_ROUGHNESS,
				aiTextureType_AMBIENT_OCCLUSION,
				aiTextureType_EMISSIVE
			};
		}

		std::size_t CountMaterialTextureReferences(const aiScene& scene)
		{
			std::size_t textureCount = 0u;
			for (unsigned int materialIndex = 0; materialIndex < scene.mNumMaterials; ++materialIndex)
			{
				const aiMaterial* material = scene.mMaterials[materialIndex];
				if (material == nullptr)
				{
					continue;
				}

				for (const aiTextureType textureType : ImportedTextureTypes())
				{
					aiString path;
					if (TryReadTextureReference(*material, textureType, path))
					{
						++textureCount;
					}
				}
			}
			return textureCount;
		}

		void AddMaterialTextureBinding(
			MaterialAssetWriteDesc& desc,
			GeneratedModelMaterialAsset& material,
			const GeneratedModelTextureAsset& texture,
			ModelTextureUsage usage)
		{
			TextureBinding binding{};
			binding.semantic = ToMaterialSemantic(usage);
			binding.textureUuid = texture.guid;
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
			material.textureGuids.push_back(texture.guid);
		}

		std::optional<TextureImportRecord> BuildTextureRecord(
			const FbxPackageImportOptions& options,
			const aiScene& scene,
			ModelPackageImportResult& result,
			const aiString& texturePath,
			ModelTextureUsage usage,
			std::size_t textureOrdinal)
		{
			TextureImportRecord record{};
			record.texture.guid = StableChildUuid(options.packageGuid, "fbx-texture", textureOrdinal);
			record.texture.sourceTextureIndex = textureOrdinal;
			record.texture.sourceImageIndex = textureOrdinal;
			record.texture.usage = usage;
			record.texture.semantic = ToTextureSemantic(usage);
			record.texture.displayName = SanitizeAssetName(std::filesystem::path(texturePath.C_Str()).stem().string(), "Texture_" + std::to_string(textureOrdinal));
			record.texture.compiledPath = result.package.importRoot / (record.texture.displayName + ".tetf");

			const std::string rawReference = texturePath.C_Str();
			if (!rawReference.empty() && rawReference.front() == '*')
			{
				const int textureIndex = std::atoi(rawReference.c_str() + 1);
				if (textureIndex < 0 || static_cast<unsigned int>(textureIndex) >= scene.mNumTextures || scene.mTextures[textureIndex] == nullptr)
				{
					AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "MissingEmbeddedTexture", rawReference, "FBX material references an embedded texture that Assimp did not provide.", "The generated material keeps scalar factors only.");
					return std::nullopt;
				}

				const aiTexture& embedded = *scene.mTextures[textureIndex];
				if (embedded.mHeight != 0u)
				{
					AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "UnsupportedEmbeddedTexturePixels", rawReference, "Uncompressed embedded FBX texture pixels are not decoded by this importer yet.", "The generated material keeps scalar factors only.");
					return std::nullopt;
				}

				record.imageExtension = EmbeddedTextureExtension(embedded);
				const auto* bytes = reinterpret_cast<const std::uint8_t*>(embedded.pcData);
				record.imageBytes.assign(bytes, bytes + embedded.mWidth);
				record.hasImageBytes = !record.imageBytes.empty();
				record.texture.displayName = SanitizeAssetName(record.texture.displayName, "EmbeddedTexture_" + std::to_string(textureOrdinal));
				record.texture.sourcePath = result.package.importRoot / (record.texture.displayName + record.imageExtension);
				return record.hasImageBytes ? std::optional<TextureImportRecord>(std::move(record)) : std::nullopt;
			}

			std::filesystem::path sourceReference = std::filesystem::path(rawReference).lexically_normal();
			std::filesystem::path absoluteTexturePath = sourceReference.is_absolute()
				? sourceReference
				: (options.sourcePath.parent_path() / sourceReference).lexically_normal();
			if (!std::filesystem::exists(absoluteTexturePath))
			{
				AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "MissingTextureFile", rawReference, "FBX material references a texture file that could not be found.", "The generated material keeps scalar factors only.");
				return std::nullopt;
			}

			std::filesystem::path relativeTexturePath;
			if (!TryMakeProjectRelative(options.assetRoot, std::filesystem::absolute(absoluteTexturePath), relativeTexturePath))
			{
				AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "TextureOutsideProject", absoluteTexturePath.generic_string(), "FBX texture reference is outside the project Assets folder.", "Move or copy the texture beside the FBX before reimporting.");
				return std::nullopt;
			}

			record.absoluteSourcePath = std::filesystem::absolute(absoluteTexturePath);
			record.texture.sourcePath = relativeTexturePath;
			record.texture.displayName = SanitizeAssetName(record.texture.sourcePath.stem().string(), record.texture.displayName);
			record.texture.compiledPath = record.texture.sourcePath;
			record.texture.compiledPath.replace_extension(".tetf");
			result.package.externalDependencies.push_back(relativeTexturePath);
			return record;
		}

		void ImportMaterialTexture(
			const FbxPackageImportOptions& options,
			const aiScene& scene,
			ModelPackageImportResult& result,
			MaterialAssetWriteDesc& materialDesc,
			GeneratedModelMaterialAsset& material,
			const aiMaterial& sourceMaterial,
			aiTextureType textureType,
			ModelTextureUsage usage,
			std::size_t& textureOrdinal,
			std::size_t totalTextureCount,
			TextureCompileProgressCallback progressCallback)
		{
			aiString path;
			if (!TryReadTextureReference(sourceMaterial, textureType, path))
			{
				return;
			}

			const std::size_t currentTextureOrdinal = textureOrdinal++;
			const std::size_t currentTextureIndex = currentTextureOrdinal + 1u;
			ReportProgress(
				options,
				0.30f + (0.42f - 0.30f) * (totalTextureCount == 0u ? 1.0f : static_cast<float>(currentTextureOrdinal) / static_cast<float>(totalTextureCount)),
				BuildIndexedStage("Texture", currentTextureIndex, totalTextureCount, "Textures"),
				BuildIndexedMessage("Texture", currentTextureIndex, totalTextureCount, std::filesystem::path(path.C_Str()).stem().string(), "Preparing FBX texture reference."));
			std::optional<TextureImportRecord> record = BuildTextureRecord(options, scene, result, path, usage, currentTextureOrdinal);
			if (!record.has_value())
			{
				return;
			}

			const std::filesystem::path outputPath = options.buildRoot / record->texture.compiledPath;
			const TextureCompileProgressCallback indexedProgress = MakeIndexedTextureProgress(std::move(progressCallback), currentTextureIndex, totalTextureCount, record->texture.displayName);
			const bool compiled = record->hasImageBytes
				? CompileTextureFromBytes(record->imageBytes, record->imageExtension, outputPath, ToTextureFileSemantic(usage), 1, true, TextureFilterType::Lanczos, indexedProgress)
				: CompileTexture(record->absoluteSourcePath, outputPath, ToTextureFileSemantic(usage), 1, true, TextureFilterType::Lanczos, indexedProgress);
			if (!compiled)
			{
				AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "TextureCompileFailed", record->texture.displayName, "Failed to compile an FBX package texture.", "The generated material keeps scalar factors only.");
				return;
			}

			if (record->hasImageBytes)
			{
				const std::filesystem::path extractedPath = options.assetRoot / record->texture.sourcePath;
				std::filesystem::create_directories(extractedPath.parent_path());
				std::ofstream extracted(extractedPath, std::ios::binary);
				extracted.write(reinterpret_cast<const char*>(record->imageBytes.data()), static_cast<std::streamsize>(record->imageBytes.size()));
				if (!extracted)
				{
					AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "EmbeddedTextureWriteFailed", record->texture.sourcePath.generic_string(), "Failed to write extracted embedded FBX texture bytes.", "The compiled texture remains available in the build cache.");
				}
			}

			AddMaterialTextureBinding(materialDesc, material, record->texture, usage);
			result.generatedTextures.push_back(std::move(record->texture));
		}

		void ImportMaterials(const FbxPackageImportOptions& options, const aiScene& scene, ModelPackageImportResult& result, std::size_t totalTextureCount)
		{
			const unsigned int materialCount = std::max(1u, scene.mNumMaterials);
			std::size_t textureOrdinal = 0u;
			ReportProgress(
				options,
				0.24f,
				"Materials",
				"Generating " + std::to_string(materialCount) + " material" + (materialCount == 1u ? "" : "s") + " and compiling " + std::to_string(totalTextureCount) + " texture reference" + (totalTextureCount == 1u ? "." : "s."));
			for (unsigned int materialIndex = 0; materialIndex < materialCount; ++materialIndex)
			{
				const float fraction = static_cast<float>(materialIndex) / static_cast<float>(materialCount);
				ReportProgress(options,
					0.24f + (0.30f - 0.24f) * fraction,
					"Materials",
					"Generating Terra material and texture assets.");
				const aiMaterial* sourceMaterial = materialIndex < scene.mNumMaterials ? scene.mMaterials[materialIndex] : nullptr;
				const std::string fallbackName = "Material_" + std::to_string(materialIndex);
				std::string materialName = fallbackName;
				if (sourceMaterial != nullptr)
				{
					aiString name;
					if (sourceMaterial->Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length > 0u)
					{
						materialName = name.C_Str();
					}
				}
				materialName = SanitizeAssetName(materialName, fallbackName);

				GeneratedModelMaterialAsset material{};
				material.guid = StableChildUuid(options.packageGuid, "fbx-material", materialIndex);
				material.sourceMaterialIndex = materialIndex;
				material.displayName = materialName;
				material.sourcePath = result.package.importRoot / (materialName + ".tmat");
				material.compiledPath = material.sourcePath;

				if (sourceMaterial != nullptr)
				{
					aiColor4D diffuse;
					if (sourceMaterial->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS)
					{
						material.baseColorFactor = { diffuse.r, diffuse.g, diffuse.b, diffuse.a };
					}

					aiColor3D emissive;
					if (sourceMaterial->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS)
					{
						material.emissiveFactor = { emissive.r, emissive.g, emissive.b };
					}

					float scalar = 0.0f;
					if (sourceMaterial->Get(AI_MATKEY_METALLIC_FACTOR, scalar) == AI_SUCCESS)
					{
						material.metallicFactor = scalar;
					}
					if (sourceMaterial->Get(AI_MATKEY_ROUGHNESS_FACTOR, scalar) == AI_SUCCESS)
					{
						material.roughnessFactor = scalar;
					}
					if (sourceMaterial->Get(AI_MATKEY_OPACITY, scalar) == AI_SUCCESS)
					{
						material.baseColorFactor[3] = scalar;
					}

					int twoSided = 0;
					if (sourceMaterial->Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS)
					{
						material.doubleSided = twoSided != 0;
					}
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

				if (sourceMaterial != nullptr)
				{
					const auto makeTextureProgress = [&options, &textureOrdinal, totalTextureCount]() {
						const float textureSliceStart = 0.30f + (0.42f - 0.30f) * (totalTextureCount == 0u ? 1.0f : static_cast<float>(textureOrdinal) / static_cast<float>(totalTextureCount));
						const float textureSliceEnd = 0.30f + (0.42f - 0.30f) * (totalTextureCount == 0u ? 1.0f : static_cast<float>(textureOrdinal + 1u) / static_cast<float>(totalTextureCount));
						return MakeNestedProgress(options, textureSliceStart, textureSliceEnd);
					};
					ImportMaterialTexture(options, scene, result, materialDesc, material, *sourceMaterial, aiTextureType_BASE_COLOR, ModelTextureUsage::BaseColor, textureOrdinal, totalTextureCount, makeTextureProgress());
					ImportMaterialTexture(options, scene, result, materialDesc, material, *sourceMaterial, aiTextureType_DIFFUSE, ModelTextureUsage::BaseColor, textureOrdinal, totalTextureCount, makeTextureProgress());
					ImportMaterialTexture(options, scene, result, materialDesc, material, *sourceMaterial, aiTextureType_NORMALS, ModelTextureUsage::Normal, textureOrdinal, totalTextureCount, makeTextureProgress());
					ImportMaterialTexture(options, scene, result, materialDesc, material, *sourceMaterial, aiTextureType_HEIGHT, ModelTextureUsage::Normal, textureOrdinal, totalTextureCount, makeTextureProgress());
					ImportMaterialTexture(options, scene, result, materialDesc, material, *sourceMaterial, aiTextureType_METALNESS, ModelTextureUsage::MetallicRoughness, textureOrdinal, totalTextureCount, makeTextureProgress());
					ImportMaterialTexture(options, scene, result, materialDesc, material, *sourceMaterial, aiTextureType_DIFFUSE_ROUGHNESS, ModelTextureUsage::MetallicRoughness, textureOrdinal, totalTextureCount, makeTextureProgress());
					ImportMaterialTexture(options, scene, result, materialDesc, material, *sourceMaterial, aiTextureType_AMBIENT_OCCLUSION, ModelTextureUsage::Occlusion, textureOrdinal, totalTextureCount, makeTextureProgress());
					ImportMaterialTexture(options, scene, result, materialDesc, material, *sourceMaterial, aiTextureType_EMISSIVE, ModelTextureUsage::Emissive, textureOrdinal, totalTextureCount, makeTextureProgress());
				}

				if (!WriteMaterialAsset(options.assetRoot / material.sourcePath, materialDesc))
				{
					AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "GeneratedMaterialWriteFailed", material.sourcePath.generic_string(), "Failed to write generated Terra material.", "The material remains visible in diagnostics only.");
				}
				if (!WriteMaterialAsset(options.buildRoot / material.compiledPath, materialDesc))
				{
					AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "GeneratedMaterialCompileFailed", material.compiledPath.generic_string(), "Failed to write generated Terra material to the build cache.", "The material source file remains available for reimport diagnostics.");
				}

				result.generatedMaterials.push_back(std::move(material));
			}
			ReportProgress(options, 0.42f, "Materials", "Generated Terra material and texture assets.");
		}

		void TraverseMeshes(
			const aiScene& scene,
			const aiNode& node,
			const aiMatrix4x4& parentTransform,
			const std::string& parentPath,
			GeneratedModelMeshAsset& packageMesh,
			ModelPackageImportResult& result,
			std::vector<RawMeshVertex>& vertices,
			std::vector<std::uint32_t>& indices,
			bool& boundsInitialized)
		{
			const aiMatrix4x4 nodeTransform = parentTransform * node.mTransformation;
			const std::string currentNodePath = NodePath(parentPath, node);
			for (unsigned int nodeMeshIndex = 0; nodeMeshIndex < node.mNumMeshes; ++nodeMeshIndex)
			{
				const unsigned int sourceMeshIndex = node.mMeshes[nodeMeshIndex];
				if (sourceMeshIndex >= scene.mNumMeshes || scene.mMeshes[sourceMeshIndex] == nullptr)
				{
					continue;
				}

				const aiMesh& sourceMesh = *scene.mMeshes[sourceMeshIndex];
				const std::uint32_t vertexOffset = static_cast<std::uint32_t>(vertices.size());
				for (unsigned int vertexIndex = 0; vertexIndex < sourceMesh.mNumVertices; ++vertexIndex)
				{
					RawMeshVertex vertex{};
					const aiVector3D position = TransformPoint(nodeTransform, sourceMesh.mVertices[vertexIndex]);
					vertex.position = { position.x, position.y, position.z };
					if (sourceMesh.HasNormals())
					{
						const aiVector3D normal = TransformDirection(nodeTransform, sourceMesh.mNormals[vertexIndex]);
						vertex.normal = { normal.x, normal.y, normal.z };
					}
					if (sourceMesh.HasTextureCoords(0))
					{
						vertex.uv = { sourceMesh.mTextureCoords[0][vertexIndex].x, sourceMesh.mTextureCoords[0][vertexIndex].y };
					}
					ExpandBounds(packageMesh, vertex, boundsInitialized);
					vertices.push_back(vertex);
				}

				bool emittedTriangles = false;
				for (unsigned int faceIndex = 0; faceIndex < sourceMesh.mNumFaces; ++faceIndex)
				{
					const aiFace& face = sourceMesh.mFaces[faceIndex];
					if (face.mNumIndices != 3u)
					{
						AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "NonTriangleFaceSkipped", currentNodePath, "Assimp produced a non-triangle face after triangulation.", "Other compatible faces continue importing.");
						continue;
					}
					indices.push_back(vertexOffset + face.mIndices[0]);
					indices.push_back(vertexOffset + face.mIndices[1]);
					indices.push_back(vertexOffset + face.mIndices[2]);
					emittedTriangles = true;
				}

				if (emittedTriangles)
				{
					ModelMaterialAssignment assignment{};
					assignment.meshGuid = packageMesh.guid;
					assignment.sourceMeshIndex = sourceMeshIndex;
					assignment.sourcePrimitiveIndex = sourceMeshIndex;
					assignment.slotName = SanitizeAssetName(std::string(sourceMesh.mName.C_Str()), "Mesh_" + std::to_string(sourceMeshIndex));
					if (sourceMesh.mMaterialIndex < result.generatedMaterials.size())
					{
						assignment.materialGuid = result.generatedMaterials[sourceMesh.mMaterialIndex].guid;
					}
					else
					{
						AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "MissingMeshMaterial", assignment.slotName, "FBX mesh references no valid material.", "A default material will be used when the mesh is placed.");
					}
					result.materialAssignments.push_back(std::move(assignment));
					if (packageMesh.sourceNodePath.empty())
					{
						packageMesh.sourceNodePath = currentNodePath;
					}
				}
			}

			for (unsigned int childIndex = 0; childIndex < node.mNumChildren; ++childIndex)
			{
				if (node.mChildren[childIndex] != nullptr)
				{
					TraverseMeshes(scene, *node.mChildren[childIndex], nodeTransform, currentNodePath, packageMesh, result, vertices, indices, boundsInitialized);
				}
			}
		}
	}

	ModelPackageImportResult ImportFbxPackage(const FbxPackageImportOptions& options)
	{
		ModelPackageImportResult result{};
		result.packageGuid = options.packageGuid;
		result.package.sourcePath = options.sourceRelativePath;
		result.package.packageKind = ModelPackageKind::Fbx;
		result.package.sourceHashHex = ComputeFileHashHex(options.sourcePath);
		result.package.importRoot = options.sourceRelativePath.parent_path() / (options.sourcePath.stem().string() + "_Generated");

		ReportProgress(options, 0.04f, "Reading", "Opening FBX source data.");
		if (NormalizeExtension(options.sourcePath) != ".fbx")
		{
			AddDiagnostic(result, ModelDiagnosticSeverity::Error, "UnsupportedPackageExtension", options.sourceRelativePath.generic_string(), "FBX importer received a non-FBX source path.");
			PromoteStatusFromDiagnostics(result);
			return result;
		}

		Assimp::Importer importer;
		constexpr unsigned int importFlags =
			aiProcess_Triangulate |
			aiProcess_SortByPType |
			aiProcess_JoinIdenticalVertices |
			aiProcess_FindInvalidData |
			aiProcess_GenSmoothNormals |
			aiProcess_CalcTangentSpace |
			aiProcess_ImproveCacheLocality;

		// Assimp owns and deletes custom progress handlers. Returning false
		// from the handler lets cancel requests abort long FBX parsing instead
		// of forcing project close to wait for the whole parse to finish.
		importer.SetProgressHandler(new FbxAssimpProgressHandler(options));
		ReportProgress(options, 0.12f, "Parsing", "Parsing FBX scene with Assimp.");
		const aiScene* scene = importer.ReadFile(options.sourcePath.string(), importFlags);
		if (scene == nullptr)
		{
			AddDiagnostic(result, ModelDiagnosticSeverity::Error, "ParseFailed", options.sourceRelativePath.generic_string(), std::string("Failed to parse FBX package: ") + importer.GetErrorString());
			PromoteStatusFromDiagnostics(result);
			return result;
		}

		AddUnsupportedDiagnostics(*scene, result);
		const std::size_t textureReferenceCount = CountMaterialTextureReferences(*scene);
		ReportProgress(
			options,
			0.18f,
			"Inventory",
			BuildPackageInventoryMessage(scene->mNumMeshes, textureReferenceCount, scene->mNumMaterials));
		ImportMaterials(options, *scene, result, textureReferenceCount);

		GeneratedModelMeshAsset packageMesh{};
		packageMesh.guid = StableChildUuid(options.packageGuid, "fbx-mesh", 0u);
		packageMesh.sourceMeshIndex = 0u;
		packageMesh.sourcePath = options.sourceRelativePath;
		packageMesh.compiledPath = options.sourceRelativePath;
		packageMesh.compiledPath.replace_extension(".temf");
		packageMesh.displayName = SanitizeAssetName(options.sourcePath.stem().string(), "FbxMesh");

		std::vector<RawMeshVertex> vertices;
		std::vector<std::uint32_t> indices;
		bool boundsInitialized = false;
		if (scene->mRootNode != nullptr)
		{
			ReportProgress(
				options,
				0.46f,
				"Meshes",
				"Extracting static mesh geometry from " + std::to_string(scene->mNumMeshes) + " source mesh" + (scene->mNumMeshes == 1u ? "." : "es."));
			TraverseMeshes(*scene, *scene->mRootNode, aiMatrix4x4(), {}, packageMesh, result, vertices, indices, boundsInitialized);
		}
		ReportProgress(options, 0.70f, "Meshes", "Static mesh geometry extracted.");

		if (vertices.empty() || indices.empty())
		{
			AddDiagnostic(result, ModelDiagnosticSeverity::Error, "NoRenderableGeometry", options.sourceRelativePath.generic_string(), "No compatible static triangle geometry was extracted from the FBX package.");
		}
		else
		{
			const std::filesystem::path compiledMeshPath = options.buildRoot / packageMesh.compiledPath;
			MeshCompileReport compileReport{};
			const VirtualGeometryAdaptiveCompileSettings compileSettings = ToCompileSettings(options.virtualGeometrySettings);
			const VirtualGeometryAdaptiveCompileSettings* compileSettingsPtr = options.hasVirtualGeometrySettings ? &compileSettings : nullptr;
			ReportProgress(options, 0.78f, "Compiling Mesh", "Building Terra mesh and virtual geometry data.");
			if (!CompileMeshFromRaw(options.sourceRelativePath, compiledMeshPath, vertices, indices, true, &compileReport, 60.0f, 1e-6f, compileSettingsPtr, MakeNestedProgress(options, 0.78f, 0.90f)))
			{
				AddDiagnostic(result, ModelDiagnosticSeverity::Error, "MeshCompileFailed", packageMesh.compiledPath.generic_string(), "Extracted FBX geometry failed Terra mesh compilation.");
			}
			ReportProgress(options, 0.90f, "Mesh Metadata", "Reading compiled mesh metadata.");
			packageMesh.virtualGeometrySettings = ToGeneratedSettings(compileReport.virtualGeometry.adaptiveSettings, options.hasVirtualGeometrySettings);
			PopulateVirtualGeometryTargetDiagnostics(compileReport, packageMesh);
			PopulateGeneratedMeshVirtualGeometryMetadata(compiledMeshPath, packageMesh, result.diagnostics);
			const std::uint32_t materialAssignmentCount = CountMaterialAssignmentsForMesh(result, packageMesh.guid);
			PopulatePackageMaterialCoverageDiagnostics(packageMesh, materialAssignmentCount);
			PopulateVirtualGeometryCoverageTags(packageMesh, materialAssignmentCount);
			result.generatedMeshes.push_back(std::move(packageMesh));
		}

		ReportProgress(options, 0.94f, "Diagnostics", "Finalizing package diagnostics.");
		PromoteStatusFromDiagnostics(result);
		return result;
	}
}
