// EditorAutomationService.cpp - Contract implementation for editor automation.
//
// This file keeps assistant-facing tool behavior in one place. Transport code
// supplies JSON, bridge code supplies policy, and this service performs the
// editor validation/action work against TerraEngine's existing systems.

#include "EditorAutomationService.h"
#include "EditorAutomationTransaction.h"

#include "Providers/AssetPrefabAutomationProvider.h"
#include "Providers/AuthoringAutomationProvider.h"
#include "Providers/EditorDiagnosticsAutomationProvider.h"
#include "Providers/HistoryPlayModeAutomationProvider.h"
#include "Providers/ProjectSceneAutomationProvider.h"
#include "Providers/PhysicsAutomationProvider.h"
#include "Providers/ScriptBuildAutomationProvider.h"

#include "../Assets/AssetImportResult.h"
#include "../Assets/AssetDatabase.h"
#include "../Core/EditorContext.h"
#include "../Project/ProjectManager.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Subsystems/ScriptCompilationSubsystem.h"
#include "../Windows/SceneViewWindow.h"
#include "../Windows/SceneViewOverlayTypes.h"
#include "../Profiling/EditorProfilerService.h"
#include "../../Export/BuildProfile.h"
#include "../../Export/ExportPipeline.h"
#include "../../Export/ExportValidator.h"
#include "../../AssetCompilers/ModelPackageImporter.h"
#include "../../AssetCompilers/PhysicsMaterialAssetWriter.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Core/Object/Component.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/Physics/CharacterControllerComponent.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/SceneMutationQueue.h"
#include "Engine/Scene/RuntimeLayout.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Core/Scheduling/FrameWorkload.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Renderer/RendererRecovery.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Physics/Assets/ColliderRecipe.h"
#include "Engine/Physics/Assets/CookedColliderAsset.h"
#include "Engine/Physics/Assets/PhysicsMaterial.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Utilities/UUID.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <unordered_set>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
#elif defined(__linux__)
	#include <unistd.h>
#endif

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		using AutomationOperationId = std::uint64_t;

		constexpr AutomationOperationId AutomationOperation(std::string_view name)
		{
			// FNV-1a provides a stable process-independent operation token. Provider
			// registration checks collisions before any handler becomes discoverable.
			AutomationOperationId hash = 14695981039346656037ull;
			for (const unsigned char character : name)
			{
				hash ^= character;
				hash *= 1099511628211ull;
			}
			return hash;
		}

		bool IsOperationOneOf(AutomationOperationId operation, std::initializer_list<AutomationOperationId> candidates)
		{
			return std::find(candidates.begin(), candidates.end(), operation) != candidates.end();
		}

		std::filesystem::path ResolveAutomationEditorExecutable()
		{
			// Export runtime discovery is based on the installed executable rather
			// than the process working directory, which may be the user's project.
#ifdef _WIN32
			std::wstring buffer(32768, L'\0');
			const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length > 0 && length < buffer.size())
			{
				buffer.resize(length);
				return std::filesystem::path(buffer);
			}
