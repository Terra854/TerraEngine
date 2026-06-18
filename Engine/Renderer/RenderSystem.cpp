// RenderSystem.cpp — Render System Implementation
//
// Implements the RenderSystem class, coordinating the high-level rendering flow for each frame.
// It handles the transition between different rendering states, manages the active camera, and integrates UI and scene drawing.
// The implementation ensures that scene data is correctly prepared and submitted to the RHI for GPU execution.

#include "RenderSystem.h"
#include "UI/UISystem.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "Core/Logger/Logger.h"
#include "Window/WindowSystem.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/LightComponent.h"
#include "Scene/SceneManager.h"
#include "Core/Object/ObjectManager.h"
#include "Components/Transform.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"
#include "Profiling/Profiler.h"
#include "Profiling/ProfilerScope.h"
#include "Core/Memory/FrameArena.h"
#include "Core/Memory/FlatMap.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "UI/UISystem.h"
#include <entt/entt.hpp>
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <algorithm>
#include <cmath>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

namespace
{
	const glm::vec3 kSkyReferenceSunDirection = glm::normalize(glm::vec3(0.0f, 0.35f, -1.0f));
	constexpr float kDirectionalShadowNear = 0.1f;
	constexpr float kDirectionalShadowFar = 180.0f;
	constexpr float kDirectionalShadowExtent = 40.0f;

	// ResolveRuntimeSettingsDirectory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::filesystem::path ResolveRuntimeSettingsDirectory()
	{
		auto& resourceManager = ResourceManager::Instance();
		// Exported builds keep writable settings beside the packaged runtime,
		// while editor/project sessions must write next to Assets so authored
		// ProjectSettings stay in <project>/ProjectSettings.
		if (resourceManager.IsExportRuntime())
		{
			return resourceManager.GetAssetRoot() / "Settings";
		}

		return resourceManager.GetProjectRoot() / "ProjectSettings";
	}

	// ResolveLegacyAssetSettingsPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::filesystem::path ResolveLegacyAssetSettingsPath()
	{
		auto& resourceManager = ResourceManager::Instance();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (resourceManager.IsExportRuntime())
		{
			return {};
		}

		// Older builds incorrectly wrote render settings inside Assets. Loading
		// that file as a fallback preserves a user's settings once, and the next
		// save writes them back to the corrected project-level directory.
		return resourceManager.GetAssetRoot() / "ProjectSettings" / "RenderSettings.json";
	}

	// SanitizeRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	RenderSettings SanitizeRenderSettings(const RenderSettings& input)
	{
		RenderSettings sanitized = input;
		sanitized.maxUploadedLights = std::max(sanitized.maxUploadedLights, 1u);
		sanitized.maxDynamicLights = std::min(sanitized.maxDynamicLights, sanitized.maxUploadedLights);
		sanitized.maxAreaLights = std::min(sanitized.maxAreaLights, sanitized.maxDynamicLights);
		sanitized.maxShadowLights = std::min(sanitized.maxShadowLights, sanitized.maxDynamicLights);
		// Shadow maps are allocated lazily by the Vulkan backend, so authoring
		// budgets may exceed four lights without paying for unused slots.
		sanitized.maxShadowLights = std::min(sanitized.maxShadowLights, kMaxShadowResourceSlots);
		sanitized.indirectStrength = std::clamp(sanitized.indirectStrength, 0.0f, 8.0f);
		sanitized.ambientOcclusionStrength = std::clamp(sanitized.ambientOcclusionStrength, 0.0f, 4.0f);
		sanitized.ambientOcclusionQuality = std::min(sanitized.ambientOcclusionQuality, 3u);
		sanitized.enableIBL = sanitized.enableIBL > 0u ? 1u : 0u;
		return sanitized;
	}

