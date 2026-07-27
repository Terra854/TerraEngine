// FbxPackageImporter.cpp - Assimp-backed FBX package importer.
//
// The importer performs worker-safe preparation: it decodes Assimp's scene,
// writes generated material/texture/build outputs, and returns a sidecar result.
// The AssetDatabase remains responsible for the protected registration boundary
// that makes those outputs visible to the editor and ResourceManager.

#include "FbxPackageImporter.h"

#include "AnimationClipCompiler.h"
#include "MaterialAssetWriter.h"
#include "MeshCompiler.h"
#include "TextureCompiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/ProgressHandler.hpp>
#include <assimp/anim.h>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#if defined(_WIN32)
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <Windows.h>
#endif

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

		constexpr unsigned int kFbxPackageImportFlags =
			aiProcess_Triangulate |
			aiProcess_SortByPType |
			aiProcess_JoinIdenticalVertices |
			aiProcess_FindInvalidData |
			aiProcess_GenSmoothNormals |
			aiProcess_CalcTangentSpace |
			aiProcess_ImproveCacheLocality |
			aiProcess_GlobalScale |
			aiProcess_PopulateArmatureData;

		std::uint64_t AppendStableHash(std::uint64_t hash, std::span<const std::uint8_t> bytes)
		{
			constexpr std::uint64_t prime = 1099511628211ull;
			for (const std::uint8_t byte : bytes)
			{
				hash ^= byte;
				hash *= prime;
			}
			return hash;
		}

		std::uint64_t AppendStableHash(std::uint64_t hash, std::string_view value)
		{
			return AppendStableHash(hash, {
				reinterpret_cast<const std::uint8_t*>(value.data()),
				value.size()
			});
		}

		template<typename Value>
		std::uint64_t AppendStablePodHash(std::uint64_t hash, const Value& value)
		{
			return AppendStableHash(hash, {
				reinterpret_cast<const std::uint8_t*>(&value),
				sizeof(Value)
			});
		}

		std::uint64_t BuildSemanticHash(
			const TerraEngine::UUID& packageGuid,
			std::string_view role,
			std::string_view semanticKey)
		{
			std::uint64_t hash = 1469598103934665603ull;
			hash = AppendStableHash(hash, packageGuid);
			hash = AppendStableHash(hash, role);
			hash = AppendStableHash(hash, semanticKey);
			return hash == 0u ? 1u : hash;
		}

		TerraEngine::Animation::AnimationGuid ToAnimationGuid(const TerraEngine::UUID& guid)
		{
			return { guid };
		}

		std::string StableHashSuffix(std::uint64_t hash)
		{
			std::ostringstream text;
			text << std::hex << hash;
			return text.str();
		}

		std::string NormalizeSemanticSegment(std::string_view value, std::string_view fallback)
		{
			std::string normalized;
			normalized.reserve(value.size());
			for (const unsigned char character : value)
			{
				if (std::isalnum(character) || character == '_' || character == '-' || character == '.')
				{
					normalized.push_back(static_cast<char>(std::tolower(character)));
				}
				else if (normalized.empty() || normalized.back() != '_')
				{
					normalized.push_back('_');
				}
			}
			return normalized.empty() ? std::string(fallback) : normalized;
		}

		bool WriteBinaryAtomically(const std::filesystem::path& outputPath, std::span<const std::uint8_t> bytes)
		{
			static std::atomic<std::uint64_t> stageSequence{0u};
			std::error_code ec;
			std::filesystem::create_directories(outputPath.parent_path(), ec);
			if (ec)
			{
				return false;
			}

			std::uint64_t contentHash = 1469598103934665603ull;
			contentHash = AppendStableHash(contentHash, bytes);
			// Use a compact sibling filename rather than appending to a potentially
			// long semantic clip name. The containing package stage is already unique;
			// hash plus sequence separates multiple atomic writes within that stage.
			const std::filesystem::path stagedPath = outputPath.parent_path()
				/ (".ta-" + StableHashSuffix(contentHash) + "-"
					+ std::to_string(stageSequence.fetch_add(1u, std::memory_order_relaxed)) + ".tmp");
			std::filesystem::remove(stagedPath, ec);
			ec.clear();
			{
				std::ofstream stream(stagedPath, std::ios::binary | std::ios::trunc);
				if (!stream)
				{
					return false;
				}
				stream.write(
					reinterpret_cast<const char*>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
				stream.flush();
				if (!stream)
				{
					stream.close();
					std::filesystem::remove(stagedPath, ec);
					return false;
				}
			}

			bool published = false;
#if defined(_WIN32)
			published = ::MoveFileExW(
				stagedPath.c_str(),
				outputPath.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
			std::filesystem::rename(stagedPath, outputPath, ec);
			published = !ec;
#endif
			if (!published)
			{
				std::filesystem::remove(stagedPath, ec);
			}
			return published;
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
			// Animation, hierarchy, bones, and morphs are consumed by the animated
			// sidecar below. Only source features outside T035 remain unsupported.
			if (scene.HasCameras())
			{
				AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "UnsupportedCameraIgnored", "cameras", "Camera data is not imported by this static FBX feature.", "Renderable meshes were imported.");
			}
			if (scene.HasLights())
			{
				AddDiagnostic(result, ModelDiagnosticSeverity::Warning, "UnsupportedLightIgnored", "lights", "Light data is not imported by this static FBX feature.", "Renderable meshes were imported.");
			}
		}

		aiMatrix4x4 ComposeSourceGlobalTransform(
			const aiMatrix4x4& parentTransform,
			const aiMatrix4x4& localTransform)
		{
			// Terra's matrices use row-major storage with column-vector application,
			// so a hierarchy global is parent * local. Assimp stores the same matrix
			// layout and its scalar multiplication formula preserves that textual
			// order. Keeping this rule in one helper prevents mesh baking and runtime
			// skeleton evaluation from drifting into different hierarchy spaces.
			return parentTransform * localTransform;
		}

		aiMatrix4x4 RebaseOffsetToPackageSpace(
			const aiMatrix4x4& meshLocalToBone,
			const aiMatrix4x4& packageToMesh)
		{
			// A source offset maps mesh-local positions to bone-local positions. The
			// cooker has already converted those positions to package space, so the
			// stored inverse bind must apply meshLocalToBone * packageToMesh. Assimp's
			// matrix operator preserves that textual order, matching the runtime's
			// globalBone * inverseBind * packageVertex palette convention.
			return meshLocalToBone * packageToMesh;
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
			const aiMatrix4x4 nodeTransform = ComposeSourceGlobalTransform(
				parentTransform,
				node.mTransformation);
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

		bool HasImportErrors(const ModelPackageImportResult& result)
		{
			return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const ModelImportDiagnostic& diagnostic) {
				return diagnostic.severity == ModelDiagnosticSeverity::Error;
			});
		}

		TerraEngine::Animation::AnimationTransform ToAnimationTransform(const aiMatrix4x4& matrix)
		{
			aiVector3D scale;
			aiVector3D translation;
			aiQuaternion rotation;
			matrix.Decompose(scale, rotation, translation);
			const float lengthSquared = rotation.x * rotation.x + rotation.y * rotation.y
				+ rotation.z * rotation.z + rotation.w * rotation.w;
			if (std::isfinite(lengthSquared) && lengthSquared > std::numeric_limits<float>::epsilon())
			{
				const float reciprocalLength = 1.0f / std::sqrt(lengthSquared);
				rotation.x *= reciprocalLength;
				rotation.y *= reciprocalLength;
				rotation.z *= reciprocalLength;
				rotation.w *= reciprocalLength;
			}
			else
			{
				rotation = aiQuaternion();
			}
			return {
				{ translation.x, translation.y, translation.z },
				{ rotation.x, rotation.y, rotation.z, rotation.w },
				{ scale.x, scale.y, scale.z }
			};
		}

		TerraEngine::Animation::AnimationMatrix4x4 ToAnimationMatrix(const aiMatrix4x4& matrix)
		{
			return {{
				matrix.a1, matrix.a2, matrix.a3, matrix.a4,
				matrix.b1, matrix.b2, matrix.b3, matrix.b4,
				matrix.c1, matrix.c2, matrix.c3, matrix.c4,
				matrix.d1, matrix.d2, matrix.d3, matrix.d4
			}};
		}

		TerraEngine::Animation::AnimationMatrix4x4 IdentityAnimationMatrix()
		{
			return {{
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f
			}};
		}

		bool MatricesNearlyEqual(
			const TerraEngine::Animation::AnimationMatrix4x4& left,
			const TerraEngine::Animation::AnimationMatrix4x4& right)
		{
			for (std::size_t index = 0u; index < left.values.size(); ++index)
			{
				if (std::abs(left.values[index] - right.values[index]) > 0.00001f)
				{
					return false;
				}
			}
			return true;
		}

		bool IsSocketName(std::string_view name)
		{
			const std::string normalized = NormalizeSemanticSegment(name, "node");
			return normalized.starts_with("socket")
				|| normalized.starts_with("attach")
				|| normalized.starts_with("mount")
				|| normalized.starts_with("locator")
				|| normalized.ends_with("_socket")
				|| normalized.ends_with("_attach");
		}

		TerraEngine::Animation::SkeletonSourceAxis ToSourceAxis(std::int32_t axis, std::int32_t sign)
		{
			using TerraEngine::Animation::SkeletonSourceAxis;
			if (axis == 0)
			{
				return sign < 0 ? SkeletonSourceAxis::NegativeX : SkeletonSourceAxis::PositiveX;
			}
			if (axis == 1)
			{
				return sign < 0 ? SkeletonSourceAxis::NegativeY : SkeletonSourceAxis::PositiveY;
			}
			return sign < 0 ? SkeletonSourceAxis::NegativeZ : SkeletonSourceAxis::PositiveZ;
		}

		template<typename Value>
		bool TryReadSceneMetadata(const aiScene& scene, std::string_view key, Value& output)
		{
			return scene.mMetaData != nullptr
				&& scene.mMetaData->Get(std::string(key), output);
		}

		FbxCoordinateConversion ReadCoordinateConversion(const aiScene& scene)
		{
			FbxCoordinateConversion conversion;
			TryReadSceneMetadata(scene, "UpAxis", conversion.upAxis);
			TryReadSceneMetadata(scene, "UpAxisSign", conversion.upAxisSign);
			TryReadSceneMetadata(scene, "FrontAxis", conversion.frontAxis);
			TryReadSceneMetadata(scene, "FrontAxisSign", conversion.frontAxisSign);
			TryReadSceneMetadata(scene, "CoordAxis", conversion.coordinateAxis);
			TryReadSceneMetadata(scene, "CoordAxisSign", conversion.coordinateAxisSign);

			float unitScaleCentimeters = 1.0f;
			double unitScaleDouble = 1.0;
			if (!TryReadSceneMetadata(scene, "UnitScaleFactor", unitScaleCentimeters)
				&& TryReadSceneMetadata(scene, "UnitScaleFactor", unitScaleDouble))
			{
				unitScaleCentimeters = static_cast<float>(unitScaleDouble);
			}
			conversion.unitScaleMeters = unitScaleCentimeters * 0.01f;
			conversion.assimpOutputNormalizedToMeters = true;
			return conversion;
		}

		struct FbxNodeLookup
		{
			std::vector<const aiNode*> sourceNodes;
			std::vector<std::string> semanticKeysByIndex;
			std::unordered_map<const aiNode*, std::size_t> indexByPointer;
			std::unordered_map<std::string, std::vector<std::size_t>> indicesByName;
			std::unordered_set<std::string> semanticKeys;
		};

		void BuildAnimatedNodeHierarchy(
			const FbxPackageImportOptions& options,
			const aiNode& source,
			std::int32_t parentIndex,
			std::string parentSemanticKey,
			FbxAnimatedPackageData& output,
			FbxNodeLookup& lookup,
			ModelPackageImportResult& package)
		{
			const std::size_t nodeIndex = output.nodes.size();
			const std::string displayName = source.mName.length > 0u
				? std::string(source.mName.C_Str())
				: (parentIndex < 0 ? std::string("Root") : std::string("Node"));
			const std::string segment = parentIndex < 0
				? std::string("$root")
				: NormalizeSemanticSegment(displayName, "node");
			std::string semanticKey = parentSemanticKey.empty()
				? segment
				: parentSemanticKey + "/" + segment;

			if (!lookup.semanticKeys.insert(semanticKey).second)
			{
				// Equal sibling labels are not a stable semantic identity. A transform
				// signature disambiguates ordinary exporter duplicates without depending
				// on source array order, while the diagnostic tells authors how to fix an
				// exact collision that cannot be reasoned about semantically.
				std::uint64_t transformHash = 1469598103934665603ull;
				transformHash = AppendStablePodHash(transformHash, source.mTransformation);
				semanticKey += "#" + StableHashSuffix(transformHash);
				if (!lookup.semanticKeys.insert(semanticKey).second)
				{
					AddDiagnostic(
						package,
						ModelDiagnosticSeverity::Error,
						"FbxAmbiguousNodeIdentity",
						displayName,
						"Two FBX sibling nodes have the same name and indistinguishable bind transform.",
						"Give the nodes unique semantic names before reimporting so animation references remain stable.");
					semanticKey += "#ambiguous";
				}
			}

			FbxAnimatedNode node;
			node.stableId = BuildSemanticHash(options.packageGuid, "fbx-node", semanticKey);
			node.parentIndex = parentIndex;
			node.semanticKey = semanticKey;
			node.displayName = displayName;
			node.localTransform = ToAnimationTransform(source.mTransformation);
			node.socketCandidate = IsSocketName(displayName);
			output.nodes.push_back(std::move(node));
			lookup.sourceNodes.push_back(&source);
			lookup.semanticKeysByIndex.push_back(semanticKey);
			lookup.indexByPointer.emplace(&source, nodeIndex);
			lookup.indicesByName[displayName].push_back(nodeIndex);

			std::vector<const aiNode*> sortedChildren;
			sortedChildren.reserve(source.mNumChildren);
			for (unsigned int childIndex = 0u; childIndex < source.mNumChildren; ++childIndex)
			{
				if (source.mChildren[childIndex] != nullptr)
				{
					sortedChildren.push_back(source.mChildren[childIndex]);
				}
			}
			std::sort(sortedChildren.begin(), sortedChildren.end(), [](const aiNode* left, const aiNode* right) {
				const std::string leftName = NormalizeSemanticSegment(left->mName.C_Str(), "node");
				const std::string rightName = NormalizeSemanticSegment(right->mName.C_Str(), "node");
				if (leftName != rightName)
				{
					return leftName < rightName;
				}
				const std::array<float, 16u> leftMatrix{
					left->mTransformation.a1, left->mTransformation.a2, left->mTransformation.a3, left->mTransformation.a4,
					left->mTransformation.b1, left->mTransformation.b2, left->mTransformation.b3, left->mTransformation.b4,
					left->mTransformation.c1, left->mTransformation.c2, left->mTransformation.c3, left->mTransformation.c4,
					left->mTransformation.d1, left->mTransformation.d2, left->mTransformation.d3, left->mTransformation.d4
				};
				const std::array<float, 16u> rightMatrix{
					right->mTransformation.a1, right->mTransformation.a2, right->mTransformation.a3, right->mTransformation.a4,
					right->mTransformation.b1, right->mTransformation.b2, right->mTransformation.b3, right->mTransformation.b4,
					right->mTransformation.c1, right->mTransformation.c2, right->mTransformation.c3, right->mTransformation.c4,
					right->mTransformation.d1, right->mTransformation.d2, right->mTransformation.d3, right->mTransformation.d4
				};
				return std::tie(leftMatrix, left->mNumMeshes, left->mNumChildren)
					< std::tie(rightMatrix, right->mNumMeshes, right->mNumChildren);
			});
			for (const aiNode* child : sortedChildren)
			{
					BuildAnimatedNodeHierarchy(
						options,
						*child,
						static_cast<std::int32_t>(nodeIndex),
						semanticKey,
						output,
						lookup,
						package);
			}
		}

		std::optional<std::size_t> ResolveNodeIndex(
			const aiNode* directNode,
			std::string_view nodeName,
			const FbxNodeLookup& lookup)
		{
			if (directNode != nullptr)
			{
				const auto direct = lookup.indexByPointer.find(directNode);
				if (direct != lookup.indexByPointer.end())
				{
					return direct->second;
				}
			}
			const auto named = lookup.indicesByName.find(std::string(nodeName));
			if (named == lookup.indicesByName.end() || named->second.size() != 1u)
			{
				return std::nullopt;
			}
			return named->second.front();
		}

		std::optional<std::size_t> ResolveAnimationNodeIndex(
			std::string_view nodeName,
			const FbxNodeLookup& lookup,
			const std::vector<bool>& deformingNodes)
		{
			const auto named = lookup.indicesByName.find(std::string(nodeName));
			if (named == lookup.indicesByName.end())
			{
				return std::nullopt;
			}
			if (named->second.size() == 1u)
			{
				return named->second.front();
			}

			// FBX permits a render model and a limb node to share a display name.
			// Animation channels only identify that display name, so a duplicate can
			// be resolved safely only when skin extraction established one deforming
			// candidate. Never use Assimp's source-array order as a tie breaker.
			std::optional<std::size_t> deformingCandidate;
			for (const std::size_t candidate : named->second)
			{
				if (candidate >= deformingNodes.size() || !deformingNodes[candidate])
				{
					continue;
				}
				if (deformingCandidate)
				{
					return std::nullopt;
				}
				deformingCandidate = candidate;
			}
			return deformingCandidate;
		}

		std::string MeshSemanticKey(const aiMesh& mesh)
		{
			if (mesh.mName.length > 0u)
			{
				return "mesh:" + NormalizeSemanticSegment(mesh.mName.C_Str(), "mesh");
			}
			// An unnamed mesh has no authored semantic label. Its topology signature
			// is the only source-order-independent discriminator available through
			// Assimp, so content edits may legitimately give that anonymous child a
			// new identity until the author supplies a stable name.
			std::uint64_t signature = 1469598103934665603ull;
			signature = AppendStablePodHash(signature, mesh.mNumVertices);
			signature = AppendStablePodHash(signature, mesh.mNumFaces);
			if (mesh.mNumVertices > 0u && mesh.mVertices != nullptr)
			{
				for (unsigned int vertexIndex = 0u; vertexIndex < mesh.mNumVertices; ++vertexIndex)
				{
					signature = AppendStablePodHash(signature, mesh.mVertices[vertexIndex]);
				}
			}
			return "mesh:unnamed:" + StableHashSuffix(signature);
		}

		std::string MaterialSectionSemanticKey(const aiScene& scene, const aiMesh& mesh)
		{
			if (mesh.mMaterialIndex < scene.mNumMaterials)
			{
				const aiMaterial* material = scene.mMaterials[mesh.mMaterialIndex];
				aiString materialName;
				if (material != nullptr
					&& material->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS
					&& materialName.length > 0u)
				{
					return "material:" + NormalizeSemanticSegment(materialName.C_Str(), "material");
				}
			}

			// An unnamed material supplies no durable authored section identity. Keep
			// the fallback intentionally shared so equal base-mesh sections fail
			// closed instead of inheriting an unstable Assimp material-array index.
			return "material:unnamed";
		}

		std::string DeformableMeshSectionSemanticKey(const aiScene& scene, const aiMesh& mesh)
		{
			// Assimp may split one authored FBX geometry into several aiMesh records
			// for its material sections. Retain the base mesh identity and append the
			// authored material semantic so skin and morph records remain distinct
			// across that lossless importer representation change.
			return MeshSemanticKey(mesh) + "/" + MaterialSectionSemanticKey(scene, mesh);
		}

		std::optional<std::size_t> ResolveBoneNodeIndex(
			const aiBone& bone,
			const FbxNodeLookup& lookup)
		{
#if !defined(ASSIMP_BUILD_NO_ARMATUREPOPULATE_PROCESS)
			return ResolveNodeIndex(bone.mNode, bone.mName.C_Str(), lookup);
#else
			return ResolveNodeIndex(nullptr, bone.mName.C_Str(), lookup);
#endif
		}

		void CollectSourceMeshTransforms(
			const aiScene& scene,
			const aiNode& node,
			const aiMatrix4x4& parentTransform,
			std::vector<std::vector<aiMatrix4x4>>& transforms)
		{
			const aiMatrix4x4 globalTransform = ComposeSourceGlobalTransform(
				parentTransform,
				node.mTransformation);
			for (unsigned int index = 0u; index < node.mNumMeshes; ++index)
			{
				if (node.mMeshes[index] < scene.mNumMeshes)
				{
					transforms[node.mMeshes[index]].push_back(globalTransform);
				}
			}
			for (unsigned int index = 0u; index < node.mNumChildren; ++index)
			{
				if (node.mChildren[index] != nullptr)
				{
					CollectSourceMeshTransforms(scene, *node.mChildren[index], globalTransform, transforms);
				}
			}
		}

		bool AssimpMatricesNearlyEqual(const aiMatrix4x4& left, const aiMatrix4x4& right)
		{
			return MatricesNearlyEqual(ToAnimationMatrix(left), ToAnimationMatrix(right));
		}

		void ExtractSkinsAndInverseBinds(
			const FbxPackageImportOptions& options,
			const aiScene& scene,
			const FbxNodeLookup& lookup,
			TerraEngine::Animation::AnimationGuid skeletonAssetId,
			FbxAnimatedPackageData& output,
			std::vector<bool>& deformingNodes,
			std::vector<bool>& inverseBindAssigned,
			std::vector<TerraEngine::Animation::AnimationMatrix4x4>& inverseBinds,
			ModelPackageImportResult& package)
		{
			bool reportedInvalidWeight = false;
			std::unordered_set<std::string> emittedSkinSemanticKeys;
			std::vector<std::vector<aiMatrix4x4>> sourceMeshTransforms(scene.mNumMeshes);
			if (scene.mRootNode != nullptr)
			{
				CollectSourceMeshTransforms(scene, *scene.mRootNode, aiMatrix4x4(), sourceMeshTransforms);
			}
			for (unsigned int meshIndex = 0u; meshIndex < scene.mNumMeshes; ++meshIndex)
			{
				const aiMesh* mesh = scene.mMeshes[meshIndex];
				if (mesh == nullptr || !mesh->HasBones())
				{
					continue;
				}

				FbxImportedSkinnedMesh skin;
				skin.sourceMeshIndex = meshIndex;
				skin.semanticKey = DeformableMeshSectionSemanticKey(scene, *mesh);
				if (!emittedSkinSemanticKeys.insert(skin.semanticKey).second)
				{
					AddDiagnostic(
						package,
						ModelDiagnosticSeverity::Error,
						"FbxAmbiguousSkinnedMeshIdentity",
						skin.semanticKey,
						"Multiple skinned FBX mesh sections have the same normalized mesh and material identity.",
						"Give each ambiguous skinned section a unique authored mesh or material name before reimporting.");
					continue;
				}
				skin.skeletonAssetId = skeletonAssetId;
				if (sourceMeshTransforms[meshIndex].empty())
				{
					AddDiagnostic(
						package,
						ModelDiagnosticSeverity::Error,
						"FbxSkinnedMeshPlacementMissing",
						skin.semanticKey,
						"A skinned FBX mesh has no hierarchy placement transform.",
						"Attach the mesh to exactly one exported hierarchy node before reimporting.");
					continue;
				}
				const aiMatrix4x4 meshTransform = sourceMeshTransforms[meshIndex].front();
				const bool hasDistinctInstances = std::any_of(
					sourceMeshTransforms[meshIndex].begin() + 1,
					sourceMeshTransforms[meshIndex].end(),
					[&](const aiMatrix4x4& candidate) { return !AssimpMatricesNearlyEqual(meshTransform, candidate); });
				if (hasDistinctInstances)
				{
					AddDiagnostic(
						package,
						ModelDiagnosticSeverity::Error,
						"FbxSkinnedMeshInstanceBindConflict",
						skin.semanticKey,
						"One skinned FBX source mesh is instanced at different hierarchy transforms, which cannot share one inverse-bind table after package-space baking.",
						"Make each differently placed skinned instance a distinct authored mesh before reimporting.");
					continue;
				}
				if (!std::isfinite(meshTransform.Determinant())
					|| std::abs(meshTransform.Determinant()) <= 0.000001f)
				{
					AddDiagnostic(
						package,
						ModelDiagnosticSeverity::Error,
						"FbxSkinnedMeshPlacementSingular",
						skin.semanticKey,
						"A skinned FBX mesh has a singular placement transform that cannot be reconciled with its inverse binds.",
						"Remove zero scale and re-export a finite invertible mesh placement.");
					continue;
				}
				aiMatrix4x4 inverseMeshTransform = meshTransform;
				inverseMeshTransform.Inverse();
				for (unsigned int boneIndex = 0u; boneIndex < mesh->mNumBones; ++boneIndex)
				{
					const aiBone* bone = mesh->mBones[boneIndex];
					if (bone == nullptr)
					{
						continue;
					}
					const std::optional<std::size_t> nodeIndex = ResolveBoneNodeIndex(*bone, lookup);
					if (!nodeIndex)
					{
						AddDiagnostic(
							package,
							ModelDiagnosticSeverity::Error,
							"FbxBoneNodeMissing",
							bone->mName.C_Str(),
							"An FBX skin bone does not resolve to exactly one hierarchy node.",
							"Ensure every deforming bone has a unique node name and is inside the exported hierarchy.");
						continue;
					}

					deformingNodes[*nodeIndex] = true;
					const TerraEngine::Animation::AnimationMatrix4x4 bind = ToAnimationMatrix(
						RebaseOffsetToPackageSpace(bone->mOffsetMatrix, inverseMeshTransform));
					if (inverseBindAssigned[*nodeIndex]
						&& !MatricesNearlyEqual(inverseBinds[*nodeIndex], bind))
					{
						AddDiagnostic(
							package,
							ModelDiagnosticSeverity::Warning,
							"FbxInverseBindConflict",
							bone->mName.C_Str(),
							"The same FBX bone carries different inverse-bind matrices in multiple meshes.",
							"Verify the meshes share one bind pose; a deterministic canonical bind is retained.");
						if (bind.values < inverseBinds[*nodeIndex].values)
						{
							inverseBinds[*nodeIndex] = bind;
						}
					}
					else
					{
						inverseBinds[*nodeIndex] = bind;
						inverseBindAssigned[*nodeIndex] = true;
					}

					const std::uint64_t jointStableId = output.nodes[*nodeIndex].stableId;
					for (unsigned int weightIndex = 0u; weightIndex < bone->mNumWeights; ++weightIndex)
					{
						const aiVertexWeight& sourceWeight = bone->mWeights[weightIndex];
						if (sourceWeight.mVertexId >= mesh->mNumVertices
							|| !std::isfinite(sourceWeight.mWeight)
							|| sourceWeight.mWeight <= 0.0f)
						{
							if (!reportedInvalidWeight)
							{
								AddDiagnostic(
									package,
									ModelDiagnosticSeverity::Warning,
									"FbxInvalidBoneWeightSkipped",
									skin.semanticKey,
									"A non-finite, non-positive, or out-of-range FBX bone influence was rejected.",
									"Normalize weights and remove invalid vertex assignments in the source file before reimporting.");
								reportedInvalidWeight = true;
							}
							continue;
						}
						skin.influences.push_back({ sourceWeight.mVertexId, jointStableId, sourceWeight.mWeight });
					}
				}
				std::sort(skin.influences.begin(), skin.influences.end(), [](const auto& left, const auto& right) {
					return std::tie(left.vertexIndex, left.jointStableId, left.weight)
						< std::tie(right.vertexIndex, right.jointStableId, right.weight);
				});
				output.skinnedMeshes.push_back(std::move(skin));
			}
		}

		bool NormalizeInverseBindsToRuntimeReferencePose(
			const std::vector<FbxAnimatedNode>& nodes,
			const std::vector<bool>& deformingNodes,
			std::vector<TerraEngine::Animation::AnimationMatrix4x4>& inverseBinds,
			ModelPackageImportResult& package)
		{
			if (nodes.size() != deformingNodes.size() || nodes.size() != inverseBinds.size())
			{
				AddDiagnostic(
					package,
					ModelDiagnosticSeverity::Error,
					"FbxReferencePoseBindCountMismatch",
					"skeleton:scene",
					"The normalized FBX hierarchy and inverse-bind tables have different joint counts.",
					"Re-export one complete skeleton hierarchy and reimport the package.");
				return false;
			}

			// Assimp offsets are still collected above to validate source ownership and
			// cross-mesh conflicts. Runtime evaluation, however, reconstructs each bind
			// joint from the normalized TRS stored in SkeletonAsset. Derive deforming
			// inverse binds from that exact reference pose so enabling animation cannot
			// move an otherwise-correct baked mesh before the first clip sample.
			std::vector<aiMatrix4x4> globalReferencePose(nodes.size());
			std::vector<TerraEngine::Animation::AnimationMatrix4x4> normalized = inverseBinds;
			bool normalizedAny = false;
			for (std::size_t index = 0u; index < nodes.size(); ++index)
			{
				const TerraEngine::Animation::AnimationTransform& transform = nodes[index].localTransform;
				const aiVector3D scale(transform.scale.x, transform.scale.y, transform.scale.z);
				const aiQuaternion rotation(
					transform.rotation.w,
					transform.rotation.x,
					transform.rotation.y,
					transform.rotation.z);
				const aiVector3D translation(
					transform.translation.x,
					transform.translation.y,
					transform.translation.z);
				const aiMatrix4x4 localReferencePose(scale, rotation, translation);
				if (nodes[index].parentIndex >= 0)
				{
					const std::size_t parentIndex = static_cast<std::size_t>(nodes[index].parentIndex);
					if (parentIndex >= index)
					{
						AddDiagnostic(
							package,
							ModelDiagnosticSeverity::Error,
							"FbxReferencePoseParentOrderInvalid",
							nodes[index].semanticKey,
							"A normalized FBX joint references a parent that is not earlier in hierarchy order.",
							"Repair the source hierarchy so every parent precedes its children.");
						return false;
					}
					globalReferencePose[index] =
						ComposeSourceGlobalTransform(globalReferencePose[parentIndex], localReferencePose);
				}
				else
				{
					globalReferencePose[index] = localReferencePose;
				}

				if (!deformingNodes[index]) continue;
				const float determinant = globalReferencePose[index].Determinant();
				if (!std::isfinite(determinant) || std::abs(determinant) <= 0.000001F)
				{
					AddDiagnostic(
						package,
						ModelDiagnosticSeverity::Error,
						"FbxReferencePoseJointSingular",
						nodes[index].semanticKey,
						"A deforming FBX joint has a singular normalized reference-pose transform.",
						"Remove zero scale from the joint hierarchy and re-export the model.");
					return false;
				}
				aiMatrix4x4 inverseReferencePose = globalReferencePose[index];
				inverseReferencePose.Inverse();
				const TerraEngine::Animation::AnimationMatrix4x4 candidate =
					ToAnimationMatrix(inverseReferencePose);
				normalizedAny = normalizedAny
					|| !MatricesNearlyEqual(normalized[index], candidate);
				normalized[index] = candidate;
			}

			// Publish the candidate table only after every deforming joint validated,
			// preserving the importer's all-or-nothing artifact publication contract.
			inverseBinds = std::move(normalized);
			if (normalizedAny)
			{
				AddDiagnostic(
					package,
					ModelDiagnosticSeverity::Info,
					"FbxReferencePoseInverseBindsNormalized",
					"skeleton:scene",
					"FBX inverse binds were normalized to the exact runtime TRS reference pose used by the cooked skeleton.",
					"The bind pose now remains stable when animation evaluation becomes active.");
			}
			return true;
		}

		std::unordered_map<std::string, std::vector<std::size_t>> ExtractMorphTargets(
			const FbxPackageImportOptions& options,
			const aiScene& scene,
			FbxAnimatedPackageData& output,
			ModelPackageImportResult& package)
		{
			std::unordered_map<std::string, std::vector<std::size_t>> morphTargetsByMeshName;
			std::unordered_set<std::uint32_t> emittedMorphTargetIds;
			for (unsigned int meshIndex = 0u; meshIndex < scene.mNumMeshes; ++meshIndex)
			{
				const aiMesh* mesh = scene.mMeshes[meshIndex];
				if (mesh == nullptr || mesh->mNumAnimMeshes == 0u)
				{
					continue;
				}
				const std::string meshKey = DeformableMeshSectionSemanticKey(scene, *mesh);
				for (unsigned int targetIndex = 0u; targetIndex < mesh->mNumAnimMeshes; ++targetIndex)
				{
					const aiAnimMesh* target = mesh->mAnimMeshes[targetIndex];
					if (target == nullptr || target->mNumVertices != mesh->mNumVertices)
					{
						AddDiagnostic(
							package,
							ModelDiagnosticSeverity::Error,
							"FbxMorphVertexCountMismatch",
							meshKey,
							"An FBX morph attachment does not match its base mesh vertex count.",
							"Re-export the morph with unchanged base topology.");
						continue;
					}

					FbxGeneratedMorphTarget morph;
					morph.sourceMeshIndex = meshIndex;
					morph.sourceTargetIndex = targetIndex;
					morph.positionDeltas.resize(mesh->mNumVertices);
					if (target->mNormals != nullptr && mesh->mNormals != nullptr)
					{
						morph.normalDeltas.resize(mesh->mNumVertices);
					}
					if (target->mTangents != nullptr && mesh->mTangents != nullptr)
					{
						morph.tangentDeltas.resize(mesh->mNumVertices);
					}
					for (unsigned int vertexIndex = 0u; vertexIndex < mesh->mNumVertices; ++vertexIndex)
					{
						if (target->mVertices != nullptr && mesh->mVertices != nullptr)
						{
							const aiVector3D delta = target->mVertices[vertexIndex] - mesh->mVertices[vertexIndex];
							morph.positionDeltas[vertexIndex] = { delta.x, delta.y, delta.z };
						}
						if (target->mNormals != nullptr && mesh->mNormals != nullptr)
						{
							const aiVector3D delta = target->mNormals[vertexIndex] - mesh->mNormals[vertexIndex];
							morph.normalDeltas[vertexIndex] = { delta.x, delta.y, delta.z };
						}
						if (target->mTangents != nullptr && mesh->mTangents != nullptr)
						{
							const aiVector3D delta = target->mTangents[vertexIndex] - mesh->mTangents[vertexIndex];
							morph.tangentDeltas[vertexIndex] = { delta.x, delta.y, delta.z };
						}
					}
					std::uint64_t morphSignature = 1469598103934665603ull;
					for (const auto& delta : morph.positionDeltas) morphSignature = AppendStablePodHash(morphSignature, delta);
					for (const auto& delta : morph.normalDeltas) morphSignature = AppendStablePodHash(morphSignature, delta);
					for (const auto& delta : morph.tangentDeltas) morphSignature = AppendStablePodHash(morphSignature, delta);
					morph.displayName = target->mName.length > 0u
						? std::string(target->mName.C_Str())
						: "Morph_" + StableHashSuffix(morphSignature);
					morph.semanticKey = meshKey + "/morph:"
						+ NormalizeSemanticSegment(morph.displayName, "morph");
					std::uint32_t stableTargetId = static_cast<std::uint32_t>(
						BuildSemanticHash(options.packageGuid, "fbx-morph", morph.semanticKey) & 0xffffffffu);
					if (stableTargetId == 0u)
					{
						stableTargetId = 1u;
					}
					if (!emittedMorphTargetIds.insert(stableTargetId).second)
					{
						AddDiagnostic(
							package,
							ModelDiagnosticSeverity::Error,
							"FbxMorphTargetIdCollision",
							morph.semanticKey,
							"Two FBX morph targets map to the same frozen 32-bit TEMF target identifier.",
							"Rename one morph target to establish a distinct semantic identity before reimporting.");
						continue;
					}
					morph.stableId = stableTargetId;
					morph.guid = BuildStableAnimationSubAssetUuid(
						options.packageGuid,
						AnimationImportSubAssetKind::MorphTarget,
						morph.semanticKey);
					const std::size_t emittedIndex = output.morphTargets.size();
					output.morphTargets.push_back(std::move(morph));
					morphTargetsByMeshName[mesh->mName.C_Str()].push_back(emittedIndex);
				}
			}
			return morphTargetsByMeshName;
		}

		aiVector3D TransformDeformationDelta(const aiMatrix4x4& transform, const aiVector3D& value)
		{
			// Morph position, normal, and tangent deltas are vector differences, not
			// absolute directions. Preserve their authored magnitude and node scale
			// while deliberately excluding translation and normalization.
			return {
				transform.a1 * value.x + transform.a2 * value.y + transform.a3 * value.z,
				transform.b1 * value.x + transform.b2 * value.y + transform.b3 * value.z,
				transform.c1 * value.x + transform.c2 * value.y + transform.c3 * value.z
			};
		}

		bool AppendDeformableMeshes(
			const aiScene& scene,
			const aiNode& node,
			const aiMatrix4x4& parentTransform,
			const FbxNodeLookup& nodeLookup,
			const std::unordered_map<std::uint32_t, const FbxGeneratedMorphTarget*>& morphTargets,
			SkinnedMeshCookInput& output,
			ModelPackageImportResult& package)
		{
			// Keep baked vertices in the same package space as the skeleton's
			// parent * local bind hierarchy through the shared Assimp seam.
			const aiMatrix4x4 nodeTransform = ComposeSourceGlobalTransform(
				parentTransform,
				node.mTransformation);
			for (unsigned int nodeMeshIndex = 0u; nodeMeshIndex < node.mNumMeshes; ++nodeMeshIndex)
			{
				const unsigned int sourceMeshIndex = node.mMeshes[nodeMeshIndex];
				if (sourceMeshIndex >= scene.mNumMeshes || scene.mMeshes[sourceMeshIndex] == nullptr)
				{
					continue;
				}
				const aiMesh& mesh = *scene.mMeshes[sourceMeshIndex];
				if (output.vertices.size() > std::numeric_limits<std::uint32_t>::max() - mesh.mNumVertices)
				{
					AddDiagnostic(
						package,
						ModelDiagnosticSeverity::Error,
						"FbxDeformableVertexRangeOverflow",
						MeshSemanticKey(mesh),
						"The merged FBX deformable vertex range exceeds the TEMF 32-bit address space.",
						"Split the package into smaller deformable mesh assets.");
					return false;
				}
				const std::uint32_t vertexOffset = static_cast<std::uint32_t>(output.vertices.size());
				std::vector<std::vector<SkinnedMeshInfluence>> influences(mesh.mNumVertices);
				for (unsigned int boneIndex = 0u; boneIndex < mesh.mNumBones; ++boneIndex)
				{
					const aiBone* bone = mesh.mBones[boneIndex];
					if (bone == nullptr)
					{
						continue;
					}
					const std::optional<std::size_t> jointIndex = ResolveBoneNodeIndex(*bone, nodeLookup);
					if (!jointIndex || *jointIndex > std::numeric_limits<std::uint32_t>::max())
					{
						AddDiagnostic(
							package,
							ModelDiagnosticSeverity::Error,
							"FbxDeformableJointMissing",
							bone->mName.C_Str(),
							"A deformable FBX mesh references a bone that is absent or ambiguous in the emitted skeleton.",
							"Restore the bone inside the exported hierarchy and give it a unique semantic name.");
						return false;
					}
					for (unsigned int weightIndex = 0u; weightIndex < bone->mNumWeights; ++weightIndex)
					{
						const aiVertexWeight& weight = bone->mWeights[weightIndex];
						if (weight.mVertexId >= mesh.mNumVertices)
						{
							AddDiagnostic(
								package,
								ModelDiagnosticSeverity::Error,
								"FbxDeformableWeightVertexOutOfRange",
								bone->mName.C_Str(),
								"An FBX bone weight references a vertex outside its source mesh.",
								"Repair the skin binding or re-export the mesh with consistent topology.");
							return false;
						}
						influences[weight.mVertexId].push_back({
							static_cast<std::uint32_t>(*jointIndex),
							weight.mWeight
						});
					}
				}

				for (unsigned int vertexIndex = 0u; vertexIndex < mesh.mNumVertices; ++vertexIndex)
				{
					SkinnedMeshCookVertex vertex;
					const aiVector3D position = TransformPoint(nodeTransform, mesh.mVertices[vertexIndex]);
					vertex.baseVertex.position = { position.x, position.y, position.z };
					if (mesh.HasNormals())
					{
						const aiVector3D normal = TransformDirection(nodeTransform, mesh.mNormals[vertexIndex]);
						vertex.baseVertex.normal = { normal.x, normal.y, normal.z };
					}
					if (mesh.HasTextureCoords(0u))
					{
						vertex.baseVertex.uv = {
							mesh.mTextureCoords[0u][vertexIndex].x,
							mesh.mTextureCoords[0u][vertexIndex].y
						};
					}
					vertex.influences = std::move(influences[vertexIndex]);
					output.vertices.push_back(std::move(vertex));
				}

				for (SkinnedMeshMorphTarget& cookedMorph : output.morphTargets)
				{
					const auto sourceMorph = morphTargets.find(cookedMorph.stableTargetId);
					const FbxGeneratedMorphTarget* source = sourceMorph != morphTargets.end()
						? sourceMorph->second
						: nullptr;
					for (unsigned int vertexIndex = 0u; vertexIndex < mesh.mNumVertices; ++vertexIndex)
					{
						std::array<float, 3u> positionDelta{};
						std::array<float, 3u> normalDelta{};
						std::array<float, 3u> tangentDelta{};
						if (source != nullptr && source->sourceMeshIndex == sourceMeshIndex
							&& vertexIndex < source->positionDeltas.size())
						{
							const auto& localDelta = source->positionDeltas[vertexIndex];
							const aiVector3D transformed = TransformDeformationDelta(
								nodeTransform,
								{ localDelta.x, localDelta.y, localDelta.z });
							positionDelta = { transformed.x, transformed.y, transformed.z };
						}
						if (source != nullptr && !source->normalDeltas.empty()
							&& source->sourceMeshIndex == sourceMeshIndex && vertexIndex < source->normalDeltas.size())
						{
							const auto& localDelta = source->normalDeltas[vertexIndex];
							const aiVector3D transformed = TransformDeformationDelta(
								nodeTransform,
								{ localDelta.x, localDelta.y, localDelta.z });
							normalDelta = { transformed.x, transformed.y, transformed.z };
						}
						if (source != nullptr && !source->tangentDeltas.empty()
							&& source->sourceMeshIndex == sourceMeshIndex && vertexIndex < source->tangentDeltas.size())
						{
							const auto& localDelta = source->tangentDeltas[vertexIndex];
							const aiVector3D transformed = TransformDeformationDelta(
								nodeTransform,
								{ localDelta.x, localDelta.y, localDelta.z });
							tangentDelta = { transformed.x, transformed.y, transformed.z };
						}
						cookedMorph.positionDeltas.push_back(positionDelta);
						if (source != nullptr && !source->normalDeltas.empty())
						{
							cookedMorph.normalDeltas.push_back(normalDelta);
						}
						if (source != nullptr && !source->tangentDeltas.empty())
						{
							cookedMorph.tangentDeltas.push_back(tangentDelta);
						}
					}
				}

				for (unsigned int faceIndex = 0u; faceIndex < mesh.mNumFaces; ++faceIndex)
				{
					const aiFace& face = mesh.mFaces[faceIndex];
					if (face.mNumIndices != 3u)
					{
						continue;
					}
					output.indices.push_back(vertexOffset + face.mIndices[0u]);
					output.indices.push_back(vertexOffset + face.mIndices[1u]);
					output.indices.push_back(vertexOffset + face.mIndices[2u]);
				}
			}

			for (unsigned int childIndex = 0u; childIndex < node.mNumChildren; ++childIndex)
			{
				if (node.mChildren[childIndex] != nullptr
					&& !AppendDeformableMeshes(
						scene,
						*node.mChildren[childIndex],
						nodeTransform,
						nodeLookup,
						morphTargets,
						output,
						package))
				{
					return false;
				}
			}
			return true;
		}

		bool CookDeformablePackageMesh(
			const FbxPackageImportOptions& options,
			const aiScene& scene,
			const FbxNodeLookup& nodeLookup,
			const FbxGeneratedSkeleton& skeleton,
			const FbxAnimatedPackageData& animation,
			ModelPackageImportResult& package)
		{
			if (package.generatedMeshes.empty())
			{
				AddDiagnostic(
					package,
					ModelDiagnosticSeverity::Error,
					"FbxDeformableOutputMissing",
					options.sourceRelativePath.generic_string(),
					"The animated FBX package has no generated TEMF destination for its deformable payload.",
					"Restore renderable triangle geometry and reimport the complete character package.");
				return false;
			}

			SkinnedMeshCookInput cookInput;
			std::vector<const FbxGeneratedMorphTarget*> orderedMorphs;
			for (const FbxGeneratedMorphTarget& morph : animation.morphTargets)
			{
				orderedMorphs.push_back(&morph);
			}
			std::sort(orderedMorphs.begin(), orderedMorphs.end(), [](const auto* left, const auto* right) {
				return std::tie(left->stableId, left->semanticKey) < std::tie(right->stableId, right->semanticKey);
			});
			for (const FbxGeneratedMorphTarget* morph : orderedMorphs)
			{
				SkinnedMeshMorphTarget cookedMorph;
				cookedMorph.stableTargetId = static_cast<std::uint32_t>(morph->stableId);
				cookInput.morphTargets.push_back(std::move(cookedMorph));
			}
			std::unordered_map<std::uint32_t, const FbxGeneratedMorphTarget*> morphLookup;
			for (const FbxGeneratedMorphTarget* morph : orderedMorphs)
			{
				morphLookup.emplace(static_cast<std::uint32_t>(morph->stableId), morph);
			}
			if (!AppendDeformableMeshes(
				scene,
				*scene.mRootNode,
				aiMatrix4x4(),
				nodeLookup,
				morphLookup,
				cookInput,
				package))
			{
				return false;
			}

			SkinnedMeshCookSettings settings;
			settings.sourceSkeletonJointCount = static_cast<std::uint32_t>(skeleton.asset.joints.size());
			settings.skeletonCompatibilityFingerprint = skeleton.asset.compatibilityFingerprint;
			SkinnedMeshCookReport report;
			const std::filesystem::path compiledPath = options.buildRoot / package.generatedMeshes.front().compiledPath;
			if (!CompileSkinnedMeshFromRaw(options.sourceRelativePath, compiledPath, cookInput, &report, settings))
			{
				std::string message = report.diagnosticMessage;
				if (report.failingVertexIndex != std::numeric_limits<std::uint32_t>::max())
				{
					message += " Failing vertex: " + std::to_string(report.failingVertexIndex) + ".";
				}
				if (report.failingJointIndex != std::numeric_limits<std::uint32_t>::max())
				{
					message += " Failing joint: " + std::to_string(report.failingJointIndex) + ".";
				}
				if (report.preservedLastKnownGood)
				{
					message += " The previous TEMF generation was preserved.";
				}
				AddDiagnostic(
					package,
					ModelDiagnosticSeverity::Error,
					report.diagnosticCode.empty() ? "FbxDeformableCookFailed" : report.diagnosticCode,
					package.generatedMeshes.front().compiledPath.generic_string(),
					std::move(message),
					report.recommendedCorrection);
				return false;
			}

			GeneratedModelMeshAsset& generatedMesh = package.generatedMeshes.front();
			std::size_t reducedVertexCount = 0u;
			float maximumDiscardedWeight = 0.0f;
			for (const SkinnedMeshCookedVertex& vertex : report.cookedVertices)
			{
				if (vertex.discardedWeight > 0.0f)
				{
					++reducedVertexCount;
					maximumDiscardedWeight = std::max(maximumDiscardedWeight, vertex.discardedWeight);
				}
			}
			if (reducedVertexCount > 0u)
			{
				AddDiagnostic(
					package,
					ModelDiagnosticSeverity::Warning,
					"FbxInfluenceReductionApplied",
					generatedMesh.compiledPath.generic_string(),
					"Deterministic four-influence reduction affected " + std::to_string(reducedVertexCount)
						+ " vertices; the largest discarded source-weight sum was "
						+ std::to_string(maximumDiscardedWeight) + ".",
					"Review skin fidelity or reduce authored influences to four per vertex before export.");
			}
			AddCoverageTag(generatedMesh, "deformable-temf");
			if (!animation.skinnedMeshes.empty()) AddCoverageTag(generatedMesh, "skinned-deformation");
			if (!animation.morphTargets.empty()) AddCoverageTag(generatedMesh, "morph-deformation");
			PopulateGeneratedMeshVirtualGeometryMetadata(compiledPath, generatedMesh, package.diagnostics);
			AddDiagnostic(
				package,
				ModelDiagnosticSeverity::Info,
				"FbxDeformableCookSummary",
				generatedMesh.compiledPath.generic_string(),
				"Cooked a real deformable TEMF payload with "
					+ std::to_string(report.cookedVertices.size()) + " vertices, "
					+ std::to_string(report.palettePartitions.size()) + " palettes, and "
					+ std::to_string(report.deformableTemf.morphTargetCount) + " morph targets.",
				"Zero-weight vertices use the format's explicit identity-deformation record.");
			return true;
		}

		std::string HashBytesHex(std::span<const std::uint8_t> bytes)
		{
			return StableHashSuffix(AppendStableHash(1469598103934665603ull, bytes));
		}

		std::uint64_t BuildSkeletonCompatibilityFingerprint(
			const std::vector<FbxAnimatedNode>& nodes,
			const std::vector<bool>& deformingNodes,
			const std::vector<TerraEngine::Animation::AnimationMatrix4x4>& inverseBinds)
		{
			std::uint64_t hash = 1469598103934665603ull;
			for (std::size_t index = 0u; index < nodes.size(); ++index)
			{
				hash = AppendStablePodHash(hash, nodes[index].stableId);
				hash = AppendStablePodHash(hash, nodes[index].parentIndex);
				hash = AppendStablePodHash(hash, nodes[index].localTransform);
				const std::uint8_t deforming = deformingNodes[index] ? 1u : 0u;
				hash = AppendStablePodHash(hash, deforming);
				hash = AppendStablePodHash(hash, inverseBinds[index]);
			}
			return hash == 0u ? 1u : hash;
		}

		bool WriteGeneratedAnimationBytes(
			const FbxPackageImportOptions& options,
			const std::filesystem::path& relativePath,
			std::span<const std::uint8_t> bytes)
		{
			const std::filesystem::path assetPath = (options.assetRoot / relativePath).lexically_normal();
			const std::filesystem::path buildPath = (options.buildRoot / relativePath).lexically_normal();
			if (!WriteBinaryAtomically(assetPath, bytes))
			{
				return false;
			}
			if (assetPath == buildPath)
			{
				return true;
			}
			return WriteBinaryAtomically(buildPath, bytes);
		}

		std::optional<FbxGeneratedSkeleton> BuildSkeletonCandidate(
			const FbxPackageImportOptions& options,
			const FbxCoordinateConversion& conversion,
			const std::vector<FbxAnimatedNode>& nodes,
			const std::vector<bool>& deformingNodes,
			const std::vector<TerraEngine::Animation::AnimationMatrix4x4>& inverseBinds,
			ModelPackageImportResult& package)
		{
			using namespace TerraEngine::Animation;
			if (nodes.empty() || nodes.size() != deformingNodes.size() || nodes.size() != inverseBinds.size())
			{
				AddDiagnostic(
					package,
					ModelDiagnosticSeverity::Error,
					"FbxSkeletonHierarchyInvalid",
					options.sourceRelativePath.generic_string(),
					"The normalized FBX hierarchy is empty or internally inconsistent.",
					"Check the exported hierarchy and reimport the package.");
				return std::nullopt;
			}
			if (ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, nodes.size()) != AnimationStatus::Ok)
			{
				AddDiagnostic(
					package,
					ModelDiagnosticSeverity::Error,
					"FbxSkeletonJointLimitExceeded",
					options.sourceRelativePath.generic_string(),
					"The FBX hierarchy exceeds the frozen skeleton joint hard limit.",
					"Split the character into supported skeletons before reimporting.");
				return std::nullopt;
			}

			FbxGeneratedSkeleton generated;
			generated.semanticKey = "skeleton:scene";
			generated.guid = BuildStableAnimationSubAssetUuid(
				options.packageGuid,
				AnimationImportSubAssetKind::Skeleton,
				generated.semanticKey);
			generated.sourcePath = package.package.importRoot / "Skeleton.teskel";
			generated.compiledPath = generated.sourcePath;

			SkeletonAsset skeleton;
			skeleton.assetId = ToAnimationGuid(generated.guid);
			skeleton.compatibilityFingerprint = BuildSkeletonCompatibilityFingerprint(nodes, deformingNodes, inverseBinds);
			skeleton.referencePoseHash = skeleton.compatibilityFingerprint;
			skeleton.rootJointIndex = 0u;
			skeleton.source.unitScaleMeters = conversion.unitScaleMeters;
			skeleton.source.upAxis = ToSourceAxis(conversion.upAxis, conversion.upAxisSign);
			skeleton.source.forwardAxis = ToSourceAxis(conversion.frontAxis, conversion.frontAxisSign);
			skeleton.source.sourcePackagePath = options.sourceRelativePath.generic_string();
			skeleton.inverseBindMatrices = inverseBinds;
			skeleton.joints.reserve(nodes.size());
			for (std::size_t index = 0u; index < nodes.size(); ++index)
			{
				SkeletonJoint joint;
				joint.stableId = nodes[index].stableId;
				joint.parentIndex = nodes[index].parentIndex;
				joint.localBindTransform = nodes[index].localTransform;
				joint.deformsMesh = deformingNodes[index];
				joint.displayName = nodes[index].displayName;
				joint.normalizedSourcePath = nodes[index].semanticKey;
				joint.semanticRole = "joint:" + nodes[index].semanticKey;
				skeleton.joints.push_back(std::move(joint));
				if (nodes[index].socketCandidate)
				{
					SkeletonSocket socket;
					socket.stableId = BuildSemanticHash(options.packageGuid, "fbx-socket", nodes[index].semanticKey);
					socket.jointStableId = nodes[index].stableId;
					socket.localTransform = {};
					socket.displayName = nodes[index].displayName;
					socket.semanticRole = "socket:" + nodes[index].semanticKey;
					skeleton.sockets.push_back(std::move(socket));
				}
			}

			std::vector<std::uint8_t> bytes;
			const AnimationStatus serializeStatus = SerializeSkeletonAsset(skeleton, bytes);
			if (serializeStatus != AnimationStatus::Ok)
			{
				AddDiagnostic(
					package,
					ModelDiagnosticSeverity::Error,
					"FbxSkeletonCookFailed",
					generated.semanticKey,
					"The normalized FBX skeleton failed checked .teskel serialization with status "
						+ std::string(ToString(serializeStatus)) + ".",
					"Fix invalid hierarchy, transform, bind, socket, or hard-limit data before reimporting.");
				return std::nullopt;
			}
			generated.asset = std::move(skeleton);
			return generated;
		}

		bool WriteSkeletonCandidate(
			const FbxPackageImportOptions& options,
			const FbxGeneratedSkeleton& generated,
			ModelPackageImportResult& package)
		{
			std::vector<std::uint8_t> bytes;
			const TerraEngine::Animation::AnimationStatus serializeStatus =
				TerraEngine::Animation::SerializeSkeletonAsset(generated.asset, bytes);
			if (serializeStatus != TerraEngine::Animation::AnimationStatus::Ok
				|| !WriteGeneratedAnimationBytes(options, generated.compiledPath, bytes))
			{
				AddDiagnostic(
					package,
					ModelDiagnosticSeverity::Error,
					"FbxSkeletonWriteFailed",
					generated.compiledPath.generic_string(),
					"The checked .teskel candidate could not be serialized or atomically replaced in the staging roots.",
					"Close programs holding the output file, verify project permissions, and reimport.");
				return false;
			}

			GeneratedAnimationSubAsset manifestAsset;
			manifestAsset.guid = generated.guid;
			manifestAsset.kind = AnimationImportSubAssetKind::Skeleton;
			manifestAsset.semanticKey = generated.semanticKey;
			manifestAsset.displayName = "Skeleton";
			manifestAsset.sourcePath = generated.sourcePath;
			manifestAsset.compiledPath = generated.compiledPath;
			manifestAsset.compatibilityFingerprint = generated.asset.compatibilityFingerprint;
			manifestAsset.contentHashHex = HashBytesHex(bytes);
			package.animationManifest.subAssets.push_back(std::move(manifestAsset));
			return true;
		}

		TerraEngine::Animation::AnimationFixedTick ToFixedAnimationTick(
			double sourceTime,
			double sourceTicksPerSecond,
			double sourceOrigin)
		{
			const double seconds = (sourceTime - sourceOrigin) / sourceTicksPerSecond;
			const double fixedTick = seconds * static_cast<double>(TerraEngine::Animation::kAnimationFixedTicksPerSecond);
			if (!std::isfinite(fixedTick))
			{
				return -1;
			}
			if (fixedTick <= 0.0)
			{
				return 0;
			}
			if (fixedTick >= static_cast<double>(std::numeric_limits<TerraEngine::Animation::AnimationFixedTick>::max()))
			{
				return std::numeric_limits<TerraEngine::Animation::AnimationFixedTick>::max();
			}
			return static_cast<TerraEngine::Animation::AnimationFixedTick>(std::llround(fixedTick));
		}

		TerraEngine::Animation::AnimationQuaternion NormalizeAnimationQuaternion(const aiQuaternion& source)
		{
			const float lengthSquared = source.x * source.x + source.y * source.y
				+ source.z * source.z + source.w * source.w;
			if (!std::isfinite(lengthSquared) || lengthSquared <= std::numeric_limits<float>::epsilon())
			{
				return {};
			}
			const float reciprocalLength = 1.0f / std::sqrt(lengthSquared);
			return {
				source.x * reciprocalLength,
				source.y * reciprocalLength,
				source.z * reciprocalLength,
				source.w * reciprocalLength
			};
		}

		template<typename Key>
		void AppendOrReplaceCollapsedKey(std::vector<Key>& keys, Key key, bool& collapsedTimestamp)
		{
			if (!keys.empty() && keys.back().tick == key.tick)
			{
				keys.back() = std::move(key);
				collapsedTimestamp = true;
				return;
			}
			keys.push_back(std::move(key));
		}

		double FindAnimationOrigin(const aiAnimation& animation)
		{
			double minimum = 0.0;
			for (unsigned int channelIndex = 0u; channelIndex < animation.mNumChannels; ++channelIndex)
			{
				const aiNodeAnim* channel = animation.mChannels[channelIndex];
				if (channel == nullptr)
				{
					continue;
				}
				if (channel->mNumPositionKeys > 0u) minimum = std::min(minimum, channel->mPositionKeys[0u].mTime);
				if (channel->mNumRotationKeys > 0u) minimum = std::min(minimum, channel->mRotationKeys[0u].mTime);
				if (channel->mNumScalingKeys > 0u) minimum = std::min(minimum, channel->mScalingKeys[0u].mTime);
			}
			for (unsigned int channelIndex = 0u; channelIndex < animation.mNumMorphMeshChannels; ++channelIndex)
			{
				const aiMeshMorphAnim* channel = animation.mMorphMeshChannels[channelIndex];
				if (channel != nullptr && channel->mNumKeys > 0u)
				{
					minimum = std::min(minimum, channel->mKeys[0u].mTime);
				}
			}
			return minimum;
		}

		TerraEngine::Animation::AnimationFixedTick FindAnimationDuration(
			const aiAnimation& animation,
			double sourceTicksPerSecond,
			double sourceOrigin)
		{
			double maximum = std::max(0.0, animation.mDuration);
			for (unsigned int channelIndex = 0u; channelIndex < animation.mNumChannels; ++channelIndex)
			{
				const aiNodeAnim* channel = animation.mChannels[channelIndex];
				if (channel == nullptr)
				{
					continue;
				}
				if (channel->mNumPositionKeys > 0u) maximum = std::max(maximum, channel->mPositionKeys[channel->mNumPositionKeys - 1u].mTime);
				if (channel->mNumRotationKeys > 0u) maximum = std::max(maximum, channel->mRotationKeys[channel->mNumRotationKeys - 1u].mTime);
				if (channel->mNumScalingKeys > 0u) maximum = std::max(maximum, channel->mScalingKeys[channel->mNumScalingKeys - 1u].mTime);
			}
			for (unsigned int channelIndex = 0u; channelIndex < animation.mNumMorphMeshChannels; ++channelIndex)
			{
				const aiMeshMorphAnim* channel = animation.mMorphMeshChannels[channelIndex];
				if (channel != nullptr && channel->mNumKeys > 0u)
				{
					maximum = std::max(maximum, channel->mKeys[channel->mNumKeys - 1u].mTime);
				}
			}
			return std::max<TerraEngine::Animation::AnimationFixedTick>(
				1,
				ToFixedAnimationTick(maximum, sourceTicksPerSecond, sourceOrigin));
		}

		std::string BuildClipSemanticKey(
			const aiAnimation& animation,
			const FbxNodeLookup& lookup)
		{
			const std::string displayName = animation.mName.length > 0u
				? std::string(animation.mName.C_Str())
				: std::string("unnamed");
			std::string semanticKey = "clip:" + NormalizeSemanticSegment(displayName, "unnamed");
			if (animation.mName.length == 0u)
			{
				std::vector<std::string> targets;
				for (unsigned int index = 0u; index < animation.mNumChannels; ++index)
				{
					const aiNodeAnim* channel = animation.mChannels[index];
					if (channel == nullptr)
					{
						continue;
					}
					const auto found = lookup.indicesByName.find(channel->mNodeName.C_Str());
					if (found != lookup.indicesByName.end() && found->second.size() == 1u)
					{
						targets.push_back(lookup.semanticKeysByIndex[found->second.front()]);
					}
				}
				for (unsigned int index = 0u; index < animation.mNumMorphMeshChannels; ++index)
				{
					if (animation.mMorphMeshChannels[index] != nullptr)
					{
						targets.push_back("morph:" + std::string(animation.mMorphMeshChannels[index]->mName.C_Str()));
					}
				}
				std::sort(targets.begin(), targets.end());
				std::uint64_t signature = 1469598103934665603ull;
				for (const std::string& target : targets)
				{
					signature = AppendStableHash(signature, target);
				}
				semanticKey += ":" + StableHashSuffix(signature);
			}
			return semanticKey;
		}

		float QuaternionYawRadians(const TerraEngine::Animation::AnimationQuaternion& rotation)
		{
			return std::atan2(
				2.0f * (rotation.w * rotation.y + rotation.x * rotation.z),
				1.0f - 2.0f * (rotation.y * rotation.y + rotation.z * rotation.z));
		}

		void AddVectorSourceTrack(
			std::uint64_t stableTargetId,
			TerraEngine::Animation::AnimationClipTrackKind kind,
			TerraEngine::Animation::AnimationInterpolation interpolation,
			const std::vector<FbxVectorKey>& keys,
			std::vector<AnimationSourceTrack>& tracks)
		{
			if (keys.empty())
			{
				return;
			}
			AnimationSourceTrack track;
			track.stableTargetId = stableTargetId;
			track.kind = kind;
			track.interpolation = keys.size() == 1u
				? TerraEngine::Animation::AnimationInterpolation::Constant
				: interpolation;
			for (const FbxVectorKey& key : keys)
			{
				AnimationSourceKey sourceKey;
				sourceKey.tick = key.tick;
				sourceKey.vectorValue = key.value;
				track.keys.push_back(sourceKey);
			}
			tracks.push_back(std::move(track));
		}

		void AddRotationSourceTrack(
			std::uint64_t stableTargetId,
			TerraEngine::Animation::AnimationInterpolation interpolation,
			const std::vector<FbxQuaternionKey>& keys,
			std::vector<AnimationSourceTrack>& tracks)
		{
			if (keys.empty())
			{
				return;
			}
			AnimationSourceTrack track;
			track.stableTargetId = stableTargetId;
			track.kind = TerraEngine::Animation::AnimationClipTrackKind::Rotation;
			track.interpolation = keys.size() == 1u
				? TerraEngine::Animation::AnimationInterpolation::Constant
				: interpolation;
			for (const FbxQuaternionKey& key : keys)
			{
				AnimationSourceKey sourceKey;
				sourceKey.tick = key.tick;
				sourceKey.rotationValue = key.value;
				track.keys.push_back(sourceKey);
			}
			tracks.push_back(std::move(track));
		}

		std::vector<FbxGeneratedAnimationClip> ExtractCompileAndWriteClips(
			const FbxPackageImportOptions& options,
			const aiScene& scene,
			const FbxNodeLookup& lookup,
			const std::vector<bool>& deformingNodes,
			const std::unordered_map<std::string, std::vector<std::size_t>>& morphTargetsByMeshName,
			const std::vector<FbxGeneratedMorphTarget>& morphTargets,
			const FbxGeneratedSkeleton& skeleton,
			ModelPackageImportResult& package)
		{
			using namespace TerraEngine::Animation;
			std::vector<FbxGeneratedAnimationClip> clips;
			if (ValidateAnimationHardLimit(AnimationLimit::PackageClips, scene.mNumAnimations) != AnimationStatus::Ok)
			{
				AddDiagnostic(
					package,
					ModelDiagnosticSeverity::Error,
					"FbxClipLimitExceeded",
					options.sourceRelativePath.generic_string(),
					"The FBX package declares more clips than the frozen package hard limit.",
					"Split the source animation library across supported packages.");
				return clips;
			}

			std::size_t rootMotionNodeIndex = 0u;
			for (std::size_t index = 0u; index < deformingNodes.size(); ++index)
			{
				if (!deformingNodes[index])
				{
					continue;
				}
				const std::int32_t parentIndex = skeleton.asset.joints[index].parentIndex;
				if (parentIndex < 0 || !deformingNodes[static_cast<std::size_t>(parentIndex)])
				{
					rootMotionNodeIndex = index;
					break;
				}
			}

			std::unordered_map<std::string, std::size_t> semanticKeyCounts;
			for (unsigned int animationIndex = 0u; animationIndex < scene.mNumAnimations; ++animationIndex)
			{
				if (scene.mAnimations[animationIndex] != nullptr)
				{
					++semanticKeyCounts[BuildClipSemanticKey(*scene.mAnimations[animationIndex], lookup)];
				}
			}
			std::unordered_set<std::string> reportedAmbiguousSemanticKeys;
			for (unsigned int animationIndex = 0u; animationIndex < scene.mNumAnimations; ++animationIndex)
			{
				const aiAnimation* animation = scene.mAnimations[animationIndex];
				if (animation == nullptr)
				{
					continue;
				}
				const double ticksPerSecond = animation->mTicksPerSecond > 0.0
					? animation->mTicksPerSecond
					: 30.0;
				if (animation->mTicksPerSecond <= 0.0)
				{
					AddDiagnostic(
						package,
						ModelDiagnosticSeverity::Warning,
						"FbxClipRateMissing",
						animation->mName.C_Str(),
						"The FBX clip does not declare ticks-per-second; the Assimp fallback of 30 Hz was used.",
						"Set an explicit frame rate in the DCC export settings for reproducible timing.");
				}
				const double sourceOrigin = FindAnimationOrigin(*animation);
				const AnimationFixedTick durationTicks = FindAnimationDuration(*animation, ticksPerSecond, sourceOrigin);
				std::string semanticKey = BuildClipSemanticKey(*animation, lookup);
				if (semanticKeyCounts[semanticKey] > 1u)
				{
					if (reportedAmbiguousSemanticKeys.insert(semanticKey).second)
					{
						AddDiagnostic(
							package,
							ModelDiagnosticSeverity::Error,
							"FbxAmbiguousClipIdentity",
							animation->mName.C_Str(),
							"Multiple FBX clips have the same normalized semantic identity.",
							"Give every clip a unique semantic name before reimporting.");
					}
					continue;
				}

				FbxGeneratedAnimationClip generated;
				generated.semanticKey = semanticKey;
				generated.displayName = animation->mName.length > 0u
					? std::string(animation->mName.C_Str())
					: "Animation";
				generated.guid = BuildStableAnimationSubAssetUuid(
					options.packageGuid,
					AnimationImportSubAssetKind::AnimationClip,
					semanticKey);
				generated.sourceTicksPerSecond = ticksPerSecond;
				const std::string clipFileName = SanitizeAssetName(generated.displayName, "Animation")
					+ "_" + StableHashSuffix(BuildSemanticHash(options.packageGuid, "fbx-clip-path", semanticKey))
					+ ".teanim";
				generated.sourcePath = package.package.importRoot / "Animations" / clipFileName;
				generated.compiledPath = generated.sourcePath;

				AnimationClipCompileInput compileInput;
				compileInput.assetId = ToAnimationGuid(generated.guid);
				compileInput.skeletonAssetId = skeleton.asset.assetId;
				compileInput.compatibilityFingerprint = skeleton.asset.compatibilityFingerprint;
				compileInput.durationTicks = durationTicks;
				compileInput.sampleRate = kAnimationFixedTicksPerSecond;
				compileInput.segmentDurationTicks = 30;
				bool collapsedTimestamp = false;
				std::unordered_set<std::string> reportedUnresolvedAnimationTargets;

				for (unsigned int channelIndex = 0u; channelIndex < animation->mNumChannels; ++channelIndex)
				{
					const aiNodeAnim* sourceChannel = animation->mChannels[channelIndex];
					if (sourceChannel == nullptr)
					{
						continue;
					}
					const std::optional<std::size_t> nodeIndex = ResolveAnimationNodeIndex(
						sourceChannel->mNodeName.C_Str(),
						lookup,
						deformingNodes);
					if (!nodeIndex)
					{
						const std::string targetName = sourceChannel->mNodeName.C_Str();
						if (reportedUnresolvedAnimationTargets.insert(targetName).second)
						{
							// Multiple component channels can refer to the same unresolved
							// target. Report the authoring fault once per clip so diagnostics
							// stay actionable without hiding the rejected clip state.
							AddDiagnostic(
								package,
								ModelDiagnosticSeverity::Error,
								"FbxAnimationTargetMissing",
								targetName,
								"An FBX animation channel has no unambiguous hierarchy target or unique deforming duplicate-name target.",
								"Give animated nodes unique names, or ensure exactly one duplicate-name node deforms the imported skin.");
						}
						continue;
					}

					FbxNodeAnimationChannel channel;
					channel.targetStableId = skeleton.asset.joints[*nodeIndex].stableId;
					channel.targetSemanticKey = skeleton.asset.joints[*nodeIndex].normalizedSourcePath;
					for (unsigned int keyIndex = 0u; keyIndex < sourceChannel->mNumPositionKeys; ++keyIndex)
					{
						const aiVectorKey& sourceKey = sourceChannel->mPositionKeys[keyIndex];
						AppendOrReplaceCollapsedKey(channel.translations, FbxVectorKey{
							ToFixedAnimationTick(sourceKey.mTime, ticksPerSecond, sourceOrigin),
							{ sourceKey.mValue.x, sourceKey.mValue.y, sourceKey.mValue.z }
						}, collapsedTimestamp);
					}
					for (unsigned int keyIndex = 0u; keyIndex < sourceChannel->mNumRotationKeys; ++keyIndex)
					{
						const aiQuatKey& sourceKey = sourceChannel->mRotationKeys[keyIndex];
						AppendOrReplaceCollapsedKey(channel.rotations, FbxQuaternionKey{
							ToFixedAnimationTick(sourceKey.mTime, ticksPerSecond, sourceOrigin),
							NormalizeAnimationQuaternion(sourceKey.mValue)
						}, collapsedTimestamp);
					}
					for (unsigned int keyIndex = 0u; keyIndex < sourceChannel->mNumScalingKeys; ++keyIndex)
					{
						const aiVectorKey& sourceKey = sourceChannel->mScalingKeys[keyIndex];
						AppendOrReplaceCollapsedKey(channel.scales, FbxVectorKey{
							ToFixedAnimationTick(sourceKey.mTime, ticksPerSecond, sourceOrigin),
							{ sourceKey.mValue.x, sourceKey.mValue.y, sourceKey.mValue.z }
						}, collapsedTimestamp);
					}

					AddVectorSourceTrack(
						channel.targetStableId,
						AnimationClipTrackKind::Translation,
						channel.translationInterpolation,
						channel.translations,
						compileInput.tracks);
					AddRotationSourceTrack(
						channel.targetStableId,
						channel.rotationInterpolation,
						channel.rotations,
						compileInput.tracks);
					AddVectorSourceTrack(
						channel.targetStableId,
						AnimationClipTrackKind::Scale,
						channel.scaleInterpolation,
						channel.scales,
						compileInput.tracks);

					if (*nodeIndex == rootMotionNodeIndex && !channel.translations.empty() && !channel.rotations.empty())
					{
						AddVectorSourceTrack(
							channel.targetStableId,
							AnimationClipTrackKind::RootTranslation,
							channel.translationInterpolation,
							channel.translations,
							compileInput.tracks);
						AnimationSourceTrack yawTrack;
						yawTrack.stableTargetId = channel.targetStableId;
						yawTrack.kind = AnimationClipTrackKind::RootYaw;
						yawTrack.interpolation = channel.rotations.size() == 1u
							? AnimationInterpolation::Constant
							: AnimationInterpolation::Linear;
						float priorYaw = 0.0f;
						bool firstYaw = true;
						for (const FbxQuaternionKey& rotationKey : channel.rotations)
						{
							float yaw = QuaternionYawRadians(rotationKey.value);
							if (!firstYaw)
							{
								while (yaw - priorYaw > 3.14159265358979323846f) yaw -= 6.28318530717958647692f;
								while (yaw - priorYaw < -3.14159265358979323846f) yaw += 6.28318530717958647692f;
							}
							yawTrack.keys.push_back({ rotationKey.tick, {}, {}, yaw });
							priorYaw = yaw;
							firstYaw = false;
						}
						compileInput.tracks.push_back(std::move(yawTrack));
					}
					generated.nodeChannels.push_back(std::move(channel));
				}

				for (unsigned int channelIndex = 0u; channelIndex < animation->mNumMorphMeshChannels; ++channelIndex)
				{
					const aiMeshMorphAnim* sourceChannel = animation->mMorphMeshChannels[channelIndex];
					if (sourceChannel == nullptr)
					{
						continue;
					}
					const auto targetGroup = morphTargetsByMeshName.find(sourceChannel->mName.C_Str());
					if (targetGroup == morphTargetsByMeshName.end())
					{
						AddDiagnostic(
							package,
							ModelDiagnosticSeverity::Error,
							"FbxMorphAnimationTargetMissing",
							sourceChannel->mName.C_Str(),
							"An FBX morph animation channel has no compatible imported morph target group.",
							"Export the morph attachment and its animation in the same mesh package.");
						continue;
					}

					FbxMorphAnimationChannel channel;
					channel.meshSemanticKey = sourceChannel->mName.C_Str();
					for (unsigned int keyIndex = 0u; keyIndex < sourceChannel->mNumKeys; ++keyIndex)
					{
						const aiMeshMorphKey& sourceKey = sourceChannel->mKeys[keyIndex];
						FbxMorphWeightKey key;
						key.tick = ToFixedAnimationTick(sourceKey.mTime, ticksPerSecond, sourceOrigin);
						if (sourceKey.mNumValuesAndWeights > 0u
							&& sourceKey.mValues != nullptr
							&& sourceKey.mWeights != nullptr)
						{
							key.targetIndices.assign(
								sourceKey.mValues,
								sourceKey.mValues + sourceKey.mNumValuesAndWeights);
						}
						key.weights.reserve(sourceKey.mNumValuesAndWeights);
						for (unsigned int valueIndex = 0u; valueIndex < key.targetIndices.size(); ++valueIndex)
						{
							key.weights.push_back(static_cast<float>(sourceKey.mWeights[valueIndex]));
						}
						AppendOrReplaceCollapsedKey(channel.keys, std::move(key), collapsedTimestamp);
					}

					for (const std::size_t emittedMorphIndex : targetGroup->second)
					{
						const FbxGeneratedMorphTarget& morph = morphTargets[emittedMorphIndex];
						AnimationSourceTrack morphTrack;
						morphTrack.stableTargetId = morph.stableId;
						morphTrack.kind = AnimationClipTrackKind::MorphWeight;
						morphTrack.interpolation = channel.keys.size() == 1u
							? AnimationInterpolation::Constant
							: AnimationInterpolation::Linear;
						for (const FbxMorphWeightKey& key : channel.keys)
						{
							float weight = 0.0f;
							for (std::size_t valueIndex = 0u; valueIndex < key.targetIndices.size(); ++valueIndex)
							{
								if (key.targetIndices[valueIndex] == morph.sourceTargetIndex && valueIndex < key.weights.size())
								{
									weight = key.weights[valueIndex];
									break;
								}
							}
							morphTrack.keys.push_back({ key.tick, {}, {}, weight });
						}
						compileInput.tracks.push_back(std::move(morphTrack));
					}
					generated.morphChannels.push_back(std::move(channel));
				}

				if (collapsedTimestamp)
				{
					AddDiagnostic(
						package,
						ModelDiagnosticSeverity::Warning,
						"FbxAnimationTimestampCollapsed",
						semanticKey,
						"Multiple source keys mapped to the same fixed 60 Hz tick; the last chronological value was retained.",
						"Export at a supported time base or increase source key spacing to avoid quantization loss.");
				}
				if (compileInput.tracks.empty())
				{
					AddDiagnostic(
						package,
						ModelDiagnosticSeverity::Error,
						"FbxClipHasNoSupportedTracks",
						semanticKey,
						"The FBX clip contains no valid node or morph channels after normalization.",
						"Export translation, rotation, scale, or morph-weight channels targeting included objects.");
					continue;
				}

				AnimationClipCompileReport compileReport;
				const AnimationStatus compileStatus = CompileAnimationClip(
					compileInput,
					{},
					generated.asset,
					compileReport);
				if (compileStatus != AnimationStatus::Ok)
				{
					AddDiagnostic(
						package,
						ModelDiagnosticSeverity::Error,
						"FbxClipCookFailed",
						semanticKey,
						"The normalized FBX clip failed deterministic .teanim compilation with status "
							+ std::string(ToString(compileStatus)) + ": " + compileReport.diagnostic,
						"Inspect invalid targets, timestamps, non-finite values, hard limits, and compression tolerances.");
					continue;
				}

				std::vector<std::uint8_t> bytes;
				const AnimationStatus serializeStatus = SerializeAnimationClipAsset(generated.asset, bytes);
				if (serializeStatus != AnimationStatus::Ok || !WriteGeneratedAnimationBytes(options, generated.compiledPath, bytes))
				{
					AddDiagnostic(
						package,
						ModelDiagnosticSeverity::Error,
						"FbxClipWriteFailed",
						generated.compiledPath.generic_string(),
						"The checked .teanim payload could not be serialized or atomically replaced.",
						"Close programs holding the output, verify permissions, and reimport after correcting any invalid clip data.");
					continue;
				}

				GeneratedAnimationSubAsset manifestAsset;
				manifestAsset.guid = generated.guid;
				manifestAsset.kind = AnimationImportSubAssetKind::AnimationClip;
				manifestAsset.semanticKey = semanticKey;
				manifestAsset.displayName = generated.displayName;
				manifestAsset.sourcePath = generated.sourcePath;
				manifestAsset.compiledPath = generated.compiledPath;
				manifestAsset.compatibilityFingerprint = skeleton.asset.compatibilityFingerprint;
				manifestAsset.contentHashHex = HashBytesHex(bytes);
				manifestAsset.dependencies.push_back({
					skeleton.guid,
					skeleton.asset.compatibilityFingerprint,
					true
				});
				package.animationManifest.subAssets.push_back(std::move(manifestAsset));
				clips.push_back(std::move(generated));
			}
			std::sort(clips.begin(), clips.end(), [](const auto& left, const auto& right) {
				return std::tie(left.semanticKey, left.guid) < std::tie(right.semanticKey, right.guid);
			});
			return clips;
		}

		void PopulateDeformationManifest(
			const FbxPackageImportOptions& options,
			const FbxGeneratedSkeleton& skeleton,
			const FbxAnimatedPackageData& animation,
			ModelPackageImportResult& package)
		{
			const std::filesystem::path meshSourcePath = package.generatedMeshes.empty()
				? options.sourceRelativePath
				: package.generatedMeshes.front().sourcePath;
			const std::filesystem::path meshCompiledPath = package.generatedMeshes.empty()
				? std::filesystem::path{}
				: package.generatedMeshes.front().compiledPath;
			const std::string cookedPayloadHash = meshCompiledPath.empty()
				? std::string{}
				: ComputeFileHashHex(options.buildRoot / meshCompiledPath);
			if (!package.generatedMeshes.empty()
				&& (!animation.skinnedMeshes.empty() || !animation.morphTargets.empty()))
			{
				GeneratedAnimationSubAsset manifestAsset;
				const GeneratedModelMeshAsset& generatedMesh = package.generatedMeshes.front();
				manifestAsset.kind = AnimationImportSubAssetKind::DeformableMesh;
				manifestAsset.semanticKey = "deformable:package-mesh";
				manifestAsset.guid = generatedMesh.guid;
				manifestAsset.displayName = generatedMesh.displayName;
				manifestAsset.sourcePath = meshSourcePath;
				manifestAsset.compiledPath = meshCompiledPath;
				manifestAsset.compatibilityFingerprint = skeleton.asset.compatibilityFingerprint;
				std::uint64_t contentHash = 1469598103934665603ull;
				for (const FbxImportedSkinnedMesh& skin : animation.skinnedMeshes)
				{
					contentHash = AppendStableHash(contentHash, skin.semanticKey);
					for (const FbxBoneInfluence& influence : skin.influences)
					{
						contentHash = AppendStablePodHash(contentHash, influence.vertexIndex);
						contentHash = AppendStablePodHash(contentHash, influence.jointStableId);
						contentHash = AppendStablePodHash(contentHash, influence.weight);
					}
				}
				manifestAsset.contentHashHex = cookedPayloadHash.empty()
					? StableHashSuffix(contentHash)
					: cookedPayloadHash;
				manifestAsset.dependencies.push_back({
					skeleton.guid,
					skeleton.asset.compatibilityFingerprint,
					true
				});
				package.animationManifest.subAssets.push_back(std::move(manifestAsset));
			}

			for (const FbxGeneratedMorphTarget& morph : animation.morphTargets)
			{
				GeneratedAnimationSubAsset manifestAsset;
				manifestAsset.guid = morph.guid;
				manifestAsset.kind = AnimationImportSubAssetKind::MorphTarget;
				manifestAsset.semanticKey = morph.semanticKey;
				manifestAsset.displayName = morph.displayName;
				manifestAsset.sourcePath = meshSourcePath;
				manifestAsset.compiledPath = meshCompiledPath;
				manifestAsset.compatibilityFingerprint = skeleton.asset.compatibilityFingerprint;
				std::uint64_t contentHash = 1469598103934665603ull;
				for (const TerraEngine::Animation::AnimationFloat3& delta : morph.positionDeltas)
				{
					contentHash = AppendStablePodHash(contentHash, delta);
				}
				for (const TerraEngine::Animation::AnimationFloat3& delta : morph.normalDeltas)
				{
					contentHash = AppendStablePodHash(contentHash, delta);
				}
				for (const TerraEngine::Animation::AnimationFloat3& delta : morph.tangentDeltas)
				{
					contentHash = AppendStablePodHash(contentHash, delta);
				}
				manifestAsset.contentHashHex = cookedPayloadHash.empty()
					? StableHashSuffix(contentHash)
					: cookedPayloadHash;
				manifestAsset.dependencies.push_back({
					skeleton.guid,
					skeleton.asset.compatibilityFingerprint,
					true
				});
				manifestAsset.dependencies.push_back({
					package.generatedMeshes.front().guid,
					skeleton.asset.compatibilityFingerprint,
					true
				});
				package.animationManifest.subAssets.push_back(std::move(manifestAsset));
			}
		}

		std::uint64_t BuildAnimatedParityFingerprint(const FbxAnimatedPackageData& animation)
		{
			std::uint64_t hash = 1469598103934665603ull;
			hash = AppendStablePodHash(hash, animation.coordinateConversion.unitScaleMeters);
			hash = AppendStablePodHash(hash, animation.coordinateConversion.upAxis);
			hash = AppendStablePodHash(hash, animation.coordinateConversion.upAxisSign);
			hash = AppendStablePodHash(hash, animation.coordinateConversion.frontAxis);
			hash = AppendStablePodHash(hash, animation.coordinateConversion.frontAxisSign);
			hash = AppendStablePodHash(hash, animation.coordinateConversion.coordinateAxis);
			hash = AppendStablePodHash(hash, animation.coordinateConversion.coordinateAxisSign);
			const std::uint8_t normalizedToMeters = animation.coordinateConversion.assimpOutputNormalizedToMeters ? 1u : 0u;
			hash = AppendStablePodHash(hash, normalizedToMeters);
			for (const FbxAnimatedNode& node : animation.nodes)
			{
				hash = AppendStablePodHash(hash, node.stableId);
				hash = AppendStablePodHash(hash, node.parentIndex);
				hash = AppendStablePodHash(hash, node.localTransform);
				hash = AppendStableHash(hash, node.semanticKey);
			}
			std::vector<std::uint64_t> skeletonFingerprints;
			for (const FbxGeneratedSkeleton& skeleton : animation.skeletons)
			{
				skeletonFingerprints.push_back(skeleton.asset.compatibilityFingerprint);
			}
			std::sort(skeletonFingerprints.begin(), skeletonFingerprints.end());
			for (const std::uint64_t fingerprint : skeletonFingerprints) hash = AppendStablePodHash(hash, fingerprint);
			std::vector<std::uint64_t> skinFingerprints;
			for (const FbxImportedSkinnedMesh& skin : animation.skinnedMeshes)
			{
				std::uint64_t skinHash = AppendStableHash(1469598103934665603ull, skin.semanticKey);
				for (const FbxBoneInfluence& influence : skin.influences)
				{
					skinHash = AppendStablePodHash(skinHash, influence.vertexIndex);
					skinHash = AppendStablePodHash(skinHash, influence.jointStableId);
					skinHash = AppendStablePodHash(skinHash, influence.weight);
				}
				skinFingerprints.push_back(skinHash);
			}
			std::sort(skinFingerprints.begin(), skinFingerprints.end());
			for (const std::uint64_t fingerprint : skinFingerprints) hash = AppendStablePodHash(hash, fingerprint);
			std::vector<std::pair<std::string, TerraEngine::Animation::AnimationFixedTick>> clipIdentities;
			for (const FbxGeneratedAnimationClip& clip : animation.clips)
			{
				clipIdentities.emplace_back(clip.semanticKey, clip.asset.durationTicks);
			}
			std::sort(clipIdentities.begin(), clipIdentities.end());
			for (const auto& [semanticKey, duration] : clipIdentities)
			{
				hash = AppendStableHash(hash, semanticKey);
				hash = AppendStablePodHash(hash, duration);
			}
			std::vector<std::uint64_t> morphIds;
			for (const FbxGeneratedMorphTarget& morph : animation.morphTargets) morphIds.push_back(morph.stableId);
			std::sort(morphIds.begin(), morphIds.end());
			for (const std::uint64_t morphId : morphIds) hash = AppendStablePodHash(hash, morphId);
			return hash == 0u ? 1u : hash;
		}
	}

	static ModelPackageImportResult ImportFbxStaticPackage(const FbxPackageImportOptions& options)
	{
		ModelPackageImportResult result{};
		result.packageGuid = options.packageGuid;
		result.package.sourcePath = options.sourceRelativePath;
		result.package.packageKind = ModelPackageKind::Fbx;
		result.package.sourceHashHex = ComputeFileHashHex(options.sourcePath);
		// Generated children follow the logical project-relative source name. The
		// physical source may be a cache or test fixture with a different basename.
		result.package.importRoot = options.sourceRelativePath.parent_path()
			/ (options.sourceRelativePath.stem().string() + "_Generated");

		ReportProgress(options, 0.04f, "Reading", "Opening FBX source data.");
		if (NormalizeExtension(options.sourcePath) != ".fbx")
		{
			AddDiagnostic(result, ModelDiagnosticSeverity::Error, "UnsupportedPackageExtension", options.sourceRelativePath.generic_string(), "FBX importer received a non-FBX source path.");
			PromoteStatusFromDiagnostics(result);
			return result;
		}

		Assimp::Importer importer;
		// Pivot helper nodes vary with exporter details and obscure semantic paths.
		// Baking them preserves the evaluated transform while keeping node identity
		// compatible with glTF's direct hierarchy contract.
		importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

		// Assimp owns and deletes custom progress handlers. Returning false
		// from the handler lets cancel requests abort long FBX parsing instead
		// of forcing project close to wait for the whole parse to finish.
		importer.SetProgressHandler(new FbxAssimpProgressHandler(options));
		ReportProgress(options, 0.12f, "Parsing", "Parsing FBX scene with Assimp.");
		const aiScene* scene = importer.ReadFile(options.sourcePath.string(), kFbxPackageImportFlags);
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
			VirtualGeometryConeSafetyCookSettings coneSafetySettings{};
			coneSafetySettings.proveOneSidedOpaqueClusters = false;
			coneSafetySettings.disabledReason = "FBX package meshes currently merge material safety, so cone culling waits for per-cluster material proof.";
			ReportProgress(options, 0.78f, "Compiling Mesh", "Building Terra mesh and virtual geometry data.");
			if (!CompileMeshFromRaw(options.sourceRelativePath, compiledMeshPath, vertices, indices, true, &compileReport, 60.0f, 1e-6f, compileSettingsPtr, MakeNestedProgress(options, 0.78f, 0.90f), &coneSafetySettings))
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

	FbxAnimatedImportResult ImportFbxPackageWithAnimation(const FbxPackageImportOptions& options)
	{
		FbxAnimatedImportResult result;
		result.package = ImportFbxStaticPackage(options);
		result.package.animationManifest.packageGuid = options.packageGuid;
		result.package.animationManifest.sourceHashHex = result.package.package.sourceHashHex;
		result.package.animationManifest.importerName = "AssimpFbxPackageImporter";
		// Version 3 records the source-derived deformable section key and the
		// duplicate-name animation-target policy. The bump prevents an existing
		// v2 sidecar from being mistaken for this materially different result.
		result.package.animationManifest.importerVersion = "3";
		result.package.animationManifest.settingsFingerprintHex = "fbx-meters-pivots-section-identity-v3";

		if (result.package.status == ModelImportStatus::Failed)
		{
			return result;
		}
		if (options.packageGuid == TerraEngine::EMPTY_UUID)
		{
			AddDiagnostic(
				result.package,
				ModelDiagnosticSeverity::Error,
				"FbxAnimationPackageIdentityMissing",
				options.sourceRelativePath.generic_string(),
				"Animated FBX import requires a stable package GUID before subassets can be cooked.",
				"Create or repair the source asset metadata and reimport the package.");
			PromoteStatusFromDiagnostics(result.package);
			return result;
		}

		Assimp::Importer importer;
		importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
		importer.SetProgressHandler(new FbxAssimpProgressHandler(options));
		const aiScene* scene = importer.ReadFile(options.sourcePath.string(), kFbxPackageImportFlags);
		if (scene == nullptr || scene->mRootNode == nullptr)
		{
			AddDiagnostic(
				result.package,
				ModelDiagnosticSeverity::Error,
				"FbxAnimationParseFailed",
				options.sourceRelativePath.generic_string(),
				std::string("The animation pass could not parse the FBX hierarchy: ") + importer.GetErrorString(),
				"Repair the source package; the common publication transaction will retain the last-known-good generation.");
			PromoteStatusFromDiagnostics(result.package);
			return result;
		}

		bool hasAnimationPayload = scene->HasAnimations();
		for (unsigned int meshIndex = 0u; meshIndex < scene->mNumMeshes; ++meshIndex)
		{
			const aiMesh* mesh = scene->mMeshes[meshIndex];
			hasAnimationPayload = hasAnimationPayload
				|| (mesh != nullptr && (mesh->HasBones() || mesh->mNumAnimMeshes > 0u));
		}
		if (!hasAnimationPayload)
		{
			CanonicalizeAnimationImportManifest(result.package.animationManifest);
			result.package.animationManifest.dependencyFingerprintHex =
				BuildAnimationImportDependencyFingerprint(result.package.animationManifest);
			return result;
		}

		result.animation.coordinateConversion = ReadCoordinateConversion(*scene);
		AddDiagnostic(
			result.package,
			ModelDiagnosticSeverity::Info,
			"FbxCoordinateConversionApplied",
			options.sourceRelativePath.generic_string(),
			"FBX units, handedness, and source axes were normalized once through Assimp GlobalScale into Terra meter-space.",
			"Source axis and unit metadata remain recorded on the generated skeleton for parity diagnostics.");
		if (scene->HasAnimations())
		{
			AddDiagnostic(
				result.package,
				ModelDiagnosticSeverity::Info,
				"FbxCurveInterpolationBaked",
				options.sourceRelativePath.generic_string(),
				"Assimp supplies FBX transform and morph curves as chronological sampled keys; Terra preserves those samples with their effective linear interpolation.",
				"Use a source export sample rate high enough to retain intended cubic or stepped motion.");
			for (unsigned int animationIndex = 0u; animationIndex < scene->mNumAnimations; ++animationIndex)
			{
				const aiAnimation* animation = scene->mAnimations[animationIndex];
				if (animation != nullptr && animation->mNumMeshChannels > 0u)
				{
					AddDiagnostic(
						result.package,
						ModelDiagnosticSeverity::Warning,
						"FbxVertexCacheAnimationUnsupported",
						animation->mName.C_Str(),
						"The FBX clip contains legacy per-frame mesh replacement channels that are distinct from supported morph targets.",
						"Export the deformation as blend shapes/morph targets or skeletal animation.");
				}
			}
		}

		FbxNodeLookup nodeLookup;
		BuildAnimatedNodeHierarchy(
			options,
			*scene->mRootNode,
			-1,
			{},
			result.animation,
			nodeLookup,
			result.package);
		std::vector<bool> deformingNodes(result.animation.nodes.size(), false);
		std::vector<TerraEngine::Animation::AnimationMatrix4x4> inverseBinds(
			result.animation.nodes.size(),
			IdentityAnimationMatrix());
		std::vector<bool> inverseBindAssigned(result.animation.nodes.size(), false);

		const std::string skeletonSemanticKey = "skeleton:scene";
		const TerraEngine::UUID skeletonGuid = BuildStableAnimationSubAssetUuid(
			options.packageGuid,
			AnimationImportSubAssetKind::Skeleton,
			skeletonSemanticKey);
		ExtractSkinsAndInverseBinds(
			options,
			*scene,
			nodeLookup,
			ToAnimationGuid(skeletonGuid),
			result.animation,
			deformingNodes,
			inverseBindAssigned,
			inverseBinds,
			result.package);
		const bool referencePoseBindsReady = NormalizeInverseBindsToRuntimeReferencePose(
			result.animation.nodes,
			deformingNodes,
			inverseBinds,
			result.package);
		const auto morphTargetsByMeshName = ExtractMorphTargets(
			options,
			*scene,
			result.animation,
			result.package);

		const std::optional<FbxGeneratedSkeleton> skeleton = referencePoseBindsReady
			? BuildSkeletonCandidate(
				options,
				result.animation.coordinateConversion,
				result.animation.nodes,
				deformingNodes,
				inverseBinds,
				result.package)
			: std::nullopt;
		if (skeleton)
		{
			const bool requiresDeformableCook = !result.animation.skinnedMeshes.empty()
				|| !result.animation.morphTargets.empty();
			const bool deformableCooked = !requiresDeformableCook
				|| CookDeformablePackageMesh(
					options,
					*scene,
					nodeLookup,
					*skeleton,
					result.animation,
					result.package);
			if (deformableCooked && WriteSkeletonCandidate(options, *skeleton, result.package))
			{
				result.animation.skeletons.push_back(*skeleton);
				result.animation.clips = ExtractCompileAndWriteClips(
					options,
					*scene,
					nodeLookup,
					deformingNodes,
					morphTargetsByMeshName,
					result.animation.morphTargets,
					*skeleton,
					result.package);
				if (requiresDeformableCook)
				{
					PopulateDeformationManifest(options, *skeleton, result.animation, result.package);
				}
			}
		}

		result.animation.parityFingerprint = BuildAnimatedParityFingerprint(result.animation);
		if (HasImportErrors(result.package))
		{
			// A failed skeleton, deformable mesh, morph, or clip candidate must not
			// expose a subset as a publishable generation. T037 owns the protected
			// filesystem transaction and can retain the prior canonical manifest.
			result.package.animationManifest.subAssets.clear();
			result.package.animationManifest.publishedAtomically = false;
		}
		CanonicalizeAnimationImportManifest(result.package.animationManifest);
		result.package.animationManifest.dependencyFingerprintHex =
			BuildAnimationImportDependencyFingerprint(result.package.animationManifest);
		AddDiagnostic(
			result.package,
			ModelDiagnosticSeverity::Info,
			"FbxAnimationImportSummary",
			options.sourceRelativePath.generic_string(),
			"Imported " + std::to_string(result.animation.nodes.size()) + " hierarchy nodes, "
				+ std::to_string(result.animation.skinnedMeshes.size()) + " skinned meshes, "
				+ std::to_string(result.animation.skeletons.size()) + " skeletons, "
				+ std::to_string(result.animation.clips.size()) + " clips, and "
				+ std::to_string(result.animation.morphTargets.size()) + " morph targets.",
			"Inspect earlier diagnostics for any rejected source object or fidelity-changing normalization.");
		PromoteStatusFromDiagnostics(result.package);
		return result;
	}

	ModelPackageImportResult ImportFbxPackage(const FbxPackageImportOptions& options)
	{
		// Existing callers retain their package-shaped return while receiving the
		// same generated animation files and durable manifest as the typed API.
		FbxAnimatedImportResult animated = ImportFbxPackageWithAnimation(options);
		return std::move(animated.package);
	}
}