#elif defined(__linux__)
			std::array<char, 4096> buffer{};
			const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
			if (length > 0)
				return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#endif
			return std::filesystem::absolute("TerraEngine-Editor");
		}

		std::string GenerateAutomationExportRunId()
		{
			const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			return "mcp-editor-" + std::to_string(ticks);
		}

		AutomationLogSeverity ExportAutomationSeverity(ExportDiagnosticSeverity severity)
		{
			switch (severity)
			{
			case ExportDiagnosticSeverity::Warning: return AutomationLogSeverity::Warning;
			case ExportDiagnosticSeverity::Error: return AutomationLogSeverity::Error;
			default: return AutomationLogSeverity::Info;
			}
		}

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

		nlohmann::json SerializeRendererRecoverySnapshot(const TerraEngine::Renderer::RendererRecoverySnapshot& snapshot)
		{
			// Automation clients should not depend on rendered overlays. Keep the
			// recovery contract as plain JSON strings and counters that stay valid
			// even when Vulkan/ImGui output is unavailable.
			return {
				{"renderSessionGeneration", snapshot.renderSessionGeneration},
				{"state", TerraEngine::Renderer::ToString(snapshot.state)},
				{"incidentId", snapshot.incidentId},
				{"sessionId", snapshot.sessionId},
				{"message", snapshot.message},
				{"firstFailureStage", TerraEngine::Renderer::ToString(snapshot.firstFailureStage)},
				{"firstFailureResult", snapshot.firstFailureResult},
				{"affectedView", TerraEngine::Renderer::ToString(snapshot.affectedView)},
				{"affectedResourceId", snapshot.affectedResourceId},
				{"affectedResourceName", snapshot.affectedResourceName},
				{"hardwareName", snapshot.hardwareName},
				{"driverVersion", snapshot.driverVersion},
				{"apiVersion", snapshot.apiVersion},
				{"frameId", snapshot.frameId},
				{"duplicateFailureCount", snapshot.duplicateFailureCount},
				{"canSaveProjectData", snapshot.canSaveProjectData},
				{"finalOutcome", TerraEngine::Renderer::ToString(snapshot.finalOutcome)},
				{"lastError", snapshot.lastError},
				{"fatalDialogReportId", snapshot.fatalDialogReportId},
				{"crashDumpPath", snapshot.crashDumpPath},
				{"crashSummarySidecarPath", snapshot.crashSummarySidecarPath},
				{"crashDumpStatus", snapshot.crashDumpStatus}
			};
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

		bool IsManagedQualifiedName(std::string_view value, bool allowDots)
		{
			if (value.empty()) return false;
			bool segmentStart = true;
			for (unsigned char character : value)
			{
				if (allowDots && character == '.')
				{
					if (segmentStart) return false;
					segmentStart = true;
					continue;
				}
				if (segmentStart ? !(std::isalpha(character) || character == '_') : !(std::isalnum(character) || character == '_')) return false;
				segmentStart = false;
			}
			return !segmentStart;
		}

		std::string StableFingerprint(std::string_view value)
		{
			std::uint64_t hash = 1469598103934665603ull;
			for (unsigned char byte : value) { hash ^= byte; hash *= 1099511628211ull; }
			std::ostringstream stream;
			stream << std::hex << hash;
			return stream.str();
		}

		std::string PhysicsImpactScope(std::string_view capability)
		{
			if (capability.starts_with("physics.settings.")) return "physics.settings";
			if (capability.starts_with("physics.layer.")) return "physics.layer";
			if (capability.starts_with("physics.material.")) return "physics.material";
			if (capability.starts_with("physics.collider.")) return "physics.collider";
			return std::string(capability);
		}

		nlohmann::json PhysicsImpactPayload(nlohmann::json arguments)
		{
			for (const char* field : {"approval", "correlationId", "idempotencyKey", "worldGeneration", "projectGeneration", "sceneGeneration"})
			{
				arguments.erase(field);
			}
			return arguments;
		}

		nlohmann::json PhysicsIdempotencyPayload(nlohmann::json arguments)
		{
			// Approval tokens and correlation IDs identify a transport attempt, not
			// the physical operation. Keep all generations so a retry can never
			// replay work into a recreated project, scene, or physics world.
			for (const char* field : {"approval", "correlationId", "idempotencyKey"})
			{
				arguments.erase(field);
			}
			return arguments;
		}

		std::string PhysicsImpactFingerprint(
			std::string_view capability,
			const nlohmann::json& arguments,
			const AutomationStateSnapshot& state)
		{
			return StableFingerprint(
				PhysicsImpactScope(capability) + "|" + PhysicsImpactPayload(arguments).dump()
				+ "|" + std::to_string(state.projectGeneration) + "|" + std::to_string(state.sceneGeneration));
		}

		nlohmann::json PhysicsVectorJson(TerraEngine::Physics::PhysicsVector3 value)
		{
			return nlohmann::json::array({value.x, value.y, value.z});
		}

		std::optional<TerraEngine::Physics::PhysicsVector3> ReadPhysicsVector(const nlohmann::json& value)
		{
			if (!value.is_array() || value.size() != 3 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number())
			{
				return std::nullopt;
			}
			const TerraEngine::Physics::PhysicsVector3 result{
				value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
			if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
			{
				return std::nullopt;
			}
			return result;
		}

		std::optional<float> ReadBoundedPhysicsNumber(const nlohmann::json& value, float minimum, float maximum)
		{
			if (!value.is_number()) return std::nullopt;
			const double parsed = value.get<double>();
			if (!std::isfinite(parsed) || parsed < static_cast<double>(minimum) || parsed > static_cast<double>(maximum))
				return std::nullopt;
			return static_cast<float>(parsed);
		}

		nlohmann::json PhysicsSettingsJson(const TerraEngine::Physics::PhysicsSettings& settings)
		{
			nlohmann::json layers = nlohmann::json::array();
			for (const TerraEngine::Physics::CollisionLayer& layer : settings.layers)
			{
				layers.push_back({
					{"id", layer.id}, {"name", layer.name}, {"collisionMask", layer.collisionMask},
					{"broadPhaseGroup", layer.broadPhaseGroup}, {"enabled", layer.enabled}
				});
			}
			return {
				{"schemaVersion", settings.schemaVersion}, {"gravity", PhysicsVectorJson(settings.gravity)},
				{"fixedTimeStep", settings.fixedTimeStep}, {"maxSubsteps", settings.maxSubsteps},
				{"maxAccumulatedTime", settings.maxAccumulatedTime}, {"velocityIterations", settings.velocityIterations},
				{"positionIterations", settings.positionIterations}, {"layers", std::move(layers)},
				{"maxCastDistance", settings.maxCastDistance}, {"maxShapeExtent", settings.maxShapeExtent},
				{"maxHitsPerQuery", settings.maxHitsPerQuery}, {"maxRequestsPerBatch", settings.maxRequestsPerBatch},
				{"retainedResultTicks", settings.retainedResultTicks}, {"debugVisualizationEnabled", settings.debugVisualizationEnabled},
				{"retainedDebugTicks", settings.retainedDebugTicks}, {"maxDebugPrimitives", settings.maxDebugPrimitives}
			};
		}

		bool ApplyPhysicsSettingsPatch(
			const nlohmann::json& patch,
			TerraEngine::Physics::PhysicsSettings& settings,
			std::string& error)
		{
			if (!patch.is_object())
			{
				error = "Physics settings patch must be an object.";
				return false;
			}
			if (patch.contains("gravity"))
			{
				const auto value = ReadPhysicsVector(patch["gravity"]);
				if (!value) { error = "gravity must contain three finite numbers."; return false; }
				settings.gravity = *value;
			}
			if (patch.contains("fixedTimeStep"))
			{
				const auto value = ReadBoundedPhysicsNumber(patch["fixedTimeStep"], 0.0001F, 1.0F);
				if (!value) { error = "fixedTimeStep must be a finite number between 0.0001 and 1.0."; return false; }
				settings.fixedTimeStep = *value;
			}
			const auto readBoundedUnsigned = [&patch, &error](std::string_view key, std::uint32_t minimum, std::uint32_t maximum, std::uint32_t& output) {
				if (!patch.contains(std::string(key))) return true;
				const nlohmann::json& value = patch[std::string(key)];
				if (!value.is_number_unsigned() || value.get<std::uint64_t>() < minimum || value.get<std::uint64_t>() > maximum)
				{
					error = std::string(key) + " is outside its supported unsigned range.";
					return false;
				}
				output = value.get<std::uint32_t>();
				return true;
			};
			if (!readBoundedUnsigned("maxSubsteps", 1U, 16U, settings.maxSubsteps)
				|| !readBoundedUnsigned("retainedDebugTicks", 1U, 32U, settings.retainedDebugTicks)
				|| !readBoundedUnsigned("maxDebugPrimitives", 1U, 100000U, settings.maxDebugPrimitives)) return false;
			if (patch.contains("debugVisualizationEnabled"))
			{
				if (!patch["debugVisualizationEnabled"].is_boolean()) { error = "debugVisualizationEnabled must be boolean."; return false; }
				settings.debugVisualizationEnabled = patch["debugVisualizationEnabled"].get<bool>();
			}
			const auto validation = settings.Validate();
			if (!validation.accepted)
			{
				error = validation.message;
				return false;
			}
			return true;
		}

		nlohmann::json PhysicsSnapshotResult(
			const AutomationRequestContext& request,
			TerraEngine::Physics::PhysicsWorldHandle world,
			TerraEngine::Physics::PhysicsTick tick,
			nlohmann::json data,
			bool truncated = false,
			bool overflow = false)
		{
			return {
				{"correlationId", request.correlationId}, {"worldGeneration", world.worldGeneration}, {"tick", tick},
				{"truncated", truncated}, {"overflow", overflow}, {"diagnostics", nlohmann::json::array()},
				{"data", std::move(data)}
			};
		}

		std::string ValidateRequiredSchemaFields(const nlohmann::json& arguments, const nlohmann::json& schema)
		{
			if (!arguments.is_object()) return "Tool arguments must be a JSON object.";
			if (!schema.is_object() || !schema.contains("required") || !schema["required"].is_array()) return {};
			for (const nlohmann::json& required : schema["required"])
			{
				if (required.is_string() && !arguments.contains(required.get<std::string>())) return "Missing required argument: " + required.get<std::string>();
			}
			return {};
		}

		const char* PermissionJson(AutomationPermission permission)
		{
			switch (permission)
			{
			case AutomationPermission::Read: return "read";
			case AutomationPermission::ProjectWrite: return "project_write";
			case AutomationPermission::GlobalWrite: return "global_write";
			}
			return "read";
		}

		const char* ApprovalPolicyJson(AutomationApprovalPolicy policy)
		{
			switch (policy)
			{
			case AutomationApprovalPolicy::None: return "none";
			case AutomationApprovalPolicy::Destructive: return "destructive";
			case AutomationApprovalPolicy::HighImpact: return "high_impact";
			}
			return "none";
		}

		const char* JobStateJson(AutomationJobState state)
		{
			switch (state)
			{
			case AutomationJobState::Queued: return "queued";
			case AutomationJobState::Preparing: return "preparing";
			case AutomationJobState::Running: return "running";
			case AutomationJobState::CancelRequested: return "cancel_requested";
			case AutomationJobState::Succeeded: return "succeeded";
			case AutomationJobState::Failed: return "failed";
			case AutomationJobState::Canceled: return "canceled";
			}
			return "failed";
		}

		nlohmann::json SerializeAutomationJob(const AutomationJobSnapshot& job)
		{
			nlohmann::json artifacts = nlohmann::json::array();
			for (const AutomationArtifact& artifact : job.artifacts)
				artifacts.push_back({{"artifactId", artifact.artifactId}, {"kind", artifact.kind}, {"displayPath", artifact.displayPath}, {"mimeType", artifact.mimeType}, {"metadata", artifact.metadata}});
			nlohmann::json diagnostics = nlohmann::json::array();
			for (const AutomationJobDiagnostic& diagnostic : job.diagnostics)
				diagnostics.push_back({{"category", diagnostic.category}, {"message", diagnostic.message}, {"details", diagnostic.details}});
			return {{"jobId", job.jobId}, {"kind", job.kind}, {"capability", job.capability}, {"requestId", job.requestId},
				{"correlationId", job.correlationId}, {"state", JobStateJson(job.state)}, {"progress", job.progress}, {"stage", job.stage},
				{"cancellable", job.cancellable}, {"cancelRequested", job.cancelRequested}, {"artifacts", artifacts}, {"diagnostics", diagnostics}, {"result", job.result}};
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
				{"pageStreamingReady", metadata.pageStreamingReady},
				{"smallMeshStreamingSkipped", metadata.smallMeshStreamingSkipped},
				{"skippedStreamingReason", metadata.skippedStreamingReason},
				{"pageIntegrityErrors", metadata.pageIntegrityErrors},
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
					{"meshletCount", diagnostic.meshletCount},
					{"message", diagnostic.message}
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

		std::optional<nlohmann::json> LoadPackageImportResultJson(const std::filesystem::path& packagePath)
		{
			const std::filesystem::path resultPath = TerraEngine::AssetPipeline::GetImportResultPath(packagePath);
			if (!std::filesystem::exists(resultPath))
			{
				return std::nullopt;
			}

			std::ifstream input(resultPath);
			if (!input)
			{
				return std::nullopt;
			}

			try
			{
				nlohmann::json result = nlohmann::json::object();
				input >> result;
				return result.is_object() ? std::optional<nlohmann::json>(std::move(result)) : std::nullopt;
			}
			catch (const nlohmann::json::exception&)
			{
				return std::nullopt;
			}
		}

		nlohmann::json BuildPackageVirtualGeometryInspectionAggregate(const nlohmann::json& importResult)
		{
			const nlohmann::json packageSummary = BuildPackageVirtualGeometryAutomationSummary(importResult);
			const nlohmann::json generatedMeshes = packageSummary.value("generatedMeshes", nlohmann::json::array());
			const std::uint32_t usableMeshCount = packageSummary.value("usableGeneratedMeshCount", 0u);
			const std::uint32_t failedMeshCount = packageSummary.value("failedGeneratedMeshCount", 0u);
			std::uint32_t detailLevelCount = 0u;
			std::uint32_t sourceTriangleCount = 0u;
			std::uint32_t meshletCount = 0u;
			std::uint32_t requestedTargetCount = 0u;
			std::uint32_t generatedLevelCount = 0u;
			std::uint32_t skippedDetailCount = 0u;
			std::uint32_t clusterCount = 0u;
			std::uint32_t materialSectionCount = 0u;
			std::uint32_t clusterMaterialSectionCount = 0u;
			std::uint32_t pageCount = 0u;
			bool fallbackCoverageValid = !generatedMeshes.empty();
			bool materialCoverageValid = !generatedMeshes.empty();
			nlohmann::json requestedTargets = nlohmann::json::array();
			nlohmann::json warnings = nlohmann::json::array();
			nlohmann::json settings = nlohmann::json::object();

			for (const nlohmann::json& mesh : importResult.value("generatedMeshes", nlohmann::json::array()))
			{
				const nlohmann::json virtualGeometry = mesh.value("virtualGeometry", nlohmann::json::object());
				detailLevelCount = std::max(detailLevelCount, virtualGeometry.value("detailLevels", 0u));
				sourceTriangleCount += virtualGeometry.value("sourceTriangles", 0u);
				meshletCount += virtualGeometry.value("meshlets", 0u);
				if (settings.empty())
				{
					settings = virtualGeometry.value("settings", nlohmann::json::object());
				}

				for (const nlohmann::json& target : virtualGeometry.value("targets", nlohmann::json::array()))
				{
					nlohmann::json row = target;
					row["meshGuid"] = mesh.value("guid", std::string());
					row["meshName"] = mesh.value("name", mesh.value("displayName", std::string()));
					const std::string status = target.value("status", std::string());
					generatedLevelCount += status == "Generated" ? 1u : 0u;
					skippedDetailCount += (status == "Skipped" || status == "Merged" || status == "Rejected") ? 1u : 0u;
					requestedTargets.push_back(std::move(row));
				}

				const nlohmann::json clusterSummary = virtualGeometry.value("clusterSummary", nlohmann::json::object());
				clusterCount += clusterSummary.value("clusterCount", 0u);
				materialSectionCount += clusterSummary.value("materialSectionCount", 0u);
				clusterMaterialSectionCount += clusterSummary.value("clusterMaterialSectionCount", 0u);
				pageCount += clusterSummary.value("pageLinkCount", 0u);
				fallbackCoverageValid = fallbackCoverageValid && clusterSummary.value("fallbackCoverageValid", false);
				materialCoverageValid = materialCoverageValid && clusterSummary.value("materialCoverageValid", false);

				for (const nlohmann::json& warning : virtualGeometry.value("warnings", nlohmann::json::array()))
				{
					warnings.push_back(warning);
				}
			}

			requestedTargetCount = static_cast<std::uint32_t>(requestedTargets.size());
			std::string status = "missing";
			if (usableMeshCount > 0u)
			{
				status = failedMeshCount == 0u ? "cooked" : "partial";
			}

			return {
				{"summary", packageSummary},
				{"virtualGeometryStatus", status},
				{"detailLevels", detailLevelCount},
				{"sourceTriangleCount", sourceTriangleCount},
				{"meshletCounts", {{"resident", meshletCount}}},
				{"settings", settings},
				{"cookDiagnostics", {
					{"requestedTargetCount", requestedTargetCount},
					{"generatedLevelCount", generatedLevelCount},
					{"skippedDetailCount", skippedDetailCount},
					{"mergedTargetCount", 0u},
					{"rejectedTargetCount", 0u},
					{"requestedTargets", requestedTargets}
				}},
				{"clusterSummary", {
					{"clusterCount", clusterCount},
					{"selectedClusterCount", 0u},
					{"fallbackClusterCount", 0u},
					{"materialSectionCount", materialSectionCount},
					{"clusterMaterialSectionCount", clusterMaterialSectionCount},
					{"fallbackCoverageValid", fallbackCoverageValid},
					{"materialCoverageValid", materialCoverageValid}
				}},
				{"pageResidencySummary", {
					{"pageCount", pageCount},
					{"residentPageCount", pageCount},
					{"pinnedFallbackPageCount", 0u},
					{"estimatedBytes", 0u},
					{"pageStreamingReady", pageCount > 0u && usableMeshCount > 0u},
					{"smallMeshStreamingSkipped", false},
					{"skippedStreamingReason", std::string()},
					{"integrityErrors", nlohmann::json::array()}
				}},
				{"cookWarnings", warnings},
				{"cookMetadata", {
					{"readable", true},
					{"hasVirtualGeometry", usableMeshCount > 0u},
					{"needsRebuild", false},
					{"virtualGeometryStatus", status},
					{"payloadGeneration", 0u},
					{"detailLevelCount", detailLevelCount},
					{"sourceTriangleCount", sourceTriangleCount},
					{"meshletCounts", {{"resident", meshletCount}}},
					{"boundsStatus", meshletCount > 0u ? "available" : "unknown"},
					{"pageStreamingReady", pageCount > 0u && usableMeshCount > 0u},
					{"smallMeshStreamingSkipped", false},
					{"skippedStreamingReason", std::string()},
					{"pageIntegrityErrors", nlohmann::json::array()},
					{"cookWarnings", warnings},
					{"rebuildGuidance", std::string()}
				}}
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

		const char* ToAutomationString(VirtualGeometryPerformanceGateStatus status)
		{
			switch (status)
			{
			case VirtualGeometryPerformanceGateStatus::Pass: return "pass";
			case VirtualGeometryPerformanceGateStatus::Fail: return "fail";
			case VirtualGeometryPerformanceGateStatus::Warning: return "warning";
			case VirtualGeometryPerformanceGateStatus::Unsupported: return "unsupported";
			default: return "unknown";
			}
		}

		const char* ToAutomationString(VirtualGeometryPerformanceGateSource source)
		{
			switch (source)
			{
			case VirtualGeometryPerformanceGateSource::Prepared: return "prepared";
			case VirtualGeometryPerformanceGateSource::GPUReadback: return "gpu-readback";
			case VirtualGeometryPerformanceGateSource::DelayedGPUReadback: return "delayed-gpu-readback";
			case VirtualGeometryPerformanceGateSource::Unsupported: return "unsupported";
			case VirtualGeometryPerformanceGateSource::Manual: return "manual";
			default: return "unknown";
			}
		}

		nlohmann::json SerializeVirtualGeometryPerformanceGate(const PerformanceGateResult& gate)
		{
			return {
				{"gateId", gate.gateId},
				{"fixtureId", gate.fixtureId},
				{"viewType", ToString(gate.viewType)},
				{"viewId", gate.viewId},
				{"assetId", TerraEngine::ToHexString(gate.assetId)},
				{"pageId", gate.pageId},
				{"metricName", gate.metricName},
				{"threshold", gate.threshold},
				{"actual", gate.actual},
				{"unit", gate.unit},
				{"status", ToAutomationString(gate.status)},
				{"source", ToAutomationString(gate.source)},
				{"diagnostic", gate.diagnostic}
			};
		}

		nlohmann::json SerializeVirtualGeometryRuntimeView(const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& view)
		{
			nlohmann::json performanceGates = nlohmann::json::array();
			for (const PerformanceGateResult& gate : view.virtualGeometryPerformanceGates)
			{
				performanceGates.push_back(SerializeVirtualGeometryPerformanceGate(gate));
			}
			nlohmann::json shadowSelections = nlohmann::json::array();
			for (const ShadowVisibilitySelection& shadow : view.shadowVisibilitySelections)
			{
				shadowSelections.push_back({
					{"shadowViewId", shadow.shadowViewId},
					{"lightId", TerraEngine::ToHexString(shadow.lightId)},
					{"frameIndex", shadow.frameIndex},
					{"candidatePackets", shadow.candidatePackets},
					{"selectedPackets", shadow.selectedPackets},
					{"rejectedByLightFrustum", shadow.rejectedByLightFrustum},
					{"selectedMeshlets", shadow.selectedMeshlets},
					{"dispatchedMeshlets", shadow.dispatchedMeshlets},
					{"fallbackReasons", shadow.fallbackReasons}
				});
			}
			return {
				{"viewType", ToString(view.viewType)},
				{"viewId", view.viewId},
				{"frameId", view.frameId},
				{"active", view.active},
				{"inactiveReason", ToString(view.inactiveReason)},
				{"sourceMeshlets", view.sourceMeshlets},
				{"selectedMeshlets", view.selectedMeshlets},
				{"dispatchedMeshlets", view.dispatchedMeshlets},
				{"culledMeshlets", view.culledMeshlets},
				{"hiZActive", view.hiZActive},
				{"hiZHistoryValid", view.hiZHistoryValid},
				{"counterSource", ToString(view.counterSource)},
				{"gpuPacketRecords", view.gpuPacketRecordCount},
				{"gpuBatchDescriptors", view.gpuBatchDescriptorCount},
				{"gpuPacketFrameSets", view.gpuPacketFrameSetCount},
				{"gpuCompactionDispatches", view.gpuCompactionDispatchCount},
				{"generatedSubmissions", view.generatedSubmissionCount},
				{"validGeneratedSubmissions", view.validGeneratedSubmissionCount},
				{"invalidGeneratedSubmissions", view.invalidGeneratedSubmissionCount},
				{"gpuCounterReadbacks", view.gpuCounterReadbackCount},
				{"gpuFallbackRecords", view.gpuFallbackRecordCount},
				{"gpuFallbackReason", view.gpuFallbackReason},
				{"residentPages", view.residentPages},
				{"requestedPages", view.requestedPages},
				{"missingPages", view.missingPages},
				{"failedPages", view.failedPages},
				{"evictedPages", view.evictedPages},
				{"pageResidency", {
					{"residentHighDetailBytes", view.pageResidency.residentHighDetailBytes},
					{"residentFallbackBytes", view.pageResidency.residentFallbackBytes},
					{"embeddedFallbackBytes", view.pageResidency.embeddedFallbackBytes},
					{"owningMeshFallbackBytes", view.pageResidency.owningMeshFallbackBytes},
					{"metadataBytes", view.pageResidency.metadataBytes},
					{"requestedBytes", view.pageResidency.requestedBytes},
					{"pinnedFallbackBytes", view.pageResidency.pinnedFallbackBytes},
					{"pendingIoBytes", view.pageResidency.pendingIoBytes},
					{"pendingUploadBytes", view.pageResidency.pendingUploadBytes},
					{"failedBytes", view.pageResidency.failedBytes},
					{"evictingBytes", view.pageResidency.evictingBytes},
					{"evictedBytes", view.pageResidency.evictedBytes},
					{"retiringBytes", view.pageResidency.retiringBytes},
					{"pendingIoPages", view.pageResidency.pendingIoPages},
					{"pendingUploadPages", view.pageResidency.pendingUploadPages},
					{"retiringPages", view.pageResidency.retiringPages}
				}},
				{"pageSources", {
					{"residentPagePayloads", view.pageSources.residentPayloads},
					{"highDetailPagePayloads", view.pageSources.highDetailPayloads},
					{"fallbackPagePayloads", view.pageSources.fallbackPayloads},
					{"embeddedFallbackPagePayloads", view.pageSources.embeddedFallbackPayloads},
					{"emittedHighDetailPages", view.pageSources.emittedHighDetailSources},
					{"emittedFallbackPages", view.pageSources.emittedFallbackSources},
					{"emittedEmbeddedFallbackPages", view.pageSources.emittedEmbeddedFallbackSources},
					{"missingPageSources", view.pageSources.missingSources},
					{"pageRequests", view.pageSources.pageRequests},
					{"pageRetirements", view.pageSources.pageRetirements},
					{"residentPayloadBytes", view.pageSources.residentPayloadBytes},
					{"fallbackPayloadBytes", view.pageSources.fallbackPayloadBytes},
					{"embeddedFallbackBytes", view.pageSources.embeddedFallbackBytes}
				}},
				{"selectedClusters", view.clusterSelection.selectedClusters},
				{"fallbackSelections", view.clusterSelection.fallbackClusters},
				{"geometryBudgetBytes", view.geometryBudgetBytes},
				{"budgetPressureBytes", view.budgetPressureBytes},
				{"performanceGates", std::move(performanceGates)},
				{"shadowVisibilitySelections", std::move(shadowSelections)},
				{"warnings", view.warnings}
			};
		}

		nlohmann::json SerializeProfilerTimingStreams()
		{
			const std::vector<TerraEngine::Profiling::ProfilerFrameRecord> frames =
				TerraEngine::Profiling::Profiler::Instance().GetFramesSnapshot();
			const std::optional<TerraEngine::Profiling::ProfilerFrameRecord> selectedFrame =
				TerraEngine::Profiling::Profiler::Instance().GetSelectedFrameSnapshot();
			nlohmann::json streams = nlohmann::json::array();
			for (const TerraEngine::Profiling::ProfilerTimingStreamAggregate& stream :
				TerraEngine::Profiling::BuildTimingStreamAggregates(frames, selectedFrame))
			{
				// Automation receives the same status-aware timing contract as
				// the Profiler UI, so agents do not scrape overlay text or treat
				// unavailable GPU timing as a real zero.
				streams.push_back({
					{"id", stream.id},
					{"label", stream.label},
					{"scope", TerraEngine::Profiling::ToString(stream.scope)},
					{"source", TerraEngine::Profiling::ToString(stream.source)},
					{"selectedStatus", TerraEngine::Profiling::ToString(stream.selectedStatus)},
					{"selectedMilliseconds", stream.selectedMilliseconds},
					{"selectedStatusReason", stream.selectedStatusReason},
					{"samples", stream.sampleCount},
					{"missing", stream.missingCount},
					{"meanMs", stream.meanMs},
					{"medianMs", stream.medianMs},
					{"p90Ms", stream.p90Ms},
					{"p95Ms", stream.p95Ms},
					{"p99Ms", stream.p99Ms},
					{"minMs", stream.minMs},
					{"maxMs", stream.maxMs},
					{"stdDevMs", stream.stdDevMs}
				});
			}
			return streams;
		}

		nlohmann::json SerializeVirtualGeometryClusterSummary(const VirtualGeometryClusterSummaryMetadata& summary)
		{
			return {
				{"clusterCount", summary.clusterCount},
				{"selectedClusterCount", summary.selectedClusterCount},
				{"fallbackClusterCount", summary.fallbackClusterCount},
				{"materialSectionCount", summary.materialSectionCount},
				{"clusterMaterialSectionCount", summary.clusterMaterialSectionCount},
				{"fallbackCoverageValid", summary.fallbackCoverageValid},
				{"materialCoverageValid", summary.materialCoverageValid}
			};
		}

		nlohmann::json SerializeVirtualGeometryPageSummary(const VirtualGeometryPageSummaryMetadata& summary)
		{
			return {
				{"pageCount", summary.pageCount},
				{"residentPageCount", summary.residentPageCount},
				{"pinnedFallbackPageCount", summary.pinnedFallbackPageCount},
				{"estimatedBytes", summary.estimatedBytes},
				{"pageStreamingReady", summary.pageStreamingReady},
				{"smallMeshStreamingSkipped", summary.smallMeshStreamingSkipped},
				{"skippedStreamingReason", summary.skippedStreamingReason},
				{"integrityErrors", summary.integrityErrors}
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
			case MutationMode::ProjectMutation: return "project_mutation";
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
			case ErrorCategory::ApprovalRequired: return "approval_required";
			case ErrorCategory::MissingTarget: return "missing_target";
			case ErrorCategory::InvalidCameraObject: return "invalid_camera_object";
			case ErrorCategory::UnsupportedCapability: return "unsupported_capability";
			case ErrorCategory::UnsupportedReadback: return "unsupported_readback";
			case ErrorCategory::QueueFull: return "queue_full";
			case ErrorCategory::OverBudget: return "over_budget";
			case ErrorCategory::Conflict: return "conflict";
			case ErrorCategory::StaleTarget: return "stale_target";
			case ErrorCategory::Unavailable: return "unavailable";
			case ErrorCategory::Transport: return "transport";
			case ErrorCategory::Internal: return "internal";
			case ErrorCategory::Canceled: return "canceled";
			case ErrorCategory::Timeout: return "timeout";
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
		const AutomationHandlerResolver resolveHandler = [this](std::string_view name) {
			// Bind the stable operation name once during provider registration. The
			// registry thereafter calls this typed handler directly, so adding a new
			// provider never extends the request-time dispatch path.
			const AutomationOperationId operation = AutomationOperation(name);
			const std::string operationName(name);
			const auto [existing, inserted] = boundProviderOperationNames.emplace(operation, operationName);
			if (!inserted && existing->second != operationName)
			{
				throw std::logic_error("Automation provider operation ID collision between " + existing->second + " and " + operationName + ".");
			}
			return AutomationHandler{[this, operation, operationName](const AutomationRequestContext& request) {
				const WriteMode mode = request.grantedPermission == AutomationPermission::Read
					? WriteMode::ReadOnly
					: WriteMode::WritesEnabled;
				nlohmann::json response = ExecuteProviderHandler(operation, operationName, request.arguments, mode, request.requestId);
				AutomationDispatchResult result;
				result.ok = response.value("ok", false);
				result.error = ErrorCategory::Internal;
				result.message = response.value("message", std::string{});
				result.data = std::move(response);
				return result;
			}};
		};

		// Registration is all-or-nothing per provider. Constructor-time failures
		// leave the corresponding tools undiscoverable instead of publishing
		// metadata without executable handlers.
		(void)capabilityRegistry.RegisterProvider(std::make_shared<ProjectSceneAutomationProvider>(resolveHandler));
		(void)capabilityRegistry.RegisterProvider(std::make_shared<AssetPrefabAutomationProvider>(resolveHandler));
		(void)capabilityRegistry.RegisterProvider(std::make_shared<ScriptBuildAutomationProvider>(resolveHandler));
		(void)capabilityRegistry.RegisterProvider(std::make_shared<EditorDiagnosticsAutomationProvider>(resolveHandler));
		(void)capabilityRegistry.RegisterProvider(std::make_shared<HistoryPlayModeAutomationProvider>(resolveHandler));
		(void)capabilityRegistry.RegisterProvider(std::make_shared<AuthoringAutomationProvider>(resolveHandler));
		// Physics names are resolved once into domain-specific typed handlers. They
		// never enter the legacy request-time central string dispatcher above.
		const AutomationHandlerResolver resolvePhysicsHandler = [this](std::string_view name) {
			return ResolvePhysicsAutomationHandler(name);
		};
		(void)capabilityRegistry.RegisterProvider(std::make_shared<PhysicsAutomationProvider>(resolvePhysicsHandler));
		RegisterDefaultCapabilities();
	}

	AutomationHandler EditorAutomationService::ResolvePhysicsAutomationHandler(std::string_view name)
	{
		const AutomationOperationId operation = AutomationOperation(name);
		const std::string operationName(name);
		const auto [existing, inserted] = boundProviderOperationNames.emplace(operation, operationName);
		if (!inserted && existing->second != operationName)
		{
			throw std::logic_error("Physics automation operation ID collision between " + existing->second + " and " + operationName + ".");
		}
		return [this, operation, operationName](const AutomationRequestContext& request) {
			return ExecutePhysicsAutomationHandler(operation, operationName, request);
		};
	}

	void EditorAutomationService::SetEditorContext(EditorContext* context)
	{
		editorContext = context;
	}

	void EditorAutomationService::RefreshPhysicsCookJob(std::string_view jobId)
	{
		const auto active = activePhysicsCookJobs.find(std::string(jobId));
		if(active == activePhysicsCookJobs.end())
			return;
		const ActivePhysicsCookJob cook = active->second;
		const auto finishIdempotency = [this, &cook, jobId](bool accepted, std::string_view status) {
			(void)idempotencyStore.PublishResult(cook.clientId, cook.capability, cook.idempotencyKey, {
				{"correlationId", cook.correlationId}, {"accepted", accepted},
				{"idempotencyDisposition", "new"}, {"jobId", std::string(jobId)},
				{"status", status}, {"diagnostics", nlohmann::json::array()}
			}, true);
		};
		const auto retained = jobService.Get(jobId);
		if(!retained || retained->state == AutomationJobState::Succeeded
			|| retained->state == AutomationJobState::Failed || retained->state == AutomationJobState::Canceled)
		{
			activePhysicsCookJobs.erase(active);
			return;
		}

		AssetDatabase& assets = AssetDatabase::Instance();
		if(BuildAutomationStateSnapshot().projectGeneration != cook.projectGeneration)
		{
			if(assets.IsAssetCompiling(cook.sourcePath))
				(void)assets.CancelImport(cook.sourcePath, false);
			(void)jobService.CompleteFailed(jobId, {{"message", "Project generation changed before collider publication."}});
			finishIdempotency(false, "stale_project");
			activePhysicsCookJobs.erase(active);
			return;
		}

		if(jobService.IsCancellationRequested(jobId))
		{
			if(assets.IsAssetCompiling(cook.sourcePath))
			{
				(void)assets.CancelImport(cook.sourcePath, false);
				return;
			}
			(void)jobService.CompleteCanceled(jobId, {{"cancellationBoundary", "before_atomic_publication"}});
			finishIdempotency(false, "canceled");
			activePhysicsCookJobs.erase(active);
			return;
		}

		if(const std::optional<AssetImportProgress> progress = assets.GetImportProgress(cook.sourcePath))
		{
			(void)jobService.SetState(jobId, AutomationJobState::Running, progress->stage.empty() ? "cooking" : progress->stage);
			(void)jobService.PublishProgress(jobId, std::clamp(static_cast<double>(progress->progress), 0.0, 0.99), progress->stage);
			if(progress->failed)
			{
				(void)jobService.CompleteFailed(jobId, {{"message", progress->message.empty() ? "Collider compiler rejected the recipe." : progress->message}});
				finishIdempotency(false, "failed");
				activePhysicsCookJobs.erase(active);
			}
			return;
		}
		if(assets.IsAssetCompiling(cook.sourcePath))
			return;

		const std::filesystem::path normalizedSource = std::filesystem::absolute(cook.sourcePath).lexically_normal();
		const auto metadata = assets.GetAssets().find(normalizedSource);
		if(metadata == assets.GetAssets().end() || metadata->second.importerName != "PhysicsColliderRecipeImporter")
		{
			(void)jobService.CompleteFailed(jobId, {{"message", "Collider recipe metadata was not published by AssetDatabase."}});
			finishIdempotency(false, "failed");
			activePhysicsCookJobs.erase(active);
			return;
		}
		const std::filesystem::path outputPath = assets.GetBuildRoot() / metadata->second.compiledPath;
		std::ifstream stream(outputPath, std::ios::binary | std::ios::ate);
		std::vector<std::byte> bytes;
		if(stream)
		{
			const std::streamsize size = stream.tellg();
			if(size >= 0)
			{
				bytes.resize(static_cast<std::size_t>(size));
				stream.seekg(0, std::ios::beg);
				if(size > 0 && !stream.read(reinterpret_cast<char*>(bytes.data()), size))
					bytes.clear();
			}
		}
		TerraEngine::Physics::CookedColliderDiagnostics diagnostics;
		const auto collider = TerraEngine::Physics::CookedColliderAsset::Deserialize(bytes, diagnostics);
		const bool compatible = collider && collider->CheckCompatibility(
			TerraEngine::Physics::CookedColliderAsset::HostPlatform(),
			TerraEngine::Physics::CookedColliderProviderFamily::Jolt,
			TerraEngine::Physics::kJoltCookProviderVersion,
			TerraEngine::Physics::kJoltCookRevision).compatible;
		if(!compatible)
		{
			(void)jobService.CompleteFailed(jobId, {{"message", "Published collider failed the Jolt 5.6 compatibility envelope check."}});
			finishIdempotency(false, "failed");
			activePhysicsCookJobs.erase(active);
			return;
		}

		const std::filesystem::path projectRoot = editorContext->projectManager->GetProjectRoot();
		const std::string displayPath = std::filesystem::relative(outputPath, projectRoot).generic_string();
		(void)jobService.AddArtifact(jobId, {TerraEngine::ToHexString(collider->Envelope().assetId), "cooked_collider", displayPath, "application/x-terraengine-cooked-collider", {
			{"provider", "Jolt"}, {"providerVersion", "5.6.0"},
			{"providerRevision", TerraEngine::Physics::kJoltCookRevision},
			{"recipeHash", collider->Envelope().recipeHash}, {"size", bytes.size()}
		}});
		(void)jobService.CompleteSucceeded(jobId, {{"outputAsset", displayPath}, {"assetId", TerraEngine::ToHexString(collider->Envelope().assetId)}, {"size", bytes.size()}});
		finishIdempotency(true, "succeeded");
		activePhysicsCookJobs.erase(active);
	}

	void EditorAutomationService::SetMainEditor(MainEditorSubsystem* editor)
	{
		// The bridge owns no windows. It receives this non-owning editor pointer so
		// semantic automation always reaches the same Scene View the user sees.
		mainEditor = editor;
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
		if (capabilityRegistry.CapabilityCount() > 0u)
		{
			// Compatibility callers still consume AssistantCapability, but the
			// provider registry is now the only built-in source of tool identity and
			// policy. Do not maintain a second central capability list in parallel.
			for (const AutomationCapabilitySnapshot& snapshot : capabilityRegistry.Snapshot())
			{
				const AutomationCapabilityDescriptor& descriptor = snapshot.descriptor;
				AssistantCapability capability;
				capability.name = descriptor.name;
				capability.category = descriptor.category;
				capability.mutationMode = descriptor.mutationMode;
				capability.mutatesState = descriptor.mutationMode != MutationMode::ReadOnly;
				capability.requiresProject = descriptor.availability.requiresProject;
				capability.requiresScene = descriptor.availability.requiresScene;
				capability.requiresApproval = descriptor.approvalPolicy != AutomationApprovalPolicy::None;
				capability.inputSummary = descriptor.description;
				(void)RegisterCapability(capability);
			}
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
		add("project.templates.list", "project", MutationMode::ReadOnly, false, false, false, "no arguments");
		add("project.state", "project", MutationMode::ReadOnly, false, false, false, "no arguments");
		add("scene.list", "scene", MutationMode::ReadOnly, true, false, false, "no arguments");
		add("scene.create", "scene", MutationMode::SceneMutation, true, false, false, "path: project-relative .tescene, name?: string, open?: bool");
		add("scene.save_as", "scene", MutationMode::SceneMutation, true, true, false, "path: project-relative .tescene");
		add("scene.rename", "scene", MutationMode::SceneMutation, true, false, true, "source/destination: project-relative .tescene paths");
		add("scene.duplicate", "scene", MutationMode::SceneMutation, true, false, false, "source/destination: project-relative .tescene paths");
		add("scene.prepare_delete", "scene", MutationMode::Destructive, true, false, true, "path: project-relative .tescene");
		add("transaction.execute", "transaction", MutationMode::SceneMutation, true, true, false, "operations: typed array, expectedSceneGeneration?: integer, label?: string");
		add("history.state", "history", MutationMode::ReadOnly, true, true, false, "no arguments");
		add("history.undo", "history", MutationMode::SceneMutation, true, true, false, "expectedSequence: integer");
		add("history.redo", "history", MutationMode::SceneMutation, true, true, false, "expectedSequence: integer");
		add("play.state", "play", MutationMode::ReadOnly, false, false, false, "no arguments");
		add("play.enter", "play", MutationMode::ProjectMutation, false, false, false, "expectedState: stopped");
		add("play.pause", "play", MutationMode::ProjectMutation, false, false, false, "expectedState: playing");
		add("play.resume", "play", MutationMode::ProjectMutation, false, false, false, "expectedState: paused");
		add("play.step", "play", MutationMode::ProjectMutation, false, false, false, "expectedState: paused");
		add("play.exit", "play", MutationMode::ProjectMutation, false, false, false, "expectedState: playing|paused");
		add("jobs.list", "jobs", MutationMode::ReadOnly, false, false, false, "limit?: integer");
		add("jobs.get", "jobs", MutationMode::ReadOnly, false, false, false, "jobId: string");
		add("jobs.cancel", "jobs", MutationMode::ProjectMutation, false, false, false, "jobId: string");
		add("logs.query", "logs", MutationMode::ReadOnly, false, false, false, "severity/category/source/text/correlationId/requestId/jobId/cursor/pageSize");
		add("asset.inspect", "asset", MutationMode::ReadOnly, true, false, false, "asset, operation, destination");
		add("asset.move", "asset", MutationMode::AssetMutation, true, false, false, "source, destination");
		add("asset.rename", "asset", MutationMode::AssetMutation, true, false, false, "asset, name|destination");
		add("asset.duplicate", "asset", MutationMode::AssetMutation, true, false, false, "source, destination");
		add("prefab.create", "prefab", MutationMode::AssetMutation, true, true, false, "object, path");
		add("prefab.instantiate", "prefab", MutationMode::SceneMutation, true, true, false, "prefab, name?");
		add("prefab.overrides.inspect", "prefab", MutationMode::ReadOnly, true, true, false, "instance");
		add("prefab.overrides.apply", "prefab", MutationMode::AssetMutation, true, true, false, "instance, expectedPrefabGeneration");
		add("prefab.overrides.revert", "prefab", MutationMode::SceneMutation, true, true, false, "instance");
		add("prefab.edit.open", "prefab", MutationMode::ProjectMutation, true, false, false, "prefab, dirtyPolicy");
		add("prefab.edit.close", "prefab", MutationMode::ProjectMutation, true, false, false, "contextId, dirtyPolicy");
		add("script.templates.list", "script", MutationMode::ReadOnly, true, false, false, "no arguments");
		add("script.create", "script", MutationMode::AssetMutation, true, false, false, "template, typeName, namespace?, destination");
		add("script.types.list", "script", MutationMode::ReadOnly, true, false, false, "no arguments");
		add("script.workspace.sync", "script", MutationMode::ProjectMutation, true, false, false, "no arguments");
		add("script.compile", "script", MutationMode::ProjectMutation, true, false, false, "profile?, correlationId?");
		add("build_profile.list", "build_profile", MutationMode::ReadOnly, true, false, false, "no arguments");
		add("build_profile.get", "build_profile", MutationMode::ReadOnly, true, false, false, "profileId");
		add("build_profile.create", "build_profile", MutationMode::ProjectMutation, true, false, false, "name");
		add("build_profile.update", "build_profile", MutationMode::ProjectMutation, true, false, false, "profileId, changes");
		add("build_profile.duplicate", "build_profile", MutationMode::ProjectMutation, true, false, false, "profileId, name");
		add("build_profile.prepare_delete", "build_profile", MutationMode::Destructive, true, false, true, "profileId");
		add("settings.project.get", "settings", MutationMode::ReadOnly, true, false, false, "key");
		add("settings.project.set", "settings", MutationMode::ProjectMutation, true, false, false, "key, value");
		add("settings.editor.get", "settings", MutationMode::ReadOnly, false, false, false, "key");
		add("settings.editor.set", "settings", MutationMode::ProjectMutation, false, false, true, "key, value");
		for (const std::string& name : {"scene_view.state", "scene_view.camera.set", "scene_view.frame_object", "scene_view.tool.set", "scene_view.orientation.set", "scene_view.snapping.set", "scene_view.visualization.set"})
			add(name, "scene_view", name == "scene_view.state" ? MutationMode::ReadOnly : MutationMode::EditorSelection, false, false, false, "semantic Scene View arguments");
		for (const std::string& name : {"profiler.capture.start", "profiler.capture.stop", "profiler.capture.list", "profiler.capture.load", "profiler.capture.summary"})
			add(name, "profiler", name.ends_with("list") || name.ends_with("summary") ? MutationMode::ReadOnly : MutationMode::ProjectMutation, true, false, false, "artifactId?");
		add("editor.commands.list", "editor_commands", MutationMode::ReadOnly, false, false, false, "no arguments");
		add("editor.commands.invoke", "editor_commands", MutationMode::ProjectMutation, false, false, false, "commandId");
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
		add("renderer.virtualGeometry.stats", "renderer", MutationMode::ReadOnly, false, false, false, "viewType?: Game|Scene");
		add("renderer.recovery.safe_save_project", "renderer", MutationMode::ProjectMutation, true, false, false, "reason?: string");
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
		if (!arguments.is_object())
		{
			return BuildError(requestId, ErrorCategory::Validation, "Tool arguments must be a JSON object.");
		}
		const auto capability = capabilityRegistry.Find(name, 1, BuildAutomationStateSnapshot());
		if (!capability)
		{
			return BuildError(requestId, ErrorCategory::UnsupportedCapability, "Capability version is not registered.");
		}
		std::string schemaError;
		if (capability->descriptor.execution.workerPreparation && ThreadPool::instance().getThreadCount() > 0u)
		{
			// JSON and descriptor copies are immutable, so worker preparation cannot
			// race the editor state revalidation performed by the handler later.
			auto task = ThreadPool::instance().createTask([arguments, schema = capability->descriptor.inputSchema]() {
				return ValidateRequiredSchemaFields(arguments, schema);
			});
			auto future = task->getFuture();
			ThreadPool::instance().enqueue(task);
			schemaError = future.get();
		}
		else schemaError = ValidateRequiredSchemaFields(arguments, capability->descriptor.inputSchema);
		if (!schemaError.empty()) return BuildError(requestId, ErrorCategory::Validation, schemaError);

		AutomationRequestContext request;
		request.requestId = requestId;
		request.clientId = "local-mcp";
		request.correlationId = arguments.value("correlationId", requestId);
		request.idempotencyKey = arguments.value("idempotencyKey", std::string{});
		request.arguments = arguments;
		request.state = BuildAutomationStateSnapshot();
		request.preserveLegacyAvailability = true;
		request.grantedPermission = writeMode == WriteMode::WritesEnabled
			? AutomationPermission::ProjectWrite
			: AutomationPermission::Read;

		// Native logs emitted while a protected handler runs inherit stable MCP
		// identities without requiring every subsystem to format correlation data.
		// Worker jobs establish their own scope when they execute because this
		// context is intentionally thread-local.
		Logger::ScopedContext logContext({
			capability->descriptor.category,
			"editor.mcp",
			request.correlationId,
			request.requestId,
			{}
		});
		const AutomationDispatchResult dispatched = capabilityRegistry.Dispatch(name, 1, request);
		nlohmann::json response;
		if (dispatched.data.is_object() && dispatched.data.contains("ok"))
		{
			response = dispatched.data;
			response["correlationId"] = request.correlationId;
		}
		else
		{
			response = dispatched.ok
				? BuildSuccess(requestId, dispatched.message, dispatched.data)
				: BuildError(requestId, dispatched.error, dispatched.message);
			response["correlationId"] = request.correlationId;
		}

		// Record a structured completion even when the called subsystem emits no
		// native log line. This guarantees that correlation-based MCP diagnostics
		// can always find the semantic operation which started a workflow.
		StructuredLogRecord completion;
		completion.severity = response.value("ok", false) ? AutomationLogSeverity::Info : AutomationLogSeverity::Warning;
		completion.category = capability->descriptor.category;
		completion.source = "editor.mcp";
		completion.correlationId = request.correlationId;
		completion.requestId = request.requestId;
		completion.message = response.value("message", dispatched.message);
		completion.details = {
			{"capability", std::string(name)},
			{"ok", response.value("ok", false)},
			{"status", response.value("status", std::string{})}
		};
		(void)logStore.Append(std::move(completion));
		return response;
	}

	AutomationDispatchResult EditorAutomationService::ExecutePhysicsAutomationHandler(
		AutomationOperationId operation,
		std::string_view operationName,
		const AutomationRequestContext& request)
	{
		using namespace TerraEngine::Physics;
		const auto reject = [](ErrorCategory category, std::string message) {
			return AutomationDispatchResult{false, category, std::move(message), nlohmann::json::object()};
		};
		const auto succeed = [](std::string message, nlohmann::json data) {
			return AutomationDispatchResult{true, ErrorCategory::Internal, std::move(message), std::move(data)};
		};
		if (!request.state.projectAvailable)
		{
			return reject(ErrorCategory::Unavailable, "Physics automation requires an already-open project.");
		}

		PhysicsSystem& physics = PhysicsSystem::Instance();
		const PhysicsWorldHandle world = physics.WorldHandle();
		const auto diagnostics = physics.LatestDiagnostics();
		const PhysicsTick tick = diagnostics ? diagnostics->tick : 0U;
		const auto snapshot = [&request, world, tick](nlohmann::json data, bool truncated = false, bool overflow = false) {
			return PhysicsSnapshotResult(request, world, tick, std::move(data), truncated, overflow);
		};

		if (operation == AutomationOperation("physics.state"))
		{
			return succeed("Read the immutable physics world state.", snapshot({
				{"worldId", world.worldId.value}, {"worldGeneration", world.worldGeneration},
				{"state", diagnostics ? std::string(ToString(diagnostics->state)) : "Absent"},
				{"settingsGeneration", physics.SettingsGeneration()},
				{"bodyCount", diagnostics ? diagnostics->counters.bodyCount : 0U},
				{"constraintCount", diagnostics ? diagnostics->counters.constraintCount : 0U},
				{"characterCount", diagnostics ? diagnostics->counters.characterCount : 0U}
			}));
		}
		if (operation == AutomationOperation("physics.settings.get"))
		{
			return succeed("Read the resolved physics settings.", snapshot(PhysicsSettingsJson(physics.ConfiguredSettings())));
		}
		if (operation == AutomationOperation("physics.layers.list"))
		{
			nlohmann::json layers = nlohmann::json::array();
			for (const CollisionLayer& layer : physics.ConfiguredSettings().layers)
			{
				layers.push_back({{"id", layer.id}, {"name", layer.name}, {"collisionMask", layer.collisionMask}, {"broadPhaseGroup", layer.broadPhaseGroup}, {"enabled", layer.enabled}});
			}
			return succeed("Listed collision layers from the immutable settings snapshot.", snapshot({{"layers", std::move(layers)}}));
		}
		if (operation == AutomationOperation("physics.diagnostics.get"))
		{
			if (!diagnostics) return succeed("Physics diagnostics are not published yet.", snapshot(nlohmann::json::object()));
			return succeed("Read immutable physics diagnostics.", snapshot({
				{"counters", {{"bodies", diagnostics->counters.bodyCount}, {"shapes", diagnostics->counters.shapeCount}, {"constraints", diagnostics->counters.constraintCount}, {"characters", diagnostics->counters.characterCount}, {"contacts", diagnostics->counters.contactCount}, {"rejected", diagnostics->counters.rejectedCount}, {"stale", diagnostics->counters.staleCount}}},
				{"queues", {{"commands", diagnostics->queues.commandDepth}, {"queries", diagnostics->queues.queryDepth}, {"events", diagnostics->queues.eventDepth}}},
				{"memory", {{"snapshots", diagnostics->memory.snapshotBytes}, {"highWater", diagnostics->memory.highWaterBytes}, {"providerAllocations", diagnostics->memory.outstandingProviderAllocations}, {"providerAllocationHighWater", diagnostics->memory.highWaterProviderAllocations}}},
				{"cache", {{"shapes", diagnostics->cache.sharedShapeCount}, {"assets", diagnostics->cache.assetCount}, {"bytes", diagnostics->cache.payloadBytes}, {"hits", diagnostics->cache.hitCount}, {"misses", diagnostics->cache.missCount}}}
			}));
		}

		const auto parseTarget = [&request, &world, &reject](const nlohmann::json& target, TerraEngine::UUID& objectId, std::uint32_t& componentGeneration) -> std::optional<AutomationDispatchResult> {
			if (!target.is_object() || !target.contains("objectId") || !target["objectId"].is_string()
				|| !target.contains("componentGeneration") || !target["componentGeneration"].is_number_unsigned()
				|| !target.contains("worldGeneration") || !target["worldGeneration"].is_number_unsigned())
			{
				return reject(ErrorCategory::Validation, "Physics target must contain objectId, componentGeneration, and worldGeneration.");
			}
			const auto parsed = TerraEngine::ParseUUID(target["objectId"].get<std::string>());
			if (!parsed) return reject(ErrorCategory::Validation, "Physics target objectId must be a 32-character UUID.");
			if (target["worldGeneration"].get<std::uint32_t>() != world.worldGeneration)
			{
				return reject(ErrorCategory::Conflict, "Physics target world generation is stale.");
			}
			objectId = *parsed;
			componentGeneration = target["componentGeneration"].get<std::uint32_t>();
			return std::nullopt;
		};

		if (operation == AutomationOperation("physics.body.inspect"))
		{
			TerraEngine::UUID objectId{};
			std::uint32_t componentGeneration = 0U;
			if (const auto error = parseTarget(request.arguments["target"], objectId, componentGeneration)) return *error;
			const auto bodies = physics.LatestBodySnapshot();
			if (!bodies || !bodies->bodies) return reject(ErrorCategory::MissingTarget, "No physics body snapshot is available.");
			const auto found = std::find_if(bodies->bodies->begin(), bodies->bodies->end(), [&](const PhysicsBodyState& body) { return body.objectId == objectId && body.componentGeneration == componentGeneration; });
			if (found == bodies->bodies->end()) return reject(ErrorCategory::MissingTarget, "The body identity or component generation is not present in the published snapshot.");
			return succeed("Read immutable body state.", PhysicsSnapshotResult(request, bodies->world, bodies->tick, {
				{"objectId", TerraEngine::ToHexString(found->objectId)}, {"objectGeneration", found->objectGeneration}, {"componentGeneration", found->componentGeneration},
				{"position", PhysicsVectorJson(found->worldPose.position)}, {"rotation", {found->worldPose.rotation.x, found->worldPose.rotation.y, found->worldPose.rotation.z, found->worldPose.rotation.w}},
				{"linearVelocity", PhysicsVectorJson(found->linearVelocity)}, {"angularVelocity", PhysicsVectorJson(found->angularVelocity)}, {"awake", found->awake}
			}));
		}
		if (operation == AutomationOperation("physics.character.inspect"))
		{
			TerraEngine::UUID objectId{};
			std::uint32_t componentGeneration = 0U;
			if (const auto error = parseTarget(request.arguments["target"], objectId, componentGeneration)) return *error;
			const auto characters = physics.LatestCharacterStatePacket();
			if (!characters || !characters->states) return reject(ErrorCategory::MissingTarget, "No character snapshot is available.");
			const auto found = std::find_if(characters->states->begin(), characters->states->end(), [&](const PhysicsCharacterState& state) { return state.objectId == objectId && state.componentGeneration == componentGeneration; });
			if (found == characters->states->end()) return reject(ErrorCategory::MissingTarget, "The character identity or component generation is not present in the published snapshot.");
			return succeed("Read immutable character state.", PhysicsSnapshotResult(request, characters->world, characters->tick, {
				{"objectId", TerraEngine::ToHexString(found->objectId)}, {"componentGeneration", found->componentGeneration}, {"position", PhysicsVectorJson(found->pose.position)},
				{"linearVelocity", PhysicsVectorJson(found->linearVelocity)}, {"groundState", static_cast<std::uint32_t>(found->groundState)}, {"groundNormal", PhysicsVectorJson(found->groundNormal)},
				{"supportVelocity", PhysicsVectorJson(found->supportVelocity)}, {"contactCount", found->contactCount}, {"supported", found->supported}
			}));
		}
		if (operation == AutomationOperation("physics.contacts.list"))
		{
			if (!request.arguments["worldGeneration"].is_number_unsigned()
				|| request.arguments["worldGeneration"].get<std::uint32_t>() != world.worldGeneration)
				return reject(ErrorCategory::Conflict, "Contact request world generation is stale.");
			if (!request.arguments["maximumContacts"].is_number_unsigned()
				|| request.arguments["maximumContacts"].get<std::uint32_t>() == 0U
				|| request.arguments["maximumContacts"].get<std::uint32_t>() > 64U)
			{
				return reject(ErrorCategory::Validation, "maximumContacts must be an unsigned value between 1 and 64.");
			}
			const std::uint32_t limit = request.arguments["maximumContacts"].get<std::uint32_t>();
			const auto packet = physics.LatestEventPacket();
			nlohmann::json events = nlohmann::json::array();
			bool truncated = false;
			if (packet && packet->events)
			{
				for (const PhysicsEvent& event : *packet->events)
				{
					if (event.type != PhysicsEventType::Contact) continue;
					if (events.size() >= limit) { truncated = true; break; }
					events.push_back({{"tick", event.tick}, {"first", TerraEngine::ToHexString(event.first.objectId)}, {"second", TerraEngine::ToHexString(event.second.objectId)}, {"point", PhysicsVectorJson(event.point)}, {"normal", PhysicsVectorJson(event.normal)}, {"impulse", event.impulse}});
				}
			}
			return succeed("Listed ordered published contacts.", PhysicsSnapshotResult(request, world, packet ? packet->tick : tick, {{"contacts", std::move(events)}}, truncated, packet ? packet->overflowed : false));
		}

		if (IsOperationOneOf(operation, {AutomationOperation("physics.raycast"), AutomationOperation("physics.shape_cast"), AutomationOperation("physics.overlap")}))
		{
			if (!world.IsValid()) return reject(ErrorCategory::Unavailable, "No active physics world is available for queries.");
			if (!request.arguments["worldGeneration"].is_number_unsigned() || request.arguments["worldGeneration"].get<std::uint32_t>() != world.worldGeneration)
				return reject(ErrorCategory::Conflict, "Query world generation is stale.");
			const auto origin = ReadPhysicsVector(request.arguments["origin"]);
			if (!origin) return reject(ErrorCategory::Validation, "Query origin must contain three finite numbers.");
			if (!request.arguments["maximumHits"].is_number_unsigned())
				return reject(ErrorCategory::Validation, "maximumHits must be an unsigned value between 1 and 64.");
			const std::uint32_t maximumHits = request.arguments["maximumHits"].get<std::uint32_t>();
			if (maximumHits == 0U || maximumHits > 64U)
				return reject(ErrorCategory::Validation, "maximumHits must be an unsigned value between 1 and 64.");
			if (request.arguments.contains("batchSize")
				&& (!request.arguments["batchSize"].is_number_unsigned()
					|| request.arguments["batchSize"].get<std::uint32_t>() == 0U
					|| request.arguments["batchSize"].get<std::uint32_t>() > 1024U))
			{
				return reject(ErrorCategory::Validation, "batchSize must be an unsigned value between 1 and 1024.");
			}
			if (request.arguments.contains("layerMask") && !request.arguments["layerMask"].is_number_unsigned())
				return reject(ErrorCategory::Validation, "layerMask must be an unsigned collision mask.");
			if (request.arguments.contains("includeTriggers") && !request.arguments["includeTriggers"].is_boolean())
				return reject(ErrorCategory::Validation, "includeTriggers must be boolean.");
			PhysicsQueryRequest query;
			query.world = world;
			query.pose.position = *origin;
			query.maximumHits = maximumHits;
			query.filter.collisionMask = request.arguments.value("layerMask", 0xFFFFFFFFU);
			query.filter.includeSensors = request.arguments.value("includeTriggers", false);
			query.correlationId = request.correlationId;
			if (operation == AutomationOperation("physics.raycast")) query.kind = PhysicsQueryKind::RayCast;
			else if (operation == AutomationOperation("physics.shape_cast")) query.kind = PhysicsQueryKind::ShapeCast;
			else query.kind = PhysicsQueryKind::Overlap;
			if (query.kind != PhysicsQueryKind::Overlap)
			{
				const auto direction = request.arguments.contains("direction") ? ReadPhysicsVector(request.arguments["direction"]) : std::optional<PhysicsVector3>{PhysicsVector3{1.0F, 0.0F, 0.0F}};
				if (!direction) return reject(ErrorCategory::Validation, "Query direction must contain three finite numbers.");
				query.direction = *direction;
				const auto distance = ReadBoundedPhysicsNumber(request.arguments.value("distance", nlohmann::json(0.0)), 0.0F, 100000.0F);
				if (!distance) return reject(ErrorCategory::Validation, "distance must be a finite number between 0 and 100000.");
				query.distance = *distance;
			}
			if (query.kind != PhysicsQueryKind::RayCast)
			{
				const auto extent = request.arguments.contains("halfExtents") ? ReadPhysicsVector(request.arguments["halfExtents"]) : std::nullopt;
				if (!extent || extent->x <= 0.0F || extent->y <= 0.0F || extent->z <= 0.0F
					|| extent->x > 10000.0F || extent->y > 10000.0F || extent->z > 10000.0F)
					return reject(ErrorCategory::Validation, "Shape query halfExtents must contain three finite values between 0 and 10000.");
				query.shapeExtent = *extent;
			}
			const AutomationJobSnapshot job = jobService.Create({"physics_query", std::string(operationName), request.requestId, request.correlationId, request.idempotencyKey, true});
			if (job.jobId.empty()) return reject(ErrorCategory::QueueFull, "Physics query job retention is full.");
			(void)jobService.SetState(job.jobId, AutomationJobState::Running, "querying");
			const PhysicsQueryResult result = physics.QueryImmediate(std::move(query));
			nlohmann::json hits = nlohmann::json::array();
			if (result.hits)
			{
				for (const PhysicsQueryHit& hit : *result.hits)
				{
					hits.push_back({{"objectId", TerraEngine::ToHexString(hit.objectId)}, {"objectGeneration", hit.objectGeneration}, {"componentGeneration", hit.componentGeneration}, {"position", PhysicsVectorJson(hit.position)}, {"normal", PhysicsVectorJson(hit.normal)}, {"distance", hit.distance}, {"fraction", hit.fraction}, {"collisionLayer", hit.collisionLayer}, {"sensor", hit.sensor}});
				}
			}
			nlohmann::json jobResult = {{"status", std::string(ToString(result.status))}, {"tick", result.tick}, {"truncated", result.truncated}, {"hits", std::move(hits)}, {"diagnostic", result.diagnostic}};
			if (result.status == PhysicsResultStatus::Success) (void)jobService.CompleteSucceeded(job.jobId, jobResult);
			else (void)jobService.CompleteFailed(job.jobId, jobResult);
			return succeed("Completed bounded physics query.", {{"correlationId", request.correlationId}, {"worldGeneration", world.worldGeneration}, {"tick", result.tick}, {"accepted", result.status == PhysicsResultStatus::Success}, {"idempotencyDisposition", "new"}, {"jobId", job.jobId}, {"diagnostics", nlohmann::json::array({{{"message", result.diagnostic}}})}});
		}
		if (operation == AutomationOperation("physics.query_job.get"))
		{
			const std::string jobId = request.arguments.value("jobId", std::string{});
			RefreshPhysicsCookJob(jobId);
			const auto job = jobService.Get(jobId);
			if (!job) return reject(ErrorCategory::MissingTarget, "Physics query/cook job was not found or expired.");
			return succeed("Read retained physics job receipt.", snapshot({{"job", SerializeAutomationJob(*job)}}));
		}

		const auto projectGenerationMatches = [&request]() {
			return request.arguments.contains("projectGeneration")
				&& request.arguments["projectGeneration"].is_number_unsigned()
				&& request.arguments["projectGeneration"].get<std::uint64_t>() == request.state.projectGeneration;
		};
		const auto approvalMatches = [&request, operationName]() {
			if (!request.arguments.contains("approval") || !request.arguments["approval"].is_object()) return false;
			const nlohmann::json& approval = request.arguments["approval"];
			return approval.value("projectGeneration", std::numeric_limits<std::uint64_t>::max()) == request.state.projectGeneration
				&& approval.value("sceneGeneration", request.state.sceneGeneration) == request.state.sceneGeneration
				&& approval.value("impactFingerprint", std::string{}) == PhysicsImpactFingerprint(operationName, request.arguments, request.state)
				&& !approval.value("approvalId", std::string{}).empty();
		};
		const auto beginIdempotency = [this, &request, operationName]() {
			return idempotencyStore.Begin({
				request.clientId, std::string(operationName), request.idempotencyKey,
				StableFingerprint(PhysicsIdempotencyPayload(request.arguments).dump()),
				{{"correlationId", request.correlationId}, {"accepted", false}, {"idempotencyDisposition", "new"}}
			});
		};
		const auto abandonIdempotency = [this, &request, operationName]() {
			// Validation happens after reservation because replay/conflict must be
			// decided before an operation can enter a typed queue. A rejected local
			// validation must release that reservation, otherwise a typo can leave a
			// permanently non-terminal idempotency record until the session ends.
			(void)idempotencyStore.Erase(request.clientId, operationName, request.idempotencyKey);
		};
		const auto rejectAfterReservation = [&abandonIdempotency, &reject](ErrorCategory category, std::string message) {
			abandonIdempotency();
			return reject(category, std::move(message));
		};
		const auto replayOrConflict = [&succeed, &reject](const IdempotencyDecision& decision) -> std::optional<AutomationDispatchResult> {
			if (decision.disposition == IdempotencyDisposition::Replay)
			{
				nlohmann::json replay = decision.result;
				replay["idempotencyDisposition"] = "replay";
				return succeed("Replayed the retained physics receipt.", std::move(replay));
			}
			if (decision.disposition == IdempotencyDisposition::Conflict)
				return reject(ErrorCategory::Conflict, "The idempotency key was already used for a different physics request.");
			if (decision.disposition == IdempotencyDisposition::CapacityExceeded)
				return reject(ErrorCategory::QueueFull, "The bounded physics idempotency store is full.");
			return std::nullopt;
		};

		if (operation == AutomationOperation("physics.settings.preflight"))
		{
			if (!projectGenerationMatches()) return reject(ErrorCategory::Conflict, "Project generation changed before physics settings preflight.");
			PhysicsSettings candidate = physics.ConfiguredSettings();
			std::string error;
			if (!ApplyPhysicsSettingsPatch(request.arguments["settings"], candidate, error)) return reject(ErrorCategory::Validation, error);
			return succeed("Prepared physics settings reference impact.", {
				{"impactFingerprint", PhysicsImpactFingerprint("physics.settings.set", request.arguments, request.state)},
				{"projectGeneration", request.state.projectGeneration}, {"sceneGeneration", request.state.sceneGeneration},
				{"referenceCount", 0U}, {"affectedObjects", nlohmann::json::array()}, {"requiresApproval", true}, {"expirySeconds", 300U}
			});
		}
		if (operation == AutomationOperation("physics.settings.set"))
		{
			if (!projectGenerationMatches() || !approvalMatches()) return reject(ErrorCategory::Conflict, "Physics settings approval fingerprint or project/scene generation is stale.");
			const IdempotencyDecision decision = beginIdempotency();
			if (const auto prior = replayOrConflict(decision)) return *prior;
			PhysicsSettings candidate = physics.ConfiguredSettings();
			std::string error;
			if (!ApplyPhysicsSettingsPatch(request.arguments["settings"], candidate, error)) return rejectAfterReservation(ErrorCategory::Validation, error);
			const bool accepted = physics.QueueSettingsUpdate(candidate);
			nlohmann::json receipt = {{"correlationId", request.correlationId}, {"worldGeneration", world.worldGeneration}, {"tick", tick}, {"accepted", accepted}, {"idempotencyDisposition", "new"}, {"diagnostics", nlohmann::json::array()}};
			(void)idempotencyStore.PublishResult(request.clientId, operationName, request.idempotencyKey, receipt, true);
			return accepted ? succeed("Queued physics settings for the protected snapshot boundary.", receipt) : reject(ErrorCategory::Validation, "Physics settings queue rejected the validated update.");
		}

		if (IsOperationOneOf(operation, {AutomationOperation("physics.layer.create"), AutomationOperation("physics.layer.update")}))
		{
			if (!projectGenerationMatches() || !approvalMatches()) return reject(ErrorCategory::Conflict, "Collision-layer approval fingerprint or project/scene generation is stale.");
			if (!request.arguments["layer"].is_object()) return reject(ErrorCategory::Validation, "layer must be an object.");
			const IdempotencyDecision decision = beginIdempotency();
			if (const auto prior = replayOrConflict(decision)) return *prior;
			const nlohmann::json& patch = request.arguments["layer"];
			if (!patch.contains("id") || !patch["id"].is_number_unsigned()) return rejectAfterReservation(ErrorCategory::Validation, "layer.id is required.");
			const std::uint32_t id = patch["id"].get<std::uint32_t>();
			if (id >= kPhysicsCollisionLayerCount) return rejectAfterReservation(ErrorCategory::Validation, "layer.id exceeds the 32-layer limit.");
			if (patch.contains("name") && (!patch["name"].is_string() || patch["name"].get<std::string>().empty()))
				return rejectAfterReservation(ErrorCategory::Validation, "layer.name must be a non-empty string.");
			if (patch.contains("collisionMask") && !patch["collisionMask"].is_number_unsigned())
				return rejectAfterReservation(ErrorCategory::Validation, "layer.collisionMask must be unsigned.");
			if (patch.contains("broadPhaseGroup") && (!patch["broadPhaseGroup"].is_number_unsigned() || patch["broadPhaseGroup"].get<std::uint64_t>() > std::numeric_limits<std::uint8_t>::max()))
				return rejectAfterReservation(ErrorCategory::Validation, "layer.broadPhaseGroup must fit in an unsigned byte.");
			if (patch.contains("enabled") && !patch["enabled"].is_boolean())
				return rejectAfterReservation(ErrorCategory::Validation, "layer.enabled must be boolean.");
			PhysicsSettings candidate = physics.ConfiguredSettings();
			CollisionLayer& layer = candidate.layers[id];
			if (patch.contains("name") && patch["name"].is_string()) layer.name = patch["name"].get<std::string>();
			if (patch.contains("collisionMask") && patch["collisionMask"].is_number_unsigned()) layer.collisionMask = patch["collisionMask"].get<std::uint32_t>();
			if (patch.contains("broadPhaseGroup") && patch["broadPhaseGroup"].is_number_unsigned()) layer.broadPhaseGroup = patch["broadPhaseGroup"].get<std::uint8_t>();
			if (patch.contains("enabled") && patch["enabled"].is_boolean()) layer.enabled = patch["enabled"].get<bool>();
			const auto validation = candidate.Validate();
			if (!validation.accepted) return rejectAfterReservation(ErrorCategory::Validation, validation.message);
			const bool accepted = physics.QueueSettingsUpdate(candidate);
			nlohmann::json receipt = {{"correlationId", request.correlationId}, {"worldGeneration", world.worldGeneration}, {"tick", tick}, {"accepted", accepted}, {"idempotencyDisposition", "new"}, {"diagnostics", nlohmann::json::array()}};
			(void)idempotencyStore.PublishResult(request.clientId, operationName, request.idempotencyKey, receipt, true);
			return accepted ? succeed("Queued the collision-layer update for the protected settings boundary.", receipt) : reject(ErrorCategory::Validation, "Collision-layer update was rejected.");
		}

		const auto findMaterialPath = [this](const TerraEngine::UUID& id) -> std::optional<std::filesystem::path> {
			if (editorContext == nullptr || editorContext->projectManager == nullptr) return std::nullopt;
			const std::filesystem::path assets = editorContext->projectManager->GetProjectRoot() / "Assets";
			std::error_code error;
			std::size_t inspected = 0;
			for (std::filesystem::recursive_directory_iterator iterator(assets, error), end; !error && iterator != end && inspected < 4096U; iterator.increment(error), ++inspected)
			{
				if (!iterator->is_regular_file(error) || iterator->path().extension() != Physics::kPhysicsMaterialExtension) continue;
				PhysicsMaterial candidate;
				PhysicsMaterialDiagnostics loadDiagnostics;
				if (TerraEngine::AssetPipeline::LoadPhysicsMaterialAsset(iterator->path(), candidate, loadDiagnostics) && candidate.assetId == id) return iterator->path();
			}
			return std::nullopt;
		};
		if (operation == AutomationOperation("physics.material.inspect"))
		{
			const auto id = TerraEngine::ParseUUID(request.arguments.value("materialId", std::string{}));
			if (!id) return reject(ErrorCategory::Validation, "materialId must be a 32-character UUID.");
			const auto path = findMaterialPath(*id);
			if (!path) return reject(ErrorCategory::MissingTarget, "Physics material was not found in the project Assets tree.");
			PhysicsMaterial material;
			PhysicsMaterialDiagnostics materialDiagnostics;
			if (!TerraEngine::AssetPipeline::LoadPhysicsMaterialAsset(*path, material, materialDiagnostics)) return reject(ErrorCategory::Validation, "Physics material failed validation.");
			return succeed("Inspected the validated physics material.", snapshot({{"material", material.ToJson()}, {"path", std::filesystem::relative(*path, editorContext->projectManager->GetProjectRoot()).generic_string()}}));
		}
		if (IsOperationOneOf(operation, {AutomationOperation("physics.material.create"), AutomationOperation("physics.material.update")}))
		{
			if (!projectGenerationMatches()) return reject(ErrorCategory::Conflict, "Project generation changed before material publication.");
			if (operation == AutomationOperation("physics.material.update") && !approvalMatches()) return reject(ErrorCategory::Conflict, "Material approval fingerprint is stale.");
			if (!request.arguments["material"].is_object()) return reject(ErrorCategory::Validation, "material must be an object.");
			const IdempotencyDecision decision = beginIdempotency();
			if (const auto prior = replayOrConflict(decision)) return *prior;
			const nlohmann::json& patch = request.arguments["material"];
			const auto id = TerraEngine::ParseUUID(patch.value("materialId", std::string{}));
			if (!id) return rejectAfterReservation(ErrorCategory::Validation, "material.materialId must be a 32-character UUID.");
			PhysicsMaterial material = PhysicsMaterial::Defaults(*id);
			std::optional<std::filesystem::path> existing = findMaterialPath(*id);
			if (operation == AutomationOperation("physics.material.create") && existing)
			{
				return rejectAfterReservation(ErrorCategory::Conflict, "A physics material with this ID already exists; use the approval-gated update capability.");
			}
			if (existing)
			{
				PhysicsMaterialDiagnostics loadDiagnostics;
				if (!TerraEngine::AssetPipeline::LoadPhysicsMaterialAsset(*existing, material, loadDiagnostics)) return rejectAfterReservation(ErrorCategory::Validation, "Existing physics material is invalid and was preserved.");
			}
			const auto assignMaterialNumber = [&patch, &rejectAfterReservation](std::string_view key, float minimum, float maximum, float& output) -> std::optional<AutomationDispatchResult> {
				if (!patch.contains(std::string(key))) return std::nullopt;
				const auto value = ReadBoundedPhysicsNumber(patch[std::string(key)], minimum, maximum);
				if (!value) return rejectAfterReservation(ErrorCategory::Validation, std::string(key) + " is outside its supported finite range.");
				output = *value;
				return std::nullopt;
			};
			if (const auto error = assignMaterialNumber("staticFriction", 0.0F, 100.0F, material.staticFriction)) return *error;
			if (const auto error = assignMaterialNumber("dynamicFriction", 0.0F, 100.0F, material.dynamicFriction)) return *error;
			if (const auto error = assignMaterialNumber("restitution", 0.0F, 1.0F, material.restitution)) return *error;
			if (const auto error = assignMaterialNumber("density", 0.0001F, 100000.0F, material.density)) return *error;
			const std::filesystem::path path = existing.value_or(editorContext->projectManager->GetProjectRoot() / "Assets" / "Physics" / "Materials" / (TerraEngine::ToHexString(*id) + std::string(kPhysicsMaterialExtension)));
			const auto write = TerraEngine::AssetPipeline::WritePhysicsMaterialAsset(path, material);
			nlohmann::json receipt = {{"correlationId", request.correlationId}, {"worldGeneration", world.worldGeneration}, {"tick", tick}, {"accepted", write.published}, {"idempotencyDisposition", "new"}, {"diagnostics", nlohmann::json::array({{{"message", write.message}}})}};
			(void)idempotencyStore.PublishResult(request.clientId, operationName, request.idempotencyKey, receipt, true);
			return write.published ? succeed("Published the validated physics material atomically.", receipt) : reject(ErrorCategory::Validation, write.message);
		}

		if (operation == AutomationOperation("physics.collider.cook"))
		{
			if (!projectGenerationMatches() || !approvalMatches()) return reject(ErrorCategory::Conflict, "Collider-cook approval fingerprint or project generation is stale.");
			const IdempotencyDecision decision = beginIdempotency();
			if (const auto prior = replayOrConflict(decision)) return *prior;
			const nlohmann::json sourceValidation = ValidateProjectRelativePath(
				nlohmann::json{{"sourceAsset", request.arguments.value("sourceAsset", std::string{})}},
				"sourceAsset", request.requestId);
			if (!sourceValidation.value("ok", false))
			{
				return rejectAfterReservation(ErrorCategory::Validation,
					sourceValidation.value("message", "Collider source path must stay inside the active project."));
			}
			const std::filesystem::path relativeSource = sourceValidation["path"].get<std::string>();
			if (relativeSource.empty() || relativeSource.begin()->string() != "Assets")
			{
				return rejectAfterReservation(ErrorCategory::Validation,
					"Collider cooking requires a project-relative source asset under Assets.");
			}
			const std::filesystem::path source = editorContext->projectManager->GetProjectRoot() / relativeSource;
			if(!std::filesystem::exists(source) || !std::filesystem::is_regular_file(source)
				|| source.extension() != TerraEngine::Physics::kColliderRecipeExtension)
			{
				return rejectAfterReservation(ErrorCategory::Validation, "sourceAsset must name an existing .tecollider recipe under project Assets.");
			}
			TerraEngine::Physics::ColliderRecipe recipe;
			TerraEngine::Physics::ColliderRecipeValidationResult recipeValidation;
			try
			{
				std::ifstream recipeStream(source);
				nlohmann::json recipeDocument;
				recipeStream >> recipeDocument;
				if(!TerraEngine::Physics::ColliderRecipe::LoadJson(recipeDocument, recipe, recipeValidation))
					return rejectAfterReservation(ErrorCategory::Validation, recipeValidation.message);
			}
			catch(const std::exception& exception)
			{
				return rejectAfterReservation(ErrorCategory::Validation, std::string("Collider recipe JSON is invalid: ") + exception.what());
			}
			const std::string requestedShape = request.arguments["recipe"].value("shape", std::string{});
			if(requestedShape != TerraEngine::Physics::ToString(recipe.kind))
				return rejectAfterReservation(ErrorCategory::Conflict, "Requested cook shape no longer matches the authored collider recipe.");
			const auto countVertices = [&](const auto& self, const TerraEngine::Physics::ColliderRecipe& value) -> std::size_t {
				std::size_t count = value.vertices.size();
				for(const TerraEngine::Physics::ColliderRecipeCompoundChild& child : value.compoundChildren)
					if(child.recipe) count += self(self, *child.recipe);
				return count;
			};
			const std::size_t maximumVertices = request.arguments["recipe"].value("maxVertices", 1'000'000U);
			if(countVertices(countVertices, recipe) > maximumVertices)
				return rejectAfterReservation(ErrorCategory::OverBudget, "Collider recipe exceeds the approved maximum vertex count.");
			const AutomationJobSnapshot job = jobService.Create({"physics_cook", std::string(operationName), request.requestId, request.correlationId, request.idempotencyKey, true});
			if(job.jobId.empty()) return rejectAfterReservation(ErrorCategory::QueueFull, "Physics cook job retention is full.");
			(void)jobService.SetState(job.jobId, AutomationJobState::Preparing, "validating_source");

			// AssetDatabase owns the task, child compiler process, cancellation token,
			// last-known-good preservation, and main-thread publication boundary.
			AssetDatabase::Instance().ImportAssetWithProgress(source, true);
			activePhysicsCookJobs[job.jobId] = {
				std::filesystem::absolute(source).lexically_normal(), request.state.projectGeneration,
				request.clientId, std::string(operationName), request.idempotencyKey, request.correlationId
			};
			(void)jobService.SetState(job.jobId, AutomationJobState::Running, "cooking");
			nlohmann::json receipt = {{"correlationId", request.correlationId}, {"worldGeneration", world.worldGeneration}, {"tick", tick}, {"accepted", true}, {"idempotencyDisposition", "new"}, {"jobId", job.jobId}, {"diagnostics", nlohmann::json::array()}};
			// The first receipt is replayable but non-terminal. Atomic asset
			// publication promotes the same record to terminal during job polling.
			(void)idempotencyStore.PublishResult(request.clientId, operationName, request.idempotencyKey, receipt, false);
			return succeed("Created a durable cancellable collider-cook receipt.", receipt);
		}

		if (IsOperationOneOf(operation, {
			AutomationOperation("physics.body.apply_force"), AutomationOperation("physics.body.apply_impulse"),
			AutomationOperation("physics.body.set_velocity"), AutomationOperation("physics.body.teleport"),
			AutomationOperation("physics.character.move"), AutomationOperation("physics.character.jump")}))
		{
			if (!world.IsValid()) return reject(ErrorCategory::Unavailable, "No active physics world is available for runtime commands.");
			if (!request.arguments["worldGeneration"].is_number_unsigned() || request.arguments["worldGeneration"].get<std::uint32_t>() != world.worldGeneration)
				return reject(ErrorCategory::Conflict, "Runtime command world generation is stale.");
			const IdempotencyDecision decision = beginIdempotency();
			if (const auto prior = replayOrConflict(decision)) return *prior;
			TerraEngine::UUID objectId{};
			std::uint32_t componentGeneration = 0U;
			if (const auto error = parseTarget(request.arguments["target"], objectId, componentGeneration))
			{
				abandonIdempotency();
				return *error;
			}
			const auto object = ObjectManager::instance().getObject(objectId);
			if (!object) return rejectAfterReservation(ErrorCategory::MissingTarget, "Runtime command target object does not exist.");
			const std::uint32_t currentComponentGeneration = TerraEngine::CompressObjectGeneration(ObjectManager::instance().getObjectComponentGeneration(*object));
			if (currentComponentGeneration != componentGeneration) return rejectAfterReservation(ErrorCategory::Conflict, "Runtime command component generation is stale.");
			PhysicsCommandMetadata metadata;
			metadata.producerId = 0x4D435050485953ull;
			metadata.producerSequence = nextTransactionId++;
			metadata.correlationId = request.correlationId;
			metadata.idempotencyKey = request.idempotencyKey;
			metadata.requestFingerprint = StableFingerprint(request.arguments.dump());
			PhysicsCommand command;
			if (operation == AutomationOperation("physics.character.move") || operation == AutomationOperation("physics.character.jump"))
			{
				const auto characters = physics.LatestCharacterStatePacket();
				if (!characters || !characters->states) return rejectAfterReservation(ErrorCategory::MissingTarget, "No character snapshot is available.");
				const auto found = std::find_if(characters->states->begin(), characters->states->end(), [&](const PhysicsCharacterState& state) { return state.objectId == objectId && state.componentGeneration == componentGeneration; });
				if (found == characters->states->end()) return rejectAfterReservation(ErrorCategory::MissingTarget, "Character target is not present in the immutable snapshot.");
				const PhysicsCharacterCommandTarget target{world, found->handle, objectId, found->objectGeneration, componentGeneration};
				if (operation == AutomationOperation("physics.character.move"))
				{
					const auto velocity = ReadPhysicsVector(request.arguments["desiredVelocity"]);
					if (!velocity) return rejectAfterReservation(ErrorCategory::Validation, "desiredVelocity must contain three finite numbers.");
					command = PhysicsCommand::MakeCharacterMove(target, *velocity, metadata);
				}
				else
				{
					const CharacterControllerComponent* authored = ObjectManager::instance().GetComponent<CharacterControllerComponent>(*object);
					command = PhysicsCommand::MakeCharacterJump(target, authored ? authored->jumpSpeed : 5.0F, metadata);
				}
			}
			else
			{
				const std::uint32_t objectGeneration = TerraEngine::CompressObjectGeneration(ObjectManager::instance().getObjectLifetimeGeneration(*object));
				const PhysicsBodyCommandTarget target{world, objectId, objectGeneration, componentGeneration};
				if (operation == AutomationOperation("physics.body.teleport"))
				{
					const auto position = ReadPhysicsVector(request.arguments["position"]);
					if (!position) return rejectAfterReservation(ErrorCategory::Validation, "position must contain three finite numbers.");
					command = PhysicsCommand::MakeTeleport(target, PhysicsPose{*position, {0.0F, 0.0F, 0.0F, 1.0F}}, {}, {}, metadata);
				}
				else
				{
					const auto value = ReadPhysicsVector(request.arguments["vector"]);
					if (!value) return rejectAfterReservation(ErrorCategory::Validation, "vector must contain three finite numbers.");
					if (operation == AutomationOperation("physics.body.apply_force")) command = PhysicsCommand::MakeContinuousForce(target, *value, metadata);
					else if (operation == AutomationOperation("physics.body.apply_impulse")) command = PhysicsCommand::MakeImpulse(target, *value, metadata);
					else command = PhysicsCommand::MakeSetVelocity(target, *value, {}, metadata);
				}
			}
			const PhysicsCommandReceipt commandReceipt = physics.SubmitCommand(std::move(command));
			nlohmann::json receipt = {{"correlationId", request.correlationId}, {"worldGeneration", world.worldGeneration}, {"tick", commandReceipt.effectiveTick}, {"accepted", commandReceipt.accepted}, {"commandId", commandReceipt.commandId}, {"idempotencyDisposition", commandReceipt.duplicateIdempotency ? "replay" : "new"}, {"diagnostics", nlohmann::json::array({{{"reason", std::string(ToString(commandReceipt.rejectionReason))}}})}};
			(void)idempotencyStore.PublishResult(request.clientId, operationName, request.idempotencyKey, receipt, true);
			return commandReceipt.accepted ? succeed("Queued typed physics command for fixed-step commit.", receipt) : reject(commandReceipt.idempotencyConflict ? ErrorCategory::Conflict : ErrorCategory::QueueFull, "Physics command queue rejected the request.");
		}

		return reject(ErrorCategory::UnsupportedCapability, "Physics capability has no bound implementation.");
	}

	nlohmann::json EditorAutomationService::ExecuteProviderHandler(AutomationOperationId operation, std::string_view operationName, const nlohmann::json& arguments, WriteMode writeMode, const std::string& requestId)
	{
		const AssistantCapability* capability = FindCapability(operationName);
		if (capability == nullptr)
		{
			return BuildError(requestId, ErrorCategory::UnsupportedCapability, "Unknown editor automation capability.");
		}
		if (capability->mutatesState && writeMode != WriteMode::WritesEnabled)
		{
			return BuildError(requestId, ErrorCategory::Permission, "The bridge is in read-only mode.");
		}
		if (!arguments.is_object())
		{
			return BuildError(requestId, ErrorCategory::Validation, "Tool arguments must be a JSON object.");
		}
		if (operation == AutomationOperation("activity.list") && bridgeToolCallback)
		{
			return bridgeToolCallback(nlohmann::json{{"name", operationName}, {"arguments", arguments}, {"requestId", requestId}});
		}
		if (operation == AutomationOperation("scene.list_objects")) return HandleSceneListObjects(arguments, requestId);
		if (operation == AutomationOperation("scene.open"))
		{
			nlohmann::json validation = ValidateProjectRelativePath(arguments, "path", requestId);
			if (!validation.value("ok", false)) return validation;
			const std::filesystem::path projectRelativePath = validation["path"].get<std::string>();
			if (projectRelativePath.empty() || projectRelativePath.begin()->string() != "Assets"
				|| projectRelativePath.extension() != ".tescene")
			{
				return BuildError(requestId, ErrorCategory::Validation,
					"Scene path must name a .tescene file under Assets.", projectRelativePath.generic_string());
			}
			// The live editor configures SceneManager with the Assets directory as
			// its root. MCP paths are project-relative, so remove that one boundary
			// component before loading instead of accidentally resolving Assets/Assets.
			const std::filesystem::path assetRelativePath = projectRelativePath.lexically_relative("Assets");
			const TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene(assetRelativePath);
			if (sceneId == TerraEngine::EMPTY_UUID || !SceneManager::Instance().SetActiveScene(sceneId))
			{
				return BuildError(requestId, ErrorCategory::MissingTarget, "Scene could not be loaded.", projectRelativePath.generic_string());
			}
			UndoRedoSystem::Instance().Clear();
			return BuildSuccess(requestId, "Opened scene.", {{"path", projectRelativePath.generic_string()}});
		}
		if (operation == AutomationOperation("scene.save")) return HandleSceneSave(arguments, requestId);
		if (operation == AutomationOperation("object.create")) return HandleObjectCreate(arguments, requestId);
		if (operation == AutomationOperation("object.delete")) return HandleObjectDelete(arguments, requestId);
		if (operation == AutomationOperation("object.select")) return HandleObjectSelect(arguments, requestId);
		if (operation == AutomationOperation("object.set_metadata")) return HandleObjectSetMetadata(arguments, requestId);
		if (operation == AutomationOperation("object.set_parent")) return HandleObjectSetParent(arguments, requestId);
		if (operation == AutomationOperation("component.add")) return HandleComponentAdd(arguments, requestId);
		if (operation == AutomationOperation("component.remove")) return HandleComponentRemove(arguments, requestId);
		if (operation == AutomationOperation("component.get")) return HandleComponentGet(arguments, requestId);
		if (operation == AutomationOperation("component.set_property")) return HandleComponentSetProperty(arguments, requestId);
		if (operation == AutomationOperation("asset.list")) return HandleAssetList(arguments, requestId);
		if (operation == AutomationOperation("asset.import")) return HandleAssetImport(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.settings.get")) return HandleVirtualGeometrySettingsGet(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.settings.set")) return HandleVirtualGeometrySettingsSet(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.settings.reset")) return HandleVirtualGeometrySettingsReset(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.rebuild")) return HandleVirtualGeometryRebuild(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.inspect")) return HandleVirtualGeometryInspect(arguments, requestId);
		if (operation == AutomationOperation("asset.prepare_delete")) return HandleAssetPrepareDelete(arguments, requestId);
		if (operation == AutomationOperation("export.validate")) return HandleExportValidate(arguments, requestId);
		if (operation == AutomationOperation("export.start")) return HandleExportStart(arguments, requestId);
		if (operation == AutomationOperation("export.cancel")) return HandleExportCancel(arguments, requestId);
		if (operation == AutomationOperation("game_view.capture")) return HandleGameViewCapture(arguments, requestId);
		if (operation == AutomationOperation("camera.capture_object")) return HandleCameraObjectCapture(arguments, requestId);
		if (operation == AutomationOperation("camera.capture_custom")) return HandleCustomCameraCapture(arguments, requestId);
		if (operation == AutomationOperation("renderer.virtualGeometry.stats")) return HandleRendererVirtualGeometryStats(arguments, requestId);
		if (operation == AutomationOperation("renderer.recovery.safe_save_project"))
		{
			std::string error;
			if (editorContext == nullptr || editorContext->projectManager == nullptr
				|| !editorContext->projectManager->SaveProjectDataForRendererRecovery(&error))
			{
				return BuildError(requestId, ErrorCategory::Internal, error.empty() ? "Renderer-recovery project save failed." : error);
			}
			const RendererRecoveryProjectActionState& state = editorContext->projectManager->GetRendererRecoveryActionState();
			return BuildSuccess(requestId, "Saved renderer-independent project data.", {
				{"projectRoot", state.lastSafeSaveRoot.generic_string()},
				{"message", state.lastSafeSaveMessage},
				{"rendererStateUsed", false}
			});
		}
		if (operation == AutomationOperation("project.templates.list"))
		{
			return BuildSuccess(requestId, "Listed project templates.", {{"templates", nlohmann::json::array({
				{{"id", "blank"}, {"displayName", "Blank"}},
				{{"id", "2d"}, {"displayName", "2D"}},
				{{"id", "3d"}, {"displayName", "3D"}}
			})}});
		}
		if (operation == AutomationOperation("project.state"))
		{
			nlohmann::json state = BuildProjectState();
			const AutomationStateSnapshot snapshot = BuildAutomationStateSnapshot();
			state["projectGeneration"] = snapshot.projectGeneration;
			state["sceneGeneration"] = snapshot.sceneGeneration;
			return BuildSuccess(requestId, "Read project state.", std::move(state));
		}
		const auto playStateData = []() {
			const ScriptingSystem& scripting = ScriptingSystem::Instance();
			const std::string currentState = !scripting.IsRunning() ? "stopped" : (scripting.IsPaused() ? "paused" : "playing");
			return nlohmann::json{{"currentState", currentState}, {"pendingTransition", nullptr}};
		};
		if (operation == AutomationOperation("play.state"))
			return BuildSuccess(requestId, "Read Play Mode state.", playStateData());
		if (IsOperationOneOf(operation, {
			AutomationOperation("play.enter"), AutomationOperation("play.pause"),
			AutomationOperation("play.resume"), AutomationOperation("play.step"),
			AutomationOperation("play.exit")}))
		{
			nlohmann::json state = playStateData();
			const std::string current = state["currentState"].get<std::string>();
			const std::string expected = arguments.value("expectedState", current);
			const auto playError = [this, &requestId, &state](ErrorCategory category, std::string_view message) {
				nlohmann::json error = BuildError(requestId, category, message);
				error["data"] = state;
				return error;
			};
			if (expected != current) return playError(ErrorCategory::Conflict, "Play Mode state changed after request preparation.");
			if (operation == AutomationOperation("play.enter"))
			{
				if (current != "stopped") return playError(ErrorCategory::Conflict, "Play Mode is already running.");
				if (editorContext == nullptr || editorContext->projectManager == nullptr || !editorContext->projectManager->HasActiveProject() || SceneManager::Instance().GetActiveScene() == nullptr)
					return playError(ErrorCategory::Unavailable, "A project with an active saved scene is required.");
				if(mainEditor == nullptr || !mainEditor->StartPlayMode())
					return playError(ErrorCategory::Unavailable, "Play Mode could not create its fresh Jolt 5.6 physics world.");
			}
			else if (operation == AutomationOperation("play.pause"))
			{
				if (current != "playing") return playError(ErrorCategory::Conflict, "Only active Play Mode can be paused.");
				if(mainEditor == nullptr || !mainEditor->SetPlayModePaused(true)) return playError(ErrorCategory::Conflict, "Physics and scripting could not enter the same paused boundary.");
			}
			else if (operation == AutomationOperation("play.resume"))
			{
				if (current != "paused") return playError(ErrorCategory::Conflict, "Only paused Play Mode can be resumed.");
				if(mainEditor == nullptr || !mainEditor->SetPlayModePaused(false)) return playError(ErrorCategory::Conflict, "Physics and scripting could not resume together.");
			}
			else if (operation == AutomationOperation("play.step"))
			{
				if (current != "paused") return playError(ErrorCategory::Conflict, "Single-step requires paused Play Mode.");
				if(mainEditor == nullptr || !mainEditor->StepPlayMode()) return playError(ErrorCategory::Conflict, "The fixed-step queue rejected this Next request.");
				state = playStateData();
				state["stepsCompleted"] = 1;
				return BuildSuccess(requestId, "Scheduled exactly one simulation frame.", std::move(state));
			}
			else if (operation == AutomationOperation("play.exit"))
			{
				if (current == "stopped") return playError(ErrorCategory::Conflict, "Play Mode is not running.");
				if(mainEditor == nullptr) return playError(ErrorCategory::Unavailable, "The native editor lifecycle coordinator is unavailable.");
				mainEditor->StopPlayMode();
				state = playStateData();
				state["restoration"] = {{"completed", true}, {"partialFailures", nlohmann::json::array()}};
				return BuildSuccess(requestId, "Exited Play Mode and restored authored state.", std::move(state));
			}
			return BuildSuccess(requestId, "Changed Play Mode state.", playStateData());
		}
		if (operation == AutomationOperation("jobs.list"))
		{
			nlohmann::json jobs = nlohmann::json::array();
			for (const AutomationJobSnapshot& job : jobService.List(arguments.value("limit", 100u))) jobs.push_back(SerializeAutomationJob(job));
			return BuildSuccess(requestId, "Listed retained automation jobs.", {{"jobs", jobs}, {"oldestRetainedSequence", jobService.OldestRetainedSequence()}});
		}
		if (operation == AutomationOperation("jobs.get"))
		{
			const std::string jobId = arguments.value("jobId", std::string{});
			RefreshPhysicsCookJob(jobId);
			if (jobId == activeScriptCompilationJobId)
			{
				const ScriptCompilationStatus& status = ScriptCompilationSubsystem::Instance().GetStatus();
				if (status.state == ScriptCoordinatorState::Queued)
				{
					(void)jobService.PublishProgress(jobId, status.progress, "queued");
				}
				else if (status.state == ScriptCoordinatorState::Building || status.state == ScriptCoordinatorState::Reloading)
				{
					(void)jobService.SetState(jobId, AutomationJobState::Running, status.state == ScriptCoordinatorState::Building ? "compiling" : "reloading");
					(void)jobService.PublishProgress(jobId, status.progress, status.state == ScriptCoordinatorState::Building ? "compiling" : "reloading");
				}
				else if (status.state == ScriptCoordinatorState::Ready)
				{
					(void)jobService.CompleteSucceeded(jobId, {{"assemblyPath", status.activeAssemblyPath.generic_string()}, {"attachableTypeCount", status.attachableTypes.size()}});
				}
				else if (status.state == ScriptCoordinatorState::Failed)
				{
					for (const ScriptCompilerDiagnostic& diagnostic : status.diagnostics)
					{
						(void)jobService.AddDiagnostic(jobId, {diagnostic.severity == "error" ? AutomationLogSeverity::Error : AutomationLogSeverity::Warning, "compiler", diagnostic.message,
							{{"code", diagnostic.code}, {"file", diagnostic.file.generic_string()}, {"line", diagnostic.line}, {"column", diagnostic.column}}});
					}
					(void)jobService.CompleteFailed(jobId, {{"summary", status.lastSummary}});
				}
				else if (status.state == ScriptCoordinatorState::Canceled)
				{
					(void)jobService.CompleteCanceled(jobId, {{"boundary", "before_assembly_publication"}});
				}
			}
			if (jobId == activeProfilerCaptureJobId && editorContext != nullptr && editorContext->editorProfiler)
			{
				EditorProfilerService& profiler = *editorContext->editorProfiler;
				if (profiler.IsBusy())
				{
					(void)jobService.SetState(jobId, AutomationJobState::Running, "saving_capture");
					(void)jobService.PublishProgress(jobId, 0.75, "saving_capture");
				}
				else
				{
					const std::vector<EditorProfilerCaptureArtifact> artifacts = profiler.GetAutomationCaptureList();
					const auto created = std::find_if(artifacts.begin(), artifacts.end(), [this](const EditorProfilerCaptureArtifact& artifact) {
						return !profilerArtifactIdsBeforeStop.contains(artifact.artifactId);
					});
					if (created != artifacts.end())
					{
						(void)jobService.AddArtifact(jobId, {created->artifactId, "profiler_capture", {}, "application/json", {
							{"name", created->artifact.fileName},
							{"frameCount", created->artifact.frameCount},
							{"status", created->artifact.status}
						}});
						(void)jobService.CompleteSucceeded(jobId, {{"artifactId", created->artifactId}});
						activeProfilerCaptureJobId.clear();
						profilerArtifactIdsBeforeStop.clear();
					}
					else if (!profiler.GetLastError().empty())
					{
						(void)jobService.CompleteFailed(jobId, {{"error", profiler.GetLastError()}});
						activeProfilerCaptureJobId.clear();
						profilerArtifactIdsBeforeStop.clear();
					}
				}
			}
			if (jobId == activeProfilerLoadJobId && editorContext != nullptr && editorContext->editorProfiler)
			{
				EditorProfilerService& profiler = *editorContext->editorProfiler;
				if (profiler.IsBusy())
				{
					(void)jobService.SetState(jobId, AutomationJobState::Running, "loading_capture");
					(void)jobService.PublishProgress(jobId, 0.75, "loading_capture");
				}
				else if (!profiler.GetLastError().empty())
				{
					(void)jobService.CompleteFailed(jobId, {{"error", profiler.GetLastError()}});
					activeProfilerLoadJobId.clear();
					activeProfilerLoadArtifactId.clear();
				}
				else
				{
					(void)jobService.CompleteSucceeded(jobId, {{"artifactId", activeProfilerLoadArtifactId}});
					activeProfilerLoadJobId.clear();
					activeProfilerLoadArtifactId.clear();
				}
			}
			const auto job = jobService.Get(jobId);
			return job ? BuildSuccess(requestId, "Read automation job.", {{"job", SerializeAutomationJob(*job)}})
				: BuildError(requestId, ErrorCategory::MissingTarget, "Automation job was not found.");
		}
		if (operation == AutomationOperation("jobs.cancel"))
		{
			const std::string jobId = arguments.value("jobId", std::string{});
			if (!jobService.RequestCancellation(jobId)) return BuildError(requestId, ErrorCategory::Conflict, "Job cannot be canceled.", jobId);
			if(const auto cook = activePhysicsCookJobs.find(jobId); cook != activePhysicsCookJobs.end())
			{
				// AssetDatabase owns the process handle. Forwarding cancellation there
				// guarantees the compiler cannot publish a staged collider afterward.
				(void)AssetDatabase::Instance().CancelImport(cook->second.sourcePath, false);
			}
			if (jobId == activeScriptCompilationJobId)
			{
				(void)ScriptCompilationSubsystem::Instance().RequestCancellation();
			}
			return BuildSuccess(requestId, "Requested cooperative job cancellation.", {{"job", SerializeAutomationJob(*jobService.Get(jobId))}});
		}
		if (operation == AutomationOperation("logs.query"))
		{
			StructuredLogQuery query;
			query.category = arguments.value("category", std::string{});
			query.source = arguments.value("source", std::string{});
			query.text = arguments.value("text", std::string{});
			query.correlationId = arguments.value("correlationId", std::string{});
			query.requestId = arguments.value("requestId", std::string{});
			query.jobId = arguments.value("jobId", std::string{});
			query.cursor = arguments.value("cursor", std::string{});
			query.pageSize = arguments.value("pageSize", 100u);
			const StructuredLogPage page = logStore.Query(query);
			if (!page.valid) return BuildError(requestId, ErrorCategory::Validation, page.error);
			nlohmann::json records = nlohmann::json::array();
			for (const StructuredLogRecord& record : page.records)
				records.push_back({{"sequence", record.sequence}, {"category", record.category}, {"source", record.source}, {"correlationId", record.correlationId}, {"requestId", record.requestId}, {"jobId", record.jobId}, {"message", record.message}, {"details", record.details}});
			return BuildSuccess(requestId, "Queried structured logs.", {{"records", records}, {"nextCursor", page.nextCursor}, {"hasMore", page.hasMore}, {"droppedBeforeCursor", page.droppedBeforeCursor}, {"oldestRetainedSequence", page.oldestRetainedSequence}});
		}
		if (operation == AutomationOperation("history.state"))
		{
			const UndoRedoSystem& history = UndoRedoSystem::Instance();
			return BuildSuccess(requestId, "Read editor history state.", {
				{"canUndo", history.CanUndo()}, {"canRedo", history.CanRedo()},
				{"undoDepth", history.GetUndoDepth()}, {"redoDepth", history.GetRedoDepth()},
				{"undoLabel", history.GetUndoLabel()}, {"redoLabel", history.GetRedoLabel()},
				{"sequence", history.GetSequence()}
			});
		}
		if (operation == AutomationOperation("history.undo") || operation == AutomationOperation("history.redo"))
		{
			UndoRedoSystem& history = UndoRedoSystem::Instance();
			if (!arguments.contains("expectedSequence") || !arguments["expectedSequence"].is_number_unsigned())
				return BuildError(requestId, ErrorCategory::Validation, "expectedSequence is required.");
			if (arguments["expectedSequence"].get<std::uint64_t>() != history.GetSequence())
				return BuildError(requestId, ErrorCategory::StaleTarget, "Editor history changed after the request was prepared.");
			const bool undo = operation == AutomationOperation("history.undo");
			if ((undo && !history.CanUndo()) || (!undo && !history.CanRedo()))
				return BuildError(requestId, ErrorCategory::Conflict, undo ? "Nothing is available to undo." : "Nothing is available to redo.");
			const std::string historyId = undo ? history.GetUndoHistoryId() : history.GetRedoHistoryId();
			if (undo) history.Undo(); else history.Redo();
			return BuildSuccess(requestId, undo ? "Undid editor transaction." : "Redid editor transaction.", {
				{"historyId", historyId}, {"sequence", history.GetSequence()}, {"visibilityBoundary", "post_editor_automation_flush"}
			});
		}
		if (operation == AutomationOperation("transaction.execute"))
		{
			if (arguments.value("cancelRequested", false))
			{
				nlohmann::json canceled = BuildError(requestId, ErrorCategory::Canceled, "Transaction was canceled before protected commit.");
				canceled["data"] = {{"cancellationBoundary", "before_commit"}};
				return canceled;
			}
			if (arguments.contains("expectedSceneGeneration")
				&& arguments["expectedSceneGeneration"].get<std::uint64_t>() != BuildAutomationStateSnapshot().sceneGeneration)
				return BuildError(requestId, ErrorCategory::StaleTarget, "Scene generation changed after transaction preparation.");
			ObjectManager& objects = ObjectManager::instance();
			const std::string transactionId = "transaction-" + std::to_string(nextTransactionId++);
			PreparedAutomationTransaction prepared = PrepareAutomationTransaction(arguments, objects, transactionId);
			if (!prepared.valid) return BuildError(requestId, prepared.error, prepared.message);

			// History commands capture before-state while the transaction is still
			// wholly unapplied. They are published only after the engine commit succeeds.
			std::vector<std::unique_ptr<ICommand>> commands;
			std::unordered_map<std::size_t, std::optional<TerraEngine::UUID>> oldParents;
			std::unordered_map<std::size_t, nlohmann::json> oldComponentPayloads;
			std::vector<std::string> affectedTargets;
			for (std::size_t index = 0; index < arguments["operations"].size(); ++index)
			{
				const nlohmann::json& operation = arguments["operations"][index];
				const std::string kind = operation["kind"].get<std::string>();
				if (kind == "object.create" || kind == "selection.set") continue;
				const nlohmann::json& targetValue = operation["target"];
				if (targetValue.is_object() && targetValue.contains("localId")) continue;
				const EditorTarget target = ResolveTarget(targetValue);
				if (!target.resolved || !target.entity) continue;
				const std::optional<Object> targetObject = objects.getObject(*target.entity);
				if (!targetObject) continue;

				const TerraEngine::UUID targetId = objects.getUUID(*targetObject);
				const std::string targetText = TerraEngine::ToHexString(targetId);
				if (std::find(affectedTargets.begin(), affectedTargets.end(), targetText) == affectedTargets.end()) affectedTargets.push_back(targetText);
				if (kind == "object.delete")
				{
					commands.push_back(std::make_unique<DeleteObjectCommand>(*target.entity));
				}
				else if (kind == "object.set_metadata")
				{
					// Store only fields written by this operation. Capturing every metadata
					// field would let a later command overwrite an earlier command during redo.
					nlohmann::json oldMetadata = nlohmann::json::object();
					nlohmann::json newMetadata = nlohmann::json::object();
					if (operation.contains("name")) { oldMetadata["name"] = objects.getObjectName(*targetObject); newMetadata["name"] = operation["name"]; }
					if (operation.contains("tag")) { oldMetadata["tag"] = objects.getObjectTag(*targetObject); newMetadata["tag"] = operation["tag"]; }
					if (operation.contains("layer")) { oldMetadata["layer"] = objects.getObjectLayer(*targetObject); newMetadata["layer"] = operation["layer"]; }
					if (operation.contains("active")) { oldMetadata["active"] = objects.isObjectActive(*targetObject); newMetadata["active"] = operation["active"]; }
					if (operation.contains("static")) { oldMetadata["static"] = objects.isObjectStatic(*targetObject); newMetadata["static"] = operation["static"]; }
					commands.push_back(std::make_unique<SetObjectMetadataCommand>(*target.entity, std::move(oldMetadata), std::move(newMetadata)));
				}
				else if (kind == "object.set_parent")
				{
					const std::optional<Object> oldParent = objects.getParent(*targetObject);
					oldParents[index] = oldParent ? std::optional<TerraEngine::UUID>(objects.getUUID(*oldParent)) : std::nullopt;
				}
				else if (kind == "component.remove")
				{
					commands.push_back(std::make_unique<RemoveComponentCommand>(*target.entity, operation.value("component", std::string{})));
				}
				else if (kind == "component.set_property")
				{
					std::lock_guard registryLock(ComponentAccessRegistryMutex());
					const auto component = ComponentAccessRegistry().find(operation.value("component", std::string{}));
					if (component != ComponentAccessRegistry().end())
					{
						oldComponentPayloads[index] = component->second.entitySerialize(objects.getRegistry(), *target.entity);
					}
				}
			}

			const TerraEngine::Scene::TransactionResult commit = TerraEngine::Scene::CommitTransaction(objects, prepared.transaction);
			if (commit.status != TerraEngine::Scene::TransactionStatus::Committed)
			{
				nlohmann::json error = BuildError(
					requestId,
					commit.status == TerraEngine::Scene::TransactionStatus::Conflicted ? ErrorCategory::Conflict : ErrorCategory::Validation,
					commit.message);
				error["data"]["transactionId"] = transactionId;
				error["data"]["conflicts"] = nlohmann::json::array();
				for (const TerraEngine::Scene::TransactionConflictRecord& conflict : commit.conflicts)
				{
					error["data"]["conflicts"].push_back({
						{"reason", TerraEngine::Scene::ToString(conflict.reason)},
						{"operationIndex", conflict.operationIndex},
						{"target", TerraEngine::ToHexString(conflict.targetObject)},
						{"component", conflict.componentType},
						{"property", conflict.propertyName},
						{"message", conflict.humanMessage}
					});
				}
				return error;
			}

			const auto resolveCommittedRef = [&](const TerraEngine::Scene::TransactionObjectRef& ref) -> std::optional<Object> {
				TerraEngine::UUID id = ref.objectId;
				if (ref.UsesLocalObject())
				{
					const auto created = commit.createdObjects.find(ref.localObjectId);
					if (created == commit.createdObjects.end()) return std::nullopt;
					id = created->second;
				}
				return objects.getObject(id);
			};

			// One recursive creation command owns each newly created hierarchy root.
			// The final parent relationship must be inspected after commit because a
			// transaction may create an object first and parent it in a later operation.
			// Looking only at CreateObject::parent would then snapshot the child twice:
			// once recursively under its parent and once as an independent command.
			std::unordered_set<TerraEngine::UUID> createdObjectIds;
			for (const auto& [localId, objectId] : commit.createdObjects)
			{
				(void)localId;
				createdObjectIds.insert(objectId);
			}
			for (const TerraEngine::Scene::TransactionWriteOperation& operation : prepared.transaction.writeSet.operations)
			{
				if (operation.kind != TerraEngine::Scene::TransactionWriteKind::CreateObject) continue;
				const auto created = commit.createdObjects.find(operation.createdLocalObjectId);
				if (created == commit.createdObjects.end()) continue;
				const std::optional<Object> object = objects.getObject(created->second);
				if (!object) continue;
				const std::optional<Object> finalParent = objects.getParent(*object);
				const std::optional<TerraEngine::UUID> finalParentId = finalParent
					? std::optional<TerraEngine::UUID>(objects.getUUID(*finalParent))
					: std::nullopt;
				const std::string id = TerraEngine::ToHexString(created->second);
				if (std::find(affectedTargets.begin(), affectedTargets.end(), id) == affectedTargets.end()) affectedTargets.push_back(id);
				if (finalParentId && createdObjectIds.contains(*finalParentId))
				{
					// A recursively serialized created ancestor owns this child and all of
					// its authored state for undo/redo replay.
					continue;
				}
				commands.push_back(std::make_unique<CreateObjectCommand>(object->EntityId()));
				if (finalParentId)
				{
					// SerializeObject intentionally records descendants, not an object's
					// external parent, so retain that relationship as a separate command.
					commands.push_back(std::make_unique<SetParentCommand>(object->EntityId(), std::nullopt, finalParentId));
				}
			}

			for (std::size_t index = 0; index < arguments["operations"].size(); ++index)
			{
				const nlohmann::json& operation = arguments["operations"][index];
				const std::string kind = operation["kind"].get<std::string>();
				if (kind != "object.set_parent" && kind != "component.add" && kind != "component.set_property") continue;
				const auto local = operation["target"].is_object() ? prepared.localObjectIds.find(operation["target"].value("localId", std::string{})) : prepared.localObjectIds.end();
				if (local != prepared.localObjectIds.end()) continue;
				const EditorTarget target = ResolveTarget(operation["target"]);
				if (!target.resolved || !target.entity) continue;
				if (kind == "object.set_parent")
				{
					std::optional<TerraEngine::UUID> newParent;
					if (operation.contains("parent") && !operation["parent"].is_null())
					{
						std::optional<Object> parent;
						if (operation["parent"].is_object())
						{
							parent = resolveCommittedRef(TerraEngine::Scene::LocalObject(prepared.localObjectIds.at(operation["parent"].value("localId", std::string{}))));
						}
						else
						{
							const EditorTarget parentTarget = ResolveTarget(operation["parent"]);
							if (parentTarget.resolved && parentTarget.entity) parent = objects.getObject(*parentTarget.entity);
						}
						if (parent) newParent = objects.getUUID(*parent);
					}
					commands.push_back(std::make_unique<SetParentCommand>(*target.entity, oldParents[index], newParent));
				}
				else if (kind == "component.add")
				{
					commands.push_back(std::make_unique<AddComponentCommand>(*target.entity, operation.value("component", std::string{})));
				}
				else
				{
					std::lock_guard registryLock(ComponentAccessRegistryMutex());
					const std::string componentName = operation.value("component", std::string{});
					const auto component = ComponentAccessRegistry().find(componentName);
					if (component != ComponentAccessRegistry().end())
					{
						commands.push_back(std::make_unique<SetComponentPayloadCommand>(*target.entity, componentName, oldComponentPayloads[index], component->second.entitySerialize(objects.getRegistry(), *target.entity)));
					}
				}
			}

			const std::string historyId = "history-" + transactionId;
			UndoRedoSystem::Instance().PushTransactionGroup(arguments.value("label", std::string("Automation transaction")), std::move(commands), historyId);
			if (prepared.selectionTarget && setSelectedEntity)
			{
				if (const std::optional<Object> selection = resolveCommittedRef(*prepared.selectionTarget)) setSelectedEntity(selection->EntityId());
			}
			SceneManager::Instance().ForceMarkActiveSceneDirty();
			return BuildSuccess(requestId, "Committed automation transaction.", {
				{"transactionId", transactionId}, {"historyId", historyId}, {"visibilityBoundary", "post_editor_automation_flush"},
				{"affectedTargets", affectedTargets}, {"appliedOperationCount", arguments["operations"].size()}, {"inverseDeltaCount", commit.inverseDeltas.size()}
			});
		}
		const auto projectAssetPath = [this](const std::string& value) -> std::optional<std::pair<std::string, std::filesystem::path>> {
			if (editorContext == nullptr || editorContext->projectManager == nullptr || !editorContext->projectManager->HasActiveProject()) return std::nullopt;
			const std::filesystem::path relative(value);
			if (relative.empty() || relative.is_absolute() || relative.extension().empty() || relative.string().find("..") != std::string::npos || relative.begin()->string() != "Assets") return std::nullopt;
			const std::filesystem::path root = editorContext->projectManager->GetProjectRoot();
			const std::filesystem::path absolute = (root / relative).lexically_normal();
			const std::filesystem::path assetRoot = (root / "Assets").lexically_normal();
			const std::string absoluteText = LowerAscii(absolute.generic_string());
			std::string assetRootText = LowerAscii(assetRoot.generic_string());
			if (!assetRootText.ends_with('/')) assetRootText.push_back('/');
			if (absoluteText.rfind(assetRootText, 0) != 0) return std::nullopt;
			return std::pair(relative.lexically_normal().generic_string(), absolute);
		};
		if (operation == AutomationOperation("asset.inspect"))
		{
			if (editorContext == nullptr || editorContext->assetDatabase == nullptr)
				return BuildError(requestId, ErrorCategory::Unavailable, "Asset database is unavailable.");
			const AssetOrganizationImpact impact = editorContext->assetDatabase->PrepareAssetOrganization(
				arguments.value("asset", std::string{}),
				arguments.value("operation", std::string("inspect")),
				arguments.value("destination", std::string{}));
			if (!impact.valid) return BuildError(requestId, ErrorCategory::Validation, impact.error);
			return BuildSuccess(requestId, "Prepared asset impact report.", {{"impact", {
				{"fingerprint", impact.fingerprint}, {"dependents", impact.dependents}, {"generatedChildren", impact.generatedChildren},
				{"repairPlan", impact.repairPlan}, {"rollbackJournalEstimate", impact.rollbackJournalEstimate}
			}}});
		}
		if (operation == AutomationOperation("asset.move") || operation == AutomationOperation("asset.rename") || operation == AutomationOperation("asset.duplicate"))
		{
			if (editorContext == nullptr || editorContext->assetDatabase == nullptr)
				return BuildError(requestId, ErrorCategory::Unavailable, "Asset database is unavailable.");
			const std::string sourceValue = operation == AutomationOperation("asset.rename") ? arguments.value("asset", std::string{}) : arguments.value("source", std::string{});
			std::string destinationValue;
			if (operation == AutomationOperation("asset.rename") && arguments.contains("name"))
				destinationValue = (std::filesystem::path(sourceValue).parent_path() / arguments["name"].get<std::string>()).generic_string();
			else destinationValue = arguments.value("destination", std::string{});
			const std::string assetOperation(operationName.substr(std::string("asset.").size()));
			const AssetOrganizationResult published = editorContext->assetDatabase->PublishAssetOrganization(
				sourceValue, destinationValue, assetOperation, arguments.value("approvedImpactFingerprint", std::string{}));
			if (!published.published)
			{
				const ErrorCategory category = published.errorCategory == "conflict" ? ErrorCategory::Conflict
					: published.errorCategory == "validation" ? ErrorCategory::Validation : ErrorCategory::Internal;
				nlohmann::json error = BuildError(requestId, category, published.error);
				error["data"] = {{"rollback", published.rollback}};
				return error;
			}
			return BuildSuccess(requestId, "Published asset organization operation.", {
				{"assetId", published.assetId}, {"path", published.path},
				{"repairedReferences", published.repairedReferences}, {"rollback", published.rollback}});
		}
		if (operation == AutomationOperation("prefab.create"))
		{
			const EditorTarget target = ResolveTarget(arguments.value("object", nlohmann::json{}));
			const auto path = projectAssetPath(arguments.value("path", std::string{}));
			if (!target.resolved || !path) return BuildError(requestId, ErrorCategory::Validation, "Prefab source or destination is invalid.");
			const PrefabAuthoringService::Result result = prefabAuthoringService.Create(*target.entity, path->first, path->second);
			if (!result.ok) return BuildError(requestId, ErrorCategory::Validation, result.message);
			return BuildSuccess(requestId, result.message, result.data);
		}
		if (IsOperationOneOf(operation, {
			AutomationOperation("prefab.instantiate"), AutomationOperation("prefab.overrides.inspect"),
			AutomationOperation("prefab.overrides.apply"), AutomationOperation("prefab.overrides.revert"),
			AutomationOperation("prefab.edit.open"), AutomationOperation("prefab.edit.close")}))
		{
			PrefabAuthoringService::Result result;
			if (operation == AutomationOperation("prefab.instantiate"))
				result = prefabAuthoringService.Instantiate(arguments.value("prefab", std::string{}), arguments.value("name", std::string{}));
			else if (operation == AutomationOperation("prefab.overrides.inspect"))
				result = prefabAuthoringService.InspectOverrides(arguments.value("instance", std::string{}));
			else if (operation == AutomationOperation("prefab.overrides.apply"))
				result = prefabAuthoringService.ApplyOverrides(arguments.value("instance", std::string{}),
					arguments.contains("expectedPrefabGeneration") ? std::optional<std::uint64_t>(arguments["expectedPrefabGeneration"].get<std::uint64_t>()) : std::nullopt);
			else if (operation == AutomationOperation("prefab.overrides.revert"))
				result = prefabAuthoringService.RevertOverrides(arguments.value("instance", std::string{}));
			else if (operation == AutomationOperation("prefab.edit.open"))
				result = prefabAuthoringService.OpenEditContext(arguments.value("prefab", std::string{}));
			else result = prefabAuthoringService.CloseEditContext(arguments.value("contextId", std::string{}));
			if (!result.ok)
			{
				const ErrorCategory category = result.errorCategory == "missing_target" ? ErrorCategory::MissingTarget
					: result.errorCategory == "conflict" ? ErrorCategory::Conflict
					: result.errorCategory == "stale_target" ? ErrorCategory::StaleTarget
					: result.errorCategory == "internal" ? ErrorCategory::Internal : ErrorCategory::Validation;
				nlohmann::json error = BuildError(requestId, category, result.message);
				if (!result.data.empty()) error["data"] = result.data;
				return error;
			}
			return BuildSuccess(requestId, result.message, result.data);
		}
		if (operation == AutomationOperation("script.templates.list"))
		{
			ScriptWorkspaceService workspace;
			nlohmann::json templates = nlohmann::json::array();
			for (const ScriptTemplateDescriptor& descriptor : workspace.ListApprovedTemplates())
			{
				templates.push_back({{"id", descriptor.id}, {"displayName", descriptor.displayName}, {"description", descriptor.description}});
			}
			return BuildSuccess(requestId, "Listed approved script templates.", {{"templates", std::move(templates)}});
		}
		if (operation == AutomationOperation("script.create"))
		{
			if (arguments.contains("contents") || arguments.contains("command"))
				return BuildError(requestId, ErrorCategory::Validation, "Script creation does not accept raw contents or process commands.");
			const ScriptCreationRequest creation{
				arguments.value("template", std::string{}), arguments.value("typeName", std::string{}),
				arguments.value("namespace", std::string{}), arguments.value("destination", std::string{})};
			ScriptCreationResult result;
			std::string error;
			ScriptCompilationSubsystem& compilation = ScriptCompilationSubsystem::Instance();
			if (compilation.GetWorkspaceService().GetProjectRoot() == editorContext->projectManager->GetProjectRoot())
			{
				if (!compilation.CreateApprovedScript(creation, result, &error))
					return BuildError(requestId, ErrorCategory::Validation, error);
			}
			else
			{
				ScriptWorkspaceService workspace;
				workspace.SetProjectRoot(editorContext->projectManager->GetProjectRoot());
				if (!workspace.CreateApprovedScript(creation, result, &error))
					return BuildError(requestId, ErrorCategory::Validation, error);
			}
			return BuildSuccess(requestId, "Created managed script.", {{"path", result.projectRelativePath.generic_string()}, {"qualifiedTypeName", result.qualifiedTypeName}});
		}
		if (operation == AutomationOperation("script.workspace.sync"))
		{
			ScriptWorkspaceService workspace;
			workspace.SetProjectRoot(editorContext->projectManager->GetProjectRoot());
			ScriptWorkspaceStatus status;
			std::string error;
			if (!workspace.SynchronizeWorkspace(status, &error)) return BuildError(requestId, ErrorCategory::Internal, error);
			return BuildSuccess(requestId, "Synchronized managed workspace.", {{"health", "healthy"}, {"projectPath", std::filesystem::relative(status.projectPath, editorContext->projectManager->GetProjectRoot()).generic_string()}});
		}
		if (operation == AutomationOperation("script.types.list"))
		{
			nlohmann::json types = nlohmann::json::array();
			for (const AttachableScriptType& type : ScriptCompilationSubsystem::Instance().GetStatus().attachableTypes)
				types.push_back({{"qualifiedName", type.qualifiedName}, {"sourcePath", type.sourcePath.generic_string()}});
			return BuildSuccess(requestId, "Listed attachable script types.", {{"types", std::move(types)}});
		}
		if (operation == AutomationOperation("script.compile"))
		{
			const AutomationJobSnapshot job = jobService.Create({"script_compile", "script.compile", requestId, arguments.value("correlationId", requestId), arguments.value("idempotencyKey", std::string{}), true});
			if (job.jobId.empty()) return BuildError(requestId, ErrorCategory::QueueFull, "Script compilation queue is full.");
			activeScriptCompilationJobId = job.jobId;
			ScriptCompilationSubsystem::Instance().RequestBuild(ScriptBuildReason::ManualRefresh);
			nlohmann::json accepted = BuildSuccess(requestId, "Accepted script compilation.", {{"jobId", job.jobId}, {"state", "queued"}, {"pollTool", "jobs.get"}, {"cancelTool", "jobs.cancel"}});
			accepted["status"] = "accepted";
			return accepted;
		}
		if (IsOperationOneOf(operation, {
			AutomationOperation("build_profile.list"), AutomationOperation("build_profile.get"),
			AutomationOperation("build_profile.create"), AutomationOperation("build_profile.update"),
			AutomationOperation("build_profile.duplicate"), AutomationOperation("build_profile.prepare_delete")}))
		{
			const std::filesystem::path root = editorContext->projectManager->GetProjectRoot();
			const std::filesystem::path directory = root / "BuildProfiles";
			const auto serializeProfile = [](const BuildProfile& profile) {
				return nlohmann::json{{"profileId", profile.id}, {"name", profile.name}, {"startupScene", profile.startupScene}, {"outputDirectory", profile.outputDirectory.generic_string()}, {"platform", ExportPlatformToString(profile.platform)}, {"enableManagedCompilation", profile.enableManagedCompilation}};
			};
			if (operation == AutomationOperation("build_profile.list"))
			{
				nlohmann::json profiles = nlohmann::json::array();
				for (const BuildProfile& profile : LoadAllBuildProfiles(directory)) profiles.push_back(serializeProfile(profile));
				return BuildSuccess(requestId, "Listed build profiles.", {{"profiles", profiles}});
			}
			const std::string profileId = arguments.value("profileId", std::string{});
			const BuildProfileCollectionResult found = FindBuildProfileInCollection(directory, profileId);
			std::optional<BuildProfile> existing = found.profile;
			if (operation == AutomationOperation("build_profile.get")) return existing ? BuildSuccess(requestId, "Read build profile.", {{"profile", serializeProfile(*existing)}}) : BuildError(requestId, ErrorCategory::MissingTarget, "Build profile was not found.");
			if (operation == AutomationOperation("build_profile.prepare_delete")) return existing ? BuildSuccess(requestId, "Prepared build-profile deletion.", {{"profileId", existing->id}, {"impact", {{"startupScene", existing->startupScene}}}, {"approvalId", "approval-build-profile-" + existing->id}}) : BuildError(requestId, ErrorCategory::MissingTarget, "Build profile was not found.");
			BuildProfile profile;
			if (operation == AutomationOperation("build_profile.create"))
			{
				const BuildProfileCollectionResult created = CreateBuildProfileDraft(arguments.value("name", std::string{}));
				if (!created.succeeded || !created.profile.has_value()) return BuildError(requestId, ErrorCategory::Validation, created.error);
				profile = *created.profile;
				profile.outputDirectory = root / "Builds" / profile.name;
			}
			else if (!existing) return BuildError(requestId, ErrorCategory::MissingTarget, "Build profile was not found.");
			else
			{
				profile = *existing;
				if (operation == AutomationOperation("build_profile.duplicate"))
				{
					const BuildProfileCollectionResult duplicated = DuplicateBuildProfileDraft(profile, arguments.value("name", profile.name + " Copy"));
					if (!duplicated.succeeded || !duplicated.profile.has_value()) return BuildError(requestId, ErrorCategory::Validation, duplicated.error);
					profile = *duplicated.profile;
				}
				else
				{
					const nlohmann::json changes = arguments.value("changes", nlohmann::json::object());
					if (changes.contains("name")) profile.name = changes["name"].get<std::string>();
					if (changes.contains("startupScene"))
					{
						// Native build profiles store scene paths relative to Assets (for
						// example, Scenes/Main.tescene). Accept the scene-tool spelling too,
						// but normalize it before validation and packaging so the manifest
						// cannot name a path that does not exist inside the TEPAK archive.
						std::string startupScene = changes["startupScene"].get<std::string>();
						std::replace(startupScene.begin(), startupScene.end(), '\\', '/');
						if (LowerAscii(startupScene).rfind("assets/", 0) == 0)
							startupScene.erase(0, std::string("Assets/").size());
						const std::filesystem::path scenePath(startupScene);
						if (scenePath.empty() || scenePath.is_absolute() || scenePath.extension() != ".tescene"
							|| startupScene.find("..") != std::string::npos
							|| !std::filesystem::exists(root / "Assets" / scenePath))
						{
							return BuildError(requestId, ErrorCategory::Validation,
								"Build profile startup scene must resolve to an existing .tescene file under Assets.", startupScene);
						}
						profile.startupScene = scenePath.lexically_normal().generic_string();
					}
					if (changes.contains("enableManagedCompilation")) profile.enableManagedCompilation = changes["enableManagedCompilation"].get<bool>();
				}
			}
			const BuildProfileCollectionResult saved = SaveBuildProfileToCollection(directory, std::move(profile));
			if (!saved.succeeded || !saved.profile.has_value()) return BuildError(requestId, ErrorCategory::Validation, saved.error);
			profile = *saved.profile;
			return BuildSuccess(requestId, "Persisted build profile.", {{"profileId", profile.id}, {"profile", serializeProfile(profile)}, {"validation", {{"valid", true}, {"errors", nlohmann::json::array()}}}});
		}
		if (IsOperationOneOf(operation, {
			AutomationOperation("settings.project.get"), AutomationOperation("settings.project.set"),
			AutomationOperation("settings.editor.get"), AutomationOperation("settings.editor.set")}))
		{
			const bool projectScope = IsOperationOneOf(operation, {
				AutomationOperation("settings.project.get"), AutomationOperation("settings.project.set")});
			const bool write = IsOperationOneOf(operation, {
				AutomationOperation("settings.project.set"), AutomationOperation("settings.editor.set")});
			const std::string key = arguments.value("key", std::string{});
			nlohmann::json allowedKeys = nlohmann::json::array();
			if (projectScope)
				for (const std::string_view allowed : TerraEngine::Editor::Settings::kProjectAutomationSettingKeys) allowedKeys.push_back(allowed);
			else
				for (const std::string_view allowed : TerraEngine::Editor::Settings::kEditorAutomationSettingKeys) allowedKeys.push_back(allowed);
			if (std::find(allowedKeys.begin(), allowedKeys.end(), key) == allowedKeys.end()) return BuildError(requestId, ErrorCategory::Validation, "Setting key is not allowlisted.", key);
			ProjectManager& projects = *editorContext->projectManager;
			if (write)
			{
				if (!arguments.contains("value")) return BuildError(requestId, ErrorCategory::Validation, "Setting value is required.");
				std::string settingError;
				if (!projects.WriteAutomationSetting(projectScope, key, arguments["value"], settingError)) return BuildError(requestId, ErrorCategory::Validation, settingError);
			}
			nlohmann::json value;
			std::string settingError;
			if (!projects.ReadAutomationSetting(projectScope, key, value, settingError)) return BuildError(requestId, ErrorCategory::Validation, settingError);
			return BuildSuccess(requestId, write ? "Persisted allowlisted setting." : "Read allowlisted setting.", {{"key", key}, {"value", value}, {"allowedKeys", allowedKeys}, {"persisted", write}});
		}
		if (IsOperationOneOf(operation, {
			AutomationOperation("scene_view.state"), AutomationOperation("scene_view.camera.set"),
			AutomationOperation("scene_view.frame_object"), AutomationOperation("scene_view.tool.set"),
			AutomationOperation("scene_view.orientation.set"), AutomationOperation("scene_view.snapping.set"),
			AutomationOperation("scene_view.visualization.set")}))
		{
			const std::shared_ptr<SceneViewWindow> sceneView = mainEditor != nullptr ? mainEditor->GetWindow<SceneViewWindow>() : nullptr;
			if (sceneView == nullptr)
				return BuildError(requestId, ErrorCategory::Unavailable, "Scene View semantic control is unavailable without a registered Scene View service.");
			if (operation == AutomationOperation("scene_view.state"))
				return BuildSuccess(requestId, "Read Scene View state.", {{"state", sceneView->GetAutomationState()}});

			std::string error;
			bool changed = false;
			if (operation == AutomationOperation("scene_view.camera.set"))
			{
				const auto position = arguments.contains("position") ? ReadVec3(arguments["position"]) : std::nullopt;
				const auto focus = arguments.contains("focus") ? ReadVec3(arguments["focus"]) : std::nullopt;
				if (!position.has_value() || !focus.has_value())
					return BuildError(requestId, ErrorCategory::Validation, "Camera position and focus must each be a three-number array.");
				changed = sceneView->SetAutomationCamera(*position, *focus, error);
			}
			else if (operation == AutomationOperation("scene_view.frame_object"))
			{
				const EditorTarget target = ResolveTarget(arguments.value("target", nlohmann::json{}));
				if (!target.resolved || !target.entity.has_value())
					return BuildError(requestId, ErrorCategory::StaleTarget, "Scene View frame target could not be resolved.");
				changed = sceneView->FrameAutomationObject(*target.entity, error);
			}
			else if (operation == AutomationOperation("scene_view.tool.set"))
			{
				if (!arguments.contains("tool") || !arguments["tool"].is_string())
					return BuildError(requestId, ErrorCategory::Validation, "Scene View tool must be a string.");
				changed = sceneView->SetAutomationTool(arguments["tool"].get<std::string>(), error);
			}
			else if (operation == AutomationOperation("scene_view.orientation.set"))
			{
				if (!arguments.contains("orientation") || !arguments["orientation"].is_string())
					return BuildError(requestId, ErrorCategory::Validation, "Scene View orientation must be a string.");
				changed = sceneView->SetAutomationOrientation(arguments["orientation"].get<std::string>(), error);
			}
			else if (operation == AutomationOperation("scene_view.snapping.set"))
			{
				if (!arguments.contains("enabled") || !arguments["enabled"].is_boolean())
					return BuildError(requestId, ErrorCategory::Validation, "Scene View snapping requires a Boolean enabled value.");
				std::optional<glm::vec3> values;
				if (arguments.contains("values"))
				{
					values = ReadVec3(arguments["values"]);
					if (!values.has_value()) return BuildError(requestId, ErrorCategory::Validation, "Snap values must contain translation, rotation-degree, and scale increments.");
				}
				changed = sceneView->SetAutomationSnapping(arguments["enabled"].get<bool>(), values, error);
			}
			else if (operation == AutomationOperation("scene_view.visualization.set"))
			{
				if (!arguments.contains("mode") || !arguments["mode"].is_string())
					return BuildError(requestId, ErrorCategory::Validation, "Scene View visualization mode must be a string.");
				changed = sceneView->SetAutomationVisualization(arguments["mode"].get<std::string>(), error);
			}
			if (!changed)
				return BuildError(requestId, ErrorCategory::Validation, error.empty() ? "Unsupported Scene View operation." : error);
			return BuildSuccess(requestId, "Updated Scene View state.", {{"state", sceneView->GetAutomationState()}});
		}
		if (IsOperationOneOf(operation, {
			AutomationOperation("profiler.capture.start"), AutomationOperation("profiler.capture.stop"),
			AutomationOperation("profiler.capture.list"), AutomationOperation("profiler.capture.load"),
			AutomationOperation("profiler.capture.summary")}))
		{
			if (editorContext == nullptr || !editorContext->editorProfiler) return BuildError(requestId, ErrorCategory::Unavailable, "Editor profiler service is unavailable.");
			if (operation == AutomationOperation("profiler.capture.list"))
			{
				nlohmann::json artifacts = nlohmann::json::array();
				for (const auto& item : editorContext->editorProfiler->GetAutomationCaptureList())
				{
					artifacts.push_back({{"artifactId", item.artifactId}, {"name", item.artifact.fileName}, {"frameCount", item.artifact.frameCount}, {"status", item.artifact.status}});
				}
				return BuildSuccess(requestId, "Listed profiler captures.", {{"artifacts", artifacts}});
			}
			if (operation == AutomationOperation("profiler.capture.load"))
			{
				std::string error;
				const std::string artifactId = arguments.value("artifactId", std::string{});
				if (!editorContext->editorProfiler->RequestLoadCaptureById(artifactId, error))
					return BuildError(requestId, ErrorCategory::MissingTarget, error, artifactId);
				const AutomationJobSnapshot job = jobService.Create({"profiler-load", std::string(operationName), requestId, arguments.value("correlationId", requestId), {}, false});
				if (job.jobId.empty()) return BuildError(requestId, ErrorCategory::QueueFull, "Profiler job queue is full.");
				activeProfilerLoadJobId = job.jobId;
				activeProfilerLoadArtifactId = artifactId;
				return BuildSuccess(requestId, "Accepted profiler capture load.", {{"jobId", job.jobId}, {"state", "queued"}, {"artifactId", artifactId}});
			}
			if (operation == AutomationOperation("profiler.capture.summary"))
			{
				EditorProfilerCaptureSummary summary;
				std::string error;
				const std::string artifactId = arguments.value("artifactId", std::string{});
				if (!editorContext->editorProfiler->SummarizeCaptureById(artifactId, summary, error))
					return BuildError(requestId, ErrorCategory::MissingTarget, error, artifactId);
				nlohmann::json streams = nlohmann::json::array();
				for (const auto& stream : summary.timingStreams)
				{
					streams.push_back({{"id", stream.id}, {"label", stream.label}, {"samples", stream.sampleCount}, {"missing", stream.missingCount}, {"meanMs", stream.meanMs}, {"p95Ms", stream.p95Ms}, {"maxMs", stream.maxMs}});
				}
				return BuildSuccess(requestId, "Summarized profiler capture.", {{"summary", {{"artifactId", summary.artifactId}, {"frameCount", summary.frameCount}, {"completeFrameCount", summary.completeFrameCount}, {"incompleteFrameCount", summary.incompleteFrameCount}, {"totalDurationMs", summary.totalDurationMs}, {"minimumFrameMs", summary.minimumFrameMs}, {"maximumFrameMs", summary.maximumFrameMs}, {"averageFrameMs", summary.averageFrameMs}, {"timingStreams", streams}}}});
			}
			if (operation == AutomationOperation("profiler.capture.start"))
			{
				editorContext->editorProfiler->StartRecording();
				if (editorContext->editorProfiler->GetState() != TerraEngine::Profiling::ProfilerSessionState::Recording)
					return BuildError(requestId, ErrorCategory::Unavailable, editorContext->editorProfiler->GetLastError());
				const AutomationJobSnapshot job = jobService.Create({"profiler", std::string(operationName), requestId, arguments.value("correlationId", requestId), {}, false});
				if (job.jobId.empty()) return BuildError(requestId, ErrorCategory::QueueFull, "Profiler job queue is full.");
				(void)jobService.CompleteSucceeded(job.jobId, {{"state", "recording"}});
				return BuildSuccess(requestId, "Started profiler capture.", {{"jobId", job.jobId}, {"state", "succeeded"}});
			}
			if (operation == AutomationOperation("profiler.capture.stop"))
			{
				profilerArtifactIdsBeforeStop.clear();
				for (const EditorProfilerCaptureArtifact& artifact : editorContext->editorProfiler->GetAutomationCaptureList())
					profilerArtifactIdsBeforeStop.insert(artifact.artifactId);
				const AutomationJobSnapshot job = jobService.Create({"profiler", std::string(operationName), requestId, arguments.value("correlationId", requestId), {}, false});
				if (job.jobId.empty()) return BuildError(requestId, ErrorCategory::QueueFull, "Profiler job queue is full.");
				activeProfilerCaptureJobId = job.jobId;
				editorContext->editorProfiler->FinishCapture();
				return BuildSuccess(requestId, "Accepted profiler capture persistence.", {{"jobId", job.jobId}, {"state", "queued"}, {"pollTool", "jobs.get"}});
			}
			return BuildError(requestId, ErrorCategory::UnsupportedCapability, "Profiler operation is not registered.");
		}
		if (operation == AutomationOperation("editor.commands.list"))
			return BuildSuccess(requestId, "Listed semantic editor commands.", {{"commands", nlohmann::json::array({
				{{"commandId", "scene.save"}, {"displayName", "Save Scene"}, {"requiresProject", true}, {"requiresScene", true}},
				{{"commandId", "scene_view.frame_selected"}, {"displayName", "Frame Selected Object"}, {"requiresProject", true}, {"requiresScene", true}},
				{{"commandId", "scene_view.tool.move"}, {"displayName", "Select Move Tool"}, {"requiresProject", true}, {"requiresScene", true}},
				{{"commandId", "scene_view.tool.rotate"}, {"displayName", "Select Rotate Tool"}, {"requiresProject", true}, {"requiresScene", true}},
				{{"commandId", "scene_view.tool.scale"}, {"displayName", "Select Scale Tool"}, {"requiresProject", true}, {"requiresScene", true}}
			})}});
		if (operation == AutomationOperation("editor.commands.invoke"))
		{
			const std::string commandId = arguments.value("commandId", std::string{});
			if (commandId == "scene.save")
			{
				const nlohmann::json result = HandleSceneSave(nlohmann::json::object(), requestId);
				if (!result.value("ok", false)) return result;
			}
			else if (commandId == "scene_view.frame_selected")
			{
				if (!getSelectedEntity || !getSelectedEntity().has_value()) return BuildError(requestId, ErrorCategory::Unavailable, "No selected object is available to frame.");
				const std::shared_ptr<SceneViewWindow> sceneView = mainEditor != nullptr ? mainEditor->GetWindow<SceneViewWindow>() : nullptr;
				if (sceneView == nullptr) return BuildError(requestId, ErrorCategory::Unavailable, "Scene View is unavailable.");
				std::string error;
				if (!sceneView->FrameAutomationObject(*getSelectedEntity(), error)) return BuildError(requestId, ErrorCategory::StaleTarget, error);
			}
			else if (commandId == "scene_view.tool.move" || commandId == "scene_view.tool.rotate" || commandId == "scene_view.tool.scale")
			{
				const std::string tool = commandId.substr(std::string("scene_view.tool.").size());
				const std::shared_ptr<SceneViewWindow> sceneView = mainEditor != nullptr ? mainEditor->GetWindow<SceneViewWindow>() : nullptr;
				if (sceneView == nullptr) return BuildError(requestId, ErrorCategory::Unavailable, "Scene View is unavailable.");
				std::string error;
				if (!sceneView->SetAutomationTool(tool, error)) return BuildError(requestId, ErrorCategory::Validation, error);
			}
			else return BuildError(requestId, ErrorCategory::Validation, "Command ID is not allowlisted.");
			return BuildSuccess(requestId, "Invoked semantic editor command.", {{"commandId", commandId}, {"completed", true}});
		}
		const auto resolveScenePath = [this, &requestId](const nlohmann::json& source, std::string_view key) -> std::optional<std::filesystem::path> {
			if (editorContext == nullptr || editorContext->projectManager == nullptr || !editorContext->projectManager->HasActiveProject())
			{
				return std::nullopt;
			}
			const nlohmann::json validation = ValidateProjectRelativePath(source, key, requestId);
			if (!validation.value("ok", false))
			{
				return std::nullopt;
			}
			const std::filesystem::path relative(validation["path"].get<std::string>());
			if (relative.extension() != ".tescene" || relative.empty() || relative.begin()->string() != "Assets")
			{
				return std::nullopt;
			}
			return (editorContext->projectManager->GetProjectRoot() / relative).lexically_normal();
		};
		if (operation == AutomationOperation("scene.list"))
		{
			const std::filesystem::path root = editorContext->projectManager->GetProjectRoot();
			nlohmann::json scenes = nlohmann::json::array();
			const SceneDocument* active = SceneManager::Instance().GetActiveScene();
			for (const std::filesystem::path& relative : SceneManager::Instance().ListSceneFiles(root))
			{
				const std::filesystem::path absolute = (root / relative).lexically_normal();
				std::error_code comparisonError;
				const bool isActive = active != nullptr && std::filesystem::equivalent(active->sourcePath, absolute, comparisonError) && !comparisonError;
				scenes.push_back({
					{"path", relative.generic_string()},
					{"name", relative.stem().string()},
					{"active", isActive}
				});
			}
			return BuildSuccess(requestId, "Listed project scenes.", {{"scenes", scenes}, {"dirty", SceneManager::Instance().IsActiveSceneDirty()}});
		}
		if (operation == AutomationOperation("scene.create"))
		{
			const auto path = resolveScenePath(arguments, "path");
			if (!path) return BuildError(requestId, ErrorCategory::Validation, "Scene path must name a .tescene file under Assets.");
			std::string error;
			if (!SceneManager::Instance().CreateSceneFile(*path, arguments.value("name", path->stem().string()), &error))
				return BuildError(requestId, ErrorCategory::Conflict, error, path->generic_string());
			if (arguments.value("open", false))
			{
				const TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene(*path);
				if (sceneId == TerraEngine::EMPTY_UUID || !SceneManager::Instance().SetActiveScene(sceneId))
					return BuildError(requestId, ErrorCategory::Internal, "Scene was created but could not be opened.");
			}
			return BuildSuccess(requestId, "Created scene.", {{"path", std::filesystem::relative(*path, editorContext->projectManager->GetProjectRoot()).generic_string()}});
		}
		if (operation == AutomationOperation("scene.save_as"))
		{
			const auto path = resolveScenePath(arguments, "path");
			if (!path) return BuildError(requestId, ErrorCategory::Validation, "Scene path must name a .tescene file under Assets.");
			if (std::filesystem::exists(*path)) return BuildError(requestId, ErrorCategory::Conflict, "Scene destination already exists.");
			if (!SceneManager::Instance().SaveScene(*path)) return BuildError(requestId, ErrorCategory::Internal, "Scene save-as failed.");
			return BuildSuccess(requestId, "Saved scene as a new file.", {{"path", std::filesystem::relative(*path, editorContext->projectManager->GetProjectRoot()).generic_string()}});
		}
		if (operation == AutomationOperation("scene.duplicate") || operation == AutomationOperation("scene.rename"))
		{
			const auto source = resolveScenePath(arguments, "source");
			const auto destination = resolveScenePath(arguments, "destination");
			if (!source || !destination) return BuildError(requestId, ErrorCategory::Validation, "Source and destination must be .tescene files under Assets.");
			std::string error;
			const bool succeeded = operation == AutomationOperation("scene.duplicate")
				? SceneManager::Instance().DuplicateSceneFile(*source, *destination, &error)
				: SceneManager::Instance().RenameSceneFile(*source, *destination, &error);
			if (!succeeded) return BuildError(requestId, ErrorCategory::Conflict, error);
			return BuildSuccess(requestId, operation == AutomationOperation("scene.duplicate") ? "Duplicated scene." : "Renamed scene.", {
				{"source", std::filesystem::relative(*source, editorContext->projectManager->GetProjectRoot()).generic_string()},
				{"destination", std::filesystem::relative(*destination, editorContext->projectManager->GetProjectRoot()).generic_string()}
			});
		}
		if (operation == AutomationOperation("scene.prepare_delete"))
		{
			const auto path = resolveScenePath(arguments, "path");
			if (!path || !std::filesystem::exists(*path)) return BuildError(requestId, ErrorCategory::MissingTarget, "Scene does not exist.");
			const std::string relative = std::filesystem::relative(*path, editorContext->projectManager->GetProjectRoot()).generic_string();
			nlohmann::json dependents = nlohmann::json::array();
			const std::filesystem::path profiles = editorContext->projectManager->GetProjectRoot() / "BuildProfiles";
			std::error_code scanError;
			if (std::filesystem::exists(profiles, scanError))
			{
				for (std::filesystem::recursive_directory_iterator iterator(profiles, scanError), end; iterator != end && !scanError; iterator.increment(scanError))
				{
					if (!iterator->is_regular_file()) continue;
					std::ifstream input(iterator->path(), std::ios::binary);
					const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
					if (contents.find(relative) != std::string::npos)
						dependents.push_back(std::filesystem::relative(iterator->path(), editorContext->projectManager->GetProjectRoot()).generic_string());
				}
			}
			return BuildSuccess(requestId, "Prepared scene deletion impact report.", {{"path", relative}, {"requiresApproval", true}, {"dependents", dependents}});
		}
		if (operation == AutomationOperation("diagnostics.get_summary")) return HandleDiagnosticsGetSummary(arguments, requestId);
		return BuildError(requestId, ErrorCategory::UnsupportedCapability, "Capability metadata exists but no handler is registered.");
	}

	nlohmann::json EditorAutomationService::BuildCapabilityList() const
	{
		nlohmann::json items = nlohmann::json::array();
		const AutomationStateSnapshot state = BuildAutomationStateSnapshot();
		for (const AutomationCapabilitySnapshot& capability : capabilityRegistry.Snapshot(state))
		{
			const AutomationCapabilityDescriptor& descriptor = capability.descriptor;
			const AssistantCapability* legacy = FindCapability(descriptor.name);
			nlohmann::json item = {
				{"name", descriptor.name},
				{"kind", legacy != nullptr ? legacy->kind : "tool"},
				{"category", descriptor.category},
				{"mutationMode", ToString(descriptor.mutationMode)},
				{"mutatesState", descriptor.mutationMode != MutationMode::ReadOnly},
				{"requiresProject", capability.availability.requiresProject},
				{"requiresScene", capability.availability.requiresScene},
				{"requiresApproval", descriptor.approvalPolicy != AutomationApprovalPolicy::None},
				{"inputSummary", legacy != nullptr ? legacy->inputSummary : descriptor.description},
				{"version", descriptor.version},
				{"description", descriptor.description},
				{"inputSchema", descriptor.inputSchema},
				{"resultSchema", descriptor.resultSchema},
				{"permission", PermissionJson(descriptor.permission)},
				{"approvalPolicy", ApprovalPolicyJson(descriptor.approvalPolicy)},
				{"provider", capability.providerId},
				{"deprecated", descriptor.deprecated},
				{"replacement", descriptor.replacement},
				{"availability", {
					{"available", capability.availability.available},
					{"reason", capability.availability.reason},
					{"requiresRenderer", capability.availability.requiresRenderer},
					{"allowedPlayStates", capability.availability.allowedPlayStates}
				}},
				{"execution", {
					{"workerPreparation", descriptor.execution.workerPreparation},
					{"protectedCommit", descriptor.execution.protectedCommit},
					{"visibilityBoundary", descriptor.execution.visibilityBoundary},
					{"preparationReads", descriptor.execution.preparationReads},
					{"commitReads", descriptor.execution.commitReads},
					{"commitWrites", descriptor.execution.commitWrites}
				}}
			};
			items.push_back(std::move(item));
		}
		return {{"capabilities", items}, {"count", items.size()}};
	}

	nlohmann::json EditorAutomationService::BuildProjectState() const
	{
		nlohmann::json data;
		const bool hasProject = editorContext != nullptr && editorContext->projectManager != nullptr && editorContext->projectManager->HasActiveProject();
		data["hasProject"] = hasProject;
		data["projectRoot"] = hasProject ? editorContext->projectManager->GetProjectRoot().string() : "";
		if (hasProject)
		{
			std::error_code identityError;
			const std::filesystem::path identity = std::filesystem::weakly_canonical(editorContext->projectManager->GetProjectRoot(), identityError);
			data["canonicalIdentity"] = identityError ? editorContext->projectManager->GetProjectRoot().lexically_normal().generic_string() : identity.generic_string();
		}
		else data["canonicalIdentity"] = "";
		const auto* scene = SceneManager::Instance().GetActiveScene();
		data["activeScene"] = scene != nullptr ? scene->name : "";
		data["sceneDirty"] = SceneManager::Instance().IsActiveSceneDirty();
		data["playMode"] = ScriptingSystem::Instance().IsRunning();
		const AutomationStateSnapshot automationState = BuildAutomationStateSnapshot();
		data["playState"] = automationState.playState;
		data["projectGeneration"] = automationState.projectGeneration;
		data["sceneGeneration"] = automationState.sceneGeneration;
		data["availableServices"] = automationState.availableServices;
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
		const nlohmann::json profilerTimingStreams = includeDetails ? SerializeProfilerTimingStreams() : nlohmann::json::array();
		nlohmann::json virtualGeometryGateJson = nlohmann::json::array();
		std::uint32_t virtualGeometryGateFailures = 0u;
		std::uint32_t virtualGeometryGateUnsupported = 0u;
		for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& view : renderDiagnostics.virtualGeometryViews)
		{
			for (const PerformanceGateResult& gate : view.virtualGeometryPerformanceGates)
			{
				nlohmann::json gateJson = SerializeVirtualGeometryPerformanceGate(gate);
				gateJson["viewType"] = ToString(view.viewType);
				virtualGeometryGateJson.push_back(std::move(gateJson));
				if (gate.status == VirtualGeometryPerformanceGateStatus::Fail)
				{
					++virtualGeometryGateFailures;
				}
				else if (gate.status == VirtualGeometryPerformanceGateStatus::Unsupported)
				{
					++virtualGeometryGateUnsupported;
				}
			}
		}
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
		const nlohmann::json sceneViewDiagnostics = TerraEngine::Editor::SceneViewOverlay::SerializeDiagnosticsSnapshot(
			TerraEngine::Editor::SceneViewOverlay::GetLatestDiagnosticsSnapshot());
		data["surfaces"] = nlohmann::json::array({
			{{"surface", "render"}, {"state", "available"}, {"details", includeDetails ? nlohmann::json{{"note", "Render diagnostics are available through the profiler/resource windows."}, {"timingStreams", profilerTimingStreams}, {"virtualGeometryPerformanceGates", virtualGeometryGateJson}, {"virtualGeometryGateFailures", virtualGeometryGateFailures}, {"virtualGeometryUnsupportedGates", virtualGeometryGateUnsupported}} : nlohmann::json::object()}},
			{{"surface", "sceneView"}, {"state", sceneViewDiagnostics.value("rendererAvailable", true) ? "available" : "rendererUnavailable"}, {"details", includeDetails ? sceneViewDiagnostics : nlohmann::json{{"activeTool", sceneViewDiagnostics.value("activeTool", "move")}, {"projectionMode", sceneViewDiagnostics.value("projectionMode", "perspective")}, {"iconsVisible", sceneViewDiagnostics.value("iconsVisible", true)}}}},
			{{"surface", "runtimeLayout"}, {"state", "available"}, {"details", nlohmann::json{{"authoredObjectCount", runtimeMembership.size()}, {"activeRuntimeObjectCount", activeRuntimeObjects}, {"membershipRevision", ObjectManager::instance().GetRuntimeActiveMembershipRevision()}}}},
			{{"surface", "resources"}, {"state", assetJobCount > 0 ? "compiling" : "available"}, {"details", nlohmann::json{{"assetCompileJobs", assetJobCount}}}},
			{{"surface", "streaming"}, {"state", "available"}, {"details", nlohmann::json::object()}},
			{{"surface", "capture"}, {"state", "available"}, {"details", nlohmann::json{{"availability", BuildCaptureAvailability()}, {"schedulerDeclarations", includeDetails ? captureSchedulers : nlohmann::json::array()}}}},
			{{"surface", "scheduler"}, {"state", "available"}, {"details", nlohmann::json{{"workerBoundaries", boundaryFindings}, {"parallelFrame", parallelFrame}, {"mutationDiagnostics", mutationDiagnostics}}}},
			{{"surface", "scripting"}, {"state", ScriptingSystem::Instance().IsRunning() ? "running" : "idle"}, {"details", nlohmann::json{{"coordinatorState", editorContext ? editorContext->scripting.coordinatorState : "Idle"}}}},
			{{"surface", "export"}, {"state", exportJobs.empty() ? "idle" : exportJobs.front().state}, {"details", nlohmann::json{{"jobs", exportJobJson}}}}
		});
		data["parallelFrame"] = parallelFrame;
		data["sceneView"] = sceneViewDiagnostics;
		data["rendererRecovery"] = SerializeRendererRecoverySnapshot(renderDiagnostics.rendererRecovery);
		data["profilerTimingStreams"] = profilerTimingStreams;
		data["hardBoundaries"] = boundaryFindings;
		data["mutationDiagnostics"] = mutationDiagnostics;
		data["virtualGeometryPerformanceGates"] = includeDetails ? virtualGeometryGateJson : nlohmann::json::array();
		data["virtualGeometryGateFailures"] = virtualGeometryGateFailures;
		data["virtualGeometryUnsupportedGates"] = virtualGeometryGateUnsupported;
		data["emptyStateSafe"] = true;
		return data;
	}

	EditorAutomationRegistry& EditorAutomationService::GetRegistry()
	{
		return capabilityRegistry;
	}

	const EditorAutomationRegistry& EditorAutomationService::GetRegistry() const
	{
		return capabilityRegistry;
	}

	EditorAutomationJobService& EditorAutomationService::GetJobService()
	{
		return jobService;
	}

	const EditorAutomationJobService& EditorAutomationService::GetJobService() const
	{
		return jobService;
	}

	EditorAutomationLogStore& EditorAutomationService::GetLogStore()
	{
		return logStore;
	}

	const EditorAutomationLogStore& EditorAutomationService::GetLogStore() const
	{
		return logStore;
	}

	EditorAutomationIdempotencyStore& EditorAutomationService::GetIdempotencyStore()
	{
		return idempotencyStore;
	}

	const EditorAutomationIdempotencyStore& EditorAutomationService::GetIdempotencyStore() const
	{
		return idempotencyStore;
	}

	AutomationStateSnapshot EditorAutomationService::BuildAutomationStateSnapshot() const
	{
		AutomationStateSnapshot snapshot;
		snapshot.projectAvailable = editorContext != nullptr
			&& editorContext->projectManager != nullptr
			&& editorContext->projectManager->HasActiveProject();
		snapshot.sceneAvailable = SceneManager::Instance().GetActiveScene() != nullptr;
		snapshot.rendererAvailable = RenderSystem::Instance().GetContext() != nullptr;
		snapshot.playState = !ScriptingSystem::Instance().IsRunning() ? "stopped" : (ScriptingSystem::Instance().IsPaused() ? "paused" : "playing");
		if (snapshot.projectAvailable)
		{
			snapshot.projectGeneration = std::hash<std::string>{}(editorContext->projectManager->GetProjectRoot().generic_string());
		}
		if (const SceneDocument* activeScene = SceneManager::Instance().GetActiveScene())
		{
			snapshot.sceneGeneration = std::hash<std::string>{}(activeScene->sourcePath.generic_string());
		}
		if (snapshot.projectAvailable)
		{
			snapshot.availableServices.insert("project");
			snapshot.availableServices.insert("assets");
		}
		if (snapshot.sceneAvailable)
		{
			snapshot.availableServices.insert("scene");
		}
		if (snapshot.rendererAvailable)
		{
			snapshot.availableServices.insert("renderer");
		}
		if (mainEditor != nullptr && mainEditor->GetWindow<SceneViewWindow>() != nullptr)
		{
			snapshot.availableServices.insert("SceneView");
		}
		return snapshot;
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

			if (TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata->importerName))
			{
				const std::optional<nlohmann::json> importResult = LoadPackageImportResultJson(absolutePath);
				if (!importResult)
				{
					failedAssets.push_back({{"path", relative.generic_string()}, {"reason", "package import sidecar was not published"}});
					continue;
				}

				const nlohmann::json aggregate = BuildPackageVirtualGeometryInspectionAggregate(*importResult);
				const nlohmann::json cookDiagnostics = aggregate.value("cookDiagnostics", nlohmann::json::object());
				const nlohmann::json clusterSummary = aggregate.value("clusterSummary", nlohmann::json::object());
				const nlohmann::json pageSummary = aggregate.value("pageResidencySummary", nlohmann::json::object());
				nlohmann::json record = {
					{"path", relative.generic_string()},
					{"guid", TerraEngine::ToHexString(metadata->guid)},
					{"importer", metadata->importerName},
					{"compiledPath", metadata->compiledPath.generic_string()},
					{"settingsSource", aggregate.value("settings", nlohmann::json::object()).value("settingsSource", std::string("ProjectDefaults"))},
					{"requestedDetailCount", cookDiagnostics.value("requestedTargetCount", 0u)},
					{"generatedDetailCount", cookDiagnostics.value("generatedLevelCount", 0u)},
					{"skippedDetails", cookDiagnostics.value("skippedDetailCount", 0u)},
					{"clusterCount", clusterSummary.value("clusterCount", 0u)},
					{"pageCount", pageSummary.value("pageCount", 0u)},
					{"fallbackCoverageValid", clusterSummary.value("fallbackCoverageValid", false)},
					{"settings", aggregate.value("settings", nlohmann::json::object())},
					{"clusterSummary", clusterSummary},
					{"pageResidencySummary", pageSummary},
					{"virtualGeometry", aggregate.value("cookMetadata", nlohmann::json::object())}
				};
				if (includePackageOutputs)
				{
					record["generatedMeshes"] = aggregate["summary"]["generatedMeshes"];
				}

				const std::string packageStatus = aggregate.value("virtualGeometryStatus", std::string("missing"));
				if (packageStatus == "missing")
				{
					record["reason"] = "package generated no usable virtual geometry mesh children";
					failedAssets.push_back(std::move(record));
				}
				else
				{
					rebuiltAssets.push_back(std::move(record));
				}
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
			if (const std::optional<nlohmann::json> importResult = LoadPackageImportResultJson(absolutePath))
			{
				const nlohmann::json aggregate = BuildPackageVirtualGeometryInspectionAggregate(*importResult);
				// Package parents do not own a renderable TEMF anymore. Surface
				// their generated mesh children as the cooked package result so MCP
				// and tests do not report the parent container as unreadable.
				data["packageVirtualGeometry"] = aggregate["summary"];
				data["virtualGeometryStatus"] = aggregate.value("virtualGeometryStatus", std::string("unreadable"));
				data["detailLevels"] = aggregate.value("detailLevels", 0u);
				data["sourceTriangleCount"] = aggregate.value("sourceTriangleCount", 0u);
				data["meshletCounts"] = aggregate.value("meshletCounts", nlohmann::json{{"resident", 0u}});
				data["boundsStatus"] = aggregate["cookMetadata"].value("boundsStatus", std::string("unknown"));
				data["settings"] = aggregate.value("settings", nlohmann::json::object());
				data["cookDiagnostics"] = aggregate.value("cookDiagnostics", nlohmann::json::object());
				data["clusterSummary"] = aggregate.value("clusterSummary", nlohmann::json::object());
				data["pageResidencySummary"] = aggregate.value("pageResidencySummary", nlohmann::json::object());
				data["cookWarnings"] = aggregate.value("cookWarnings", nlohmann::json::array());
				data["cookMetadata"] = aggregate.value("cookMetadata", nlohmann::json::object());
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
		AutomationJobSnapshot durableJob = jobService.Create({
			"export",
			"export.start",
			requestId,
			arguments.value("correlationId", requestId),
			arguments.value("idempotencyKey", std::string{}),
			true
		});
		if (durableJob.jobId.empty())
		{
			return BuildError(requestId, ErrorCategory::QueueFull, "Automation job retention is full.");
		}
		ExportJob job;
		job.jobId = durableJob.jobId;
		job.profile = validation["data"].value("profile", profile);
		job.state = "queued";
		job.progress = 0.0f;
		exportJobs.insert(exportJobs.begin(), job);
		if (exportJobs.size() > 16)
		{
			exportJobs.pop_back();
		}
		if (activityCallback)
		{
			activityCallback(ActivityEventType::Export, requestId, "export.start", "Created export job " + job.jobId + ".");
		}

		// Unit-level service fixtures do not own an Engine or installed runtime
		// layout; they drive terminal publication explicitly. A live editor does,
		// so enqueue the shared headless/editor pipeline and publish its progress to
		// the durable session job rather than leaving an accepted job queued forever.
		if (editorContext != nullptr && editorContext->engine != nullptr)
		{
			const std::filesystem::path projectRoot = editorContext->projectManager->GetProjectRoot();
			const std::optional<BuildProfile> profileSnapshot = FindBuildProfile(projectRoot, job.profile);
			if (!profileSnapshot.has_value())
			{
				(void)jobService.CompleteFailed(job.jobId, {{"error", "Build profile disappeared before export scheduling."}});
				return BuildError(requestId, ErrorCategory::StaleTarget, "Build profile disappeared before export scheduling.", job.profile);
			}

			ExportRequest exportRequest;
			exportRequest.projectRoot = std::filesystem::absolute(projectRoot).lexically_normal();
			exportRequest.profile = *profileSnapshot;
			exportRequest.profilePath = profileSnapshot->filePath.empty()
				? std::filesystem::path{}
				: std::filesystem::absolute(profileSnapshot->filePath).lexically_normal();
			const std::filesystem::path configuredOutput = profileSnapshot->outputDirectory.empty()
				? projectRoot / "Builds" / profileSnapshot->name
				: (profileSnapshot->outputDirectory.is_absolute() ? profileSnapshot->outputDirectory : projectRoot / profileSnapshot->outputDirectory);
			exportRequest.finalOutputDirectory = std::filesystem::absolute(configuredOutput).lexically_normal();
			exportRequest.runId = GenerateAutomationExportRunId();
			exportRequest.evidenceDirectory = exportRequest.finalOutputDirectory.parent_path()
				/ "ExportEvidence" / exportRequest.runId;
			exportRequest.invocationKind = ExportInvocationKind::Editor;
			std::string layoutError;
			(void)DiscoverExportRuntimeLayout(ResolveAutomationEditorExecutable(), exportRequest.runtimeLayout, layoutError);
			if (!layoutError.empty())
			{
				(void)jobService.AddDiagnostic(job.jobId, {
					AutomationLogSeverity::Warning,
					"export.runtime_layout",
					layoutError,
					{{"stage", "runtime_layout"}}
				});
			}
			(void)jobService.SetState(job.jobId, AutomationJobState::Preparing, "preparing");

			const std::string durableJobId = job.jobId;
			auto exportTask = ThreadPool::instance().createTask([this, exportRequest = std::move(exportRequest), durableJobId]() mutable {
				if (jobService.IsCancellationRequested(durableJobId))
				{
					(void)jobService.CompleteCanceled(durableJobId, {{"cancellationBoundary", "before_pipeline"}});
					return;
				}
				(void)jobService.SetState(durableJobId, AutomationJobState::Running, "validation");
				ExportPipeline pipeline;
				const ExportResult result = pipeline.Execute(exportRequest, [this, durableJobId](const ExportProgress& progress) {
					// The job service serializes worker publication internally. Progress
					// remains monotonic and reconnect-safe while the editor UI stays live.
					(void)jobService.PublishProgress(durableJobId, progress.overallProgress, std::string(ToString(progress.stage)));
				});

				for (const ExportDiagnostic& diagnostic : result.diagnostics)
				{
					(void)jobService.AddDiagnostic(durableJobId, {
						ExportAutomationSeverity(diagnostic.severity),
						diagnostic.category,
						diagnostic.message,
						{{"stage", diagnostic.stage}, {"code", diagnostic.code}, {"file", diagnostic.filePath}, {"line", diagnostic.line}, {"suggestion", diagnostic.suggestion}}
					});
				}
				if (!result.outputDirectory.empty())
				{
					(void)jobService.AddArtifact(durableJobId, {
						"export-output-" + result.runId,
						"export_output",
						result.outputDirectory.generic_string(),
						"application/vnd.terraengine.export",
						{{"profileId", result.profileId}, {"runId", result.runId}}
					});
				}
				if (!result.evidenceDirectory.empty())
				{
					(void)jobService.AddArtifact(durableJobId, {
						"export-evidence-" + result.runId,
						"export_evidence",
						result.evidenceDirectory.generic_string(),
						"application/json",
						{{"runId", result.runId}}
					});
				}
				const nlohmann::json terminalResult{
					{"success", result.success},
					{"runId", result.runId},
					{"stage", ToString(result.stage)},
					{"failureCategory", ToString(result.failureCategory)},
					{"exitCode", result.exitCode},
					{"message", result.message},
					{"outputDirectory", result.outputDirectory.generic_string()},
					{"evidenceDirectory", result.evidenceDirectory.generic_string()}
				};
				if (jobService.IsCancellationRequested(durableJobId))
					(void)jobService.CompleteCanceled(durableJobId, terminalResult);
				else if (result.success)
					(void)jobService.CompleteSucceeded(durableJobId, terminalResult);
				else
					(void)jobService.CompleteFailed(durableJobId, terminalResult);
			});
			ThreadPool::instance().enqueue(exportTask);
		}
		nlohmann::json accepted = BuildSuccess(requestId, "Accepted export job.", {{"jobId", job.jobId}, {"profile", job.profile}, {"progress", job.progress}, {"state", job.state}, {"pollTool", "jobs.get"}, {"cancelTool", "jobs.cancel"}});
		accepted["status"] = "accepted";
		return accepted;
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
				(void)jobService.RequestCancellation(jobId);
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
		// The public MCP contract groups authored camera values under `camera` so
		// capture transport options remain distinct. Accept legacy top-level camera
		// values as a compatibility fallback for older clients.
		const nlohmann::json& camera = arguments.contains("camera") && arguments["camera"].is_object()
			? arguments["camera"]
			: arguments;
		const std::optional<glm::vec3> position = ReadVec3(camera.value("position", nlohmann::json()));
		if (!position)
		{
			return BuildError(requestId, ErrorCategory::Validation, "camera.capture_custom requires position: [x, y, z].");
		}

		glm::vec3 up = ReadVec3(camera.value("up", nlohmann::json())).value_or(glm::vec3(0.0f, 1.0f, 0.0f));
		if (glm::length(up) < 0.0001f)
		{
			return BuildError(requestId, ErrorCategory::Validation, "Custom camera up vector must be non-zero.");
		}
		glm::vec3 forward(0.0f, 0.0f, -1.0f);
		const nlohmann::json targetValue = camera.contains("lookAt")
			? camera["lookAt"]
			: camera.value("target", nlohmann::json());
		if (const std::optional<glm::vec3> targetPoint = ReadVec3(targetValue))
		{
			forward = *targetPoint - *position;
		}
		else if (const std::optional<glm::vec3> requestedForward = ReadVec3(camera.value("forward", nlohmann::json())))
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
			projection = camera.value("projection", projection);
			fov = camera.value("verticalFovDegrees", fov);
			aspect = camera.value("aspectRatio", aspect);
			orthographicHeight = camera.value("orthographicHeight", orthographicHeight);
			nearPlane = camera.value("nearPlane", nearPlane);
			farPlane = camera.value("farPlane", farPlane);
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

	nlohmann::json EditorAutomationService::HandleRendererVirtualGeometryStats(const nlohmann::json& arguments, const std::string& requestId)
	{
		// The optional view selector lets MCP clients compare Game and Scene
		// surfaces without reading editor overlay text. A missing value uses the
		// Game View because that is the authored camera path used for runtime
		// validation and recent Hi-Z debugging.
		RenderViewType viewType = RenderViewType::Game;
		if (arguments.contains("viewType"))
		{
			const std::optional<RenderViewType> parsedViewType = ParseAutomationRenderViewType(arguments["viewType"]);
			if (!parsedViewType)
			{
				return BuildError(requestId, ErrorCategory::Validation, "viewType must be Game or Scene.");
			}
			if (*parsedViewType != RenderViewType::Game && *parsedViewType != RenderViewType::Scene)
			{
				return BuildError(requestId, ErrorCategory::Validation, "Virtual geometry stats are only exposed for Game and Scene views.");
			}
			viewType = *parsedViewType;
		}

		if (editorContext == nullptr || editorContext->engine == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Renderer is not available.");
		}

		RenderSystem* renderSystem = editorContext->engine->GetSystem<RenderSystem>();
		if (renderSystem == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "RenderSystem is not registered.");
		}

		const RenderStatsSnapshot stats = renderSystem->GetLatestRenderStats(viewType);
		if (!stats.valid)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "No render stats have been produced for the requested view.", ToString(viewType));
		}

		// Return both the raw meshlet counters and their provenance. The same
		// numbers may come from prepared CPU counts during warmup or true GPU
		// readback after the task shader has completed, so automation must not
		// guess which path it is observing.
		nlohmann::json performanceGates = nlohmann::json::array();
		nlohmann::json pageResidency = nlohmann::json::object();
		const TerraEngine::Renderer::RenderDiagnosticsSummary diagnostics = renderSystem->GetLatestRenderGraphDiagnostics();
		for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& view : diagnostics.virtualGeometryViews)
		{
			if (view.viewType != viewType)
			{
				continue;
			}
			for (const PerformanceGateResult& gate : view.virtualGeometryPerformanceGates)
			{
				performanceGates.push_back(SerializeVirtualGeometryPerformanceGate(gate));
			}
			pageResidency = {
				{"residentHighDetailBytes", view.pageResidency.residentHighDetailBytes},
				{"residentFallbackBytes", view.pageResidency.residentFallbackBytes},
				{"embeddedFallbackBytes", view.pageResidency.embeddedFallbackBytes},
				{"owningMeshFallbackBytes", view.pageResidency.owningMeshFallbackBytes},
				{"metadataBytes", view.pageResidency.metadataBytes},
				{"requestedBytes", view.pageResidency.requestedBytes},
				{"pinnedFallbackBytes", view.pageResidency.pinnedFallbackBytes},
				{"pendingIoBytes", view.pageResidency.pendingIoBytes},
				{"pendingUploadBytes", view.pageResidency.pendingUploadBytes},
				{"failedBytes", view.pageResidency.failedBytes},
				{"evictingBytes", view.pageResidency.evictingBytes},
				{"evictedBytes", view.pageResidency.evictedBytes},
				{"retiringBytes", view.pageResidency.retiringBytes}
			};
			break;
		}
		for (const PerformanceGateResult& gate : TerraEngine::Renderer::BuildVisibleCommandReusePerformanceGates(stats))
		{
			performanceGates.push_back(SerializeVirtualGeometryPerformanceGate(gate));
		}
		nlohmann::json data = {
			{"viewType", ToString(stats.viewType)},
			{"valid", stats.valid},
			{"frameTimeMs", stats.frameTimeMs},
			{"fps", stats.fps},
			{"drawCalls", stats.drawCalls},
			{"triangles", stats.triangleCount},
			{"meshletsSelected", stats.meshletsTotal},
			{"meshletsDispatched", stats.meshletsDispatched},
			{"meshletsCulled", stats.meshletsCulled},
			{"meshletsCulledHiZ", stats.meshletsCulledHiZ},
			{"clusterPackets", stats.virtualGeometryClusterPackets},
			{"submittedBatches", stats.virtualGeometrySubmittedBatches},
			{"singlePacketFallbackBatches", stats.virtualGeometrySinglePacketFallbackBatches},
			{"candidatePackets", stats.virtualGeometryCandidatePackets},
			{"compactedPackets", stats.virtualGeometryCompactedPackets},
			{"rejectedPackets", stats.virtualGeometryRejectedPackets},
			{"failOpenPackets", stats.virtualGeometryFailOpenPackets},
			{"gpuPacketRecords", stats.virtualGeometryGpuPacketRecords},
			{"gpuBatchDescriptors", stats.virtualGeometryGpuBatchDescriptors},
			{"gpuPacketFrameSets", stats.virtualGeometryGpuPacketFrameSets},
			{"gpuValidFrameSets", stats.virtualGeometryGpuValidFrameSets},
			{"gpuInvalidFrameSets", stats.virtualGeometryGpuInvalidFrameSets},
			{"gpuCompactionDispatches", stats.virtualGeometryGpuCompactionDispatches},
			{"generatedSubmissions", stats.virtualGeometryGeneratedSubmissions},
			{"validGeneratedSubmissions", stats.virtualGeometryValidGeneratedSubmissions},
			{"invalidGeneratedSubmissions", stats.virtualGeometryInvalidGeneratedSubmissions},
			{"fallbackGeneratedSubmissions", stats.virtualGeometryFallbackGeneratedSubmissions},
			{"gpuCounterReadbacks", stats.virtualGeometryGpuCounterReadbacks},
			{"gpuCounterLatencyFrames", stats.virtualGeometryGpuCounterLatencyFrames},
			{"gpuFallbackRecords", stats.virtualGeometryGpuFallbackRecords},
			{"gpuFallbackAffectedPackets", stats.virtualGeometryGpuFallbackAffectedPackets},
			{"gpuFallbackAffectedBatches", stats.virtualGeometryGpuFallbackAffectedBatches},
			{"gpuFallbackReason", stats.virtualGeometryGpuFallbackReason},
			{"performanceGateCount", stats.virtualGeometryPerformanceGateCount},
			{"performanceGateFailures", stats.virtualGeometryPerformanceGateFailures},
			{"performanceGateUnsupported", stats.virtualGeometryPerformanceGateUnsupported},
			{"firstFailedPerformanceGate", stats.virtualGeometryFirstFailedPerformanceGate},
			{"visibleCommandCacheHit", stats.visibleCommandCacheHit},
			{"visibleCommandConservativeRefresh", stats.visibleCommandConservativeRefresh},
			{"detailedRenderDiagnosticsRequested", stats.detailedRenderDiagnosticsRequested},
			{"visibleCommandCount", stats.visibleCommandCount},
			{"visibleCommandCopiedCount", stats.visibleCommandCopiedCount},
			{"visibleStreamingUniqueMeshes", stats.visibleStreamingUniqueMeshes},
			{"visibleStreamingUniqueMaterials", stats.visibleStreamingUniqueMaterials},
			{"visibleStreamingUniqueTextures", stats.visibleStreamingUniqueTextures},
			{"visibleStreamingDuplicateTouchesEliminated", stats.visibleStreamingDuplicateTouchesEliminated},
			{"visibleCommandReuseFallbackReason", stats.visibleCommandReuseFallbackReason},
			{"residentPages", stats.virtualGeometryResidentPages},
			{"requestedPages", stats.virtualGeometryRequestedPages},
			{"missingPages", stats.virtualGeometryMissingPages},
			{"failedPages", stats.virtualGeometryFailedPages},
			{"evictedPages", stats.virtualGeometryEvictedPages},
			{"pinnedFallbackPages", stats.virtualGeometryPinnedFallbackPages},
			{"residentPagePayloads", stats.virtualGeometryResidentPagePayloads},
			{"highDetailPagePayloads", stats.virtualGeometryHighDetailPagePayloads},
			{"fallbackPagePayloads", stats.virtualGeometryFallbackPagePayloads},
			{"embeddedFallbackPagePayloads", stats.virtualGeometryEmbeddedFallbackPagePayloads},
			{"emittedHighDetailPages", stats.virtualGeometryEmittedHighDetailPages},
			{"emittedFallbackPages", stats.virtualGeometryEmittedFallbackPages},
			{"emittedEmbeddedFallbackPages", stats.virtualGeometryEmittedEmbeddedFallbackPages},
			{"missingPageSources", stats.virtualGeometryMissingPageSources},
			{"pageSourceRequests", stats.virtualGeometryPageSourceRequests},
			{"pageSourceRetirements", stats.virtualGeometryPageSourceRetirements},
			{"residentPayloadBytes", stats.virtualGeometryResidentPagePayloadBytes},
			{"fallbackPayloadBytes", stats.virtualGeometryFallbackPagePayloadBytes},
			{"embeddedFallbackBytes", stats.virtualGeometryEmbeddedFallbackBytes},
			{"pageResidency", std::move(pageResidency)},
			{"counterSource", ToString(stats.meshletCounterSource)},
			{"counterFrameId", stats.meshletCounterFrameId},
			{"counterLatencyFrames", stats.meshletCounterLatencyFrames},
			{"counterReason", stats.meshletCounterReason},
			{"taskCullingActive", stats.taskCullingActive},
			{"hiZActive", stats.hiZActive},
			{"hiZState", ToString(stats.hiZState)},
			{"hiZStatusReason", stats.hiZStatusReason},
			{"hiZWarmupFrames", stats.hiZWarmupFrames},
			{"hiZBaselineMatched", stats.hiZBaselineMatched},
			{"hiZFalseOcclusionRiskWarnings", stats.hiZFalseOcclusionRiskWarnings},
			{"hiZDebugCounters", {
				{"tests", stats.hiZTests},
				{"projectionFailed", stats.hiZProjectionFailed},
				{"fullRectCulled", stats.hiZFullRectCulled},
				{"rectCoverageCulled", stats.hiZRectCoverageCulled},
				{"centerCulled", stats.hiZCenterCulled},
				{"vertexCoverageCulled", stats.hiZVertexCoverageCulled},
				{"noOcclusionProof", stats.hiZNoOcclusionProof}
			}},
			{"performanceGates", std::move(performanceGates)},
			{"vrsActive", stats.vrsActive}
		};
		return BuildSuccess(requestId, "Read virtual geometry culling stats.", std::move(data));
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