	// SafeNormalizeDirection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 SafeNormalizeDirection(const glm::vec3& direction, const glm::vec3& fallback)
	{
		if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z))
		{
			return fallback;
		}

		const float length = glm::length(direction);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (length <= 0.0001f)
		{
			return fallback;
		}

		return direction / length;
	}

	// ApplySkyYawOffset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 ApplySkyYawOffset(const glm::vec3& direction, float degrees)
	{
		const glm::quat yawRotation = glm::angleAxis(glm::radians(degrees), glm::vec3(0.0f, 1.0f, 0.0f));
		return SafeNormalizeDirection(yawRotation * direction, kSkyReferenceSunDirection);
	}

	// SafeNormalizeShadowDirection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 SafeNormalizeShadowDirection(const glm::vec3& value, const glm::vec3& fallback)
	{
		const float length = glm::length(value);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!std::isfinite(length) || length <= 0.00001f)
		{
			return fallback;
		}

		return value / length;
	}

	// ComputeDirectionalShadowCenter is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::vec3 ComputeDirectionalShadowCenter(const GPULight& light, const glm::vec3& cameraWorldPosition)
	{
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float texelWorldSize = (kDirectionalShadowExtent * 2.0f) / 1024.0f;

		// Matching the Vulkan light-space snap keeps CPU culling aligned with the
		// exact shadow map coverage that the GPU will render.
		const glm::vec3 provisionalLightPosition = cameraWorldPosition - lightDirection * 60.0f;
		const glm::mat4 provisionalView = glm::lookAtRH(provisionalLightPosition, cameraWorldPosition, worldUp);
		glm::vec4 snappedCenterLightSpace = provisionalView * glm::vec4(cameraWorldPosition, 1.0f);
		snappedCenterLightSpace.x = std::floor(snappedCenterLightSpace.x / texelWorldSize) * texelWorldSize;
		snappedCenterLightSpace.y = std::floor(snappedCenterLightSpace.y / texelWorldSize) * texelWorldSize;
		return glm::vec3(glm::inverse(provisionalView) * snappedCenterLightSpace);
	}

	// BuildDirectionalShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 BuildDirectionalShadowViewProjection(const GPULight& light, const glm::vec3& cameraWorldPosition)
	{
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const glm::vec3 snappedCenterWorld = ComputeDirectionalShadowCenter(light, cameraWorldPosition);
		const glm::vec3 lightPosition = snappedCenterWorld - lightDirection * 60.0f;
		glm::mat4 view = glm::lookAtRH(lightPosition, snappedCenterWorld, worldUp);
		glm::mat4 projection = glm::orthoRH_ZO(-kDirectionalShadowExtent, kDirectionalShadowExtent, -kDirectionalShadowExtent, kDirectionalShadowExtent, kDirectionalShadowNear, kDirectionalShadowFar);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

	// BuildSpotShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 BuildSpotShadowViewProjection(const GPULight& light)
	{
		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float range = std::max(light.directionAndRange.w, 0.5f);
		const float outerConeCos = std::clamp(light.coneAnglesAndArea.y, -0.999f, 0.999f);
		const float outerAngleRadians = std::max(0.1f, std::acos(outerConeCos) * 2.0f);
		glm::mat4 view = glm::lookAtRH(lightPosition, lightPosition + lightDirection, worldUp);
		glm::mat4 projection = glm::perspectiveRH_ZO(outerAngleRadians, 1.0f, 0.1f, range);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

	// BuildAreaShadowViewProjection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	glm::mat4 BuildAreaShadowViewProjection(const GPULight& light)
	{
		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const glm::vec3 lightDirection = SafeNormalizeShadowDirection(glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float range = std::max(light.directionAndRange.w, 0.5f);
		const float width = std::max(light.coneAnglesAndArea.z, 0.5f);
		const float height = std::max(light.coneAnglesAndArea.w, 0.5f);
		const float fov = std::atan2(std::max(width, height), range) * 2.0f;
		glm::mat4 view = glm::lookAtRH(lightPosition, lightPosition + lightDirection, worldUp);
		glm::mat4 projection = glm::perspectiveRH_ZO(fov, 1.0f, 0.1f, range);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

}

// RenderSystem::Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderSystem& RenderSystem::Instance()
{
	// Provide a singleton instance so the main engine loop can register the
	// system without managing heap allocations or ownership semantics.
	static RenderSystem instance;
	return instance;
}

RenderSystem::RenderSystem()
	// objManager is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: objManager(ObjectManager::instance())
{
	latestGameStats.viewType = RenderViewType::Game;
	latestSceneStats.viewType = RenderViewType::Scene;
	latestGameBudgetDiagnostics.viewType = RenderViewType::Game;
	latestSceneBudgetDiagnostics.viewType = RenderViewType::Scene;
	latestGameLightingDiagnostics.viewType = RenderViewType::Game;
	latestSceneLightingDiagnostics.viewType = RenderViewType::Scene;
}

void RenderSystem::SetSceneViewContextTint(const std::unordered_set<TerraEngine::UUID>& focusedObjectIds)
{
	// The editor calls this once per Scene View frame while prefab context mode
	// is active. An empty set disables the cue so stale selections do not tint
	// unrelated views on later frames.
	sceneViewContextFocusedObjectIds = focusedObjectIds;
	sceneViewContextTintEnabled = !sceneViewContextFocusedObjectIds.empty();
}

// RenderSystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::Initialize()
{
	TERRAENGINE_LOG(LogLevel::Info, "Initializing RenderSystem...");
	
	rhi = std::make_unique<VulkanContext>();
	rhi->Initialize(WindowSystem::Instance());

	WindowSystem::Instance().SetResizeCallback([](int width, int height) {
		try {
			auto& renderSystem = RenderSystem::Instance();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (renderSystem.GetContext()) {
				static_cast<VulkanContext*>(renderSystem.GetContext())->Resize(width, height);
			}
		} catch (const std::exception& e) {
			TERRAENGINE_LOG(LogLevel::Error, "Exception in resize callback: " << e.what());
		} catch (...) {
			TERRAENGINE_LOG(LogLevel::Error, "Unknown exception in resize callback");
		}
	});

	LoadSettings();

	initialized = true;
}

template<typename T>
using FrameVector = std::vector<T, FrameAllocator<T>>;

template<typename K, typename V>
using FrameMap = FlatMap<K, V, FrameAllocator<std::pair<K, V>>>;

// RenderSystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::Update(float deltaTime)
{
	if (!initialized) {
		return;
	}
	TERRAENGINE_PROFILE_SCOPE("RenderSystem::Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);

	// Standalone runtime UI should track the real framebuffer size. In the
	// editor, however, the Game View is letterboxed inside an ImGui window and
	// owns its own viewport size, so forcing the full GLFW framebuffer here
	// would make UI layout and hit testing react to the wrong rectangle.
	if (!editorModeEnabled)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Sync Runtime Viewport", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto* windowHandle = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle()))
		{
			int framebufferWidth = 0;
			int framebufferHeight = 0;
			glfwGetFramebufferSize(windowHandle, &framebufferWidth, &framebufferHeight);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (framebufferWidth > 0 && framebufferHeight > 0)
			{
				UISystem::Instance().SetGameViewportSize(glm::vec2(static_cast<float>(framebufferWidth), static_cast<float>(framebufferHeight)));
			}
		}
	}

	// Reset the frame allocator at the start of the frame logic
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reset Frame Arena", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		FrameArena::Instance().Reset();
	}

	// Capture all transforms first so hierarchical world transforms can be derived for cameras and renderables.
	FrameMap<entt::entity, Transform> localTransforms;
	localTransforms.reserve(1000); // Guess initial size

	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Transforms", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		objManager.forEachObjectWithComponents<Transform>(
			[&](Object object, Transform& transform)
			{
				Transform reflectedTransform{};
				reflectedTransform.position = GetComponentProperty<Transform, glm::vec3>(transform, "Position");
				reflectedTransform.rotation = GetComponentProperty<Transform, glm::quat>(transform, "Rotation");
				reflectedTransform.scale = GetComponentProperty<Transform, glm::vec3>(transform, "Scale");
				localTransforms.emplace(object.EntityId(), reflectedTransform);
			}
		);
	}

	// Snapshot camera components so view/projection data is sourced from
	// authored entities instead of the legacy hardcoded values.
	FrameVector<CameraEntry> cameras;
	cameras.reserve(10);

	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Cameras", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		objManager.forEachObjectWithComponents<Transform, CameraComponent>(
			[&](Object object, Transform& transform, CameraComponent& cameraComp)
			{
				CameraComponent reflectedCamera{};
				reflectedCamera.verticalFovDegrees = GetComponentProperty<CameraComponent, float>(cameraComp, "verticalFovDegrees");
				reflectedCamera.aspectRatio = GetComponentProperty<CameraComponent, float>(cameraComp, "aspectRatio");
				reflectedCamera.nearPlane = GetComponentProperty<CameraComponent, float>(cameraComp, "nearPlane");
				reflectedCamera.farPlane = GetComponentProperty<CameraComponent, float>(cameraComp, "farPlane");
				reflectedCamera.up = GetComponentProperty<CameraComponent, glm::vec3>(cameraComp, "up");

				cameras.push_back({ object.EntityId(), reflectedCamera });
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!localTransforms.contains(object.EntityId()))
				{
					Transform reflectedTransform{};
					reflectedTransform.position = GetComponentProperty<Transform, glm::vec3>(transform, "Position");
					reflectedTransform.rotation = GetComponentProperty<Transform, glm::quat>(transform, "Rotation");
					reflectedTransform.scale = GetComponentProperty<Transform, glm::vec3>(transform, "Scale");
					localTransforms.emplace(object.EntityId(), reflectedTransform);
				}
			}
		);
	}
	// Throttle warning spam so missing cameras do not flood the console every frame.
	static bool loggedMissingCamera = false;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (cameras.empty() && !hasSceneViewCamera)
	{
		if (!loggedMissingCamera)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "RenderSystem skipped frame because no CameraComponent was found in the scene.");
			loggedMissingCamera = true;
		}
	}
	else
	{
		loggedMissingCamera = false;
	}

	// Snapshot renderable components to avoid locking the registry during parallel work.
	FrameVector<RenderableEntry> renderables;
	renderables.reserve(1000);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!cameras.empty() || hasSceneViewCamera)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Renderables", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		objManager.forEachObjectWithComponents<Transform, MeshRenderComponent>(
			[&](Object object, Transform& transform, MeshRenderComponent& meshComp)
			{
				const TerraEngine::UUID meshId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "meshId");
				const TerraEngine::UUID materialId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "materialId");
				const TerraEngine::UUID spatialObjectId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "spatialObjectId");
				const bool objectTaskCullingEnabled = GetComponentProperty<MeshRenderComponent, bool>(meshComp, "taskShaderCullingEnabled");
				
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (meshId != TerraEngine::EMPTY_UUID && materialId != TerraEngine::EMPTY_UUID)
				{
					// Ensure resources still exist in the cache to prevent renderer crashes
					auto& rm = ResourceManager::Instance();
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (rm.HasResource(meshId) && rm.HasResource(materialId))
					{
						MeshRenderComponent reflectedMesh{};
						reflectedMesh.meshId = meshId;
						reflectedMesh.materialId = materialId;
						reflectedMesh.spatialObjectId = spatialObjectId;
						reflectedMesh.taskShaderCullingEnabled = objectTaskCullingEnabled;

						const TerraEngine::UUID objectId = spatialObjectId != TerraEngine::EMPTY_UUID ? spatialObjectId : object.getUUID();
						renderables.push_back({ object.EntityId(), objectId, objManager.isObjectStatic(object), reflectedMesh });
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (!localTransforms.contains(object.EntityId()))
						{
							Transform reflectedTransform{};
							reflectedTransform.position = GetComponentProperty<Transform, glm::vec3>(transform, "Position");
							reflectedTransform.rotation = GetComponentProperty<Transform, glm::quat>(transform, "Rotation");
							reflectedTransform.scale = GetComponentProperty<Transform, glm::vec3>(transform, "Scale");
							localTransforms.emplace(object.EntityId(), reflectedTransform);
						}
					}
				}
			}
		);
	}

	// Snapshot scene light components so packing can run in worker tasks without
	// touching the entity registry after this point.
	FrameVector<LightEntry> lights;
	lights.reserve(256);
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Lights", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		objManager.forEachObjectWithComponents<Transform, LightComponent>(
			[&](Object object, Transform& transform, LightComponent& lightComp)
			{
				LightComponent reflectedLight{};
				const std::uint32_t encodedType = GetComponentProperty<LightComponent, std::uint32_t>(lightComp, "type");
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (encodedType > static_cast<std::uint32_t>(LightType::Area))
				{
					// Unsupported legacy light types are intentionally skipped so they
					// are not silently remapped to another runtime behavior.
					return;
				}
				reflectedLight.type = static_cast<LightType>(encodedType);
				
				const std::uint32_t encodedUnit = GetComponentProperty<LightComponent, std::uint32_t>(lightComp, "unit");
				reflectedLight.unit = static_cast<LightUnit>(encodedUnit);

				const std::uint32_t encodedAppearance = GetComponentProperty<LightComponent, std::uint32_t>(lightComp, "appearance");
				reflectedLight.appearance = static_cast<LightAppearance>(encodedAppearance);

				reflectedLight.color = GetComponentProperty<LightComponent, glm::vec3>(lightComp, "color");
				reflectedLight.temperature = GetComponentProperty<LightComponent, float>(lightComp, "temperature");
				reflectedLight.intensity = GetComponentProperty<LightComponent, float>(lightComp, "intensity");
				reflectedLight.range = GetComponentProperty<LightComponent, float>(lightComp, "range");
				reflectedLight.innerConeAngle = GetComponentProperty<LightComponent, float>(lightComp, "innerConeAngle");
				reflectedLight.outerConeAngle = GetComponentProperty<LightComponent, float>(lightComp, "outerConeAngle");
				reflectedLight.width = GetComponentProperty<LightComponent, float>(lightComp, "width");
				reflectedLight.height = GetComponentProperty<LightComponent, float>(lightComp, "height");
				reflectedLight.castsShadows = GetComponentProperty<LightComponent, bool>(lightComp, "castsShadows");
				reflectedLight.shadowStrength = GetComponentProperty<LightComponent, float>(lightComp, "shadowStrength");
				reflectedLight.shadowBias = GetComponentProperty<LightComponent, float>(lightComp, "shadowBias");
				reflectedLight.indirectMultiplier = GetComponentProperty<LightComponent, float>(lightComp, "indirectMultiplier");
				reflectedLight.authoredPriority = GetComponentProperty<LightComponent, float>(lightComp, "authoredPriority");
				reflectedLight.stabilityPreference = GetComponentProperty<LightComponent, float>(lightComp, "stabilityPreference");

				lights.push_back({ object.EntityId(), object.getUUID(), reflectedLight });
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!localTransforms.contains(object.EntityId()))
				{
					Transform reflectedTransform{};
					reflectedTransform.position = GetComponentProperty<Transform, glm::vec3>(transform, "Position");
					reflectedTransform.rotation = GetComponentProperty<Transform, glm::quat>(transform, "Rotation");
					reflectedTransform.scale = GetComponentProperty<Transform, glm::vec3>(transform, "Scale");
					localTransforms.emplace(object.EntityId(), reflectedTransform);
				}
			}
		);
	}

	// Cache of computed world transforms to avoid recomputing during traversal.
	FrameMap<entt::entity, Transform> worldTransforms;
	worldTransforms.reserve(localTransforms.size());

	// Use a set for cycle detection. FlatSet would be better but std::set with FrameAllocator is okayish for small depth,
	// or just use vector and linear scan for small depth. Recursion depth is usually small in scene graphs.
	// Let's use FrameVector for "visiting" stack.
	using VisitingStack = FrameVector<entt::entity>;
	
	auto computeWorldTransform = [&](auto&& self, entt::entity entity, VisitingStack& visiting) -> Transform
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto* cached = worldTransforms.find(entity))
		{
			return *cached;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!localTransforms.contains(entity))
		{
			return Transform{};
		}
		// Prevent cycles from causing infinite recursion.
		// Linear scan is fast for small hierarchies
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::find(visiting.begin(), visiting.end(), entity) != visiting.end()) {
			return localTransforms.at(entity);
		}

		visiting.push_back(entity);

		Transform world = localTransforms.at(entity);
		const entt::entity parent = objManager.getParentEntity(entity);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (parent != entt::null)
		{
			Transform parentWorld = self(self, parent, visiting);
			world.position = parentWorld.position + parentWorld.rotation * (parentWorld.scale * world.position);
			world.rotation = parentWorld.rotation * world.rotation;
			world.scale = parentWorld.scale * world.scale;
		}

		visiting.pop_back();
		worldTransforms.emplace(entity, world);
		return world;
	};

	// Pre-compute world transforms
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Resolve World Transforms", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		VisitingStack visiting;
		visiting.reserve(16);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& entry : renderables)
		{
			computeWorldTransform(computeWorldTransform, entry.entity, visiting);
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& entry : lights)
		{
			computeWorldTransform(computeWorldTransform, entry.entity, visiting);
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& entry : cameras)
		{
			computeWorldTransform(computeWorldTransform, entry.entity, visiting);
		}
	}

	auto makeModelMatrix = [](const Transform& transform) {
		glm::mat4 model = glm::mat4(1.0f);
		model = glm::translate(model, transform.position);
		model = model * glm::mat4_cast(transform.rotation);
		model = glm::scale(model, transform.scale);
		return model;
	};

	std::vector<TerraEngine::Scene::SpatialObjectRecord> spatialRecords;
	std::unordered_map<entt::entity, std::size_t> renderableLookup;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Refresh Spatial Index", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		spatialRecords.reserve(renderables.size());
		renderableLookup.reserve(renderables.size());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::size_t index = 0; index < renderables.size(); ++index)
		{
			const RenderableEntry& entry = renderables[index];
			renderableLookup.emplace(entry.entity, index);
			const Transform& transform = worldTransforms.at(entry.entity);
			const glm::mat4 model = makeModelMatrix(transform);
			TerraEngine::Scene::SpatialAABB localBounds = ResourceManager::Instance().GetMeshLocalBounds(entry.mesh.meshId);
			TerraEngine::Scene::SpatialAABB worldBounds = localBounds.IsUsable()
				? TerraEngine::Scene::TransformAABB(localBounds, model)
				: TerraEngine::Scene::MakeFallbackBounds(transform.position, 1.0f);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!worldBounds.IsUsable())
			{
				worldBounds = TerraEngine::Scene::MakeFallbackBounds(transform.position, 1.0f);
			}

			TerraEngine::Scene::SpatialObjectRecord record{};
			record.objectId = entry.objectId;
			record.entity = entry.entity;
			record.active = true;
			record.isStatic = entry.isStatic;
			record.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable | TerraEngine::Scene::SpatialCategoryEditorSelectable;
			record.worldBounds = worldBounds;
			record.revision = objManager.GetSpatialRevision();
			spatialRecords.push_back(record);
		}
		spatialIndex.Refresh(spatialRecords);
	}

	std::vector<DrawCommand> renderableDrawCommands(renderables.size());
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!renderables.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Build one reusable draw command per renderable so camera views and
		// shadow queries can cheaply select subsets without rebuilding models.
		const size_t workerCount = std::max<size_t>(1, std::thread::hardware_concurrency());
		const size_t taskCount = std::min(renderables.size(), workerCount);
		const size_t chunkSize = (renderables.size() + taskCount - 1) / taskCount;
		std::vector<std::future<void>> futures;
		futures.reserve(taskCount);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex)
		{
			const size_t start = taskIndex * chunkSize;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (start >= renderables.size())
			{
				break;
			}
			const size_t end = std::min(renderables.size(), start + chunkSize);
			auto task = ThreadPool::instance().createTask([start, end, &renderables, &renderableDrawCommands, &worldTransforms, &makeModelMatrix]() {
					TERRAENGINE_PROFILE_SCOPE("RenderSystem Task: Build Draw Command Chunk", TerraEngine::Profiling::ProfilerModuleId::TaskExecution);
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (size_t index = start; index < end; ++index)
				{
					const RenderableEntry& entry = renderables[index];
					const MeshRenderComponent& meshComp = entry.mesh;
					DrawCommand command{};
					command.meshId = meshComp.meshId;
					command.materialId = meshComp.materialId;
					command.objectId = entry.objectId;
					command.taskShaderCullingEnabled = meshComp.taskShaderCullingEnabled;
					command.modelMatrix = makeModelMatrix(worldTransforms.at(entry.entity));
					renderableDrawCommands[index] = command;
				}
			});
			futures.push_back(task->getFuture());
			ThreadPool::instance().enqueue(task);
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& future : futures)
		{
			future.get();
		}
	}

	auto collectCandidateIndices = [&](const TerraEngine::Scene::SpatialQueryResult& query) {
		std::vector<std::size_t> candidateIndices;
		candidateIndices.reserve(query.hits.size());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const TerraEngine::Scene::SpatialQueryHit& hit : query.hits)
		{
			if (const auto lookup = renderableLookup.find(hit.entity); lookup != renderableLookup.end())
			{
				candidateIndices.push_back(lookup->second);
			}
		}
		return candidateIndices;
	};

	auto buildDrawCommandsFromCandidateIndices = [&](const std::vector<std::size_t>& candidateIndices) {
		std::vector<DrawCommand> commands;
		commands.reserve(candidateIndices.size());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const std::size_t candidateIndex : candidateIndices)
		{
			commands.push_back(renderableDrawCommands[candidateIndex]);
		}
		return commands;
	};

	auto buildVisibleDrawCommands = [&](const glm::mat4& viewProjection) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Frustum Cull Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const TerraEngine::Scene::SpatialFrustum frustum = TerraEngine::Scene::ExtractFrustum(viewProjection);
		TerraEngine::Scene::SpatialQueryFilter filter{};
		filter.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;
		filter.collectDiagnostics = true;
		const TerraEngine::Scene::SpatialQueryResult query = spatialIndex.QueryFrustum(frustum, filter);
		return buildDrawCommandsFromCandidateIndices(collectCandidateIndices(query));
	};

	// Pack reflected lights in parallel so large scenes keep the CPU upload path
	// scalable while preserving deterministic ordering and stable light IDs.
	std::vector<FrameLightCandidate> packedLights;
	if (!lights.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Pack Lights", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const size_t workerCount = std::max<size_t>(1, std::thread::hardware_concurrency());
		const size_t taskCount = std::min(lights.size(), workerCount);
		const size_t chunkSize = (lights.size() + taskCount - 1) / taskCount;

		std::vector<std::future<std::vector<FrameLightCandidate>>> futures;
		futures.reserve(taskCount);

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex)
		{
			const size_t start = taskIndex * chunkSize;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (start >= lights.size())
			{
				break;
			}
			const size_t end = std::min(lights.size(), start + chunkSize);

			auto task = ThreadPool::instance().createTask([start, end, &lights, &worldTransforms]() {
				TERRAENGINE_PROFILE_SCOPE("RenderSystem Task: Pack Light Chunk", TerraEngine::Profiling::ProfilerModuleId::TaskExecution);
				std::vector<FrameLightCandidate> chunkLights;
				chunkLights.reserve(end - start);

				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (size_t i = start; i < end; ++i)
				{
					const LightEntry& entry = lights[i];
					const Transform transform = worldTransforms.at(entry.entity);
					const LightComponent& light = entry.light;

					const glm::quat normalizedRotation = glm::normalize(transform.rotation);
					glm::vec3 direction = normalizedRotation * glm::vec3(0.0f, 0.0f, -1.0f);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z) || glm::length(direction) < 1e-5f)
					{
						direction = glm::vec3(0.0f, -1.0f, 0.0f);
					}
					else
					{
						direction = glm::normalize(direction);
					}

					glm::vec3 finalColor = light.color;
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (light.appearance == LightAppearance::FilterAndTemperature)
					{
						finalColor *= LightComponent::KelvinToRGB(light.temperature);
					}

					float clampedIntensity = std::max(light.intensity, 0.0f);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (light.type != LightType::Directional && light.unit == LightUnit::Lumens)
					{
						clampedIntensity /= (4.0f * 3.14159265f); // Convert Lumens to Candela
					}

					const float clampedRange = std::max(light.range, 0.001f);
					const float clampedInnerAngle = std::clamp(light.innerConeAngle, 0.0f, 1.56f);
					const float clampedOuterAngle = std::clamp(std::max(light.outerConeAngle, clampedInnerAngle + 0.001f), 0.0f, 1.57f);

					GPULight gpuLight{};
					gpuLight.positionAndType = glm::vec4(transform.position, static_cast<float>(static_cast<std::uint32_t>(light.type)));
					gpuLight.directionAndRange = glm::vec4(direction, clampedRange);
					gpuLight.colorAndIntensity = glm::vec4(
						std::max(finalColor.x, 0.0f),
						std::max(finalColor.y, 0.0f),
						std::max(finalColor.z, 0.0f),
						clampedIntensity);
					gpuLight.coneAnglesAndArea = glm::vec4(
						std::cos(clampedInnerAngle),
						std::cos(clampedOuterAngle),
						std::max(light.width, 0.001f),
						std::max(light.height, 0.001f));
					gpuLight.shadowAndPadding = glm::vec4(light.castsShadows ? 1.0f : 0.0f, -1.0f, 0.0f, 0.0f);
					gpuLight.extraParams = glm::vec4(
						std::clamp(light.shadowStrength, 0.0f, 1.0f),
						std::max(light.shadowBias, 0.0f),
						std::max(light.indirectMultiplier, 0.0f),
						light.authoredPriority);

					FrameLightCandidate candidate{};
					candidate.lightId = entry.lightId;
					candidate.gpuLight = gpuLight;
					candidate.stabilityPreference = std::max(light.stabilityPreference, 0.0f);
					candidate.candidateFlags = 0u;
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (light.castsShadows)
					{
						candidate.candidateFlags |= kFrameLightCandidateFlagShadowCapable;
					}
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (light.type == LightType::Area)
					{
						candidate.candidateFlags |= kFrameLightCandidateFlagSpecialCost;
					}
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (light.type == LightType::Directional)
					{
						candidate.candidateFlags |= kFrameLightCandidateFlagDirectional;
					}
					chunkLights.push_back(candidate);
				}

				return chunkLights;
			});

			futures.push_back(task->getFuture());
			ThreadPool::instance().enqueue(task);
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& future : futures)
		{
			auto chunk = future.get();
			packedLights.insert(packedLights.end(), chunk.begin(), chunk.end());
		}
	}

	auto buildShadowDrawLists = [&](const glm::vec3& cameraWorldPosition) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Shadow Draw Lists", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		std::vector<ShadowDrawList> shadowDrawLists;
		shadowDrawLists.reserve(packedLights.size());
		TerraEngine::Scene::SpatialQueryFilter filter{};
		filter.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const FrameLightCandidate& candidate : packedLights)
		{
			if ((candidate.candidateFlags & kFrameLightCandidateFlagShadowCapable) == 0u)
			{
				continue;
			}

			ShadowDrawList shadowDrawList{};
			shadowDrawList.lightId = candidate.lightId;

			const std::uint32_t lightType = static_cast<std::uint32_t>(std::max(candidate.gpuLight.positionAndType.w, 0.0f) + 0.5f);
			TerraEngine::Scene::SpatialQueryResult query{};
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (lightType == static_cast<std::uint32_t>(LightType::Directional))
			{
				const glm::mat4 lightViewProjection = BuildDirectionalShadowViewProjection(candidate.gpuLight, cameraWorldPosition);
				query = spatialIndex.QueryFrustum(TerraEngine::Scene::ExtractFrustum(lightViewProjection), filter);
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (lightType == static_cast<std::uint32_t>(LightType::Point))
			{
				TerraEngine::Scene::SpatialSphere lightBounds{};
				lightBounds.center = glm::vec3(candidate.gpuLight.positionAndType);
				lightBounds.radius = std::max(candidate.gpuLight.directionAndRange.w, 0.5f);
				query = spatialIndex.QuerySphere(lightBounds, filter);
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (lightType == static_cast<std::uint32_t>(LightType::Spot))
			{
				const glm::mat4 lightViewProjection = BuildSpotShadowViewProjection(candidate.gpuLight);
				query = spatialIndex.QueryFrustum(TerraEngine::Scene::ExtractFrustum(lightViewProjection), filter);
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (lightType == static_cast<std::uint32_t>(LightType::Area))
			{
				const glm::mat4 lightViewProjection = BuildAreaShadowViewProjection(candidate.gpuLight);
				query = spatialIndex.QueryFrustum(TerraEngine::Scene::ExtractFrustum(lightViewProjection), filter);
			}

			shadowDrawList.commands = buildDrawCommandsFromCandidateIndices(collectCandidateIndices(query));
			shadowDrawLists.push_back(std::move(shadowDrawList));
		}

		return shadowDrawLists;
	};

	SkyRuntimeState skyRuntimeState{};
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Resolve Sky State", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const SceneSkySettings activeSkySettings = SceneManager::Instance().GetActiveSceneSkySettings();
		skyRuntimeState.isValid = true;
		skyRuntimeState.usingFallback = true;
		skyRuntimeState.hasProblem = false;
		skyRuntimeState.dirty = skyStateDirty;
		skyRuntimeState.forceBlackFallback = !activeSkySettings.enabled;
		std::string skyIssueMessage;

		const LightEntry* firstDirectionalLight = nullptr;
		const LightEntry* boundDirectionalLight = nullptr;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const LightEntry& lightEntry : lights)
		{
			if (lightEntry.light.type != LightType::Directional)
			{
				continue;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (firstDirectionalLight == nullptr)
			{
				firstDirectionalLight = &lightEntry;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (lightEntry.lightId == activeSkySettings.directionalLightObjectId)
			{
				boundDirectionalLight = &lightEntry;
			}
		}

		glm::vec3 baseSunDirection = kSkyReferenceSunDirection;
		const LightEntry* resolvedDirectionalLight = nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (activeSkySettings.enabled)
		{
			if (activeSkySettings.directionalLightObjectId != TerraEngine::EMPTY_UUID)
			{
				resolvedDirectionalLight = boundDirectionalLight;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (resolvedDirectionalLight == nullptr)
				{
					skyRuntimeState.hasProblem = true;
					skyRuntimeState.forceBlackFallback = true;
					skyIssueMessage = "Configured sky directional light could not be resolved. Rendering a black background.";
				}
			}
			else
			{
				resolvedDirectionalLight = firstDirectionalLight;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (resolvedDirectionalLight == nullptr)
				{
					skyRuntimeState.hasProblem = true;
					skyRuntimeState.forceBlackFallback = true;
					skyIssueMessage = "Scene sky requires a directional light. Rendering a black background.";
				}
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (resolvedDirectionalLight != nullptr)
		{
			if (const Transform* lightTransform = worldTransforms.find(resolvedDirectionalLight->entity))
			{
				const glm::quat lightRotation = glm::normalize(lightTransform->rotation);
				const glm::vec3 lightForward = lightRotation * glm::vec3(0.0f, 0.0f, -1.0f);
				baseSunDirection = SafeNormalizeDirection(-lightForward, baseSunDirection);
			}
		}

		skyRuntimeState.resolvedSunDirection = ApplySkyYawOffset(baseSunDirection, activeSkySettings.rotationOffsetDegrees);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (activeSkySettings.enabled && !skyRuntimeState.forceBlackFallback && !activeSkySettings.sourceAsset.empty())
		{
			const TerraEngine::UUID resolvedTextureId = ResourceManager::Instance().ResolveAssetReference(activeSkySettings.sourceAsset, AssetType::Texture);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (resolvedTextureId == TerraEngine::EMPTY_UUID)
			{
				skyRuntimeState.hasProblem = true;
				skyIssueMessage = "Scene sky source could not be resolved. Rendering the fallback background.";
			}
			else
			{
				skyRuntimeState.resolvedSkyHandle = resolvedTextureId;
				skyRuntimeState.usingFallback = false;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skyRuntimeState.hasProblem)
		{
			const std::string issueKey = activeSkySettings.sourceAsset + "|" + skyIssueMessage;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (issueKey != lastLoggedSkyIssueKey)
			{
				TERRAENGINE_LOG(LogLevel::Warning, "[Sky] " << skyIssueMessage << " Source: " << activeSkySettings.sourceAsset);
				lastLoggedSkyIssueKey = issueKey;
			}
		}
		else
		{
			lastLoggedSkyIssueKey.clear();
		}
	}

	FrameRenderData frameData;
	const SceneSkySettings activeSkySettings = SceneManager::Instance().GetActiveSceneSkySettings();
	frameData.deltaTimeSeconds = deltaTime;
	frameData.globalTaskShaderCullingEnabled = taskShaderCullingEnabled;
	frameData.cullingEpsilon = 0.001f;
	frameData.activePreset = currentPreset;
	frameData.customOverrideActive = (currentPreset == QualityPreset::Custom);
	frameData.settings = SanitizeRenderSettings(currentSettings);
	frameData.sky.renderBackground = true;
	frameData.sky.hasTexture = skyRuntimeState.resolvedSkyHandle != TerraEngine::EMPTY_UUID && !skyRuntimeState.usingFallback;
	frameData.sky.usingFallback = skyRuntimeState.usingFallback;
	frameData.sky.forceBlack = skyRuntimeState.forceBlackFallback;
	frameData.sky.intensity = activeSkySettings.intensity;
	frameData.sky.exposure = activeSkySettings.exposure;
	frameData.sky.rotationOffsetDegrees = activeSkySettings.rotationOffsetDegrees;
	frameData.sky.horizonTint = activeSkySettings.horizonTint;
	frameData.sky.zenithTint = activeSkySettings.zenithTint;
	frameData.sky.groundTint = activeSkySettings.groundTint;
	frameData.sky.sunDirection = skyRuntimeState.resolvedSunDirection;
	frameData.sky.textureId = skyRuntimeState.resolvedSkyHandle;
	frameData.lightCandidates = std::move(packedLights);
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Fetch UI Batch", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		frameData.uiBatch = UISystem::Instance().GetCombinedFrameBatch();
	}

	rhi->SetGlobalTaskShaderCullingEnabled(taskShaderCullingEnabled);
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: RHI Begin Frame", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		rhi->BeginFrame();
	}

	TerraEngine::Scene::SpatialDiagnosticsSnapshot sceneSpatialDiagnostics{};
	TerraEngine::Scene::SpatialDiagnosticsSnapshot gameSpatialDiagnostics{};
	// Convert each camera submission into an explicit output owner so the RHI
	// can give Scene, Game, and future offscreen cameras independent snapshot
	// ranges instead of inferring ownership from call order alone.
	auto makeOutputRequest = [](RenderOutputKind kind, RenderViewType diagnosticsView, std::uint32_t outputId) {
		RenderOutputRequest request{};
		request.kind = kind;
		request.priority = RenderOutputPriority::Required;
		request.diagnosticsView = diagnosticsView;
		request.outputId = outputId;
		return request;
	};
	auto applySceneViewContextTint = [&]() {
		if (!sceneViewContextTintEnabled || sceneViewContextFocusedObjectIds.empty())
		{
			return;
		}

		// Prefab context mode keeps the opened prefab branch in normal material
		// color and gently washes every surrounding scene command toward white.
		// This runs only on the Scene View command list before submission.
		const glm::vec4 contextTint(1.0f, 1.0f, 1.0f, 0.25f);
		frameData.editorContextTint = contextTint;
		for (DrawCommand& command : frameData.commands)
		{
			command.editorTint = sceneViewContextFocusedObjectIds.contains(command.objectId)
				? glm::vec4(0.0f)
				: contextTint;
		}
	};

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hasSceneViewCamera)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Scene View", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		frameData.viewMatrix = sceneViewCamera.view;
		frameData.projectionMatrix = sceneViewCamera.proj;
		frameData.cameraWorldPosition = sceneViewCamera.pos;
		frameData.commands = buildVisibleDrawCommands(frameData.projectionMatrix * frameData.viewMatrix);
		applySceneViewContextTint();
		frameData.shadowDrawLists = buildShadowDrawLists(frameData.cameraWorldPosition);
		sceneSpatialDiagnostics = spatialIndex.GetDiagnosticsSnapshot();
		rhi->RenderFrame(frameData, makeOutputRequest(RenderOutputKind::SceneView, RenderViewType::Scene, 1u));
	}

	// Asset-style prefab editing has no active runtime scene. When the editor
	// requests suppression, skip every Game View submission path so opening a
	// prefab asset cannot burn a full offscreen game render every frame.
	if (!gameViewSuppressed && !gameViewCameras.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Game View Cameras", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& cam : gameViewCameras)
		{
			frameData.editorContextTint = glm::vec4(0.0f);
			frameData.viewMatrix = cam.view;
			frameData.projectionMatrix = cam.proj;
			frameData.cameraWorldPosition = cam.pos;
			frameData.commands = buildVisibleDrawCommands(frameData.projectionMatrix * frameData.viewMatrix);
			frameData.shadowDrawLists = buildShadowDrawLists(frameData.cameraWorldPosition);
			gameSpatialDiagnostics = spatialIndex.GetDiagnosticsSnapshot();
			rhi->RenderFrame(frameData, makeOutputRequest(RenderOutputKind::GameView, RenderViewType::Game, 2u));
		}
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (!gameViewSuppressed && cameras.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Blank Game View", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		FrameRenderData blankData{};
		blankData.viewMatrix = glm::mat4(1.0f);
		blankData.projectionMatrix = glm::mat4(1.0f);
		blankData.cameraWorldPosition = glm::vec3(0.0f);
		blankData.deltaTimeSeconds = deltaTime;
		blankData.globalTaskShaderCullingEnabled = taskShaderCullingEnabled;
			blankData.cullingEpsilon = 0.001f;
			blankData.activePreset = currentPreset;
			blankData.customOverrideActive = (currentPreset == QualityPreset::Custom);
			blankData.settings = SanitizeRenderSettings(currentSettings);
			blankData.sky = frameData.sky;
			blankData.editorContextTint = glm::vec4(0.0f);
			blankData.commands.clear();
			blankData.shadowDrawLists.clear();
			rhi->RenderFrame(blankData, makeOutputRequest(RenderOutputKind::GameView, RenderViewType::Game, 2u));
	}
	else if (!gameViewSuppressed)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Submit Runtime Cameras", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t cameraIndex = 0; cameraIndex < cameras.size(); ++cameraIndex)
		{
			const entt::entity cameraEntity = cameras[cameraIndex].entity;
			const auto& camera = cameras[cameraIndex].camera;
			
			VisitingStack visiting;
			visiting.reserve(16);
			
			Transform cameraTransform = computeWorldTransform(computeWorldTransform, cameraEntity, visiting);

			// Normalize the rotation so derived direction vectors remain unit length
			// even if authoring data drifts slightly off the identity quaternion.
			const glm::quat cameraRotation = glm::normalize(cameraTransform.rotation);
			const glm::vec3 defaultForward(0.0f, 0.0f, -1.0f);
			const glm::vec3 forward = cameraRotation * defaultForward;
			glm::vec3 up = cameraRotation * camera.up;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (glm::length(up) < 0.0001f)
			{
				up = glm::vec3(0.0f, 1.0f, 0.0f);
			}

			// Clamp projection inputs to keep glm::perspective from receiving invalid values.
			const float clampedFov = std::max(0.001f, camera.verticalFovDegrees);
			const float safeAspect = camera.aspectRatio > 0.0f ? camera.aspectRatio : 1.0f;
			const float nearPlane = std::max(0.0001f, camera.nearPlane);
			const float farPlane = std::max(nearPlane + 0.001f, camera.farPlane);
			frameData.viewMatrix = glm::lookAt(cameraTransform.position, cameraTransform.position + forward, glm::normalize(up));
			frameData.projectionMatrix = glm::perspective(glm::radians(clampedFov), safeAspect, nearPlane, farPlane);
			frameData.projectionMatrix[1][1] *= -1;
			frameData.cameraWorldPosition = cameraTransform.position;
			frameData.editorContextTint = glm::vec4(0.0f);
			frameData.commands = buildVisibleDrawCommands(frameData.projectionMatrix * frameData.viewMatrix);
			frameData.shadowDrawLists = buildShadowDrawLists(frameData.cameraWorldPosition);
			gameSpatialDiagnostics = spatialIndex.GetDiagnosticsSnapshot();

			rhi->RenderFrame(frameData, makeOutputRequest(RenderOutputKind::MainRuntimeView, RenderViewType::Game, 2u));
		}
	}
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: RHI End Frame", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		rhi->EndFrame();
	}

	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Publish Render Stats", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		std::lock_guard<std::mutex> lock(statsMutex);
		latestGameStats = rhi->GetLatestRenderStats(RenderViewType::Game);
		latestSceneStats = rhi->GetLatestRenderStats(RenderViewType::Scene);
		latestGameStats.spatialIndexedObjectCount = gameSpatialDiagnostics.indexedObjectCount;
		latestGameStats.spatialVisibleCandidateCount = gameSpatialDiagnostics.visibleCandidateCount;
		latestGameStats.spatialRejectedObjectCount = gameSpatialDiagnostics.rejectedObjectCount;
		latestGameStats.spatialFallbackObjectCount = gameSpatialDiagnostics.fallbackObjectCount;
		latestGameStats.spatialRefreshMode = gameSpatialDiagnostics.refreshMode;
		latestGameStats.spatialAddedObjectCount = gameSpatialDiagnostics.addedObjectCount;
		latestGameStats.spatialUpdatedObjectCount = gameSpatialDiagnostics.updatedObjectCount;
		latestGameStats.spatialRemovedObjectCount = gameSpatialDiagnostics.removedObjectCount;
		latestGameStats.spatialUnchangedObjectCount = gameSpatialDiagnostics.unchangedObjectCount;
		latestGameStats.spatialFullRefreshObjectCount = gameSpatialDiagnostics.fullRefreshObjectCount;
		latestSceneStats.spatialIndexedObjectCount = sceneSpatialDiagnostics.indexedObjectCount;
		latestSceneStats.spatialVisibleCandidateCount = sceneSpatialDiagnostics.visibleCandidateCount;
		latestSceneStats.spatialRejectedObjectCount = sceneSpatialDiagnostics.rejectedObjectCount;
		latestSceneStats.spatialFallbackObjectCount = sceneSpatialDiagnostics.fallbackObjectCount;
		latestSceneStats.spatialRefreshMode = sceneSpatialDiagnostics.refreshMode;
		latestSceneStats.spatialAddedObjectCount = sceneSpatialDiagnostics.addedObjectCount;
		latestSceneStats.spatialUpdatedObjectCount = sceneSpatialDiagnostics.updatedObjectCount;
		latestSceneStats.spatialRemovedObjectCount = sceneSpatialDiagnostics.removedObjectCount;
		latestSceneStats.spatialUnchangedObjectCount = sceneSpatialDiagnostics.unchangedObjectCount;
		latestSceneStats.spatialFullRefreshObjectCount = sceneSpatialDiagnostics.fullRefreshObjectCount;
		latestGameSpatialDiagnostics = gameSpatialDiagnostics;
		latestSceneSpatialDiagnostics = sceneSpatialDiagnostics;
		latestGameBudgetDiagnostics = rhi->GetLatestFrameBudgetDiagnostics(RenderViewType::Game);
		latestSceneBudgetDiagnostics = rhi->GetLatestFrameBudgetDiagnostics(RenderViewType::Scene);
		latestGameLightingDiagnostics = rhi->GetLatestLightingDiagnostics(RenderViewType::Game);
		latestSceneLightingDiagnostics = rhi->GetLatestLightingDiagnostics(RenderViewType::Scene);
		latestRenderSnapshotDiagnostics = rhi->GetLatestRenderSnapshotDiagnostics();
		latestSkyRuntimeState = skyRuntimeState;
		TerraEngine::Profiling::Profiler::Instance().RecordRenderStats(latestGameStats, latestSceneStats);
	}

	skyStateDirty = false;

	ClearOverrideCameras();
}

// RenderSystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::Shutdown()
{
	if (rhi) {
		rhi->Shutdown();
		rhi.reset();
	}
}

// RenderSystem::SystemName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string RenderSystem::SystemName()
{
	return "RenderSystem";
}

// RenderSystem::ReloadMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::ReloadMaterial(const TerraEngine::UUID& materialId)
{
	ResourceManager::Instance().ReloadMaterial(materialId);
	// Surface runtime-contract fallbacks during hot reload so artists can see
	// when a material is rendering with authored defaults instead of textures.
	if (const Material* material = ResourceManager::Instance().GetMaterial(materialId))
	{
		for (const std::string& warning : material->fallbackWarnings)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Material " << TerraEngine::ToHexString(materialId) << ": " << warning);
		}
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (rhi)
	{
		rhi->InvalidateMaterial(materialId);
	}
}

// RenderSystem::ReloadMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::ReloadMesh(const TerraEngine::UUID& meshId)
{
	ResourceManager::Instance().ReloadMesh(meshId);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (rhi)
	{
		rhi->InvalidateMesh(meshId);
	}
}

// RenderSystem::ReloadTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::ReloadTexture(const TerraEngine::UUID& textureId)
{
	auto future = ResourceManager::Instance().ReloadTexture(textureId);
	// Wait for the background transcode to finish before we swap the GPU resource
	future.get();

	if (rhi)
	{
		rhi->InvalidateTexture(textureId);
	}
}

