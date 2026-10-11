// EditorAutomationService.cpp - Contract implementation for editor automation.
//
// This file keeps assistant-facing tool behavior in one place. Transport code
// supplies JSON, bridge code supplies policy, and this service performs the
// editor validation/action work against TerraEngine's existing systems.

#include "EditorAutomationService.h"
#include "EditorAutomationTransaction.h"
#include "AnimationAdvancedOperations.h"
#include "AnimationAssetOperations.h"
#include "AnimationAutomationJobs.h"
#include "AnimationAutomationSchemas.h"
#include "AnimationControllerOperations.h"
#include "AnimationRuntimeOperations.h"

#include "Providers/AssetPrefabAutomationProvider.h"
#include "Providers/AnimationAutomationProvider.h"
#include "Providers/AuthoringAutomationProvider.h"
#include "Providers/TerrainAutomationProvider.h"
#include "../Terrain/TerrainAutomationAdapter.h"
#include "../Windows/TerrainWindow.h"
#include "TerrainAutomationSchema.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Providers/EditorDiagnosticsAutomationProvider.h"
#include "Providers/HistoryPlayModeAutomationProvider.h"
#include "Providers/ProjectSceneAutomationProvider.h"
#include "Providers/PhysicsAutomationProvider.h"
#include "Providers/AudioAutomationProvider.h"
#include "Providers/NavigationAutomationProvider.h"
#include "Providers/LocomotionAutomationProvider.h"
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
#include "../Windows/ProfilerWindow.h"
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
#include "Engine/Components/MeshRenderComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Navigation/Agents/NavigationAgentSystem.h"
#include "Engine/Navigation/NavigationSystem.h"
#include "Engine/Components/Physics/CharacterControllerComponent.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/SceneMutationQueue.h"
#include "Engine/Scene/RuntimeLayout.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Core/Scheduling/FrameWorkload.h"
#include "Engine/Animation/AnimationPresentationSystem.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Animation/Assets/AnimationClipAsset.h"
#include "Engine/Animation/Assets/AnimationControllerAsset.h"
#include "Engine/Animation/Diagnostics/AnimationDiagnostics.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Renderer/RendererRecovery.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Resource/ShaderCollection.h"
#include "../Core/EditorSystem.h"
#include "Engine/Resource/VirtualGeometryStreamingSystem.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Physics/Assets/ColliderRecipe.h"
#include "Engine/Physics/Assets/CookedColliderAsset.h"
#include "Engine/Physics/Assets/PhysicsMaterial.h"
#include "Engine/Navigation/NavigationSystem.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Utilities/UUID.h"
#include "ThreadPool/ThreadPool.h"
#include "Editor/Export/EditorExportOperationController.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <string_view>
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

		bool ReadBoundedAutomationFile(
			const std::filesystem::path& path,
			std::size_t maximumBytes,
			std::vector<std::uint8_t>& output,
			std::string& error)
		{
			output.clear();
			std::ifstream stream(path, std::ios::binary | std::ios::ate);
			if (!stream)
			{
				error = "The compiled asset could not be opened.";
				return false;
			}
			const std::streamoff size = stream.tellg();
			if (size < 0 || static_cast<std::uint64_t>(size) > maximumBytes)
			{
				error = "The compiled asset is outside the bounded automation read size.";
				return false;
			}
			try
			{
				output.resize(static_cast<std::size_t>(size));
			}
			catch (const std::bad_alloc&)
			{
				error = "The bounded automation read could not allocate its detached buffer.";
				return false;
			}
			stream.seekg(0, std::ios::beg);
			if (!output.empty()
				&& !stream.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size())))
			{
				output.clear();
				error = "The compiled asset ended before the bounded read completed.";
				return false;
			}
			return true;
		}

		std::string_view AnimationChildKindName(GeneratedModelPackageChildKind kind)
		{
			switch (kind)
			{
			case GeneratedModelPackageChildKind::Mesh: return "mesh";
			case GeneratedModelPackageChildKind::Material: return "material";
			case GeneratedModelPackageChildKind::Texture: return "texture";
			case GeneratedModelPackageChildKind::Skeleton: return "skeleton";
			case GeneratedModelPackageChildKind::AnimationClip: return "animation_clip";
			case GeneratedModelPackageChildKind::MorphTarget: return "morph_target";
			case GeneratedModelPackageChildKind::DeformableMesh: return "deformable_mesh";
			case GeneratedModelPackageChildKind::Prefab: return "prefab";
			}
			return "unknown";
		}

		std::string ScriptCoordinatorStateName(ScriptCoordinatorState state)
		{
			switch (state)
			{
			case ScriptCoordinatorState::Idle:
				return "idle";
			case ScriptCoordinatorState::Queued:
				return "queued";
			case ScriptCoordinatorState::Building:
				return "building";
			case ScriptCoordinatorState::Reloading:
				return "reloading";
			case ScriptCoordinatorState::Ready:
				return "ready";
			case ScriptCoordinatorState::Failed:
				return "failed";
			case ScriptCoordinatorState::Canceled:
				return "canceled";
			}
			return "unknown";
		}

		std::string ManagedReadinessValidationName(ManagedReadinessValidation validation)
		{
			switch (validation)
			{
			case ManagedReadinessValidation::Valid:
				return "valid";
			case ManagedReadinessValidation::Missing:
				return "missing";
			case ManagedReadinessValidation::Corrupt:
				return "corrupt";
			case ManagedReadinessValidation::Stale:
				return "stale";
			case ManagedReadinessValidation::OutputMissing:
				return "output_missing";
			case ManagedReadinessValidation::OutputHashMismatch:
				return "output_hash_mismatch";
			}
			return "unknown";
		}

		nlohmann::json BuildManagedReadinessEvidence(const ScriptCompilationStatus& status, bool forcedBuild)
		{
			// This is a read-only summary of the coordinator's published state. It
			// deliberately names a cache miss for explicit compilation instead of
			// exposing implementation paths or mutable manifest contents.
			return {
				{"state", ScriptCoordinatorStateName(status.state)},
				{"gameplayAssemblyLoaded", status.gameplayAssemblyLoaded},
				{"queuedBuild", status.queuedBuild},
				{"forcedBuild", forcedBuild || status.forcedBuild},
				{"cache", {
					{"hit", status.reusedReadyOutput},
					{"reason", forcedBuild || status.forcedBuild
						? "explicit_compile_requested"
						: ManagedReadinessValidationName(status.readinessValidation)}
				}},
				{"validation", ManagedReadinessValidationName(status.readinessValidation)},
				{"assemblyPublished", !status.activeAssemblyPath.empty()}
			};
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

		nlohmann::json SerializeSceneViewPerformancePreference(
			const TerraEngine::Editor::SceneViewOverlay::SceneOverlayPreference& preference)
		{
			using namespace TerraEngine::Editor::SceneViewOverlay;
			return {
				{"largeSceneMode", ToString(preference.largeSceneMode)},
				{"largeSceneObjectThreshold", preference.largeSceneObjectThreshold},
				{"visibleMarkerBudget", preference.visibleMarkerBudget},
				{"markerPresentationBatchSize", preference.markerPresentationBatchSize},
				{"densityRadiusPixels", preference.densityRadiusPixels},
				{"densityThreshold", preference.densityThreshold},
				{"maximumMarkerDistance", preference.maximumMarkerDistance},
				{"minimumProjectedSizePixels", preference.minimumProjectedSizePixels},
				{"physicsMarkerMode", ToString(preference.physicsMarkerMode)},
				{"densePrimitiveTier", ToString(preference.densePrimitiveTier)}
			};
		}

		nlohmann::json SerializeSceneViewPerformanceState(
			const TerraEngine::Editor::SceneViewOverlay::SceneViewDiagnosticsSnapshot& snapshot)
		{
			const nlohmann::json serialized = TerraEngine::Editor::SceneViewOverlay::SerializeDiagnosticsSnapshot(snapshot);
			// Keep these additive field names explicit at the automation boundary so
			// older clients can ignore them while performance clients validate the
			// complete configured/effective and exemption evidence contract.
			return {
				{"largeScene", serialized.value("largeScene", nlohmann::json{
					{"configuredMode", "auto"}, {"effective", false}, {"reason", "unavailable"}, {"reductions", nlohmann::json::array()}})},
				{"markers", serialized.value("markers", nlohmann::json{
					{"source", 0u}, {"candidates", 0u}, {"viewRejected", 0u}, {"densityConsolidated", 0u},
					{"budgetRejected", 0u}, {"presented", 0u}, {"presentationBatches", 0u},
					{"selectedExemptions", 0u}, {"cameraExemptions", 0u}, {"lightExemptions", 0u}})},
				{"stageTimings", serialized.value("stageTimings", nlohmann::json::array())}
			};
		}

		bool ParseSceneViewPerformancePreference(
			const nlohmann::json& value,
			TerraEngine::Editor::SceneViewOverlay::SceneOverlayPreference& candidate,
			std::string& error)
		{
			using namespace TerraEngine::Editor::SceneViewOverlay;
			const std::array<std::string_view, 10> required{
				"largeSceneMode", "largeSceneObjectThreshold", "visibleMarkerBudget",
				"markerPresentationBatchSize", "densityRadiusPixels", "densityThreshold",
				"maximumMarkerDistance", "minimumProjectedSizePixels", "physicsMarkerMode",
				"densePrimitiveTier"
			};
			if (!value.is_object() || value.size() != required.size())
			{
				error = "Scene View settings must be one complete document with no unknown fields.";
				return false;
			}
			for (const std::string_view field : required)
			{
				if (!value.contains(std::string(field)))
				{
					error = "Scene View settings are missing required field: " + std::string(field);
					return false;
				}
			}

			auto readUnsigned = [&](std::string_view field, std::uint32_t minimum, std::uint32_t maximum, std::uint32_t& output) {
				const nlohmann::json& input = value[std::string(field)];
				if (!input.is_number_unsigned() && !input.is_number_integer())
				{
					error = std::string(field) + " must be an integer.";
					return false;
				}
				const std::int64_t parsed = input.get<std::int64_t>();
				if (parsed < minimum || parsed > maximum)
				{
					error = std::string(field) + " is outside its supported range.";
					return false;
				}
				output = static_cast<std::uint32_t>(parsed);
				return true;
			};
			auto readFloat = [&](std::string_view field, float minimum, float maximum, float& output) {
				const nlohmann::json& input = value[std::string(field)];
				if (!input.is_number())
				{
					error = std::string(field) + " must be numeric.";
					return false;
				}
				const double parsed = input.get<double>();
				if (!std::isfinite(parsed) || parsed < minimum || parsed > maximum)
				{
					error = std::string(field) + " is outside its supported range.";
					return false;
				}
				output = static_cast<float>(parsed);
				return true;
			};
			if (!readUnsigned("largeSceneObjectThreshold", 1u, 10000000u, candidate.largeSceneObjectThreshold)
				|| !readUnsigned("visibleMarkerBudget", 64u, 100000u, candidate.visibleMarkerBudget)
				|| !readUnsigned("markerPresentationBatchSize", 32u, 8192u, candidate.markerPresentationBatchSize)
				|| !readUnsigned("densityThreshold", 2u, 256u, candidate.densityThreshold)
				|| !readFloat("densityRadiusPixels", 4.0f, 128.0f, candidate.densityRadiusPixels)
				|| !readFloat("maximumMarkerDistance", 0.0f, 1000000000.0f, candidate.maximumMarkerDistance)
				|| !readFloat("minimumProjectedSizePixels", 0.0f, 64.0f, candidate.minimumProjectedSizePixels))
			{
				return false;
			}
			if (!value["largeSceneMode"].is_string() || !value["physicsMarkerMode"].is_string() || !value["densePrimitiveTier"].is_string())
			{
				error = "Scene View mode fields must be strings.";
				return false;
			}
			const std::string largeMode = value["largeSceneMode"].get<std::string>();
			const std::string physicsMode = value["physicsMarkerMode"].get<std::string>();
			const std::string denseTier = value["densePrimitiveTier"].get<std::string>();
			if (largeMode == "auto") candidate.largeSceneMode = LargeSceneMode::Automatic;
			else if (largeMode == "on") candidate.largeSceneMode = LargeSceneMode::On;
			else if (largeMode == "off") candidate.largeSceneMode = LargeSceneMode::Off;
			else { error = "largeSceneMode must be auto, on, or off."; return false; }
			if (physicsMode == "selected_only") candidate.physicsMarkerMode = PhysicsMarkerMode::SelectedOnly;
			else if (physicsMode == "clustered") candidate.physicsMarkerMode = PhysicsMarkerMode::Clustered;
			else if (physicsMode == "all") candidate.physicsMarkerMode = PhysicsMarkerMode::All;
			else { error = "physicsMarkerMode must be selected_only, clustered, or all."; return false; }
			if (denseTier == "full") candidate.densePrimitiveTier = DensePrimitiveTier::Full;
			else if (denseTier == "simple") candidate.densePrimitiveTier = DensePrimitiveTier::Simple;
			else if (denseTier == "point") candidate.densePrimitiveTier = DensePrimitiveTier::Point;
			else { error = "densePrimitiveTier must be full, simple, or point."; return false; }
			return ValidatePreference(candidate, &error);
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
			const glm::quat rotation = glm::normalize(transform.worldPose.rotation);
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
			// Capture matrices are camera-local. The renderer retains this canonical
			// origin separately so a distant camera never becomes a global float.
			request.cameraCanonicalPosition = transform.worldPose.position;
			request.viewMatrix = glm::lookAt(glm::vec3(0.0f), forward, glm::normalize(up));
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
				{"format", {
					{"codec", metadata.codec},
					{"profile", metadata.codecProfile},
					{"hash", metadata.formatHash},
					{"encodedBytes", metadata.encodedGeometryBytes},
					{"decodedBytes", metadata.decodedGeometryBytes},
					{"bytesPerSourceTriangle", metadata.bytesPerSourceTriangle}
				}},
				{"quality", {
					{"maximumObjectError", metadata.maximumObjectError},
					{"screenErrorState", "runtime-view-dependent"}
				}},
				{"cache", {
					{"key", metadata.cacheKey},
					{"state", metadata.cacheState},
					{"reason", metadata.cacheReason}
				}},
				{"pageMetrics", {
					{"pageCount", metadata.pageCount},
					{"fragmentationState", "unavailable-expanded-profile"},
					{"dependencyFanOutState", "unavailable-expanded-profile"}
				}},
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
					// Import metadata is a cooked catalog, not live physical residency.
					// Runtime page publications are reported by render diagnostics instead.
					{"residentPageCount", 0u},
					{"pinnedFallbackPageCount", 0u},
					{"estimatedBytes", 0u},
					{"pageStreamingReady", false},
					{"physicalResidencyAuthoritative", false},
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
					{"pageStreamingReady", false},
					{"physicalResidencyAuthoritative", false},
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

		std::optional<TerraEngine::Renderer::VirtualGeometryRasterRoute>
		ParseVirtualGeometryValidationRoute(const nlohmann::json& value)
		{
			if (!value.is_string()) return std::nullopt;
			const std::string route = LowerAscii(value.get<std::string>());
			if (route == "hardware") return TerraEngine::Renderer::VirtualGeometryRasterRoute::Hardware;
			if (route == "compute") return TerraEngine::Renderer::VirtualGeometryRasterRoute::Compute;
			if (route == "indexed") return TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed;
			return std::nullopt;
		}

		const char* ToAutomationString(TerraEngine::Renderer::VirtualGeometryRasterRoute route)
		{
			switch (route)
			{
			case TerraEngine::Renderer::VirtualGeometryRasterRoute::Hardware: return "Hardware";
			case TerraEngine::Renderer::VirtualGeometryRasterRoute::Compute: return "Compute";
			case TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed: return "Indexed";
			case TerraEngine::Renderer::VirtualGeometryRasterRoute::Fallback: return "Fallback";
			default: return "Unknown";
			}
		}

		std::optional<glm::uvec2> ParseAutomationPixel(const nlohmann::json& value)
		{
			if (!value.is_array() || value.size() != 2u) return std::nullopt;
			try
			{
				return glm::uvec2(value[0].get<std::uint32_t>(), value[1].get<std::uint32_t>());
			}
			catch (const nlohmann::json::exception&)
			{
				return std::nullopt;
			}
		}

		nlohmann::json SerializeAuthoritativeVisibilityPick(
			const TerraEngine::Renderer::AuthoritativeVisibilityPickState& state)
		{
			nlohmann::json data = {
				{"status", ToString(state.status)},
				{"pixel", nlohmann::json::array({state.pixel.x, state.pixel.y})},
				{"captureTarget", state.dedicatedCaptureTarget},
				{"reason", state.reason},
				{"pick", nullptr}
			};
			if (state.pick)
			{
				data["pick"] = {
					{"objectId", TerraEngine::ToHexString(state.pick->objectId)},
					{"meshId", TerraEngine::ToHexString(state.pick->meshId)},
					{"materialId", TerraEngine::ToHexString(state.pick->materialId)},
					{"frameId", state.pick->frameId},
					{"packetId", state.pick->packetId},
					{"clusterId", state.pick->clusterId},
					{"meshletId", state.pick->meshletId},
					{"selectedLod", state.pick->selectedLod},
					{"debugIdentity", state.pick->debugIdentity},
					{"fallback", state.pick->fallback}
				};
			}
			return data;
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

		const char* ToAutomationString(
			TerraEngine::Renderer::VirtualGeometryCapabilityStage stage)
		{
			using Stage = TerraEngine::Renderer::VirtualGeometryCapabilityStage;
			switch(stage)
			{
			case Stage::Traversal: return "traversal";
			case Stage::Compaction: return "compaction";
			case Stage::Indirect: return "indirect-generation";
			case Stage::Decode: return "decode";
			case Stage::HardwareRaster: return "hardware-raster";
			case Stage::ComputeRaster: return "compute-raster";
			case Stage::Resolve: return "visibility-resolve";
			case Stage::Shading: return "shading";
			case Stage::Streaming: return "streaming-update";
			case Stage::Deformation: return "deformation";
			case Stage::Shadow: return "shadow";
			case Stage::Capture: return "capture";
			case Stage::RayTracing: return "ray-tracing";
			default: return "unknown";
			}
		}

		const char* ToAutomationString(
			TerraEngine::Renderer::VirtualGeometryCapabilityBackend backend)
		{
			using Backend = TerraEngine::Renderer::VirtualGeometryCapabilityBackend;
			switch(backend)
			{
			case Backend::Vulkan: return "vulkan";
			default: return "unknown";
			}
		}

		const char* ToAutomationString(
			TerraEngine::Renderer::VirtualGeometryCapabilityRoute route)
		{
			using Route = TerraEngine::Renderer::VirtualGeometryCapabilityRoute;
			switch(route)
			{
			case Route::MeshShader: return "mesh-shader";
			case Route::ComputeRaster: return "compute-raster";
			case Route::IndexedFallback: return "indexed-fallback";
			default: return "unknown";
			}
		}

		const char* ToAutomationString(
			TerraEngine::Renderer::VirtualGeometryMeasurementAvailability availability)
		{
			using Availability =
				TerraEngine::Renderer::VirtualGeometryMeasurementAvailability;
			switch(availability)
			{
			case Availability::Pending: return "pending";
			case Availability::Unavailable: return "unavailable";
			case Availability::Invalid: return "invalid";
			case Availability::Measured: return "measured";
			default: return "invalid";
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
				{"rasterPacketCounts", {
					{"hardware", view.hardwareRasterPacketCount},
					{"compute", view.computeRasterPacketCount},
					{"indexed", view.indexedRasterPacketCount}
				}},
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
				{"animatedVirtualGeometry", {
					{"pageDemands", view.animatedPageDemandCount},
					{"uniqueDispatches", view.animatedUniqueDispatchCount},
					{"reuseCount", view.animatedReuseCount},
					{"animationLods", view.animatedAnimationLods},
					{"skippedWorkCount", view.animatedSkippedWorkCount},
					{"boundsSource", view.animatedBoundsSource},
					{"fallbackCount", view.animatedFallbackCount},
					{"currentBytes", view.deformationCurrentBytes},
					{"historyBytes", view.deformationHistoryBytes},
					{"boundsBytes", view.deformationBoundsBytes},
					{"highWaterBytes", view.deformationHighWaterBytes},
					{"shaderContractValid", view.animatedShaderContractValid},
					{"fallbackReason", view.animatedFallbackReason},
					{"diagnosticEmitted", view.animatedDiagnosticEmitted},
					{"diagnosticCoalesced", view.animatedDiagnosticCoalesced},
					{"diagnosticDropped", view.animatedDiagnosticDropped}
				}},
				{"performanceGates", std::move(performanceGates)},
				{"shadowVisibilitySelections", std::move(shadowSelections)},
				{"warnings", view.warnings}
			};
		}

		// Diagnostics may expose only the one explicitly requested frame.  The V1
		// catalog/index contract deliberately has no whole-session frame snapshot.
		nlohmann::json SerializeProfilerTimingStreams(const EditorProfilerPresentationSnapshot* presentation)
		{
			if (presentation == nullptr || !presentation->selectedFrame.has_value())
			{
				return {{"available", false}, {"timingStreams", nlohmann::json::array()},
					{"protectedBoundaryTimings", nlohmann::json::array()}, {"workVolumes", nlohmann::json::object()}};
			}

			const auto& frame = *presentation->selectedFrame;
			nlohmann::json timingStreams = nlohmann::json::array();
			for (const auto& stream : frame.timingStreams)
			{
				timingStreams.push_back({{"id", stream.id}, {"label", stream.label},
					{"status", TerraEngine::Profiling::ToString(stream.status)},
					{"milliseconds", stream.milliseconds}, {"reason", stream.statusReason}});
			}
			const auto serializeDuration = [](const TerraEngine::Profiling::TelemetryDuration& duration) {
				return nlohmann::json{{"available", duration.availability
					== TerraEngine::Profiling::TelemetryAvailability::Available},
					{"nanoseconds", duration.nanoseconds}, {"reason", duration.unavailableReason}};
			};
			nlohmann::json protectedBoundaryTimings = nlohmann::json::array();
			for (const auto& boundary : frame.protectedBoundaries)
			{
				protectedBoundaryTimings.push_back({{"name", boundary.boundaryName},
					{"queue", serializeDuration(boundary.queueDuration)},
					{"ownershipWait", serializeDuration(boundary.ownershipWaitDuration)},
					{"execution", serializeDuration(boundary.executionDuration)},
					{"flush", serializeDuration(boundary.flushDuration)},
					{"publication", serializeDuration(boundary.publicationDuration)}});
			}
			nlohmann::json workVolumes = nlohmann::json::object();
			if (frame.physicsWorkVolume.has_value())
			{
				const auto& volume = *frame.physicsWorkVolume;
				workVolumes["physics"] = {{"tick", volume.tick}, {"total", volume.totalBodyCount},
					{"active", volume.activeBodyCount}, {"examined", volume.examinedBodyCount},
					{"extracted", volume.extractedBodyCount}, {"converted", volume.convertedBodyCount},
					{"changed", volume.changedBodyCount}, {"published", volume.publishedBodyCount}};
			}
			return {{"available", true}, {"frameIndex", frame.frameIndex}, {"durationMs", frame.durationMs},
				{"timingStreams", std::move(timingStreams)},
				{"protectedBoundaryTimings", std::move(protectedBoundaryTimings)},
				{"workVolumes", std::move(workVolumes)}};
		}

		// The persisted V1 summary is a fixed-size artifact header.  It supplies
		// capture evidence without decoding or copying the complete frame history.
		nlohmann::json SerializeProfilerCaptureEvidence(const EditorProfilerPresentationSnapshot* presentation)
		{
			if (presentation == nullptr || !presentation->selectedSummary.has_value())
			{
				return {{"formatIdentifier", "TerraEngineProfilerJournalV1"}, {"formatVersion", 1u}, {"available", false}};
			}

			const auto& summary = *presentation->selectedSummary;
			return {{"formatIdentifier", "TerraEngineProfilerJournalV1"}, {"formatVersion", 1u}, {"available", true},
				{"committedFrameCount", summary.frameCount}, {"completeFrameCount", summary.completeFrameCount},
				{"committedSegmentCount", summary.committedSegmentCount}, {"totalDurationNanoseconds", summary.totalDurationNanoseconds},
				{"droppedDetailRecords", summary.droppedDetailRecordCount}, {"droppedDetailBytes", summary.droppedDetailByteCount},
				{"recoveredIncompleteTail", presentation->selectedCapture.has_value()
					&& presentation->selectedCapture->artifact.recoveredIncompleteTail}};
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
				{"criticalPathTimeNs", summary.criticalPathTimeNs},
				{"dependencyWaveCount", summary.dependencyWaveCount},
				{"workerCompletionWakeCount", summary.workerCompletionWakeCount},
				{"completionDrivenDispatchCount", summary.completionDrivenDispatchCount},
				{"peakConcurrentWorkerCount", summary.peakConcurrentWorkerCount},
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

		const char* WorkloadPathText(TerraEngine::Profiling::WorkloadExecutionPath path)
		{
			switch (path)
			{
			case TerraEngine::Profiling::WorkloadExecutionPath::Inline: return "inline";
			case TerraEngine::Profiling::WorkloadExecutionPath::Worker: return "worker";
			case TerraEngine::Profiling::WorkloadExecutionPath::Protected: return "protected";
			case TerraEngine::Profiling::WorkloadExecutionPath::Rejected: return "rejected";
			case TerraEngine::Profiling::WorkloadExecutionPath::None:
			default: return "omitted";
			}
		}

		const char* WorkloadStatusText(TerraEngine::Profiling::WorkloadExecutionStatus status)
		{
			switch (status)
			{
			case TerraEngine::Profiling::WorkloadExecutionStatus::NotReady: return "notReady";
			case TerraEngine::Profiling::WorkloadExecutionStatus::Inline: return "inline";
			case TerraEngine::Profiling::WorkloadExecutionStatus::Queued: return "queued";
			case TerraEngine::Profiling::WorkloadExecutionStatus::Running: return "running";
			case TerraEngine::Profiling::WorkloadExecutionStatus::Completed: return "completed";
			case TerraEngine::Profiling::WorkloadExecutionStatus::Rejected: return "rejected";
			case TerraEngine::Profiling::WorkloadExecutionStatus::Cancelled: return "cancelled";
			case TerraEngine::Profiling::WorkloadExecutionStatus::Inactive:
			default: return "inactive";
			}
		}

		const char* WorkerActivityText(ThreadPoolWorkerActivity activity)
		{
			switch (activity)
			{
			case ThreadPoolWorkerActivity::Idle: return "idle";
			case ThreadPoolWorkerActivity::Running: return "running";
			case ThreadPoolWorkerActivity::Stopped: return "stopped";
			case ThreadPoolWorkerActivity::Starting:
			default: return "starting";
			}
		}

		nlohmann::json SerializeDuration(const TerraEngine::Profiling::TelemetryDuration& duration)
		{
			return {
				{"available", duration.availability == TerraEngine::Profiling::TelemetryAvailability::Available},
				{"nanoseconds", duration.nanoseconds},
				{"unavailableReason", duration.unavailableReason}
			};
		}

		nlohmann::json SerializeParallelFrameDiagnostics(const ::Engine* engine, bool includeDetails)
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
			nlohmann::json executionRecords = nlohmann::json::array();
			nlohmann::json dependencyEdges = nlohmann::json::array();
			nlohmann::json workers = nlohmann::json::array();
			const std::shared_ptr<const ThreadPoolTelemetrySnapshot> telemetry = ThreadPool::instance().GetTelemetrySnapshot();
			// Retained per-dispatch histories can be large, so compact callers receive
			// the stable envelope and aggregate sampling facts without copying arrays.
			if (includeDetails)
			{
				for (const TerraEngine::Profiling::WorkloadExecutionRecord& record : plan.executionRecords)
				{
					executionRecords.push_back({
						{"descriptorId", record.descriptorId}, {"descriptorGeneration", record.descriptorGeneration},
						{"ownerSystem", record.ownerSystem}, {"phase", record.phase}, {"workloadName", record.workloadName},
						{"activationPredicate", record.activationPredicateKey}, {"active", record.active}, {"ready", record.ready},
						{"path", WorkloadPathText(record.selectedPath)}, {"status", WorkloadStatusText(record.status)},
						{"workerIdentityAvailable", record.workerIdentityAvailable}, {"workerId", record.workerId},
						{"queueDepthAtEnqueue", record.queueDepthAtEnqueue}, {"queueDepthAtStart", record.queueDepthAtStart},
						{"dispatchCount", record.dispatchCount}, {"usefulWorkItemCount", record.usefulWorkItemCount},
						{"dependencyWave", record.dependencyWave}, {"completionSequence", record.completionSequence},
						{"partitionIndex", record.partitionIndex}, {"partitionCount", record.partitionCount},
						{"itemBegin", record.itemBegin}, {"itemEnd", record.itemEnd},
						{"sameFrameVisibilityRequired", record.sameFrameVisibilityRequired},
						{"queueDuration", SerializeDuration(record.queueDuration)}, {"runDuration", SerializeDuration(record.runDuration)},
						{"coordinatorWaitDuration", SerializeDuration(record.coordinatorWaitDuration)},
						{"ownershipWaitDuration", SerializeDuration(record.ownershipWaitDuration)},
						{"flushDuration", SerializeDuration(record.flushDuration)}, {"publicationDuration", SerializeDuration(record.publicationDuration)}
					});
				}
				for (const TerraEngine::Scheduling::FrameWorkloadDeclaration& workload : plan.workloads)
				{
					for (const std::string& dependency : workload.explicitDependencies)
					{
						dependencyEdges.push_back({{"beforeDescriptorId", dependency}, {"afterDescriptorId", workload.stableDescriptorId}, {"afterWorkload", workload.workloadName}});
					}
				}
				for (const ThreadPoolWorkerTelemetry& worker : telemetry->workers)
				{
					nlohmann::json history = nlohmann::json::array();
					for (const ThreadPoolWorkerTelemetrySample& sample : worker.history)
					{
						history.push_back({{"sequence", sample.sequence}, {"queueDepthAtEnqueue", sample.queueDepthAtEnqueue}, {"queueDepthAtStart", sample.queueDepthAtStart}, {"usefulItemCount", sample.usefulItemCount}, {"waitNanoseconds", sample.waitNanoseconds}, {"runNanoseconds", sample.runNanoseconds}, {"idleNanoseconds", sample.idleNanoseconds}});
					}
					workers.push_back({{"workerId", worker.workerIndex}, {"activity", WorkerActivityText(worker.activity)}, {"latestQueueDepthAtEnqueue", worker.latestQueueDepthAtEnqueue}, {"latestQueueDepthAtStart", worker.latestQueueDepthAtStart}, {"dispatchCount", worker.dispatchCount}, {"usefulItemCount", worker.usefulItemCount}, {"waitNanoseconds", worker.waitNanoseconds}, {"runNanoseconds", worker.runNanoseconds}, {"idleNanoseconds", worker.idleNanoseconds}, {"droppedHistorySampleCount", worker.droppedHistorySampleCount}, {"history", history}});
				}
			}
			const TerraEngine::Scheduling::ScheduleSummary& schedule = engine->GetSchedulerSummary();

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
				{"droppedExecutionRecordCount", plan.droppedExecutionRecordCount},
				{"executionTelemetry", nlohmann::json{{"detailIncluded", includeDetails}, {"executionRecords", executionRecords}, {"dependencyEdges", dependencyEdges}, {"workerTelemetryGeneration", telemetry->generation}, {"workerTelemetrySamplingIntervalDispatches", telemetry->samplingIntervalDispatches}, {"workerHistoryCapacityPerWorker", telemetry->historyCapacityPerWorker}, {"droppedWorkerHistorySampleCount", telemetry->droppedHistorySampleCount}, {"workers", workers}, {"conditionalFlush", nlohmann::json{{"afterPhysicsActive", schedule.afterPhysicsFlushActive}, {"consumer", schedule.afterPhysicsConsumer}, {"reason", schedule.afterPhysicsReason}}}}},
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
				{"accessRejectedMutationCount", scripting.accessReport.rejectedMutations},
				// Frame outcomes distinguish an empty workload from callbacks that were
				// deliberately skipped or failed after managed dispatch was attempted.
				{"scriptingFrame", {
					{"frameId", scripting.frameId},
					{"totalScripts", scripting.accessReport.totalScripts},
					{"updatedScripts", scripting.updatedScripts},
					{"skippedScripts", scripting.skippedScripts},
					{"failedScripts", scripting.failedScripts}
				}}
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

	nlohmann::json EditorAutomationService::SummarizeFramePublicationEvidence(
		const std::vector<TerraEngine::Profiling::ProfilerFrameRecord>& frames)
	{
		// Storage lane 6 is deliberately reserved by the profiler contract. Keep
		// automation aligned with the user-facing seven-lane table rather than
		// exposing that internal slot as an invented phase.
		constexpr std::array<std::size_t, 7U> kTimingIndices{0U, 1U, 2U, 3U, 4U, 5U, 7U};
		constexpr std::array<const char*, 7U> kTimingIds{
			"persistent_state", "dynamic_refresh", "view_selection",
			"animation_evaluation", "protected_publication", "backend_preparation",
			"uninstrumented_remainder"};
		constexpr std::array<const char*, 7U> kTimingLabels{
			"Persistent state maintenance", "Dynamic refresh", "View selection",
			"Animation evaluation", "Protected publication", "Backend preparation",
			"Uninstrumented remainder"};
		struct LaneAggregate
		{
			std::uint64_t availableFrameCount = 0U;
			std::uint64_t unavailableFrameCount = 0U;
			std::uint64_t totalNanoseconds = 0U;
		};
		std::array<LaneAggregate, kTimingIndices.size()> lanes{};
		std::uint64_t evidenceFrameCount = 0U;
		std::uint64_t missingEvidenceFrameCount = 0U;
		std::uint64_t completeFrameCount = 0U;
		std::uint64_t incompleteFrameCount = 0U;
		std::uint64_t availabilityMaskUnion = 0U;
		std::uint64_t availabilityMaskIntersection = 0U;
		bool hasAvailabilityMask = false;
		std::uint64_t conservationEligibleFrameCount = 0U;
		std::uint64_t conservedFrameCount = 0U;
		std::uint64_t unconservedFrameCount = 0U;
		std::uint64_t unavailableConservationFrameCount = 0U;
		std::uint64_t invalidDurationFrameCount = 0U;
		std::uint64_t totalConservationFrameNanoseconds = 0U;
		std::uint64_t totalAttributedNanoseconds = 0U;
		std::uint64_t totalRemainderNanoseconds = 0U;
		std::uint64_t maximumAbsoluteConservationErrorNanoseconds = 0U;
		std::uint64_t invalidReservedLaneFrameCount = 0U;
		std::uint64_t invalidReservedLaneNanoseconds = 0U;

		const auto saturatingAdd = [](std::uint64_t& destination, std::uint64_t value) {
			if (value > std::numeric_limits<std::uint64_t>::max() - destination)
			{
				destination = std::numeric_limits<std::uint64_t>::max();
				return;
			}
			destination += value;
		};
		const auto frameDurationNanoseconds = [](double durationMs) -> std::optional<std::uint64_t> {
			constexpr double kNanosecondsPerMillisecond = 1'000'000.0;
			const double maximumMilliseconds = static_cast<double>(
				std::numeric_limits<std::uint64_t>::max()) / kNanosecondsPerMillisecond;
			if (!std::isfinite(durationMs) || durationMs < 0.0
				|| durationMs > maximumMilliseconds)
			{
				return std::nullopt;
			}
			return static_cast<std::uint64_t>(std::llround(
				durationMs * kNanosecondsPerMillisecond));
		};

		for (const TerraEngine::Profiling::ProfilerFrameRecord& frame : frames)
		{
			if (frame.complete)
			{
				++completeFrameCount;
			}
			else
			{
				++incompleteFrameCount;
			}
			if (!frame.framePublication.has_value())
			{
				++missingEvidenceFrameCount;
				for (LaneAggregate& lane : lanes)
				{
					++lane.unavailableFrameCount;
				}
				continue;
			}

			++evidenceFrameCount;
			const TerraEngine::Profiling::ProfilerFramePublicationEvidence& evidence =
				*frame.framePublication;
			availabilityMaskUnion |= evidence.availabilityMask;
			availabilityMaskIntersection = hasAvailabilityMask
				? availabilityMaskIntersection & evidence.availabilityMask
				: evidence.availabilityMask;
			hasAvailabilityMask = true;

			bool allPublicLanesAvailable = true;
			std::uint64_t attributedNanoseconds = 0U;
			for (std::size_t laneIndex = 0U; laneIndex < lanes.size(); ++laneIndex)
			{
				const bool laneAvailable = (evidence.availabilityMask & (1ULL << laneIndex)) != 0U;
				LaneAggregate& lane = lanes[laneIndex];
				if (!laneAvailable)
				{
					++lane.unavailableFrameCount;
					allPublicLanesAvailable = false;
					continue;
				}
				const std::uint64_t nanoseconds =
					evidence.disjointTimingNanoseconds[kTimingIndices[laneIndex]];
				++lane.availableFrameCount;
				saturatingAdd(lane.totalNanoseconds, nanoseconds);
				saturatingAdd(attributedNanoseconds, nanoseconds);
			}
			if (evidence.disjointTimingNanoseconds[6U] != 0U)
			{
				++invalidReservedLaneFrameCount;
				saturatingAdd(invalidReservedLaneNanoseconds,
					evidence.disjointTimingNanoseconds[6U]);
			}
			// Interrupted capture-tail frames can retain useful per-lane evidence, but
			// never reached EndFrame's complete frame-clock boundary. Keep their lane
			// availability above while excluding them from reconciliation statistics.
			if (!frame.complete)
			{
				continue;
			}
			if (!allPublicLanesAvailable)
			{
				++unavailableConservationFrameCount;
				continue;
			}
			const std::optional<std::uint64_t> expectedNanoseconds =
				frameDurationNanoseconds(frame.durationMs);
			if (!expectedNanoseconds.has_value())
			{
				++invalidDurationFrameCount;
				continue;
			}

			++conservationEligibleFrameCount;
			saturatingAdd(totalConservationFrameNanoseconds, *expectedNanoseconds);
			saturatingAdd(totalAttributedNanoseconds, attributedNanoseconds);
			saturatingAdd(totalRemainderNanoseconds,
				evidence.disjointTimingNanoseconds[kTimingIndices.back()]);
			const std::uint64_t absoluteError = attributedNanoseconds >= *expectedNanoseconds
				? attributedNanoseconds - *expectedNanoseconds
				: *expectedNanoseconds - attributedNanoseconds;
			maximumAbsoluteConservationErrorNanoseconds = std::max(
				maximumAbsoluteConservationErrorNanoseconds, absoluteError);
			if (absoluteError == 0U)
			{
				++conservedFrameCount;
			}
			else
			{
				++unconservedFrameCount;
			}
		}

		nlohmann::json serializedLanes = nlohmann::json::array();
		for (std::size_t laneIndex = 0U; laneIndex < lanes.size(); ++laneIndex)
		{
			const LaneAggregate& lane = lanes[laneIndex];
			serializedLanes.push_back({
				{"id", kTimingIds[laneIndex]},
				{"label", kTimingLabels[laneIndex]},
				{"available", lane.availableFrameCount != 0U},
				{"availableFrameCount", lane.availableFrameCount},
				{"unavailableFrameCount", lane.unavailableFrameCount},
				{"totalNanoseconds", lane.totalNanoseconds}});
		}
		return {
			{"available", evidenceFrameCount != 0U},
			{"frameCount", frames.size()},
			{"completeFrameCount", completeFrameCount},
			{"incompleteFrameCount", incompleteFrameCount},
			{"evidenceFrameCount", evidenceFrameCount},
			{"missingEvidenceFrameCount", missingEvidenceFrameCount},
			{"availabilityMaskUnion", availabilityMaskUnion},
			{"availabilityMaskIntersection", hasAvailabilityMask ? availabilityMaskIntersection : 0U},
			{"lanes", std::move(serializedLanes)},
			{"conservation", {
				{"eligibleFrameCount", conservationEligibleFrameCount},
				{"conservedFrameCount", conservedFrameCount},
				{"unconservedFrameCount", unconservedFrameCount},
				{"unavailableFrameCount", unavailableConservationFrameCount},
				{"invalidDurationFrameCount", invalidDurationFrameCount},
				{"totalFrameNanoseconds", totalConservationFrameNanoseconds},
				{"totalAttributedNanoseconds", totalAttributedNanoseconds},
				{"totalRemainderNanoseconds", totalRemainderNanoseconds},
				{"maximumAbsoluteErrorNanoseconds", maximumAbsoluteConservationErrorNanoseconds},
				{"invalidReservedLaneFrameCount", invalidReservedLaneFrameCount},
				{"invalidReservedLaneNanoseconds", invalidReservedLaneNanoseconds}}}
		};
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
		// Animation owns a domain-specific resolver because its generation-safe
		// transactions, retained jobs, and renderer captures must never fall through
		// the legacy central operation switch.
		const AutomationHandlerResolver resolveAnimationHandler = [this](std::string_view name) {
			return ResolveAnimationAutomationHandler(name);
		};
		(void)capabilityRegistry.RegisterProvider(std::make_shared<AnimationAutomationProvider>(resolveAnimationHandler));
		// Physics names are resolved once into domain-specific typed handlers. They
		// never enter the legacy request-time central string dispatcher above.
		const AutomationHandlerResolver resolvePhysicsHandler = [this](std::string_view name) {
			return ResolvePhysicsAutomationHandler(name);
		};
		(void)capabilityRegistry.RegisterProvider(std::make_shared<PhysicsAutomationProvider>(resolvePhysicsHandler));
		const AutomationHandlerResolver resolveAudioHandler = [](std::string_view name) {
			const std::string operation(name);
			return AutomationHandler{[operation](const AutomationRequestContext& request) {
				AutomationDispatchResult result;
				result.error = ErrorCategory::Validation;
				auto& audio = TerraEngine::Audio::AudioSystem::Instance();
				if(operation == "audio.inspect") {
					const auto playback = audio.LatestPlaybackSnapshot();
					const auto mix = audio.LatestMixSnapshot();
					result.ok = true; result.data = {{"frame", playback ? playback->frame : 0U},
						{"voiceCount", playback ? playback->voices.size() : 0U}, {"masterVolume", mix.master.volume},
						{"masterMuted", mix.master.muted}, {"mixGeneration", mix.generation}};
					return result;
				}
				if(operation == "audio.mix") {
					std::string diagnostic;
					result.ok = audio.SetMasterMix(request.arguments.value("volume", 1.0F), request.arguments.value("muted", false), diagnostic);
					result.message = diagnostic; result.data = {{"accepted", result.ok}, {"visibilityBoundary", "next_audio_commit"}};
					return result;
				}
				const auto objectId = TerraEngine::ParseUUID(request.arguments.value("objectId", std::string{}));
				if(!objectId) { result.message = "audio.control requires a 32-character objectId."; return result; }
				TerraEngine::Audio::AudioControlCommand command;
				command.source.objectId = *objectId;
				command.source.objectGeneration = request.arguments.value("objectGeneration", 0U);
				command.source.componentGeneration = request.arguments.value("componentGeneration", 0U);
				command.source.playbackGeneration = request.arguments.value("playbackGeneration", 0U);
				const std::string kind = request.arguments.value("kind", std::string{});
				if(kind == "play") command.kind = TerraEngine::Audio::AudioControlKind::Play;
				else if(kind == "pause") command.kind = TerraEngine::Audio::AudioControlKind::Pause;
				else if(kind == "resume") command.kind = TerraEngine::Audio::AudioControlKind::Resume;
				else if(kind == "stop") command.kind = TerraEngine::Audio::AudioControlKind::Stop;
				else { result.message = "Audio control kind must be play, pause, resume, or stop."; return result; }
				TerraEngine::Audio::AudioControlBatch batch;
				batch.requestedFrame = request.arguments.value("requestedFrame", 0ULL);
				batch.producerId = request.arguments.value("producerId", 2ULL);
				batch.correlationId = request.correlationId; batch.idempotencyKey = request.idempotencyKey;
				batch.requestFingerprint = request.arguments.dump(); batch.commands.push_back(command);
				const auto receipt = audio.SubmitControlBatch(std::move(batch));
				result.ok = receipt.status != TerraEngine::Audio::AudioControlStatus::Rejected;
				result.error = result.ok ? ErrorCategory::Internal : ErrorCategory::QueueFull;
				result.message = receipt.diagnostic;
				result.data = {{"accepted", result.ok}, {"batchSequence", receipt.batchSequence},
					{"status", static_cast<std::uint32_t>(receipt.status)}, {"rejection", static_cast<std::uint32_t>(receipt.rejection)},
					{"visibilityBoundary", "next_audio_commit"}};
				return result;
			}};
		};
		(void)capabilityRegistry.RegisterProvider(std::make_shared<AudioAutomationProvider>(resolveAudioHandler));
		// Navigation names resolve to a typed domain boundary. In particular, no
		// navigation request can fall through the legacy FindCapability dispatcher.
		const AutomationHandlerResolver resolveNavigationHandler = [this](std::string_view name) {
			return ResolveNavigationAutomationHandler(name);
		};
		(void)capabilityRegistry.RegisterProvider(std::make_shared<NavigationAutomationProvider>(resolveNavigationHandler));
		// Locomotion owns its copied domain adapter. All four handlers are resolved
		// through the typed registry rather than the legacy capability switch.
		(void)capabilityRegistry.RegisterProvider(std::make_shared<LocomotionAutomationProvider>());
		RegisterDefaultCapabilities();
	}

	EditorAutomationService::~EditorAutomationService()
	{
		// Durable sessions are keyed by this service's job store. Retire them before
		// the store address can be reused by a later bridge, otherwise a disconnected
		// recording or database worker could outlive its project/editor owner.
		AnimationAutomationJobs(jobService).CancelAllRecordings();
		animationAutomationDetailedDiagnosticsLease.Reset();
	}

	AutomationHandler EditorAutomationService::ResolveAnimationAutomationHandler(std::string_view name)
	{
		const AutomationOperationId operation = AutomationOperation(name);
		const std::string operationName(name);
		const auto [existing, inserted] = boundProviderOperationNames.emplace(operation, operationName);
		if (!inserted && existing->second != operationName)
		{
			throw std::logic_error("Animation automation operation ID collision between " + existing->second + " and " + operationName + ".");
		}
		return [this, operation, operationName](const AutomationRequestContext& request) {
			return ExecuteAnimationAutomationHandler(operation, operationName, request);
		};
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

	AutomationHandler EditorAutomationService::ResolveNavigationAutomationHandler(std::string_view name)
	{
		const AutomationOperationId operation = AutomationOperation(name);
		const std::string operationName(name);
		const auto [existing, inserted] = boundProviderOperationNames.emplace(operation, operationName);
		if (!inserted && existing->second != operationName)
		{
			throw std::logic_error("Navigation automation operation ID collision between " + existing->second + " and " + operationName + ".");
		}
		return [this, operation, operationName](const AutomationRequestContext& request) {
			return ExecuteNavigationAutomationHandler(operation, operationName, request);
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

	void EditorAutomationService::PumpAnimationRecordingsAtProtectedFrame()
	{
		// AnimationAutomationJobs loads only sealed published snapshots here; no UI,
		// MCP connection, or job.get request participates in capture ownership.
		AnimationAutomationJobs(jobService).PumpActiveRecordingsFromPublishedSnapshots();
	}

	bool EditorAutomationService::RestoreRendererEfficiencySourcesAtProtectedFrame(std::string& error)
	{
		if(rendererEfficiencyDirtySources.empty()) { error.clear(); return true; }
		if(!editorContext || !editorContext->projectManager || !editorContext->assetDatabase)
		{ error = "Owned source restoration is unavailable without its original project."; return false; }
		const auto root = editorContext->projectManager->GetProjectRoot();
		const auto operation = rendererEfficiencyValidation.Status().at("operationId").get<std::string>();
		if(!rendererEfficiencyValidation.ValidateOwnedRestoration(root, BuildAutomationStateSnapshot().projectGeneration, operation, error)) return false;
		// Stop must not leave an authored variant behind for the next run. These
		// rare bounded file replacements are sequential at the protected boundary;
		// expensive compile/reload work continues through native asynchronous jobs.
		for(auto it = rendererEfficiencyDirtySources.begin(); it != rendererEfficiencyDirtySources.end();)
		{
			const auto baseline = rendererEfficiencySourceBaselines.find(*it);
			if(baseline == rendererEfficiencySourceBaselines.end()) { error = "The original owned source bytes are unavailable."; return false; }
			RendererEfficiencySourcePatch patch;
			if(!PrepareRendererEfficiencySourcePatch(root, *it, {}, baseline->second.first, baseline->second.second, patch, error)
				|| !ApplyRendererEfficiencySourcePatch(root, patch, error)) return false;
			baseline->second.second = patch.replacementHash;
			editorContext->assetDatabase->ImportAssetWithProgress(root / *it, true);
			it = rendererEfficiencyDirtySources.erase(it);
		}
		rendererEfficiencyResourceWork = {}; error.clear(); return true;
	}
	void EditorAutomationService::PumpRendererEfficiencyResourceMutationsAtProtectedFrame(RenderSystem& renderer)
	{
		if(!editorContext || !editorContext->projectManager || !editorContext->assetDatabase || !rendererEfficiencyValidation.Active()) return;
		// Marker-only foundation sessions have no resource recipe. Explicit stop
		// restoration remains in its separate existing cleanup entry point.
		if(rendererEfficiencyValidation.InstalledSteps().is_null()) return;
		const auto root = editorContext->projectManager->GetProjectRoot();
		const auto generation = BuildAutomationStateSnapshot().projectGeneration;
		const auto status = rendererEfficiencyValidation.Status();
		const auto operation = status.at("operationId").get<std::string>();
		auto& work = rendererEfficiencyResourceWork;
		std::string error;
		if(!rendererEfficiencyValidation.ValidateOwnedAdmission(root, generation, operation, error)) return;
		try
		{
			if(!work.operationId.empty() && work.operationId != operation) work = {};
			const auto now = std::chrono::steady_clock::now();
			if(!work.phase.empty() && work.phase != "Failed" && now >= work.deadline)
				throw std::runtime_error("Owned resource stage exceeded 300 seconds: " + work.phase + " (" + work.sourcePath.generic_string() + ").");
			// Count an import only when the native database actually retains its
			// same-source job. A declined ingress is bounded rather than resubmitted
			// every frame. Original patch and cleanup bytes survive all retries.
			const auto dispatchImport = [&]() {
				work.phase = "AwaitingImportAdmission"; work.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
				editorContext->assetDatabase->ImportAssetWithProgress(root / work.sourcePath, true);
				if(editorContext->assetDatabase->IsAssetCompiling(root / work.sourcePath)) {
					++work.admittedImports; work.phase = "AwaitingImportSettlement";
				}
			};
			if(work.phase.empty())
			{
				std::filesystem::path source, replacement;
				bool baseline = false, denseBaseline = false;
				const auto denseStep = rendererEfficiencyValidation.PendingDenseResourceBaseline();
				const auto currentIndex = status.at("completedSteps").get<std::size_t>();
				const auto& currentSteps = rendererEfficiencyValidation.InstalledSteps().at("steps");
				const bool densePageStep = currentIndex < currentSteps.size() && currentSteps.at(currentIndex).at("action") == "page-revision";
				const auto restorable = std::find_if(rendererEfficiencyDirtySources.begin(), rendererEfficiencyDirtySources.end(), [&](const auto& path) {
					return !densePageStep || !rendererEfficiencyDenseSourceBaselines.contains(path);
				});
				// Prepare dense geometry before this step's normal authored/animation
				// baseline. Subsequent steps still restore the original direct source.
				if(!denseStep.is_null())
				{
					if(!rendererEfficiencyValidation.BeginResourceBaselineReset(root, generation, error, true)) return;
					baseline = denseBaseline = true;
					source = std::filesystem::path("Assets") / denseStep.at("nativePayload").at("sourcePath").get<std::string>();
					replacement = denseStep.at("nativePayload").at("denseBaselineReplacementPath").get<std::string>();
				}
				else if(status.at("state") == "PreparingBaseline" && restorable != rendererEfficiencyDirtySources.end())
				{
					if(!rendererEfficiencyValidation.BeginResourceBaselineReset(root, generation, error)) return;
					baseline = true; source = *restorable;
				}
				else
				{
					const auto step = rendererEfficiencyValidation.PendingNativeStep();
					if(step.is_null() || !step.at("nativePayload").contains("replacementPath")) return;
					source = std::filesystem::path("Assets") / step.at("nativePayload").at("sourcePath").get<std::string>();
					replacement = step.at("nativePayload").at("replacementPath").get<std::string>();
				}
				work.operationId = operation; work.stepId = status.at("fixtureStepId").get<std::string>();
				work.sourcePath = source; work.baseline = baseline; work.denseBaseline = denseBaseline; work.phase = "PreparingPatch";
				work.deadline = now + std::chrono::seconds(300);
				for(const auto& asset : rendererEfficiencyValidation.InstalledSteps().at("assets"))
					if(std::filesystem::path("Assets") / asset.at("sourcePath").get<std::string>() == source) {
						const auto id = TerraEngine::ParseUUID(asset.at("packageGuid").get<std::string>());
						if(id) work.packageId = *id;
					}
				if(work.packageId == TerraEngine::EMPTY_UUID) throw std::runtime_error("Owned resource source has no sealed package identity.");
				const auto found = rendererEfficiencySourceBaselines.find(source);
				const auto retained = found == rendererEfficiencySourceBaselines.end() ? std::pair<std::string, std::string>{} : found->second;
				const auto semantic = rendererEfficiencyDenseSourceBaselines.find(source);
				const auto semanticBytes = !baseline && semantic != rendererEfficiencyDenseSourceBaselines.end() ? semantic->second : std::string{};
				auto task = ThreadPool::instance().createTask([root, source, replacement, retained, semanticBytes]() {
					RendererEfficiencySourcePatch patch; std::string reason;
					if(!semanticBytes.empty()) (void)PrepareRendererEfficiencyPageVariantPatch(root, source, replacement, retained.first, retained.second, semanticBytes, patch, reason);
					else (void)PrepareRendererEfficiencySourcePatch(root, source, replacement, retained.first, retained.second, patch, reason);
					return std::pair{std::move(patch), std::move(reason)};
				});
				work.patchJob = task->getFuture(); ThreadPool::instance().enqueue(task); return;
			}
			if(work.phase == "PreparingPatch")
			{
				if(work.patchJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
				auto [patch, reason] = work.patchJob.get();
				if(!reason.empty()) throw std::runtime_error(reason);
				// The second protected check is after detached file preparation and
				// before either authored input or importer ingress changes.
				if(!rendererEfficiencyValidation.ValidateOwnedAdmission(root, generation, operation, reason)) throw std::runtime_error(reason);
				if(!ApplyRendererEfficiencySourcePatch(root, patch, reason)) throw std::runtime_error(reason);
				// Never replace cleanup's original topology with the dense comparison
				// baseline retained inside a subsequent page-edit receipt.
				auto [original, inserted] = rendererEfficiencySourceBaselines.try_emplace(work.sourcePath, patch.baselineBytes, patch.replacementHash);
				original->second.second = patch.replacementHash;
				if(work.denseBaseline) rendererEfficiencyDenseSourceBaselines[work.sourcePath] = patch.replacementBytes;
				else if(work.baseline) rendererEfficiencyDenseSourceBaselines.erase(work.sourcePath);
				rendererEfficiencyDirtySources.insert(work.sourcePath); work.patch = std::move(patch);
				work.priorGeneration = ResourceManager::Instance().GetRendererResourcePublicationGeneration();
				dispatchImport();
			}
			if(work.phase == "AwaitingImportAdmission")
			{
				rendererEfficiencyValidation.ReportNativeResourcePending("Awaiting actual same-source import admission: " + work.sourcePath.generic_string());
				if(!editorContext->assetDatabase->IsAssetCompiling(root / work.sourcePath)) return;
				++work.admittedImports; work.phase = "AwaitingImportSettlement"; work.deadline = now + std::chrono::seconds(300);
			}
			if(work.phase == "AwaitingImportSettlement")
			{
				rendererEfficiencyValidation.ReportNativeResourcePending("Awaiting physical same-source import settlement (attempt " + std::to_string(work.admittedImports) + "/3): " + work.sourcePath.generic_string());
				if(editorContext->assetDatabase->IsAssetCompiling(root / work.sourcePath)) return;
				work.phase = "AwaitingImportReceipt"; work.deadline = now + std::chrono::seconds(300);
			}
			if(work.phase == "AwaitingImportReceipt")
			{
				// Discovery can admit a same-source job between two protected polls.
				// Never classify or retry a terminal sidecar while that owner is still
				// physically retained; this also includes canceled draining jobs.
				if(editorContext->assetDatabase->IsAssetCompiling(root / work.sourcePath)) {
					rendererEfficiencyValidation.ReportNativeResourcePending("Awaiting physical same-source import settlement before receipt decision."); return;
				}
				if(!work.importJob.valid() && std::chrono::steady_clock::now() >= work.retry)
				{
					const auto steps = rendererEfficiencyValidation.InstalledSteps();
					const auto source = work.sourcePath; const auto expectedSourceHash = work.patch->replacementHash; const auto package = work.packageId;
					auto task = ThreadPool::instance().createTask([root, steps, source, expectedSourceHash, package]() {
						RendererEfficiencyResourceWork::ImportPreparation result;
						// The copied typed observation binds terminal decisions to actual
						// source SHA and package identity, not an error-message substring.
						result.observation = InspectRendererEfficiencyOwnedImport(root, source, package, expectedSourceHash);
						if(result.observation.disposition == RendererEfficiencyImportDisposition::Ready)
							(void)PrepareRendererEfficiencyFixtureBindings(root, steps, result.prepared, result.error);
						return result;
					});
					work.importJob = task->getFuture(); ThreadPool::instance().enqueue(task);
				}
				if(!work.importJob.valid() || work.importJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
				auto result = work.importJob.get();
				const auto decision = DecideRendererEfficiencyImportAdmission(result.observation, work.admittedImports, work.lastFailedImportGeneration);
				if(decision == RendererEfficiencyImportDecision::Fail)
					throw std::runtime_error("Owned import admission failed after " + std::to_string(work.admittedImports) + "/3 imports: " + result.observation.reason);
				if(decision == RendererEfficiencyImportDecision::Reimport) {
					work.lastFailedImportGeneration = result.observation.generation;
					// Settlement was proven above; no second source patch is applied.
					dispatchImport(); return;
				}
				if(decision == RendererEfficiencyImportDecision::Wait || !result.error.empty()) {
					rendererEfficiencyValidation.ReportNativeResourcePending("Awaiting strict owned import receipt/bindings: " + (result.error.empty() ? result.observation.reason : result.error));
					work.retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(250); return;
				}
				work.preparation = std::move(result.prepared);
				for(const auto& candidate : work.preparation->candidates)
				{
					const auto& binding = candidate.binding;
					if(binding.sourcePath != std::filesystem::canonical(root / work.sourcePath)) continue;
					auto& resources = ResourceManager::Instance();
					const auto type = binding.kind == "Texture" ? AssetType::Texture : binding.kind == "Material" ? AssetType::Material : AssetType::Mesh;
					const auto prior = resources.TryCopyRendererEfficiencyResourcePublication(binding.assetId, type);
					if(prior) work.priorAssetGenerations[binding.assetId] = prior->resourceGeneration;
					if(binding.kind == "Mesh" || binding.kind == "DeformableMesh") work.reloads.emplace_back(binding.assetId, resources.ReloadMeshAsync(binding.assetId));
					else if(binding.kind == "Material") work.reloads.emplace_back(binding.assetId, resources.ReloadMaterialAsync(binding.assetId));
					else if(binding.kind == "Texture") work.reloads.emplace_back(binding.assetId, resources.ReloadTexturePublicationAsync(binding.assetId));
				}
				if(work.reloads.empty()) throw std::runtime_error("The owned source has no supported actual resource reloads.");
				work.phase = "AwaitingNativePublication";
				work.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
			}
			if(work.phase == "AwaitingNativePublication")
			{
				rendererEfficiencyValidation.ReportNativeResourcePending("Awaiting exact native resource publication: " + work.sourcePath.generic_string());
				for(const auto& [id, future] : work.reloads)
				{
					if(future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
					if(future.get() != id) throw std::runtime_error("Native resource reload did not publish the expected asset identity.");
				}
				if(ResourceManager::Instance().GetRendererResourcePublicationGeneration() <= work.priorGeneration) return;
				std::vector<RendererEfficiencyPublishedBinding> bindings;
				if(!ResolveRendererEfficiencyFixtureBindings(*work.preparation, ResourceManager::Instance(), bindings, error)) {
					rendererEfficiencyValidation.ReportNativeResourcePending("Awaiting strict native publication: " + error); return;
				}
				// Invalidate only after the real CPU reload and immutable cache snapshot
				// agree. The protected renderer capture then reuploads that owner;
				// acceptance still requires the subsequently completed GPU evidence.
				for(const auto& [id, future] : work.reloads)
				{
					const auto binding = std::find_if(bindings.begin(), bindings.end(), [&](const auto& value) { return value.assetId == id; });
					if(binding == bindings.end()) throw std::runtime_error("Reloaded resource is absent from fresh native binding receipt.");
					const auto prior = work.priorAssetGenerations.find(id);
					if(prior == work.priorAssetGenerations.end() || !prior->second || binding->resourceGeneration <= prior->second) throw std::runtime_error("Native asset publication did not advance from its exact captured owner generation.");
					const auto type = binding->kind == "Texture" ? AssetType::Texture : binding->kind == "Material" ? AssetType::Material : AssetType::Mesh;
					if(!renderer.QueueRendererResourcePublicationInvalidation(id, type, binding->resourceGeneration, &error)) throw std::runtime_error(error);
					const auto currentStep = rendererEfficiencyValidation.InstalledSteps().at("steps").at(status.at("completedSteps").get<std::size_t>());
					const nlohmann::json publication{{"kind", "ResourcePublication"}, {"fixtureStepId", work.stepId}, {"action", work.denseBaseline ? "dense-page-baseline" : work.baseline ? "baseline-restoration" : currentStep.at("action").get<std::string>()},
						{"assetId", TerraEngine::ToHexString(id)}, {"assetType", binding->kind}, {"sourcePath", work.sourcePath.generic_string()},
						{"sourceSha256", work.patch->replacementHash}, {"baselineSourceSha256", work.patch->baselineHash},
						{"publicationGeneration", binding->resourceGeneration}, {"previousPublicationGeneration", prior->second}, {"actualNativeOwnerAccepted", true}};
					if(!rendererEfficiencyValidation.RecordFixtureReceipt(root, generation, operation, publication, error)) throw std::runtime_error(error);
				}
				if(work.denseBaseline)
				{
					const auto grid = std::find_if(bindings.begin(), bindings.end(), [](const auto& binding) { return binding.bindingKey == "grid.mesh"; });
					if(grid == bindings.end() || !rendererEfficiencyValidation.RetainDenseResourceBaselineOwner(root, generation, grid->assetId, grid->resourceGeneration, error)) throw std::runtime_error(error.empty() ? "Dense baseline has no exact named grid publication." : error);
				}
				if(!rendererEfficiencyValidation.AdmitPublishedFixtureBindings(root, generation, operation, bindings,
					ObjectManager::instance(), TerraEngine::Scene::SceneMutationQueue::Instance(), error)) throw std::runtime_error(error);
				if(work.baseline && !work.denseBaseline) rendererEfficiencyDirtySources.erase(work.sourcePath);
				work = {};
			}
		}
		catch(const std::exception& exception)
		{
			// A failed real importer/cache publication cannot be converted into a
			// fixture frame. Retain cleanup bytes and owned Stop identity while
			// ending the collector promptly with the actual terminal limitation.
			rendererEfficiencyValidation.FailNativeResourcePreparation(exception.what());
			renderer.ClearRendererEfficiencyFixtureIntent(operation);
			work.phase = "Failed";
		}
	}
	bool EditorAutomationService::PumpRendererEfficiencyDeviceRecreationAtProtectedFrame(RenderSystem& renderer)
	{
#if defined(TERRAENGINE_EDITOR_TESTS)
		(void)renderer;
		const auto pending = rendererEfficiencyValidation.PendingNativeStep();
		if(!pending.is_null() && pending.at("action") == "device-recovery")
			rendererEfficiencyValidation.ReportNativeUnavailable("The headless editor test lane has no native Vulkan/UI device recreation provider.");
		return true;
#else
		if(!rendererEfficiencyValidation.Active() || !editorContext || !editorContext->engine || !editorContext->projectManager) return true;
		auto* editor = dynamic_cast<EditorSystem*>(editorContext->engine->GetEditorSystem());
		if(!editor) return true;
		const auto root = editorContext->projectManager->GetProjectRoot(); const auto generation = BuildAutomationStateSnapshot().projectGeneration;
		const auto status = rendererEfficiencyValidation.Status(); const auto operation = status.at("operationId").get<std::string>(); std::string error;
		if(!rendererEfficiencyValidation.ValidateOwnedAdmission(root, generation, operation, error)) return true;
		const auto pending = rendererEfficiencyValidation.PendingNativeStep();
		if(!rendererEfficiencyDeviceRequest)
		{
			if(pending.is_null() || pending.at("action") != "device-recovery") return true;
			const auto options = renderer.GetRendererEfficiencyOptionsSnapshot(); const auto step = pending.at("id").get<std::string>();
			const auto captured = renderer.GetRendererEfficiencyFixtureControlSnapshot();
			if(captured.correlation.operationId != operation || captured.correlation.fixtureStepId != step || !captured.controlsApplied)
			{
				Renderer::RendererEfficiencyFixtureIntent intent; intent.cameras = rendererEfficiencyValidation.FixtureCameras(); intent.correlation = {operation, step, 0, status.at("mutationGeneration").get<std::uint64_t>(), options.settingsGeneration, options.rendererSessionGeneration};
				const auto receipt = renderer.QueueRendererEfficiencyFixtureIntent(intent);
				if(!receipt.accepted) rendererEfficiencyValidation.ReportNativeUnavailable(receipt.reason);
				return true;
			}
			RendererEfficiencyDeviceRecreationRequest request{operation, step, status.at("fixtureId").get<std::string>(), status.at("fixtureHash").get<std::string>(), root,
				generation, options.rendererSessionGeneration, renderer.GetContext() ? renderer.GetContext()->GetBackendResourceGeneration() : 0};
			if(!editor->QueueRendererEfficiencyDeviceRecreation(request, error)) { rendererEfficiencyValidation.ReportNativeUnavailable(error); return true; }
			rendererEfficiencyDeviceRequest = request; rendererEfficiencyPreDeviceOptions = options; rendererEfficiencyDeviceSettingsGeneration = 0; return true;
		}
		const auto& request = *rendererEfficiencyDeviceRequest; const auto receipt = editor->GetRendererEfficiencyDeviceRecreationReceipt();
		if(request.operationId != operation || receipt.request.operationId != request.operationId || receipt.request.fixtureStepId != request.fixtureStepId
			|| receipt.request.projectRoot != root || receipt.request.projectGeneration != generation
			|| receipt.request.expectedRendererSessionGeneration != request.expectedRendererSessionGeneration
			|| receipt.request.expectedBackendResourceGeneration != request.expectedBackendResourceGeneration) return true;
		if(receipt.status == RendererEfficiencyDeviceRecreationStatus::Pending) return true;
		if(!HasCompletedRendererEfficiencyDeviceRecreation(receipt)) { rendererEfficiencyValidation.ReportNativeUnavailable(receipt.reason); return true; }
		const auto actual = renderer.GetLatestRendererRecoverySnapshot();
		if(actual.renderSessionGeneration != receipt.newRendererSessionGeneration || !renderer.GetContext()
			|| renderer.GetContext()->GetBackendResourceGeneration() != receipt.newBackendResourceGeneration) return true;
		// Only this exact completion receipt permits the expected epoch transition.
		// An arbitrary renderer replacement still follows the normal failure path.
		if(!rendererEfficiencyDeviceSettingsGeneration)
		{
			// Backend recreation completes before RenderSystem observes its new
			// lifetime. That capture invalidates old pending options; wait until
			// it has rebound the actual new session before enqueueing our controls.
			const auto rebound = renderer.GetRendererEfficiencyOptionsSnapshot();
			if(rebound.rendererSessionGeneration != receipt.newRendererSessionGeneration
				|| rebound.optionsSessionGeneration <= rendererEfficiencyPreDeviceOptions.optionsSessionGeneration) return false;
			const auto optionsReceipt = renderer.QueueRendererEfficiencyOptions(rendererEfficiencyPreDeviceOptions.options);
			if(!optionsReceipt.accepted) { rendererEfficiencyValidation.ReportNativeUnavailable(optionsReceipt.error); return false; }
			rendererEfficiencyDeviceSettingsGeneration = optionsReceipt.requestedGeneration; return false;
		}
		const auto fresh = renderer.GetRendererEfficiencyOptionsSnapshot();
		if(fresh.settingsGeneration != rendererEfficiencyDeviceSettingsGeneration || fresh.rendererSessionGeneration != receipt.newRendererSessionGeneration) return false;
		const auto& cameras = rendererEfficiencyValidation.FixtureCameras();
		const auto selectedView = cameras.empty() ? RenderViewType::Scene : cameras.front().viewType;
		const auto copied = renderer.GetLatestRendererEfficiencyEvidence(selectedView);
		if(copied.effectiveOptions.optionsSessionGeneration != fresh.optionsSessionGeneration
			|| copied.effectiveOptions.rendererSessionGeneration != receipt.newRendererSessionGeneration
			|| copied.effectiveOptions.settingsGeneration != rendererEfficiencyDeviceSettingsGeneration) return false;
		const nlohmann::json nativeDeviceReceipt = {{"kind", "DeviceRecreation"}, {"fixtureStepId", request.fixtureStepId}, {"status", "Completed"},
			{"idleProven", receipt.idleProven}, {"rebound", receipt.rebound}, {"oldRendererSessionGeneration", receipt.oldRendererSessionGeneration}, {"newRendererSessionGeneration", receipt.newRendererSessionGeneration},
			{"oldBackendResourceGeneration", receipt.oldBackendResourceGeneration}, {"newBackendResourceGeneration", receipt.newBackendResourceGeneration},
			{"priorOptionsEpoch", rendererEfficiencyPreDeviceOptions.optionsSessionGeneration}, {"newOptionsEpoch", fresh.optionsSessionGeneration}, {"visibleSettingsGeneration", fresh.settingsGeneration},
			{"request", {{"operationId", request.operationId}, {"fixtureStepId", request.fixtureStepId}, {"fixtureId", request.fixtureId}, {"fixtureHash", request.fixtureHash}, {"projectGeneration", request.projectGeneration},
				{"expectedRendererSessionGeneration", request.expectedRendererSessionGeneration}, {"expectedBackendResourceGeneration", request.expectedBackendResourceGeneration}}}, {"reason", receipt.reason}};
		if(!rendererEfficiencyValidation.RecordFixtureReceipt(root, generation, operation, nativeDeviceReceipt, error)) { rendererEfficiencyValidation.ReportNativeUnavailable(error); return true; }
		if(rendererEfficiencyValidation.Status().value("stage",std::string{})=="P5") {
			// The existing native recovery path already queued and observed these
			// exact settings. Record the new epoch instead of reusing an old receipt.
			const nlohmann::json budgetReceipt={{"kind","StaticBudgetPublication"},{"fixtureStepId",request.fixtureStepId},{"operationId",operation},
				{"requestedGeneration",rendererEfficiencyDeviceSettingsGeneration},{"priorOptions",SerializeRendererEfficiencyOptions(rendererEfficiencyPreDeviceOptions.options)},
				{"requestedOptions",SerializeRendererEfficiencyOptions(fresh.options)},{"effectiveOptions",SerializeRendererEfficiencyOptions(fresh.options)},
				{"settingsGeneration",fresh.settingsGeneration},{"rendererSessionGeneration",fresh.rendererSessionGeneration},{"optionsSessionGeneration",fresh.optionsSessionGeneration}};
			if(!rendererEfficiencyValidation.RecordFixtureReceipt(root,generation,operation,budgetReceipt,error)){rendererEfficiencyValidation.ReportNativeUnavailable(error);return true;}
		}
		if(!rendererEfficiencyValidation.AdmitCompletedDeviceRecreation(root, generation, operation, request.fixtureStepId,
			rendererEfficiencyPreDeviceOptions.optionsSessionGeneration, fresh, receipt.newBackendResourceGeneration, error))
		{ rendererEfficiencyValidation.ReportNativeUnavailable(error); return true; }
		rendererEfficiencyDeviceRequest.reset(); rendererEfficiencyDeviceSettingsGeneration = 0; return true;
#endif
	}
	struct EditorAutomationService::RendererEfficiencyShaderWork
	{
		std::string operationId, stepId, phase;
		TerraEngine::UUID shaderId{};
		std::uint64_t priorObservedGeneration = 0;
		std::filesystem::path copiedSourcePath;
		std::string copiedSourceHash;
		std::vector<std::pair<TerraEngine::UUID, std::vector<ShaderStagePath>>> collections;
		std::map<TerraEngine::UUID, std::uint64_t> priorCollectionGenerations;
		std::future<std::tuple<std::filesystem::path, std::string, std::string>> copyJob;
		RendererEfficiencyShaderPublicationReceipt receipt;
		std::vector<std::pair<TerraEngine::UUID, std::shared_future<TerraEngine::UUID>>> rebuilds;
		std::future<std::pair<bool, std::string>> restoreJob;
		bool dirty = false;
	};
	void EditorAutomationService::ScheduleRendererEfficiencyShaderRestoreAtProtectedFrame()
	{
		if(!rendererEfficiencyShaderWork || rendererEfficiencyShaderWork->restoreJob.valid()
			|| !rendererEfficiencyShaderWork->receipt.completion.valid()) return;
		const auto receipt = rendererEfficiencyShaderWork->receipt; const auto collections = rendererEfficiencyShaderWork->collections;
		auto task = ThreadPool::instance().createTask([receipt, collections]() {
			try
			{
				// The dependent wait runs on a worker, never the editor/render boundary.
				// Native restore compares our exact request owner and cache epoch, so a
				// later unrelated shader publication cannot be overwritten by cleanup.
				if(receipt.completion.get() == TerraEngine::EMPTY_UUID) return std::make_pair(false, std::string("Owned shader publication failed before restoration."));
				auto& resources = ResourceManager::Instance();
				if(!resources.RestoreRendererEfficiencyShaderOverride(receipt)) return std::make_pair(false, std::string("Owned shader restoration was superseded or its resource epoch retired."));
				for(const auto& [id, paths] : collections)
					if(resources.ReloadShaderCollectionAsync(paths).get() != id) return std::make_pair(false, std::string("Default shader collection restoration did not publish its expected identity."));
				return std::make_pair(true, std::string{});
			}
			catch(const std::exception& exception) { return std::make_pair(false, std::string(exception.what())); }
		});
		rendererEfficiencyShaderWork->restoreJob = task->getFuture(); ThreadPool::instance().enqueue(task);
	}
	void EditorAutomationService::PumpRendererEfficiencyShaderMutationAtProtectedFrame()
	{
		if(!editorContext || !editorContext->projectManager || !rendererEfficiencyValidation.Active()) return;
		const auto root = editorContext->projectManager->GetProjectRoot(); const auto generation = BuildAutomationStateSnapshot().projectGeneration;
		const auto status = rendererEfficiencyValidation.Status(); const auto operation = status.at("operationId").get<std::string>(); std::string error;
		if(!rendererEfficiencyValidation.ValidateOwnedAdmission(root, generation, operation, error)) { rendererEfficiencyValidation.ReportNativeUnavailable(error); return; }
		try
		{
			if(rendererEfficiencyShaderWork && rendererEfficiencyShaderWork->dirty && status.at("state") == "PreparingBaseline")
			{
				if(!rendererEfficiencyValidation.BeginResourceBaselineReset(root, generation, error)) throw std::runtime_error(error);
				ScheduleRendererEfficiencyShaderRestoreAtProtectedFrame(); return;
			}
			if(rendererEfficiencyShaderWork && rendererEfficiencyShaderWork->restoreJob.valid())
			{
				if(rendererEfficiencyShaderWork->restoreJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
				auto [restored, reason] = rendererEfficiencyShaderWork->restoreJob.get();
				if(!restored) throw std::runtime_error(reason);
				rendererEfficiencyShaderWork.reset();
				if(!rendererEfficiencyValidation.AdmitResourceBaselinePublication(root, generation, ResourceManager::Instance().GetRendererResourcePublicationGeneration(), error)) throw std::runtime_error(error);
				return;
			}
			const auto step = rendererEfficiencyValidation.PendingNativeStep();
			if(step.is_null() || step.at("action") != "shader-revision") return;
			auto& resources = ResourceManager::Instance();
			if(!rendererEfficiencyShaderWork)
			{
				auto work = std::make_shared<RendererEfficiencyShaderWork>(); work->operationId = operation; work->stepId = step.at("id").get<std::string>();
				const auto engineRoot = resources.GetEngineRoot();
				const auto mesh = engineRoot / "Shaders/Default.mesh.tspf", fragment = engineRoot / "Shaders/Default.frag.tspf", taskPath = engineRoot / "Shaders/Default.task.tspf";
				for(const auto& id : {resources.GetDefaultShaderCollectionID(), resources.GetDefaultTaskShaderCollectionID()})
				{
					const auto collection = resources.AcquireShaderCollectionSnapshot(id); if(!collection) continue;
					std::vector<ShaderStagePath> paths;
					if(std::any_of(collection->stages.begin(), collection->stages.end(), [](const auto& binding) { return binding.stage == ShaderStage::Task; })) paths.push_back({ShaderStage::Task, taskPath});
					paths.push_back({ShaderStage::Mesh, mesh}); paths.push_back({ShaderStage::Fragment, fragment});
					for(const auto& binding : collection->stages) if(binding.stage == ShaderStage::Mesh)
					{
						if(!binding.shader || !binding.publicationGeneration || (work->shaderId != TerraEngine::EMPTY_UUID && work->shaderId != binding.shaderId)) throw std::runtime_error("Actual default Mesh stage publication is unavailable or inconsistent.");
						work->shaderId = binding.shaderId;
						if(work->priorObservedGeneration && work->priorObservedGeneration != binding.publicationGeneration) throw std::runtime_error("Default shader collections disagree on their accepted Mesh publication.");
						work->priorObservedGeneration = binding.publicationGeneration;
					}
					if(!collection->collectionGeneration) throw std::runtime_error("Default shader collection has no accepted publication generation.");
					work->priorCollectionGenerations[id] = collection->collectionGeneration;
					work->collections.emplace_back(id, std::move(paths));
				}
				if(work->shaderId == TerraEngine::EMPTY_UUID || work->collections.empty()) throw std::runtime_error("No actual default Mesh shader collection is published.");
				auto task = ThreadPool::instance().createTask([root, operation, mesh]() {
					try
					{
						RendererEfficiencyInstallIdentity identity; std::string reason;
						if(!PrepareRendererEfficiencyInstallIdentity(identity, reason)) throw std::runtime_error(reason);
						const auto source = std::filesystem::canonical(mesh), install = std::filesystem::canonical(identity.installRoot);
						const auto relative = source.lexically_relative(install); if(relative.empty() || *relative.begin() == "..") throw std::runtime_error("Default shader is outside the actual verified install closure.");
						const auto folder = root / "renderer-efficiency-shaders" / operation;
						std::filesystem::path checked;
						for(const auto& part : folder)
						{
							checked /= part;
							if(std::filesystem::is_symlink(checked)) throw std::runtime_error("Owned shader copy folder redirects outside its project.");
#if defined(_WIN32)
							const auto attributes = GetFileAttributesW(checked.c_str()); if(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Owned shader copy folder traverses a junction.");
#endif
						}
						std::filesystem::create_directories(folder); const auto destination = folder / "Default.mesh.tspf";
						if(std::filesystem::is_symlink(destination)) throw std::runtime_error("Owned shader destination is a symbolic link.");
#if defined(_WIN32)
						const auto destinationAttributes = GetFileAttributesW(destination.c_str());
						if(destinationAttributes != INVALID_FILE_ATTRIBUTES && (destinationAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Owned shader destination is a reparse point.");
#endif
						std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
						std::string hash;
						if(!HashRendererEfficiencyOwnedInput(root, destination.lexically_relative(root), hash, reason)) throw std::runtime_error(reason);
						return std::tuple{destination, std::move(hash), std::string{}};
					}
					catch(const std::exception& exception) { return std::tuple{std::filesystem::path{}, std::string{}, std::string(exception.what())}; }
				});
				work->copyJob = task->getFuture(); work->phase = "Copying"; ThreadPool::instance().enqueue(task); rendererEfficiencyShaderWork = std::move(work); return;
			}
			auto& work = *rendererEfficiencyShaderWork;
			if(work.operationId != operation || work.stepId != step.at("id").get<std::string>()) throw std::runtime_error("Owned shader operation changed while publication was pending.");
			if(work.phase == "Copying")
			{
				if(work.copyJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
				auto [path, hash, reason] = work.copyJob.get(); if(!reason.empty()) throw std::runtime_error(reason);
				work.copiedSourcePath = path.lexically_relative(root); work.copiedSourceHash = std::move(hash);
				work.receipt = resources.BeginRendererEfficiencyShaderOverride(work.shaderId, path);
				if(!work.receipt.completion.valid()) throw std::runtime_error("Native scoped shader publication was not admitted.");
				work.phase = "AwaitingShader";
			}
			if(work.phase == "AwaitingShader")
			{
				if(work.receipt.completion.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
				if(work.receipt.completion.get() != work.shaderId || !resources.GetRendererEfficiencyShaderOverrideOwner(work.receipt)) throw std::runtime_error("Native scoped shader request did not publish its exact owner.");
				work.dirty = true;
				for(const auto& [id, paths] : work.collections) work.rebuilds.emplace_back(id, resources.ReloadShaderCollectionAsync(paths));
				work.phase = "AwaitingCollections";
			}
			if(work.phase == "AwaitingCollections")
			{
				const auto owner = resources.GetRendererEfficiencyShaderOverrideOwner(work.receipt);
				nlohmann::json acceptedCollections = nlohmann::json::array();
				for(const auto& [id, future] : work.rebuilds)
				{
					if(future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
					if(future.get() != id) throw std::runtime_error("Actual default shader collection reassembly failed.");
					const auto collection = resources.AcquireShaderCollectionSnapshot(id);
					if(!collection || !owner || !std::any_of(collection->stages.begin(), collection->stages.end(), [&](const auto& binding) { return binding.stage == ShaderStage::Mesh && binding.shader == owner; })) throw std::runtime_error("Default collection does not retain the exact owned shader publication.");
					nlohmann::json stages = nlohmann::json::array();
					for(const auto& binding : collection->stages)
					{
						if(!binding.shader || !binding.publicationGeneration || binding.shader->publicationGeneration != binding.publicationGeneration) throw std::runtime_error("Dependent shader stage lacks an exact accepted immutable publication.");
						stages.push_back({{"stage", static_cast<std::uint32_t>(binding.stage)}, {"shaderId", TerraEngine::ToHexString(binding.shaderId)}, {"publicationGeneration", binding.publicationGeneration}, {"acceptedOverrideOwner", binding.shader == owner}});
					}
					if(collection->collectionGeneration <= work.priorCollectionGenerations.at(id)) throw std::runtime_error("Dependent shader collection publication did not advance.");
					acceptedCollections.push_back({{"collectionId", TerraEngine::ToHexString(id)}, {"collectionGeneration", collection->collectionGeneration}, {"previousCollectionGeneration", work.priorCollectionGenerations.at(id)}, {"stages", std::move(stages)}});
				}
				if(!owner || owner->publicationGeneration <= work.priorObservedGeneration) throw std::runtime_error("Scoped shader publication did not advance from the actual observed baseline owner.");
				for(const auto& collection : acceptedCollections)
				{
					// Caster stamps name a shader collection, while the lease owns one
					// stage. Preserve both real identities instead of conflating epochs.
					const nlohmann::json publication{{"kind", "ShaderPublication"}, {"fixtureStepId", work.stepId}, {"action", "shader-revision"},
						{"shaderId", collection.at("collectionId")}, {"shaderStageId", TerraEngine::ToHexString(work.shaderId)},
						{"sourcePath", work.copiedSourcePath.generic_string()}, {"sourceSha256", work.copiedSourceHash},
						{"shaderCollectionGeneration", collection.at("collectionGeneration")}, {"previousShaderCollectionGeneration", collection.at("previousCollectionGeneration")},
						{"shaderStagePublicationGeneration", owner->publicationGeneration}, {"previousObservedShaderStagePublicationGeneration", work.priorObservedGeneration},
						{"actualNativeOwnerAccepted", true}, {"stages", collection.at("stages")}};
					if(!rendererEfficiencyValidation.RecordFixtureReceipt(root, generation, operation, publication, error)) throw std::runtime_error(error);
				}
				if(!rendererEfficiencyValidation.AdmitNativeStepPublication(root, generation, operation, work.stepId, owner->publicationGeneration, error)) throw std::runtime_error(error);
				work.phase = "Published";
			}
		}
		catch(const std::exception& exception)
		{
			rendererEfficiencyValidation.ReportNativeUnavailable(exception.what());
			if(rendererEfficiencyShaderWork) rendererEfficiencyShaderWork->phase = "Failed";
		}
	}
	bool EditorAutomationService::ValidateRendererEfficiencyDeviceRecreationOwner(const std::filesystem::path& root,
		std::uint64_t generation, std::string_view operation, std::string_view step, std::string& error) const
	{
		std::lock_guard lock(rendererEfficiencyMutex);
		if(!editorContext || !editorContext->projectManager || BuildAutomationStateSnapshot().projectGeneration != generation
			|| editorContext->projectManager->GetProjectRoot().lexically_normal() != root.lexically_normal())
		{ error = "Device recreation project ownership changed before execution."; return false; }
		if(!rendererEfficiencyValidation.ValidateOwnedAdmission(root, generation, operation, error)) return false;
		const auto pending = rendererEfficiencyValidation.PendingNativeStep();
		if(pending.is_null() || pending.at("id").get<std::string>() != step || pending.at("action") != "device-recovery")
		{ error = "Device recreation does not match the pending owned recovery step."; return false; }
		error.clear(); return true;
	}
	void EditorAutomationService::PumpRendererValidationMutationsAtProtectedFrame()
	{
		{
			// Observe immutable renderer receipts at the existing protected editor
			// boundary. Polling MCP status never becomes a frame producer.
			std::lock_guard efficiencyLock(rendererEfficiencyMutex);
			if(rendererEfficiencyValidation.Active() && editorContext && editorContext->engine)
			{
				if(auto* renderer = editorContext->engine->GetSystem<RenderSystem>())
				{
					rendererEfficiencyValidation.PumpFixtureReceiptsAtProtectedFrame(
						editorContext->projectManager ? editorContext->projectManager->GetProjectRoot() : std::filesystem::path{},
						BuildAutomationStateSnapshot().projectGeneration, ObjectManager::instance(), TerraEngine::Scene::SceneMutationQueue::Instance());
					if(PumpRendererEfficiencyDeviceRecreationAtProtectedFrame(*renderer))
					rendererEfficiencyValidation.ObserveAtProtectedFrame(editorContext->projectManager ? editorContext->projectManager->GetProjectRoot() : std::filesystem::path{},
						BuildAutomationStateSnapshot().projectGeneration, renderer->GetLatestRendererEfficiencyEvidence(RenderViewType::Scene));
					if(rendererEfficiencyValidation.Active() && rendererEfficiencyValidation.Status().at("state") == "AwaitingAssets")
					{
						const auto operation = rendererEfficiencyValidation.Status().at("operationId").get<std::string>();
						const auto root = editorContext->projectManager->GetProjectRoot();
						if(rendererEfficiencyBootstrapOperation != operation)
						{
							rendererEfficiencyBootstrapOperation = operation; rendererEfficiencyDeviceRequest.reset(); rendererEfficiencyDeviceSettingsGeneration = 0;
							// Serialize published scheduler values once per owned run. Do not
							// retain its work callbacks or invent declaration/parallelism claims.
							const auto& schedule = editorContext->engine->GetSchedulerSummary();
							const auto& plan = editorContext->engine->GetParallelFrameExecutionPlan();
							nlohmann::json workloads = nlohmann::json::array();
							for(const auto& workload : plan.workloads) workloads.push_back({{"name", workload.workloadName}, {"ownerSystem", workload.ownerSystem}, {"phase", workload.phase},
								{"reads", workload.reads}, {"writes", workload.writes}, {"dependencies", workload.explicitDependencies}, {"workerEligible", workload.workerEligible},
								{"mainThreadRequired", workload.mainThreadRequired}, {"phaseKind", TerraEngine::Scheduling::ToString(workload.phaseKind)}, {"boundaryClassification", TerraEngine::Scheduling::ToString(workload.boundaryClassification)},
								{"mutationPolicy", TerraEngine::Scheduling::ToString(workload.mutationPolicy)}, {"protectedCommit", workload.protectedCommit}, {"protectedReason", workload.protectedReason}, {"safeSubwork", workload.safeSubwork}});
							const nlohmann::json schedulerReceipt = {{"kind", "Scheduler"}, {"frameId", plan.frameId}, {"valid", plan.valid}, {"completed", plan.completed},
								{"executionMode", TerraEngine::Scheduling::ToString(plan.executionMode)}, {"parallelExecutionUsed", plan.parallelExecutionUsed},
								{"preparationOption", SerializeRendererEfficiencyOptions(renderer->GetRendererEfficiencyOptionsSnapshot().options).at("validationPreparation")},
								{"systems", schedule.systems}, {"phases", schedule.phases}, {"flushPoints", schedule.flushPoints}, {"mainThreadSystems", schedule.mainThreadSystems},
								{"workerCategories", schedule.workerCoverageReport.workerCategories}, {"protectedCategories", schedule.workerCoverageReport.protectedCategories}, {"workloads", std::move(workloads)}};
							std::string schedulerError;
							if(!rendererEfficiencyValidation.RecordFixtureReceipt(root, BuildAutomationStateSnapshot().projectGeneration, operation, schedulerReceipt, schedulerError)) rendererEfficiencyValidation.ReportBootstrapUnavailable(schedulerError);
							rendererEfficiencyBindingJob = {}; rendererEfficiencyBindingPreparation.reset(); rendererEfficiencyBindingLoads.clear();
							rendererEfficiencySourceBaselines.clear(); rendererEfficiencyDenseSourceBaselines.clear(); rendererEfficiencyDirtySources.clear(); rendererEfficiencyResourceWork = {};
							rendererEfficiencyBootstrapRetry = {};
						}
						if(!rendererEfficiencyBindingPreparation && !rendererEfficiencyBindingJob.valid()
							&& std::chrono::steady_clock::now() >= rendererEfficiencyBootstrapRetry)
						{
							const auto steps = rendererEfficiencyValidation.InstalledSteps();
							auto task = ThreadPool::instance().createTask([root, steps]() {
								RendererEfficiencyFixtureBindingPreparation prepared; std::string reason;
								(void)PrepareRendererEfficiencyFixtureBindings(root, steps, prepared, reason);
								return std::pair{std::move(prepared), std::move(reason)};
							});
							rendererEfficiencyBindingJob = task->getFuture(); ThreadPool::instance().enqueue(task);
						}
						if(rendererEfficiencyBindingJob.valid() && rendererEfficiencyBindingJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
						{
							auto [prepared, reason] = rendererEfficiencyBindingJob.get();
							if(!reason.empty() || prepared.candidates.empty())
							{
								rendererEfficiencyValidation.ReportBootstrapUnavailable(reason.empty() ? "No native package bindings have been prepared." : reason);
								rendererEfficiencyBootstrapRetry = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
							}
							else rendererEfficiencyBindingPreparation = std::move(prepared);
						}
						if(rendererEfficiencyBindingPreparation)
						{
							// Resource loads are native asynchronous requests. Their futures do
							// not prove publication; the compact resolver checks actual caches.
							if(rendererEfficiencyBindingLoads.empty())
								for(const auto& candidate : rendererEfficiencyBindingPreparation->candidates)
								{
									const auto& binding = candidate.binding; auto& resources = ResourceManager::Instance();
									if(binding.kind == "Mesh" || binding.kind == "DeformableMesh")
									{
										const auto current = resources.TryCopyRendererEfficiencyResourcePublication(binding.assetId, AssetType::Mesh);
										rendererEfficiencyBindingLoads.push_back(current && current->present ? resources.ReloadMeshAsync(binding.assetId) : resources.LoadMeshAsync(binding.cookedPath));
									}
									else if(binding.kind == "Material")
									{
										const auto current = resources.TryCopyRendererEfficiencyResourcePublication(binding.assetId, AssetType::Material);
										rendererEfficiencyBindingLoads.push_back(current && current->present ? resources.ReloadMaterialAsync(binding.assetId) : resources.LoadMaterialAsync(binding.cookedPath));
									}
									else if(binding.kind == "Texture")
									{
										const auto current = resources.TryCopyRendererEfficiencyResourcePublication(binding.assetId, AssetType::Texture);
										rendererEfficiencyBindingLoads.push_back(current && current->present ? resources.ReloadTexturePublicationAsync(binding.assetId) : resources.LoadTextureAsync(binding.cookedPath));
									}
									else if(binding.kind == "Skeleton") rendererEfficiencyBindingLoads.push_back(resources.LoadAnimationAssetAsync(binding.cookedPath, AssetType::Skeleton));
									else if(binding.kind == "AnimationClip") rendererEfficiencyBindingLoads.push_back(resources.LoadAnimationAssetAsync(binding.cookedPath, AssetType::AnimationClip));
									else rendererEfficiencyValidation.ReportBootstrapUnavailable("Unsupported published binding resource kind: " + binding.kind);
								}
							std::vector<RendererEfficiencyPublishedBinding> bindings; std::string reason;
							// A cached older payload cannot satisfy a still-pending reload.
							bool loadsPublished = rendererEfficiencyBindingLoads.size() == rendererEfficiencyBindingPreparation->candidates.size();
							for(std::size_t index = 0; loadsPublished && index < rendererEfficiencyBindingLoads.size(); ++index)
							{
								const auto& future = rendererEfficiencyBindingLoads[index];
								loadsPublished = future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
								if(loadsPublished)
								{
									try { loadsPublished = future.get() == rendererEfficiencyBindingPreparation->candidates[index].binding.assetId; }
									catch(const std::exception& exception) { loadsPublished = false; reason = exception.what(); }
								}
							}
							if(!loadsPublished) rendererEfficiencyValidation.ReportBootstrapUnavailable(reason.empty() ? "Awaiting actual native asset load/reload completion." : reason);
							else if(ResolveRendererEfficiencyFixtureBindings(*rendererEfficiencyBindingPreparation, ResourceManager::Instance(), bindings, reason))
							{
								if(!rendererEfficiencyValidation.AdmitPublishedFixtureBindings(root, BuildAutomationStateSnapshot().projectGeneration,
									operation, bindings, ObjectManager::instance(), TerraEngine::Scene::SceneMutationQueue::Instance(), reason))
									rendererEfficiencyValidation.ReportBootstrapUnavailable(reason);
							}
							else rendererEfficiencyValidation.ReportBootstrapUnavailable(reason);
						}
					}
					PumpRendererEfficiencyShaderMutationAtProtectedFrame();
					PumpRendererEfficiencyResourceMutationsAtProtectedFrame(*renderer);
					rendererEfficiencyValidation.PumpFixtureAtProtectedFrame(
						editorContext->projectManager ? editorContext->projectManager->GetProjectRoot() : std::filesystem::path{},
						BuildAutomationStateSnapshot().projectGeneration, ObjectManager::instance(), TerraEngine::Scene::SceneMutationQueue::Instance(), *renderer);
					if(!rendererEfficiencyValidation.Active())
					{
						rendererEfficiencyValidation.CancelPendingFixtureWorkAtProtectedFrame(TerraEngine::Scene::SceneMutationQueue::Instance());
						renderer->ClearRendererEfficiencyFixtureIntent(rendererEfficiencyValidation.Status().value("operationId", std::string{}));
						ScheduleRendererEfficiencyShaderRestoreAtProtectedFrame();
			std::string sourceRestoreError; (void)RestoreRendererEfficiencySourcesAtProtectedFrame(sourceRestoreError);
					}
					if(!rendererEfficiencyValidation.Active() && rendererEfficiencyValidation.PriorToken().valid)
						(void)renderer->QueueRendererEfficiencyRestore(rendererEfficiencyValidation.PriorToken());
				}
			}
		}
		std::lock_guard lock(sparseTransformMutationMutex);
		SparseTransformMutationState& state = sparseTransformMutation;
		// A stop request disables new batches immediately, but the last batch may
		// still be waiting for the declared protected scene-mutation flush. Keep
		// pumping that one batch until its exact values are visible so benchmark
		// evidence never mistakes an MCP/flush scheduling race for a failed commit.
		if(state.failed || state.targets.empty()
			|| (!state.active && state.pendingExpected.empty()))
		{
			return;
		}

		ObjectManager& objects = ObjectManager::instance();
		if(!state.pendingExpected.empty())
		{
			// The prior frame's queue flush is proved by reading the exact values that
			// were enqueued. A missing or mismatched target fails closed instead of
			// reporting an uncommitted frame as benchmark evidence.
			for(const TerraEngine::Scene::TransactionTransformUpdate& expected : state.pendingExpected)
			{
				const std::optional<Object> object = objects.getObject(expected.objectId);
				const Transform* transform = object ? objects.GetComponent<Transform>(*object) : nullptr;
				if(transform == nullptr || transform->worldPose.position != expected.pose.position
					|| transform->worldPose.rotation != expected.pose.rotation)
				{
					state.failed = true;
					state.active = false;
					state.failureReason = "A protected sparse transform transaction did not become visible before the next editor frame.";
					return;
				}
			}
			++state.committedFrameCount;
			state.committedMutationCount += state.pendingExpected.size();
			state.pendingExpected.clear();
		}
		if(!state.active)
		{
			// Stop requested: the final batch is now proven, so do not enqueue a
			// replacement batch for the next frame.
			return;
		}

		std::vector<TerraEngine::Scene::TransactionTransformUpdate> updates;
		updates.reserve(state.batchSize);
		const bool displaced = ((state.enqueuedFrameCount / std::max<std::size_t>(1U,
			state.targets.size() / state.batchSize)) & 1U) == 0U;
		for(std::size_t index = 0U; index < state.batchSize; ++index)
		{
			const SparseTransformTarget& target = state.targets[(state.cursor + index) % state.targets.size()];
			TerraEngine::Scene::WorldPose pose = target.baselinePose;
			if(displaced)
			{
				const TerraEngine::Scene::WorldCoordinateResult moved = TerraEngine::Scene::ApplyWorldDelta(
					pose.position, {{}, {static_cast<double>(state.displacement), 0.0, 0.0}});
				if (!moved.Succeeded())
				{
					state.failed = true;
					state.active = false;
					state.failureReason = "A sparse validation pose exceeded the canonical world range.";
					return;
				}
				pose.position = moved.position;
			}
			updates.push_back({target.objectId, pose});
		}
		state.cursor = (state.cursor + state.batchSize) % state.targets.size();

		TerraEngine::Scene::TransactionalMutationBuilder builder(
			state.operationId + "-frame-" + std::to_string(state.enqueuedFrameCount + 1U),
			TerraEngine::Scene::TransactionOrigin::Automation,
			"RendererSparseTransformValidation");
		builder.SetBudget({std::max<std::size_t>(state.batchSize, 1U), 1U});
		builder.UpdateTransformBatch(objects, updates);
		TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(std::move(builder).Build());
		state.pendingExpected = std::move(updates);
		++state.enqueuedFrameCount;
		state.enqueuedMutationCount += state.pendingExpected.size();
	}

	void EditorAutomationService::RefreshAnimationImportJob(std::string_view jobId)
	{
		const auto active = activeAnimationImportJobs.find(std::string(jobId));
		if (active == activeAnimationImportJobs.end())
		{
			return;
		}
		const ActiveAnimationImportJob import = active->second;
		const std::optional<AutomationJobSnapshot> retained = jobService.Get(jobId);
		if (!retained || retained->state == AutomationJobState::Succeeded
			|| retained->state == AutomationJobState::Failed
			|| retained->state == AutomationJobState::Canceled)
		{
			activeAnimationImportJobs.erase(active);
			return;
		}
		if (std::chrono::system_clock::now() >= import.deadline)
		{
			if (editorContext != nullptr && editorContext->assetDatabase != nullptr
				&& editorContext->assetDatabase->IsAssetCompiling(import.sourcePath))
			{
				(void)editorContext->assetDatabase->CancelImport(import.sourcePath, false);
			}
			(void)jobService.AddDiagnostic(jobId, {
				AutomationLogSeverity::Error,
				"timeout",
				"Animation import exceeded its declared timeout before publication.",
				{{"recommendedAction", "Inspect importer diagnostics, then retry with a new idempotency key or a larger bounded timeout."}}
			});
			(void)jobService.CompleteFailed(jobId, {
				{"status", "timeout"},
				{"message", "Animation import exceeded its declared timeout before publication."},
				{"recommendedAction", "Inspect importer diagnostics, then retry with a new idempotency key or a larger bounded timeout."}
			});
			activeAnimationImportJobs.erase(active);
			return;
		}
		if (editorContext == nullptr || editorContext->assetDatabase == nullptr
			|| BuildAutomationStateSnapshot().projectGeneration != import.projectGeneration)
		{
			if (editorContext != nullptr && editorContext->assetDatabase != nullptr
				&& editorContext->assetDatabase->IsAssetCompiling(import.sourcePath))
			{
				(void)editorContext->assetDatabase->CancelImport(import.sourcePath, false);
			}
			(void)jobService.CompleteFailed(jobId, {{"message", "Project generation changed before animation import publication."}});
			activeAnimationImportJobs.erase(active);
			return;
		}

		AssetDatabase& assets = *editorContext->assetDatabase;
		if (jobService.IsCancellationRequested(jobId))
		{
			if (assets.IsAssetCompiling(import.sourcePath))
			{
				(void)assets.CancelImport(import.sourcePath, false);
				return;
			}
			(void)jobService.CompleteCanceled(jobId, {{"cancellationBoundary", "before_animation_asset_publication"}});
			activeAnimationImportJobs.erase(active);
			return;
		}
		if (const std::optional<AssetImportProgress> progress = assets.GetImportProgress(import.sourcePath))
		{
			(void)jobService.SetState(jobId, AutomationJobState::Running, progress->stage.empty() ? "importing" : progress->stage);
			(void)jobService.PublishProgress(jobId, std::clamp(static_cast<double>(progress->progress), 0.0, 0.99), progress->stage);
			if (progress->failed)
			{
				(void)jobService.CompleteFailed(jobId, {{"message", progress->message.empty() ? "Animation import failed validation." : progress->message}});
				activeAnimationImportJobs.erase(active);
			}
			return;
		}
		if (assets.IsAssetCompiling(import.sourcePath))
		{
			(void)jobService.SetState(jobId, AutomationJobState::Running, "importing");
			return;
		}

		const std::filesystem::path normalized = std::filesystem::absolute(import.sourcePath).lexically_normal();
		if (assets.GetAssets().find(normalized) == assets.GetAssets().end())
		{
			(void)jobService.CompleteFailed(jobId, {{"message", "Animation import completed without publishing asset metadata."}});
			activeAnimationImportJobs.erase(active);
			return;
		}
		(void)jobService.PublishProgress(jobId, 1.0, "published");
		(void)jobService.CompleteSucceeded(jobId, {{"sourcePath", normalized.generic_string()}});
		activeAnimationImportJobs.erase(active);
	}

	void EditorAutomationService::SetMainEditor(MainEditorSubsystem* editor)
	{
		// The bridge owns no windows. It receives this non-owning editor pointer so
		// semantic automation always reaches the same Scene View the user sees.
		mainEditor = editor;
		if(mainEditor != nullptr)
			if(auto terrainTools = mainEditor->GetWindow<TerrainWindow>())
				SetTerrainAutomationAdapter(&terrainTools->AutomationAdapter());
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

	void EditorAutomationService::PumpTerrainAutomationJobsAtProtectedFrame()
	{
		using CollisionStatus = TerraEngine::Physics::Terrain::TerrainCollisionStatus;
		std::vector<std::pair<std::string, PendingTerrainCollisionJob>> pending;
		{
			std::scoped_lock lock(terrainAutomationMutex);
			for(const auto& entry : pendingTerrainCollisionJobs) pending.push_back(entry);
		}
		if(pending.empty()) return;

		const auto snapshot = TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance().Snapshot();
		for(const auto& [jobId, expected] : pending)
		{
			bool terminal = false;
			if(jobService.IsCancellationRequested(jobId))
			{
				if(terrainAutomationAdapter) (void)terrainAutomationAdapter->DispatchCommand("terrain.collision-cancel", nlohmann::json::object());
				(void)jobService.CompleteCanceled(jobId, {{"outcome", "Cancelled"},
					{"visibilityBoundary", "terrain_collision_publish_before_physics"}, {"lastKnownGoodActive", true}});
				terminal = true;
			}
			else if(snapshot.status == CollisionStatus::Current)
			{
				const bool current = TerraEngine::Terrain::ToHexString(snapshot.sourceRootHash) == expected.sourceRootHash
					&& snapshot.sourcePublicationGeneration == expected.sourcePublicationGeneration;
				const nlohmann::json result = {{"outcome", current ? "Committed" : "StaleGeneration"},
					{"sourceRootHash", TerraEngine::Terrain::ToHexString(snapshot.sourceRootHash)},
					{"sourcePublicationGeneration", snapshot.sourcePublicationGeneration},
					{"buildLatencyMilliseconds", snapshot.buildLatencyMilliseconds},
					{"visibilityBoundary", "terrain_collision_publish_before_physics"},
					{"lastKnownGoodActive", !current}};
				if(current) (void)jobService.CompleteSucceeded(jobId, result);
				else (void)jobService.CompleteFailed(jobId, result);
				terminal = true;
			}
			else if(snapshot.status == CollisionStatus::Stale || snapshot.status == CollisionStatus::Failed
				|| snapshot.status == CollisionStatus::Cancelled)
			{
				const nlohmann::json result = {{"outcome", snapshot.status == CollisionStatus::Cancelled ? "Cancelled" : "DerivedBuildFailed"},
					{"message", snapshot.message}, {"lastKnownGoodActive", true},
					{"visibilityBoundary", "terrain_collision_publish_before_physics"}};
				if(snapshot.status == CollisionStatus::Cancelled) (void)jobService.CompleteCanceled(jobId, result);
				else (void)jobService.CompleteFailed(jobId, result);
				terminal = true;
			}
			if(terminal)
			{
				std::scoped_lock lock(terrainAutomationMutex);
				pendingTerrainCollisionJobs.erase(jobId);
			}
		}
	}

	void EditorAutomationService::SetTerrainAutomationAdapter(TerraEngine::Editor::Terrain::TerrainAutomationAdapter* adapter)
	{
		terrainAutomationAdapter = adapter;
		if(adapter != nullptr)
			adapter->SetReceiptCallback([this](std::string_view jobId, const TerraEngine::Terrain::TerrainTransactionReceipt& receipt, const TerraEngine::Editor::Automation::TerrainAutomationRequest& request) {
				// Values here originate in the typed request/receipt, never guessed UI
				// timing. This lets reconnecting clients audit the exact visibility fact.
				nlohmann::json terminal={{"operationId",TerraEngine::ToHexString(receipt.transactionId)}, {"jobId",std::string(jobId)}, {"operation",request.operation}, {"previousRootHash",TerraEngine::Terrain::ToHexString(receipt.previousRootHash)}, {"publishedRootHash",TerraEngine::Terrain::ToHexString(receipt.publishedRootHash)}, {"publicationGeneration",receipt.publicationGeneration}, {"affectedBounds",{{"minX",request.bounds.minX},{"minZ",request.bounds.minZ},{"maxXExclusive",request.bounds.maxXExclusive},{"maxZExclusive",request.bounds.maxZExclusive}}}, {"actualBudget",{{"areas",receipt.affectedAreas.size()},{"maximumAreas",request.budget.maximumAreas},{"maximumSamples",request.budget.maximumSamples},{"maximumPreparedBytes",request.budget.maximumPreparedBytes}}}, {"visibilityBoundary","terrain_authoring_publish"}, {"derivedInvalidationAreas",receipt.invalidation ? receipt.invalidation->areas.size() : 0u}, {"warnings",nlohmann::json::array()}, {"repairActions",nlohmann::json::array()}, {"message",receipt.message}};
				if(receipt.outcome==TerraEngine::Terrain::TerrainEditOutcome::Committed) (void)jobService.CompleteSucceeded(jobId,std::move(terminal));
				else if(receipt.outcome==TerraEngine::Terrain::TerrainEditOutcome::Cancelled) (void)jobService.CompleteCanceled(jobId,std::move(terminal));
				else (void)jobService.CompleteFailed(jobId,std::move(terminal));
			});
		if(terrainAutomationProviderRegistered || adapter == nullptr) return;
		const AutomationHandlerResolver resolver = [this](std::string_view name) {
			const std::string operation(name);
			return AutomationHandler{[this, operation](const AutomationRequestContext& context) {
				// Public terrain routes follow the installed contract. The internal
				// route names keep the existing typed producers compact and are never
				// exposed as a second mutation API.
				std::string route = operation;
				if(operation == "terrain.asset.inspect") route = "terrain.inspect";
				else if(operation == "terrain.import_heightmap") route = "terrain.import";
				else if(operation == "terrain.stroke.apply") route = "terrain.sculpt";
				else if(operation == "terrain.layers.set") route = "terrain.layer";
				else if(operation == "terrain.holes.set") route = "terrain.hole";
				else if(operation == "terrain.operation.start") route = "terrain.procedural";
				if(terrainAutomationAdapter == nullptr)
					return AutomationDispatchResult{false, ErrorCategory::Unavailable, "Terrain automation requires the editor terrain command adapter.", {}};
				if(route == "terrain.job.get")
				{
					const auto jobId=context.arguments.value("jobId",std::string{}); const auto job=jobService.Get(jobId);
					return job ? AutomationDispatchResult{true,ErrorCategory::Internal,"Terrain job snapshot.",{{"jobId",job->jobId},{"state",static_cast<int>(job->state)},{"progress",job->progress},{"terminalReceipt",job->result}}}
						: AutomationDispatchResult{false,ErrorCategory::Unavailable,"Terrain job is unavailable or expired.",{}};
				}
				if(route == "terrain.job.cancel")
					return jobService.RequestCancellation(context.arguments.value("jobId",std::string{})) ? AutomationDispatchResult{true,ErrorCategory::Internal,"Terrain job cancellation requested.",{{"cancelRequested",true}}} : AutomationDispatchResult{false,ErrorCategory::Unavailable,"Terrain job cannot be cancelled.",{}};
				if(route == "terrain.export")
				{
					// Export already has a durable, cancellable, progress-reporting job
					// backed by ExportPipeline. Terrain reuses that authority instead of
					// creating a second packager or completing a placeholder job.
					auto response = HandleExportStart(context.arguments, context.requestId);
					const bool accepted = response.value("ok", false);
					const nlohmann::json responseData = accepted
						? response.value("data", nlohmann::json::object())
						: response;
					return AutomationDispatchResult{accepted,
						accepted ? ErrorCategory::Internal : ErrorCategory::Unavailable,
						response.value("message", accepted ? "Terrain export job accepted." : "Terrain export job rejected."),
						responseData};
				}
				if(route == "terrain.inspect")
				{
					const auto capture = terrainAutomationAdapter->Inspect();
					if(!capture) return AutomationDispatchResult{false,ErrorCategory::Unavailable,"Terrain inspection requires a current immutable root.",{}};
					// Generation-checked edit requests must repeat the exact immutable area
					// observations they inspected. Publish a bounded, deterministic copy of
					// those observations here; otherwise an MCP client would have to read the
					// terrain asset's private data directory to construct a valid request.
					constexpr std::size_t maximumInspectionAreas = 4096u;
					nlohmann::json areas = nlohmann::json::array();
					std::size_t totalAreaCount = 0u;
					for(const TerraEngine::Terrain::TerrainDirectoryShard& shard : capture->root->directoryShards)
					{
						for(const auto& [coordinate, descriptor] : shard.descriptors)
						{
							++totalAreaCount;
							if(areas.size() >= maximumInspectionAreas)
								continue;
							nlohmann::json area = {
								{"areaX", std::to_string(coordinate.x)},
								{"areaZ", std::to_string(coordinate.z)},
								{"editGeneration", std::to_string(descriptor.editGeneration)},
								{"authoredContentHash", TerraEngine::Terrain::ToHexString(descriptor.authoredContentHash)},
								{"minHeightMm", descriptor.minHeightMm},
								{"maxHeightMm", descriptor.maxHeightMm}
							};
							if(descriptor.channels.heightHash)
								area["heightHash"] = TerraEngine::Terrain::ToHexString(*descriptor.channels.heightHash);
							if(descriptor.channels.holeHash)
								area["holeHash"] = TerraEngine::Terrain::ToHexString(*descriptor.channels.holeHash);
							areas.push_back(std::move(area));
						}
					}
					return AutomationDispatchResult{true,ErrorCategory::Internal,"Terrain immutable root inspection.",{
						{"assetUuid",TerraEngine::ToHexString(capture->root->assetUuid)},
						{"rootHash",TerraEngine::Terrain::ToHexString(capture->root->rootContentHash)},
						{"publicationGeneration",capture->publicationGeneration},
						{"layerStackGeneration",std::to_string(capture->root->layerStackGeneration)},
						{"authoredBounds",{{"minX",std::to_string(capture->root->authoredBounds.minX)},
							{"minZ",std::to_string(capture->root->authoredBounds.minZ)},
							{"maxXExclusive",std::to_string(capture->root->authoredBounds.maxXExclusive)},
							{"maxZExclusive",std::to_string(capture->root->authoredBounds.maxZExclusive)}}},
						{"areas",std::move(areas)},
						{"totalAreaCount",totalAreaCount},
						{"areasTruncated",totalAreaCount > maximumInspectionAreas},
						{"visibilityBoundary","immutable_terrain_snapshot"}}};
				}
				if(route == "terrain.create" || route == "terrain.import" || route == "terrain.rebuild"
					|| route == "terrain.collision-refresh" || route == "terrain.validate"
					|| route == "terrain.save" || route == "terrain.diagnostics.get"
					|| route == "terrain.session.begin" || route == "terrain.session.get"
					|| route == "terrain.session.cancel" || route == "terrain.session.commit")
				{
					if(route == "terrain.rebuild")
					{
						std::scoped_lock lock(terrainAutomationMutex);
						if(!pendingTerrainCollisionJobs.empty())
							return AutomationDispatchResult{false, ErrorCategory::Unavailable,
								"A terrain collision rebuild is already awaiting its protected publication boundary.", nlohmann::json::object()};
					}
					const auto job=jobService.Create({"terrain_command",operation,context.requestId,context.correlationId,context.idempotencyKey,true});
					(void)jobService.SetState(job.jobId,AutomationJobState::Running,"terrain_editor_command");
					auto result = terrainAutomationAdapter->DispatchCommand(route, context.arguments);
					nlohmann::json terminal = result.result;
					terminal["jobId"] = job.jobId;
					terminal["operation"] = operation;
					terminal["message"] = result.message;
					if(result.accepted)
					{
						if(result.category == "Pending" && result.result.contains("sourceRootHash")
							&& result.result.contains("sourcePublicationGeneration"))
						{
							{
								std::scoped_lock lock(terrainAutomationMutex);
								pendingTerrainCollisionJobs.emplace(job.jobId, PendingTerrainCollisionJob{
									result.result["sourceRootHash"].get<std::string>(),
									result.result["sourcePublicationGeneration"].get<std::uint64_t>()});
							}
							(void)jobService.PublishProgress(job.jobId, 0.5, "waiting_for_terrain_collision_publish");
							return AutomationDispatchResult{true,ErrorCategory::Internal,result.message,
								{{"jobId",job.jobId},{"state","running"},{"visibilityBoundary","terrain_collision_publish_before_physics"}}};
						}
						(void)jobService.CompleteSucceeded(job.jobId, terminal);
						return AutomationDispatchResult{true,ErrorCategory::Internal,result.message,{{"jobId",job.jobId},{"terminalReceipt",std::move(terminal)}}};
					}
					(void)jobService.CompleteFailed(job.jobId, terminal);
					return AutomationDispatchResult{false, result.category=="InvalidSchema"?ErrorCategory::Validation:ErrorCategory::Unavailable,
						result.message, {{"jobId",job.jobId},{"category",result.category}}};
				}
				if(!terrainAutomationAdapter->IsConfigured())
					return AutomationDispatchResult{false, ErrorCategory::Unavailable, "Terrain edit automation requires an open editor terrain session.", {}};
				const auto parsed=ParseTerrainAutomationRequest(context.arguments);
				if(!parsed.accepted) return AutomationDispatchResult{false, ErrorCategory::Validation, parsed.message, {{"category",parsed.category}}};
				const auto preflight=terrainAutomationAdapter->Preflight(parsed.request);
				if(!preflight.accepted) return AutomationDispatchResult{false, preflight.category=="StaleGeneration"?ErrorCategory::StaleTarget:ErrorCategory::OverBudget, preflight.message, {{"category",preflight.category}}};
				if(parsed.request.previewOnly)
					return AutomationDispatchResult{true, ErrorCategory::Internal, "Terrain detached preview preflight accepted.", {{"previewOnly",true},{"approvalFingerprint",preflight.fingerprint},{"visibilityBoundary","terrain_authoring_publish"}}};
				auto operationMatchesCapability = [&]() {
					const auto& kind = parsed.request.operation;
					if(route == "terrain.sculpt") return kind=="raise" || kind=="lower" || kind=="smooth" || kind=="flatten" || kind=="ramp" || kind=="stamp" || kind=="height-remap";
					if(route == "terrain.layer") return kind=="paint" || kind=="erase-layer" || kind=="fill-layer" || kind=="normalize-layers";
					if(route == "terrain.hole") return kind=="paint-hole" || kind=="erase-hole";
					if(route == "terrain.procedural") return kind=="terrace" || kind=="noise" || kind=="hydraulic-erosion" || kind=="thermal-erosion" || kind=="spline-grade";
					return false;
				};
				if(!operationMatchesCapability()) return AutomationDispatchResult{false,ErrorCategory::Validation,"Terrain operation kind does not match the requested capability.",{{"category","UnsupportedOperation"}}};
				const auto job=jobService.Create({"terrain_edit",operation,context.requestId,context.correlationId,context.idempotencyKey,true}); std::string error;
				if(route == "terrain.layer" || route == "terrain.hole")
				{
					const auto started = terrainAutomationAdapter->DispatchTypedOperation(operation, parsed.request);
					if(!started.accepted || !started.transactionId)
					{
						(void)jobService.CompleteFailed(job.jobId,{{"category",started.category},{"message",started.message}});
						return AutomationDispatchResult{false,ErrorCategory::Unavailable,started.message,{{"jobId",job.jobId},{"category",started.category}}};
					}
					terrainAutomationAdapter->TrackPendingTransaction(*started.transactionId, job.jobId, parsed.request);
				}
				else if(!terrainAutomationAdapter->PrepareAndEnqueue(parsed.request,job.jobId,error)) { (void)jobService.CompleteFailed(job.jobId,{{"category","Unavailable"},{"message",error}}); return AutomationDispatchResult{false,ErrorCategory::Unavailable,error,{{"jobId",job.jobId}}}; }
				(void)jobService.SetState(job.jobId,AutomationJobState::Preparing,"queued_for_terrain_authoring_boundary");
				return AutomationDispatchResult{true,ErrorCategory::Internal,"Terrain transaction queued for protected publication.",{{"jobId",job.jobId},{"visibilityBoundary","terrain_authoring_publish"}}};
			}};
		};
		terrainAutomationProviderRegistered=capabilityRegistry.RegisterProvider(std::make_shared<TerrainAutomationProvider>(resolver)).registered;
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
		add("prefab.instantiate_many", "prefab", MutationMode::SceneMutation, true, true, false,
			"prefab, count, namePrefix?, spacing?, columns?, origin?, requestedAnimationMode?, defaultClipAssetIds?, defaultClipStartFrameStep?");
		add("prefab.overrides.inspect", "prefab", MutationMode::ReadOnly, true, true, false, "instance");
		add("prefab.overrides.apply", "prefab", MutationMode::AssetMutation, true, true, false, "instance, expectedPrefabGeneration");
		add("prefab.overrides.revert", "prefab", MutationMode::SceneMutation, true, true, false, "instance");
		add("prefab.edit.open", "prefab", MutationMode::ProjectMutation, true, false, false, "prefab, dirtyPolicy");
		add("prefab.edit.close", "prefab", MutationMode::ProjectMutation, true, false, false, "contextId, dirtyPolicy");
		add("script.status", "script", MutationMode::ReadOnly, true, false, false, "no arguments");
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
		add("scene_view.settings.get", "scene_view", MutationMode::ReadOnly, false, false, false, "no arguments");
		add("scene_view.settings.set", "scene_view", MutationMode::EditorSelection, false, false, true, "settings: complete validated Scene View performance document");
		add("profiler.window.visibility.set", "profiler", MutationMode::EditorSelection, false, false, false, "visible: bool");
		for (const std::string& name : {"profiler.capture.start", "profiler.capture.stop", "profiler.capture.list", "profiler.capture.load", "profiler.capture.summary"})
			add(name, "profiler", name.ends_with("list") || name.ends_with("summary") ? MutationMode::ReadOnly : MutationMode::ProjectMutation, true, false, false, "artifactId?");
		add("editor.commands.list", "editor_commands", MutationMode::ReadOnly, false, false, false, "no arguments");
		add("editor.commands.invoke", "editor_commands", MutationMode::ProjectMutation, false, false, false, "commandId");
		add("scene.open", "scene", MutationMode::SceneMutation, true, false, true, "path: project-relative scene path");
		add("scene.save", "scene", MutationMode::SceneMutation, true, true, false, "path?: project-relative scene path");
		add("scene.list_objects", "scene", MutationMode::ReadOnly, false, false, false,
			"includeComponents?: bool, search?: string, offset?: uint, limit?: 1..256");
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
		add("asset.content.verify", "asset", MutationMode::ReadOnly, true, false, false, "path: project-relative asset path, requestCount: 1..32768, cancellationEvery?: 0..32768, expectedPayloadBytes?: integer");
		add("asset.import", "asset", MutationMode::AssetMutation, true, false, false, "path: project-relative asset path, force?: bool; model packages such as glTF/FBX return generated asset ids and diagnostics");
		add("asset.virtualGeometry.settings.get", "asset", MutationMode::ReadOnly, true, false, false, "asset?: asset path|uuid, includeResolved?: bool");
		add("asset.virtualGeometry.settings.set", "asset", MutationMode::AssetMutation, true, false, false, "asset: asset path|uuid, override: sparse settings, validateOnly?: bool, rebuildAfterSave?: bool");
		add("asset.virtualGeometry.settings.reset", "asset", MutationMode::AssetMutation, true, false, false, "asset: asset path|uuid, rebuildAfterReset?: bool");
		add("asset.virtualGeometry.rebuild", "asset", MutationMode::AssetMutation, true, false, false, "target: asset path|uuid|all, force?: bool, includePackageOutputs?: bool, diagnosticsLevel?: summary|detailed|debug");
		add("asset.virtualGeometry.inspect", "asset", MutationMode::ReadOnly, true, false, false, "asset: asset path|uuid, includeRuntimeState?: bool, viewType?: Game|Scene|Shadow|ExportRuntime");
		// Production tooling aliases deliberately reuse the validated asset
		// handlers. They expose explicit dotted contracts without inventing a
		// successful result when the installed compiler/runtime is unavailable.
		add("asset.virtualGeometry.codec.inspect", "asset", MutationMode::ReadOnly, true, false, false, "asset: asset path|uuid");
		add("asset.virtualGeometry.cache.inspect", "asset", MutationMode::ReadOnly, true, false, false, "asset: asset path|uuid");
		add("asset.virtualGeometry.migration.inspect", "asset", MutationMode::ReadOnly, true, false, false, "asset: asset path|uuid");
		add("asset.virtualGeometry.repair.plan", "asset", MutationMode::ReadOnly, true, false, false, "asset: asset path|uuid");
		add("asset.virtualGeometry.repair.execute", "asset", MutationMode::AssetMutation, true, false, false, "target: asset path|uuid|all, force?: bool");
		add("asset.virtualGeometry.benchmark.status", "asset", MutationMode::ReadOnly, true, false, false, "asset?: asset path|uuid");
		add("asset.virtualGeometry.benchmark.capture", "asset", MutationMode::ExportMutation, true, false, false, "target: asset path|uuid, output?: path");
		add("asset.prepare_delete", "asset", MutationMode::Destructive, true, false, true, "path: project-relative asset path");
		add("export.validate", "export", MutationMode::ReadOnly, true, false, false, "profile: string");
		add("export.start", "export", MutationMode::ExportMutation, true, false, true, "profile: string, confirmOutput?: string");
		add("export.cancel", "export", MutationMode::ExportMutation, true, false, false, "jobId: string");
		add("game_view.capture", "capture", MutationMode::ReadOnly, false, false, false, "width?: integer, height?: integer, format?: image/png, freshness?: latestCompleted|nextFrame, includeOverlays?: bool, transparentBackground?: bool");
		add("camera.capture_object", "capture", MutationMode::ReadOnly, false, true, false, "object|camera|target: uuid|entity|name, includeDisabled?: bool, width?: integer, height?: integer, format?: image/png");
		add("camera.capture_custom", "capture", MutationMode::ReadOnly, false, false, false, "camera.position: canonical cell+offset, lookAt?: canonical cell+offset, forward?: local [x,y,z], up?: local [x,y,z], projection?: perspective|orthographic");
		add("renderer.virtualGeometry.stats", "renderer", MutationMode::ReadOnly, false, false, false, "viewType?: Game|Scene|Shadow");
		add("renderer.virtualGeometry.capabilityEvidence", "renderer", MutationMode::ReadOnly, false, false, false, "viewType?: Game|Scene");
		add("renderer.virtualGeometry.validationRoute.set", "renderer", MutationMode::EditorSelection, false, false, false, "route: Hardware|Compute|Indexed|Hybrid");
		add("renderer.virtualGeometry.validationRoute.reset", "renderer", MutationMode::EditorSelection, false, false, false, "no arguments");
		add("renderer.virtualGeometry.authoritativePick", "renderer", MutationMode::ReadOnly, false, false, false, "viewType?: Game|Scene, pixel: [x,y], request?: bool, capture?: bool");
		add("renderer.validation.sparseTransform.start", "renderer", MutationMode::SceneMutation, true, true, false, "expectedObjectCount: integer, percentPerFrame?: number, displacement?: number");
		add("renderer.validation.sparseTransform.status", "renderer", MutationMode::ReadOnly, true, true, false, "operationId?: string");
		add("renderer.validation.sparseTransform.stop", "renderer", MutationMode::SceneMutation, true, true, false, "operationId: string");
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
		// Schema validation is bounded owner preparation while an export occupies
		// the shared pool. Waiting for a queued schema task here can starve cancel,
		// duplicate admission and job observation when the pool has one worker.
		const bool exportOwnsWorker = editorContext && editorContext->exportController
			&& editorContext->exportController->HasPendingOperation();
		if (capability->descriptor.execution.workerPreparation && !exportOwnsWorker && ThreadPool::instance().getThreadCount() > 0u)
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
		// Most editor tools use textual correlations for logs, while the typed
		// navigation-agent ABI deliberately uses a nonzero uint64 correlation.
		// Preserve that native value in the request context as text without
		// changing the validated JSON payload seen by the typed handler.
		if(const auto correlation = arguments.find("correlationId"); correlation != arguments.end()
			&& correlation->is_number_unsigned())
			request.correlationId = std::to_string(correlation->get<std::uint64_t>());
		else request.correlationId = arguments.value("correlationId", requestId);
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

	AutomationDispatchResult EditorAutomationService::ExecuteAnimationPreviewCommit(
		const nlohmann::json& arguments,
		bool rendererAvailable)
	{
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::Editor::Animation;
		const auto reject = [](ErrorCategory category, std::string message) {
			return AutomationDispatchResult{false, category, std::move(message), nlohmann::json::object()};
		};
		const auto mapStatus = [](AnimationStatus status) {
			switch(status)
			{
			case AnimationStatus::InvalidArgument:
			case AnimationStatus::InvalidType:
			case AnimationStatus::InvalidChunk:
			case AnimationStatus::InvalidEnvelope: return ErrorCategory::Validation;
			case AnimationStatus::StaleHandle: return ErrorCategory::StaleTarget;
			case AnimationStatus::HardLimitExceeded:
			case AnimationStatus::BufferTooSmall: return ErrorCategory::OverBudget;
			case AnimationStatus::Unsupported: return ErrorCategory::UnsupportedCapability;
			case AnimationStatus::Cancelled: return ErrorCategory::Canceled;
			case AnimationStatus::MissingAsset:
			case AnimationStatus::NotReady: return ErrorCategory::MissingTarget;
			case AnimationStatus::Rejected: return ErrorCategory::Conflict;
			default: return ErrorCategory::Internal;
			}
		};
		if(editorContext == nullptr || editorContext->assetDatabase == nullptr)
		{
			return reject(ErrorCategory::Unavailable,
				"Animation preview requires the open project's AssetDatabase.");
		}
		const std::optional<UUID> assetId = TerraEngine::ParseUUID(
			arguments.value("assetId", std::string{}));
		if(!assetId)
		{
			return reject(ErrorCategory::Validation, "assetId is not a UUID.");
		}

		AssetDatabase& assets = *editorContext->assetDatabase;
		std::filesystem::path compiledPath;
		std::string displayName;
		std::uint64_t publishedGeneration = 1U;
		AnimationCompatibilityFingerprint publishedFingerprint = 0U;
		if(const AssetMetadata* metadata = assets.FindAssetByGuid(*assetId))
		{
			if(metadata->importerName != "AnimationClipAssetImporter")
			{
				return reject(ErrorCategory::MissingTarget,
					"animation.preview.play requires a registered animation clip asset.");
			}
			compiledPath = assets.GetBuildRoot() / metadata->compiledPath;
			displayName = metadata->sourcePath.filename().string();
			publishedGeneration = std::max<std::uint64_t>(1U,
				ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
					*assetId, AssetType::AnimationClip));
		}
		else
		{
			GeneratedModelPackageChildMetadata generated;
			if(!assets.TryGetGeneratedModelPackageChildMetadata(
				*assetId, GeneratedModelPackageChildKind::AnimationClip, generated))
			{
				return reject(ErrorCategory::MissingTarget,
					"The requested standalone or generated animation clip was not found.");
			}
			compiledPath = assets.GetBuildRoot() / generated.compiledPath;
			displayName = generated.displayName;
			publishedGeneration = std::max<std::uint64_t>(1U, generated.publishedGeneration);
			publishedFingerprint = generated.compatibilityFingerprint;
		}

		constexpr std::size_t kMaximumPreviewClipBytes = 256U * 1024U * 1024U;
		std::vector<std::uint8_t> bytes;
		std::string readError;
		if(!ReadBoundedAutomationFile(compiledPath, kMaximumPreviewClipBytes, bytes, readError))
		{
			return reject(ErrorCategory::Unavailable, std::move(readError));
		}
		AnimationPreviewAssetInput input;
		const AnimationStatus decoded = DeserializeAnimationClipAsset(bytes, 0U, input.clip);
		if(decoded != AnimationStatus::Ok || input.clip.assetId.value != *assetId)
		{
			return reject(ErrorCategory::Validation,
				"The requested clip failed checked deserialization with status "
				+ std::string(ToString(decoded)) + ".");
		}
		if(publishedFingerprint != 0U
			&& input.clip.compatibilityFingerprint != publishedFingerprint)
		{
			return reject(ErrorCategory::StaleTarget,
				"The generated clip fingerprint no longer matches its package publication.");
		}
		input.clip.runtimeGeneration = publishedGeneration;
		input.residentSegments.assign(input.clip.segments.size(), 1U);
		for(const AnimationClipTrack& track : input.clip.tracks)
		{
			if(track.kind == AnimationClipTrackKind::Translation
				|| track.kind == AnimationClipTrackKind::Rotation
				|| track.kind == AnimationClipTrackKind::Scale)
			{
				input.jointStableIds.push_back(track.stableTargetId);
			}
		}
		std::sort(input.jointStableIds.begin(), input.jointStableIds.end());
		input.jointStableIds.erase(
			std::unique(input.jointStableIds.begin(), input.jointStableIds.end()),
			input.jointStableIds.end());
		SkeletonAsset skeleton;
		AnimationGeneration skeletonGeneration = 0U;
		if(ResourceManager::Instance().CopySkeletonPublication(
			input.clip.skeletonAssetId.value, skeleton, skeletonGeneration)
			&& skeleton.compatibilityFingerprint == input.clip.compatibilityFingerprint)
		{
			input.referenceSkeleton = std::move(skeleton);
		}

		AnimationPreviewSessionDescriptor descriptor;
		descriptor.selectionId = *assetId;
		descriptor.clipAssetId = input.clip.assetId;
		descriptor.clipGeneration = publishedGeneration;
		descriptor.compatibilityFingerprint = input.clip.compatibilityFingerprint;
		descriptor.durationTicks = input.clip.durationTicks;
		descriptor.sampleRate = input.clip.sampleRate;
		descriptor.displayName = displayName.empty() ? compiledPath.filename().string() : displayName;
		descriptor.compiledPath = compiledPath;
		descriptor.looping = arguments.value("looping", true);
		descriptor.reverse = arguments.value("reverse", false);
		descriptor.speed = arguments.value("speed", 1.0F);
		descriptor.followSelection = false;

		const std::string action = arguments.value("action", std::string("play"));
		const bool dryRun = arguments.value("dryRun", false);
		const bool playModeActive = ScriptingSystem::Instance().IsRunning();
		const auto executeTransport = [&](AnimationPreviewController& preview,
			AnimationPreviewSessionHandle handle) {
			if(action == "open") return AnimationStatus::Ok;
			if(action == "play") return preview.Play(handle);
			if(action == "pause") return preview.Pause(handle);
			if(action == "resume") return preview.Resume(handle);
			if(action == "stop") return preview.Stop(handle);
			if(action == "seek")
			{
				if(arguments.contains("normalizedTime"))
				{
					return preview.SeekNormalized(handle, arguments["normalizedTime"].get<float>());
				}
				return preview.Seek(handle, arguments.value("seekTick", 0LL));
			}
			if(action == "step") return preview.Step(handle, arguments.value("deltaTicks", 1LL));
			if(action == "set_speed") return preview.SetSpeed(handle, arguments.value("speed", 1.0F));
			if(action == "set_looping") return preview.SetLooping(handle, arguments.value("flag", true));
			if(action == "set_reverse") return preview.SetReverse(handle, arguments.value("flag", false));
			if(action == "tick") return preview.TickSession(handle, arguments.value("deltaSeconds", 0.0));
			if(action == "close")
			{
				return preview.DestroySession(handle, AnimationPreviewEndReason::Explicit);
			}
			return AnimationStatus::Unsupported;
		};

		if(dryRun)
		{
			AnimationPreviewController preview;
			preview.SynchronizeContext(*assetId, playModeActive);
			AnimationPreviewSessionHandle handle;
			AnimationStatus status = preview.CreateIsolatedSession(
				descriptor, input, rendererAvailable, handle);
			if(status == AnimationStatus::Ok)
			{
				status = executeTransport(preview, handle);
			}
			if(status != AnimationStatus::Ok)
			{
				return reject(mapStatus(status),
					"The preview request failed dry-run validation with status "
					+ std::string(ToString(status)) + ".");
			}
			AutomationDispatchResult result;
			result.ok = true;
			result.message = "Validated the isolated preview request without changing editor preview state.";
			result.data = {
				{"action", action},
				{"assetId", TerraEngine::ToHexString(*assetId)},
				{"clipGeneration", publishedGeneration},
				{"rendererAvailable", rendererAvailable},
				{"evaluationMode", rendererAvailable ? "cpu_reference" : "cpu_renderer_fallback"},
				{"publication", "none_dry_run"}
			};
			return result;
		}

		animationAutomationPreview.SynchronizeContext(*assetId, playModeActive);
		bool alive = animationAutomationPreview.IsSessionAlive(animationAutomationPreviewHandle);
		if(alive)
		{
			AnimationPreviewSessionState current;
			if(animationAutomationPreview.GetSessionState(animationAutomationPreviewHandle, current)
				!= AnimationStatus::Ok
				|| animationAutomationPreviewAssetId != *assetId
				|| current.descriptor.clipGeneration != publishedGeneration
				|| action == "open")
			{
				(void)animationAutomationPreview.DestroySession(
					animationAutomationPreviewHandle,
					AnimationPreviewEndReason::ClipGenerationChanged);
				animationAutomationPreviewHandle = {};
				animationAutomationPreviewAssetId = EMPTY_UUID;
				alive = false;
			}
		}
		if(!alive && action == "close")
		{
			AutomationDispatchResult result;
			result.ok = true;
			result.message = "The isolated automation preview was already closed.";
			result.data = {{"action", action}, {"active", false}, {"authoredMutationCount", 0U}};
			return result;
		}
		if(!alive)
		{
			const AnimationStatus created = animationAutomationPreview.CreateIsolatedSession(
				descriptor, input, rendererAvailable, animationAutomationPreviewHandle);
			if(created != AnimationStatus::Ok)
			{
				return reject(mapStatus(created),
					"The isolated preview session could not be created: "
					+ std::string(ToString(created)) + ".");
			}
			animationAutomationPreviewAssetId = *assetId;
		}

		const AnimationStatus status = executeTransport(
			animationAutomationPreview, animationAutomationPreviewHandle);
		if(status != AnimationStatus::Ok)
		{
			return reject(mapStatus(status),
				"The isolated preview command was rejected with status "
				+ std::string(ToString(status)) + ".");
		}
		if(action == "close")
		{
			animationAutomationPreviewHandle = {};
			animationAutomationPreviewAssetId = EMPTY_UUID;
			AutomationDispatchResult result;
			result.ok = true;
			result.message = "Closed the isolated automation preview without changing authored state.";
			result.data = {{"action", action}, {"active", false}, {"authoredMutationCount", 0U}};
			return result;
		}

		AnimationPreviewSessionState state;
		const AnimationStatus stateStatus = animationAutomationPreview.GetSessionState(
			animationAutomationPreviewHandle, state);
		if(stateStatus != AnimationStatus::Ok)
		{
			return reject(mapStatus(stateStatus),
				"The preview changed but its copied state could not be read back.");
		}
		AutomationDispatchResult result;
		result.ok = true;
		result.message = "Applied the command to the isolated editor preview world.";
		result.data = {
			{"action", action},
			{"assetId", TerraEngine::ToHexString(*assetId)},
			{"active", state.active},
			{"handle", {
				{"slotIndex", state.handle.slotIndex},
				{"generation", state.handle.generation}
			}},
			{"clipGeneration", state.descriptor.clipGeneration},
			{"sessionRevision", state.sessionRevision},
			{"playback", {
				{"tick", state.playback.absoluteTick},
				{"resolvedTick", state.playback.resolvedTick},
				{"loopIndex", state.playback.loopCount},
				{"runState", static_cast<std::uint32_t>(state.playback.runState)},
				{"looping", state.playback.looping},
				{"reverse", state.playback.reverse},
				{"speed", state.playback.effectiveSpeed}
			}},
			{"output", {
				{"complete", state.output.complete},
				{"jointCount", state.output.joints.size()},
				{"morphCount", state.output.morphs.size()},
				{"curveCount", state.output.curves.size()},
				{"eventCount", state.output.events.size()},
				{"fallbackChannelCount", state.output.fallbackChannelCount},
				{"unavailableChannelCount", state.output.unavailableChannelCount}
			}},
			{"rendererAvailable", state.rendererAvailable},
			{"evaluationMode", static_cast<std::uint32_t>(state.evaluationMode)},
			{"authoredMutationCount", 0U},
			{"visibilityBoundary", "isolated_preview_protected_publish"}
		};
		return result;
	}

	AutomationDispatchResult EditorAutomationService::ExecuteAnimationAutomationHandler(
		AutomationOperationId operation,
		std::string_view operationName,
		const AutomationRequestContext& request)
	{
		const auto reject = [](ErrorCategory category, std::string message) {
			return AutomationDispatchResult{false, category, std::move(message), nlohmann::json::object()};
		};
		const auto succeed = [](std::string message, nlohmann::json data) {
			return AutomationDispatchResult{true, ErrorCategory::Internal, std::move(message), std::move(data)};
		};
		const auto categoryFromName = [](std::string_view name) {
			if (name == "validation") return ErrorCategory::Validation;
			if (name == "permission") return ErrorCategory::Permission;
			if (name == "approval_required") return ErrorCategory::ApprovalRequired;
			if (name == "missing_target") return ErrorCategory::MissingTarget;
			if (name == "invalid_camera_object") return ErrorCategory::InvalidCameraObject;
			if (name == "unsupported_capability") return ErrorCategory::UnsupportedCapability;
			if (name == "unsupported_readback") return ErrorCategory::UnsupportedReadback;
			if (name == "queue_full") return ErrorCategory::QueueFull;
			if (name == "over_budget") return ErrorCategory::OverBudget;
			if (name == "conflict") return ErrorCategory::Conflict;
			if (name == "stale_target") return ErrorCategory::StaleTarget;
			if (name == "unavailable") return ErrorCategory::Unavailable;
			if (name == "canceled") return ErrorCategory::Canceled;
			if (name == "timeout") return ErrorCategory::Timeout;
			return ErrorCategory::Internal;
		};
		const auto dispatchEnvelope = [&categoryFromName](const nlohmann::json& envelope) {
			AutomationDispatchResult result;
			result.ok = envelope.value("ok", false);
			result.message = envelope.value("message", std::string{});
			result.data = envelope.value("data", nlohmann::json::object());
			if (!result.ok && envelope.contains("error") && envelope["error"].is_object())
			{
				result.error = categoryFromName(envelope["error"].value("category", std::string{}));
			}
			return result;
		};

		if (!request.state.projectAvailable)
		{
			return reject(ErrorCategory::Unavailable, "Animation automation requires an already-open project.");
		}
		// Validate every operation, including job reads/cancellation, before any
		// shared store is inspected or mutated. This keeps the advertised schema
		// authoritative instead of letting the fast job branches accept bad types
		// or unadvertised fields.
		const AutomationDispatchResult validatedRequest = ValidateAnimationAutomationRequest(
			operationName,
			request.arguments);
		if (!validatedRequest.ok)
		{
			return validatedRequest;
		}

		// Handlers execute at the editor's protected automation boundary. Keeping
		// the lock across generation comparison and publication prevents two worker-
		// prepared edits from both accepting the same generation.
		std::scoped_lock lock(animationAutomationMutex);
		if (animationAutomationProjectGeneration != request.state.projectGeneration)
		{
			// Do not carry an old project's recorder sessions across the protected
			// project-generation boundary. The job store reset below would otherwise
			// leave only the in-memory recorder alive.
			AnimationAutomationJobs(jobService).CancelAllRecordings();
			animationAutomationPreview.DestroyAllSessions(
				TerraEngine::Editor::Animation::AnimationPreviewEndReason::Explicit);
			animationAutomationPreviewHandle = {};
			animationAutomationPreviewAssetId = TerraEngine::EMPTY_UUID;
			animationAutomationProjectGeneration = request.state.projectGeneration;
			animationAutomationGeneration = 1U;
			animationAutomationTransactions.Reset();
			animationAutomationDetailedDiagnosticsLease.Reset();
			activeAnimationImportJobs.clear();
		}

		AnimationAutomationJobs animationJobs(jobService);
		if (operation == AutomationOperation("animation.job.get"))
		{
			const std::string jobId = request.arguments.value("jobId", std::string{});
			RefreshAnimationImportJob(jobId);
			return animationJobs.Get(jobId);
		}
		if (operation == AutomationOperation("animation.job.cancel"))
		{
			const std::string jobId = request.arguments.value("jobId", std::string{});
			RefreshAnimationImportJob(jobId);
			return animationJobs.Cancel(jobId);
		}

		nlohmann::json arguments = request.arguments;
		arguments["correlationId"] = request.correlationId;
		if (IsOperationOneOf(operation, {
			AutomationOperation("animation.runtime.diagnostics"),
			AutomationOperation("animation.camera.capture")
		}))
		{
			arguments["rendererAvailable"] = request.state.rendererAvailable;
		}
		const bool isImport = IsOperationOneOf(operation, {
			AutomationOperation("animation.asset.import"),
			AutomationOperation("animation.asset.reimport")
		});
		const bool isMutation = isImport || IsOperationOneOf(operation, {
			AutomationOperation("animation.controller.edit"),
			AutomationOperation("animation.advanced.edit"),
			AutomationOperation("animation.recording.start"),
			AutomationOperation("animation.recording.stop"),
			AutomationOperation("animation.preview.play"),
			AutomationOperation("animation.runtime.parameters"),
			// Runtime procedural and retarget requests only enqueue copied records.
			// Their observable state remains owned by AnimationSystem's later protected
			// capture/publication boundary, but MCP retries still need this outer receipt.
			AutomationOperation("animation.runtime.procedural.goal"),
			AutomationOperation("animation.runtime.retarget.request"),
			AutomationOperation("animation.runtime.motion_match.request"),
			AutomationOperation("animation.runtime.ragdoll.request"),
			// The durable job remains separately reconnect-readable, while this receipt
			// preserves project-generation and idempotency semantics at admission.
			AutomationOperation("animation.motion_database.build")
		});
		if (isImport)
		{
			// Import paths are protected by the open-project generation. Clients do
			// not need a separate animation generation before the asset exists.
			arguments["expectedGeneration"] = animationAutomationGeneration;
		}

		std::string idempotencyClientScope = request.clientId + "#" + std::to_string(request.state.projectGeneration);
		bool idempotencyReserved = false;
		if (isMutation)
		{
			const std::string key = arguments.value("idempotencyKey", std::string{});
			if (key.empty())
			{
				return reject(ErrorCategory::Validation, "Animation mutations require a non-empty idempotency key.");
			}
			nlohmann::json semanticArguments = arguments;
			// Correlation identifies one transport attempt, not the requested work.
			// Excluding it lets a retry that omitted an explicit correlation ID replay
			// the original receipt instead of conflicting with the bridge-generated ID.
			semanticArguments.erase("correlationId");
			const IdempotencyDecision decision = idempotencyStore.Begin({
				idempotencyClientScope,
				std::string(operationName),
				key,
				std::string(operationName) + "\n" + std::to_string(request.state.projectGeneration) + "\n" + semanticArguments.dump(),
				{{"correlationId", request.correlationId}, {"status", "preparing"}}
			});
			if (decision.disposition == IdempotencyDisposition::Replay)
			{
				if (decision.result.value("receiptVersion", 0U) == 1U)
				{
					AutomationDispatchResult replay;
					replay.ok = decision.result.value("ok", false);
					replay.error = categoryFromName(decision.result.value("errorCategory", std::string{}));
					replay.message = decision.result.value("message", std::string{});
					replay.data = decision.result.value("data", nlohmann::json::object());
					replay.data["idempotencyDisposition"] = "replay";
					return replay;
				}
				// Records created by an earlier editor build used the data object as
				// the receipt. Preserve that same-session compatibility additively.
				nlohmann::json replay = decision.result;
				replay["idempotencyDisposition"] = "replay";
				return succeed("Replayed the retained animation automation receipt.", std::move(replay));
			}
			if (decision.disposition == IdempotencyDisposition::Conflict)
			{
				return reject(ErrorCategory::Conflict, "The animation idempotency key was already used for different work.");
			}
			if (decision.disposition == IdempotencyDisposition::CapacityExceeded)
			{
				return reject(ErrorCategory::QueueFull, "The bounded animation idempotency store is full.");
			}
			idempotencyReserved = true;
			// The domain receipt store receives a project/client/operation-scoped key;
			// equal user keys from separate clients or projects cannot alias.
			// The public key is bounded to 128 bytes. Collapse the project/client/
			// operation scope into a stable compact domain key before a second schema
			// validation and the native command identities derive from it.
			arguments["idempotencyKey"] = "domain-" + std::to_string(AutomationOperation(
				idempotencyClientScope + "#" + std::string(operationName) + "#" + key));
		}
		const auto abandonReservation = [&]() {
			if (idempotencyReserved)
			{
				(void)idempotencyStore.Erase(
					idempotencyClientScope,
					operationName,
					request.arguments.value("idempotencyKey", std::string{}));
			}
		};
		const auto publishReservation = [&](const AutomationDispatchResult& terminalResult) {
			if (!idempotencyReserved)
			{
				return;
			}
			(void)idempotencyStore.PublishResult(
				idempotencyClientScope,
				operationName,
				request.arguments.value("idempotencyKey", std::string{}),
				{
					{"receiptVersion", 1U},
					{"ok", terminalResult.ok},
					{"errorCategory", std::string(ToString(terminalResult.error))},
					{"message", terminalResult.message},
					{"data", terminalResult.data}
				},
				true);
		};

		nlohmann::json importPreflightData = nlohmann::json::object();
		if (isImport)
		{
			nlohmann::json preflightArguments = {
				{"path", arguments.value("sourcePath", std::string{})},
				{"force", operation == AutomationOperation("animation.asset.reimport")},
				{"dryRun", true}
			};
			const AutomationDispatchResult preflight = dispatchEnvelope(
				HandleAssetImport(preflightArguments, request.requestId));
			if (!preflight.ok)
			{
				abandonReservation();
				return preflight;
			}
			importPreflightData = preflight.data;
		}

		AutomationDispatchResult result;
		if (isImport || operation == AutomationOperation("animation.asset.inspect"))
		{
			result = ExecuteAnimationAssetOperation(operationName, arguments, animationAutomationTransactions, animationAutomationGeneration);
		}
		else if (IsOperationOneOf(operation, {
			AutomationOperation("animation.controller.edit"),
			AutomationOperation("animation.controller.validate")
		}))
		{
			result = ExecuteAnimationControllerOperation(
				operationName,
				arguments,
				animationAutomationTransactions,
				animationAutomationGeneration,
				editorContext != nullptr ? editorContext->assetDatabase.get() : nullptr);
		}
		else if (operation == AutomationOperation("animation.advanced.edit"))
		{
			result = ExecuteAnimationAdvancedOperation(
				operationName,
				arguments,
				animationAutomationTransactions,
				animationAutomationGeneration,
				editorContext != nullptr ? editorContext->assetDatabase.get() : nullptr);
		}
		else
		{
			std::function<AutomationDispatchResult()> protectedCommit;
			if(operation == AutomationOperation("animation.preview.play"))
			{
				protectedCommit = [this, &arguments, rendererAvailable = request.state.rendererAvailable]() {
					return ExecuteAnimationPreviewCommit(arguments, rendererAvailable);
				};
			}
			result = ExecuteAnimationRuntimeOperation(
				operationName,
				arguments,
				animationAutomationTransactions,
				animationAutomationGeneration,
				protectedCommit,
				&animationJobs,
				editorContext != nullptr && editorContext->assetDatabase != nullptr
					? editorContext->assetDatabase->GetAssetRoot() : std::filesystem::path{},
				&animationAutomationDetailedDiagnosticsLease);
		}
		if (!result.ok)
		{
			abandonReservation();
			return result;
		}

		if (operation == AutomationOperation("animation.asset.inspect"))
		{
			const auto assetId = TerraEngine::ParseUUID(arguments.value("assetId", std::string{}));
			if (!assetId || editorContext == nullptr || editorContext->assetDatabase == nullptr)
			{
				return reject(ErrorCategory::MissingTarget, "The requested animation asset is unavailable in the open project.");
			}
			const AssetMetadata* metadata = editorContext->assetDatabase->FindAssetByGuid(*assetId);
			if (metadata == nullptr)
			{
				return reject(ErrorCategory::MissingTarget, "The requested animation asset was not found.");
			}
			result.data["sourcePath"] = metadata->sourcePath.generic_string();
			result.data["compiledPath"] = metadata->compiledPath.generic_string();
			result.data["importer"] = metadata->importerName;
			result.data["registeredAssetId"] = TerraEngine::ToHexString(metadata->guid);
			result.data["subassets"] = nlohmann::json::array();
			result.data["sourceDiagnostics"] = nlohmann::json::array();

			const std::size_t maximumItems = arguments.value(
				"maximumItems",
				static_cast<std::uint64_t>(kAnimationAutomationMaximumItems));
			std::size_t emittedChildren = 0U;
			for (const GeneratedModelPackageChildKind kind : {
				GeneratedModelPackageChildKind::Mesh,
				GeneratedModelPackageChildKind::Material,
				GeneratedModelPackageChildKind::Texture,
				GeneratedModelPackageChildKind::Skeleton,
				GeneratedModelPackageChildKind::AnimationClip,
				GeneratedModelPackageChildKind::MorphTarget,
				GeneratedModelPackageChildKind::DeformableMesh,
				GeneratedModelPackageChildKind::Prefab
			})
			{
				std::vector<TerraEngine::UUID> childIds;
				if (!editorContext->assetDatabase->TryGetGeneratedModelPackageChildIds(metadata->guid, kind, childIds))
				{
					continue;
				}
				for (const TerraEngine::UUID childId : childIds)
				{
					if (emittedChildren >= maximumItems)
					{
						break;
					}
					GeneratedModelPackageChildMetadata child;
					if (!editorContext->assetDatabase->TryGetGeneratedModelPackageChildMetadata(childId, kind, child))
					{
						continue;
					}
					nlohmann::json dependencies = nlohmann::json::array();
					for (const TerraEngine::UUID dependency : child.dependencies)
					{
						dependencies.push_back(TerraEngine::ToHexString(dependency));
					}
					result.data["subassets"].push_back({
						{"assetId", TerraEngine::ToHexString(child.guid)},
						{"kind", AnimationChildKindName(child.kind)},
						{"displayName", child.displayName},
						{"semanticKey", child.semanticKey},
						{"compiledPath", child.compiledPath.generic_string()},
						{"compatibilityFingerprint", child.compatibilityFingerprint},
						{"publishedGeneration", child.publishedGeneration},
						{"readOnlyPackageChild", child.readOnlyPackageChild},
						{"dependencies", std::move(dependencies)}
					});
					++emittedChildren;
				}
			}

			ModelPackageAnimationSummaryMetadata summary;
			if (editorContext->assetDatabase->TryGetModelPackageAnimationSummary(*metadata, summary))
			{
				result.data["packageSummary"] = {
					{"readable", summary.readable},
					{"publishedAtomically", summary.publishedAtomically},
					{"preservedLastKnownGood", summary.preservedLastKnownGood},
					{"prefabBindingsValid", summary.prefabBindingsValid},
					{"schemaVersion", summary.schemaVersion},
					{"candidateGeneration", summary.candidateGeneration},
					{"publishedGeneration", summary.publishedGeneration},
					{"lastKnownGoodGeneration", summary.lastKnownGoodGeneration},
					{"generatedChildCount", summary.generatedChildCount},
					{"returnedChildCount", emittedChildren},
					{"resultsTruncated", emittedChildren < summary.generatedChildCount},
					{"skeletonCount", summary.skeletonCount},
					{"animationClipCount", summary.animationClipCount},
					{"morphTargetCount", summary.morphTargetCount},
					{"deformableMeshCount", summary.deformableMeshCount},
					{"prefabCount", summary.prefabCount},
					{"dependencyFingerprint", summary.dependencyFingerprint}
				};
				for (const std::string& diagnostic : summary.diagnostics)
				{
					if (result.data["sourceDiagnostics"].size() >= maximumItems)
					{
						break;
					}
					result.data["sourceDiagnostics"].push_back({
						{"category", "import"},
						{"status", summary.errorCount == 0U ? "published" : "invalid"},
						{"correlationId", request.correlationId},
						{"generation", summary.publishedGeneration},
						{"message", diagnostic},
						{"recommendedAction", summary.errorCount == 0U
							? "No action is required."
							: "Inspect the package import result and repair the named source or binding."}
					});
				}
			}
		}

		if (operation == AutomationOperation("animation.controller.validate"))
		{
			const auto assetId = TerraEngine::ParseUUID(arguments.value("assetId", std::string{}));
			if (!assetId || editorContext == nullptr || editorContext->assetDatabase == nullptr)
			{
				return reject(ErrorCategory::MissingTarget, "The requested animation controller is unavailable in the open project.");
			}
			const AssetMetadata* metadata = editorContext->assetDatabase->FindAssetByGuid(*assetId);
			if (metadata == nullptr || metadata->importerName != "AnimationControllerAssetImporter")
			{
				return reject(ErrorCategory::MissingTarget, "The requested asset is not a registered animation controller.");
			}

			constexpr std::size_t kMaximumControllerBytes = 64U * 1024U * 1024U;
			const std::filesystem::path compiledPath =
				editorContext->assetDatabase->GetBuildRoot() / metadata->compiledPath;
			std::vector<std::uint8_t> bytes;
			std::string readError;
			TerraEngine::Animation::AnimationControllerAsset controller;
			TerraEngine::Animation::AnimationStatus status = TerraEngine::Animation::AnimationStatus::InvalidEnvelope;
			if (ReadBoundedAutomationFile(compiledPath, kMaximumControllerBytes, bytes, readError))
			{
				status = TerraEngine::Animation::DeserializeAnimationControllerAsset(bytes, 0U, controller);
				if (status == TerraEngine::Animation::AnimationStatus::Ok)
				{
					status = TerraEngine::Animation::ValidateAnimationControllerAsset(controller);
				}
			}

			const bool valid = status == TerraEngine::Animation::AnimationStatus::Ok;
			nlohmann::json diagnostics = nlohmann::json::array();
			if (!valid)
			{
				diagnostics.push_back({
					{"category", readError.empty() ? "controller_validation" : "asset_read"},
					{"status", readError.empty() ? std::string(TerraEngine::Animation::ToString(status)) : "unavailable"},
					{"correlationId", request.correlationId},
					{"generation", animationAutomationGeneration},
					{"message", readError.empty() ? "The compiled controller did not pass its bounded codec and semantic validation." : readError},
					{"recommendedAction", "Recompile or repair the controller source, then validate the newly published generation."}
				});
			}
			result.message = valid
				? "Validated the compiled animation controller."
				: "The compiled animation controller is invalid; no authored or runtime state changed.";
			result.data = {
				{"schemaVersion", kAnimationAutomationSchemaVersion},
				{"assetId", arguments.value("assetId", std::string{})},
				{"valid", valid},
				{"status", valid ? "ok" : (readError.empty() ? std::string(TerraEngine::Animation::ToString(status)) : "unavailable")},
				{"compatibilityFingerprint", controller.compatibilityFingerprint},
				{"counts", {
					{"parameters", controller.parameters.size()},
					{"nodes", controller.nodes.size()},
					{"states", controller.states.size()},
					{"transitions", controller.transitions.size()},
					{"layers", controller.layers.size()},
					{"subgraphs", controller.subgraphDependencies.size()}
				}},
				{"diagnostics", std::move(diagnostics)},
				{"immutable", true}
			};
		}

		if (operation == AutomationOperation("animation.runtime.diagnostics"))
		{
			using namespace TerraEngine::Animation;
			const std::optional<AnimationDiagnosticsSnapshot> summary =
				AcquireAnimationDiagnosticsSnapshot();
			const AnimationProductionIntegrationSnapshot integration =
				AnimationSystem::Instance().ReadProductionIntegrationSnapshot();
			const TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot
				backendExecution =
					RenderSystem::Instance().GetLatestAnimationGpuBackendExecution();
			const std::vector<TerraEngine::Renderer::RenderFrameRenderableRecord>
				renderables =
					RenderSystem::Instance().GetLatestRenderablesSnapshot();
			const std::shared_ptr<const PublishedGpuAnimationPresentationPlan>
				gpuPresentation =
					AnimationPresentationSystem::Instance()
						.LoadPublishedGpuAnimationPresentationPlan();
			const std::size_t maximumItems = arguments.value(
				"maximumItems",
				static_cast<std::uint64_t>(kAnimationAutomationMaximumItems));
			const auto renderSourceName = [](
				TerraEngine::Renderer::AnimationRenderEvaluationSource source)
				-> std::string_view
			{
				using TerraEngine::Renderer::AnimationRenderEvaluationSource;
				switch (source)
				{
					case AnimationRenderEvaluationSource::None:
						return "None";
					case AnimationRenderEvaluationSource::CpuPose:
						return "CpuPose";
					case AnimationRenderEvaluationSource::GpuVisual:
						return "GpuVisual";
					case AnimationRenderEvaluationSource::HybridAuthoritative:
						return "HybridAuthoritative";
					case AnimationRenderEvaluationSource::SafeFallback:
						return "SafeFallback";
				}
				return "Unknown";
			};
			const auto backendRecordJson = [](
				const TerraEngine::Renderer::AnimationGpuBackendExecutionRecord& record)
				-> nlohmann::json
			{
				if (!record.IsComplete())
				{
					return nullptr;
				}
				return {
					{"frameId", record.frameId},
					{"submissionSerial", record.submissionSerial},
					{"planGeneration", record.planGeneration},
					{"rendererGeneration", record.rendererGeneration},
					{"recordedDispatchCount", record.recordedDispatchCount}
				};
			};
			nlohmann::json gpuPresentationJson = nullptr;
			if (gpuPresentation)
			{
				const GpuAnimationPlan& plan = gpuPresentation->plan;
				const std::vector<GpuAnimationWorkPacket>& workPackets =
					plan.WorkPackets();
				// Keep backward compatibility with the representative first packet,
				// while the bounded packet list below lets diagnostics associate every
				// animated draw with its exact offsets and playback cursor.
				nlohmann::json firstPacket = nullptr;
				nlohmann::json packets = nlohmann::json::array();
				if (!workPackets.empty() && plan.RenderIdentityCount() != 0U)
				{
					const GpuAnimationWorkPacket& packet =
						workPackets.front();
					const GpuAnimationRenderIdentity& identity =
						plan.RenderIdentity(0U);
					nlohmann::json generation = nullptr;
					if (packet.generationRecordIndex
						< plan.GenerationRecordCount())
					{
						const GpuAnimationGenerationRecord& record =
							plan.GenerationRecord(packet.generationRecordIndex);
						generation = {
							{"instanceSlot", record.instanceSlot},
							{"instanceGeneration", record.instanceGeneration},
							{"componentGeneration", record.componentGeneration},
							{"meshGeneration", record.meshGeneration},
							{"resourceGeneration", record.resourceGeneration},
							{"rendererGeneration", record.rendererGeneration},
							{"sceneGeneration", record.sceneGeneration},
							{"workGeneration", record.workGeneration},
							{"programGeneration", record.programGeneration}
						};
					}
					firstPacket = {
						{"stableInstanceId", packet.stableInstanceId},
						{"workPacketIndex", 0U},
						{"stageMask", packet.stageMask},
						{"jointCount", packet.jointCount},
						{"morphCount", packet.morphCount},
						{"sampleTick", plan.SampleTick(0U)},
						{"localPoseOffsetBytes", packet.localPoseOffsetBytes},
						{"modelPoseOffsetBytes", packet.modelPoseOffsetBytes},
						{"paletteOffsetBytes", packet.paletteOffsetBytes},
						{"generation", std::move(generation)},
						{"renderIdentity", {
							{"meshId", TerraEngine::ToHexString(identity.meshId)},
							{"meshHeaderVersion", identity.meshHeaderVersion},
							{"deformablePayloadGeneration",
								identity.deformablePayloadGeneration},
							{"rendererGeneration", identity.rendererGeneration},
							{"sceneGeneration", identity.sceneGeneration},
							{"skeletonCompatibilityFingerprint",
								identity.skeletonCompatibilityFingerprint}
						}}
					};
				}
				for (std::size_t packetIndex = 0U;
					packetIndex < workPackets.size()
						&& packets.size() < maximumItems;
					++packetIndex)
				{
					if (packetIndex >= plan.RenderIdentityCount())
					{
						break;
					}
					const GpuAnimationWorkPacket& packet =
						workPackets[packetIndex];
					const GpuAnimationRenderIdentity& identity =
						plan.RenderIdentity(packetIndex);
					nlohmann::json generation = nullptr;
					if (packet.generationRecordIndex
						< plan.GenerationRecordCount())
					{
						const GpuAnimationGenerationRecord& record =
							plan.GenerationRecord(packet.generationRecordIndex);
						generation = {
							{"instanceSlot", record.instanceSlot},
							{"instanceGeneration", record.instanceGeneration},
							{"componentGeneration", record.componentGeneration},
							{"meshGeneration", record.meshGeneration},
							{"resourceGeneration", record.resourceGeneration},
							{"rendererGeneration", record.rendererGeneration},
							{"sceneGeneration", record.sceneGeneration},
							{"workGeneration", record.workGeneration},
							{"programGeneration", record.programGeneration}
						};
					}
					packets.push_back({
						{"workPacketIndex", packetIndex},
						{"stableInstanceId", packet.stableInstanceId},
						{"stableSubmissionOrdinal",
							packet.stableSubmissionOrdinal},
						{"generationRecordIndex",
							packet.generationRecordIndex},
						{"stageMask", packet.stageMask},
						{"flags", packet.flags},
						{"jointCount", packet.jointCount},
						{"morphCount", packet.morphCount},
						{"paletteEntryCount", packet.paletteEntryCount},
						{"sampleTick", plan.SampleTick(packetIndex)},
						{"scratchOffsetBytes", packet.scratchOffsetBytes},
						{"localPoseOffsetBytes", packet.localPoseOffsetBytes},
						{"modelPoseOffsetBytes", packet.modelPoseOffsetBytes},
						{"paletteOffsetBytes", packet.paletteOffsetBytes},
						{"morphOutputOffsetBytes",
							packet.morphOutputOffsetBytes},
						{"generation", std::move(generation)},
						{"renderIdentity", {
							{"meshId", TerraEngine::ToHexString(identity.meshId)},
							{"meshHeaderVersion", identity.meshHeaderVersion},
							{"deformablePayloadGeneration",
								identity.deformablePayloadGeneration},
							{"rendererGeneration", identity.rendererGeneration},
							{"sceneGeneration", identity.sceneGeneration},
							{"skeletonCompatibilityFingerprint",
								identity.skeletonCompatibilityFingerprint}
						}}
					});
				}
				gpuPresentationJson = {
					{"presentationGeneration",
						gpuPresentation->presentationGeneration},
					{"sourceGeneration", gpuPresentation->sourceGeneration},
					{"loweringStatus",
						static_cast<std::uint32_t>(
							gpuPresentation->loweringStatus)},
					{"planStatus",
						static_cast<std::uint32_t>(
							gpuPresentation->planStatus)},
					{"safeFallback", gpuPresentation->safeFallback},
					{"planGeneration", plan.generation},
					{"workPacketCount",
						workPackets.size()},
					{"dispatchCommandCount",
						plan.DispatchCommands().size()},
					{"ownerMappingCount",
						gpuPresentation->OwnerObjectMappings().size()},
					{"packets", std::move(packets)},
					{"packetsTruncated",
						workPackets.size()
							> maximumItems},
					{"firstPacket", std::move(firstPacket)}
				};
			}

			nlohmann::json diagnostics = nlohmann::json::array();
			nlohmann::json renderableBindings = nlohmann::json::array();
			std::size_t animatedRenderableCount = 0U;
			for (const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable :
				renderables)
			{
				const TerraEngine::Renderer::AnimationRenderGenerationRecord& animation =
					renderable.animation;
				if (animation.source
					== TerraEngine::Renderer::AnimationRenderEvaluationSource::None)
				{
					continue;
				}
				++animatedRenderableCount;
				if (renderableBindings.size() >= maximumItems)
				{
					continue;
				}
				// Keep this record identity-only and bounded. It deliberately omits
				// matrices, palettes, morph arrays, and backend handles.
				const Mesh* runtimeMesh =
					ResourceManager::Instance().GetMesh(renderable.meshId);
				renderableBindings.push_back({
					{"objectId", TerraEngine::ToHexString(renderable.objectId)},
					{"meshId", TerraEngine::ToHexString(renderable.meshId)},
					{"source", std::string(renderSourceName(animation.source))},
					{"complete", animation.complete},
					{"safeFallback", animation.safeFallback},
					{"stableInstanceId", animation.stableInstanceId},
					{"poseGeneration", animation.poseGeneration},
					{"paletteGeneration", animation.paletteGeneration},
					{"programGeneration", animation.programGeneration},
					{"animationPlanGeneration", animation.animationPlanGeneration},
					{"rendererGeneration", animation.rendererGeneration},
					{"firstGpuWorkPacket", animation.firstGpuWorkPacket},
					{"gpuWorkPacketCount", animation.gpuWorkPacketCount},
					{"gpuStageMask", animation.gpuStageMask},
					// These small resource facts explain why an otherwise complete
					// animation binding did or did not produce virtual-page work.
					// They never expose page bytes or mutable backend handles.
					{"meshLoaded", runtimeMesh != nullptr},
					{"deformableDemandedPagesAvailable",
						runtimeMesh != nullptr
							&& runtimeMesh->hasDeformableDemandedPages},
					{"deformableDemandedPageEntryCount",
						runtimeMesh != nullptr
							? runtimeMesh->deformableDemandedPageEntries.size()
							: 0U},
					{"deformableDemandedPageSectionSetCount",
						runtimeMesh != nullptr
							? runtimeMesh->deformableDemandedPageSections.size()
							: 0U},
					{"deformableDemandedPageDiagnostic",
						runtimeMesh != nullptr
							? "Canonical TEMF demanded pages are validated."
							: "Mesh is not loaded."}
				});
			}
			if (!summary)
			{
				diagnostics.push_back({
					{"category", "animation_snapshot"},
					{"status", "not_ready"},
					{"correlationId", request.correlationId},
					{"generation", AnimationSystem::Instance().AuthorityPublicationGeneration()},
					{"message", "No complete animation diagnostics snapshot has reached the protected publication boundary yet."},
					{"recommendedAction", "Advance one editor frame or inspect the renderer-independent production integration snapshot returned with this result."}
				});
			}

			nlohmann::json summaryJson = nullptr;
			if (summary)
			{
				const AnimationDiagnosticsCounters& counters = summary->counters;
				const AnimationDiagnosticsTimings& timings = summary->timings;
				summaryJson = {
					{"schemaVersion", summary->schemaVersion},
					{"frameIndex", summary->frameIndex},
					{"fixedTick", summary->fixedTick},
					{"detailLevel", static_cast<std::uint32_t>(summary->detailLevel)},
					{"dormant", summary->dormant},
					{"baseline", {
						{"feature", std::string(summary->baseline.feature)},
						{"sourceCommit", std::string(summary->baseline.sourceCommit)},
						{"manifestSha256", std::string(summary->baseline.manifestSha256)},
						{"evidenceSha256", std::string(summary->baseline.evidenceSha256)},
						{"workloadsSha256", std::string(summary->baseline.workloadsSha256)}
					}},
					{"counters", {
						{"activeAnimators", counters.activeAnimators},
						{"evaluatedAnimators", counters.evaluatedAnimators},
						{"suspendedAnimators", counters.suspendedAnimators},
						{"degradedAnimators", counters.degradedAnimators},
						{"schedulerMembershipChecks", counters.schedulerMembershipChecks},
						{"workerJobs", counters.workerJobs},
						{"transientAllocations", counters.transientAllocations},
						{"poseAllocations", counters.poseAllocations},
						{"residencyRequests", counters.residencyRequests},
						{"gpuDispatches", counters.gpuDispatches},
						{"gpuUploadBytes", counters.gpuUploadBytes},
						{"gpuReadbacks", counters.gpuReadbacks},
						{"gpuWaits", counters.gpuWaits},
						{"physicsHandoffs", counters.physicsHandoffs},
						{"managedCallbacks", counters.managedCallbacks},
						{"parameterQueueDepth", counters.parameterQueueDepth},
						{"commandQueueDepth", counters.commandQueueDepth},
						{"eventQueueDepth", counters.eventQueueDepth},
						{"rootMotionQueueDepth", counters.rootMotionQueueDepth},
						{"rejectedBatches", counters.rejectedBatches},
						{"staleWorkItems", counters.staleWorkItems},
						{"gpuVisualAnimators", counters.gpuVisualAnimators},
						{"hybridAuthoritativeAnimators", counters.hybridAuthoritativeAnimators},
						{"cpuFallbackAnimators", counters.cpuFallbackAnimators},
						{"cpuSampledJoints", counters.cpuSampledJoints},
						{"gpuEvaluatedJoints", counters.gpuEvaluatedJoints},
						{"gpuBatches", counters.gpuBatches},
						{"compactEligibleAnimators", counters.compactEligibleAnimators},
						{"compactProcessedAnimators", counters.compactProcessedAnimators},
						{"completeCapturedAnimators", counters.completeCapturedAnimators},
						{"completeProcessedAnimators", counters.completeProcessedAnimators},
						{"compactToCompleteTransitions", counters.compactToCompleteTransitions},
						{"completeToCompactTransitions", counters.completeToCompactTransitions},
						{"retainedTopologyHits", counters.retainedTopologyHits},
						{"retainedTopologyMisses", counters.retainedTopologyMisses},
						{"compactPlanGroups", counters.compactPlanGroups},
						{"compactDispatches", counters.compactDispatches},
						{"rejectedCandidateCount", counters.rejectedCandidateCount},
						{"gpuTransferBytes", counters.gpuTransferBytes},
						{"gpuReadbackBytes", counters.gpuReadbackBytes},
						{"completePoseReadbackBytes", counters.completePoseReadbackBytes},
						{"boundedMetadataReadbackBytes", counters.boundedMetadataReadbackBytes},
						{"residencyHits", counters.residencyHits},
						{"residencyMisses", counters.residencyMisses},
						{"fallbackCount", counters.fallbackCount},
						{"modeSwitchCount", counters.modeSwitchCount}
					}},
					{"timings", {
						{"preparationMs", timings.preparationMs},
						{"evaluationMs", timings.evaluationMs},
						{"publicationMs", timings.publicationMs},
						{"presentationMs", timings.presentationMs},
						{"cpuAuthorityMs", timings.cpuAuthorityMs},
						{"gpuPlanningMs", timings.gpuPlanningMs},
						{"gpuExecutionMs", timings.gpuExecutionMs},
						{"deformationMs", timings.deformationMs},
						{"compactCaptureMs", timings.compactCaptureMs},
						{"completeCaptureMs", timings.completeCaptureMs},
						{"compactEvaluationMs", timings.compactEvaluationMs},
						{"completeEvaluationMs", timings.completeEvaluationMs},
						{"topologyBuildMs", timings.topologyBuildMs},
						{"topologyRefreshMs", timings.topologyRefreshMs}
					}}
				};
			}

			nlohmann::json detailedJson = nullptr;
			if (arguments.value("includeDetails", false))
			{
				// The runtime adapter retained this lease before returning to the
				// service. Do not create a local replacement here: its destructor would
				// disable detailed collection before the next protected frame.
				if (!animationAutomationDetailedDiagnosticsLease.EnsureRequested())
				{
					return reject(ErrorCategory::Unavailable,
						"Detailed animation diagnostics could not acquire the bounded automation collection request.");
				}
				const std::optional<AnimationDetailedDiagnosticsSnapshot> detailed =
					AcquireAnimationDetailedDiagnosticsSnapshot();
				if (!detailed)
				{
					diagnostics.push_back({
						{"category", "animation_detailed_snapshot"},
						{"status", "pending_publication"},
						{"correlationId", request.correlationId},
						{"generation", AnimationSystem::Instance().AuthorityPublicationGeneration()},
						{"message", "Detailed animation diagnostics were requested and will become available after the next protected animation publication."},
						{"recommendedAction", "Advance an editor or simulation frame, then request detailed diagnostics again."}
					});
				}
				else
				{
					nlohmann::json animators = nlohmann::json::array();
					for (const AnimationAnimatorDiagnosticRecord& animator : detailed->animators)
					{
						if (animators.size() >= maximumItems)
						{
							break;
						}
						animators.push_back({
							{"stableInstanceId", animator.stableInstanceId},
							{"instanceSlot", animator.instanceSlot},
							{"instanceGeneration", animator.instanceGeneration},
							{"requestedPolicy", static_cast<std::uint32_t>(animator.requestedPolicy)},
							{"effectiveMode", std::string(ToString(animator.effectiveMode))},
							{"selectionReason", std::string(ToString(animator.selectionReason))},
							{"fallbackReason", std::string(ToString(animator.fallbackReason))},
							{"poseResidency", static_cast<std::uint32_t>(animator.poseResidency)},
							{"cpuAuthoritativeJointCount", animator.cpuAuthoritativeJointCount},
							{"gpuStageMask", animator.gpuStageMask},
							{"batchOrdinal", animator.batchOrdinal},
							{"gpuEvaluatedElements", animator.gpuEvaluatedElements},
							{"transferBytes", animator.transferBytes},
							{"readbackBytes", animator.readbackBytes},
							{"completePoseReadbackBytes", animator.completePoseReadbackBytes},
							{"readbackCount", animator.readbackCount},
							{"synchronizationWaitCount", animator.synchronizationWaitCount},
							{"modeSwitchCount", animator.modeSwitchCount}
						});
					}
					detailedJson = {
						{"schemaVersion", detailed->schemaVersion},
						{"frameIndex", detailed->frameIndex},
						{"collapsed", detailed->collapsed},
						{"animators", std::move(animators)},
						{"totalAnimatorRecords", detailed->animators.size()},
						{"recordsTruncated", detailed->animators.size() > maximumItems},
						{"modeSwitchHistoryCount", detailed->modeSwitchHistory.size()}
					};
				}
			}

			result.message = summary
				? "Read the immutable animation runtime diagnostics snapshot."
				: "Animation diagnostics are not published yet; returned renderer-independent readiness data.";
			result.data = {
				{"schemaVersion", kAnimationAutomationSchemaVersion},
				{"rendererAvailable", request.state.rendererAvailable},
				{"summaryAvailable", summary.has_value()},
				// Callers need to distinguish a deliberately deferred detailed
				// snapshot from a collapsed or empty result. Keep this main service
				// endpoint aligned with AnimationRuntimeOperations' public contract.
				{"detailsPendingPublication",
					arguments.value("includeDetails", false)
						&& detailedJson.is_null()},
				{"summary", std::move(summaryJson)},
				{"details", std::move(detailedJson)},
				{"gpuPresentation", std::move(gpuPresentationJson)},
				{"renderableBindings", {
					{"records", std::move(renderableBindings)},
					{"totalAnimatedRenderables", animatedRenderableCount},
					{"recordsTruncated", animatedRenderableCount > maximumItems},
					{"animatedPageDemandCount",
						RenderSystem::Instance()
							.GetLatestAnimatedGeometryPageDemandCount()}
				}},
				{"gpuBackendExecution", {
					{"submitted", backendExecution.HasSubmittedWork()},
					{"completed", backendExecution.HasCompletedWork()},
					{"latestSubmitted",
						backendRecordJson(backendExecution.latestSubmitted)},
					{"latestCompleted",
						backendRecordJson(backendExecution.latestCompleted)}
				}},
				{"integration", {
					{"planStatus", std::string(ToString(integration.planStatus))},
					{"rejectionReason", std::string(ToString(integration.rejectionReason))},
					{"rejectionItemIndex",
						integration.rejectionItemIndex == std::numeric_limits<std::size_t>::max()
							? nlohmann::json(nullptr)
							: nlohmann::json(integration.rejectionItemIndex)},
					{"executionPolicy", static_cast<std::uint32_t>(integration.executionPolicy)},
					{"availableWorkerCount", integration.availableWorkerCount},
					{"selectedWorkerCount", integration.selectedWorkerCount},
					{"itemCount", integration.itemCount},
					{"rangeCount", integration.rangeCount},
					{"scratchBytes", integration.scratchBytes},
					{"cacheEntryCount", integration.cacheEntryCount},
					{"reusedItemCount", integration.reusedItemCount},
					{"fallbackItemCount", integration.fallbackItemCount},
					{"bindingEpisodeCount", integration.bindingEpisodeCount},
					{"bindingReconciledCount", integration.bindingReconciledCount},
					{"bindingFailureCount", integration.bindingFailureCount},
					{"bindingReconciliationMs",
						static_cast<double>(integration.bindingReconciliationNanoseconds)
							/ 1000000.0},
					{"deterministicPublicationValidated", integration.deterministicPublicationValidated},
					{"cancellationRequested", integration.cancellationRequested},
					{"memory", {
						{"configured", integration.memory.configured},
						{"shutdown", integration.memory.shutdown},
						{"totalBytesInUse", integration.memory.totalBytesInUse},
						{"totalHighWaterBytes", integration.memory.totalHighWaterBytes},
						{"activeReservations", integration.memory.activeReservations},
						{"pressureEvents", integration.memory.pressureEvents},
						{"allocationFailures", integration.memory.allocationFailures}
					}}
				}},
				{"diagnostics", std::move(diagnostics)}
			};
		}

		if (operation == AutomationOperation("animation.camera.capture"))
		{
			const std::string source = arguments.value("source", std::string{});
			nlohmann::json envelope;
			if (source == "game")
			{
				envelope = HandleGameViewCapture(arguments, request.requestId);
			}
			else if (source == "camera")
			{
				envelope = HandleCameraObjectCapture(arguments, request.requestId);
			}
			else if (source == "custom")
			{
				envelope = HandleCustomCameraCapture(arguments, request.requestId);
			}
			else if (source == "scene")
			{
				const std::shared_ptr<SceneViewWindow> sceneView = mainEditor != nullptr
					? mainEditor->GetWindow<SceneViewWindow>()
					: nullptr;
				if (sceneView == nullptr)
				{
					return reject(ErrorCategory::Unavailable, "Scene View is unavailable in the installed editor session.");
				}
				const nlohmann::json sceneState = sceneView->GetAutomationState();
				const nlohmann::json camera = sceneState.value("camera", nlohmann::json::object());
				nlohmann::json sceneCapture = arguments;
				sceneCapture["name"] = "Scene View";
				// Scene View already publishes canonical locations. Preserve that exact
				// representation while adapting it to the custom-capture envelope.
				// Float world-vector fallbacks would silently move an unbounded camera.
				sceneCapture["camera"] = {
					{"position", camera.value("position", nlohmann::json())},
					{"lookAt", camera.value("focus", nlohmann::json())},
					{"up", camera.value("up", nlohmann::json::array({0.0, 1.0, 0.0}))},
					{"projection", sceneState.value("projection", "perspective")},
					{"verticalFovDegrees", camera.value("fieldOfViewDegrees", 60.0)},
					{"orthographicHeight", 10.0},
					{"nearPlane", 0.1},
					{"farPlane", 10000.0}};
				envelope = HandleCustomCameraCapture(sceneCapture, request.requestId);
				if (envelope.value("ok", false) && envelope.contains("data"))
				{
					envelope["data"]["automationSource"] = "scene";
					envelope["data"]["sceneViewState"] = std::move(sceneState);
				}
			}
			else
			{
				return reject(ErrorCategory::Validation, "Animation capture source is not supported.");
			}
			return dispatchEnvelope(envelope);
		}

		if (isImport && arguments.value("dryRun", false))
		{
			result.data["preflight"] = std::move(importPreflightData);
		}
		if (isImport && !arguments.value("dryRun", false))
		{
			// Reserve the durable job before queuing the real AssetDatabase mutation.
			// If admission fails, the preflight above had no side effects and the
			// caller can retry without duplicating an already-queued import.
			const AutomationDispatchResult started = animationJobs.Start({
				"animation_import",
				std::string(operationName),
				request.requestId,
				request.correlationId,
				request.arguments.value("idempotencyKey", std::string{}),
				true
			});
			if (!started.ok)
			{
				abandonReservation();
				return started;
			}
			const std::string jobId = started.data.value("jobId", std::string{});
			nlohmann::json importArguments = {
				{"path", arguments.value("sourcePath", std::string{})},
				{"force", operation == AutomationOperation("animation.asset.reimport")},
				{"dryRun", false}
			};
			AutomationDispatchResult imported = dispatchEnvelope(
				HandleAssetImport(importArguments, request.requestId));
			if (!imported.ok)
			{
				(void)jobService.CompleteFailed(jobId, {
					{"message", imported.message},
					{"errorCategory", std::string(ToString(imported.error))}
				});
				imported.data["jobId"] = jobId;
				imported.data["correlationId"] = request.correlationId;
				publishReservation(imported);
				return imported;
			}
			const std::filesystem::path relative = imported.data.value("path", std::string{});
			const std::filesystem::path absolute = editorContext->assetDatabase->GetAssetRoot() / relative;
			const std::uint32_t timeoutMs = arguments.value("timeoutMs", 300000U);
			activeAnimationImportJobs.emplace(jobId, ActiveAnimationImportJob{
				absolute,
				request.state.projectGeneration,
				std::chrono::system_clock::now() + std::chrono::milliseconds(timeoutMs)
			});
			RefreshAnimationImportJob(jobId);
			result.data["jobId"] = jobId;
			result.data["import"] = std::move(imported.data);
		}

		const bool dryRun = arguments.value("dryRun", false);
		if (isMutation && !dryRun)
		{
			++animationAutomationGeneration;
		}
		result.data["generation"] = animationAutomationGeneration;
		result.data["correlationId"] = request.correlationId;
		result.data["diagnostics"] = result.data.value("diagnostics", nlohmann::json::array());
		if (isMutation)
		{
			result.data["idempotencyDisposition"] = "new";
			publishReservation(result);
		}
		return result;
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
			nlohmann::json scheduler = nlohmann::json::array();
			nlohmann::json stages = nlohmann::json::array();
			if (diagnostics->scheduler)
			{
				// Diagnostics are published data, so copying their scheduler declaration
				// here cannot expose a mutable scheduler or provider-owned work item.
				for (const PhysicsSchedulerDeclaration& declaration : *diagnostics->scheduler)
				{
					scheduler.push_back({
						{"workload", declaration.workload}, {"phase", declaration.phase},
						{"reads", declaration.reads}, {"writes", declaration.writes},
						{"boundary", declaration.boundary}, {"workerSafe", declaration.workerSafe}
					});
				}
			}
			// Stage records remain copied values. Automation clients never receive a
			// provider handle or a reference to mutable profiler storage.
			for (const PhysicsStageMeasurement& stage : diagnostics->stages)
			{
				stages.push_back({
					{"kind", static_cast<std::uint32_t>(stage.stage)}, {"durationNanoseconds", stage.durationNanoseconds},
					{"workItems", stage.workItems}, {"allocationCount", stage.allocationCount}, {"allocationBytes", stage.allocationBytes},
					{"allocationCountAvailable", stage.allocationCountAvailable}, {"allocationBytesAvailable", stage.allocationBytesAvailable},
					{"availability", static_cast<std::uint32_t>(stage.availability)},
					{"unavailableReason", static_cast<std::uint32_t>(stage.unavailableReason)}
				});
			}
			return succeed("Read immutable physics diagnostics.", snapshot({
				{"counters", {{"bodies", diagnostics->counters.bodyCount}, {"shapes", diagnostics->counters.shapeCount}, {"constraints", diagnostics->counters.constraintCount}, {"characters", diagnostics->counters.characterCount}, {"contacts", diagnostics->counters.contactCount}, {"rejected", diagnostics->counters.rejectedCount}, {"stale", diagnostics->counters.staleCount}}},
				{"queues", {{"commands", diagnostics->queues.commandDepth}, {"queries", diagnostics->queues.queryDepth}, {"events", diagnostics->queues.eventDepth}}},
				{"memory", {{"snapshots", diagnostics->memory.snapshotBytes}, {"highWater", diagnostics->memory.highWaterBytes}, {"providerAllocations", diagnostics->memory.outstandingProviderAllocations}, {"providerAllocationHighWater", diagnostics->memory.highWaterProviderAllocations}}},
				{"cache", {{"shapes", diagnostics->cache.sharedShapeCount}, {"assets", diagnostics->cache.assetCount}, {"bytes", diagnostics->cache.payloadBytes}, {"hits", diagnostics->cache.hitCount}, {"misses", diagnostics->cache.missCount}}},
				{"authoring", {
					{"generation", diagnostics->authoring.authoringGeneration}, {"snapshotGeneration", diagnostics->authoring.snapshotGeneration},
					{"assetJournalGeneration", diagnostics->authoring.assetJournalGeneration}, {"changedObjects", diagnostics->authoring.changedObjects},
					{"added", diagnostics->authoring.added}, {"removed", diagnostics->authoring.removed}, {"transforms", diagnostics->authoring.transforms},
					{"bodyComponents", diagnostics->authoring.bodyComponents}, {"colliderComponents", diagnostics->authoring.colliderComponents},
					{"hierarchyDependencies", diagnostics->authoring.hierarchyDependencies}, {"colliderAssets", diagnostics->authoring.colliderAssets},
					{"materialAssets", diagnostics->authoring.materialAssets}, {"settings", diagnostics->authoring.settings},
					{"assetObservations", diagnostics->authoring.assetObservations}, {"snapshotReused", diagnostics->authoring.snapshotReused},
					{"fullAssetRefresh", diagnostics->authoring.fullAssetRefresh}}},
				{"preparation", {
					{"executionPath", diagnostics->preparation.executionPath}, {"estimatedWorkItems", diagnostics->preparation.estimatedWorkItems},
					{"ranges", diagnostics->preparation.rangeCount}, {"creates", diagnostics->preparation.creates}, {"removals", diagnostics->preparation.removals},
					{"updates", diagnostics->preparation.updates}, {"dependencyRebuilds", diagnostics->preparation.dependencyRebuilds},
					{"scratchActiveItems", diagnostics->preparation.scratchActiveItems}, {"scratchHighWatermark", diagnostics->preparation.scratchHighWatermark},
					{"scratchAllocations", diagnostics->preparation.scratchAllocations}, {"scratchReuses", diagnostics->preparation.scratchReuses},
					{"rejected", diagnostics->preparation.rejected}, {"failure", diagnostics->preparation.failure}}},
				{"extraction", {
					{"providerOrderGeneration", diagnostics->extraction.providerOrderGeneration}, {"totalBodies", diagnostics->extraction.totalBodies},
					{"activeBodies", diagnostics->extraction.activeBodies}, {"examinedBodies", diagnostics->extraction.examinedBodies},
					{"extractedBodies", diagnostics->extraction.extractedBodies}, {"convertedBodies", diagnostics->extraction.convertedBodies},
					{"changedBodies", diagnostics->extraction.changedBodies}, {"removedBodies", diagnostics->extraction.removedBodies},
					{"ranges", diagnostics->extraction.rangeCount}, {"fieldMask", diagnostics->extraction.fieldMask}, {"fullState", diagnostics->extraction.fullState},
					{"parallelReads", diagnostics->extraction.parallelReads}, {"serialFallback", diagnostics->extraction.serialFallback},
					{"eventDrain", diagnostics->extraction.eventDrain}}},
				{"stateStore", {
					{"generation", diagnostics->stateStore.storeGeneration}, {"providerOrderGeneration", diagnostics->stateStore.providerOrderGeneration},
					{"publications", diagnostics->stateStore.publicationCount}, {"rejectedBatches", diagnostics->stateStore.rejectedBatchCount},
					{"pages", diagnostics->stateStore.pageCount}, {"pagesReused", diagnostics->stateStore.pagesReused},
					{"pagesCopied", diagnostics->stateStore.pagesCopied}, {"bodies", diagnostics->stateStore.bodyCount},
					{"retainedBytes", diagnostics->stateStore.retainedBytes}}},
				{"publication", {
					{"preparedPackets", diagnostics->publication.preparedPackets}, {"acceptedPackets", diagnostics->publication.acceptedPackets},
					{"rejectedPackets", diagnostics->publication.rejectedPackets}, {"acknowledgedEntries", diagnostics->publication.acknowledgedEntries},
					{"emptyPacketsSkipped", diagnostics->publication.emptyPacketsSkipped}, {"entries", diagnostics->publication.currentEntries},
					{"chunks", diagnostics->publication.currentChunks}, {"pendingGroups", diagnostics->publication.pendingGroups},
					{"targetBoundary", diagnostics->publication.targetBoundary}, {"lastFailure", diagnostics->publication.lastFailure}}},
				{"rate", {
					{"aggregateIntervalFrames", diagnostics->rate.aggregateIntervalFrames}, {"lastAggregateTick", diagnostics->rate.lastAggregateTick},
					{"aggregateRefreshed", diagnostics->rate.aggregateRefreshed}, {"currentHealthPublished", diagnostics->rate.currentHealthPublished}}},
				{"timings", {
					{"authoringPreparationMs", diagnostics->timings.prepareMs}, {"providerCommitMs", diagnostics->timings.commitMs},
					{"simulationMs", diagnostics->timings.simulateMs}, {"extractionMs", diagnostics->timings.extractionMs},
					{"queryMs", diagnostics->timings.queryMs}, {"eventPublicationMs", diagnostics->timings.eventPublishMs},
					{"stateValidationMs", diagnostics->timings.stateValidationMs}, {"snapshotConstructionMs", diagnostics->timings.snapshotConstructionMs},
					{"transactionConstructionMs", diagnostics->timings.transactionConstructionMs}, {"publicationMs", diagnostics->timings.publishMs}}},
				{"quality", {
					{"settingsSchemaVersion", diagnostics->quality.settingsSchemaVersion}, {"settingsGeneration", diagnostics->quality.settingsGeneration},
					{"worldGeneration", diagnostics->quality.worldGeneration}, {"velocityIterations", diagnostics->quality.velocityIterations},
					{"positionIterations", diagnostics->quality.positionIterations}, {"applied", diagnostics->quality.applied}}},
				{"jobs", {
					{"submitted", diagnostics->jobs.submitted}, {"completed", diagnostics->jobs.completed}, {"rejected", diagnostics->jobs.rejected},
					{"batches", diagnostics->jobs.batchCount}, {"lockAcquisitions", diagnostics->jobs.lockAcquisitionCount}, {"pumpSubmissions", diagnostics->jobs.pumpSubmissionCount},
					{"barrierWaits", diagnostics->jobs.barrierWaitCount}, {"barrierWaitMicroseconds", diagnostics->jobs.barrierWaitMicroseconds},
					{"maximumBarrierWaitMicroseconds", diagnostics->jobs.maximumBarrierWaitMicroseconds}, {"jobExecutionMicroseconds", diagnostics->jobs.jobExecutionMicroseconds},
					{"queued", diagnostics->jobs.queued}, {"activePumps", diagnostics->jobs.activePumps}, {"maximumActivePumps", diagnostics->jobs.maximumActivePumps},
					{"workerLimit", diagnostics->jobs.workerLimit}, {"observedWorkerThreads", diagnostics->jobs.observedWorkerThreads}, {"idle", diagnostics->jobs.idle}}},
				{"stages", std::move(stages)},
				{"scheduler", std::move(scheduler)}
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
			std::optional<TerraEngine::UUID> targetId;
			if (request.arguments.contains("target"))
			{
				std::uint32_t ignoredComponentGeneration = 0U;
				if (const auto error = parseTarget(request.arguments["target"], targetId.emplace(), ignoredComponentGeneration)) return *error;
			}
			if ((request.arguments.contains("fromTick") && !request.arguments["fromTick"].is_number_unsigned())
				|| (request.arguments.contains("toTick") && !request.arguments["toTick"].is_number_unsigned()))
			{
				return reject(ErrorCategory::Validation, "fromTick and toTick must be unsigned tick values.");
			}
			const std::uint32_t fromTick = request.arguments.value("fromTick", 0U);
			const std::uint32_t toTick = request.arguments.value("toTick", std::numeric_limits<std::uint32_t>::max());
			if (fromTick > toTick) return reject(ErrorCategory::Validation, "fromTick must not be greater than toTick.");
			std::optional<std::uint8_t> collisionLayer;
			if (request.arguments.contains("collisionLayer"))
			{
				if (!request.arguments["collisionLayer"].is_number_unsigned()
					|| request.arguments["collisionLayer"].get<std::uint64_t>() >= kPhysicsCollisionLayerCount)
				{
					return reject(ErrorCategory::Validation, "collisionLayer must be between 0 and 31.");
				}
				collisionLayer = static_cast<std::uint8_t>(request.arguments["collisionLayer"].get<std::uint32_t>());
			}
			const PhysicsTick currentTick = tick;
			const PhysicsTick retainedTicks = physics.ConfiguredSettings().retainedResultTicks;
			const PhysicsTick firstRetainedTick = currentTick > retainedTicks ? currentTick - retainedTicks : 1U;
			// Only retained packets may be examined. This bounds a hostile 0..MAX
			// request to the same short history window configured for the world.
			const PhysicsTick effectiveFromTick = std::max<PhysicsTick>(fromTick, firstRetainedTick);
			const PhysicsTick effectiveToTick = std::min<PhysicsTick>(toTick, currentTick);
			const bool rangeBounded = effectiveFromTick != fromTick || effectiveToTick != toTick;
			nlohmann::json events = nlohmann::json::array();
			bool truncated = rangeBounded;
			bool overflow = false;
			if (effectiveFromTick <= effectiveToTick)
			{
				// Ascending ticks preserve publication order across packets; each packet
				// is already deterministically sorted at the protected publish boundary.
				for (PhysicsTick requestedTick = effectiveFromTick; requestedTick <= effectiveToTick; ++requestedTick)
				{
					const std::shared_ptr<const PhysicsEventPacket> packet = physics.ReadRetainedEventPacket(requestedTick);
					if (!packet || !packet->events) continue;
					overflow = overflow || packet->overflowed;
					for (const PhysicsEvent& event : *packet->events)
					{
						if (event.type != PhysicsEventType::Contact) continue;
						if (targetId && event.first.objectId != *targetId && event.second.objectId != *targetId) continue;
						if (collisionLayer && event.first.collisionLayer != *collisionLayer && event.second.collisionLayer != *collisionLayer) continue;
						if (events.size() >= limit) { truncated = true; break; }
						events.push_back({{"tick", event.tick}, {"first", TerraEngine::ToHexString(event.first.objectId)}, {"second", TerraEngine::ToHexString(event.second.objectId)}, {"point", PhysicsVectorJson(event.point)}, {"normal", PhysicsVectorJson(event.normal)}, {"impulse", event.impulse}});
					}
					if ((truncated && events.size() >= limit) || requestedTick == effectiveToTick) break;
				}
			}
			return succeed("Listed ordered published contacts.", PhysicsSnapshotResult(request, world, currentTick, {{"contacts", std::move(events)}, {"effectiveFromTick", effectiveFromTick}, {"effectiveToTick", effectiveToTick}}, truncated, overflow));
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
			// Paused simulation consumes commands only with the next explicit step.
			// Requiring the caller to say so prevents a request from appearing to have
			// changed the world while the editor is deliberately paused.
			const bool hasApplyOnNextStep = request.arguments.contains("applyOnNextStep");
			if (hasApplyOnNextStep && !request.arguments["applyOnNextStep"].is_boolean())
				return reject(ErrorCategory::Validation, "applyOnNextStep must be boolean when provided.");
			const bool applyOnNextStep = hasApplyOnNextStep && request.arguments["applyOnNextStep"].get<bool>();
			if (request.state.playState == "paused" && !applyOnNextStep)
				return reject(ErrorCategory::Conflict, "Paused physics commands require applyOnNextStep=true.");
			if (request.state.playState != "playing" && request.state.playState != "paused")
				return reject(ErrorCategory::Unavailable, "Runtime physics commands require playing or a paused explicit Next step.");
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
					if (operation == AutomationOperation("physics.body.apply_force"))
					{
						if (request.arguments.contains("applicationMode") && !request.arguments["applicationMode"].is_string())
							return rejectAfterReservation(ErrorCategory::Validation, "applicationMode must be Force or Acceleration.");
						const std::string applicationMode = request.arguments.value("applicationMode", std::string("Force"));
						const std::optional<PhysicsCommand> forceCommand = PhysicsAutomationProvider::TryCreateApplyForceCommand(
							applicationMode, target, *value, metadata);
						if (!forceCommand)
							return rejectAfterReservation(ErrorCategory::Validation, "applicationMode must be Force or Acceleration.");
						command = *forceCommand;
					}
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

	AutomationDispatchResult EditorAutomationService::ExecuteNavigationAutomationHandler(
		AutomationOperationId operation,
		std::string_view operationName,
		const AutomationRequestContext& request)
	{
		using namespace TerraEngine::Navigation;
		const auto reject = [](ErrorCategory category, std::string message) {
			return AutomationDispatchResult{false, category, std::move(message), nlohmann::json::object()};
		};
		const auto succeed = [](std::string message, nlohmann::json data) {
			return AutomationDispatchResult{true, ErrorCategory::Internal, std::move(message), std::move(data)};
		};
		const auto hasOnlyFields = [](const nlohmann::json& value, std::initializer_list<std::string_view> allowed) {
			if (!value.is_object()) return false;
			for (auto entry = value.begin(); entry != value.end(); ++entry)
			{
				if (std::find(allowed.begin(), allowed.end(), entry.key()) == allowed.end()) return false;
			}
			return true;
		};
		const auto readBoundedCount = [](const nlohmann::json& value, std::string_view key,
			std::size_t fallback, std::size_t maximum, std::size_t& count) {
			count = fallback;
			const auto found = value.find(std::string(key));
			if (found == value.end()) return true;
			if (!found->is_number_unsigned()) return false;
			const std::uint64_t candidate = found->get<std::uint64_t>();
			if (candidate == 0U || candidate > maximum) return false;
			count = static_cast<std::size_t>(candidate);
			return true;
		};
		const auto parseOwner = [](const nlohmann::json& value, NavigationOwnerHandle& owner) {
			if (!value.is_object()
				|| !value.contains("worldId") || !value["worldId"].is_number_unsigned()
				|| !value.contains("worldGeneration") || !value["worldGeneration"].is_number_unsigned()
				|| !value.contains("ownerId") || !value["ownerId"].is_number_unsigned()
				|| !value.contains("ownerGeneration") || !value["ownerGeneration"].is_number_unsigned()) return false;
			owner = {{value["worldId"].get<std::uint64_t>(), value["worldGeneration"].get<NavigationGeneration>()},
				{value["ownerId"].get<std::uint64_t>()}, value["ownerGeneration"].get<NavigationGeneration>()};
			return owner.IsValid();
		};
		const auto parseRequest = [&parseOwner](const nlohmann::json& value, NavigationRequestHandle& handle) {
			if (!value.is_object() || !value.contains("owner")
				|| !value.contains("slotIndex") || !value["slotIndex"].is_number_unsigned()
				|| !value.contains("slotGeneration") || !value["slotGeneration"].is_number_unsigned()) return false;
			const std::uint64_t index = value["slotIndex"].get<std::uint64_t>();
			if (index >= static_cast<std::uint64_t>(kInvalidNavigationSlot)) return false;
			NavigationOwnerHandle owner{};
			if (!parseOwner(value["owner"], owner)) return false;
			handle = {owner, static_cast<NavigationSlotIndex>(index), value["slotGeneration"].get<NavigationGeneration>()};
			return handle.IsValid();
		};
		const auto serializeOwner = [](const NavigationOwnerHandle& owner) {
			return nlohmann::json{{"worldId", owner.world.worldId.value}, {"worldGeneration", owner.world.worldGeneration},
				{"ownerId", owner.ownerId.value}, {"ownerGeneration", owner.ownerGeneration}};
		};
		const auto serializeRequest = [&serializeOwner](const NavigationRequestHandle& handle) {
			return nlohmann::json{{"owner", serializeOwner(handle.owner)}, {"slotIndex", handle.slotIndex},
				{"slotGeneration", handle.slotGeneration}};
		};
		const auto serializeLocation = [](const NavigationLocation& location) {
			return nlohmann::json{{"worldId", location.world.worldId.value}, {"worldGeneration", location.world.worldGeneration},
				{"position", Serializer::SerializeWorldPosition(location.position)}};
		};
		const auto parseAgent = [](const nlohmann::json& value, NavigationAgentIdentity& identity) {
			if(!value.is_object() || !value.contains("worldId") || !value.contains("worldGeneration")
				|| !value.contains("objectId") || !value.contains("lifetimeGeneration")
				|| !value.contains("attachmentGeneration") || !value.contains("runtimeSlot")
				|| !value.contains("runtimeSlotGeneration") || !value["objectId"].is_string()) return false;
			const auto objectId = TerraEngine::ParseUUID(value["objectId"].get<std::string>());
			if(!objectId || !value["worldId"].is_number_unsigned() || !value["worldGeneration"].is_number_unsigned()
				|| !value["lifetimeGeneration"].is_number_unsigned() || !value["attachmentGeneration"].is_number_unsigned()
				|| !value["runtimeSlot"].is_number_unsigned() || !value["runtimeSlotGeneration"].is_number_unsigned()) return false;
			const std::uint64_t slot = value["runtimeSlot"].get<std::uint64_t>();
			if(slot >= kInvalidNavigationSlot) return false;
			identity = {{value["worldId"].get<std::uint64_t>(), value["worldGeneration"].get<NavigationGeneration>()},
				{*objectId, value["lifetimeGeneration"].get<NavigationGeneration>()}, value["attachmentGeneration"].get<NavigationGeneration>(),
				static_cast<NavigationSlotIndex>(slot), value["runtimeSlotGeneration"].get<NavigationGeneration>()};
			return identity.IsValid();
		};
		const auto serializeAgent = [](const NavigationAgentIdentity& identity) {
			return nlohmann::json{{"worldId", identity.world.worldId.value}, {"worldGeneration", identity.world.worldGeneration},
				{"objectId", TerraEngine::ToHexString(identity.object.objectId)}, {"lifetimeGeneration", identity.object.lifetimeGeneration},
				{"attachmentGeneration", identity.attachmentGeneration}, {"runtimeSlot", identity.runtimeSlot},
				{"runtimeSlotGeneration", identity.runtimeSlotGeneration}};
		};

		if (!request.state.projectAvailable || request.state.projectGeneration == 0U)
			return reject(ErrorCategory::Unavailable, "Navigation automation requires an already-open project with a stable generation.");
		if (!request.arguments.is_object())
			return reject(ErrorCategory::Validation, "Navigation automation arguments must be an object.");
		if (request.arguments.contains("schemaVersion")
			&& (!request.arguments["schemaVersion"].is_number_unsigned()
				|| request.arguments["schemaVersion"].get<std::uint64_t>() != 1U))
			return reject(ErrorCategory::Validation, "Navigation automation supports only schemaVersion 1.");

		NavigationSystem& navigation = NavigationSystem::Instance();
		const std::shared_ptr<const NavigationWorldRoot> root = navigation.PublishedWorld();
		const auto requireWorld = [&root, &reject]() -> std::optional<AutomationDispatchResult> {
			if (!root || !root->world.IsValid())
				return reject(ErrorCategory::Unavailable, "No immutable navigation world is published for this project.");
			return std::nullopt;
		};
		const auto readCommandValues = [&reject](const nlohmann::json& value,
			NavigationGeneration& configuration, NavigationGeneration& intent,
			std::uint64_t& producer, std::uint64_t& correlation) -> std::optional<AutomationDispatchResult> {
			// Generation values have meaningful high words (runtime slots deliberately
			// use them), so never let a JSON default deduce a 32-bit conversion.
			configuration = std::uint64_t{0};
			intent = std::uint64_t{0};
			if(value.contains("expectedConfigurationRevision")) {
				if(!value["expectedConfigurationRevision"].is_number_unsigned())
					return reject(ErrorCategory::Validation, "expectedConfigurationRevision must be an unsigned 64-bit value.");
				configuration = value["expectedConfigurationRevision"].get<NavigationGeneration>();
			}
			if(value.contains("expectedIntentSequence")) {
				if(!value["expectedIntentSequence"].is_number_unsigned())
					return reject(ErrorCategory::Validation, "expectedIntentSequence must be an unsigned 64-bit value.");
				intent = value["expectedIntentSequence"].get<NavigationGeneration>();
			}
			if(!value.contains("producerOrderingToken") || !value["producerOrderingToken"].is_number_unsigned()
				|| !value.contains("correlationId") || !value["correlationId"].is_number_unsigned())
				return reject(ErrorCategory::Validation, "Agent commands require unsigned producerOrderingToken and correlationId.");
			producer = value["producerOrderingToken"].get<std::uint64_t>();
			correlation = value["correlationId"].get<std::uint64_t>();
			if(producer == 0U || correlation == 0U) return reject(ErrorCategory::Validation, "Agent command ordering and correlation values must be non-zero.");
			return std::nullopt;
		};

		if(operation == AutomationOperation("navigation.agent.bind")) {
			if(!hasOnlyFields(request.arguments, {"schemaVersion", "objectId"}) || !request.arguments.contains("objectId")
				|| !request.arguments["objectId"].is_string()) return reject(ErrorCategory::Validation, "navigation.agent.bind requires only a published object identifier.");
			if(const auto unavailable = requireWorld()) return *unavailable;
			const auto objectId = TerraEngine::ParseUUID(request.arguments["objectId"].get<std::string>());
			if(!objectId) return reject(ErrorCategory::Validation, "objectId must be a canonical navigation object identifier.");
			// Binding deliberately reads only the atomically published bridge. It must
			// never make an automation read mutate or scan live component storage.
			const auto resolved = NavigationAgentSystem::Instance().ResolvePublishedAgent(*objectId);
			if(!resolved || resolved->world != root->world)
				return reject(ErrorCategory::StaleTarget, "The published object has no active NavigationAgentComponent in this world generation.");
			const NavigationAgentIdentity identity = *resolved;
			return succeed("Bound the active generation-qualified navigation agent.", {{"schemaVersion", 1U}, {"agent", serializeAgent(identity)}});
		}

		if(operation == AutomationOperation("navigation.agent.destination.location")
			|| operation == AutomationOperation("navigation.agent.destination.object")
			|| operation == AutomationOperation("navigation.agent.stop")) {
			const bool isLocation = operation == AutomationOperation("navigation.agent.destination.location");
			const bool isObject = operation == AutomationOperation("navigation.agent.destination.object");
			if(!hasOnlyFields(request.arguments, isLocation ? std::initializer_list<std::string_view>{"schemaVersion", "agent", "location", "expectedConfigurationRevision", "expectedIntentSequence", "producerOrderingToken", "correlationId"}
				: isObject ? std::initializer_list<std::string_view>{"schemaVersion", "agent", "target", "expectedConfigurationRevision", "expectedIntentSequence", "producerOrderingToken", "correlationId"}
				: std::initializer_list<std::string_view>{"schemaVersion", "agent", "expectedConfigurationRevision", "expectedIntentSequence", "producerOrderingToken", "correlationId"}))
				return reject(ErrorCategory::Validation, "Navigation agent command contains an unsupported field.");
			if(const auto unavailable = requireWorld()) return *unavailable;
			NavigationAgentIdentity identity{};
			if(!request.arguments.contains("agent") || !parseAgent(request.arguments["agent"], identity) || identity.world != root->world)
				return reject(ErrorCategory::StaleTarget, "The agent identity is malformed or belongs to another world generation.");
			NavigationGeneration configuration = 0U, intent = 0U;
			std::uint64_t producer = 0U, correlation = 0U;
			if(const auto error = readCommandValues(request.arguments, configuration, intent, producer, correlation)) return *error;
			NavigationAgentReceipt receipt{};
			if(isLocation) {
				std::string error;
				if(!request.arguments.contains("location") || !request.arguments["location"].is_object()
					|| !request.arguments["location"].contains("worldId") || !request.arguments["location"]["worldId"].is_number_unsigned()
					|| !request.arguments["location"].contains("worldGeneration") || !request.arguments["location"]["worldGeneration"].is_number_unsigned())
					return reject(ErrorCategory::Validation, "Location destination requires unsigned world identity fields.");
				const auto position = Serializer::DeserializeWorldPosition(request.arguments["location"].value("position", nlohmann::json{}), &error);
				if(!position || request.arguments["location"]["worldId"].get<std::uint64_t>() != root->world.worldId.value
					|| request.arguments["location"]["worldGeneration"].get<NavigationGeneration>() != root->world.worldGeneration)
					return reject(ErrorCategory::Validation, "Location must be canonical and belong to the current navigation world.");
				receipt = NavigationAgentSystem::Instance().QueueLocationDestination(identity, {root->world, *position}, configuration, intent, producer, correlation);
			} else if(isObject) {
				if(!request.arguments.contains("target") || !request.arguments["target"].is_object() || !request.arguments["target"].contains("objectId")
					|| !request.arguments["target"]["objectId"].is_string() || !request.arguments["target"].contains("lifetimeGeneration")
					|| !request.arguments["target"]["lifetimeGeneration"].is_number_unsigned()) return reject(ErrorCategory::Validation, "Object destination requires objectId and lifetimeGeneration.");
				const auto targetId = TerraEngine::ParseUUID(request.arguments["target"]["objectId"].get<std::string>());
				if(!targetId || request.arguments["target"]["lifetimeGeneration"].get<NavigationGeneration>() == 0U)
					return reject(ErrorCategory::Validation, "Object destination identity is invalid.");
				receipt = NavigationAgentSystem::Instance().QueueObjectDestination(identity, {*targetId, request.arguments["target"]["lifetimeGeneration"].get<NavigationGeneration>()}, configuration, intent, producer, correlation);
			} else receipt = NavigationAgentSystem::Instance().QueueStop(identity, configuration, intent, producer, correlation);
			const bool queued = receipt.state == NavigationAgentReceiptState::Queued;
			return succeed(queued ? "Queued a copied navigation-agent command; protected capture decides final admission."
				: "Navigation-agent command was rejected without changing the existing intent.", {{"schemaVersion", 1U}, {"commitReceipt", {{"agent", serializeAgent(receipt.identity)}, {"state", std::string(ToString(receipt.state))}, {"outcome", std::string(ToString(receipt.outcome))}, {"reason", std::string(ToString(receipt.reason))}, {"correlationId", receipt.correlationId}, {"admittedSequence", receipt.admittedSequence}}}});
		}

		if(operation == AutomationOperation("navigation.agent.receipt") || operation == AutomationOperation("navigation.agent.status") || operation == AutomationOperation("navigation.agent.guidance")) {
			if(!hasOnlyFields(request.arguments, operation == AutomationOperation("navigation.agent.receipt")
				? std::initializer_list<std::string_view>{"schemaVersion", "agent", "correlationId"} : std::initializer_list<std::string_view>{"schemaVersion", "agent"}))
				return reject(ErrorCategory::Validation, "Navigation agent read contains an unsupported field.");
			NavigationAgentIdentity identity{};
			if(!request.arguments.contains("agent") || !parseAgent(request.arguments["agent"], identity)) return reject(ErrorCategory::Validation, "A complete generation-qualified agent identity is required.");
			NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
			if(operation == AutomationOperation("navigation.agent.receipt")) {
				if(!request.arguments.contains("correlationId") || !request.arguments["correlationId"].is_number_unsigned()) return reject(ErrorCategory::Validation, "Receipt reads require correlationId.");
				const std::uint64_t correlation = request.arguments["correlationId"].get<std::uint64_t>();
				if(correlation == 0U) return reject(ErrorCategory::Validation, "Receipt correlationId must be non-zero.");
				const auto receipt = agents.ReadReceipt(identity, correlation);
				// Receipt retention is bounded. Absence is an observable expiration, not
				// an ambiguous stale-target error that would hide the caller's command ID.
				const NavigationAgentReceipt visible = receipt.value_or(NavigationAgentReceipt{
					identity, NavigationAgentReceiptState::Expired, NavigationOutcome::Expired,
					NavigationReason::Expired, correlation, 0U});
				const NavigationOutcome outcome = visible.state == NavigationAgentReceiptState::Expired
					? NavigationOutcome::Expired : visible.outcome;
				const NavigationReason reason = visible.state == NavigationAgentReceiptState::Expired
					? NavigationReason::Expired : visible.reason;
				return succeed("Read the bounded navigation-agent command receipt.", {{"schemaVersion", 1U}, {"status", {{"state", std::string(ToString(visible.state))}, {"outcome", std::string(ToString(outcome))}, {"reason", std::string(ToString(reason))}, {"correlationId", visible.correlationId}, {"admittedSequence", visible.admittedSequence}}}});
			}
			if(operation == AutomationOperation("navigation.agent.status")) {
				const auto status = agents.ReadStatus(identity);
				return succeed("Read copied navigation-agent runtime status.", {{"schemaVersion", 1U}, {"status", {{"state", std::string(ToString(status.state))}, {"outcome", std::string(ToString(status.outcome))}, {"reason", std::string(ToString(status.reason))}, {"configurationRevision", status.configurationRevision}, {"intentSequence", status.intentSequence}, {"publicationRevision", status.publicationRevision}}}});
			}
			const auto guidance = agents.ReadGuidance(identity);
			const nlohmann::json destination = guidance.destination.kind == NavigationDestinationKind::Object
				? nlohmann::json{{"kind", "object"}, {"objectId", TerraEngine::ToHexString(guidance.destination.object.objectId)}, {"lifetimeGeneration", guidance.destination.object.lifetimeGeneration}}
				: nlohmann::json{{"kind", "location"}, {"location", serializeLocation(guidance.destination.location)}};
			return succeed("Read copied advisory navigation-agent guidance.", {{"schemaVersion", 1U}, {"guidance", {
				{"identity", serializeAgent(guidance.identity)}, {"usable", guidance.IsUsable()}, {"state", std::string(ToString(guidance.state))},
				{"outcome", std::string(ToString(guidance.outcome))}, {"reason", std::string(ToString(guidance.reason))},
				{"configurationRevision", guidance.configurationRevision}, {"intentSequence", guidance.intentSequence},
				{"publicationGeneration", guidance.publicationGeneration}, {"routeGeneration", guidance.routeGeneration},
				{"observation", {{"sceneGeneration", guidance.observation.sceneGeneration}, {"sourceTransformRevision", guidance.observation.sourceTransformRevision}, {"destinationTransformRevision", guidance.observation.destinationTransformRevision}, {"terrainPublicationGeneration", guidance.observation.terrainPublicationGeneration}, {"worldPublicationGeneration", guidance.observation.worldPublicationGeneration}, {"sourceIndexRevision", guidance.observation.sourceIndexRevision}}},
				{"destination", destination}, {"nextWaypoint", serializeLocation(guidance.nextWaypoint)}, {"remainingDistanceMetres", guidance.remainingDistanceMetres}, {"direction", {guidance.suggestedDirection.x, guidance.suggestedDirection.y, guidance.suggestedDirection.z}}}}});
		}

		if (operation == AutomationOperation("navigation.settings.get"))
		{
			if (!hasOnlyFields(request.arguments, {"schemaVersion"}))
				return reject(ErrorCategory::Validation, "navigation.settings.get accepts only schemaVersion.");
			// NavigationSystem deliberately exposes no mutable settings reference. The
			// defaults are copied into this result, so automation cannot alter runtime limits.
			return succeed("Read the immutable navigation settings reference profile.", {
				{"schemaVersion", 1U}, {"snapshot", Serializer::SerializeNavigationSettings(NavigationSettings::Defaults())},
				{"configuredRuntimeSettingsAvailable", false}, {"source", "frozen_defaults"}});
		}

		if (operation == AutomationOperation("navigation.diagnostics.get"))
		{
			if (!hasOnlyFields(request.arguments, {"schemaVersion", "maxItems"}))
				return reject(ErrorCategory::Validation, "navigation.diagnostics.get accepts only schemaVersion and maxItems.");
			std::size_t maximumRecords = 256U;
			if (!readBoundedCount(request.arguments, "maxItems", 256U, 1024U, maximumRecords))
				return reject(ErrorCategory::Validation, "maxItems must be an unsigned value from 1 through 1024.");
			const std::shared_ptr<const NavigationDiagnosticSnapshot> diagnostics = navigation.PublishedDiagnostics();
			nlohmann::json records = nlohmann::json::array();
			if (diagnostics)
			{
				for (std::size_t index = 0U; index < diagnostics->records.size() && index < maximumRecords; ++index)
				{
					const NavigationDiagnosticRecord& record = diagnostics->records[index];
					records.push_back({{"severity", static_cast<std::uint32_t>(record.severity)},
						{"measurementReason", static_cast<std::uint32_t>(record.measurementReason)},
						{"publishedGeneration", record.publishedGeneration}, {"measuredNanoseconds", record.measuredNanoseconds},
						{"code", record.code}, {"message", record.message}});
				}
			}
			return succeed("Read copied immutable navigation diagnostics.", {{"schemaVersion", 1U}, {"snapshot", {
				{"revision", diagnostics ? diagnostics->revision : 0U}, {"records", std::move(records)},
				{"truncated", diagnostics && diagnostics->records.size() > maximumRecords},
				{"droppedRecordCount", diagnostics ? diagnostics->droppedRecordCount : 0U},
				{"droppedByteCount", diagnostics ? diagnostics->droppedByteCount : 0U},
				{"retainedByteCount", diagnostics ? diagnostics->retainedByteCount : 0U},
				{"queueDepth", diagnostics ? diagnostics->queueDepth : 0U}, {"staleDropCount", diagnostics ? diagnostics->staleDropCount : 0U}}}});
		}

		if (operation == AutomationOperation("navigation.route.request"))
		{
			// Route requests carry a complete identity-free profile value. Keep the
			// allow-list aligned with the required field below so valid public
			// requests reach NavigationSystem instead of being rejected as unknown.
			if (!hasOnlyFields(request.arguments, {"schemaVersion", "owner", "start", "destination", "profile", "profileId", "allowPartial", "maxItems"}))
				return reject(ErrorCategory::Validation, "navigation.route.request contains an unsupported field.");
			if (const auto unavailable = requireWorld()) return *unavailable;
			if (!request.arguments.contains("owner") || !request.arguments.contains("start")
				|| !request.arguments.contains("destination") || !request.arguments.contains("profile"))
				return reject(ErrorCategory::Validation, "A route request requires owner, start, destination, and an explicit profile value.");
			NavigationOwnerHandle owner{};
			if (!parseOwner(request.arguments["owner"], owner) || owner.world != root->world)
				return reject(ErrorCategory::Conflict, "The route owner does not belong to the published navigation world.");
			std::string positionError;
			const auto start = Serializer::DeserializeWorldPosition(request.arguments["start"], &positionError);
			if (!start) return reject(ErrorCategory::Validation, "start is not a canonical finite world position: " + positionError);
			const auto destination = Serializer::DeserializeWorldPosition(request.arguments["destination"], &positionError);
			if (!destination) return reject(ErrorCategory::Validation, "destination is not a canonical finite world position: " + positionError);
			std::string profileError;
			const auto profile = Serializer::DeserializeNavigationProfile(request.arguments["profile"], &profileError);
			if (!profile)
				return reject(ErrorCategory::Validation, "profile must be a complete identity-free navigation profile: " + profileError);
			if (request.arguments.contains("allowPartial") && !request.arguments["allowPartial"].is_boolean())
				return reject(ErrorCategory::Validation, "allowPartial must be boolean when supplied.");
			std::size_t requestedPoints = 4096U;
			if (!readBoundedCount(request.arguments, "maxItems", 4096U, 4096U, requestedPoints))
				return reject(ErrorCategory::Validation, "maxItems must be an unsigned value from 1 through 4096.");
			Ground::GroundRouteQuery::Request routeRequest;
			routeRequest.owner = owner;
			routeRequest.start = {root->world, *start};
			routeRequest.destination = {root->world, *destination};
			routeRequest.profile = *profile;
			routeRequest.allowPartial = request.arguments.value("allowPartial", false);
			routeRequest.maxExpansions = static_cast<std::uint32_t>(std::min<std::size_t>(requestedPoints * 8U, 32768U));
			if (!ValidateNavigationLocation(routeRequest.start).Succeeded() || !ValidateNavigationLocation(routeRequest.destination).Succeeded())
				return reject(ErrorCategory::Validation, "Route endpoints must be canonical finite locations in the owner world.");
			// The system captures immutable published coverage and private shard data;
			// automation supplies endpoints and policy, never a synthetic topology.
			const NavigationRequestHandle handle = navigation.SubmitOneShotRoute(routeRequest);
			if (!handle.IsValid()) return reject(ErrorCategory::QueueFull, "Navigation route admission rejected the copied request.");
			return succeed("Submitted a copied provider-neutral navigation route request.", {{"schemaVersion", 1U}, {"commitReceipt", {
				{"accepted", true}, {"operation", std::string(operationName)}, {"request", serializeRequest(handle)},
				{"worldGeneration", root->world.worldGeneration}, {"maxItems", requestedPoints}}}});
		}

		if (operation == AutomationOperation("navigation.route.read") || operation == AutomationOperation("navigation.route.cancel"))
		{
			if (!hasOnlyFields(request.arguments, {"schemaVersion", "request", "maxItems"}))
				return reject(ErrorCategory::Validation, "Navigation route read/cancel accepts only schemaVersion, request, and maxItems.");
			if (const auto unavailable = requireWorld()) return *unavailable;
			if (!request.arguments.contains("request")) return reject(ErrorCategory::Validation, "Navigation route read/cancel requires a generation-qualified request object.");
			NavigationRequestHandle handle{};
			if (!parseRequest(request.arguments["request"], handle) || handle.owner.world != root->world)
				return reject(ErrorCategory::Conflict, "The navigation request handle is malformed or belongs to another world.");
			if (operation == AutomationOperation("navigation.route.cancel"))
			{
				if (!navigation.CancelRequest(handle)) return reject(ErrorCategory::StaleTarget, "The navigation request is no longer cancelable for this owner generation.");
				return succeed("Requested cooperative cancellation of the copied navigation route.", {{"schemaVersion", 1U}, {"commitReceipt", {
					{"accepted", true}, {"request", serializeRequest(handle)}, {"worldGeneration", root->world.worldGeneration}}}});
			}
			std::size_t maximumPoints = 1024U;
			if (!readBoundedCount(request.arguments, "maxItems", 1024U, 4096U, maximumPoints))
				return reject(ErrorCategory::Validation, "maxItems must be an unsigned value from 1 through 4096.");
			const NavigationStoreReadResult<NavigationRequestRecord> read = navigation.ReadRequest(handle);
			if (!read.HasRecord()) return reject(ErrorCategory::StaleTarget, "The navigation request handle is stale, expired, or not owned by this world.");
			const NavigationRequestRecord& record = *read.record;
			nlohmann::json points = nlohmann::json::array();
			const NavigationRouteHandle routeHandle = navigation.PublishedRouteForRequest(handle);
			const Ground::GroundRouteQuery::RetainedRoute route = navigation.ReadRoute(routeHandle);
			for (std::size_t index = 0U; index < route.points.size() && index < maximumPoints; ++index)
				points.push_back(serializeLocation(route.points[index]));
			return succeed("Read copied navigation request and route state.", {{"schemaVersion", 1U}, {"route", {
				{"request", serializeRequest(handle)}, {"status", std::string(ToString(read.status))},
				{"lifecycle", std::string(ToString(record.lifecycle))}, {"outcome", std::string(ToString(record.outcome))},
				{"reason", std::string(ToString(record.reason))}, {"start", serializeLocation(record.start)},
				{"destination", serializeLocation(record.destination)}, {"routeValid", route.IsValid()},
				{"points", std::move(points)}, {"truncated", route.points.size() > maximumPoints}}}});
		}

		if (operation == AutomationOperation("navigation.cache.prepare") || operation == AutomationOperation("navigation.cache.status"))
		{
			const bool isPrepare = operation == AutomationOperation("navigation.cache.prepare");
			if (!hasOnlyFields(request.arguments, isPrepare ? std::initializer_list<std::string_view>{"schemaVersion"}
				: std::initializer_list<std::string_view>{"schemaVersion", "jobId"}))
				return reject(ErrorCategory::Validation, "Navigation cache operation contains an unsupported field.");
			if (!isPrepare && (!request.arguments.contains("jobId") || !request.arguments["jobId"].is_string()
				|| request.arguments["jobId"].get<std::string>().empty() || request.arguments["jobId"].get<std::string>().size() > 128U))
				return reject(ErrorCategory::Validation, "navigation.cache.status requires a bounded non-empty jobId.");
			// No cache-builder service is currently registered with NavigationSystem.
			// Returning unavailable is deliberate: creating a synthetic job would claim
			// progress that no scheduler-owned cache worker can actually perform.
			return reject(ErrorCategory::Unavailable, "Navigation cache preparation is unavailable because no cache builder is registered.");
		}

		return reject(ErrorCategory::UnsupportedCapability, "Navigation capability has no bound implementation.");
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
			// An explicit MCP open is an authoring reread boundary. Do not reuse an
			// earlier document just because an offline tool preserved its filename.
			const TerraEngine::UUID sceneId = SceneManager::Instance().ReloadScene(assetRelativePath);
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
		if (operation == AutomationOperation("asset.content.verify")) return HandleAssetContentVerify(arguments, requestId);
		if (operation == AutomationOperation("asset.import")) return HandleAssetImport(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.settings.get")) return HandleVirtualGeometrySettingsGet(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.settings.set")) return HandleVirtualGeometrySettingsSet(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.settings.reset")) return HandleVirtualGeometrySettingsReset(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.rebuild")) return HandleVirtualGeometryRebuild(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.inspect")) return HandleVirtualGeometryInspect(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.codec.inspect")
			|| operation == AutomationOperation("asset.virtualGeometry.cache.inspect")
			|| operation == AutomationOperation("asset.virtualGeometry.migration.inspect")
			|| operation == AutomationOperation("asset.virtualGeometry.repair.plan"))
			return HandleVirtualGeometryInspect(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.repair.execute")) return HandleVirtualGeometryRebuild(arguments, requestId);
		if (operation == AutomationOperation("asset.virtualGeometry.benchmark.status"))
			return BuildError(requestId, ErrorCategory::Unavailable, "Benchmark evidence requires an installed runner and authoritative measured capture.");
		if (operation == AutomationOperation("asset.virtualGeometry.benchmark.capture"))
			return BuildError(requestId, ErrorCategory::Unavailable, "Benchmark capture requires an installed runner and live adapter/readback.");
		if (operation == AutomationOperation("asset.prepare_delete")) return HandleAssetPrepareDelete(arguments, requestId);
		if (operation == AutomationOperation("export.validate")) return HandleExportValidate(arguments, requestId);
		if (operation == AutomationOperation("export.start")) return HandleExportStart(arguments, requestId);
		if (operation == AutomationOperation("export.cancel")) return HandleExportCancel(arguments, requestId);
		if (operation == AutomationOperation("game_view.capture")) return HandleGameViewCapture(arguments, requestId);
		if (operation == AutomationOperation("camera.capture_object")) return HandleCameraObjectCapture(arguments, requestId);
		if (operation == AutomationOperation("camera.capture_custom")) return HandleCustomCameraCapture(arguments, requestId);
		if (operation == AutomationOperation("renderer.efficiency.stats")) return HandleRendererEfficiency(0, arguments, requestId);
		if (operation == AutomationOperation("renderer.efficiency.validation.set")) return HandleRendererEfficiency(1, arguments, requestId);
		if (operation == AutomationOperation("renderer.efficiency.validation.reset")) return HandleRendererEfficiency(2, arguments, requestId);
		if (operation == AutomationOperation("renderer.efficiency.validation.start")) return HandleRendererEfficiency(3, arguments, requestId);
		if (operation == AutomationOperation("renderer.efficiency.validation.status")) return HandleRendererEfficiency(4, arguments, requestId);
		if (operation == AutomationOperation("renderer.efficiency.validation.stop")) return HandleRendererEfficiency(5, arguments, requestId);
		if (operation == AutomationOperation("renderer.virtualGeometry.stats")) return HandleRendererVirtualGeometryStats(arguments, requestId);
		if (operation == AutomationOperation("renderer.virtualGeometry.capabilityEvidence")) return HandleRendererVirtualGeometryCapabilityEvidence(arguments, requestId);
		if (operation == AutomationOperation("renderer.virtualGeometry.validationRoute.set")) return HandleRendererVirtualGeometryValidationRouteSet(arguments, requestId);
		if (operation == AutomationOperation("renderer.virtualGeometry.validationRoute.reset")) return HandleRendererVirtualGeometryValidationRouteReset(arguments, requestId);
		if (operation == AutomationOperation("renderer.virtualGeometry.authoritativePick")) return HandleRendererVirtualGeometryAuthoritativePick(arguments, requestId);
		if (operation == AutomationOperation("renderer.validation.sparseTransform.start")) return HandleRendererSparseTransformStart(arguments, requestId);
		if (operation == AutomationOperation("renderer.validation.sparseTransform.status")) return HandleRendererSparseTransformStatus(arguments, requestId);
		if (operation == AutomationOperation("renderer.validation.sparseTransform.stop")) return HandleRendererSparseTransformStop(arguments, requestId);
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
				state = playStateData();
				const ProjectReadinessState readiness = editorContext->projectManager->GetReadinessState();
				state["readiness"] = {
					{"simulationEssentialReady", readiness.simulationEssentialReady},
					{"editorMetadataDeferred", readiness.editorMetadataDeferred},
					{"sceneSource", readiness.sceneReadinessSource}
				};
				return BuildSuccess(requestId, "Entered Play Mode with a complete authored scene.", std::move(state));
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
				const MainEditorSubsystem::PlayModeRestorationResult& restoration = mainEditor->GetLastPlayModeRestorationResult();
				nlohmann::json partialFailures = nlohmann::json::array();
				if (!restoration.completed)
				{
					partialFailures.push_back(restoration.reason.empty() ? "authored_restore_incomplete" : restoration.reason);
				}
				// Preserve the original completed/partialFailures fields and append
				// protected-boundary, generation, and authored-state facts for clients
				// that need to distinguish a stale packet from a successful restore.
				state["restoration"] = {
					{"completed", restoration.completed},
					{"partialFailures", std::move(partialFailures)},
					{"boundary", restoration.boundary},
					{"preparedGeneration", restoration.preparedGeneration},
					{"publishedGeneration", restoration.publishedGeneration},
					{"stale", restoration.stale},
					{"cancelled", restoration.cancelled},
					{"selectionRestored", restoration.selectionRestored},
					{"dirtyStateRestored", restoration.dirtyStateRestored},
					{"prefabStatePreserved", restoration.prefabStatePreserved}
				};
				state["readiness"] = {{"source", "authored_bulk_packet"}, {"snapshotCaptured", restoration.snapshotCaptured}};
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
					(void)jobService.CompleteSucceeded(jobId, {
						{"assemblyPath", status.activeAssemblyPath.generic_string()},
						{"attachableTypeCount", status.attachableTypes.size()},
						{"forcedBuild", true},
						{"readiness", BuildManagedReadinessEvidence(status, true)}
					});
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
				// The stop job owns only journal finalization and its bounded index
				// validation. A catalog refresh or selected-frame request is unrelated
				// work and must not hold this durable completion hostage.
				if (profiler.IsPersisting())
				{
					(void)jobService.SetState(jobId, AutomationJobState::Running, "saving_capture");
					(void)jobService.PublishProgress(jobId, 0.75, "saving_capture");
				}
				else
				{
					// Persistence publishes the exact saved path through the profiler
					// service. Do not infer it from a before/after list delta: project-open
					// discovery can finish during recording and make an older capture look new.
					const std::optional<EditorProfilerCaptureArtifact> created = profiler.GetLastSavedCaptureArtifact();
					if (created.has_value())
					{
						(void)jobService.AddArtifact(jobId, {created->artifactId, "profiler_capture", {}, "application/json", {
							{"name", created->artifact.fileName},
							{"frameCount", created->artifact.frameCount},
							{"status", created->artifact.status}
						}});
						(void)jobService.CompleteSucceeded(jobId, {{"artifactId", created->artifactId}});
						activeProfilerCaptureJobId.clear();
					}
					else if (!profiler.GetLastError().empty())
					{
						(void)jobService.CompleteFailed(jobId, {{"error", profiler.GetLastError()}});
						activeProfilerCaptureJobId.clear();
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
			if (const auto active = activeExportOperations.find(jobId); active != activeExportOperations.end()) {
				const auto disposition = active->second.controller->RequestCancellation(active->second.operationId);
				if (disposition == ExportCancellationDisposition::Accepted) (void)jobService.RequestCancellation(jobId);
				return BuildSuccess(requestId, "Observed export cancellation disposition.", {{"job", SerializeAutomationJob(*jobService.Get(jobId))},
					{"cancellationDisposition", disposition == ExportCancellationDisposition::Accepted ? "accepted" : disposition == ExportCancellationDisposition::TooLate ? "too_late" : "already_terminal"}});
			}
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
			const auto transactionStarted = std::chrono::steady_clock::now();
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
			const auto preparationFinished = std::chrono::steady_clock::now();

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
			const auto commitFinished = std::chrono::steady_clock::now();
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

			const bool typedCanonicalHistory = std::all_of(
				prepared.transaction.writeSet.operations.begin(),
				prepared.transaction.writeSet.operations.end(),
				[](const TerraEngine::Scene::TransactionWriteOperation& write) {
					return write.kind == TerraEngine::Scene::TransactionWriteKind::SetWorldTransform
						|| write.kind == TerraEngine::Scene::TransactionWriteKind::TranslateWorld;
				});
			// Canonical transform history replays typed operations through the mutation
			// queue. That preserves exact cells and makes a rejected replay atomic.
			const bool recordedTypedCanonicalHistory = typedCanonicalHistory
				&& UndoRedoSystem::Instance().RecordCommittedTransaction(prepared.transaction, commit);

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

			const std::string historyId = recordedTypedCanonicalHistory
				? prepared.transaction.id
				: "history-" + transactionId;
			if (!recordedTypedCanonicalHistory)
			{
				UndoRedoSystem::Instance().PushTransactionGroup(
					arguments.value("label", std::string("Automation transaction")),
					std::move(commands), historyId);
			}
			if (prepared.selectionTarget && setSelectedEntity)
			{
				if (const std::optional<Object> selection = resolveCommittedRef(*prepared.selectionTarget)) setSelectedEntity(selection->EntityId());
			}
			SceneManager::Instance().ForceMarkActiveSceneDirty();
			const auto transactionFinished = std::chrono::steady_clock::now();
			const auto millisecondsBetween = [](const auto begin, const auto end) {
				return std::chrono::duration<double, std::milli>(end - begin).count();
			};
			return BuildSuccess(requestId, "Committed automation transaction.", {
				{"transactionId", transactionId}, {"historyId", historyId}, {"visibilityBoundary", "post_editor_automation_flush"},
				{"affectedTargets", affectedTargets}, {"appliedOperationCount", arguments["operations"].size()}, {"inverseDeltaCount", commit.inverseDeltas.size()},
				{"timings", {
					{"preparationMs", millisecondsBetween(transactionStarted, preparationFinished)},
					{"commitMs", millisecondsBetween(preparationFinished, commitFinished)},
					{"historyAndPublicationMs", millisecondsBetween(commitFinished, transactionFinished)},
					{"totalMs", millisecondsBetween(transactionStarted, transactionFinished)}
				}}
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
		if(operation == AutomationOperation("profiler.window.visibility.set"))
		{
			if(!arguments.contains("visible") || !arguments["visible"].is_boolean())
				return BuildError(requestId, ErrorCategory::Validation,
					"Profiler window visibility requires a Boolean visible value.");
			const std::shared_ptr<ProfilerWindow> window = mainEditor != nullptr
				? mainEditor->GetWindow<ProfilerWindow>() : nullptr;
			if(!window)
				return BuildError(requestId, ErrorCategory::Unavailable,
					"Profiler window is unavailable.");
			const bool visible = arguments["visible"].get<bool>();
			window->SetVisible(visible);
			if (!visible && editorContext != nullptr && editorContext->editorProfiler != nullptr)
			{
				// A hidden window must not leak its last open-frame CPU sample into
				// the closed side of the installed idle-overhead A/B measurement.
				editorContext->editorProfiler->ClearProfilerWindowDrawTiming();
			}
			// EditorSystem publishes this timing immediately after its GLFW/ImGui
			// frame boundary. The profiler service owns the typed atomic snapshot so
			// this automation layer does not depend directly on the ImGui runtime.
			const EditorProfilerFrameTiming frameTiming = editorContext != nullptr
				&& editorContext->editorProfiler != nullptr
				? editorContext->editorProfiler->GetEditorFrameTiming()
				: EditorProfilerFrameTiming{};
			const TerraEngine::Profiling::ProfilerLatestEngineFrameTiming engineTiming =
				TerraEngine::Profiling::Profiler::Instance().GetLatestEngineFrameTiming();
			return BuildSuccess(requestId, "Updated profiler window visibility.", {
				{"visible", window->IsVisible()},
				{"engineFrameIndex", engineTiming.frameIndex},
				{"engineCpuTimeMs", engineTiming.durationMs},
				{"engineCpuTimeValid", engineTiming.valid},
				{"editorFrameIndex", frameTiming.frameIndex},
				{"editorFrameTimeMs", frameTiming.frameTimeMs},
				{"profilerWindowDrawFrameIndex", frameTiming.profilerWindowDrawFrameIndex},
				{"profilerWindowDrawMs", frameTiming.profilerWindowDrawMs},
				{"profilerWindowDrawValid", frameTiming.profilerWindowDrawValid},
				{"imguiRenderFrameIndex", frameTiming.imguiRenderFrameIndex},
				{"imguiRenderMs", frameTiming.imguiRenderMs},
				{"imguiRenderValid", frameTiming.imguiRenderValid},
				{"presentUiFrameIndex", frameTiming.presentUiFrameIndex},
				{"presentUiMs", frameTiming.presentUiMs},
				{"presentUiValid", frameTiming.presentUiValid}
			});
		}
		if (IsOperationOneOf(operation, {
			AutomationOperation("prefab.instantiate"), AutomationOperation("prefab.instantiate_many"),
			AutomationOperation("prefab.overrides.inspect"),
			AutomationOperation("prefab.overrides.apply"), AutomationOperation("prefab.overrides.revert"),
			AutomationOperation("prefab.edit.open"), AutomationOperation("prefab.edit.close")}))
		{
			PrefabAuthoringService::Result result;
			if (operation == AutomationOperation("prefab.instantiate")
				|| operation == AutomationOperation("prefab.instantiate_many"))
			{
				const bool instantiateMany =
					operation == AutomationOperation("prefab.instantiate_many");
				PrefabAuthoringService::BulkInstantiationRequest bulkRequest;
				if (instantiateMany)
				{
					if (!arguments.contains("count") || !arguments["count"].is_number_integer())
					{
						return BuildError(requestId, ErrorCategory::Validation,
							"prefab.instantiate_many count must be an integer.");
					}
					const std::int64_t requestedCount = arguments["count"].get<std::int64_t>();
					if (requestedCount < 1 || requestedCount > 4096)
					{
						return BuildError(requestId, ErrorCategory::Validation,
							"prefab.instantiate_many count must be between 1 and 4096.");
					}
					bulkRequest.count = static_cast<std::uint32_t>(requestedCount);
					bulkRequest.namePrefix = arguments.value("namePrefix", std::string{});
					bulkRequest.spacing = arguments.value("spacing", 0.0f);
					if (arguments.contains("requestedAnimationMode"))
					{
						if (!arguments["requestedAnimationMode"].is_number_integer())
						{
							return BuildError(requestId, ErrorCategory::Validation,
								"prefab.instantiate_many requestedAnimationMode must be an integer.");
						}
						const std::int64_t requestedMode =
							arguments["requestedAnimationMode"].get<std::int64_t>();
						if (requestedMode < 0 || requestedMode > 3)
						{
							return BuildError(requestId, ErrorCategory::Validation,
								"prefab.instantiate_many requestedAnimationMode must be between 0 and 3.");
						}
						bulkRequest.requestedAnimationMode =
							static_cast<TerraEngine::Animation::AnimationExecutionPolicy>(
								requestedMode);
					}
					if (arguments.contains("defaultClipAssetIds"))
					{
						const nlohmann::json& clipIds = arguments["defaultClipAssetIds"];
						if (!clipIds.is_array() || clipIds.empty()
							|| clipIds.size() > static_cast<std::size_t>(requestedCount))
						{
							return BuildError(requestId, ErrorCategory::Validation,
								"prefab.instantiate_many defaultClipAssetIds must contain between 1 and count UUIDs.");
						}
						for (const nlohmann::json& value : clipIds)
						{
							if (!value.is_string())
							{
								return BuildError(requestId, ErrorCategory::Validation,
									"prefab.instantiate_many defaultClipAssetIds must contain UUID strings.");
							}
							const std::optional<TerraEngine::UUID> parsed =
								TerraEngine::ParseUUID(value.get<std::string>());
							if (!parsed.has_value() || *parsed == TerraEngine::EMPTY_UUID)
							{
								return BuildError(requestId, ErrorCategory::Validation,
									"prefab.instantiate_many defaultClipAssetIds contains an invalid UUID.");
							}
							bulkRequest.defaultClipAssetIds.push_back(*parsed);
						}
					}
					if (arguments.contains("defaultClipStartFrameStep"))
					{
						if (!arguments["defaultClipStartFrameStep"].is_number_integer())
						{
							return BuildError(requestId, ErrorCategory::Validation,
								"prefab.instantiate_many defaultClipStartFrameStep must be an integer.");
						}
						const std::int64_t requestedStep =
							arguments["defaultClipStartFrameStep"].get<std::int64_t>();
						const std::uint64_t maximumStep = requestedCount > 1
							? std::numeric_limits<std::uint32_t>::max()
								/ static_cast<std::uint64_t>(requestedCount - 1)
							: std::numeric_limits<std::uint32_t>::max();
						if (requestedStep < 0 || static_cast<std::uint64_t>(requestedStep) > maximumStep)
						{
							return BuildError(requestId, ErrorCategory::Validation,
								"prefab.instantiate_many defaultClipStartFrameStep exceeds the safe range for count.");
						}
						bulkRequest.defaultClipStartFrameStep =
							static_cast<std::uint32_t>(requestedStep);
					}
					if (arguments.contains("columns"))
					{
						if (!arguments["columns"].is_number_integer())
						{
							return BuildError(requestId, ErrorCategory::Validation,
								"prefab.instantiate_many columns must be an integer.");
						}
						const std::int64_t requestedColumns = arguments["columns"].get<std::int64_t>();
						if (requestedColumns < 1 || requestedColumns > requestedCount)
						{
							return BuildError(requestId, ErrorCategory::Validation,
								"prefab.instantiate_many columns must be between 1 and count.");
						}
						bulkRequest.columns = static_cast<std::uint32_t>(requestedColumns);
					}
					if (arguments.contains("origin"))
					{
						const nlohmann::json& origin = arguments["origin"];
						if (!origin.is_array() || origin.size() != 3u
							|| !std::all_of(origin.begin(), origin.end(), [](const nlohmann::json& coordinate) {
								return coordinate.is_number();
							}))
						{
							return BuildError(requestId, ErrorCategory::Validation,
								"prefab.instantiate_many origin must contain exactly three numbers.");
						}
						bulkRequest.origin = glm::vec3(
							origin[0].get<float>(),
							origin[1].get<float>(),
							origin[2].get<float>());
					}
				}
				const auto instantiateResolvedPrefab =
					[&](std::string_view resolvedPrefabId) -> PrefabAuthoringService::Result {
						if (instantiateMany)
						{
							return prefabAuthoringService.InstantiateMany(
								resolvedPrefabId, bulkRequest);
						}
						return prefabAuthoringService.Instantiate(
							resolvedPrefabId, arguments.value("name", std::string{}));
					};

				const std::string prefabIdentity = arguments.value("prefab", std::string{});
				const AssetMetadata* registeredPrefab = nullptr;
				if (editorContext != nullptr && editorContext->assetDatabase != nullptr)
				{
					// Stable GUID input avoids path ambiguity after a move. Path input is
					// accepted only after projectAssetPath has proven it remains under Assets.
					if (const std::optional<TerraEngine::UUID> guid = TerraEngine::ParseUUID(prefabIdentity))
					{
						registeredPrefab = editorContext->assetDatabase->FindAssetByGuid(*guid);
					}
					if (registeredPrefab == nullptr)
					{
						if (const auto validatedPath = projectAssetPath(prefabIdentity))
						{
							const std::string wanted = LowerAscii(validatedPath->second.lexically_normal().generic_string());
							for (const auto& [absolutePath, metadata] : editorContext->assetDatabase->GetAssets())
							{
								if (LowerAscii(absolutePath.lexically_normal().generic_string()) == wanted)
								{
									registeredPrefab = &metadata;
									break;
								}
							}
						}
					}
				}

				if (registeredPrefab != nullptr && registeredPrefab->importerName == "PrefabImporter")
				{
					std::uint64_t publishedGeneration = 1u;
					GeneratedModelPackageChildMetadata generatedChild;
					if (editorContext->assetDatabase->TryGetGeneratedModelPackageChildMetadata(
						registeredPrefab->guid, GeneratedModelPackageChildKind::Prefab, generatedChild))
					{
						// Generated animation packages publish all children under one
						// generation. Retaining it makes stale override checks deterministic.
						publishedGeneration = std::max<std::uint64_t>(1u, generatedChild.publishedGeneration);
					}
					const std::filesystem::path relativePath =
						(std::filesystem::path("Assets") / registeredPrefab->sourcePath).lexically_normal();
					const std::filesystem::path absolutePath =
						(editorContext->assetDatabase->GetAssetRoot() / registeredPrefab->sourcePath).lexically_normal();
					const PrefabAuthoringService::Result registered = prefabAuthoringService.RegisterExistingAsset(
						relativePath, absolutePath, registeredPrefab->guid, publishedGeneration);
					// Registered assets enter the service's protected graph transaction,
					// which is also used by native hierarchy drag/drop. The response keeps
					// the indexed GUID/generation while instanceId names the real root.
					result = registered.ok
						? instantiateResolvedPrefab(
							registered.data.value("prefabId", std::string{}))
						: registered;
				}
				else
				{
					// Session-created prefabs are immediately usable before the file watcher
					// publishes them into AssetDatabase, so preserve that short local path.
					result = instantiateResolvedPrefab(prefabIdentity);
				}
			}
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
		if (operation == AutomationOperation("script.status"))
		{
			const ScriptCompilationSubsystem& compiler = ScriptCompilationSubsystem::Instance();
			const ScriptCompilationStatus& status = compiler.GetStatus();
			const ScriptWorkspaceService& workspace = compiler.GetWorkspaceService();
			const ScriptingSystem::ScriptingFrameResult& scripting = ScriptingSystem::Instance().GetLatestScriptingFrameResult();
			const std::string profile = status.activeProfile == ScriptBuildProfile::Debug ? "Debug" : "Release";
			const ManagedReadinessInputs fingerprints = workspace.BuildManagedReadinessInputs(profile);
			const std::filesystem::path assemblyPath = workspace.GetAssemblyPath();
			std::error_code relativeError;
			const std::filesystem::path projectRoot = editorContext != nullptr && editorContext->projectManager != nullptr
				? editorContext->projectManager->GetProjectRoot()
				: std::filesystem::path{};
			const std::filesystem::path relativeAssembly = projectRoot.empty()
				? assemblyPath
				: std::filesystem::relative(assemblyPath, projectRoot, relativeError);
			return BuildSuccess(requestId, "Read managed scripting readiness.", {
				{"coordinatorState", ScriptCoordinatorStateName(status.state)},
				{"readiness", BuildManagedReadinessEvidence(status, false)},
				{"fingerprint", {
					{"source", fingerprints.sourceFingerprint},
					{"workspace", fingerprints.workspaceFingerprint},
					{"profile", fingerprints.buildProfile},
					{"bridge", fingerprints.bridgeFingerprint},
					{"analyzer", fingerprints.analyzerFingerprint},
					{"runtimeAbi", fingerprints.runtimeAbiFingerprint}
				}},
				{"loadedOutput", {
					{"identity", relativeError ? assemblyPath.filename().generic_string() : relativeAssembly.generic_string()},
					{"fingerprint", ManagedReadinessManifest::FingerprintFile(assemblyPath)},
					{"published", status.gameplayAssemblyLoaded && !assemblyPath.empty()}
				}},
				// The latest protected scripting frame identifies whether there was no
				// runnable work or managed callbacks were skipped or failed.
				{"frame", {
					{"frameId", scripting.frameId},
					{"totalScripts", scripting.accessReport.totalScripts},
					{"updatedScripts", scripting.updatedScripts},
					{"skippedScripts", scripting.skippedScripts},
					{"failedScripts", scripting.failedScripts}
				}}
			});
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
			ScriptCompilationSubsystem::Instance().RequestBuild(ScriptBuildReason::ExplicitRebuild);
			// Manual compile is intentionally a cache-bypassing user request. Keep
			// the established durable-job fields and append explicit evidence rather
			// than asking MCP clients to infer that behavior from the queue state.
			const ScriptCompilationStatus& status = ScriptCompilationSubsystem::Instance().GetStatus();
			nlohmann::json accepted = BuildSuccess(requestId, "Accepted script compilation.", {
				{"jobId", job.jobId},
				{"state", "queued"},
				{"pollTool", "jobs.get"},
				{"cancelTool", "jobs.cancel"},
				{"forcedBuild", true},
				{"readiness", BuildManagedReadinessEvidence(status, true)}
			});
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
			AutomationOperation("scene_view.state"), AutomationOperation("scene_view.settings.get"),
			AutomationOperation("scene_view.settings.set"), AutomationOperation("scene_view.camera.set"),
			AutomationOperation("scene_view.frame_object"), AutomationOperation("scene_view.tool.set"),
			AutomationOperation("scene_view.orientation.set"), AutomationOperation("scene_view.snapping.set"),
			AutomationOperation("scene_view.visualization.set")}))
		{
			if (operation == AutomationOperation("scene_view.settings.get")
				|| operation == AutomationOperation("scene_view.settings.set"))
			{
				if (editorContext == nullptr || editorContext->projectManager == nullptr)
				{
					return BuildError(requestId, ErrorCategory::Unavailable, "Editor preferences are unavailable.");
				}
				ProjectManager& projects = *editorContext->projectManager;
				if (operation == AutomationOperation("scene_view.settings.set"))
				{
					if (!arguments.contains("settings"))
					{
						return BuildError(requestId, ErrorCategory::Validation, "A complete Scene View settings document is required.");
					}
					SceneViewOverlay::SceneOverlayPreference candidate = projects.GetEditorPreferences().sceneViewOverlay;
					std::string settingsError;
					if (!ParseSceneViewPerformancePreference(arguments["settings"], candidate, settingsError)
						|| !projects.SetSceneViewOverlayPreference(candidate, &settingsError))
					{
						return BuildError(requestId, ErrorCategory::Validation, settingsError);
					}
				}
				const SceneViewOverlay::SceneOverlayPreference& configured = projects.GetEditorPreferences().sceneViewOverlay;
				const std::uint32_t objectCount = static_cast<std::uint32_t>(std::min<std::size_t>(
					ObjectManager::instance().getRegistry().storage<entt::entity>().size(),
					std::numeric_limits<std::uint32_t>::max()));
				const SceneViewOverlay::SceneViewLargeSceneState effective = SceneViewOverlay::ResolveLargeSceneState(configured, objectCount);
				nlohmann::json effectiveSettings = SerializeSceneViewPerformancePreference(configured);
				effectiveSettings["largeSceneEffective"] = effective.effective;
				effectiveSettings["effectiveReason"] = effective.effective ? "object_threshold" : "below_threshold";
				return BuildSuccess(
					requestId,
					operation == AutomationOperation("scene_view.settings.set") ? "Persisted Scene View performance settings." : "Read Scene View performance settings.",
					{{"configured", SerializeSceneViewPerformancePreference(configured)},
					 {"effective", std::move(effectiveSettings)},
					 {"persisted", operation == AutomationOperation("scene_view.settings.set")}});
			}
			const std::shared_ptr<SceneViewWindow> sceneView = mainEditor != nullptr ? mainEditor->GetWindow<SceneViewWindow>() : nullptr;
			if (sceneView == nullptr)
				return BuildError(requestId, ErrorCategory::Unavailable, "Scene View semantic control is unavailable without a registered Scene View service.");
			if (operation == AutomationOperation("scene_view.state"))
			{
				nlohmann::json state = sceneView->GetAutomationState();
				const nlohmann::json performance = SerializeSceneViewPerformanceState(SceneViewOverlay::GetLatestDiagnosticsSnapshot());
				state["largeScene"] = performance["largeScene"];
				state["markers"] = performance["markers"];
				state["stageTimings"] = performance["stageTimings"];
				return BuildSuccess(requestId, "Read Scene View state.", {{"state", std::move(state)}});
			}

			std::string error;
			bool changed = false;
			if (operation == AutomationOperation("scene_view.camera.set"))
			{
				TerraEngine::Scene::WorldPosition position;
				TerraEngine::Scene::WorldPosition focus;
				if (!arguments.contains("position") || !arguments.contains("focus")
					|| !ParseCanonicalWorldPosition(arguments["position"], position, error)
					|| !ParseCanonicalWorldPosition(arguments["focus"], focus, error))
				{
					return BuildError(requestId, ErrorCategory::Validation,
						error.empty() ? "Camera position and focus must be canonical WorldPosition objects." : error);
				}
				changed = sceneView->SetAutomationCamera(position, focus, error);
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
			AutomationOperation("profiler.capture.summary"), AutomationOperation("profiler.capture.health"),
			AutomationOperation("profiler.capture.index"), AutomationOperation("profiler.capture.detail"),
			AutomationOperation("profiler.capture.operation")}))
		{
			if (editorContext == nullptr || !editorContext->editorProfiler) return BuildError(requestId, ErrorCategory::Unavailable, "Editor profiler service is unavailable.");
			const std::shared_ptr<const EditorProfilerPresentationSnapshot> presentation = editorContext->editorProfiler->GetPresentationSnapshot();
			if (!presentation) return BuildError(requestId, ErrorCategory::Unavailable, "Profiler presentation is not ready.");
			if (operation == AutomationOperation("profiler.capture.health"))
			{
				const auto& health = presentation->health;
				return BuildSuccess(requestId, "Read bounded profiler health.", {{"formatIdentifier", "TerraEngineProfilerJournalV1"}, {"formatVersion", 1u},
					{"projectEpoch", presentation->projectEpoch}, {"state", TerraEngine::Profiling::ToString(presentation->sessionState)},
					{"sessionArtifactId", presentation->activeSessionArtifactId},
					{"producedFrames", health.producedFrameCount}, {"queuedFrames", health.queuedFrameCount}, {"committedFrames", health.committedFrameCount},
					{"residentBytes", health.residentBytes}, {"residentHighWaterBytes", health.residentHighWaterBytes}, {"pendingBytes", health.pendingBytes},
					{"inFlightBytes", health.inFlightBytes}, {"persistenceLagFrames", health.persistenceLagFrames},
					{"persistenceLagHighWaterFrames", health.persistenceLagHighWaterFrames},
					{"collectionNanoseconds", health.collectionNanoseconds}, {"sealingNanoseconds", health.sealingNanoseconds},
					{"queueingNanoseconds", health.queueingNanoseconds}, {"persistenceNanoseconds", health.persistenceNanoseconds},
					{"finalizationNanoseconds", health.finalizationNanoseconds}, {"persistedBytes", health.persistedBytes},
					{"persistenceOperationCount", health.persistenceOperationCount},
					{"persistenceThroughputBytesPerSecond", health.persistenceThroughputBytesPerSecond},
					{"droppedDetailRecords", health.droppedDetailRecordCount},
					{"droppedDetailBytes", health.droppedDetailByteCount}, {"rejectedCoreFrames", health.rejectedCoreFrameCount},
					{"destinationWritable", health.destinationWritable}, {"terminalFailure", health.terminalFailure}, {"lastError", presentation->lastError}});
			}
			if (operation == AutomationOperation("profiler.capture.operation"))
			{
				std::optional<std::uint64_t> requestedOperationId;
				if (arguments.contains("operationId"))
				{
					const nlohmann::json& requested = arguments["operationId"];
					if ((!requested.is_number_unsigned() && !requested.is_number_integer())
						|| (requested.is_number_integer() && requested.get<std::int64_t>() <= 0))
					{
						return BuildError(requestId, ErrorCategory::Validation,
							"operationId must be a positive integer.");
					}
					requestedOperationId = requested.get<std::uint64_t>();
				}
				auto serializeOperation = [](const EditorProfilerOperationStatus& item) {
					return nlohmann::json{{"operationId", item.operationId}, {"active", item.active},
						{"kind", ToString(item.kind)}, {"state", ToString(item.state)}};
				};
				nlohmann::json operations = nlohmann::json::array();
				const EditorProfilerOperationStatus* requestedOperation = nullptr;
				for (const auto& item : presentation->operations)
				{
					if (item.operationId != 0u)
					{
						operations.push_back(serializeOperation(item));
					}
					if (requestedOperationId.has_value() && item.operationId == *requestedOperationId)
					{
						requestedOperation = &item;
					}
				}
				if (requestedOperationId.has_value() && requestedOperation == nullptr)
				{
					return BuildError(requestId, ErrorCategory::MissingTarget,
						"Profiler operation identifier is stale or unknown for the active project.");
				}
				// A sealed catalog artifact is authoritative after persistence; live
				// health is authoritative during recording. Neither path opens frame
				// history, so polling remains a scalar V1 operation.
				const std::uint64_t selectedCommittedFrames = presentation->selectedCapture.has_value()
					? presentation->selectedCapture->artifact.committedFrameCount : 0u;
				const std::uint64_t committedFrames = std::max(
					presentation->health.committedFrameCount, selectedCommittedFrames);
				const bool recoveredIncompleteTail = presentation->selectedCapture.has_value()
					&& presentation->selectedCapture->artifact.recoveredIncompleteTail;
				const bool persistenceActive = std::any_of(
					presentation->operations.begin(), presentation->operations.end(),
					[](const EditorProfilerOperationStatus& item) {
						return item.kind == EditorProfilerOperationKind::Persisting && item.active;
					});
				const bool completedCapture = presentation->selectedCapture.has_value()
					&& presentation->selectedCapture->artifact.complete;
				const char* finalizationState = presentation->health.terminalFailure
					? "failed"
					: ((presentation->sessionState == TerraEngine::Profiling::ProfilerSessionState::Finalizing
						|| persistenceActive) ? "finalizing"
						: (recoveredIncompleteTail ? "recovered_incomplete_tail"
							: (completedCapture ? "completed" : "idle")));
				nlohmann::json response = {
					{"formatIdentifier", "TerraEngineProfilerJournalV1"}, {"formatVersion", 1u},
					{"projectEpoch", presentation->projectEpoch}, {"operations", std::move(operations)},
					{"finalization", {{"state", finalizationState}, {"committedFrameCount", committedFrames},
						{"recoveredIncompleteTail", recoveredIncompleteTail},
						{"terminalFailure", presentation->health.terminalFailure}, {"lastError", presentation->lastError}}},
					{"status", presentation->statusText}, {"lastError", presentation->lastError}};
				if (requestedOperation != nullptr)
				{
					response["operation"] = serializeOperation(*requestedOperation);
				}
				return BuildSuccess(requestId, "Read bounded profiler operation state.", std::move(response));
			}
			if (operation == AutomationOperation("profiler.capture.index"))
			{
				auto readBoundedUnsigned = [&arguments, &requestId, this](std::string_view name,
					std::uint64_t fallback, std::uint64_t maximum, std::uint64_t& outValue) -> std::optional<nlohmann::json> {
					const std::string key(name);
					if (!arguments.contains(key))
					{
						outValue = fallback;
						return std::nullopt;
					}
					const nlohmann::json& value = arguments[key];
					if ((!value.is_number_unsigned() && !value.is_number_integer())
						|| (value.is_number_integer() && value.get<std::int64_t>() < 0))
					{
						return BuildError(requestId, ErrorCategory::Validation,
							std::string(name) + " must be a non-negative integer.");
					}
					const std::uint64_t parsed = value.get<std::uint64_t>();
					if (parsed > maximum)
					{
						return BuildError(requestId, ErrorCategory::Validation,
							std::string(name) + " exceeds its bounded profiler request limit.");
					}
					outValue = parsed;
					return std::nullopt;
				};
				std::uint64_t requestedOffset = 0u;
				std::uint64_t requestedLimit = 128u;
				if (const auto error = readBoundedUnsigned("offset", 0u,
					static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()), requestedOffset); error.has_value()) return *error;
				if (const auto error = readBoundedUnsigned("limit", 128u, 512u, requestedLimit); error.has_value()
					|| requestedLimit == 0u) return error.has_value() ? *error
						: BuildError(requestId, ErrorCategory::Validation, "limit must be at least one.");
				const std::size_t offset = static_cast<std::size_t>(requestedOffset);
				const std::size_t limit = static_cast<std::size_t>(requestedLimit);
				const auto& page = presentation->indexPage;
				if (!page.has_value() || page->offset != offset || page->pageSize != limit)
				{
					// Index history is page-owned. Queue a bounded read instead of
					// retaining a full journal list merely to answer this request.
					editorContext->editorProfiler->RequestIndexPage(offset, limit);
					return BuildSuccess(requestId, "Queued profiler index page.",
						{{"formatIdentifier", "TerraEngineProfilerJournalV1"}, {"formatVersion", 1u},
							{"projectEpoch", presentation->projectEpoch}, {"state", "queued"},
							{"offset", offset}, {"limit", limit}});
				}
				nlohmann::json frames = nlohmann::json::array();
				for (const auto& frame : page->frames)
					frames.push_back({{"frameIndex", frame.frameIndex}, {"durationMs", frame.durationMs}, {"complete", frame.complete}});
				return BuildSuccess(requestId, "Read profiler index page.",
					{{"formatIdentifier", "TerraEngineProfilerJournalV1"}, {"formatVersion", 1u},
						{"projectEpoch", presentation->projectEpoch}, {"state", "completed"},
						{"offset", page->offset}, {"total", page->totalFrameCount}, {"frames", frames}});
			}
			if (operation == AutomationOperation("profiler.capture.detail"))
			{
				if (!arguments.contains("frameIndex")
					|| (!arguments["frameIndex"].is_number_unsigned()
						&& !arguments["frameIndex"].is_number_integer())
					|| (arguments["frameIndex"].is_number_integer()
						&& arguments["frameIndex"].get<std::int64_t>() < 0))
				{
					return BuildError(requestId, ErrorCategory::Validation,
						"frameIndex must be a non-negative integer.");
				}
				const std::uint64_t frameIndex = arguments["frameIndex"].get<std::uint64_t>();
				if (!presentation->selectedFrame.has_value() || presentation->selectedFrame->frameIndex != frameIndex)
				{
					editorContext->editorProfiler->RequestSelectedFrame(frameIndex);
					return BuildSuccess(requestId, "Queued profiler frame detail.",
						{{"formatIdentifier", "TerraEngineProfilerJournalV1"}, {"formatVersion", 1u},
							{"projectEpoch", presentation->projectEpoch}, {"state", "queued"},
							{"frameIndex", frameIndex}});
				}
				const auto& frame = *presentation->selectedFrame;
				return BuildSuccess(requestId, "Read selected profiler frame detail.",
					{{"formatIdentifier", "TerraEngineProfilerJournalV1"}, {"formatVersion", 1u},
						{"projectEpoch", presentation->projectEpoch}, {"state", "completed"},
						{"frameIndex", frame.frameIndex}, {"durationMs", frame.durationMs},
						{"complete", frame.complete}, {"sampleCount", frame.samples.size()},
						{"timingStreamCount", frame.timingStreams.size()}});
			}
			if (operation == AutomationOperation("profiler.capture.list"))
			{
				nlohmann::json artifacts = nlohmann::json::array();
				for (const auto& item : editorContext->editorProfiler->GetAutomationCaptureList())
				{
					// Catalog facts distinguish a recovered tail from a complete V1
					// capture without loading detailed frame payloads.
					artifacts.push_back({
						{"artifactId", item.artifactId}, {"name", item.artifact.fileName},
						{"frameCount", item.artifact.frameCount}, {"status", item.artifact.status},
						{"formatIdentifier", "TerraEngineProfilerJournalV1"}, {"formatVersion", 1u}, {"committedFrameCount", item.artifact.committedFrameCount},
						{"complete", item.artifact.complete}, {"recoveredIncompleteTail", item.artifact.recoveredIncompleteTail},
						{"captureLevel", TerraEngine::Profiling::ToString(item.artifact.metadata.settings.detailLevel)},
						{"maximumChunkFrames", item.artifact.metadata.settings.maximumChunkFrames},
						{"maximumChunkBytes", item.artifact.metadata.settings.maximumChunkBytes}
					});
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
				if (presentation->selectedSummary.has_value())
				{
					const auto& summary = *presentation->selectedSummary;
					const double averageFrameMs = summary.frameCount == 0u ? 0.0
						: static_cast<double>(summary.totalDurationNanoseconds)
							/ static_cast<double>(summary.frameCount) / 1000000.0;
					nlohmann::json batching = {
						{"available", summary.committedSegmentCount != 0u},
						{"logicalInputBytes", summary.logicalInputBytes},
						{"encodedPayloadBytes", summary.encodedPayloadBytes},
						{"storedSegmentBytes", summary.storedSegmentBytes},
						{"persistenceOperationCount", summary.persistenceOperationCount}};
					if (summary.committedSegmentCount != 0u)
					{
						batching["framesPerPersistenceOperation"] = summary.persistenceOperationCount == 0u
							? nlohmann::json(nullptr)
							: nlohmann::json(static_cast<double>(summary.frameCount)
								/ static_cast<double>(summary.persistenceOperationCount));
						batching["storedToEncodedRatio"] = summary.encodedPayloadBytes == 0u
							? nlohmann::json(nullptr)
							: nlohmann::json(static_cast<double>(summary.storedSegmentBytes)
								/ static_cast<double>(summary.encodedPayloadBytes));
					}
					else
					{
						batching["framesPerPersistenceOperation"] = nullptr;
						batching["storedToEncodedRatio"] = nullptr;
					}
					nlohmann::json boundedSummary = {
						{"formatIdentifier", "TerraEngineProfilerJournalV1"}, {"formatVersion", 1u},
						{"frameCount", summary.frameCount}, {"committedFrameCount", summary.frameCount},
						{"completeFrameCount", summary.completeFrameCount},
						{"complete", presentation->selectedCapture.has_value()
							&& presentation->selectedCapture->artifact.complete},
						{"committedSegmentCount", summary.committedSegmentCount},
						{"totalDurationNanoseconds", summary.totalDurationNanoseconds},
						{"averageFrameMs", averageFrameMs},
						{"frameMedianMs", static_cast<double>(summary.finalEngineCpuWindowMedianNanoseconds) / 1000000.0},
						{"frameP95Ms", static_cast<double>(summary.finalEngineCpuWindowP95Nanoseconds) / 1000000.0},
						{"firstWindow", {
							{"count", summary.firstEngineCpuWindowSampleCount},
							{"totalNanoseconds", summary.firstEngineCpuWindowTotalNanoseconds},
							{"medianMs", static_cast<double>(summary.firstEngineCpuWindowMedianNanoseconds) / 1000000.0},
							{"p95Ms", static_cast<double>(summary.firstEngineCpuWindowP95Nanoseconds) / 1000000.0}}},
						{"finalWindow", {
							{"count", summary.finalEngineCpuWindowSampleCount},
							{"totalNanoseconds", summary.finalEngineCpuWindowTotalNanoseconds},
							{"medianMs", static_cast<double>(summary.finalEngineCpuWindowMedianNanoseconds) / 1000000.0},
							{"p95Ms", static_cast<double>(summary.finalEngineCpuWindowP95Nanoseconds) / 1000000.0}}},
						{"droppedDetailRecords", summary.droppedDetailRecordCount},
						{"droppedDetailBytes", summary.droppedDetailByteCount},
						{"batching", std::move(batching)},
						{"profilerSelf", {
							{"collectionNanoseconds", summary.profilerCollectionNanoseconds},
							{"sealingNanoseconds", summary.profilerSealingNanoseconds},
							{"queueingNanoseconds", summary.profilerQueueingNanoseconds},
							{"persistenceNanoseconds", summary.profilerPersistenceNanoseconds},
							{"persistedBytes", summary.persistedBytes},
							{"persistenceOperationCount", summary.persistenceOperationCount},
							{"persistenceThroughputBytesPerSecond", summary.persistenceThroughputBytesPerSecond},
							{"persistenceLagHighWaterFrames", summary.persistenceLagHighWaterFrames}}}
					};
					return BuildSuccess(requestId, "Read bounded V1 profiler summary.",
						{{"summary", std::move(boundedSummary)}});
				}
				return BuildError(requestId, ErrorCategory::Unavailable, "Load a V1 profiler capture before requesting its summary.");
			}
			if (operation == AutomationOperation("profiler.capture.start"))
			{
				TerraEngine::Profiling::ProfilerCaptureSettings settings{};
				settings.maxDurationSeconds = 0.0;
				try
				{
					const std::string captureLevel = arguments.value("captureLevel", std::string("detailed"));
					if (captureLevel == "detailed") settings.detailLevel = TerraEngine::Profiling::ProfilerCaptureDetailLevel::Detailed;
					else if (captureLevel == "sampled") settings.detailLevel = TerraEngine::Profiling::ProfilerCaptureDetailLevel::Sampled;
					else if (captureLevel == "aggregate") settings.detailLevel = TerraEngine::Profiling::ProfilerCaptureDetailLevel::Aggregate;
					else return BuildError(requestId, ErrorCategory::Validation, "captureLevel must be detailed, sampled, or aggregate.");
					settings.maximumChunkFrames = arguments.value("maximumChunkFrames", settings.maximumChunkFrames);
					settings.maximumChunkBytes = arguments.value("maximumChunkBytes", settings.maximumChunkBytes);
					settings.sampleIntervalFrames = arguments.value("sampleIntervalFrames", settings.sampleIntervalFrames);
					if (arguments.contains("maxFrames"))
					{
						const nlohmann::json& requestedMaxFrames = arguments["maxFrames"];
						if (!requestedMaxFrames.is_number_integer() && !requestedMaxFrames.is_number_unsigned())
							return BuildError(requestId, ErrorCategory::Validation, "maxFrames must be an integer.");
						if (requestedMaxFrames.is_number_unsigned())
						{
							const std::uint64_t value = requestedMaxFrames.get<std::uint64_t>();
							if (value > 4096u)
								return BuildError(requestId, ErrorCategory::Validation, "maxFrames must be zero (unlimited) or between 1 and 4096.");
							settings.maxFrames = static_cast<std::uint32_t>(value);
						}
						else
						{
							const std::int64_t value = requestedMaxFrames.get<std::int64_t>();
							if (value < 0 || value > 4096)
								return BuildError(requestId, ErrorCategory::Validation, "maxFrames must be zero (unlimited) or between 1 and 4096.");
							settings.maxFrames = static_cast<std::uint32_t>(value);
						}
					}
				}
				catch (const nlohmann::json::exception&)
				{
					return BuildError(requestId, ErrorCategory::Validation, "Profiler capture options have invalid types.");
				}
				if (settings.maximumChunkFrames == 0u || settings.maximumChunkFrames > 4096u || settings.maximumChunkBytes < 1024u || settings.maximumChunkBytes > 8388608u || settings.sampleIntervalFrames == 0u || settings.sampleIntervalFrames > 1024u || settings.maxFrames > 4096u)
					return BuildError(requestId, ErrorCategory::Validation, "Profiler capture bounds are outside the supported range.");
				editorContext->editorProfiler->StartRecording(settings);
				if (editorContext->editorProfiler->GetState() != TerraEngine::Profiling::ProfilerSessionState::Recording)
					return BuildError(requestId, ErrorCategory::Unavailable, editorContext->editorProfiler->GetLastError());
				const AutomationJobSnapshot job = jobService.Create({"profiler", std::string(operationName), requestId, arguments.value("correlationId", requestId), {}, false});
				if (job.jobId.empty()) return BuildError(requestId, ErrorCategory::QueueFull, "Profiler job queue is full.");
				(void)jobService.CompleteSucceeded(job.jobId, {{"state", "recording"}});
				return BuildSuccess(requestId, "Started profiler capture.", {{"jobId", job.jobId}, {"state", "succeeded"},
					{"captureLevel", TerraEngine::Profiling::ToString(settings.detailLevel)}, {"formatIdentifier", "TerraEngineProfilerJournalV1"}, {"formatVersion", 1u},
					{"bounds", {{"maximumChunkFrames", settings.maximumChunkFrames}, {"maximumChunkBytes", settings.maximumChunkBytes}, {"sampleIntervalFrames", settings.sampleIntervalFrames}, {"maxFrames", settings.maxFrames}}}});
			}
			if (operation == AutomationOperation("profiler.capture.stop"))
			{
				const TerraEngine::Profiling::ProfilerSessionState profilerState =
					editorContext->editorProfiler->GetState();
				// A stop request owns one transition from live recording to durable
				// persistence. Reject repeated or concurrent stops before they can
				// snapshot and save the already-finalized document a second time.
				if (profilerState != TerraEngine::Profiling::ProfilerSessionState::Recording
					&& profilerState != TerraEngine::Profiling::ProfilerSessionState::Paused)
					return BuildError(requestId, ErrorCategory::Unavailable,
						"Profiler capture is not recording or paused.");
				if (!activeProfilerCaptureJobId.empty())
					return BuildError(requestId, ErrorCategory::Unavailable,
						"Profiler capture persistence is already active.");
				const AutomationJobSnapshot job = jobService.Create({"profiler", std::string(operationName), requestId, arguments.value("correlationId", requestId), {}, false});
				if (job.jobId.empty()) return BuildError(requestId, ErrorCategory::QueueFull, "Profiler job queue is full.");
				activeProfilerCaptureJobId = job.jobId;
				// This measures only the protected service handoff (seal plus durable
				// ticket creation). It deliberately excludes MCP queue/HTTP latency and
				// asynchronous journal draining, so a slow client response is diagnosable
				// without misattributing it to journal finalization.
				const auto finishSubmissionStarted = std::chrono::steady_clock::now();
				const bool finishAccepted = editorContext->editorProfiler->FinishCapture();
				const double finishCaptureSubmissionMilliseconds = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - finishSubmissionStarted).count();
				if (!finishAccepted)
				{
					const std::string error = editorContext->editorProfiler->GetLastError();
					(void)jobService.CompleteFailed(job.jobId, {{"error", error}});
					activeProfilerCaptureJobId.clear();
					return BuildSuccess(requestId, "Profiler capture contained no runtime frames.",
						{{"jobId", job.jobId}, {"state", "failed"}, {"error", error},
							{"finishCaptureSubmissionMilliseconds", finishCaptureSubmissionMilliseconds}});
				}
				return BuildSuccess(requestId, "Accepted profiler capture persistence.",
					{{"jobId", job.jobId}, {"state", "queued"}, {"pollTool", "jobs.get"},
						{"finishCaptureSubmissionMilliseconds", finishCaptureSubmissionMilliseconds}});
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
			if (!SceneManager::Instance().SaveScene(*path)) return BuildError(requestId, ErrorCategory::Internal,
				"Scene save-as failed: " + SceneManager::Instance().GetLastSaveResult().reason);
			// Automation saves participate in the same revision-aware dirty state as
			// Ctrl+S, so subsequent undo/redo reports the persisted version correctly.
			if (!SceneManager::Instance().IsActiveSceneDirty()) UndoRedoSystem::Instance().SetSavePoint();
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
				{"limits", descriptor.limits},
				{"generationContract", descriptor.generationContract},
				{"jobBehavior", descriptor.jobBehavior},
				{"diagnosticsContract", descriptor.diagnosticsContract},
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
		// The retained native source path is the actual default SaveScene target,
		// so owned automation can check containment without guessing from the display name.
		data["activeScenePath"] = scene != nullptr ? scene->sourcePath.generic_string() : "";
		data["sceneDirty"] = SceneManager::Instance().IsActiveSceneDirty();
		// Automation checks the native close-dialog readiness at this protected snapshot,
		// rather than assuming a completed validation operation has settled imports.
		data["assetImportsActive"] = AssetDatabase::Instance().IsCompiling();
		data["playMode"] = ScriptingSystem::Instance().IsRunning();
		const AutomationStateSnapshot automationState = BuildAutomationStateSnapshot();
		data["playState"] = automationState.playState;
		data["projectGeneration"] = automationState.projectGeneration;
		data["sceneGeneration"] = automationState.sceneGeneration;
		data["availableServices"] = automationState.availableServices;
		if (hasProject)
		{
			const ProjectSceneLoadDecision decision = editorContext->projectManager->GetLastSceneLoadDecision();
			const ProjectReadinessState readiness = editorContext->projectManager->GetReadinessState();
			data["derivedScene"] = {
				{"state", decision.cacheState},
				{"cacheHit", decision.cacheHit},
				{"usedAuthoritativeSource", decision.usedAuthoritativeSource},
				{"regenerationPublished", decision.regenerationPublished},
				{"cancelled", decision.cancelled},
				{"sourcePath", decision.sourcePath.generic_string()},
				{"derivedPath", decision.derivedPath.generic_string()},
				{"reason", decision.reason}
			};
			data["readiness"] = {
				{"projectSettingsReady", readiness.projectSettingsReady},
				{"simulationEssentialReady", readiness.simulationEssentialReady},
				{"editorMetadataDeferred", readiness.editorMetadataDeferred},
				{"editorMetadataReady", readiness.editorMetadataReady},
				{"sceneSource", readiness.sceneReadinessSource},
				{"reason", readiness.sceneReadinessReason}
			};
		}
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
		const nlohmann::json parallelFrame = SerializeParallelFrameDiagnostics(engine, includeDetails);
		const nlohmann::json mutationDiagnostics = SerializeMutationDiagnostics();
		const TerraEngine::Renderer::RenderDiagnosticsSummary renderDiagnostics = RenderSystem::Instance().GetLatestRenderGraphDiagnostics();
		const std::shared_ptr<const EditorProfilerPresentationSnapshot> profilerPresentation =
			editorContext != nullptr && editorContext->editorProfiler
				? editorContext->editorProfiler->GetPresentationSnapshot()
				: nullptr;
		const nlohmann::json profilerTimingStreams = includeDetails
			? SerializeProfilerTimingStreams(profilerPresentation.get())
			: nlohmann::json::array();
		const nlohmann::json profilerCaptureEvidence = includeDetails
			? SerializeProfilerCaptureEvidence(profilerPresentation.get())
			: nlohmann::json::object();
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
		data["profilerCaptureEvidence"] = profilerCaptureEvidence;
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
			snapshot.projectGeneration = editorContext->projectManager->GetProjectLoadGeneration();
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

	void EditorAutomationService::PumpExportJobsAtProtectedFrame()
	{
		// The controller polls futures outside service/mailbox locks; all durable
		// job, activity and frontend effects are serialized by this owner adapter.
		for (auto iterator = activeExportOperations.begin(); iterator != activeExportOperations.end(); ) {
			auto& active = iterator->second;
			active.controller->Poll();
			const auto lookup = active.controller->Lookup(active.operationId);
			if (lookup.disposition == EditorExportLookupDisposition::Expired || lookup.disposition == EditorExportLookupDisposition::Unknown) {
				// Retention expiry and a never-issued reference are terminal consumer
				// outcomes. Durable job copies survive, while ongoing polling and old
				// project launch authority are relinquished at this protected boundary.
				const bool expired = lookup.disposition == EditorExportLookupDisposition::Expired;
				const nlohmann::json terminal{{"success", false}, {"category", expired ? "export.expired" : "export.unknown"},
					{"operationId", active.operationId}, {"message", expired ? "Export observation expired." : "Export operation is unknown."}};
				(void)jobService.CompleteFailed(iterator->first, terminal);
				for (auto& job : exportJobs) if (job.jobId == iterator->first && job.state != "succeeded" && job.state != "failed" && job.state != "canceled") job.state = "failed";
				iterator = activeExportOperations.erase(iterator); continue;
			}
			const auto& observation = lookup.observation;
			if (observation->revision != active.revision) {
				active.revision = observation->revision;
				(void)jobService.SetState(iterator->first, AutomationJobState::Running, std::string(ToString(observation->progress.stage)));
				(void)jobService.PublishProgress(iterator->first, observation->progress.overallProgress, std::string(ToString(observation->progress.stage)));
			}
			const auto result = active.controller->Consume(active.operationId, 2).result;
			if (!result) { ++iterator; continue; }
			for (const auto& diagnostic : result->diagnostics) {
				(void)jobService.AddDiagnostic(iterator->first, {ExportAutomationSeverity(diagnostic.severity), diagnostic.category, diagnostic.message,
					{{"stage", diagnostic.stage}, {"code", diagnostic.code}, {"file", diagnostic.filePath}, {"line", diagnostic.line}, {"suggestion", diagnostic.suggestion}}});
			}
			if (result->success && !result->outputDirectory.empty()) (void)jobService.AddArtifact(iterator->first,
				{"export-output-" + result->runId, "export_output", result->outputDirectory.generic_string(), "application/vnd.terraengine.export", {{"profileId", result->profileId}, {"runId", result->runId}}});
			if (!result->evidenceDirectory.empty()) (void)jobService.AddArtifact(iterator->first,
				{"export-evidence-" + result->runId, "export_evidence", result->evidenceDirectory.generic_string(), "application/json", {{"runId", result->runId}}});
			const nlohmann::json terminal{{"success", result->success}, {"canceled", result->canceled}, {"runId", result->runId},
				{"stage", ToString(result->stage)}, {"failureCategory", ToString(result->failureCategory)}, {"exitCode", result->exitCode}, {"message", result->message},
				{"outputDirectory", result->outputDirectory.generic_string()}, {"evidenceDirectory", result->evidenceDirectory.generic_string()}};
			if (result->canceled) (void)jobService.CompleteCanceled(iterator->first, terminal);
			else if (result->success) (void)jobService.CompleteSucceeded(iterator->first, terminal);
			else (void)jobService.CompleteFailed(iterator->first, terminal);
			for (auto& job : exportJobs) if (job.jobId == iterator->first) { job.state = result->canceled ? "canceled" : result->success ? "succeeded" : "failed"; job.progress = observation->progress.overallProgress; }
			iterator = activeExportOperations.erase(iterator);
		}
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
		if (component == Transform::kComponentName)
		{
			// Transform owns the exact world coordinate. Reflection cannot prove the
			// required coordinate normalization or parent-space propagation, so MCP
			// callers must use the dedicated typed world operations instead.
			result.message = "Transform reflection writes are unsupported; use world.set_transform or world.translate through transaction.apply.";
			return result;
		}

		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto componentIt = ComponentAccessRegistry().find(component);
		if (componentIt == ComponentAccessRegistry().end() || !componentIt->second.entityHasComponent(ObjectManager::instance().getRegistry(), *target.entity))
		{
			result.message = "Component is not present on the object.";
			return result;
		}

		const TerraEngine::Reflection::Property reflectedProperty =
			TerraEngine::Reflection::FindProperty(componentIt->second.reflectionType, property);
		if (!TerraEngine::Reflection::IsValid(reflectedProperty))
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
		// Canonical transform transactions require a current observed revision.
		// Expose it with identity reads so clients never guess a revision or weaken
		// optimistic concurrency checks just to place a newly authored character.
		data["transformRevision"] = objectHandle
			? objectManager.GetObjectTransformRevision(*objectHandle) : std::uint64_t{0};
		std::optional<Object> parent = objectHandle ? objectManager.getParent(*objectHandle) : std::nullopt;
		// Keep the original entity-valued parent field for existing automation
		// clients, and publish a stable UUID alongside it so callers can bind a
		// hierarchy result across scene reloads or compare it with object UUIDs.
		data["parent"] = parent ? nlohmann::json(static_cast<std::uint32_t>(parent->EntityId())) : nullptr;
		data["parentUuid"] = parent
			? nlohmann::json(TerraEngine::ToHexString(objectManager.getUUID(*parent)))
			: nullptr;
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
		// Scene listings cross the MCP transport as one JSON message. Keep every
		// response bounded so a dense scene cannot monopolize the protected editor
		// boundary or cause the client to disconnect while serializing a huge tree.
		constexpr std::uint64_t kDefaultPageSize = 128u;
		constexpr std::uint64_t kMaximumPageSize = 256u;
		auto readNonNegativeInteger = [&](std::string_view name, std::uint64_t defaultValue,
			std::uint64_t& outValue) -> bool {
			const auto value = arguments.find(std::string(name));
			if (value == arguments.end())
			{
				outValue = defaultValue;
				return true;
			}
			if (value->is_number_unsigned())
			{
				outValue = value->get<std::uint64_t>();
				return true;
			}
			if (value->is_number_integer())
			{
				const std::int64_t signedValue = value->get<std::int64_t>();
				if (signedValue >= 0)
				{
					outValue = static_cast<std::uint64_t>(signedValue);
					return true;
				}
			}
			return false;
		};

		const bool includeComponents = arguments.value("includeComponents", false);
		const std::string search = arguments.value("search", "");
		std::uint64_t offset = 0u;
		std::uint64_t limit = kDefaultPageSize;
		if (!readNonNegativeInteger("offset", 0u, offset)
			|| !readNonNegativeInteger("limit", kDefaultPageSize, limit)
			|| limit == 0u || limit > kMaximumPageSize)
		{
			return BuildError(requestId, ErrorCategory::Validation,
				"scene.list_objects requires offset >= 0 and limit in the range [1, 256].");
		}
		nlohmann::json objects = nlohmann::json::array();
		ObjectManager& objectManager = ObjectManager::instance();
		std::vector<Object> roots = objectManager.getRootObjects();
		objectManager.SortObjectsByStableId(roots);
		std::unordered_set<entt::entity> visited;
		std::vector<Object> pending;
		pending.reserve(roots.size());
		// Push in reverse because the explicit LIFO worklist preserves the existing
		// stable depth-first order without consuming one native stack frame per
		// authored hierarchy level.
		for (auto root = roots.rbegin(); root != roots.rend(); ++root)
		{
			pending.push_back(*root);
		}
		std::uint64_t matchedCount = 0u;
		bool hasMore = false;
		while (!pending.empty())
		{
			const Object object = pending.back();
			pending.pop_back();
			// A malformed hierarchy must not make an automation read recurse
			// forever. The visited set also covers malformed cycles now that traversal
			// is iterative rather than recursive.
			if (!visited.insert(object.EntityId()).second)
			{
				continue;
			}
			const bool matches = search.empty()
				|| ContainsCaseInsensitive(objectManager.getObjectName(object), search)
				|| ContainsCaseInsensitive(objectManager.getObjectTag(object), search)
				|| ContainsCaseInsensitive(objectManager.getObjectLayer(object), search);
			if (matches)
			{
				if (matchedCount >= offset)
				{
					if (objects.size() >= limit)
					{
						hasMore = true;
						break;
					}
					// Component discovery is intentionally deferred until an object is
					// inside the requested page. Large filtered-out ranges therefore do
					// not pay one registry scan per object.
					objects.push_back(SerializeObjectSummary(object.EntityId(), includeComponents));
				}
				++matchedCount;
			}
			// Children are returned in stable UUID order by ObjectManager, which
			// makes pages deterministic across entity allocation. Reverse insertion
			// keeps the first child at the front of the depth-first traversal.
			std::vector<Object> children = objectManager.getChildren(object);
			for (auto child = children.rbegin(); child != children.rend(); ++child)
			{
				pending.push_back(*child);
			}
		}
		const std::uint64_t returned = static_cast<std::uint64_t>(objects.size());
		return BuildSuccess(requestId, "Listed scene objects.", {
			{"objects", std::move(objects)},
			{"offset", offset},
			{"limit", limit},
			{"returned", returned},
			{"hasMore", hasMore},
			{"nextOffset", hasMore ? offset + returned : 0u}
		});
	}

	nlohmann::json EditorAutomationService::HandleSceneSave(const nlohmann::json& arguments, const std::string& requestId)
	{
		const bool saved = arguments.contains("path") && arguments["path"].is_string()
			? SceneManager::Instance().SaveScene(arguments["path"].get<std::string>())
			: SceneManager::Instance().SaveScene();
		if (!saved)
		{
			return BuildError(requestId, ErrorCategory::Internal,
				"Scene save failed: " + SceneManager::Instance().GetLastSaveResult().reason);
		}
		// Store the exact history state that reached disk rather than relying on
		// SceneManager's temporary dirty-flag clear.
		// A complete file can contain an earlier captured revision if a newer
		// edit arrived during I/O. Keep that newer history dirty until saved too.
		if (!SceneManager::Instance().IsActiveSceneDirty()) UndoRedoSystem::Instance().SetSavePoint();
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
		const std::optional<Object> objectHandle = objectManager.getObject(object);
		if (!objectHandle)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Object was not found.", target.input);
		}

		// The public one-property tool lowers to the same typed transaction as a
		// multi-operation MCP request. This keeps validation, conflict detection,
		// rollback, and reflected setter success at one protected commit boundary.
		const std::string transactionId = "component-property-" + std::to_string(nextTransactionId++);
		const nlohmann::json transactionArguments = {
			{"format", "TerraSpatialTransactionV1"},
			{"label", "MCP component property edit"},
			{"operations", nlohmann::json::array({{
				{"kind", "component.set_property"},
				{"target", TerraEngine::ToHexString(objectManager.getUUID(*objectHandle))},
				{"component", component},
				{"property", property},
				{"value", arguments["value"]}
			}})}
		};
		PreparedAutomationTransaction prepared = PrepareAutomationTransaction(
			transactionArguments, objectManager, transactionId);
		if (!prepared.valid)
		{
			return BuildError(requestId, prepared.error, prepared.message);
		}

		nlohmann::json oldPayload;
		{
			std::lock_guard lock(ComponentAccessRegistryMutex());
			const auto registered = ComponentAccessRegistry().find(component);
			if (registered == ComponentAccessRegistry().end()
				|| !registered->second.entityHasComponent(objectManager.getRegistry(), object))
			{
				return BuildError(requestId, ErrorCategory::MissingTarget, "Component is not present on the object.", component);
			}
			oldPayload = registered->second.entitySerialize(objectManager.getRegistry(), object);
		}

		const TerraEngine::Scene::TransactionResult commit =
			TerraEngine::Scene::CommitTransaction(objectManager, prepared.transaction);
		if (commit.status != TerraEngine::Scene::TransactionStatus::Committed)
		{
			const ErrorCategory category = commit.status == TerraEngine::Scene::TransactionStatus::Conflicted
				? ErrorCategory::Conflict : ErrorCategory::Validation;
			return BuildError(requestId, category, commit.message);
		}

		nlohmann::json newPayload;
		{
			std::lock_guard lock(ComponentAccessRegistryMutex());
			const auto registered = ComponentAccessRegistry().find(component);
			if (registered == ComponentAccessRegistry().end()
				|| !registered->second.entityHasComponent(objectManager.getRegistry(), object))
			{
				return BuildError(requestId, ErrorCategory::Internal, "Committed component property could not be read back.", component);
			}
			newPayload = registered->second.entitySerialize(objectManager.getRegistry(), object);
		}

		UndoRedoSystem::Instance().PushCommand(
			std::make_unique<SetComponentPayloadCommand>(object, component, oldPayload, newPayload));
		MarkSceneMutated(ActivityEventType::Mutation, requestId, "component.set_property", "Updated component property through a typed transaction.");
		return BuildSuccess(requestId, "Updated component property.", {
			{"component", component}, {"property", property}, {"transactionId", transactionId},
			{"visibilityBoundary", "post_editor_automation_flush"}
		});
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
		const auto listStarted = std::chrono::steady_clock::now();
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
		const auto listFinished = std::chrono::steady_clock::now();
		return BuildSuccess(requestId, "Listed project assets.", {
			{"assets", assets},
			{"timings", {{"listMs", std::chrono::duration<double, std::milli>(
				listFinished - listStarted).count()}}}
		});
	}

	nlohmann::json EditorAutomationService::HandleAssetContentVerify(const nlohmann::json& arguments, const std::string& requestId)
	{
		// This read-only operation is useful to integrity tools as well as performance
		// diagnostics: paths remain project-contained, request counts are bounded, and
		// no payload bytes are copied into the JSON response.
		nlohmann::json validation = ValidateAssetPath(arguments, requestId);
		if (!validation.value("ok", false)) return validation;
		if (editorContext == nullptr || editorContext->assetDatabase == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Asset database is not available.");
		}
		if (!arguments.contains("requestCount")
			|| (!arguments["requestCount"].is_number_unsigned() && !arguments["requestCount"].is_number_integer()))
		{
			return BuildError(requestId, ErrorCategory::Validation, "requestCount must be an integer from 1 through 32768.");
		}
		const std::int64_t signedRequestCount = arguments["requestCount"].get<std::int64_t>();
		if (signedRequestCount < 1 || signedRequestCount > 32768)
		{
			return BuildError(requestId, ErrorCategory::Validation, "requestCount must be an integer from 1 through 32768.");
		}
		const std::uint32_t requestCount = static_cast<std::uint32_t>(signedRequestCount);
		const std::int64_t signedCancellationEvery = arguments.value("cancellationEvery", std::int64_t{0});
		if (signedCancellationEvery < 0 || signedCancellationEvery > 32768)
		{
			return BuildError(requestId, ErrorCategory::Validation, "cancellationEvery must be an integer from 0 through 32768.");
		}
		const std::uint32_t cancellationEvery = static_cast<std::uint32_t>(signedCancellationEvery);
		const std::uint64_t expectedPayloadBytes = arguments.value("expectedPayloadBytes", std::uint64_t{0});
		if (expectedPayloadBytes > 268435456u)
		{
			return BuildError(requestId, ErrorCategory::Validation, "expectedPayloadBytes exceeds the 256 MiB verification limit.");
		}

		const std::filesystem::path relative = validation["path"].get<std::string>();
		const std::filesystem::path absolutePath = editorContext->assetDatabase->GetAssetRoot() / relative;
		if (!std::filesystem::exists(absolutePath) || std::filesystem::is_directory(absolutePath))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Asset content file was not found.", relative.generic_string());
		}

		std::uint64_t fingerprint = 1469598103934665603ull;
		std::uint64_t totalBytesRead = 0u;
		std::uint32_t completedCount = 0u;
		std::uint32_t cancelledCount = 0u;
		std::uint64_t stagingAllocationCount = 0u;
		std::uint64_t stagingReuseCount = 0u;
		std::uint64_t hiddenFallbackCount = 0u;
		std::vector<std::uint8_t> reusableStaging;
		const auto loadStarted = std::chrono::steady_clock::now();
		for (std::uint32_t requestIndex = 0u; requestIndex < requestCount; ++requestIndex)
		{
			// Cancellation is decided before IO, matching the resource streaming
			// contract that a cancelled private publication token exposes no payload.
			if (cancellationEvery != 0u && (requestIndex + 1u) % cancellationEvery == 0u)
			{
				++cancelledCount;
				continue;
			}
			// Reusable staging passed the allocation, correctness, memory, and soak
			// gates, so repeated verification has one production path. The vector
			// owns its bytes for the operation and retains capacity between reads.
			const std::size_t previousCapacity = reusableStaging.capacity();
			if (!ResourceManager::Instance().ReadFileBytesInto(absolutePath, reusableStaging)) {
				return BuildError(requestId, ErrorCategory::Internal,
					"Asset content could not be read into reusable staging.", relative.generic_string());
			}
			if (reusableStaging.capacity() != previousCapacity) ++stagingAllocationCount;
			else ++stagingReuseCount;
			const std::vector<std::uint8_t>& bytes = reusableStaging;
			if (expectedPayloadBytes != 0u && bytes.size() != expectedPayloadBytes)
			{
				return BuildError(requestId, ErrorCategory::Conflict, "Asset content size changed during verification.", relative.generic_string());
			}
			for (const std::uint8_t value : bytes)
			{
				fingerprint ^= value;
				fingerprint *= 1099511628211ull;
			}
			totalBytesRead += bytes.size();
			++completedCount;
		}
		const auto loadFinished = std::chrono::steady_clock::now();
		std::ostringstream fingerprintText;
		fingerprintText << std::hex << fingerprint;
		return BuildSuccess(requestId, "Verified project asset content through ResourceManager.", {
			{"path", relative.generic_string()}, {"requestedCount", requestCount},
			{"completedCount", completedCount}, {"cancelledCount", cancelledCount},
			{"bytesRead", totalBytesRead}, {"contentFingerprint", fingerprintText.str()},
			{"memoryStrategy", "reusable-arena"},
			{"memoryStrategySource", "built-in-default"},
			{"stagingPolicy", "reusable-buffer"},
			{"stagingAllocationCount", stagingAllocationCount},
			{"stagingReuseCount", stagingReuseCount},
			{"stagingCapacityBytes", reusableStaging.capacity()},
			{"hiddenFallbackCount", hiddenFallbackCount},
			{"timings", {{"loadMs", std::chrono::duration<double, std::milli>(loadFinished - loadStarted).count()}}}
		});
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
		if (arguments.value("dryRun", false))
		{
			// Dry-run uses the exact path, existence, containment, and asset-database
			// checks above, then stops before finalizing or queuing any importer work.
			// This makes automation preflight meaningful without creating a hidden
			// mutation that a rejected durable-job reservation cannot account for.
			return BuildSuccess(requestId, "Validated asset import without queuing work.", {
				{"path", validation["path"]},
				{"force", arguments.value("force", false)},
				{"status", "validated"},
				{"dryRun", true}
			});
		}
		// The asset watcher/startup reconcile may already be compiling this exact
		// source. Finalize only that job before the explicit synchronous import so
		// the response cannot read an older package sidecar. Unrelated jobs continue
		// running and retain their own deferred requests.
		editorContext->assetDatabase->FinalizeActiveImportForExplicitReimport(absolutePath);
		if (!std::filesystem::exists(absolutePath) || std::filesystem::is_directory(absolutePath))
		{
			return BuildError(
				requestId,
				ErrorCategory::MissingTarget,
				"Asset file was removed while its prior import was being finalized.",
				relative.generic_string());
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
			exportRequest.evidenceDirectory = BuildDefaultExportEvidenceDirectory(
				exportRequest.projectRoot, exportRequest.runId);
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

			// Both frontends share the project-owned admission controller. The worker
			// retains immutable input/shared operation state, never this service.
			if (!editorContext->exportController) editorContext->exportController = std::make_shared<EditorExportOperationController>();
			EditorExportOperationRequest operationRequest;
			operationRequest.request = std::move(exportRequest);
			operationRequest.projectGeneration = editorContext->exportController->Generation();
			const auto admission = editorContext->exportController->Admit(std::move(operationRequest));
			if (!admission.accepted) {
				(void)jobService.CompleteFailed(job.jobId, {{"category", "conflict"}, {"message", admission.error}});
				for (auto& retained : exportJobs) if (retained.jobId == job.jobId) retained.state = "failed";
				return BuildError(requestId, ErrorCategory::Conflict, admission.error);
			}
			activeExportOperations[job.jobId] = {editorContext->exportController, admission.operationId, 0};
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
				const auto active = activeExportOperations.find(jobId);
				if (active != activeExportOperations.end()) {
					const auto disposition = active->second.controller->RequestCancellation(active->second.operationId);
					if (disposition == ExportCancellationDisposition::Accepted) { (void)jobService.RequestCancellation(jobId); job.cancelRequested = true; }
					return BuildSuccess(requestId, disposition == ExportCancellationDisposition::Accepted ? "Export cancellation requested." : "Export cancellation is too late.",
						{{"jobId", jobId}, {"state", job.state}, {"cancellationDisposition", disposition == ExportCancellationDisposition::Accepted ? "accepted" : "too_late"}});
				}
				// Durable terminal jobs cannot be relabeled after commit/settlement.
				if (job.state == "succeeded" || job.state == "failed" || job.state == "canceled") return BuildSuccess(requestId, "Export already settled.", {{"jobId", jobId}, {"state", job.state}, {"cancellationDisposition", "already_terminal"}});
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
		if (request.freshness == "nextFrame")
		{
			const std::vector<Object> cameras =
				ObjectManager::instance().getObjectsWithComponents<Transform, CameraComponent>();
			if (cameras.empty())
			{
				return BuildError(requestId, ErrorCategory::MissingTarget, "No active Game View camera is available.");
			}
			const std::optional<AuthoredCameraCaptureSnapshot> camera =
				ReadAuthoredCameraCaptureSnapshot(ObjectManager::instance(), cameras.back());
			if (!camera)
			{
				return BuildError(requestId, ErrorCategory::InvalidCameraObject, "The active Game View camera could not be snapshotted.");
			}

			// A hidden Game View dock tab does not refresh its cached render target.
			// Snapshot the same last active camera that the visible Game View would
			// submit and render it through the dedicated target for nextFrame.
			TerraEngine::Renderer::RenderCaptureRequest nextFrameRequest =
				BuildCaptureRequestFromCamera(
					TerraEngine::Renderer::RenderCaptureSourceKind::GameView,
					"Game View",
					camera->transform,
					camera->camera,
					size,
					request.transparentBackground);
			nextFrameRequest.requestId = request.requestId;
			nextFrameRequest.format = request.format;
			nextFrameRequest.freshness = request.freshness;
			nextFrameRequest.includeOverlays = request.includeOverlays;
			nextFrameRequest.allowBackendFallback = request.allowBackendFallback;
			nextFrameRequest.timeoutMs = request.timeoutMs;
			nextFrameRequest.dedicatedRenderTarget = true;
			request = std::move(nextFrameRequest);
		}

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
		// Custom capture is a strict breaking payload. A named camera object keeps
		// options separate, but absolute locations must be canonical addresses.
		if (!arguments.contains("camera") || !arguments["camera"].is_object())
		{
			return BuildError(requestId, ErrorCategory::Validation, "camera.capture_custom requires a camera object.");
		}
		const nlohmann::json& camera = arguments["camera"];
		TerraEngine::Scene::WorldPosition position;
		std::string coordinateError;
		if (!ParseCanonicalWorldPosition(camera.value("position", nlohmann::json()), position, coordinateError))
		{
			return BuildError(requestId, ErrorCategory::Validation, coordinateError);
		}

		glm::vec3 up = ReadVec3(camera.value("up", nlohmann::json())).value_or(glm::vec3(0.0f, 1.0f, 0.0f));
		if (glm::length(up) < 0.0001f)
		{
			return BuildError(requestId, ErrorCategory::Validation, "Custom camera up vector must be non-zero.");
		}
		glm::vec3 forward(0.0f, 0.0f, -1.0f);
		const nlohmann::json targetValue = camera.value("lookAt", nlohmann::json());
		TerraEngine::Scene::WorldPosition targetPoint;
		if (ParseCanonicalWorldPosition(targetValue, targetPoint, coordinateError))
		{
			const TerraEngine::Scene::WorldLocalizationResult localized =
				TerraEngine::Scene::LocalizeWorldPosition(targetPoint, position, TerraEngine::Scene::kTransformSpaceMaximumRelativeRadiusMetres);
			if (!localized.Succeeded())
			{
				return BuildError(requestId, ErrorCategory::Validation, "Custom camera target exceeds the bounded camera working frame.");
			}
			forward = glm::vec3(localized.offsetMetres);
		}
		else if (!targetValue.is_null())
		{
			return BuildError(requestId, ErrorCategory::Validation,
				"Custom camera lookAt must be a canonical WorldPosition object.");
		}
		else if (const std::optional<glm::vec3> requestedForward = ReadVec3(camera.value("forward", nlohmann::json())))
		{
			forward = *requestedForward;
		}
		if (glm::length(forward) < 0.0001f)
		{
			return BuildError(requestId, ErrorCategory::Validation, "Custom camera forward direction must be non-zero.");
		}
		// glm::lookAt requires an up direction independent from forward.  MCP
		// callers commonly use world-up, so a camera aimed straight up/down would
		// otherwise produce a degenerate basis and invalid capture matrices.
		const glm::vec3 normalizedForward = glm::normalize(forward);
		glm::vec3 normalizedUp = glm::normalize(up);
		bool upFallbackApplied = false;
		if (std::abs(glm::dot(normalizedForward, normalizedUp)) > 0.999f)
		{
			// Select the world axis least parallel to the requested forward vector;
			// crossing twice gives a deterministic perpendicular, unit-length up.
			const glm::vec3 axis = std::abs(normalizedForward.x) <= std::abs(normalizedForward.y)
				&& std::abs(normalizedForward.x) <= std::abs(normalizedForward.z)
				? glm::vec3(1.0f, 0.0f, 0.0f)
				: (std::abs(normalizedForward.y) <= std::abs(normalizedForward.z)
					? glm::vec3(0.0f, 1.0f, 0.0f)
					: glm::vec3(0.0f, 0.0f, 1.0f));
			const glm::vec3 side = glm::normalize(glm::cross(normalizedForward, axis));
			normalizedUp = glm::normalize(glm::cross(side, normalizedForward));
			upFallbackApplied = true;
		}

		const glm::uvec2 size(validation["width"].get<std::uint32_t>(), validation["height"].get<std::uint32_t>());
		RenderViewType captureViewType = RenderViewType::Game;
		// The strict custom-camera envelope keeps all camera semantics together;
		// read viewType from that descriptor while retaining the default Game view.
		if (camera.contains("viewType"))
		{
			const std::optional<RenderViewType> parsed = ParseAutomationRenderViewType(camera["viewType"]);
			if (!parsed || (*parsed != RenderViewType::Game && *parsed != RenderViewType::Scene))
			{
				return BuildError(requestId, ErrorCategory::Validation, "viewType must be Game or Scene.");
			}
			captureViewType = *parsed;
		}
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
		request.viewType = captureViewType;
		request.sourceName = arguments.value("name", std::string("Custom Camera"));
		request.targetSize = size;
		request.format = validation["format"].get<std::string>();
		request.freshness = validation["freshness"].get<std::string>();
		request.includeOverlays = validation["includeOverlays"].get<bool>();
		request.transparentBackground = validation["transparentBackground"].get<bool>();
		request.allowBackendFallback = validation["allowBackendFallback"].get<bool>();
		request.timeoutMs = validation["timeoutMs"].get<std::uint32_t>();
		request.cameraCanonicalPosition = position;
		request.viewMatrix = glm::lookAt(glm::vec3(0.0f), normalizedForward, normalizedUp);
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
			{"position", SerializeCanonicalWorldPosition(position)},
			{"forward", SerializeVec3(normalizedForward)},
			{"up", SerializeVec3(normalizedUp)},
			{"upFallbackApplied", upFallbackApplied},
			{"projection", projection},
			{"verticalFovDegrees", fov},
			{"orthographicHeight", orthographicHeight},
			{"aspectRatio", aspect},
			{"nearPlane", nearPlane},
			{"farPlane", farPlane},
			{"viewType", ToString(captureViewType)}
		};
		return result;
	}

	nlohmann::json EditorAutomationService::HandleRendererEfficiency(int action, const nlohmann::json& arguments, const std::string& requestId)
	{
		using namespace TerraEngine::Renderer;
		std::lock_guard lock(rendererEfficiencyMutex);
		if(!editorContext || !editorContext->engine) return BuildError(requestId, ErrorCategory::MissingTarget, "Renderer is unavailable.");
		auto* renderer = editorContext->engine->GetSystem<RenderSystem>();
		if(!renderer) return BuildError(requestId, ErrorCategory::MissingTarget, "RenderSystem is not registered.");
		std::string error;
		if(action == 0)
		{
			RenderViewType view = RenderViewType::Game;
			if(arguments.contains("viewType"))
			{
				if(!arguments["viewType"].is_string()) return BuildError(requestId, ErrorCategory::Validation, "viewType must be Game or Scene.");
				const auto type = arguments["viewType"].get<std::string>();
				if(type != "Game" && type != "Scene") return BuildError(requestId, ErrorCategory::Validation, "viewType must be Game or Scene.");
				view = type == "Game" ? RenderViewType::Game : RenderViewType::Scene;
			}
			// Numeric selectors are validated before conversion; negatives and
			// oversized values must never wrap into a different view's identity.
			std::uint64_t viewId = 0, after = 0;
			for(const char* key : {"viewId", "afterFrameSerial"})
			{
				if(!arguments.contains(key)) continue;
				const auto& value = arguments[key];
				if(!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
					return BuildError(requestId, ErrorCategory::Validation, std::string(key) + " must be a nonnegative integer.");
				const auto number = value.get<std::uint64_t>();
				if(std::string_view(key) == "viewId") viewId = number; else after = number;
			}
			if(viewId > UINT32_MAX) return BuildError(requestId, ErrorCategory::Validation, "viewId exceeds uint32.");
			const auto evidence = renderer->GetLatestRendererEfficiencyEvidence(view, static_cast<std::uint32_t>(viewId));
			auto data = SerializeRendererEfficiencyEvidence(evidence);
			data["fresh"] = evidence.frameSerial > after;
			data["publicationStatus"] = data["status"];
			return BuildSuccess(requestId, "Read copied renderer efficiency evidence.", std::move(data));
		}
		if(action == 1 || action == 2)
		{
			RendererEfficiencyOptionsReceipt receipt;
			if(action == 1)
			{
				RendererEfficiencyOptions options;
				if(!ParseRendererEfficiencyOptions(arguments, renderer->GetRendererEfficiencyOptionsSnapshot().options, options, error))
					return BuildError(requestId, ErrorCategory::Validation, error);
				receipt = renderer->QueueRendererEfficiencyOptions(options);
			}
			else if(arguments.contains("priorToken"))
			{
				RendererEfficiencyOptionsToken token;
				if(!ParseRendererEfficiencyToken(arguments["priorToken"], token, error)) return BuildError(requestId, ErrorCategory::Validation, error);
				receipt = renderer->QueueRendererEfficiencyRestore(token);
			}
			else receipt = renderer->QueueRendererEfficiencyOptions({});
			if(!receipt.accepted) return BuildError(requestId, ErrorCategory::Unavailable, receipt.error);
			return BuildSuccess(requestId, "Queued transient options for the next protected renderer capture.",
				{{"requestedGeneration", receipt.requestedGeneration}, {"queuedSettingsGeneration", receipt.requestedGeneration},
					{"priorToken", SerializeRendererEfficiencyToken(receipt.priorToken)}, {"visibilityBoundary", "protected_render_settings_capture"}});
		}
		if(action == 3)
		{
			if(!editorContext->projectManager || !BuildAutomationStateSnapshot().sceneAvailable)
				return BuildError(requestId, ErrorCategory::MissingTarget, "An owned fixture project and scene must be open.");
			if(rendererEfficiencyValidation.Active()) return BuildError(requestId, ErrorCategory::Conflict, "A validation session is already active.");
			if(rendererEfficiencyShaderWork)
			{
				ScheduleRendererEfficiencyShaderRestoreAtProtectedFrame();
				if(rendererEfficiencyShaderWork->restoreJob.valid())
				{
					if(rendererEfficiencyShaderWork->restoreJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
						return BuildError(requestId, ErrorCategory::Conflict, "Prior owned default shader restoration is still pending.");
					const auto [restored, reason] = rendererEfficiencyShaderWork->restoreJob.get();
					if(!restored) { rendererEfficiencyShaderWork.reset(); return BuildError(requestId, ErrorCategory::Unavailable, reason); }
				}
				rendererEfficiencyShaderWork.reset();
			}
			if(!rendererEfficiencyValidation.Start(editorContext->projectManager->GetProjectRoot(), BuildAutomationStateSnapshot().projectGeneration,
				arguments, renderer->GetRendererEfficiencyOptionsSnapshot(), error)) return BuildError(requestId, ErrorCategory::Validation, error);
			if(!rendererEfficiencyValidation.InstalledSteps().is_null() && editorContext->assetDatabase)
			{
				// The normal asset pipeline publishes results during its protected
				// update. Queueing an import never counts as bootstrap completion.
				try
				{
					const auto ownedAssets = std::filesystem::canonical(editorContext->projectManager->GetProjectRoot() / "Assets");
					if(std::filesystem::canonical(editorContext->assetDatabase->GetAssetRoot()) != ownedAssets)
						throw std::runtime_error("Asset pipeline root does not match the owned validation project.");
					for(const auto& asset : rendererEfficiencyValidation.InstalledSteps().at("assets"))
					{
						const std::filesystem::path relative(asset.at("sourcePath").get<std::string>());
						if(relative.empty() || relative.is_absolute()) throw std::runtime_error("Owned import source must be Assets-relative.");
						std::filesystem::path source = ownedAssets;
						for(const auto& component : relative)
						{
							if(component == "." || component == "..") throw std::runtime_error("Owned import source cannot escape Assets.");
							source /= component;
							if(std::filesystem::is_symlink(source)) throw std::runtime_error("Owned import source redirects outside its project.");
#if defined(_WIN32)
							const auto attributes = GetFileAttributesW(source.c_str());
							if(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
								throw std::runtime_error("Owned import source traverses a junction/reparse point.");
#endif
						}
						if(!std::filesystem::is_regular_file(source)) throw std::runtime_error("Owned import source is not a regular file.");
						if(!rendererEfficiencyValidation.ValidateOwnedAdmission(editorContext->projectManager->GetProjectRoot(), BuildAutomationStateSnapshot().projectGeneration,
							rendererEfficiencyValidation.Status().at("operationId").get<std::string>(), error)) throw std::runtime_error(error);
						// Force the normal importer to publish this owned source, even if
						// a staged placeholder previously had a matching timestamp.
						editorContext->assetDatabase->ImportAssetWithProgress(source, true);
					}
				}
				catch(const std::exception& exception)
				{
					std::string ignored;
					rendererEfficiencyValidation.Stop(rendererEfficiencyValidation.Status().at("operationId").get<std::string>(), ignored);
					return BuildError(requestId, ErrorCategory::Unavailable, exception.what());
				}
			}
			return BuildSuccess(requestId, "Opened owned validation session; stage GPU acceptance remains unavailable until measured.", rendererEfficiencyValidation.Status());
		}
		if(!arguments.contains("operationId") || !arguments["operationId"].is_string())
			return BuildError(requestId, ErrorCategory::Validation, "operationId is required.");
		const auto operationId = arguments["operationId"].get<std::string>();
		if(!rendererEfficiencyValidation.HasOperation(operationId)) return BuildError(requestId, ErrorCategory::MissingTarget, "Validation operation was not found.");
		if(action == 5)
		{
			// Stop production even when recovery invalidates restoration. Reporting
			// that failure must not leave an active session blocking future starts.
			if(!rendererEfficiencyValidation.Stop(operationId, error)) return BuildError(requestId, ErrorCategory::MissingTarget, error);
			rendererEfficiencyValidation.CancelPendingFixtureWorkAtProtectedFrame(TerraEngine::Scene::SceneMutationQueue::Instance());
			renderer->ClearRendererEfficiencyFixtureIntent(operationId);
			ScheduleRendererEfficiencyShaderRestoreAtProtectedFrame();
			std::string sourceRestoreError;
			const bool sourceRestored = RestoreRendererEfficiencySourcesAtProtectedFrame(sourceRestoreError);
			RendererEfficiencyOptionsReceipt restore;
			if(rendererEfficiencyValidation.PriorToken().valid)
				restore = renderer->QueueRendererEfficiencyRestore(rendererEfficiencyValidation.PriorToken());
			else restore.error = "Prior options token belongs to a retired renderer options session.";
			auto data = rendererEfficiencyValidation.Status();
			data["shaderRestorationStatus"] = rendererEfficiencyShaderWork && rendererEfficiencyShaderWork->restoreJob.valid() ? "Pending" : "NotRequired";
			data["sourceRestorationStatus"] = sourceRestored ? "Restored" : "Unavailable";
			data["sourceRestorationError"] = sourceRestoreError;
			data["restorationStatus"] = restore.accepted ? "Queued" : "Unavailable";
			data["restorationError"] = restore.error;
			data["requestedGeneration"] = restore.accepted ? nlohmann::json(restore.requestedGeneration) : nlohmann::json(nullptr);
			return BuildSuccess(requestId, restore.accepted ? "Stopped owned validation session and queued its prior options."
				: "Stopped owned validation session; prior options restoration is unavailable.", std::move(data));
		}
		return BuildSuccess(requestId, "Read validation progress without advancing fixture steps.", rendererEfficiencyValidation.Status());
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
			if (*parsedViewType != RenderViewType::Game && *parsedViewType != RenderViewType::Scene
				&& *parsedViewType != RenderViewType::Shadow)
			{
				return BuildError(requestId, ErrorCategory::Validation,
					"viewType must be Game, Scene, or Shadow.");
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

		// Vulkan's fence-delayed RenderStatsSnapshot storage owns Game and Scene
		// timing slots only. Shadow selection is nevertheless published as an
		// immutable RenderSystem diagnostic snapshot, so never ask the two-view
		// stats store for Shadow and accidentally return Game data as proof.
		RenderStatsSnapshot stats{};
		const bool shadowDiagnosticsOnly = viewType == RenderViewType::Shadow;
		if (!shadowDiagnosticsOnly)
		{
			stats = renderSystem->GetLatestRenderStats(viewType);
			if (!stats.valid)
			{
				return BuildError(requestId, ErrorCategory::MissingTarget,
					"No render stats have been produced for the requested view.", ToString(viewType));
			}
		}
		else
		{
			stats.viewType = RenderViewType::Shadow;
		}

		// Return both the raw meshlet counters and their provenance. The same
		// numbers may come from prepared CPU counts during warmup or true GPU
		// readback after the task shader has completed, so automation must not
		// guess which path it is observing.
		nlohmann::json performanceGates = nlohmann::json::array();
		nlohmann::json pageResidency = nlohmann::json::object();
		nlohmann::json animatedVirtualGeometry = {
			{"pageDemands", 0u},
			{"uniqueDispatches", 0u},
			{"reuseCount", 0u},
			{"animationLods", nlohmann::json::array()},
			{"skippedWorkCount", 0u},
			{"boundsSource", "NoAnimatedPageWork"},
			{"fallbackCount", 0u},
			{"currentBytes", 0u},
			{"historyBytes", 0u},
			{"boundsBytes", 0u},
			{"highWaterBytes", 0u},
			{"shaderContractValid", true},
			{"fallbackReason", ""},
			{"diagnosticEmitted", 0u},
			{"diagnosticCoalesced", 0u},
			{"diagnosticDropped", 0u}
		};
		std::uint32_t selectedClusters = 0u;
		std::uint32_t fallbackSelections = 0u;
		std::uint32_t hardwareRasterPackets = 0u;
		std::uint32_t computeRasterPackets = 0u;
		std::uint32_t indexedRasterPackets = 0u;
		nlohmann::json selectedLodHistogram = nlohmann::json::array();
		bool foundRequestedViewDiagnostics = false;
		std::uint64_t requestedViewDiagnosticsFrameId = 0u;
		const TerraEngine::Renderer::RenderDiagnosticsSummary diagnostics = renderSystem->GetLatestRenderGraphDiagnostics();
		for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& view : diagnostics.virtualGeometryViews)
		{
			if (view.viewType != viewType)
			{
				continue;
			}
			foundRequestedViewDiagnostics = true;
			requestedViewDiagnosticsFrameId = view.frameId;
			if (shadowDiagnosticsOnly)
			{
				// Shadow work has no per-view Vulkan timestamp slot. Copy only the
				// renderer-published selection data, preserving its counter source so
				// MCP clients cannot confuse CPU preparation with completed GPU work.
				stats.valid = view.frameId != 0u;
				stats.viewType = RenderViewType::Shadow;
				stats.renderSize = view.hiZRenderSize;
				stats.meshletsTotal = view.selectedMeshlets;
				stats.meshletsDispatched = view.dispatchedMeshlets;
				stats.meshletsCulled = view.culledMeshlets;
				stats.meshletsCulledHiZ = view.culledByHiZ;
				stats.virtualGeometryClusterPackets = view.clusterPacketCount;
				stats.virtualGeometrySubmittedBatches = view.submittedBatchCount;
				stats.virtualGeometrySinglePacketFallbackBatches = view.singlePacketFallbackBatchCount;
				stats.virtualGeometryCandidatePackets = view.candidateClusterCount;
				stats.virtualGeometryCompactedPackets = view.compactedClusterCount;
				stats.virtualGeometryRejectedPackets = view.rejectedClusterCount;
				stats.virtualGeometryFailOpenPackets = view.failOpenClusterCount;
				stats.virtualGeometryResidentPages = view.residentPages;
				stats.virtualGeometryRequestedPages = view.requestedPages;
				stats.virtualGeometryMissingPages = view.missingPages;
				stats.virtualGeometryFailedPages = view.failedPages;
				stats.virtualGeometryEvictedPages = view.evictedPages;
				stats.virtualGeometryPinnedFallbackPages = view.pinnedFallbackPages;
				stats.meshletCounterSource = view.counterSource;
				stats.meshletCounterFrameId = view.counterReadbackFrameId != 0u
					? view.counterReadbackFrameId : view.counterPreparedFrameId;
				stats.meshletCounterLatencyFrames = view.counterReadbackLatencyFrames;
				stats.meshletCounterReason = view.counterUnavailableReason;
				stats.hiZActive = false;
				stats.hiZState = view.hiZState;
				stats.hiZStatusReason = view.hiZInvalidReason;
				stats.hiZWarmupFrames = view.hiZWarmupFrames;
				stats.hiZBaselineMatched = view.hiZBaselineMatched;
				stats.hiZFalseOcclusionRiskWarnings = view.hiZFalseOcclusionRiskWarnings;
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
			// The direct stats tool must expose the same immutable animated-page
			// snapshot as asset inspection. Keeping one bounded nested object avoids
			// forcing MCP clients to scrape the Resource Residency window.
			animatedVirtualGeometry = {
				{"pageDemands", view.animatedPageDemandCount},
				{"uniqueDispatches", view.animatedUniqueDispatchCount},
				{"reuseCount", view.animatedReuseCount},
				{"animationLods", view.animatedAnimationLods},
				{"skippedWorkCount", view.animatedSkippedWorkCount},
				{"boundsSource", view.animatedBoundsSource},
				{"fallbackCount", view.animatedFallbackCount},
				{"currentBytes", view.deformationCurrentBytes},
				{"historyBytes", view.deformationHistoryBytes},
				{"boundsBytes", view.deformationBoundsBytes},
				{"highWaterBytes", view.deformationHighWaterBytes},
				{"shaderContractValid", view.animatedShaderContractValid},
				{"fallbackReason", view.animatedFallbackReason},
				{"diagnosticEmitted", view.animatedDiagnosticEmitted},
				{"diagnosticCoalesced", view.animatedDiagnosticCoalesced},
				{"diagnosticDropped", view.animatedDiagnosticDropped}
			};
			selectedClusters = view.clusterSelection.selectedClusters;
			fallbackSelections = view.clusterSelection.fallbackClusters;
			hardwareRasterPackets = view.hardwareRasterPacketCount;
			computeRasterPackets = view.computeRasterPacketCount;
			indexedRasterPackets = view.indexedRasterPacketCount;
			// Publish the renderer's final per-LOD cluster counts so live MCP
			// validation can prove that distance selection reached a coarse LOD.
			for (const std::uint32_t selectedCount : view.selectedLodHistogram)
			{
				selectedLodHistogram.push_back(selectedCount);
			}
			break;
		}
		if (shadowDiagnosticsOnly && (!foundRequestedViewDiagnostics || !stats.valid))
		{
			return BuildError(requestId, ErrorCategory::MissingTarget,
				"No current Shadow virtual-geometry diagnostics have been published.",
				ToString(viewType));
		}
		for (const PerformanceGateResult& gate : TerraEngine::Renderer::BuildVisibleCommandReusePerformanceGates(stats))
		{
			performanceGates.push_back(SerializeVirtualGeometryPerformanceGate(gate));
		}
		// Streaming runs before RenderSystem and publishes through a protected
		// boundary, so copy its immutable diagnostics instead of inferring worker
		// IO or Vulkan upload completion from renderer selection counters.
		const TerraEngine::Resource::VirtualGeometryStreamingSystem& streamingSystem =
			TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance();
		const TerraEngine::Resource::VirtualGeometryStreamingDiagnostics streamingSnapshot =
			streamingSystem.DiagnosticsSnapshot();
		const TerraEngine::Resource::VirtualGeometryStreamingPlan streamingPlan =
			streamingSystem.LastPreparedPlan();
		const TerraEngine::Resource::VirtualGeometryPhysicalPageResidencySnapshot
			streamingResidency = streamingSystem.ResidencySnapshot();
		const std::optional<TerraEngine::Renderer::VirtualGeometryRasterRoute>
			validationRouteOverride = renderSystem->GetVirtualGeometryValidationRouteOverride();
		const nlohmann::json validationRoute = {
			{"requestedRoute", validationRouteOverride
				? nlohmann::json(ToAutomationString(*validationRouteOverride))
				: nlohmann::json(nullptr)},
			{"overrideActive", validationRouteOverride.has_value()},
			{"hybridSceneRequested", renderSystem->IsVirtualGeometryHybridValidationSceneRequested()},
			{"hybridMeaning", "Hybrid is a mixed packet/material scene using Hardware, Compute, and Indexed packets; it is not a backend route."},
			{"executedPacketCounts", {
				{"hardware", hardwareRasterPackets},
				{"compute", computeRasterPackets},
				{"indexed", indexedRasterPackets}
			}},
			{"hybridCoverageComplete",
				hardwareRasterPackets > 0u
					&& computeRasterPackets > 0u
					&& indexedRasterPackets > 0u}
		};
		std::uint32_t pendingDecodePages = 0u;
		std::uint32_t pendingUploadPages = 0u;
		std::uint32_t failedDecodePages = 0u;
		for (const TerraEngine::Resource::VirtualGeometryStreamingDecodedPage& decoded :
			streamingPlan.decodedPages)
		{
			switch (decoded.status)
			{
			case TerraEngine::Resource::VirtualGeometryStreamingDecodeStatus::PendingDecode:
				++pendingDecodePages;
				break;
			case TerraEngine::Resource::VirtualGeometryStreamingDecodeStatus::PendingUpload:
				++pendingUploadPages;
				break;
			case TerraEngine::Resource::VirtualGeometryStreamingDecodeStatus::Failed:
				++failedDecodePages;
				break;
			case TerraEngine::Resource::VirtualGeometryStreamingDecodeStatus::NotRequested:
			case TerraEngine::Resource::VirtualGeometryStreamingDecodeStatus::Cancelled:
				break;
			}
		}
		nlohmann::json streamingEvents = nlohmann::json::array();
		for (const TerraEngine::Resource::VirtualGeometryStreamingDiagnosticEvent& event :
			streamingSnapshot.recentEvents)
		{
			streamingEvents.push_back({
				{"frameId", event.frameId},
				{"pageId", event.pageId},
				{"stage", event.stage},
				{"detail", event.detail},
				{"failure", event.failure}
			});
		}
		nlohmann::json streamingRequests = nlohmann::json::array();
		for (const TerraEngine::Resource::VirtualGeometryStreamingFeedback& streamingRequest :
			streamingPlan.requests)
		{
			// Exact-range ownership is the decisive live-streaming handoff. Expose
			// the ordered block count too, so automation can distinguish a valid
			// multi-block page from a missing mesh-metadata handoff.
			streamingRequests.push_back({
				{"assetId", TerraEngine::ToHexString(streamingRequest.assetId)},
				{"pageId", streamingRequest.pageId},
				{"fallbackPageId", streamingRequest.fallbackPageId},
				{"clusterId", streamingRequest.clusterId},
				{"hasExactBlockReads", !streamingRequest.exactBlockReads.empty()},
				{"exactBlockCount", streamingRequest.exactBlockReads.size()},
				{"expectedStoredBytes", streamingRequest.expectedStoredByteCount},
				{"expectedDecodedBytes", streamingRequest.expectedDecodedByteCount},
				{"canonicalPageReadDiagnostic", streamingRequest.canonicalPageReadDiagnostic}
			});
		}
		const nlohmann::json streaming = {
			{"submittedRequests", streamingSnapshot.submittedRequests},
			{"expandedDependencyRequests", streamingSnapshot.expandedDependencyRequests},
			{"preparedDecodeIntents", streamingSnapshot.preparedDecodeIntents},
			{"preparedUploadIntents", streamingSnapshot.preparedUploadIntents},
			{"completedReads", streamingSnapshot.completedReads},
			{"cancelledReads", streamingSnapshot.cancelledReads},
			{"submittedPublicationCandidates", streamingSnapshot.submittedPublicationCandidates},
			{"acceptedPublicationBatches", streamingSnapshot.acceptedPublicationBatches},
			{"acceptedPublicationPages", streamingSnapshot.acceptedPublicationPages},
			{"rejectedPublications", streamingSnapshot.rejectedPublications},
			{"visibleResidentPages", streamingSnapshot.visibleResidentPages},
			{"lastPreparedFrame", streamingSnapshot.lastPreparedFrame},
			{"lastPublicationFrame", streamingSnapshot.lastPublicationFrame},
			{"lastRejectionReason", streamingSnapshot.lastRejectionReason},
			{"currentRequestCount", streamingPlan.requests.size()},
			{"currentDecodeIntentCount", streamingPlan.decodeIntents.size()},
			{"currentUploadIntentCount", streamingPlan.uploadIntents.size()},
			{"currentDecodedPageCount", streamingPlan.decodedPages.size()},
			{"pendingDecodePages", pendingDecodePages},
			{"pendingUploadPages", pendingUploadPages},
			{"failedDecodePages", failedDecodePages},
			{"physicalResidencyRows", streamingResidency.pages.size()},
			{"physicalResidencyGeneration", streamingResidency.publicationGeneration},
			{"currentRequests", std::move(streamingRequests)},
			{"recentEvents", std::move(streamingEvents)}
		};
		nlohmann::json data = {
			{"viewType", ToString(stats.viewType)},
			{"outputKind", static_cast<std::uint32_t>(stats.outputKind)},
			{"outputId", stats.outputId},
			{"dedicatedCaptureOutput", stats.dedicatedCaptureOutput},
			{"publicationFrameId", stats.publicationFrameId},
			{"publicationGeneration", stats.publicationGeneration},
			{"rendererSessionGeneration", stats.rendererSessionGeneration},
			{"valid", stats.valid},
			// Shadow selection has current immutable diagnostics but intentionally
			// no per-view GPU timestamp/readback slot. Make that boundary explicit
			// instead of presenting default zero timing values as a measurement.
			{"statsAvailability", shadowDiagnosticsOnly
				? "current_shadow_selection_diagnostics"
				: "fence_delayed_render_stats"},
			{"diagnosticsFrameId", shadowDiagnosticsOnly
				? requestedViewDiagnosticsFrameId : 0u},
			{"frameTimeMs", stats.frameTimeMs},
			{"timings", {
				{"preparationMs", static_cast<double>(
					stats.optimization.topologyBuildNanoseconds
					+ stats.optimization.dynamicMaterializationNanoseconds) / 1000000.0},
				{"topologyBuildMs", static_cast<double>(
					stats.optimization.topologyBuildNanoseconds) / 1000000.0},
				{"dynamicMaterializationMs", static_cast<double>(
					stats.optimization.dynamicMaterializationNanoseconds) / 1000000.0},
				{"visibleInstanceCount", stats.optimization.visibleInstanceCount},
				{"optimizationValid", stats.optimization.valid != 0U}
			}},
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
			// Route decisions are immutable frame diagnostics. Publishing them here
			// lets installed automation prove that a low-poly capture used the
			// conventional direct path and suppressed every VG-only workload.
			{"geometryRoute", stats.geometryRoute},
			{"geometryRouteSource", stats.geometryRouteSource},
			{"geometryRoutePolicyIdentity", stats.geometryRoutePolicyIdentity},
			{"geometryRouteCalibrationIdentity", stats.geometryRouteCalibrationIdentity},
			{"geometryRouteHysteresisFrames", stats.geometryRouteHysteresisFrames},
			{"geometryRouteDirectBatchCount", stats.geometryRouteDirectBatchCount},
			{"geometryRouteSharedInstanceCount", stats.geometryRouteSharedInstanceCount},
			{"geometryRouteSuppressedPageRequests", stats.geometryRouteSuppressedPageRequests},
			{"geometryRouteSuppressedDecodeBlocks", stats.geometryRouteSuppressedDecodeBlocks},
			{"geometryRouteSuppressedResidencyRecords", stats.geometryRouteSuppressedResidencyRecords},
			{"geometryRouteFallbackReason", stats.geometryRouteFallbackReason},
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
			{"selectedClusters", selectedClusters},
			{"validationRoute", validationRoute},
			{"selectedLodHistogram", std::move(selectedLodHistogram)},
			{"fallbackSelections", fallbackSelections},
			{"animatedVirtualGeometry", std::move(animatedVirtualGeometry)},
			{"streaming", streaming},
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

	nlohmann::json EditorAutomationService::HandleRendererVirtualGeometryValidationRouteSet(
		const nlohmann::json& arguments,
		const std::string& requestId)
	{
		if (!arguments.contains("route") || !arguments["route"].is_string())
		{
			return BuildError(requestId, ErrorCategory::Validation,
				"route must be Hardware, Compute, Indexed, or Hybrid.");
		}
		const std::string requestedRoute = LowerAscii(arguments["route"].get<std::string>());
		const std::optional<TerraEngine::Renderer::VirtualGeometryRasterRoute> route =
			ParseVirtualGeometryValidationRoute(arguments["route"]);
		if (requestedRoute != "hybrid" && !route)
		{
			// Validate the complete route selector before consulting live state so
			// malformed requests never degrade into misleading renderer errors.
			return BuildError(requestId, ErrorCategory::Validation,
				"route must be Hardware, Compute, Indexed, or Hybrid.");
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

		if (requestedRoute == "hybrid")
		{
			// Hybrid is evidence from a deliberately mixed packet/material scene,
			// not a fourth Vulkan backend or a synthetic forced packet route.
			renderSystem->SetVirtualGeometryHybridValidationSceneRequested(true);
			return BuildSuccess(requestId,
				"Requested mixed packet/material hybrid validation; no synthetic backend route was enabled.",
				{{"requestedRoute", "Hybrid"},
					{"overrideActive", false},
					{"hybridSceneRequested", true},
					{"diagnostic", "Use a scene containing hardware-compatible and compute-forced materials; inspect emitted packet routes."}});
		}

		std::string unavailableReason;
		if (!renderSystem->SetVirtualGeometryValidationRouteOverride(*route, &unavailableReason))
		{
			return BuildError(requestId, ErrorCategory::Unavailable,
				unavailableReason.empty()
					? "The requested Vulkan validation route is unavailable."
					: unavailableReason);
		}
		return BuildSuccess(requestId, "Set transient Vulkan virtual-geometry validation route.", {
			{"requestedRoute", ToAutomationString(*route)},
			{"overrideActive", true},
			{"hybridSceneRequested", false},
			{"diagnostic", "The route is copied into prepared packets; incompatible materials use an explicit indexed fallback."}
		});
	}

	nlohmann::json EditorAutomationService::HandleRendererVirtualGeometryValidationRouteReset(
		const nlohmann::json&,
		const std::string& requestId)
	{
		if (editorContext == nullptr || editorContext->engine == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "Renderer is not available.");
		}
		RenderSystem* renderSystem = editorContext->engine->GetSystem<RenderSystem>();
		if (renderSystem == nullptr)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget, "RenderSystem is not registered.");
		}
		// Reset is idempotent so installed validation runs always restore the
		// ordinary material-driven route policy, even after a failed route test.
		renderSystem->ClearVirtualGeometryValidationRouteOverride();
		return BuildSuccess(requestId, "Cleared transient Vulkan virtual-geometry validation routing.", {
			{"overrideActive", false},
			{"hybridSceneRequested", false}
		});
	}

	nlohmann::json EditorAutomationService::HandleRendererVirtualGeometryAuthoritativePick(
		const nlohmann::json& arguments,
		const std::string& requestId)
	{
		RenderViewType viewType = RenderViewType::Scene;
		if (arguments.contains("viewType"))
		{
			const std::optional<RenderViewType> parsed = ParseAutomationRenderViewType(arguments["viewType"]);
			if (!parsed || (*parsed != RenderViewType::Game && *parsed != RenderViewType::Scene))
			{
				return BuildError(requestId, ErrorCategory::Validation, "viewType must be Game or Scene.");
			}
			viewType = *parsed;
		}
		if (!arguments.contains("pixel"))
		{
			return BuildError(requestId, ErrorCategory::Validation, "pixel must be a [x,y] uint32 array.");
		}
		const std::optional<glm::uvec2> pixel = ParseAutomationPixel(arguments["pixel"]);
		if (!pixel)
		{
			return BuildError(requestId, ErrorCategory::Validation, "pixel must be a [x,y] uint32 array.");
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

		const bool request = arguments.value("request", false);
		const bool captureTarget = arguments.value("capture", false);
		if (request)
		{
			// Queueing is non-blocking. A false return is represented below as an
			// unavailable state, never as a fabricated CPU-projection pick.
			(void)renderSystem->RequestConventionalVisibilityIdentityPick(viewType, *pixel, captureTarget);
		}
		const TerraEngine::Renderer::AuthoritativeVisibilityPickState state =
			renderSystem->GetConventionalVisibilityIdentityPickState(viewType, *pixel, captureTarget);
		return BuildSuccess(requestId, "Read asynchronous Vulkan authoritative identity-pick state.", {
			{"viewType", ToString(viewType)},
			{"requested", request},
			{"captureTarget", captureTarget},
			{"authoritative", SerializeAuthoritativeVisibilityPick(state)}
		});
	}

	nlohmann::json EditorAutomationService::HandleRendererSparseTransformStart(
		const nlohmann::json& arguments,
		const std::string& requestId)
	{
		if(!arguments.contains("expectedObjectCount")
			|| !arguments["expectedObjectCount"].is_number_integer())
		{
			return BuildError(requestId, ErrorCategory::Validation,
				"expectedObjectCount must be a positive integer.");
		}
		const std::int64_t requestedCount = arguments["expectedObjectCount"].get<std::int64_t>();
		const double requestedPercent = arguments.value("percentPerFrame", 1.0);
		const double requestedDisplacement = arguments.value("displacement", 0.01);
		if(requestedCount < 1 || requestedCount > 100000
			|| !std::isfinite(requestedPercent) || requestedPercent <= 0.0
			|| requestedPercent > 100.0
			|| !std::isfinite(requestedDisplacement) || requestedDisplacement <= 0.0
			|| requestedDisplacement > 10.0)
		{
			return BuildError(requestId, ErrorCategory::Validation,
				"Sparse transform bounds are invalid.");
		}

		std::lock_guard lock(sparseTransformMutationMutex);
		if(sparseTransformMutation.active)
		{
			return BuildError(requestId, ErrorCategory::Conflict,
				"A sparse transform validation operation is already active.");
		}

		ObjectManager& objects = ObjectManager::instance();
		std::vector<SparseTransformTarget> targets;
		for(const Object& object : objects.getObjectsWithComponentsIncludingInactive<Transform, MeshRenderComponent>())
		{
			if(!objects.isObjectStatic(object))
			{
				continue;
			}
			const Transform* transform = objects.GetComponent<Transform>(object);
			if(transform == nullptr)
			{
				continue;
			}
			targets.push_back({objects.getUUID(object), transform->worldPose});
		}
		std::sort(targets.begin(), targets.end(), [](const SparseTransformTarget& left,
			const SparseTransformTarget& right) {
			return TerraEngine::ToHexString(left.objectId) < TerraEngine::ToHexString(right.objectId);
		});
		if(targets.size() != static_cast<std::size_t>(requestedCount))
		{
			return BuildError(requestId, ErrorCategory::Conflict,
				"The active scene does not contain the exact requested static renderable population.");
		}

		SparseTransformMutationState next;
		next.active = true;
		next.operationId = "renderer-sparse-transform-" + std::to_string(nextTransactionId++);
		next.percentPerFrame = static_cast<float>(requestedPercent);
		next.displacement = static_cast<float>(requestedDisplacement);
		next.targets = std::move(targets);
		next.batchSize = std::max<std::size_t>(1U, static_cast<std::size_t>(std::ceil(
			static_cast<double>(next.targets.size()) * requestedPercent / 100.0)));
		sparseTransformMutation = std::move(next);
		return BuildSuccess(requestId, "Started protected sparse transform validation.", {
			{"operationId", sparseTransformMutation.operationId},
			{"active", true},
			{"objectCount", sparseTransformMutation.targets.size()},
			{"percentPerFrame", sparseTransformMutation.percentPerFrame},
			{"batchSize", sparseTransformMutation.batchSize},
			{"visibilityBoundary", "protected_scene_mutation_flush"}
		});
	}

	nlohmann::json EditorAutomationService::HandleRendererSparseTransformStatus(
		const nlohmann::json& arguments,
		const std::string& requestId)
	{
		std::lock_guard lock(sparseTransformMutationMutex);
		const SparseTransformMutationState& state = sparseTransformMutation;
		const std::string requestedId = arguments.value("operationId", std::string{});
		if(!requestedId.empty() && requestedId != state.operationId)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget,
				"Sparse transform validation operation was not found.");
		}
		return BuildSuccess(requestId, "Read sparse transform validation status.", {
			{"operationId", state.operationId}, {"active", state.active},
			{"failed", state.failed}, {"failureReason", state.failureReason},
			{"objectCount", state.targets.size()}, {"batchSize", state.batchSize},
			{"percentPerFrame", state.percentPerFrame},
			{"enqueuedFrameCount", state.enqueuedFrameCount},
			{"committedFrameCount", state.committedFrameCount},
			{"enqueuedMutationCount", state.enqueuedMutationCount},
			{"committedMutationCount", state.committedMutationCount},
			{"pendingMutationCount", state.pendingExpected.size()},
			{"visibilityBoundary", "protected_scene_mutation_flush"}
		});
	}

	nlohmann::json EditorAutomationService::HandleRendererSparseTransformStop(
		const nlohmann::json& arguments,
		const std::string& requestId)
	{
		if(!arguments.contains("operationId") || !arguments["operationId"].is_string())
		{
			return BuildError(requestId, ErrorCategory::Validation,
				"operationId is required.");
		}
		std::lock_guard lock(sparseTransformMutationMutex);
		SparseTransformMutationState& state = sparseTransformMutation;
		if(arguments["operationId"].get<std::string>() != state.operationId)
		{
			return BuildError(requestId, ErrorCategory::MissingTarget,
				"Sparse transform validation operation was not found.");
		}
		// Disable production first. If the current protected flush already made
		// the final batch visible, finalize it synchronously; otherwise leave the
		// exact expectations intact for the next protected-frame pump. Callers use
		// the status operation to wait for pendingMutationCount to reach zero.
		state.active = false;
		if(!state.pendingExpected.empty() && !state.failed)
		{
			ObjectManager& objects = ObjectManager::instance();
			bool finalBatchVisible = true;
			for(const TerraEngine::Scene::TransactionTransformUpdate& expected : state.pendingExpected)
			{
				const std::optional<Object> object = objects.getObject(expected.objectId);
				const Transform* transform = object ? objects.GetComponent<Transform>(*object) : nullptr;
				if(transform == nullptr || transform->worldPose.position != expected.pose.position
					|| transform->worldPose.rotation != expected.pose.rotation)
				{
					finalBatchVisible = false;
					break;
				}
			}
			if(finalBatchVisible)
			{
				++state.committedFrameCount;
				state.committedMutationCount += state.pendingExpected.size();
				state.pendingExpected.clear();
			}
		}
		return BuildSuccess(requestId, "Requested protected sparse transform validation stop.", {
			{"operationId", state.operationId}, {"active", false},
			{"failed", state.failed}, {"failureReason", state.failureReason},
			{"objectCount", state.targets.size()}, {"batchSize", state.batchSize},
			{"percentPerFrame", state.percentPerFrame},
			{"enqueuedFrameCount", state.enqueuedFrameCount},
			{"committedFrameCount", state.committedFrameCount},
			{"enqueuedMutationCount", state.enqueuedMutationCount},
			{"committedMutationCount", state.committedMutationCount},
			{"pendingMutationCount", state.pendingExpected.size()},
			{"visibilityBoundary", "protected_scene_mutation_flush"}
		});
	}

	nlohmann::json EditorAutomationService::HandleRendererVirtualGeometryCapabilityEvidence(
		const nlohmann::json& arguments,
		const std::string& requestId)
	{
		// Evidence is scoped to one rendered surface. Rejecting unsupported
		// selectors before reading live state prevents clients from silently
		// combining camera, shadow, or export generations.
		RenderViewType viewType = RenderViewType::Game;
		if(arguments.contains("viewType"))
		{
			const std::optional<RenderViewType> parsedViewType =
				ParseAutomationRenderViewType(arguments["viewType"]);
			if(!parsedViewType
				|| (*parsedViewType != RenderViewType::Game
					&& *parsedViewType != RenderViewType::Scene))
			{
				return BuildError(
					requestId,
					ErrorCategory::Validation,
					"viewType must be Game or Scene.");
			}
			viewType = *parsedViewType;
		}

		if(editorContext == nullptr || editorContext->engine == nullptr)
		{
			return BuildError(
				requestId,
				ErrorCategory::MissingTarget,
				"Renderer is not available.");
		}

		RenderSystem* renderSystem = editorContext->engine->GetSystem<RenderSystem>();
		if(renderSystem == nullptr)
		{
			return BuildError(
				requestId,
				ErrorCategory::MissingTarget,
				"RenderSystem is not registered.");
		}

		const TerraEngine::Renderer::RenderDiagnosticsSummary diagnostics =
			renderSystem->GetLatestRenderGraphDiagnostics();
		const TerraEngine::Renderer::VirtualGeometryCapabilityDiagnosticsSnapshot&
			evidence = diagnostics.virtualGeometryCapabilityDiagnostics;

		nlohmann::json capabilityRows = nlohmann::json::array();
		nlohmann::json measurementRows = nlohmann::json::array();
		nlohmann::json timingRows = nlohmann::json::array();
		nlohmann::json fallbackRows = nlohmann::json::array();
		nlohmann::json evidenceIds = nlohmann::json::array();
		std::string backend = "unknown";
		std::string device;

		for(const TerraEngine::Renderer::VirtualGeometryCapabilityEvidence& capability
			: evidence.capabilities)
		{
			backend = ToAutomationString(capability.key.backend);
			if(!capability.key.deviceIdentity.empty())
			{
				device = capability.key.deviceIdentity;
			}
			const bool executed = capability.CanReportExecuted();
			const bool validated = capability.CanReportValidated();
			capabilityRows.push_back({
				{"stage", ToAutomationString(capability.key.stage)},
				{"backend", ToAutomationString(capability.key.backend)},
				{"route", ToAutomationString(capability.key.route)},
				{"deviceIdentity", capability.key.deviceIdentity},
				{"requiredFeatures", capability.key.requiredFeatures},
				{"planned", capability.planned},
				{"supported", capability.supported},
				{"executed", executed},
				{"validated", validated},
				{"supportReason", capability.supportReason},
				{"queriedLimits", capability.queriedLimits},
				{"evidenceId", executed
					? capability.execution.evidenceId
					: std::string()},
				{"validationArtifactIds", validated
					? capability.validationArtifactIds
					: std::vector<std::string>{}},
				{"source", capability.source},
				{"timestamp", capability.timestamp}
			});
			if(executed)
			{
				evidenceIds.push_back(capability.execution.evidenceId);
			}
			if(capability.fallbackRoute
				!= TerraEngine::Renderer::VirtualGeometryCapabilityRoute::Unknown
				|| !capability.fallbackReason.empty())
			{
				fallbackRows.push_back({
					{"stage", ToAutomationString(capability.key.stage)},
					{"route", ToAutomationString(capability.fallbackRoute)},
					{"reason", capability.fallbackReason}
				});
			}
		}

		for(const TerraEngine::Renderer::VirtualGeometryMeasurementRecord& measurement
			: evidence.measurements)
		{
			const bool authoritative = measurement.HasAuthoritativeDuration();
			measurementRows.push_back({
				{"stage", ToAutomationString(measurement.stage)},
				{"frameId", measurement.identity.frameId},
				{"viewId", measurement.identity.viewId},
				{"rendererGeneration", measurement.identity.rendererGeneration},
				{"availability", ToAutomationString(measurement.availability)},
				{"rawTicks", measurement.rawTicks.has_value()
					? nlohmann::json(*measurement.rawTicks)
					: nlohmann::json(nullptr)},
				{"durationNanoseconds", authoritative
					? nlohmann::json(*measurement.durationNanoseconds)
					: nlohmann::json(nullptr)},
				{"queryIdentity", measurement.queryIdentity},
				{"executionEvidenceId", authoritative
					? measurement.executionEvidenceId
					: std::string()},
				{"reason", measurement.rejectionReason},
				{"warmupOrdinal", measurement.warmupOrdinal},
				{"sampleOrdinal", measurement.sampleOrdinal}
			});
		}

		for(const TerraEngine::Renderer::VirtualGeometryGpuTimingSnapshot& timing
			: evidence.timingSnapshots)
		{
			const bool authoritative = timing.HasAuthoritativeDuration();
			timingRows.push_back({
				{"stage", ToAutomationString(timing.stage)},
				{"frameId", timing.identity.frameId},
				{"viewId", timing.identity.viewId},
				{"rendererGeneration", timing.identity.rendererGeneration},
				{"availability", ToAutomationString(timing.availability)},
				{"durationNanoseconds", authoritative
					? nlohmann::json(*timing.durationNanoseconds)
					: nlohmann::json(nullptr)},
				{"queryIdentity", timing.queryIdentity},
				{"executionEvidenceId", authoritative
					? timing.executionEvidenceId
					: std::string()},
				{"reason", timing.reason},
				{"latencyFrames", timing.latencyFrames}
			});
		}

		return BuildSuccess(
			requestId,
			"Read virtual geometry capability, timing, and evidence.",
			{
				{"schemaVersion", evidence.schemaVersion},
				{"requestedViewType", ToString(viewType)},
				{"frameId", evidence.publicationIdentity.frameId},
				{"viewId", evidence.publicationIdentity.viewId},
				{"rendererGeneration",
					evidence.publicationIdentity.rendererGeneration},
				{"backend", backend},
				{"device", device},
				{"driver", ""},
				{"capabilities", std::move(capabilityRows)},
				{"measurements", std::move(measurementRows)},
				{"timingSnapshots", std::move(timingRows)},
				{"fallbacks", std::move(fallbackRows)},
				{"overflows", nlohmann::json::array()},
				{"diagnostics", {
					{"coalesced", 0u},
					{"dropped", 0u}
				}},
				{"evidenceIds", std::move(evidenceIds)}
			});
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
