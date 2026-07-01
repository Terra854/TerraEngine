// EditorAutomationService.cpp - Contract implementation for editor automation.
//
// This file keeps assistant-facing tool behavior in one place. Transport code
// supplies JSON, bridge code supplies policy, and this service performs the
// editor validation/action work against TerraEngine's existing systems.

#include "EditorAutomationService.h"

#include "../Assets/AssetImportResult.h"
#include "../Assets/AssetDatabase.h"
#include "../Core/EditorContext.h"
#include "../Project/ProjectManager.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "../../Export/BuildProfile.h"
#include "../../Export/ExportValidator.h"
#include "../../AssetCompilers/ModelPackageImporter.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Core/Object/Component.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/SceneMutationQueue.h"
#include "Engine/Scene/RuntimeLayout.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Core/Scheduling/FrameWorkload.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <chrono>
#include <unordered_set>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		std::string EntityToString(entt::entity entity)
		{
			return std::to_string(static_cast<std::uint32_t>(entity));
		}

		bool IsWriteMutation(MutationMode mode)
		{
			return mode != MutationMode::ReadOnly;
		}

		nlohmann::json StatusEnvelope(bool ok, std::string_view requestId, RequestStatus status, std::string_view message)
		{
			nlohmann::json result;
			result["ok"] = ok;
			result["requestId"] = requestId;
			result["status"] = ToString(status);
			result["message"] = message;
			return result;
		}

		bool ContainsCaseInsensitive(std::string value, std::string query)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value.find(query) != std::string::npos;
		}

		std::string LowerAscii(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		}

		const char* CaptureSourceKindJson(TerraEngine::Renderer::RenderCaptureSourceKind sourceKind)
		{
			switch (sourceKind)
			{
			case TerraEngine::Renderer::RenderCaptureSourceKind::GameView: return "gameView";
			case TerraEngine::Renderer::RenderCaptureSourceKind::CameraObject: return "cameraObject";
			case TerraEngine::Renderer::RenderCaptureSourceKind::CustomCamera: return "customCamera";
			default: return "unknown";
			}
		}

		nlohmann::json ReadUVec2(const nlohmann::json& arguments, std::uint32_t defaultWidth = 1280u, std::uint32_t defaultHeight = 720u)
		{
			const std::uint32_t width = arguments.value("width", defaultWidth);
			const std::uint32_t height = arguments.value("height", defaultHeight);
			if (width == 0u || height == 0u)
			{
				return {{"ok", false}, {"message", "Capture width and height must be positive integers."}};
			}
			return {{"ok", true}, {"size", nlohmann::json::array({width, height})}};
		}

		std::optional<glm::vec3> ReadVec3(const nlohmann::json& value)
		{
			if (!value.is_array() || value.size() != 3u)
			{
				return std::nullopt;
			}
			try
			{
				const glm::vec3 result(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
				if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
				{
					return std::nullopt;
				}
				return result;
			}
			catch (const nlohmann::json::exception&)
			{
				return std::nullopt;
			}
		}

		nlohmann::json SerializeVec3(const glm::vec3& value)
		{
			return nlohmann::json::array({ value.x, value.y, value.z });
		}

		struct AuthoredCameraCaptureSnapshot
		{
			TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
			std::string name;
			bool objectActive = false;
			bool cameraComponentActive = false;
			Transform transform;
			CameraComponent camera;
		};

		std::optional<AuthoredCameraCaptureSnapshot> ReadAuthoredCameraCaptureSnapshot(ObjectManager& objectManager, const Object& object)
		{
			if (!objectManager.isObjectValid(object))
			{
				return std::nullopt;
			}

			// MCP camera-object capture reads authored editor state directly.
			// That keeps disabled camera objects available for inspection without
			// activating them or depending on runtime active-membership queries.
			const Transform* transform = objectManager.GetComponent<Transform>(object);
			const CameraComponent* camera = objectManager.GetComponent<CameraComponent>(object);
			if (transform == nullptr || camera == nullptr)
			{
				return std::nullopt;
			}

			AuthoredCameraCaptureSnapshot snapshot;
			snapshot.objectId = objectManager.getUUID(object);
			snapshot.name = objectManager.getObjectName(object);
			snapshot.objectActive = objectManager.isObjectActive(object);
			snapshot.cameraComponentActive = camera->active;
			snapshot.transform = *transform;
			snapshot.camera = *camera;
			return snapshot;
		}

		TerraEngine::Renderer::RenderCaptureRequest BuildCaptureRequestFromCamera(
			TerraEngine::Renderer::RenderCaptureSourceKind sourceKind,
			std::string sourceName,
			const Transform& transform,
			const CameraComponent& camera,
			const glm::uvec2& size,
			bool transparentBackground)
		{
			const glm::quat rotation = glm::normalize(transform.rotation);
			const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
			glm::vec3 up = rotation * camera.up;
			if (glm::length(up) < 0.0001f)
			{
				up = glm::vec3(0.0f, 1.0f, 0.0f);
			}

			TerraEngine::Renderer::RenderCaptureRequest request;
			request.sourceKind = sourceKind;
			request.sourceName = std::move(sourceName);
			request.targetSize = size;
			request.transparentBackground = transparentBackground;
			request.cameraWorldPosition = transform.position;
			request.viewMatrix = glm::lookAt(transform.position, transform.position + forward, glm::normalize(up));
			request.projectionMatrix = glm::perspectiveZO(glm::radians(camera.verticalFovDegrees), camera.aspectRatio, camera.nearPlane, camera.farPlane);
			request.projectionMatrix[1][1] *= -1.0f;
			return request;
		}

		nlohmann::json BuildCaptureSuccessEnvelope(const EditorAutomationService& service, std::string_view requestId, const TerraEngine::Renderer::RenderCaptureResult& capture)
		{
			nlohmann::json source = {
				{"kind", CaptureSourceKindJson(capture.sourceKind)},
				{"description", capture.sourceName}
			};
			nlohmann::json data = {
				{"requestId", capture.requestId.empty() ? std::string(requestId) : capture.requestId},
				{"source", source},
				{"sourceKind", TerraEngine::Renderer::ToString(capture.sourceKind)},
				{"sourceName", capture.sourceName},
				{"mimeType", capture.mimeType},
				{"width", capture.size.x},
				{"height", capture.size.y},
				{"frameId", capture.frameId},
				{"freshness", capture.freshness},
				{"overlaysIncluded", capture.overlaysIncluded},
				{"backendReadback", capture.backendReadback},
				{"deterministicFallback", capture.deterministicFallback},
				{"warnings", capture.warnings}
			};
			nlohmann::json result = service.BuildSuccess(requestId, capture.message, std::move(data));
			result["mcpContent"] = nlohmann::json::array({
				{{"type", "image"}, {"mimeType", capture.mimeType}, {"data", capture.base64Data}}
			});
			return result;
		}

		std::string NormalizeVirtualGeometryStatus(std::string status)
		{
			status = LowerAscii(std::move(status));
			if (status == "needsrebuild")
			{
				return "needsRebuild";
			}
			if (status.empty() || status == "unknown")
			{
				return "unknown";
			}
			return status;
		}

		std::string VirtualGeometryStatusFromMetadata(const VirtualGeometryAssetMetadata& metadata)
		{
			if (!metadata.readable)
			{
				return "unreadable";
			}
			if (metadata.needsRebuild)
			{
				return "needsRebuild";
			}
			if (metadata.hasVirtualGeometry)
			{
				return "cooked";
			}
			return "missing";
		}

		nlohmann::json SerializeVirtualGeometryMetadata(const VirtualGeometryAssetMetadata& metadata)
		{
			return {
				{"readable", metadata.readable},
				{"hasVirtualGeometry", metadata.hasVirtualGeometry},
				{"needsRebuild", metadata.needsRebuild},
				{"virtualGeometryStatus", VirtualGeometryStatusFromMetadata(metadata)},
				{"payloadGeneration", metadata.payloadGeneration},
				{"detailLevelCount", metadata.detailLevelCount},
				{"sourceTriangleCount", metadata.sourceTriangleCount},
				{"meshletCounts", {{"resident", metadata.meshletCount}}},
				{"boundsStatus", metadata.meshletCount > 0u ? "available" : "unknown"},
				{"cookWarnings", metadata.warnings},
				{"rebuildGuidance", metadata.rebuildGuidance}
			};
		}

		nlohmann::json SerializeVirtualGeometrySettings(const VirtualGeometryAdaptiveSettingsMetadata& settings)
		{
			return {
				{"valid", settings.valid},
				{"overridePresent", settings.overridePresent},
				{"settingsSource", settings.settingsSource},
				{"targetRatios", settings.targetRatios},
				{"minimumSourceTriangles", settings.minimumSourceTriangles},
				{"minimumGeneratedTriangles", settings.minimumGeneratedTriangles},
				{"minimumMeshletsPerGeneratedLevel", settings.minimumMeshletsPerGeneratedLevel},
				{"minimumUsefulTriangleReduction", settings.minimumUsefulTriangleReduction},
				{"minimumUsefulMeshletReduction", settings.minimumUsefulMeshletReduction},
				{"maximumGeometricError", settings.maximumGeometricError},
				{"validationErrors", settings.validationErrors}
			};
		}

		std::string BuildVirtualGeometrySettingsSignature(const VirtualGeometryAdaptiveSettingsMetadata& settings)
		{
			// Automation clients need a stable token to compare settings before
			// and after a save. FNV-1a over the normalized JSON keeps the contract
			// deterministic without exposing internal metadata file paths.
			const std::string text = SerializeVirtualGeometrySettings(settings).dump();
			std::uint64_t hash = 1469598103934665603ull;
			for (unsigned char byte : text)
			{
				hash ^= static_cast<std::uint64_t>(byte);
				hash *= 1099511628211ull;
			}
			std::ostringstream stream;
			stream << std::hex << hash;
			return stream.str();
		}

		template <typename ValueType>
		ValueType JsonValueOrAlias(const nlohmann::json& object, const char* primary, const char* alias, ValueType fallback)
		{
			if (object.contains(primary))
			{
				return object.value(primary, fallback);
			}
			return object.value(alias, fallback);
		}

		VirtualGeometryAdaptiveSettingsMetadata ParseVirtualGeometrySettingsOverride(
			const nlohmann::json& overrideObject,
			const VirtualGeometryAdaptiveSettingsMetadata& defaults)
		{
			VirtualGeometryAdaptiveSettingsMetadata settings = defaults;
			settings.overridePresent = true;
			settings.settingsSource = "PerMeshOverride";
			if (overrideObject.contains("targetRatios") && overrideObject["targetRatios"].is_array())
			{
				settings.targetRatios.clear();
				for (const nlohmann::json& ratio : overrideObject["targetRatios"])
				{
					if (ratio.is_number())
					{
						settings.targetRatios.push_back(ratio.get<float>());
					}
				}
			}
			settings.minimumSourceTriangles = JsonValueOrAlias<std::uint32_t>(overrideObject, "minimumSourceTriangles", "minSourceTrianglesForMultiLod", settings.minimumSourceTriangles);
			settings.minimumGeneratedTriangles = JsonValueOrAlias<std::uint32_t>(overrideObject, "minimumGeneratedTriangles", "minFinalTriangleCount", settings.minimumGeneratedTriangles);
			settings.minimumMeshletsPerGeneratedLevel = JsonValueOrAlias<std::uint32_t>(overrideObject, "minimumMeshletsPerGeneratedLevel", "minSourceMeshletsForMultiLod", settings.minimumMeshletsPerGeneratedLevel);
			settings.minimumUsefulTriangleReduction = JsonValueOrAlias<float>(overrideObject, "minimumUsefulTriangleReduction", "minTriangleReductionPerLevel", settings.minimumUsefulTriangleReduction);
			settings.minimumUsefulMeshletReduction = JsonValueOrAlias<float>(overrideObject, "minimumUsefulMeshletReduction", "minMeshletReductionPerLevel", settings.minimumUsefulMeshletReduction);
			settings.maximumGeometricError = JsonValueOrAlias<float>(overrideObject, "maximumGeometricError", "geometricErrorTolerance", settings.maximumGeometricError);
			return settings;
		}

		nlohmann::json SerializeVirtualGeometryCookDiagnostics(const std::vector<VirtualGeometryDetailDiagnosticMetadata>& diagnostics)
		{
			nlohmann::json targets = nlohmann::json::array();
			std::uint32_t generatedCount = 0u;
			std::uint32_t skippedCount = 0u;
			std::uint32_t mergedCount = 0u;
			std::uint32_t rejectedCount = 0u;
			for (const VirtualGeometryDetailDiagnosticMetadata& diagnostic : diagnostics)
			{
				generatedCount += diagnostic.status == "Generated" ? 1u : 0u;
				skippedCount += diagnostic.status == "Skipped" ? 1u : 0u;
				mergedCount += diagnostic.status == "Merged" ? 1u : 0u;
				rejectedCount += diagnostic.status == "Rejected" ? 1u : 0u;
				targets.push_back({
					{"targetIndex", diagnostic.targetIndex},
					{"requestedRatio", diagnostic.requestedRatio},
					{"status", diagnostic.status},
					{"reason", diagnostic.reason},
					{"desiredTriangleCount", diagnostic.desiredTriangleCount},
					{"desiredMeshletCount", diagnostic.desiredMeshletCount},
					{"actualTriangleCount", diagnostic.actualTriangleCount},
					{"actualMeshletCount", diagnostic.actualMeshletCount},
					{"nearestGeneratedLevelIndex", diagnostic.nearestGeneratedLevelIndex},
					{"triangleCount", diagnostic.triangleCount},
					{"meshletCount", diagnostic.meshletCount}
				});
			}

			return {
				{"requestedTargetCount", static_cast<std::uint32_t>(targets.size())},
				{"generatedLevelCount", generatedCount},
				{"skippedDetailCount", skippedCount},
				{"mergedTargetCount", mergedCount},
				{"rejectedTargetCount", rejectedCount},
				{"requestedTargets", targets}
			};
		}

		bool PackageVirtualGeometryStatusIsUsable(std::string_view status)
		{
			return status == "Cooked" || status == "Skipped" || status == "cooked" || status == "skipped";
		}

		nlohmann::json SerializePackageGeneratedMeshForAutomation(const nlohmann::json& mesh, bool packageSucceeded)
		{
			const nlohmann::json virtualGeometry = mesh.value("virtualGeometry", nlohmann::json::object());
			const std::string rawStatus = virtualGeometry.value("status", "Unknown");
			const nlohmann::json targets = virtualGeometry.value("targets", nlohmann::json::array());
			const bool usable = packageSucceeded && PackageVirtualGeometryStatusIsUsable(rawStatus);
			nlohmann::json errors = nlohmann::json::array();
			if (!usable && rawStatus != "Unknown")
			{
				errors.push_back("Generated mesh is not marked usable for virtual geometry automation because its package import or metadata validation failed.");
			}

			return {
				{"guid", mesh.value("guid", std::string())},
				{"sourcePath", mesh.value("sourcePath", std::string())},
				{"compiledPath", mesh.value("compiledPath", std::string())},
				{"displayName", mesh.value("displayName", std::string())},
				{"usable", usable},
				{"virtualGeometryStatus", NormalizeVirtualGeometryStatus(rawStatus)},
				{"detailLevelCount", virtualGeometry.value("detailLevels", 0u)},
				{"sourceTriangleCount", virtualGeometry.value("sourceTriangles", 0u)},
				{"selectedFallbackAvailable", usable || virtualGeometry.value("meshlets", 0u) > 0u},
				{"settings", virtualGeometry.value("settings", nlohmann::json::object())},
				{"cookDiagnostics", {
					{"requestedTargetCount", static_cast<std::uint32_t>(targets.size())},
					{"requestedTargets", targets}
				}},
				{"warnings", virtualGeometry.value("warnings", nlohmann::json::array())},
				{"errors", errors}
			};
		}

		nlohmann::json BuildPackageVirtualGeometryAutomationSummary(const nlohmann::json& importResult)
		{
			const bool packageSucceeded = importResult.value("status", std::string()) != "Failed";
			nlohmann::json generatedMeshes = nlohmann::json::array();
			std::uint32_t usableCount = 0u;
			std::uint32_t failedCount = 0u;
			for (const nlohmann::json& mesh : importResult.value("generatedMeshes", nlohmann::json::array()))
			{
				nlohmann::json normalized = SerializePackageGeneratedMeshForAutomation(mesh, packageSucceeded);
				usableCount += normalized.value("usable", false) ? 1u : 0u;
				failedCount += normalized.value("usable", false) ? 0u : 1u;
				generatedMeshes.push_back(std::move(normalized));
			}

			return {
				{"generatedMeshes", generatedMeshes},
				{"usableGeneratedMeshCount", usableCount},
				{"failedGeneratedMeshCount", failedCount},
				{"assetRegistrationBoundary", "asset-database-finish-import"}
			};
		}

		std::filesystem::path NormalizeAutomationAssetRelativePath(const AssetDatabase& database, const std::filesystem::path& input)
		{
			if (input.is_absolute())
			{
				std::error_code ec;
				return std::filesystem::relative(input, database.GetAssetRoot(), ec).lexically_normal();
			}

			std::filesystem::path relative = input.lexically_normal();
			auto begin = relative.begin();
			if (begin != relative.end())
			{
				std::string first = begin->string();
				std::transform(first.begin(), first.end(), first.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (first == "assets")
				{
					std::filesystem::path stripped;
					for (++begin; begin != relative.end(); ++begin)
					{
						stripped /= *begin;
					}
					relative = stripped;
				}
			}
			return relative.lexically_normal();
		}

		const AssetMetadata* ResolveAutomationAssetMetadata(AssetDatabase& database, const std::string& target, std::filesystem::path& outAbsolutePath)
		{
			if (const auto parsedUuid = TerraEngine::ParseUUID(target))
			{
				if (const AssetMetadata* metadata = database.FindAssetByGuid(*parsedUuid))
				{
					outAbsolutePath = database.GetAssetRoot() / metadata->sourcePath;
					return metadata;
				}
			}

			const std::filesystem::path relative = NormalizeAutomationAssetRelativePath(database, target);
			outAbsolutePath = std::filesystem::absolute(database.GetAssetRoot() / relative);
			if (const auto it = database.GetAssets().find(outAbsolutePath); it != database.GetAssets().end())
			{
				return &it->second;
			}
			return nullptr;
		}

		std::optional<RenderViewType> ParseAutomationRenderViewType(const nlohmann::json& value)
		{
			if (!value.is_string())
			{
				return std::nullopt;
			}
			const std::string view = LowerAscii(value.get<std::string>());
			if (view == "game") return RenderViewType::Game;
			if (view == "scene") return RenderViewType::Scene;
			if (view == "shadow") return RenderViewType::Shadow;
			if (view == "exportruntime" || view == "export_runtime" || view == "export runtime") return RenderViewType::ExportRuntime;
			return std::nullopt;
		}

		nlohmann::json SerializeVirtualGeometryRuntimeView(const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& view)
		{
			return {
				{"viewType", ToString(view.viewType)},
				{"frameId", view.frameId},
				{"active", view.active},
				{"inactiveReason", ToString(view.inactiveReason)},
				{"sourceMeshlets", view.sourceMeshlets},
				{"selectedMeshlets", view.selectedMeshlets},
				{"dispatchedMeshlets", view.dispatchedMeshlets},
				{"culledMeshlets", view.culledMeshlets},
				{"hiZActive", view.hiZActive},
				{"hiZHistoryValid", view.hiZHistoryValid},
				{"residentPages", view.residentPages},
				{"requestedPages", view.requestedPages},
				{"missingPages", view.missingPages},
				{"failedPages", view.failedPages},
				{"evictedPages", view.evictedPages},
				{"selectedClusters", view.clusterSelection.selectedClusters},
				{"fallbackSelections", view.clusterSelection.fallbackClusters},
				{"geometryBudgetBytes", view.geometryBudgetBytes},
				{"budgetPressureBytes", view.budgetPressureBytes},
				{"warnings", view.warnings}
			};
		}

		nlohmann::json SerializeVirtualGeometryClusterSummary(const VirtualGeometryClusterSummaryMetadata& summary)
		{
			return {
				{"clusterCount", summary.clusterCount},
				{"selectedClusterCount", summary.selectedClusterCount},
				{"fallbackClusterCount", summary.fallbackClusterCount},
				{"fallbackCoverageValid", summary.fallbackCoverageValid}
			};
		}

		nlohmann::json SerializeVirtualGeometryPageSummary(const VirtualGeometryPageSummaryMetadata& summary)
		{
			return {
				{"pageCount", summary.pageCount},
				{"residentPageCount", summary.residentPageCount},
				{"pinnedFallbackPageCount", summary.pinnedFallbackPageCount},
				{"estimatedBytes", summary.estimatedBytes}
			};
		}

		nlohmann::json SerializeExportDiagnostic(const ExportDiagnostic& diagnostic)
		{
			const char* severity = "info";
			if (diagnostic.severity == ExportDiagnosticSeverity::Warning) severity = "warning";
			if (diagnostic.severity == ExportDiagnosticSeverity::Error) severity = "error";
			return {
				{"severity", severity},
				{"category", diagnostic.category},
				{"message", diagnostic.message},
				{"filePath", diagnostic.filePath},
				{"line", diagnostic.line}
			};
		}

		nlohmann::json SerializeExportValidation(const ExportValidationResult& result)
		{
			nlohmann::json diagnostics = nlohmann::json::array();
			for (const ExportDiagnostic& diagnostic : result.diagnostics)
			{
				diagnostics.push_back(SerializeExportDiagnostic(diagnostic));
			}
			return {
				{"canExport", result.passed},
				{"errorCount", result.errorCount},
				{"warningCount", result.warningCount},
				{"issues", diagnostics},
				{"materialFallbacks", result.materialFallbackDiagnostics}
			};
		}

		std::optional<BuildProfile> FindBuildProfile(const std::filesystem::path& projectRoot, const std::string& selector)
		{
			const std::filesystem::path profileDirectory = projectRoot / "BuildProfiles";
			for (BuildProfile profile : LoadAllBuildProfiles(profileDirectory))
			{
				if (profile.name == selector || profile.id == selector || profile.filePath.filename().string() == selector)
				{
					return profile;
				}
			}
			const std::filesystem::path directPath = profileDirectory / selector;
			if (std::filesystem::exists(directPath))
			{
				BuildProfile profile = LoadBuildProfile(directPath);
				if (!profile.name.empty())
				{
					return profile;
				}
			}
			return std::nullopt;
		}

		nlohmann::json SerializeHardBoundary(const TerraEngine::Scheduling::HardBoundary& boundary)
		{
			return {
				{"name", boundary.boundaryName},
				{"classification", TerraEngine::Scheduling::ToString(boundary.currentClassification)},
				{"riskFactors", boundary.riskFactors},
				{"safeSubwork", boundary.safeSubwork},
				{"protectedCommit", boundary.protectedCommit},
				{"requiredEvidence", boundary.requiredEvidence}
			};
		}

		nlohmann::json SerializeArchitectureMigrationSummary(const TerraEngine::Scheduling::ArchitectureMigrationSummary& summary)
		{
			return {
				{"workerSafeCount", summary.workerSafeCount},
				{"protectedCount", summary.protectedCount},
				{"inlineCount", summary.inlineCount},
				{"rejectedCount", summary.rejectedCount},
				{"fallbackCount", summary.fallbackCount},
				{"mutationFlushCount", summary.mutationFlushCount},
				{"workerPhaseCount", summary.workerPhaseCount},
				{"protectedCommitCount", summary.protectedCommitCount},
				{"barrierCount", summary.barrierCount},
				{"hardBoundaryCount", summary.hardBoundaryCount},
				{"partiallyWorkerSafeBoundaryCount", summary.partiallyWorkerSafeBoundaryCount},
				{"protectedBaselineTimeNs", summary.protectedBaselineTimeNs},
				{"workerPreparationTimeNs", summary.workerPreparationTimeNs},
				{"protectedCommitTimeNs", summary.protectedCommitTimeNs},
				{"coordinatorWaitTimeNs", summary.coordinatorWaitTimeNs},
				{"workerUtilization", summary.workerUtilization},
				{"activeMembershipInvalidationCount", summary.activeMembershipInvalidationCount},
				{"staleObjectHandleCount", summary.staleObjectHandleCount},
				{"hierarchySnapshotRebuildCount", summary.hierarchySnapshotRebuildCount},
				{"componentAccessGrantDeniedCount", summary.componentAccessGrantDeniedCount},
				{"fallbackReasons", summary.fallbackReasons},
				{"workerCategories", summary.workerCategories},
				{"protectedCategories", summary.protectedCategories},
				{"rejectedCategories", summary.rejectedCategories},
				{"hardBoundaryFindings", summary.hardBoundaryFindings},
				{"formatted", summary.formatted}
			};
		}

		nlohmann::json SerializeWorkerCoverage(const TerraEngine::Scheduling::WorkerCoverageReport& report)
		{
			return {
				{"frameId", report.frameId},
				{"workerCategories", report.workerCategories},
				{"protectedCategories", report.protectedCategories},
				{"rejectedCategories", report.rejectedCategories},
				{"fallbackCategories", report.fallbackCategories},
				{"hardBoundaryFindings", report.hardBoundaryFindings},
				{"barrierCount", report.barrierCount},
				{"protectedTimeNs", report.protectedTimeNs},
				{"workerTimeNs", report.workerTimeNs},
				{"formattedSummary", report.formattedSummary}
			};
		}

		nlohmann::json SerializeParallelFrameDiagnostics(const ::Engine* engine)
		{
			if (engine == nullptr)
			{
				return {{"available", false}};
			}

			const TerraEngine::Scheduling::FrameExecutionPlan& plan = engine->GetParallelFrameExecutionPlan();
			const TerraEngine::Scene::LayoutDiagnosticsSummary& layout = engine->GetRuntimeLayoutDiagnostics();
			nlohmann::json diagnostics = nlohmann::json::array();
			for (const TerraEngine::Scheduling::ArchitectureDiagnostic& diagnostic : plan.diagnostics)
			{
				diagnostics.push_back({
					{"severity", TerraEngine::Scheduling::ToString(diagnostic.severity)},
					{"code", TerraEngine::Scheduling::ToString(diagnostic.code)},
					{"workloadName", diagnostic.workloadName},
					{"resourceName", diagnostic.resourceName},
					{"message", diagnostic.message},
					{"recommendedAction", diagnostic.recommendedAction}
				});
			}

			return {
				{"available", true},
				{"frameId", plan.frameId},
				{"executionMode", TerraEngine::Scheduling::ToString(plan.executionMode)},
				{"parallelExecutionUsed", plan.parallelExecutionUsed},
				{"valid", plan.valid},
				{"completed", plan.completed},
				{"workloadCount", plan.workloads.size()},
				{"resultCount", plan.results.size()},
				{"diagnostics", diagnostics},
				{"migrationSummary", SerializeArchitectureMigrationSummary(engine->GetArchitectureMigrationSummary())},
				{"workerCoverage", SerializeWorkerCoverage(engine->GetWorkerCoverageReport())},
				{"runtimeLayout", nlohmann::json{
					{"frameId", layout.frameId},
					{"layoutReused", layout.layoutReused},
					{"layoutRebuildCount", layout.layoutRebuildCount},
					{"rebuildReasons", layout.rebuildReasons},
					{"activeObjectCount", layout.activeObjectCount},
					{"totalObjectCount", layout.totalObjectCount},
					{"transformRecordCount", layout.transformRecordCount},
					{"renderRecordCount", layout.renderRecordCount},
					{"fallbackCount", layout.fallbackCount},
					{"metadataScanFallbackCount", layout.metadataScanFallbackCount},
					{"fullObjectFacadeFallbackCount", layout.fullObjectFacadeFallbackCount},
					{"objectHandleFallbackCount", layout.objectHandleFallbackCount},
					{"staleObjectHandleCount", layout.staleObjectHandleCount},
					{"validationMismatchCount", layout.validationMismatchCount},
					{"smallSceneGuardTriggered", layout.smallSceneGuardTriggered}
				}}
			};
		}

		nlohmann::json SerializeMutationDiagnostics()
		{
			const ScriptingSystem::ScriptingFrameResult& scripting = ScriptingSystem::Instance().GetLatestScriptingFrameResult();
			return {
				{"pendingMutationCount", TerraEngine::Scene::SceneMutationQueue::Instance().Size()},
				{"acceptedMutationCount", scripting.acceptedMutations},
				{"rejectedMutationCount", scripting.rejectedMutations},
				{"rollbackCount", 0},
				{"rejectedBatchesPreventPartialState", true},
				{"typedMutationCount", scripting.typedMutations},
				{"jsonMutationFallbackCount", scripting.jsonMutationFallbacks},
				{"coalescedMutationCount", scripting.coalescedMutations},
				{"deferredMutationCount", scripting.accessReport.deferredMutations},
				{"accessRejectedMutationCount", scripting.accessReport.rejectedMutations}
			};
		}
	}

	const char* ToString(BridgeState value)
	{
		switch (value)
		{
			case BridgeState::Disabled: return "disabled";
			case BridgeState::Listening: return "listening";
			case BridgeState::Connected: return "connected";
			case BridgeState::Stopping: return "stopping";
			case BridgeState::Stopped: return "stopped";
		}
		return "unknown";
	}

	const char* ToString(WriteMode value)
	{
		return value == WriteMode::WritesEnabled ? "writes_enabled" : "read_only";
	}

	const char* ToString(RequestStatus value)
	{
		switch (value)
		{
			case RequestStatus::Queued: return "queued";
			case RequestStatus::Running: return "running";
			case RequestStatus::WaitingApproval: return "waiting_approval";
			case RequestStatus::Completed: return "completed";
			case RequestStatus::Failed: return "failed";
			case RequestStatus::Rejected: return "rejected";
			case RequestStatus::Canceled: return "canceled";
		}
		return "unknown";
	}

	const char* ToString(MutationMode value)
	{
		switch (value)
		{
			case MutationMode::ReadOnly: return "read_only";
			case MutationMode::EditorSelection: return "editor_selection";
			case MutationMode::SceneMutation: return "scene_mutation";
			case MutationMode::AssetMutation: return "asset_mutation";
			case MutationMode::ExportMutation: return "export_mutation";
			case MutationMode::Destructive: return "destructive";
		}
		return "unknown";
	}

	const char* ToString(ErrorCategory value)
	{
		switch (value)
		{
			case ErrorCategory::Validation: return "validation";
			case ErrorCategory::Permission: return "permission";
			case ErrorCategory::MissingTarget: return "missing_target";
			case ErrorCategory::InvalidCameraObject: return "invalid_camera_object";
			case ErrorCategory::UnsupportedCapability: return "unsupported_capability";
			case ErrorCategory::UnsupportedReadback: return "unsupported_readback";
			case ErrorCategory::QueueFull: return "queue_full";
			case ErrorCategory::OverBudget: return "over_budget";
			case ErrorCategory::Conflict: return "conflict";
			case ErrorCategory::Transport: return "transport";
			case ErrorCategory::Internal: return "internal";
			case ErrorCategory::Canceled: return "canceled";
		}
		return "internal";
	}

	const char* ToString(ActivityEventType value)
	{
		switch (value)
		{
			case ActivityEventType::Connection: return "connection";
			case ActivityEventType::Rejection: return "rejection";
			case ActivityEventType::Configuration: return "configuration";
			case ActivityEventType::Request: return "request";
			case ActivityEventType::Approval: return "approval";
			case ActivityEventType::Mutation: return "mutation";
			case ActivityEventType::Diagnostic: return "diagnostic";
			case ActivityEventType::Capture: return "capture";
			case ActivityEventType::Export: return "export";
		}
		return "request";
	}

	std::optional<WriteMode> ParseWriteMode(std::string_view value)
	{
		if (value == "read_only")
		{
			return WriteMode::ReadOnly;
		}
		if (value == "writes_enabled")
		{
			return WriteMode::WritesEnabled;
		}
		return std::nullopt;
	}

	EditorAutomationService::EditorAutomationService()
	{
		RegisterDefaultCapabilities();
	}

	void EditorAutomationService::SetEditorContext(EditorContext* context)
	{
		editorContext = context;
	}

	void EditorAutomationService::SetSelectionCallbacks(std::function<std::optional<entt::entity>()> getter, std::function<void(std::optional<entt::entity>)> setter)
	{
		getSelectedEntity = std::move(getter);
		setSelectedEntity = std::move(setter);
	}

	void EditorAutomationService::SetBridgeToolCallback(BridgeToolCallback callback)
	{
		bridgeToolCallback = std::move(callback);
	}

	void EditorAutomationService::SetActivityCallback(ActivityCallback callback)
	{
		activityCallback = std::move(callback);
	}

	bool EditorAutomationService::RegisterCapability(const AssistantCapability& capability)
	{
		if (capability.name.empty() || capabilityIndexByName.find(capability.name) != capabilityIndexByName.end())
		{
			return false;
		}
		capabilityIndexByName.emplace(capability.name, capabilities.size());
		capabilities.push_back(capability);
		return true;
	}

	void EditorAutomationService::RegisterDefaultCapabilities()
	{
		if (!capabilities.empty())
		{
			return;
		}

		const auto add = [this](std::string name, std::string category, MutationMode mode, bool requiresProject, bool requiresScene, bool requiresApproval, std::string inputSummary) {
			AssistantCapability capability;
			capability.name = std::move(name);
			capability.category = std::move(category);
			capability.mutationMode = mode;
			capability.mutatesState = IsWriteMutation(mode);
			capability.requiresProject = requiresProject;
			capability.requiresScene = requiresScene;
			capability.requiresApproval = requiresApproval;
			capability.inputSummary = std::move(inputSummary);
			RegisterCapability(capability);
		};

		// Bridge enablement and write scope are editor-owned controls. MCP
		// clients may inspect status and activity, but they cannot grant or
		// revoke their own write permissions.
		add("activity.list", "activity", MutationMode::ReadOnly, false, false, false, "limit?: integer, filter?: string");
		add("scene.open", "scene", MutationMode::SceneMutation, true, false, true, "path: project-relative scene path");
		add("scene.save", "scene", MutationMode::SceneMutation, true, true, false, "path?: project-relative scene path");
		add("scene.list_objects", "scene", MutationMode::ReadOnly, false, false, false, "includeComponents?: bool, search?: string");
		add("object.create", "object", MutationMode::SceneMutation, false, false, false, "name: string, parent?: target, select?: bool");
		add("object.delete", "object", MutationMode::Destructive, false, true, true, "object: uuid|entity|name");
		add("object.select", "object", MutationMode::EditorSelection, false, true, false, "object: uuid|entity|name");
		add("object.set_metadata", "object", MutationMode::SceneMutation, false, true, false, "object plus name/tag/layer/active/static fields");
		add("object.set_parent", "object", MutationMode::SceneMutation, false, true, false, "object: target, parent?: target|null");
		add("component.add", "component", MutationMode::SceneMutation, false, true, false, "object: target, component: string");
		add("component.remove", "component", MutationMode::SceneMutation, false, true, false, "object: target, component: string");
		add("component.get", "component", MutationMode::ReadOnly, false, true, false, "object: target, component: string");
		add("component.set_property", "component", MutationMode::SceneMutation, false, true, false, "object/component/property/value");
		add("asset.list", "asset", MutationMode::ReadOnly, true, false, false, "type?: string, search?: string");
		add("asset.import", "asset", MutationMode::AssetMutation, true, false, false, "path: project-relative asset path, force?: bool; model packages such as glTF/FBX return generated asset ids and diagnostics");
		add("asset.virtualGeometry.settings.get", "asset", MutationMode::ReadOnly, true, false, false, "asset?: asset path|uuid, includeResolved?: bool");
		add("asset.virtualGeometry.settings.set", "asset", MutationMode::AssetMutation, true, false, false, "asset: asset path|uuid, override: sparse settings, validateOnly?: bool, rebuildAfterSave?: bool");
		add("asset.virtualGeometry.settings.reset", "asset", MutationMode::AssetMutation, true, false, false, "asset: asset path|uuid, rebuildAfterReset?: bool");
		add("asset.virtualGeometry.rebuild", "asset", MutationMode::AssetMutation, true, false, false, "target: asset path|uuid|all, force?: bool, includePackageOutputs?: bool, diagnosticsLevel?: summary|detailed|debug");
		add("asset.virtualGeometry.inspect", "asset", MutationMode::ReadOnly, true, false, false, "asset: asset path|uuid, includeRuntimeState?: bool, viewType?: Game|Scene|Shadow|ExportRuntime");
		add("asset.prepare_delete", "asset", MutationMode::Destructive, true, false, true, "path: project-relative asset path");
		add("export.validate", "export", MutationMode::ReadOnly, true, false, false, "profile: string");
		add("export.start", "export", MutationMode::ExportMutation, true, false, true, "profile: string, confirmOutput?: string");
		add("export.cancel", "export", MutationMode::ExportMutation, true, false, false, "jobId: string");
		add("game_view.capture", "capture", MutationMode::ReadOnly, false, false, false, "width?: integer, height?: integer, format?: image/png, freshness?: latestCompleted|nextFrame, includeOverlays?: bool, transparentBackground?: bool");
		add("camera.capture_object", "capture", MutationMode::ReadOnly, false, true, false, "object|camera|target: uuid|entity|name, includeDisabled?: bool, width?: integer, height?: integer, format?: image/png");
		add("camera.capture_custom", "capture", MutationMode::ReadOnly, false, false, false, "position: [x,y,z], target?: [x,y,z], forward?: [x,y,z], up?: [x,y,z], projection?: perspective|orthographic, width?: integer, height?: integer");
		add("diagnostics.get_summary", "diagnostics", MutationMode::ReadOnly, false, false, false, "includeDetails?: bool, surface?: string");
	}

	const std::vector<AssistantCapability>& EditorAutomationService::GetCapabilities() const
	{
		return capabilities;
	}

	const AssistantCapability* EditorAutomationService::FindCapability(std::string_view name) const
	{
		const auto it = capabilityIndexByName.find(std::string(name));
		return it == capabilityIndexByName.end() ? nullptr : &capabilities[it->second];
	}

	nlohmann::json EditorAutomationService::BuildSuccess(std::string_view requestId, std::string_view message, nlohmann::json data) const
	{
		nlohmann::json result = StatusEnvelope(true, requestId, RequestStatus::Completed, message);
		result["data"] = std::move(data);
		result["error"] = nullptr;
		return result;
	}

	nlohmann::json EditorAutomationService::BuildError(std::string_view requestId, ErrorCategory category, std::string_view message, std::string_view target) const
	{
		nlohmann::json result = StatusEnvelope(false, requestId, category == ErrorCategory::Permission ? RequestStatus::Rejected : RequestStatus::Failed, message);
		result["data"] = nlohmann::json::object();
		result["error"] = {
			{"category", ToString(category)},
			{"details", message}
		};
		if (!target.empty())
		{
			result["error"]["target"] = target;
		}
		return result;
	}

	nlohmann::json EditorAutomationService::HandleTool(std::string_view name, const nlohmann::json& arguments, WriteMode writeMode, const std::string& requestId)
	{
		const AssistantCapability* capability = FindCapability(name);
		if (capability == nullptr)
		{
			return BuildError(requestId, ErrorCategory::UnsupportedCapability, "Unknown editor automation capability.");
		}
		const std::string toolName(name);
		if (capability->mutatesState && writeMode != WriteMode::WritesEnabled)
		{
			return BuildError(requestId, ErrorCategory::Permission, "The bridge is in read-only mode.");
		}
		if (!arguments.is_object())
		{
			return BuildError(requestId, ErrorCategory::Validation, "Tool arguments must be a JSON object.");
		}
		if (capability->requiresProject && (editorContext == nullptr || editorContext->projectManager == nullptr || !editorContext->projectManager->HasActiveProject()))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Open a project before running this capability.");
		}

		if (toolName == "activity.list" && bridgeToolCallback)
		{
			return bridgeToolCallback(nlohmann::json{{"name", toolName}, {"arguments", arguments}, {"requestId", requestId}});
		}
		if (toolName == "scene.list_objects") return HandleSceneListObjects(arguments, requestId);
		if (toolName == "scene.open")
		{
			nlohmann::json validation = ValidateProjectRelativePath(arguments, "path", requestId);
			if (!validation.value("ok", false)) return validation;
			const std::filesystem::path relativeScenePath = validation["path"].get<std::string>();
			const TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene(relativeScenePath);
			if (sceneId == TerraEngine::EMPTY_UUID || !SceneManager::Instance().SetActiveScene(sceneId))
			{
				return BuildError(requestId, ErrorCategory::MissingTarget, "Scene could not be loaded.", relativeScenePath.generic_string());
			}
			UndoRedoSystem::Instance().Clear();
			return BuildSuccess(requestId, "Opened scene.", {{"path", relativeScenePath.generic_string()}});
		}
		if (toolName == "scene.save") return HandleSceneSave(arguments, requestId);
		if (toolName == "object.create") return HandleObjectCreate(arguments, requestId);
		if (toolName == "object.delete") return HandleObjectDelete(arguments, requestId);
		if (toolName == "object.select") return HandleObjectSelect(arguments, requestId);
		if (toolName == "object.set_metadata") return HandleObjectSetMetadata(arguments, requestId);
		if (toolName == "object.set_parent") return HandleObjectSetParent(arguments, requestId);
		if (toolName == "component.add") return HandleComponentAdd(arguments, requestId);
		if (toolName == "component.remove") return HandleComponentRemove(arguments, requestId);
		if (toolName == "component.get") return HandleComponentGet(arguments, requestId);
		if (toolName == "component.set_property") return HandleComponentSetProperty(arguments, requestId);
		if (toolName == "asset.list") return HandleAssetList(arguments, requestId);
		if (toolName == "asset.import") return HandleAssetImport(arguments, requestId);
		if (toolName == "asset.virtualGeometry.settings.get") return HandleVirtualGeometrySettingsGet(arguments, requestId);
		if (toolName == "asset.virtualGeometry.settings.set") return HandleVirtualGeometrySettingsSet(arguments, requestId);
		if (toolName == "asset.virtualGeometry.settings.reset") return HandleVirtualGeometrySettingsReset(arguments, requestId);
		if (toolName == "asset.virtualGeometry.rebuild") return HandleVirtualGeometryRebuild(arguments, requestId);
		if (toolName == "asset.virtualGeometry.inspect") return HandleVirtualGeometryInspect(arguments, requestId);
		if (toolName == "asset.prepare_delete") return HandleAssetPrepareDelete(arguments, requestId);
		if (toolName == "export.validate") return HandleExportValidate(arguments, requestId);
		if (toolName == "export.start") return HandleExportStart(arguments, requestId);
		if (toolName == "export.cancel") return HandleExportCancel(arguments, requestId);
		if (toolName == "game_view.capture") return HandleGameViewCapture(arguments, requestId);
		if (toolName == "camera.capture_object") return HandleCameraObjectCapture(arguments, requestId);
		if (toolName == "camera.capture_custom") return HandleCustomCameraCapture(arguments, requestId);
		if (toolName == "diagnostics.get_summary") return HandleDiagnosticsGetSummary(arguments, requestId);
		return BuildError(requestId, ErrorCategory::UnsupportedCapability, "Capability metadata exists but no handler is registered.");
	}

	nlohmann::json EditorAutomationService::BuildCapabilityList() const
	{
		nlohmann::json items = nlohmann::json::array();
		for (const AssistantCapability& capability : capabilities)
		{
			items.push_back({
				{"name", capability.name},
				{"kind", capability.kind},
				{"category", capability.category},
				{"mutationMode", ToString(capability.mutationMode)},
				{"mutatesState", capability.mutatesState},
				{"requiresProject", capability.requiresProject},
				{"requiresScene", capability.requiresScene},
				{"requiresApproval", capability.requiresApproval},
				{"inputSummary", capability.inputSummary}
			});
		}
		return {{"capabilities", items}, {"count", items.size()}};
	}

	nlohmann::json EditorAutomationService::BuildProjectState() const
	{
		nlohmann::json data;
		const bool hasProject = editorContext != nullptr && editorContext->projectManager != nullptr && editorContext->projectManager->HasActiveProject();
		data["hasProject"] = hasProject;
		data["projectRoot"] = hasProject ? editorContext->projectManager->GetProjectRoot().string() : "";
		const auto* scene = SceneManager::Instance().GetActiveScene();
		data["activeScene"] = scene != nullptr ? scene->name : "";
		data["sceneDirty"] = SceneManager::Instance().IsActiveSceneDirty();
		data["playMode"] = ScriptingSystem::Instance().IsRunning();
		data["selectedObject"] = nullptr;
		if (getSelectedEntity)
		{
			std::optional<entt::entity> selectedEntity = getSelectedEntity();
			if (selectedEntity && ObjectManager::instance().getObject(*selectedEntity))
			{
				data["selectedObject"] = SerializeObjectSummary(*selectedEntity, false);
			}
		}
		return data;
	}

	nlohmann::json EditorAutomationService::BuildSceneSummary() const
	{
		nlohmann::json data;
		const auto* scene = SceneManager::Instance().GetActiveScene();
		data["activeScene"] = scene != nullptr ? scene->name : "";
		data["dirty"] = SceneManager::Instance().IsActiveSceneDirty();
		data["roots"] = nlohmann::json::array();
		std::vector<Object> roots = ObjectManager::instance().getRootObjects();
		ObjectManager::instance().SortObjectsByStableId(roots);
		std::size_t objectCount = 0;
		for (Object object : roots)
		{
			data["roots"].push_back(SerializeObjectSummary(object.EntityId(), true));
			++objectCount;
		}
		data["objectCount"] = objectCount;
		return data;
	}

	nlohmann::json EditorAutomationService::BuildDiagnosticsSummary(bool includeDetails) const
	{
		nlohmann::json exportJobJson = nlohmann::json::array();
		for (const ExportJob& job : exportJobs)
		{
			exportJobJson.push_back({
				{"jobId", job.jobId},
				{"profile", job.profile},
				{"state", job.state},
				{"progress", job.progress},
				{"cancelRequested", job.cancelRequested}
			});
		}
		const std::size_t assetJobCount = editorContext != nullptr && editorContext->assetDatabase != nullptr
			? editorContext->assetDatabase->GetCompileJobsCount()
			: 0u;
		nlohmann::json data;
		nlohmann::json boundaryFindings = nlohmann::json::array();
		for (const TerraEngine::Scheduling::HardBoundary& boundary : TerraEngine::Scheduling::InitialWorkerHardBoundaries())
		{
			boundaryFindings.push_back(SerializeHardBoundary(boundary));
		}
		const ::Engine* engine = editorContext != nullptr ? editorContext->engine : nullptr;
		const nlohmann::json parallelFrame = SerializeParallelFrameDiagnostics(engine);
		const nlohmann::json mutationDiagnostics = SerializeMutationDiagnostics();
		const TerraEngine::Renderer::RenderDiagnosticsSummary renderDiagnostics = RenderSystem::Instance().GetLatestRenderGraphDiagnostics();
		nlohmann::json captureSchedulers = nlohmann::json::array();
		for (const TerraEngine::Renderer::RenderCaptureSchedulerDiagnostics& capture : renderDiagnostics.captureSchedulers)
		{
			captureSchedulers.push_back({
				{"owningSystem", capture.owningSystem},
				{"schedulerPhase", capture.schedulerPhase},
				{"sourceKind", capture.sourceKind},
				{"reads", capture.readResources},
				{"writes", capture.writeResources},
				{"workerSafePreparation", capture.workerSafePreparation},
				{"protectedBoundary", capture.protectedBoundary},
				{"visibilityBoundary", capture.visibilityBoundary},
				{"backendReadback", capture.backendReadback},
				{"deterministicFallback", capture.deterministicFallback}
			});
		}
		const auto& runtimeMembership = ObjectManager::instance().GetRuntimeActiveMembershipSnapshot();
		std::size_t activeRuntimeObjects = 0u;
		for (const TerraEngine::Scene::RuntimeActiveMembership& membership : runtimeMembership)
		{
			if (membership.objectActive)
			{
				++activeRuntimeObjects;
			}
		}
		data["surfaces"] = nlohmann::json::array({
			{{"surface", "render"}, {"state", "available"}, {"details", includeDetails ? nlohmann::json{{"note", "Render diagnostics are available through the profiler/resource windows."}} : nlohmann::json::object()}},
			{{"surface", "runtimeLayout"}, {"state", "available"}, {"details", nlohmann::json{{"authoredObjectCount", runtimeMembership.size()}, {"activeRuntimeObjectCount", activeRuntimeObjects}, {"membershipRevision", ObjectManager::instance().GetRuntimeActiveMembershipRevision()}}}},
			{{"surface", "resources"}, {"state", assetJobCount > 0 ? "compiling" : "available"}, {"details", nlohmann::json{{"assetCompileJobs", assetJobCount}}}},
			{{"surface", "streaming"}, {"state", "available"}, {"details", nlohmann::json::object()}},
			{{"surface", "capture"}, {"state", "available"}, {"details", nlohmann::json{{"availability", BuildCaptureAvailability()}, {"schedulerDeclarations", includeDetails ? captureSchedulers : nlohmann::json::array()}}}},
			{{"surface", "scheduler"}, {"state", "available"}, {"details", nlohmann::json{{"workerBoundaries", boundaryFindings}, {"parallelFrame", parallelFrame}, {"mutationDiagnostics", mutationDiagnostics}}}},
			{{"surface", "scripting"}, {"state", ScriptingSystem::Instance().IsRunning() ? "running" : "idle"}, {"details", nlohmann::json{{"coordinatorState", editorContext ? editorContext->scripting.coordinatorState : "Idle"}}}},
			{{"surface", "export"}, {"state", exportJobs.empty() ? "idle" : exportJobs.front().state}, {"details", nlohmann::json{{"jobs", exportJobJson}}}}
		});
		data["parallelFrame"] = parallelFrame;
		data["hardBoundaries"] = boundaryFindings;
		data["mutationDiagnostics"] = mutationDiagnostics;
		data["emptyStateSafe"] = true;
		return data;
	}

	nlohmann::json EditorAutomationService::BuildCaptureAvailability() const
	{
		const TerraEngine::Renderer::RenderCaptureAvailability availability = RenderSystem::Instance().GetCaptureAvailability();
		const McpCaptureLimits limits = GetMcpCaptureLimits();
		return {
			{"available", availability.available},
			{"state", availability.state},
			{"message", availability.message},
			{"retryable", availability.retryable},
			{"limits", {
				{"defaultWidth", limits.defaultWidth},
				{"defaultHeight", limits.defaultHeight},
				{"maxConcurrentCaptures", limits.maxConcurrentCaptures},
				{"maxQueuedCaptures", limits.maxQueuedCaptures},
				{"defaultTimeoutMs", limits.defaultTimeoutMs}
			}},
			{"sources", {
				{"gameView", true},
				{"cameraObject", true},
				{"disabledCameraObject", true},
				{"customCamera", true}
			}},
			{"queue", {
				{"pending", availability.queueState.pendingCaptureCount},
				{"active", availability.queueState.activeCaptureCount},
				{"completed", availability.queueState.completedResultCount},
				{"full", availability.queueFull}
			}},
			{"rendererInitialized", availability.rendererInitialized},
			{"gameViewAvailable", availability.gameViewAvailable},
			{"backendReadbackAvailable", availability.backendReadbackAvailable},
			{"deterministicFallbackAvailable", availability.deterministicFallbackAvailable},
			{"backendId", availability.backendId},
			{"protectedBoundary", availability.protectedBoundary},
			{"supportedSources", availability.supportedSources},
			{"notes", availability.notes}
		};
	}

	McpCaptureLimits EditorAutomationService::GetMcpCaptureLimits() const
	{
		const TerraEngine::Renderer::RenderCaptureLimits renderLimits = RenderSystem::Instance().GetCaptureLimits();
		McpCaptureLimits limits;
		limits.defaultWidth = renderLimits.defaultWidth;
		limits.defaultHeight = renderLimits.defaultHeight;
		limits.maxWidth = renderLimits.maxWidth;
		limits.maxHeight = renderLimits.maxHeight;
		limits.maxConcurrentCaptures = renderLimits.maxConcurrentCaptures;
		limits.maxQueuedCaptures = renderLimits.maxQueuedCaptures;
		limits.defaultTimeoutMs = renderLimits.defaultTimeoutMs;
		return limits;
	}

	nlohmann::json EditorAutomationService::ValidateCaptureArguments(const nlohmann::json& arguments, const std::string& requestId) const
	{
		const McpCaptureLimits limits = GetMcpCaptureLimits();
		auto readPositiveInteger = [&](std::string_view key, std::uint32_t defaultValue) -> std::optional<std::uint32_t> {
			const std::string field(key);
			if (!arguments.contains(field))
			{
				return defaultValue;
			}
			const nlohmann::json& value = arguments[field];
			try
			{
				if (value.is_number_unsigned())
				{
					const std::uint64_t raw = value.get<std::uint64_t>();
					if (raw == 0u || raw > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
					{
						return std::nullopt;
					}
					return static_cast<std::uint32_t>(raw);
				}
				if (value.is_number_integer())
				{
					const std::int64_t raw = value.get<std::int64_t>();
					if (raw <= 0 || raw > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
					{
						return std::nullopt;
					}
					return static_cast<std::uint32_t>(raw);
				}
			}
			catch (const nlohmann::json::exception&)
			{
				return std::nullopt;
			}
			return std::nullopt;
		};

		const std::optional<std::uint32_t> width = readPositiveInteger("width", limits.defaultWidth);
		const std::optional<std::uint32_t> height = readPositiveInteger("height", limits.defaultHeight);
		if (!width || !height)
		{
			return BuildError(requestId, ErrorCategory::Validation, "Capture width and height must be positive integers.");
		}
		if ((limits.maxWidth && *width > *limits.maxWidth) || (limits.maxHeight && *height > *limits.maxHeight))
		{
			return BuildError(requestId, ErrorCategory::OverBudget, "Capture width or height exceeds the configured MCP image limits.");
		}

		std::string format = "image/png";
		std::string freshness = "latestCompleted";
		bool includeOverlays = false;
		bool transparentBackground = false;
		bool allowBackendFallback = true;
		std::uint32_t timeoutMs = limits.defaultTimeoutMs;
		try
		{
			format = arguments.value("format", format);
			freshness = arguments.value("freshness", freshness);
			includeOverlays = arguments.value("includeOverlays", includeOverlays);
			transparentBackground = arguments.value("transparentBackground", transparentBackground);
			allowBackendFallback = arguments.value("allowBackendFallback", allowBackendFallback);
			timeoutMs = arguments.value("timeoutMs", timeoutMs);
		}
		catch (const nlohmann::json::exception&)
		{
			return BuildError(requestId, ErrorCategory::Validation, "Capture option types are invalid.");
		}
		if (format != "image/png")
		{
			return BuildError(requestId, ErrorCategory::UnsupportedReadback, "Only image/png capture output is currently supported.");
		}
		if (freshness != "latestCompleted" && freshness != "nextFrame")
		{
			return BuildError(requestId, ErrorCategory::Validation, "Capture freshness must be latestCompleted or nextFrame.");
		}
		if (timeoutMs == 0u)
		{
			timeoutMs = limits.defaultTimeoutMs;
		}

		return {
			{"ok", true},
			{"width", *width},
			{"height", *height},
			{"format", format},
			{"freshness", freshness},
			{"includeOverlays", includeOverlays},
			{"transparentBackground", transparentBackground},
			{"allowBackendFallback", allowBackendFallback},
			{"timeoutMs", timeoutMs}
		};
	}

	ErrorCategory EditorAutomationService::MapCaptureFailureCategory(std::string_view category) const
	{
		if (category == "missing_target")
		{
			return ErrorCategory::MissingTarget;
		}
		if (category == "invalid_camera_object")
		{
			return ErrorCategory::InvalidCameraObject;
		}
		if (category == "unsupported_readback")
		{
			return ErrorCategory::UnsupportedReadback;
		}
		if (category == "queue_full")
		{
			return ErrorCategory::QueueFull;
		}
		if (category == "over_budget")
		{
			return ErrorCategory::OverBudget;
		}
		if (category == "timeout")
		{
			return ErrorCategory::Canceled;
		}
		return ErrorCategory::Internal;
	}

	nlohmann::json EditorAutomationService::BuildActivityList(const std::vector<ActivityRecord>& records) const
	{
		nlohmann::json items = nlohmann::json::array();
		for (const ActivityRecord& record : records)
		{
			items.push_back({
				{"sequence", record.sequence},
				{"requestId", record.requestId},
				{"capability", record.capability},
				{"message", record.message},
				{"outcome", record.outcome},
				{"type", ToString(record.type)}
			});
		}
		return {{"activity", items}, {"count", items.size()}};
	}

	std::vector<ExportJob> EditorAutomationService::GetExportJobs() const
	{
		return exportJobs;
	}

	EditorAutomationService::ReflectionMutationPreflight EditorAutomationService::PreflightReflectionMutation(const nlohmann::json& arguments) const
	{
		ReflectionMutationPreflight result;
		result.classification = "protected";
		result.safeSubwork = "Target, component, property, and payload validation can run before the protected editor mutation.";
		result.protectedCommit = "Reflected setters, undo stack updates, object structure notifications, scene dirty markers, and selection/runtime inspector side effects stay protected.";
		if (!arguments.is_object())
		{
			result.message = "Tool arguments must be a JSON object.";
			return result;
		}

		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		const std::string component = arguments.value("component", "");
		const std::string property = arguments.value("property", "");
		if (!target.resolved || component.empty() || property.empty() || !arguments.contains("value"))
		{
			result.message = "component.set_property requires object, component, property, and value.";
			return result;
		}

		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto componentIt = ComponentAccessRegistry().find(component);
		if (componentIt == ComponentAccessRegistry().end() || !componentIt->second.entityHasComponent(ObjectManager::instance().getRegistry(), *target.entity))
		{
			result.message = "Component is not present on the object.";
			return result;
		}

		const nlohmann::json existingPayload = componentIt->second.entitySerialize(ObjectManager::instance().getRegistry(), *target.entity);
		if (!existingPayload.contains(property))
		{
			result.message = "Property is not present in component reflection metadata.";
			return result;
		}

		result.valid = true;
		result.message = "Reflection mutation preflight passed; apply the edit through the protected editor mutation handler.";
		return result;
	}

	EditorTarget EditorAutomationService::ResolveTarget(const nlohmann::json& value) const
	{
		EditorTarget target;
		if (value.is_number_unsigned() || value.is_number_integer())
		{
			const auto entity = static_cast<entt::entity>(value.get<std::uint32_t>());
			target.input = EntityToString(entity);
			ObjectManager& objects = ObjectManager::instance();
			if (std::optional<Object> object = objects.getObject(entity))
			{
				target.entity = entity;
				target.displayName = objects.getObjectName(*object);
				target.uuid = TerraEngine::ToHexString(objects.getUUID(*object));
				target.resolved = true;
			}
			return target;
		}
		if (!value.is_string())
		{
			return target;
		}

		target.input = value.get<std::string>();
		if (const auto parsed = TerraEngine::ParseUUID(target.input))
		{
			ObjectManager& objects = ObjectManager::instance();
			if (auto object = objects.getObject(*parsed))
			{
				target.entity = object->EntityId();
				target.displayName = objects.getObjectName(*object);
				target.uuid = TerraEngine::ToHexString(objects.getUUID(*object));
				target.resolved = true;
				return target;
			}
		}
		if (auto object = ObjectManager::instance().FindObjectByName(target.input))
		{
			target.entity = object->EntityId();
			target.displayName = ObjectManager::instance().getObjectName(*object);
			target.uuid = TerraEngine::ToHexString(ObjectManager::instance().getUUID(*object));
			target.resolved = true;
		}
		return target;
	}

	nlohmann::json EditorAutomationService::SerializeObjectSummary(entt::entity object, bool includeComponents) const
	{
		ObjectManager& objectManager = ObjectManager::instance();
		std::optional<Object> objectHandle = objectManager.getObject(object);
		nlohmann::json data;
		data["entity"] = static_cast<std::uint32_t>(object);
		data["uuid"] = objectHandle ? TerraEngine::ToHexString(objectManager.getUUID(*objectHandle)) : std::string{};
		data["name"] = objectHandle ? objectManager.getObjectName(*objectHandle) : "Unnamed";
		data["tag"] = objectHandle ? objectManager.getObjectTag(*objectHandle) : "Untagged";
		data["layer"] = objectHandle ? objectManager.getObjectLayer(*objectHandle) : "Default";
		data["active"] = objectHandle ? objectManager.isObjectActive(*objectHandle) : false;
		data["static"] = objectHandle ? objectManager.isObjectStatic(*objectHandle) : false;
		std::optional<Object> parent = objectHandle ? objectManager.getParent(*objectHandle) : std::nullopt;
		data["parent"] = parent ? nlohmann::json(static_cast<std::uint32_t>(parent->EntityId())) : nullptr;
		if (includeComponents)
		{
			nlohmann::json components = nlohmann::json::array();
			std::lock_guard lock(ComponentAccessRegistryMutex());
			for (const auto& [name, access] : ComponentAccessRegistry())
			{
				if (access.entityHasComponent && access.entityHasComponent(objectManager.getRegistry(), object))
				{
					components.push_back(name);
				}
			}
			data["components"] = components;
		}
		return data;
	}

	nlohmann::json EditorAutomationService::HandleSceneListObjects(const nlohmann::json& arguments, const std::string& requestId)
	{
		const bool includeComponents = arguments.value("includeComponents", false);
		const std::string search = arguments.value("search", "");
		nlohmann::json objects = nlohmann::json::array();
		std::vector<Object> roots = ObjectManager::instance().getRootObjects();
		ObjectManager::instance().SortObjectsByStableId(roots);
		for (Object object : roots)
		{
			const bool matches = search.empty()
				|| ContainsCaseInsensitive(ObjectManager::instance().getObjectName(object), search)
				|| ContainsCaseInsensitive(ObjectManager::instance().getObjectTag(object), search)
				|| ContainsCaseInsensitive(ObjectManager::instance().getObjectLayer(object), search);
			if (matches)
			{
				objects.push_back(SerializeObjectSummary(object.EntityId(), includeComponents));
			}
		}
		return BuildSuccess(requestId, "Listed scene objects.", {{"objects", objects}});
	}

	nlohmann::json EditorAutomationService::HandleSceneSave(const nlohmann::json& arguments, const std::string& requestId)
	{
		const bool saved = arguments.contains("path") && arguments["path"].is_string()
			? SceneManager::Instance().SaveScene(arguments["path"].get<std::string>())
			: SceneManager::Instance().SaveScene();
		if (!saved)
		{
			return BuildError(requestId, ErrorCategory::Internal, "Scene save failed.");
		}
		if (activityCallback)
		{
			activityCallback(ActivityEventType::Mutation, requestId, "scene.save", "Saved active scene.");
		}
		return BuildSuccess(requestId, "Saved active scene.", {{"dirty", SceneManager::Instance().IsActiveSceneDirty()}});
	}

	nlohmann::json EditorAutomationService::HandleObjectCreate(const nlohmann::json& arguments, const std::string& requestId)
	{
		if (!arguments.contains("name") || !arguments["name"].is_string())
		{
			return BuildError(requestId, ErrorCategory::Validation, "object.create requires a string name.");
		}
		Object object = ObjectManager::instance().createObject(arguments["name"].get<std::string>());
		const entt::entity entity = object.EntityId();
		if (arguments.contains("parent") && !arguments["parent"].is_null())
		{
			EditorTarget parent = ResolveTarget(arguments["parent"]);
			if (!parent.resolved)
			{
				ObjectManager::instance().removeObject(object);
				return BuildError(requestId, ErrorCategory::MissingTarget, "Parent object was not found.", parent.input);
			}
			std::optional<Object> parentObject = ObjectManager::instance().getObject(*parent.entity);
			if (!parentObject || !ObjectManager::instance().setParent(object, parentObject))
			{
				ObjectManager::instance().removeObject(object);
				return BuildError(requestId, ErrorCategory::Conflict, "Parent change would create an invalid hierarchy.");
			}
		}
		if (arguments.value("select", false) && setSelectedEntity)
		{
			setSelectedEntity(entity);
		}
		UndoRedoSystem::Instance().PushCommand(std::make_unique<CreateObjectCommand>(entity));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "object.create", "Created object " + ObjectManager::instance().getObjectName(object) + ".");
		return BuildSuccess(requestId, "Created object.", {{"object", SerializeObjectSummary(entity, true)}});
	}

	nlohmann::json EditorAutomationService::HandleObjectDelete(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		if (!target.resolved)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object was not found.", target.input);
		}
		// DeleteObjectCommand snapshots the object before it disappears, so the
		// command is created first and pushed only after the mutation succeeds.
		auto command = std::make_unique<DeleteObjectCommand>(*target.entity);
		if (std::optional<Object> object = ObjectManager::instance().getObject(*target.entity))
		{
			ObjectManager::instance().removeObject(*object);
		}
		UndoRedoSystem::Instance().PushCommand(std::move(command));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "object.delete", "Deleted object " + target.displayName + ".");
		return BuildSuccess(requestId, "Deleted object.", {{"object", target.displayName}});
	}

	nlohmann::json EditorAutomationService::HandleObjectSelect(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		if (!target.resolved || !setSelectedEntity)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Selectable object was not found.", target.input);
		}
		setSelectedEntity(*target.entity);
		return BuildSuccess(requestId, "Selected object.", {{"object", target.displayName}});
	}

	nlohmann::json EditorAutomationService::HandleObjectSetMetadata(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		if (!target.resolved)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object was not found.", target.input);
		}
		ObjectManager& objectManager = ObjectManager::instance();
		const entt::entity object = *target.entity;
		std::optional<Object> objectHandle = objectManager.getObject(object);
		if (!objectHandle)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object was not found.", target.input);
		}
		const nlohmann::json oldMetadata = {
			{"name", objectManager.getObjectName(*objectHandle)},
			{"tag", objectManager.getObjectTag(*objectHandle)},
			{"layer", objectManager.getObjectLayer(*objectHandle)},
			{"active", objectManager.isObjectActive(*objectHandle)},
			{"static", objectManager.isObjectStatic(*objectHandle)}
		};
		if (arguments.contains("name") && arguments["name"].is_string()) objectManager.setObjectName(*objectHandle, arguments["name"].get<std::string>());
		if (arguments.contains("tag") && arguments["tag"].is_string()) objectManager.setObjectTag(*objectHandle, arguments["tag"].get<std::string>());
		if (arguments.contains("layer") && arguments["layer"].is_string()) objectManager.setObjectLayer(*objectHandle, arguments["layer"].get<std::string>());
		if (arguments.contains("active") && arguments["active"].is_boolean()) objectManager.setObjectActive(*objectHandle, arguments["active"].get<bool>());
		if (arguments.contains("static") && arguments["static"].is_boolean()) objectManager.setObjectStatic(*objectHandle, arguments["static"].get<bool>());
		const nlohmann::json newMetadata = {
			{"name", objectManager.getObjectName(*objectHandle)},
			{"tag", objectManager.getObjectTag(*objectHandle)},
			{"layer", objectManager.getObjectLayer(*objectHandle)},
			{"active", objectManager.isObjectActive(*objectHandle)},
			{"static", objectManager.isObjectStatic(*objectHandle)}
		};
		UndoRedoSystem::Instance().PushCommand(std::make_unique<SetObjectMetadataCommand>(object, oldMetadata, newMetadata));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "object.set_metadata", "Updated object metadata.");
		return BuildSuccess(requestId, "Updated object metadata.", {{"object", SerializeObjectSummary(object, true)}});
	}

	nlohmann::json EditorAutomationService::HandleObjectSetParent(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget childTarget = ResolveTarget(arguments.value("object", nlohmann::json()));
		if (!childTarget.resolved)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Child object was not found.", childTarget.input);
		}
		const entt::entity child = *childTarget.entity;
		std::optional<TerraEngine::UUID> oldParentUuid;
		std::optional<Object> childObject = ObjectManager::instance().getObject(child);
		if (!childObject)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Child object was not found.", childTarget.input);
		}
		if (std::optional<Object> oldParent = ObjectManager::instance().getParent(*childObject))
		{
			oldParentUuid = ObjectManager::instance().getUUID(*oldParent);
		}
		std::optional<Object> parent;
		std::optional<TerraEngine::UUID> newParentUuid;
		if (arguments.contains("parent") && !arguments["parent"].is_null())
		{
			EditorTarget parentTarget = ResolveTarget(arguments["parent"]);
			if (!parentTarget.resolved)
			{
				return BuildError(requestId, ErrorCategory::MissingTarget, "Parent object was not found.", parentTarget.input);
			}
			parent = ObjectManager::instance().getObject(*parentTarget.entity);
			if (!parent)
			{
				return BuildError(requestId, ErrorCategory::MissingTarget, "Parent object was not found.", parentTarget.input);
			}
			newParentUuid = ObjectManager::instance().getUUID(*parent);
		}
		if (!ObjectManager::instance().setParent(*childObject, parent))
		{
			return BuildError(requestId, ErrorCategory::Conflict, "Parent change would create an invalid hierarchy.");
		}
		UndoRedoSystem::Instance().PushCommand(std::make_unique<SetParentCommand>(child, oldParentUuid, newParentUuid));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "object.set_parent", "Updated object parent.");
		return BuildSuccess(requestId, "Updated object parent.", {{"object", SerializeObjectSummary(child, true)}});
	}

	nlohmann::json EditorAutomationService::HandleComponentAdd(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		const std::string component = arguments.value("component", "");
		if (!target.resolved || component.empty())
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object or component name was not found.", target.input);
		}
		ObjectManager& objectManager = ObjectManager::instance();
		const entt::entity object = *target.entity;
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(component);
		if (it == ComponentAccessRegistry().end())
		{
			return BuildError(requestId, ErrorCategory::UnsupportedCapability, "Component is not registered for automation.", component);
		}
		it->second.entityAddComponent(objectManager.getRegistry(), object);
		if (std::optional<Object> objectHandle = objectManager.getObject(object))
		{
			objectManager.notifyObjectStructureChanged(*objectHandle);
		}
		UndoRedoSystem::Instance().PushCommand(std::make_unique<AddComponentCommand>(object, component));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "component.add", "Added component " + component + ".");
		return BuildSuccess(requestId, "Added component.", {{"object", target.displayName}, {"component", component}});
	}

	nlohmann::json EditorAutomationService::HandleComponentRemove(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		const std::string component = arguments.value("component", "");
		if (!target.resolved || component.empty())
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object or component name was not found.", target.input);
		}
		ObjectManager& objectManager = ObjectManager::instance();
		const entt::entity object = *target.entity;
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(component);
		if (it == ComponentAccessRegistry().end() || !it->second.entityHasComponent(objectManager.getRegistry(), object))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Component is not present on the object.", component);
		}
		// RemoveComponentCommand captures the existing component payload for
		// undo, so construct it while the component is still attached.
		auto command = std::make_unique<RemoveComponentCommand>(object, component);
		it->second.entityRemoveComponent(objectManager.getRegistry(), object);
		if (std::optional<Object> objectHandle = objectManager.getObject(object))
		{
			objectManager.notifyObjectStructureChanged(*objectHandle);
		}
		UndoRedoSystem::Instance().PushCommand(std::move(command));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "component.remove", "Removed component " + component + ".");
		return BuildSuccess(requestId, "Removed component.", {{"object", target.displayName}, {"component", component}});
	}

	nlohmann::json EditorAutomationService::HandleComponentGet(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		const std::string component = arguments.value("component", "");
		if (!target.resolved || component.empty())
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object or component name was not found.", target.input);
		}
		ObjectManager& objectManager = ObjectManager::instance();
		const entt::entity object = *target.entity;
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(component);
		if (it == ComponentAccessRegistry().end() || !it->second.entityHasComponent(objectManager.getRegistry(), object))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Component is not present on the object.", component);
		}
		return BuildSuccess(requestId, "Read component.", {{"component", component}, {"properties", it->second.entitySerialize(objectManager.getRegistry(), object)}});
	}

	nlohmann::json EditorAutomationService::HandleComponentSetProperty(const nlohmann::json& arguments, const std::string& requestId)
	{
		EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json()));
		const std::string component = arguments.value("component", "");
		const std::string property = arguments.value("property", "");
		const ReflectionMutationPreflight preflight = PreflightReflectionMutation(arguments);
		if (!preflight.valid)
		{
			return BuildError(requestId, ErrorCategory::Validation, preflight.message);
		}
		ObjectManager& objectManager = ObjectManager::instance();
		const entt::entity object = *target.entity;
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(component);
		if (it == ComponentAccessRegistry().end() || !it->second.entityHasComponent(objectManager.getRegistry(), object))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Component is not present on the object.", component);
		}
		const nlohmann::json oldPayload = it->second.entitySerialize(objectManager.getRegistry(), object);
		it->second.entityDeserialize(objectManager.getRegistry(), object, nlohmann::json{{property, arguments["value"]}});
		const nlohmann::json newPayload = it->second.entitySerialize(objectManager.getRegistry(), object);
		if (std::optional<Object> objectHandle = objectManager.getObject(object))
		{
			objectManager.notifyObjectStructureChanged(*objectHandle);
		}
		UndoRedoSystem::Instance().PushCommand(std::make_unique<SetComponentPayloadCommand>(object, component, oldPayload, newPayload));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "component.set_property", "Updated component property.");
		return BuildSuccess(requestId, "Updated component property.", {{"component", component}, {"property", property}});
	}

	nlohmann::json EditorAutomationService::ValidateProjectRelativePath(const nlohmann::json& arguments, std::string_view key, const std::string& requestId) const
	{
		if (!arguments.contains(std::string(key)) || !arguments[std::string(key)].is_string())
		{
			return BuildError(requestId, ErrorCategory::Validation, "Expected a project-relative path string.");
		}
		const std::filesystem::path relative = arguments[std::string(key)].get<std::string>();
		if (relative.is_absolute() || relative.string().find("..") != std::string::npos)
		{
			return BuildError(requestId, ErrorCategory::Validation, "Path must stay inside the active project.");
		}
		return nlohmann::json{{"ok", true}, {"path", relative.generic_string()}};
	}

	nlohmann::json EditorAutomationService::ValidateAssetPath(const nlohmann::json& arguments, const std::string& requestId) const
	{
		nlohmann::json validation = ValidateProjectRelativePath(arguments, "path", requestId);
		if (!validation.value("ok", false))
		{
			return validation;
		}
		std::filesystem::path relative = validation["path"].get<std::string>();
		if (!relative.empty() && relative.begin()->string() == "Assets")
		{
			relative = std::filesystem::relative(relative, "Assets");
		}
		if (relative.empty())
		{
			return BuildError(requestId, ErrorCategory::Validation, "Asset path must name a file under Assets.");
		}
		return nlohmann::json{{"ok", true}, {"path", relative.generic_string()}};
	}

	nlohmann::json EditorAutomationService::HandleAssetList(const nlohmann::json& arguments, const std::string& requestId)
	{
		const std::string search = arguments.value("search", "");
		nlohmann::json assets = nlohmann::json::array();
		if (editorContext != nullptr && editorContext->assetDatabase != nullptr)
		{
			const std::filesystem::path assetRoot = editorContext->assetDatabase->GetAssetRoot();
			for (const auto& [absolutePath, metadata] : editorContext->assetDatabase->GetAssets())
			{
				const std::string path = std::filesystem::relative(absolutePath, assetRoot).generic_string();
				if (search.empty() || ContainsCaseInsensitive(path, search) || ContainsCaseInsensitive(metadata.importerName, search))
				{
					assets.push_back({
						{"path", path},
						{"guid", TerraEngine::ToHexString(metadata.guid)},
						{"importer", metadata.importerName},
						{"compiledPath", metadata.compiledPath.generic_string()},
						{"status", editorContext->assetDatabase->IsAssetCompiling(absolutePath) ? "compiling" : "known"}
					});
				}
			}
		}
		return BuildSuccess(requestId, "Listed project assets.", {{"assets", assets}});
	}

	nlohmann::json EditorAutomationService::HandleAssetImport(const nlohmann::json& arguments, const std::string& requestId)
	{
		nlohmann::json validation = ValidateAssetPath(arguments, requestId);
		if (!validation.value("ok", false)) return validation;
		if (editorContext == nullptr || editorContext->assetDatabase == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Asset database is not available.");
		}
		const std::filesystem::path relative = validation["path"].get<std::string>();
		const std::filesystem::path absolutePath = editorContext->assetDatabase->GetAssetRoot() / relative;
		if (!std::filesystem::exists(absolutePath) || std::filesystem::is_directory(absolutePath))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Asset file was not found.", relative.generic_string());
		}
		const std::filesystem::path canonicalAssetRoot = std::filesystem::weakly_canonical(editorContext->assetDatabase->GetAssetRoot());
		const std::filesystem::path canonicalAssetPath = std::filesystem::weakly_canonical(absolutePath);
		const std::string rootText = canonicalAssetRoot.generic_string();
		const std::string assetText = canonicalAssetPath.generic_string();
		if (assetText.size() < rootText.size()
			|| assetText.compare(0, rootText.size(), rootText) != 0
			|| (assetText.size() > rootText.size() && assetText[rootText.size()] != '/'))
		{
			return BuildError(requestId, ErrorCategory::Validation, "Asset import path must stay inside the active project's Assets folder.", relative.generic_string());
		}
		editorContext->assetDatabase->ImportAsset(absolutePath, arguments.value("force", false));
		if (activityCallback)
		{
			activityCallback(ActivityEventType::Mutation, requestId, "asset.import", "Queued asset import.");
		}

		nlohmann::json data = {
			{"path", validation["path"]},
			{"force", arguments.value("force", false)},
			{"status", "queued"}
		};
		if (TerraEngine::AssetPipeline::IsModelPackageSourceExtension(absolutePath))
		{
			const std::filesystem::path resultPath = TerraEngine::AssetPipeline::GetImportResultPath(absolutePath);
			if (std::filesystem::exists(resultPath))
			{
				std::ifstream in(resultPath);
				nlohmann::json importResult = nlohmann::json::object();
				in >> importResult;
				data["status"] = importResult.value("status", "Unknown");
				data["virtualGeometry"] = BuildPackageVirtualGeometryAutomationSummary(importResult);
				data["generatedMeshes"] = data["virtualGeometry"]["generatedMeshes"];
				data["importResult"] = std::move(importResult);
			}
			else
			{
				data["importResult"] = nullptr;
			}
		}
		else
		{
			std::filesystem::path resolvedPath;
			if (const AssetMetadata* metadata = ResolveAutomationAssetMetadata(*editorContext->assetDatabase, relative.generic_string(), resolvedPath))
			{
				VirtualGeometryAssetMetadata virtualGeometryMetadata{};
				if (editorContext->assetDatabase->TryGetVirtualGeometryMetadata(*metadata, virtualGeometryMetadata))
				{
					data["virtualGeometry"] = SerializeVirtualGeometryMetadata(virtualGeometryMetadata);
					data["status"] = virtualGeometryMetadata.needsRebuild ? "needsRebuild" : "imported";
				}
			}
		}
		return BuildSuccess(requestId, "Queued asset import.", std::move(data));
	}

	nlohmann::json EditorAutomationService::HandleVirtualGeometrySettingsGet(const nlohmann::json& arguments, const std::string& requestId)
	{
		if (editorContext == nullptr || editorContext->assetDatabase == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Asset database is not available.");
		}

		const VirtualGeometryAdaptiveSettingsMetadata defaults = editorContext->assetDatabase->GetDefaultVirtualGeometryAdaptiveSettings();
		nlohmann::json data = {
			{"projectDefaults", SerializeVirtualGeometrySettings(defaults)},
			{"settingsSignature", BuildVirtualGeometrySettingsSignature(defaults)},
			{"hasMeshOverride", false},
			{"meshOverride", nullptr},
			{"resolvedSettings", SerializeVirtualGeometrySettings(defaults)},
			{"validation", {{"valid", defaults.valid}, {"errors", defaults.validationErrors}}}
		};
		if (!arguments.contains("asset") || arguments["asset"].is_null())
		{
			return BuildSuccess(requestId, "Resolved virtual geometry project defaults.", std::move(data));
		}

		const std::string asset = arguments.value("asset", std::string());
		std::filesystem::path absolutePath;
		const AssetMetadata* metadata = ResolveAutomationAssetMetadata(*editorContext->assetDatabase, asset, absolutePath);
		if (metadata == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Virtual geometry settings asset was not found.", asset);
		}

		VirtualGeometryAdaptiveSettingsMetadata resolved;
		if (!editorContext->assetDatabase->TryGetVirtualGeometryAdaptiveSettings(*metadata, resolved))
		{
			return BuildError(requestId, ErrorCategory::Validation, "Selected asset is not a mesh asset.", asset);
		}

		const bool hasOverride = editorContext->assetDatabase->HasVirtualGeometryPerMeshOverride(*metadata);
		data["assetId"] = TerraEngine::ToHexString(metadata->guid);
		data["asset"] = std::filesystem::relative(absolutePath, editorContext->assetDatabase->GetAssetRoot()).generic_string();
		data["hasMeshOverride"] = hasOverride;
		data["meshOverride"] = hasOverride ? SerializeVirtualGeometrySettings(resolved) : nlohmann::json(nullptr);
		data["resolvedSettings"] = SerializeVirtualGeometrySettings(resolved);
		data["settingsSignature"] = BuildVirtualGeometrySettingsSignature(resolved);
		data["validation"] = {{"valid", resolved.valid}, {"errors", resolved.validationErrors}};
		return BuildSuccess(requestId, "Resolved virtual geometry settings.", std::move(data));
	}

	nlohmann::json EditorAutomationService::HandleVirtualGeometrySettingsSet(const nlohmann::json& arguments, const std::string& requestId)
	{
		if (editorContext == nullptr || editorContext->assetDatabase == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Asset database is not available.");
		}
		if (!arguments.contains("asset") || !arguments["asset"].is_string())
		{
			return BuildError(requestId, ErrorCategory::Validation, "asset.virtualGeometry.settings.set requires an asset.");
		}
		if (!arguments.contains("override") || !arguments["override"].is_object())
		{
			return BuildError(requestId, ErrorCategory::Validation, "asset.virtualGeometry.settings.set requires an override object.");
		}

		const std::string asset = arguments.value("asset", std::string());
		std::filesystem::path absolutePath;
		const AssetMetadata* metadata = ResolveAutomationAssetMetadata(*editorContext->assetDatabase, asset, absolutePath);
		if (metadata == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Virtual geometry settings asset was not found.", asset);
		}
		if (!editorContext->assetDatabase->CanEditVirtualGeometryPerMeshOverride(*metadata))
		{
			return BuildError(requestId, ErrorCategory::Validation, "Selected asset is not an imported mesh asset with editable virtual geometry settings.", asset);
		}

		const VirtualGeometryAdaptiveSettingsMetadata settings =
			ParseVirtualGeometrySettingsOverride(arguments["override"], editorContext->assetDatabase->GetDefaultVirtualGeometryAdaptiveSettings());
		const bool validateOnly = arguments.value("validateOnly", false);
		const bool rebuildAfterSave = arguments.value("rebuildAfterSave", false);
		std::string error;
		if (!validateOnly && !editorContext->assetDatabase->SaveVirtualGeometryPerMeshOverride(metadata->guid, settings, &error))
		{
			return BuildError(requestId, ErrorCategory::Validation, error.empty() ? "Virtual geometry settings were invalid." : error, asset);
		}

		std::string status = validateOnly ? "validated" : "saved";
		if (!validateOnly && rebuildAfterSave)
		{
			if (!editorContext->assetDatabase->RebuildVirtualGeometryAsset(metadata->guid, &error))
			{
				return BuildError(requestId, ErrorCategory::Validation, error.empty() ? "Virtual geometry rebuild failed after saving settings." : error, asset);
			}
			status = "rebuildQueued";
		}

		VirtualGeometryAdaptiveSettingsMetadata resolved;
		editorContext->assetDatabase->TryGetVirtualGeometryAdaptiveSettings(*metadata, resolved);
		if (validateOnly)
		{
			resolved = settings;
		}
		if (activityCallback && !validateOnly)
		{
			activityCallback(ActivityEventType::Mutation, requestId, "asset.virtualGeometry.settings.set", "Saved virtual geometry per-mesh settings.");
		}
		return BuildSuccess(requestId, "Processed virtual geometry settings.", {
			{"status", status},
			{"assetId", TerraEngine::ToHexString(metadata->guid)},
			{"hasMeshOverride", !validateOnly && editorContext->assetDatabase->HasVirtualGeometryPerMeshOverride(*metadata)},
			{"resolvedSettings", SerializeVirtualGeometrySettings(resolved)},
			{"settingsSignature", BuildVirtualGeometrySettingsSignature(resolved)},
			{"validationErrors", resolved.validationErrors},
			{"warnings", nlohmann::json::array()},
			{"assetRegistrationBoundary", "asset-metadata-save"}
		});
	}

	nlohmann::json EditorAutomationService::HandleVirtualGeometrySettingsReset(const nlohmann::json& arguments, const std::string& requestId)
	{
		if (editorContext == nullptr || editorContext->assetDatabase == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Asset database is not available.");
		}
		if (!arguments.contains("asset") || !arguments["asset"].is_string())
		{
			return BuildError(requestId, ErrorCategory::Validation, "asset.virtualGeometry.settings.reset requires an asset.");
		}

		const std::string asset = arguments.value("asset", std::string());
		std::filesystem::path absolutePath;
		const AssetMetadata* metadata = ResolveAutomationAssetMetadata(*editorContext->assetDatabase, asset, absolutePath);
		if (metadata == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Virtual geometry settings asset was not found.", asset);
		}
		if (!editorContext->assetDatabase->CanEditVirtualGeometryPerMeshOverride(*metadata))
		{
			return BuildError(requestId, ErrorCategory::Validation, "Selected asset is not an imported mesh asset with editable virtual geometry settings.", asset);
		}

		const bool rebuildAfterReset = arguments.value("rebuildAfterReset", false);
		std::string error;
		if (!editorContext->assetDatabase->ClearVirtualGeometryPerMeshOverride(metadata->guid, &error))
		{
			return BuildError(requestId, ErrorCategory::Validation, error.empty() ? "Virtual geometry settings reset failed." : error, asset);
		}

		std::string status = "reset";
		if (rebuildAfterReset)
		{
			if (!editorContext->assetDatabase->RebuildVirtualGeometryAsset(metadata->guid, &error))
			{
				return BuildError(requestId, ErrorCategory::Validation, error.empty() ? "Virtual geometry rebuild failed after resetting settings." : error, asset);
			}
			status = "rebuildQueued";
		}

		VirtualGeometryAdaptiveSettingsMetadata resolved;
		editorContext->assetDatabase->TryGetVirtualGeometryAdaptiveSettings(*metadata, resolved);
		if (activityCallback)
		{
			activityCallback(ActivityEventType::Mutation, requestId, "asset.virtualGeometry.settings.reset", "Reset virtual geometry per-mesh settings.");
		}
		return BuildSuccess(requestId, "Reset virtual geometry settings.", {
			{"status", status},
			{"assetId", TerraEngine::ToHexString(metadata->guid)},
			{"hasMeshOverride", false},
			{"resolvedSettings", SerializeVirtualGeometrySettings(resolved)},
			{"settingsSignature", BuildVirtualGeometrySettingsSignature(resolved)},
			{"warnings", nlohmann::json::array()}
		});
	}

	nlohmann::json EditorAutomationService::HandleVirtualGeometryRebuild(const nlohmann::json& arguments, const std::string& requestId)
	{
		if (editorContext == nullptr || editorContext->assetDatabase == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Asset database is not available.");
		}

		const std::string target = arguments.value("target", std::string());
		if (target.empty())
		{
			return BuildError(requestId, ErrorCategory::Validation, "asset.virtualGeometry.rebuild requires a target.");
		}

		const bool force = arguments.value("force", false);
		const bool includePackageOutputs = arguments.value("includePackageOutputs", true);
		const std::string diagnosticsLevel = arguments.value("diagnosticsLevel", "summary");
		const auto started = std::chrono::steady_clock::now();

		std::vector<std::filesystem::path> rebuildTargets;
		if (LowerAscii(target) == "all")
		{
			for (const auto& [absolutePath, metadata] : editorContext->assetDatabase->GetAssets())
			{
				if (metadata.importerName == "MeshImporter" || TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName))
				{
					rebuildTargets.push_back(absolutePath);
				}
			}
		}
		else
		{
			std::filesystem::path absolutePath;
			const AssetMetadata* metadata = ResolveAutomationAssetMetadata(*editorContext->assetDatabase, target, absolutePath);
			if (metadata == nullptr && std::filesystem::exists(absolutePath))
			{
				rebuildTargets.push_back(absolutePath);
			}
			else if (metadata != nullptr)
			{
				rebuildTargets.push_back(absolutePath);
			}
			else
			{
				return BuildError(requestId, ErrorCategory::MissingTarget, "Virtual geometry rebuild target was not found.", target);
			}
		}

		nlohmann::json rebuiltAssets = nlohmann::json::array();
		nlohmann::json skippedAssets = nlohmann::json::array();
		nlohmann::json failedAssets = nlohmann::json::array();
		nlohmann::json warnings = nlohmann::json::array();
		const std::filesystem::path assetRoot = editorContext->assetDatabase->GetAssetRoot();

		for (const std::filesystem::path& absolutePath : rebuildTargets)
		{
			const std::filesystem::path relative = std::filesystem::relative(absolutePath, assetRoot);
			if (!std::filesystem::exists(absolutePath))
			{
				failedAssets.push_back({{"path", relative.generic_string()}, {"reason", "missing source asset"}});
				continue;
			}

			editorContext->assetDatabase->ImportAsset(absolutePath, force);
			std::filesystem::path resolvedPath;
			const AssetMetadata* metadata = ResolveAutomationAssetMetadata(*editorContext->assetDatabase, relative.generic_string(), resolvedPath);
			if (metadata == nullptr)
			{
				failedAssets.push_back({{"path", relative.generic_string()}, {"reason", "asset metadata was not published"}});
				continue;
			}

			VirtualGeometryAssetMetadata virtualGeometryMetadata{};
			const bool readable = editorContext->assetDatabase->TryGetVirtualGeometryMetadata(*metadata, virtualGeometryMetadata);
			VirtualGeometryAdaptiveSettingsMetadata settingsMetadata;
			editorContext->assetDatabase->TryGetVirtualGeometryAdaptiveSettings(*metadata, settingsMetadata);
			std::vector<VirtualGeometryDetailDiagnosticMetadata> detailDiagnostics;
			editorContext->assetDatabase->TryGetVirtualGeometryDetailDiagnostics(*metadata, detailDiagnostics);
			std::uint32_t generatedDetailCount = 0u;
			for (const VirtualGeometryDetailDiagnosticMetadata& diagnostic : detailDiagnostics)
			{
				generatedDetailCount += diagnostic.status == "Generated" ? 1u : 0u;
			}
			VirtualGeometryClusterSummaryMetadata clusterSummary;
			editorContext->assetDatabase->TryGetVirtualGeometryClusterSummary(*metadata, clusterSummary);
			VirtualGeometryPageSummaryMetadata pageSummary;
			editorContext->assetDatabase->TryGetVirtualGeometryPageSummary(*metadata, pageSummary);
			nlohmann::json record = {
				{"path", relative.generic_string()},
				{"guid", TerraEngine::ToHexString(metadata->guid)},
				{"importer", metadata->importerName},
				{"compiledPath", metadata->compiledPath.generic_string()},
				{"settingsSource", settingsMetadata.settingsSource},
				{"requestedDetailCount", virtualGeometryMetadata.requestedTargetCount},
				{"generatedDetailCount", generatedDetailCount},
				{"skippedDetails", virtualGeometryMetadata.skippedDetailCount},
				{"clusterCount", clusterSummary.clusterCount},
				{"pageCount", pageSummary.pageCount},
				{"fallbackCoverageValid", clusterSummary.fallbackCoverageValid},
				{"settings", SerializeVirtualGeometrySettings(settingsMetadata)},
				{"clusterSummary", SerializeVirtualGeometryClusterSummary(clusterSummary)},
				{"pageResidencySummary", SerializeVirtualGeometryPageSummary(pageSummary)},
				{"virtualGeometry", readable ? SerializeVirtualGeometryMetadata(virtualGeometryMetadata) : nlohmann::json::object()}
			};

			if (!readable || virtualGeometryMetadata.needsRebuild)
			{
				record["reason"] = readable ? "virtual geometry metadata needs rebuild" : "virtual geometry metadata is not readable";
				failedAssets.push_back(record);
			}
			else if (!virtualGeometryMetadata.hasVirtualGeometry && metadata->importerName != "MeshImporter" && !TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata->importerName))
			{
				record["reason"] = "asset is not a virtual geometry mesh source";
				skippedAssets.push_back(record);
			}
			else
			{
				if (includePackageOutputs && TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata->importerName))
				{
					const std::filesystem::path resultPath = TerraEngine::AssetPipeline::GetImportResultPath(absolutePath);
					if (std::filesystem::exists(resultPath))
					{
						std::ifstream input(resultPath);
						nlohmann::json importResult = nlohmann::json::object();
						input >> importResult;
						record["generatedMeshes"] = BuildPackageVirtualGeometryAutomationSummary(importResult)["generatedMeshes"];
					}
				}
				rebuiltAssets.push_back(record);
			}
		}

		const auto finished = std::chrono::steady_clock::now();
		const double durationMs = std::chrono::duration<double, std::milli>(finished - started).count();
		const std::string status = failedAssets.empty()
			? "completed"
			: (rebuiltAssets.empty() ? "failed" : "partial");
		if (activityCallback)
		{
			activityCallback(ActivityEventType::Mutation, requestId, "asset.virtualGeometry.rebuild", "Processed virtual geometry rebuild request.");
		}

		return BuildSuccess(requestId, "Processed virtual geometry rebuild.", {
			{"status", status},
			{"target", target},
			{"force", force},
			{"includePackageOutputs", includePackageOutputs},
			{"diagnosticsLevel", diagnosticsLevel},
			{"assetRegistrationBoundary", "asset-database-finish-import"},
			{"rebuiltAssets", rebuiltAssets},
			{"skippedAssets", skippedAssets},
			{"failedAssets", failedAssets},
			{"warnings", warnings},
			{"durationMs", durationMs}
		});
	}

	nlohmann::json EditorAutomationService::HandleVirtualGeometryInspect(const nlohmann::json& arguments, const std::string& requestId)
	{
		if (editorContext == nullptr || editorContext->assetDatabase == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Asset database is not available.");
		}

		const std::string asset = arguments.value("asset", std::string());
		if (asset.empty())
		{
			return BuildError(requestId, ErrorCategory::Validation, "asset.virtualGeometry.inspect requires an asset.");
		}

		std::filesystem::path absolutePath;
		const AssetMetadata* metadata = ResolveAutomationAssetMetadata(*editorContext->assetDatabase, asset, absolutePath);
		if (metadata == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Virtual geometry asset was not found.", asset);
		}

		VirtualGeometryAssetMetadata virtualGeometryMetadata{};
		const bool readable = editorContext->assetDatabase->TryGetVirtualGeometryMetadata(*metadata, virtualGeometryMetadata);
		nlohmann::json cookMetadata = readable ? SerializeVirtualGeometryMetadata(virtualGeometryMetadata) : nlohmann::json::object();
		VirtualGeometryAdaptiveSettingsMetadata settingsMetadata;
		editorContext->assetDatabase->TryGetVirtualGeometryAdaptiveSettings(*metadata, settingsMetadata);
		std::vector<VirtualGeometryDetailDiagnosticMetadata> detailDiagnostics;
		editorContext->assetDatabase->TryGetVirtualGeometryDetailDiagnostics(*metadata, detailDiagnostics);

		nlohmann::json runtimeDiagnostics = {
			{"available", false},
			{"views", nlohmann::json::array()}
		};
		nlohmann::json residencyDiagnostics = {
			{"residentPages", 0u},
			{"requestedPages", 0u},
			{"missingPages", 0u},
			{"budgetPressureBytes", 0u}
		};
		nlohmann::json selectionSummary = {
			{"selectedClusters", 0u},
			{"fallbackSelections", 0u},
			{"selectedMeshlets", 0u},
			{"dispatchedMeshlets", 0u},
			{"invalidSelectionReasons", nlohmann::json::array()}
		};
		nlohmann::json hiZValidation = {
			{"trusted", false},
			{"inactiveReason", "unavailable"},
			{"culledByHiZ", 0u}
		};
		if (arguments.value("includeRuntimeState", false))
		{
			const auto viewFilter = arguments.contains("viewType")
				? ParseAutomationRenderViewType(arguments["viewType"])
				: std::optional<RenderViewType>{};
			const TerraEngine::Renderer::RenderDiagnosticsSummary summary = RenderSystem::Instance().GetLatestRenderGraphDiagnostics();
			for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& view : summary.virtualGeometryViews)
			{
				if (viewFilter && view.viewType != *viewFilter)
				{
					continue;
				}
				runtimeDiagnostics["available"] = true;
				runtimeDiagnostics["views"].push_back(SerializeVirtualGeometryRuntimeView(view));
				residencyDiagnostics["residentPages"] = residencyDiagnostics.value("residentPages", 0u) + view.residentPages;
				residencyDiagnostics["requestedPages"] = residencyDiagnostics.value("requestedPages", 0u) + view.requestedPages;
				residencyDiagnostics["missingPages"] = residencyDiagnostics.value("missingPages", 0u) + view.missingPages;
				residencyDiagnostics["budgetPressureBytes"] = residencyDiagnostics.value("budgetPressureBytes", 0ull) + view.budgetPressureBytes;
				selectionSummary["selectedClusters"] = selectionSummary.value("selectedClusters", 0u) + view.clusterSelection.selectedClusters;
				selectionSummary["fallbackSelections"] = selectionSummary.value("fallbackSelections", 0u) + view.clusterSelection.fallbackClusters;
				selectionSummary["selectedMeshlets"] = selectionSummary.value("selectedMeshlets", 0u) + view.selectedMeshlets;
				selectionSummary["dispatchedMeshlets"] = selectionSummary.value("dispatchedMeshlets", 0u) + view.dispatchedMeshlets;
				hiZValidation["trusted"] = hiZValidation.value("trusted", false) || view.hiZHistoryValid;
				hiZValidation["inactiveReason"] = view.hiZInvalidReason.empty() ? std::string("none") : view.hiZInvalidReason;
				hiZValidation["culledByHiZ"] = hiZValidation.value("culledByHiZ", 0u) + view.culledByHiZ;
			}
		}

		const std::filesystem::path relative = std::filesystem::relative(absolutePath, editorContext->assetDatabase->GetAssetRoot());
		VirtualGeometryClusterSummaryMetadata clusterSummary;
		editorContext->assetDatabase->TryGetVirtualGeometryClusterSummary(*metadata, clusterSummary);
		VirtualGeometryPageSummaryMetadata pageSummary;
		editorContext->assetDatabase->TryGetVirtualGeometryPageSummary(*metadata, pageSummary);
		nlohmann::json data = {
			{"assetId", TerraEngine::ToHexString(metadata->guid)},
			{"sourceKey", relative.generic_string()},
			{"compiledPath", metadata->compiledPath.generic_string()},
			{"virtualGeometryStatus", readable ? cookMetadata.value("virtualGeometryStatus", "unreadable") : "unreadable"},
			{"detailLevels", readable ? cookMetadata.value("detailLevelCount", 0u) : 0u},
			{"sourceTriangleCount", readable ? cookMetadata.value("sourceTriangleCount", 0u) : 0u},
			{"meshletCounts", readable ? cookMetadata["meshletCounts"] : nlohmann::json{{"resident", 0u}}},
			{"boundsStatus", readable ? cookMetadata.value("boundsStatus", "unknown") : "unknown"},
			{"settings", SerializeVirtualGeometrySettings(settingsMetadata)},
			{"cookDiagnostics", SerializeVirtualGeometryCookDiagnostics(detailDiagnostics)},
			{"clusterSummary", SerializeVirtualGeometryClusterSummary(clusterSummary)},
			{"pageResidencySummary", SerializeVirtualGeometryPageSummary(pageSummary)},
			{"selectionSummary", selectionSummary},
			{"hiZValidation", hiZValidation},
			{"staleReasons", nlohmann::json::array()},
			{"cookWarnings", readable ? cookMetadata["cookWarnings"] : nlohmann::json::array()},
			{"cookMetadata", cookMetadata},
			{"runtimeDiagnostics", runtimeDiagnostics},
			{"residencyDiagnostics", residencyDiagnostics},
			{"assetRegistrationBoundary", "asset-database-finish-import"}
		};
		if (TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata->importerName))
		{
			const std::filesystem::path resultPath = TerraEngine::AssetPipeline::GetImportResultPath(absolutePath);
			if (std::filesystem::exists(resultPath))
			{
				std::ifstream input(resultPath);
				nlohmann::json importResult = nlohmann::json::object();
				input >> importResult;
				data["packageVirtualGeometry"] = BuildPackageVirtualGeometryAutomationSummary(importResult);
			}
		}
		return BuildSuccess(requestId, "Inspected virtual geometry asset.", std::move(data));
	}

	nlohmann::json EditorAutomationService::HandleAssetPrepareDelete(const nlohmann::json& arguments, const std::string& requestId)
	{
		nlohmann::json validation = ValidateAssetPath(arguments, requestId);
		if (!validation.value("ok", false)) return validation;
		return BuildSuccess(requestId, "Prepared asset delete approval.", {{"path", validation["path"]}, {"requiresApproval", true}});
	}

	nlohmann::json EditorAutomationService::HandleExportValidate(const nlohmann::json& arguments, const std::string& requestId)
	{
		const std::string profile = arguments.value("profile", "");
		if (profile.empty())
		{
			return BuildError(requestId, ErrorCategory::Validation, "export.validate requires a profile name.");
		}
		const std::filesystem::path projectRoot = editorContext->projectManager->GetProjectRoot();
		std::optional<BuildProfile> buildProfile = FindBuildProfile(projectRoot, profile);
		if (!buildProfile)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Build profile was not found.", profile);
		}
		nlohmann::json data = SerializeExportValidation(ExportValidator{}.ValidateAll(*buildProfile, projectRoot));
		data["profile"] = buildProfile->name;
		data["profileId"] = buildProfile->id;
		return BuildSuccess(requestId, "Validated export profile.", std::move(data));
	}

	nlohmann::json EditorAutomationService::HandleExportStart(const nlohmann::json& arguments, const std::string& requestId)
	{
		const std::string profile = arguments.value("profile", "");
		if (profile.empty())
		{
			return BuildError(requestId, ErrorCategory::Validation, "export.start requires a profile name.");
		}
		nlohmann::json validation = HandleExportValidate(arguments, requestId);
		if (!validation.value("ok", false))
		{
			return validation;
		}
		ExportJob job;
		job.jobId = requestId + "-export";
		job.profile = validation["data"].value("profile", profile);
		job.state = validation["data"].value("canExport", false) ? "queued" : "blocked";
		job.progress = job.state == "queued" ? 0.05f : 0.0f;
		exportJobs.insert(exportJobs.begin(), job);
		if (exportJobs.size() > 16)
		{
			exportJobs.pop_back();
		}
		if (activityCallback)
		{
			activityCallback(ActivityEventType::Export, requestId, "export.start", "Created export job " + job.jobId + ".");
		}
		return BuildSuccess(requestId, "Started export job.", {{"jobId", job.jobId}, {"profile", job.profile}, {"progress", job.progress}, {"state", job.state}});
	}

	nlohmann::json EditorAutomationService::HandleExportCancel(const nlohmann::json& arguments, const std::string& requestId)
	{
		const std::string jobId = arguments.value("jobId", "");
		if (jobId.empty())
		{
			return BuildError(requestId, ErrorCategory::Validation, "export.cancel requires a jobId.");
		}
		for (ExportJob& job : exportJobs)
		{
			if (job.jobId == jobId)
			{
				job.cancelRequested = true;
				job.state = "canceled";
				job.progress = 0.0f;
				if (activityCallback)
				{
					activityCallback(ActivityEventType::Export, requestId, "export.cancel", "Canceled export job " + jobId + ".");
				}
				return BuildSuccess(requestId, "Canceled export job.", {{"jobId", jobId}, {"state", job.state}});
			}
		}
		return BuildError(requestId, ErrorCategory::MissingTarget, "Export job was not found.", jobId);
	}

	nlohmann::json EditorAutomationService::HandleGameViewCapture(const nlohmann::json& arguments, const std::string& requestId)
	{
		const nlohmann::json validation = ValidateCaptureArguments(arguments, requestId);
		if (!validation.value("ok", false))
		{
			return validation;
		}
		const glm::uvec2 size(validation["width"].get<std::uint32_t>(), validation["height"].get<std::uint32_t>());
		TerraEngine::Renderer::RenderCaptureRequest request;
		request.requestId = requestId;
		request.sourceKind = TerraEngine::Renderer::RenderCaptureSourceKind::GameView;
		request.sourceName = "Game View";
		request.targetSize = size;
		request.format = validation["format"].get<std::string>();
		request.freshness = validation["freshness"].get<std::string>();
		request.includeOverlays = validation["includeOverlays"].get<bool>();
		request.transparentBackground = validation["transparentBackground"].get<bool>();
		request.allowBackendFallback = validation["allowBackendFallback"].get<bool>();
		request.timeoutMs = validation["timeoutMs"].get<std::uint32_t>();

		const TerraEngine::Renderer::RenderCaptureResult capture = RenderSystem::Instance().CaptureImage(request);
		if (!capture.ok)
		{
			return BuildError(requestId, MapCaptureFailureCategory(capture.errorCategory), capture.message);
		}
		return BuildCaptureSuccessEnvelope(*this, requestId, capture);
	}

	nlohmann::json EditorAutomationService::HandleCameraObjectCapture(const nlohmann::json& arguments, const std::string& requestId)
	{
		const nlohmann::json targetValue = arguments.contains("object")
			? arguments["object"]
			: (arguments.contains("camera") ? arguments["camera"] : arguments.value("target", nlohmann::json()));
		if (targetValue.is_string())
		{
			const std::string targetName = targetValue.get<std::string>();
			if (!TerraEngine::ParseUUID(targetName))
			{
				std::size_t matchingNameCount = 0u;
				for (const Object& object : ObjectManager::instance().getObjectsWithComponentsIncludingInactive<Transform>())
				{
					if (ObjectManager::instance().getObjectName(object) == targetName)
					{
						++matchingNameCount;
					}
				}
				if (matchingNameCount > 1u)
				{
					return BuildError(requestId, ErrorCategory::Conflict, "Camera object target name is ambiguous; use a UUID or entity id.", targetName);
				}
			}
		}
		const EditorTarget target = ResolveTarget(targetValue);
		if (!target.resolved)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Camera object was not found.", target.input);
		}
		const nlohmann::json validation = ValidateCaptureArguments(arguments, requestId);
		if (!validation.value("ok", false))
		{
			return validation;
		}
		const glm::uvec2 size(validation["width"].get<std::uint32_t>(), validation["height"].get<std::uint32_t>());

		ObjectManager& objectManager = ObjectManager::instance();
		const Object object = objectManager.makeObjectHandle(*target.entity);
		const bool includeDisabled = arguments.value("includeDisabled", true);
		const std::optional<AuthoredCameraCaptureSnapshot> authoredCamera = ReadAuthoredCameraCaptureSnapshot(objectManager, object);
		if (!authoredCamera)
		{
			return BuildError(requestId, ErrorCategory::InvalidCameraObject, "Target object does not have both Transform and CameraComponent.", target.displayName);
		}
		if (!includeDisabled && (!authoredCamera->objectActive || !authoredCamera->cameraComponentActive))
		{
			return BuildError(requestId, ErrorCategory::InvalidCameraObject, "Camera object is disabled and includeDisabled is false.", target.displayName);
		}
		if (!std::isfinite(authoredCamera->camera.verticalFovDegrees)
			|| !std::isfinite(authoredCamera->camera.aspectRatio)
			|| !std::isfinite(authoredCamera->camera.nearPlane)
			|| !std::isfinite(authoredCamera->camera.farPlane)
			|| authoredCamera->camera.verticalFovDegrees <= 0.0f
			|| authoredCamera->camera.verticalFovDegrees >= 179.0f
			|| authoredCamera->camera.aspectRatio <= 0.0f
			|| authoredCamera->camera.nearPlane <= 0.0f
			|| authoredCamera->camera.farPlane <= authoredCamera->camera.nearPlane)
		{
			return BuildError(requestId, ErrorCategory::InvalidCameraObject, "Camera object projection values are invalid.", target.displayName);
		}

		TerraEngine::Renderer::RenderCaptureRequest request = BuildCaptureRequestFromCamera(
			TerraEngine::Renderer::RenderCaptureSourceKind::CameraObject,
			authoredCamera->name,
			authoredCamera->transform,
			authoredCamera->camera,
			size,
			validation["transparentBackground"].get<bool>());
		request.requestId = requestId;
		request.format = validation["format"].get<std::string>();
		request.freshness = validation["freshness"].get<std::string>();
		request.includeOverlays = validation["includeOverlays"].get<bool>();
		request.allowBackendFallback = validation["allowBackendFallback"].get<bool>();
		request.timeoutMs = validation["timeoutMs"].get<std::uint32_t>();
		request.sourceObjectId = authoredCamera->objectId;
		const TerraEngine::Renderer::RenderCaptureResult capture = RenderSystem::Instance().CaptureImage(request);
		if (!capture.ok)
		{
			return BuildError(requestId, MapCaptureFailureCategory(capture.errorCategory), capture.message);
		}
		nlohmann::json result = BuildCaptureSuccessEnvelope(*this, requestId, capture);
		nlohmann::json cameraObject = {
			{"entity", static_cast<std::uint32_t>(*target.entity)},
			{"objectId", TerraEngine::ToHexString(authoredCamera->objectId)},
			{"displayName", authoredCamera->name},
			{"objectActive", authoredCamera->objectActive},
			{"cameraComponentActive", authoredCamera->cameraComponentActive},
			{"includeDisabled", includeDisabled},
			{"disabled", !authoredCamera->objectActive || !authoredCamera->cameraComponentActive}
		};
		result["data"]["source"]["cameraObject"] = cameraObject;
		result["data"]["source"]["objectId"] = cameraObject["objectId"];
		result["data"]["source"]["displayName"] = authoredCamera->name;
		result["data"]["source"]["disabled"] = cameraObject["disabled"];
		result["data"]["object"] = std::move(cameraObject);
		return result;
	}

	nlohmann::json EditorAutomationService::HandleCustomCameraCapture(const nlohmann::json& arguments, const std::string& requestId)
	{
		const nlohmann::json validation = ValidateCaptureArguments(arguments, requestId);
		if (!validation.value("ok", false))
		{
			return validation;
		}
		const std::optional<glm::vec3> position = ReadVec3(arguments.value("position", nlohmann::json()));
		if (!position)
		{
			return BuildError(requestId, ErrorCategory::Validation, "camera.capture_custom requires position: [x, y, z].");
		}

		glm::vec3 up = ReadVec3(arguments.value("up", nlohmann::json())).value_or(glm::vec3(0.0f, 1.0f, 0.0f));
		if (glm::length(up) < 0.0001f)
		{
			return BuildError(requestId, ErrorCategory::Validation, "Custom camera up vector must be non-zero.");
		}
		glm::vec3 forward(0.0f, 0.0f, -1.0f);
		if (const std::optional<glm::vec3> targetPoint = ReadVec3(arguments.value("target", nlohmann::json())))
		{
			forward = *targetPoint - *position;
		}
		else if (const std::optional<glm::vec3> requestedForward = ReadVec3(arguments.value("forward", nlohmann::json())))
		{
			forward = *requestedForward;
		}
		if (glm::length(forward) < 0.0001f)
		{
			return BuildError(requestId, ErrorCategory::Validation, "Custom camera forward direction must be non-zero.");
		}

		const glm::uvec2 size(validation["width"].get<std::uint32_t>(), validation["height"].get<std::uint32_t>());
		std::string projection = "perspective";
		float fov = 45.0f;
		float aspect = static_cast<float>(size.x) / static_cast<float>(size.y);
		float orthographicHeight = 10.0f;
		float nearPlane = 0.1f;
		float farPlane = 1000.0f;
		try
		{
			projection = arguments.value("projection", projection);
			fov = arguments.value("verticalFovDegrees", fov);
			aspect = arguments.value("aspectRatio", aspect);
			orthographicHeight = arguments.value("orthographicHeight", orthographicHeight);
			nearPlane = arguments.value("nearPlane", nearPlane);
			farPlane = arguments.value("farPlane", farPlane);
		}
		catch (const nlohmann::json::exception&)
		{
			return BuildError(requestId, ErrorCategory::Validation, "Custom camera projection option types are invalid.");
		}
		if (projection != "perspective" && projection != "orthographic")
		{
			return BuildError(requestId, ErrorCategory::Validation, "Custom camera projection must be perspective or orthographic.");
		}
		if (!std::isfinite(fov) || !std::isfinite(aspect) || !std::isfinite(orthographicHeight) || !std::isfinite(nearPlane) || !std::isfinite(farPlane))
		{
			return BuildError(requestId, ErrorCategory::Validation, "Custom camera projection values must be finite.");
		}
		if (nearPlane <= 0.0f || farPlane <= nearPlane || aspect <= 0.0f)
		{
			return BuildError(requestId, ErrorCategory::Validation, "Custom camera projection values are invalid.");
		}
		if ((projection == "perspective" && (fov <= 0.0f || fov >= 179.0f)) || (projection == "orthographic" && orthographicHeight <= 0.0f))
		{
			return BuildError(requestId, ErrorCategory::Validation, "Custom camera projection values are invalid.");
		}

		TerraEngine::Renderer::RenderCaptureRequest request;
		request.requestId = requestId;
		request.sourceKind = TerraEngine::Renderer::RenderCaptureSourceKind::CustomCamera;
		request.sourceName = arguments.value("name", std::string("Custom Camera"));
		request.targetSize = size;
		request.format = validation["format"].get<std::string>();
		request.freshness = validation["freshness"].get<std::string>();
		request.includeOverlays = validation["includeOverlays"].get<bool>();
		request.transparentBackground = validation["transparentBackground"].get<bool>();
		request.allowBackendFallback = validation["allowBackendFallback"].get<bool>();
		request.timeoutMs = validation["timeoutMs"].get<std::uint32_t>();
		request.cameraWorldPosition = *position;
		request.viewMatrix = glm::lookAt(*position, *position + glm::normalize(forward), glm::normalize(up));
		if (projection == "orthographic")
		{
			const float halfHeight = orthographicHeight * 0.5f;
			const float halfWidth = halfHeight * aspect;
			request.projectionMatrix = glm::orthoRH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight, nearPlane, farPlane);
		}
		else
		{
			request.projectionMatrix = glm::perspectiveZO(glm::radians(fov), aspect, nearPlane, farPlane);
		}
		request.projectionMatrix[1][1] *= -1.0f;

		const TerraEngine::Renderer::RenderCaptureResult capture = RenderSystem::Instance().CaptureImage(request);
		if (!capture.ok)
		{
			return BuildError(requestId, MapCaptureFailureCategory(capture.errorCategory), capture.message);
		}
		nlohmann::json result = BuildCaptureSuccessEnvelope(*this, requestId, capture);
		result["data"]["camera"] = {
			{"position", SerializeVec3(*position)},
			{"forward", SerializeVec3(glm::normalize(forward))},
			{"up", SerializeVec3(glm::normalize(up))},
			{"projection", projection},
			{"verticalFovDegrees", fov},
			{"orthographicHeight", orthographicHeight},
			{"aspectRatio", aspect},
			{"nearPlane", nearPlane},
			{"farPlane", farPlane}
		};
		return result;
	}

	nlohmann::json EditorAutomationService::HandleDiagnosticsGetSummary(const nlohmann::json& arguments, const std::string& requestId)
	{
		return BuildSuccess(requestId, "Read diagnostics summary.", BuildDiagnosticsSummary(arguments.value("includeDetails", false)));
	}

	void EditorAutomationService::MarkSceneMutated(ActivityEventType type, const std::string& requestId, const std::string& capability, const std::string& message) const
	{
		// Scene mutations are serialized through the editor-facing service so
		// ObjectManager callbacks, dirty-state tracking, and editor selection
		// all observe the change in the same order as user-driven edits.
		SceneManager::Instance().ForceMarkActiveSceneDirty();
		if (activityCallback)
		{
			activityCallback(type, requestId, capability, message);
		}
	}
}