// RenderSystem::DeleteResource is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::DeleteResource(const TerraEngine::UUID& id)
{
	if (rhi)
	{
		rhi->WaitIdle();
		rhi->InvalidateResource(id);
	}
}

// RenderSystem::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::Clear()
{
	if (rhi)
	{
		rhi->ClearResources();
	}
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestGameStats = {};
		latestSceneStats = {};
		latestGameBudgetDiagnostics = {};
		latestSceneBudgetDiagnostics = {};
		latestGameLightingDiagnostics = {};
		latestSceneLightingDiagnostics = {};
		latestRenderSnapshotDiagnostics = {};
		latestGameStats.viewType = RenderViewType::Game;
		latestSceneStats.viewType = RenderViewType::Scene;
		latestGameBudgetDiagnostics.viewType = RenderViewType::Game;
		latestSceneBudgetDiagnostics.viewType = RenderViewType::Scene;
		latestGameLightingDiagnostics.viewType = RenderViewType::Game;
		latestSceneLightingDiagnostics.viewType = RenderViewType::Scene;
		latestSkyRuntimeState = {};
	}
	skyStateDirty = true;
	lastLoggedSkyIssueKey.clear();
	ClearOverrideCameras();
}

// RenderSystem::SetTaskShaderCullingEnabled is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::SetTaskShaderCullingEnabled(bool enabled)
{
	taskShaderCullingEnabled = enabled;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (rhi)
	{
		rhi->SetGlobalTaskShaderCullingEnabled(enabled);
	}
}

// RenderSystem::GetLatestRenderStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderStatsSnapshot RenderSystem::GetLatestRenderStats(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneStats;
	}
	return latestGameStats;
}

// RenderSystem::GetLatestSpatialDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::Scene::SpatialDiagnosticsSnapshot RenderSystem::GetLatestSpatialDiagnostics(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneSpatialDiagnostics;
	}
	return latestGameSpatialDiagnostics;
}

// RenderSystem::GetLatestFrameBudgetDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
FrameBudgetDiagnosticsSnapshot RenderSystem::GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneBudgetDiagnostics;
	}
	return latestGameBudgetDiagnostics;
}

// RenderSystem::GetLatestLightingDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
LightingDiagnosticsSnapshot RenderSystem::GetLatestLightingDiagnostics(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneLightingDiagnostics;
	}
	return latestGameLightingDiagnostics;
}

// RenderSystem::GetLatestRenderSnapshotDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderSnapshotDiagnosticsSnapshot RenderSystem::GetLatestRenderSnapshotDiagnostics() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestRenderSnapshotDiagnostics;
}

// RenderSystem::GetLatestSkyRuntimeState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
SkyRuntimeState RenderSystem::GetLatestSkyRuntimeState() const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestSkyRuntimeState;
}

// RenderSystem::MarkSkyStateDirty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::MarkSkyStateDirty()
{
	skyStateDirty = true;
	lastLoggedSkyIssueKey.clear();
}

// RenderSystem::SetQualityPreset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::SetQualityPreset(QualityPreset preset)
{
	if (preset == QualityPreset::Custom)
	{
		currentPreset = QualityPreset::Custom;
		return;
	}

	currentPreset = preset;
	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (preset)
	{
	case QualityPreset::Low:
		currentSettings.shadowResolution = 512u;
		currentSettings.shadowFilteringSize = 1u;
		currentSettings.maxUploadedLights = 64u;
		currentSettings.maxDynamicLights = 32u;
		currentSettings.maxShadowLights = 1u;
		currentSettings.maxAreaLights = 0u;
		currentSettings.enableLTC = 0u;
		currentSettings.enableIBL = 0u;
		currentSettings.indirectStrength = 0.35f;
		currentSettings.ambientOcclusionStrength = 0.5f;
		currentSettings.ambientOcclusionQuality = 0u;
		currentSettings.anisotropicFiltering = 1u;
		break;
	case QualityPreset::Medium:
		currentSettings.shadowResolution = 1024u;
		currentSettings.shadowFilteringSize = 3u;
		currentSettings.maxUploadedLights = 128u;
		currentSettings.maxDynamicLights = 64u;
		currentSettings.maxShadowLights = 2u;
		currentSettings.maxAreaLights = 2u;
		currentSettings.enableLTC = 1u;
		currentSettings.enableIBL = 1u;
		currentSettings.indirectStrength = 0.75f;
		currentSettings.ambientOcclusionStrength = 0.75f;
		currentSettings.ambientOcclusionQuality = 1u;
		currentSettings.anisotropicFiltering = 1u;
		break;
	case QualityPreset::High:
		currentSettings.shadowResolution = 2048u;
		currentSettings.shadowFilteringSize = 5u;
		currentSettings.maxUploadedLights = 256u;
		currentSettings.maxDynamicLights = 128u;
		currentSettings.maxShadowLights = 3u;
		currentSettings.maxAreaLights = 4u;
		currentSettings.enableLTC = 1u;
		currentSettings.enableIBL = 1u;
		currentSettings.indirectStrength = 1.0f;
		currentSettings.ambientOcclusionStrength = 1.0f;
		currentSettings.ambientOcclusionQuality = 2u;
		currentSettings.anisotropicFiltering = 4u;
		break;
	case QualityPreset::Ultra:
		currentSettings.shadowResolution = 4096u;
		currentSettings.shadowFilteringSize = 7u;
		currentSettings.maxUploadedLights = 512u;
		currentSettings.maxDynamicLights = 256u;
		currentSettings.maxShadowLights = 8u;
		currentSettings.maxAreaLights = 8u;
		currentSettings.enableLTC = 1u;
		currentSettings.enableIBL = 1u;
		currentSettings.indirectStrength = 1.25f;
		currentSettings.ambientOcclusionStrength = 1.25f;
		currentSettings.ambientOcclusionQuality = 3u;
		currentSettings.anisotropicFiltering = 16u;
		break;
	}

	currentSettings = SanitizeRenderSettings(currentSettings);
	SaveSettings();
}

// RenderSystem::SetRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::SetRenderSettings(const RenderSettings& settings)
{
	currentSettings = SanitizeRenderSettings(settings);
	currentPreset = QualityPreset::Custom;
	SaveSettings();
}

// RenderSystem::LoadSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::LoadSettings()
{
	namespace fs = std::filesystem;
	fs::path settingsPath = ResolveRuntimeSettingsDirectory() / "RenderSettings.json";

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fs::exists(settingsPath))
	{
		const fs::path legacySettingsPath = ResolveLegacyAssetSettingsPath();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!legacySettingsPath.empty() && fs::exists(legacySettingsPath))
		{
			settingsPath = legacySettingsPath;
		}
		else
		{
			SetQualityPreset(QualityPreset::High);
			return;
		}
	}

	try
	{
		std::ifstream file(settingsPath);
		nlohmann::json j;
		file >> j;

		currentPreset = static_cast<QualityPreset>(j.value("preset", 2u));
		
		auto s = j["settings"];
		currentSettings.shadowResolution = s.value("shadowResolution", 2048u);
		currentSettings.shadowFilteringSize = s.value("shadowFilteringSize", 5u);
		currentSettings.maxUploadedLights = s.value("maxUploadedLights", 256u);
		currentSettings.maxDynamicLights = s.value("maxDynamicLights", 128u);
		currentSettings.maxShadowLights = s.value("maxShadowLights", 8u);
		currentSettings.maxAreaLights = s.value("maxAreaLights", 4u);
		currentSettings.enableLTC = s.value("enableLTC", 1u);
		currentSettings.enableIBL = s.value("enableIBL", 1u);
		currentSettings.indirectStrength = s.value("indirectStrength", 1.0f);
		currentSettings.ambientOcclusionStrength = s.value("ambientOcclusionStrength", 1.0f);
		currentSettings.ambientOcclusionQuality = s.value("ambientOcclusionQuality", 2u);
		currentSettings.anisotropicFiltering = s.value("anisotropicFiltering", 4u);
		currentSettings = SanitizeRenderSettings(currentSettings);
	}
	catch (const std::exception& e)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to load RenderSettings: " << e.what());
		SetQualityPreset(QualityPreset::High);
	}
}

// RenderSystem::SaveSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenderSystem::SaveSettings()
{
	namespace fs = std::filesystem;
	const fs::path settingsDir = ResolveRuntimeSettingsDirectory();
	const fs::path settingsPath = settingsDir / "RenderSettings.json";

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fs::exists(settingsDir))
	{
		fs::create_directories(settingsDir);
	}

	try
	{
		nlohmann::json j;
		j["preset"] = static_cast<uint32_t>(currentPreset);
		
		nlohmann::json s;
		s["shadowResolution"] = currentSettings.shadowResolution;
		s["shadowFilteringSize"] = currentSettings.shadowFilteringSize;
		s["maxUploadedLights"] = currentSettings.maxUploadedLights;
		s["maxDynamicLights"] = currentSettings.maxDynamicLights;
		s["maxShadowLights"] = currentSettings.maxShadowLights;
		s["maxAreaLights"] = currentSettings.maxAreaLights;
		s["enableLTC"] = currentSettings.enableLTC;
		s["enableIBL"] = currentSettings.enableIBL;
		s["indirectStrength"] = currentSettings.indirectStrength;
		s["ambientOcclusionStrength"] = currentSettings.ambientOcclusionStrength;
		s["ambientOcclusionQuality"] = currentSettings.ambientOcclusionQuality;
		s["anisotropicFiltering"] = currentSettings.anisotropicFiltering;
		
		j["settings"] = s;

		std::ofstream file(settingsPath);
		file << j.dump(4);
	}
	catch (const std::exception& e)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to save RenderSettings: " << e.what());
	}
}
