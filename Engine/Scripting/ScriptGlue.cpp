#include "ScriptGlue.h"
#include "Core/Logger/Logger.h"
#include "Core/MainEngine.h"
#include "Core/Object/Component.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"
#include "Components/UIComponent.h"
#include "Components/Transform.h"
#include "Components/ScriptComponent.h"
#include "DotNet/DotNetHost.h"
#include "ScriptingSystem.h"
#include "UI/UIElement.h"
#include "UI/UISystem.h"
#include "Input/InputSystem.h"
#include "Scene/SceneManager.h"
#include "Utilities/PlatformUtils.h"

#include <array>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Renderer/RenderSystem.h"
#include "Resource/ResourceManager.h"

static std::shared_mutex g_ScriptGlueMutex;
// Native_ScriptComponent_GetBindingPayload returns a raw const char*, so each
// thread keeps its own backing string alive until the next bridge call.
thread_local std::string g_ScriptBindingPayloadBuffer;
thread_local std::string g_ApplicationReturnBuffer;
std::mutex g_ApplicationBridgeMutex;
Engine* g_ApplicationHostEngine = nullptr;
ApplicationEditorStopPlayModeCallback g_ApplicationEditorStopCallback = nullptr;
thread_local std::string g_UIBridgeReturnBuffer;

enum class ManagedUIStyleProperty : int
{
	Width = 0,
	Height = 1,
	Color = 2,
	BackgroundColor = 3
};

struct ManagedUIElementHandle
{
	std::weak_ptr<UIElement> weakElement;
	std::shared_ptr<UIElement> ownedElement;
};

std::mutex g_UIHandleMutex;
std::unordered_map<UIElement*, std::unordered_set<ManagedUIElementHandle*>> g_UIHandleIndex;
std::once_flag g_UIEventDispatchResolveOnce;
bool g_UIEventDispatchResolveAttempted = false;
bool g_UIEventDispatchUnavailableLogged = false;

enum class ManagedUIEventType : int
{
	Click = 0,
	MouseEnter = 1,
	MouseLeave = 2
};

using ManagedUIDispatchEventFn = void (*)(void*, int, float, float);
ManagedUIDispatchEventFn g_ManagedUIDispatchEvent = nullptr;

enum class ScriptType : int
{
	Bool = 0,
	Int = 1,
	Float = 2,
	EntityID = 3,
	Vector2 = 4,
	Vector3 = 5,
	Vector4 = 6,
	Quaternion = 7
};

static TerraEngine::Reflection::Variant MarshallFromNative(void* data, ScriptType type)
{
    switch (type)
    {
	case ScriptType::Bool: return *static_cast<bool*>(data);
	case ScriptType::Int: return *static_cast<int*>(data);
	case ScriptType::Float: return *static_cast<float*>(data);
	case ScriptType::EntityID: return *static_cast<uint32_t*>(data);
	case ScriptType::Vector2: return *static_cast<glm::vec2*>(data);
	case ScriptType::Vector3: return *static_cast<glm::vec3*>(data);
	case ScriptType::Vector4: return *static_cast<glm::vec4*>(data);
	case ScriptType::Quaternion: return *static_cast<glm::quat*>(data);
	}
	return {};
}

static void MarshallToNative(const TerraEngine::Reflection::Variant& var, void* data, ScriptType type)
{
    if (!TerraEngine::Reflection::IsValid(var)) return;

    switch (type)
    {
	case ScriptType::Bool: *static_cast<bool*>(data) = TerraEngine::Reflection::ConvertValue<bool>(var); break;
	case ScriptType::Int: *static_cast<int*>(data) = TerraEngine::Reflection::ConvertValue<int>(var); break;
	case ScriptType::Float: *static_cast<float*>(data) = TerraEngine::Reflection::ConvertValue<float>(var); break;
	case ScriptType::EntityID: *static_cast<uint32_t*>(data) = TerraEngine::Reflection::ConvertValue<uint32_t>(var); break;
	case ScriptType::Vector2: *static_cast<glm::vec2*>(data) = TerraEngine::Reflection::ConvertValue<glm::vec2>(var); break;
	case ScriptType::Vector3: *static_cast<glm::vec3*>(data) = TerraEngine::Reflection::ConvertValue<glm::vec3>(var); break;
	case ScriptType::Vector4: *static_cast<glm::vec4*>(data) = TerraEngine::Reflection::ConvertValue<glm::vec4>(var); break;
	case ScriptType::Quaternion: *static_cast<glm::quat*>(data) = TerraEngine::Reflection::ConvertValue<glm::quat>(var); break;
	}
}

static ScriptBinding* ResolveBinding(Object& object, const char* bindingId)
{
	if (bindingId == nullptr || bindingId[0] == '\0')
	{
		return nullptr;
	}

	ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>();
	if (scriptComponent == nullptr)
	{
		return nullptr;
	}

	return scriptComponent->FindBinding(bindingId);
}

static uint32_t InvalidObjectId()
{
	return static_cast<uint32_t>(entt::null);
}

static const char* WriteApplicationReturnText(std::string text)
{
	g_ApplicationReturnBuffer = std::move(text);
	return g_ApplicationReturnBuffer.c_str();
}

static const char* WriteUIBridgeReturnText(std::string text)
{
	g_UIBridgeReturnBuffer = std::move(text);
	return g_UIBridgeReturnBuffer.c_str();
}

static Engine* GetApplicationHostEngine()
{
	std::lock_guard<std::mutex> lock(g_ApplicationBridgeMutex);
	return g_ApplicationHostEngine;
}

static ApplicationEditorStopPlayModeCallback GetApplicationEditorStopCallback()
{
	std::lock_guard<std::mutex> lock(g_ApplicationBridgeMutex);
	return g_ApplicationEditorStopCallback;
}

static const char* GetMeshRendererAssetReference(uint32_t objectId, std::string_view propertyName, AssetType type)
{
	static thread_local std::string assetReferenceBuffer;
	assetReferenceBuffer.clear();

	if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId)))
	{
		auto& registry = ComponentAccessRegistry();
		if (auto it = registry.find("MeshRenderComponent"); it != registry.end())
		{
			TerraEngine::Reflection::Variant value = it->second.getter(*objOpt, propertyName);
			if (TerraEngine::Reflection::IsType<TerraEngine::UUID>(value))
			{
				assetReferenceBuffer = ResourceManager::Instance().DescribeAssetReference(
					TerraEngine::Reflection::GetValue<TerraEngine::UUID>(value),
					type).generic_string();
			}
		}
	}

	return assetReferenceBuffer.c_str();
}

static void SetMeshRendererAssetReference(uint32_t objectId, std::string_view propertyName, AssetType type, const char* assetReference)
{
	if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId)))
	{
		auto& registry = ComponentAccessRegistry();
		if (auto it = registry.find("MeshRenderComponent"); it != registry.end())
		{
			TerraEngine::UUID resolvedId = TerraEngine::EMPTY_UUID;
			if (assetReference != nullptr && assetReference[0] != '\0')
			{
				resolvedId = ResourceManager::Instance().ResolveAssetReference(assetReference, type);
				if (resolvedId == TerraEngine::EMPTY_UUID)
				{
					TERRAENGINE_LOG(LogLevel::Warning,
						"[Scripting] Could not resolve asset reference '" << assetReference
						<< "' for MeshRenderComponent property '" << propertyName << "'.");
					return;
				}
			}

			it->second.setter(*objOpt, propertyName, resolvedId);
		}
	}
}

static void ForgetUIHandleIndexLocked(ManagedUIElementHandle* handle)
{
	for (auto iterator = g_UIHandleIndex.begin(); iterator != g_UIHandleIndex.end();)
	{
		iterator->second.erase(handle);
		if (iterator->second.empty())
		{
			iterator = g_UIHandleIndex.erase(iterator);
		}
		else
		{
			++iterator;
		}
	}
}

static void BindUIHandleToElement(ManagedUIElementHandle* handle, const std::shared_ptr<UIElement>& element, bool keepDetachedOwnership)
{
	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	ForgetUIHandleIndexLocked(handle);
	handle->weakElement = element;
	handle->ownedElement = keepDetachedOwnership ? element : nullptr;
	if (element)
	{
		g_UIHandleIndex[element.get()].insert(handle);
	}
}

static ManagedUIElementHandle* CreateUIHandle(const std::shared_ptr<UIElement>& element, bool keepDetachedOwnership)
{
	if (!element)
	{
		return nullptr;
	}

	ManagedUIElementHandle* handle = new ManagedUIElementHandle();
	BindUIHandleToElement(handle, element, keepDetachedOwnership);
	return handle;
}

static std::shared_ptr<UIElement> ResolveUIHandle(void* handlePointer)
{
	if (handlePointer == nullptr)
	{
		return nullptr;
	}

	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	ManagedUIElementHandle* handle = static_cast<ManagedUIElementHandle*>(handlePointer);
	if (handle->ownedElement)
	{
		return handle->ownedElement;
	}

	return handle->weakElement.lock();
}

static void PromoteDetachedUIOwnership(const std::shared_ptr<UIElement>& element)
{
	if (!element)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	if (auto iterator = g_UIHandleIndex.find(element.get()); iterator != g_UIHandleIndex.end())
	{
		for (ManagedUIElementHandle* handle : iterator->second)
		{
			handle->ownedElement = element;
		}
	}
}

static void DropDetachedUIOwnership(const std::shared_ptr<UIElement>& element)
{
	if (!element)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	if (auto iterator = g_UIHandleIndex.find(element.get()); iterator != g_UIHandleIndex.end())
	{
		for (ManagedUIElementHandle* handle : iterator->second)
		{
			handle->ownedElement.reset();
		}
	}
}

static void RefreshManagedElementStyle(UIElement& element)
{
	// Runtime setters act like inline style overrides layered on top of the
	// already-authored computed style, so only the touched property needs to be
	// copied through before the display style is refreshed.
	element.RefreshDisplayStyle();
	element.MarkDirty();
}

static std::filesystem::path ResolveManagedBridgeAssemblyPath()
{
	const std::filesystem::path current = std::filesystem::current_path();
	const std::filesystem::path candidates[] = {
		current / "Managed" / "TerraEngine.CSharp.dll",
		current / "Editor" / "Managed" / "TerraEngine.CSharp.dll",
		current.parent_path() / "Editor" / "Managed" / "TerraEngine.CSharp.dll"
	};

	for (const std::filesystem::path& candidate : candidates)
	{
		if (std::filesystem::exists(candidate))
		{
			return candidate;
		}
	}

	return candidates[0];
}

static ManagedUIDispatchEventFn ResolveManagedUIEventDispatcher()
{
	std::call_once(g_UIEventDispatchResolveOnce, []() {
		g_UIEventDispatchResolveAttempted = true;
		if (!DotNetHost::Instance().IsInitialized() || DotNetHost::Instance().IsExportMode())
		{
			return;
		}

		g_ManagedUIDispatchEvent = reinterpret_cast<ManagedUIDispatchEventFn>(DotNetHost::Instance().GetManagedFunction(
			ResolveManagedBridgeAssemblyPath(),
			L"TerraEngine.InternalCalls, TerraEngine.CSharp",
			L"DispatchNativeUIEvent"));
	});

	if (g_ManagedUIDispatchEvent == nullptr && g_UIEventDispatchResolveAttempted && !g_UIEventDispatchUnavailableLogged)
	{
		g_UIEventDispatchUnavailableLogged = true;
		const LogLevel level = DotNetHost::Instance().IsExportMode() ? LogLevel::Debug : LogLevel::Warning;
		TERRAENGINE_LOG(level, "[UI] Managed UI event dispatch is unavailable. Falling back to managed polling events where possible.");
	}

	return g_ManagedUIDispatchEvent;
}

static ManagedUIElementHandle* FindFirstUIHandleForElement(UIElement& element)
{
	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	if (auto iterator = g_UIHandleIndex.find(&element); iterator != g_UIHandleIndex.end() && !iterator->second.empty())
	{
		return *iterator->second.begin();
	}

	return nullptr;
}

static void DispatchManagedUIEvent(UIElement& element, ManagedUIEventType eventType, const glm::vec2& pointerPosition)
{
	ManagedUIDispatchEventFn dispatcher = ResolveManagedUIEventDispatcher();
	if (dispatcher == nullptr)
	{
		return;
	}

	ManagedUIElementHandle* handle = FindFirstUIHandleForElement(element);
	if (handle == nullptr)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "[UI] Tried to dispatch an event for element '" << element.id << "' without an active managed handle.");
		return;
	}

	dispatcher(handle, static_cast<int>(eventType), pointerPosition.x, pointerPosition.y);
}

static void DispatchManagedClickEvent(UIElement& element, const glm::vec2& pointerPosition)
{
	DispatchManagedUIEvent(element, ManagedUIEventType::Click, pointerPosition);
}

static void DispatchManagedMouseEnterEvent(UIElement& element, const glm::vec2& pointerPosition)
{
	DispatchManagedUIEvent(element, ManagedUIEventType::MouseEnter, pointerPosition);
}

static void DispatchManagedMouseLeaveEvent(UIElement& element, const glm::vec2& pointerPosition)
{
	DispatchManagedUIEvent(element, ManagedUIEventType::MouseLeave, pointerPosition);
}

void SetApplicationHostEngine(Engine* engine)
{
	std::lock_guard<std::mutex> lock(g_ApplicationBridgeMutex);
	g_ApplicationHostEngine = engine;
}

void SetApplicationEditorStopPlayModeCallback(ApplicationEditorStopPlayModeCallback callback)
{
	std::lock_guard<std::mutex> lock(g_ApplicationBridgeMutex);
	g_ApplicationEditorStopCallback = callback;
}

extern "C" {

    void Native_Log(int level, const char* message, const char* function, int line) {
        LogLevel logLevel = static_cast<LogLevel>(level);
        Logger::Instance().Log(logLevel, "C#", line, function, message, true);
    }

	void Native_Application_Quit() {
		if (ScriptingSystem::Instance().IsExportMode()) {
			if (Engine* hostEngine = GetApplicationHostEngine()) {
				hostEngine->SignalShutdown();
				return;
			}

			TERRAENGINE_LOG(LogLevel::Warning, "Application.Quit() was called in export mode before the host engine was registered.");
			return;
		}

		if (ApplicationEditorStopPlayModeCallback stopPlayMode = GetApplicationEditorStopCallback()) {
			stopPlayMode();
			return;
		}

		// Fall back to stopping script execution so a missing editor callback does
		// not keep runaway gameplay scripts alive forever during development.
		TERRAENGINE_LOG(LogLevel::Warning, "Application.Quit() could not reach the editor Play Mode stop callback; stopping scripts without restoring the scene snapshot.");
		ScriptingSystem::Instance().SetRunning(false);
		ScriptingSystem::Instance().SetPaused(false);
	}

	void Native_Application_OpenURL(const char* url) {
		TerraEngine::PlatformUtils::OpenUrl(url != nullptr ? url : "");
	}

	const char* Native_Application_GetPersistentDataPath() {
		const std::filesystem::path persistentDataPath = TerraEngine::PlatformUtils::GetPersistentDataPath(
			ResourceManager::Instance().GetProductName());
		return WriteApplicationReturnText(persistentDataPath.string());
	}

	const char* Native_Application_GetStreamingAssetsPath() {
		return WriteApplicationReturnText(ResourceManager::Instance().GetAssetRoot().string());
	}

	bool Native_Application_IsEditor() {
		return !ScriptingSystem::Instance().IsExportMode();
	}

	std::uint32_t Native_Application_GetPlatform() {
		return static_cast<std::uint32_t>(TerraEngine::PlatformUtils::GetRuntimePlatform());
	}

	const char* Native_Application_GetProductName() {
		return WriteApplicationReturnText(ResourceManager::Instance().GetProductName());
	}

	const char* Native_Application_GetVersion() {
		return WriteApplicationReturnText(ResourceManager::Instance().GetVersion());
	}

    uint32_t Native_Object_Create(const char* name) {
        std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
        Object obj = ObjectManager::instance().createObject(name ? name : "New Object");
        return static_cast<uint32_t>(obj.EntityId());
    }

    void Native_Object_Destroy(uint32_t objectId) {
        std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            ObjectManager::instance().removeObject(*objOpt);
        }
    }

	const char* Native_Object_GetName(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		static thread_local std::string nameBuffer;
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			nameBuffer = ObjectManager::instance().getObjectName(*objOpt);
			return nameBuffer.c_str();
		}
		return "";
	}

	void Native_Object_SetName(uint32_t objectId, const char* name) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			ObjectManager::instance().setObjectName(*objOpt, name ? name : "");
		}
	}

	uint32_t Native_Object_Find(const char* name) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (name == nullptr || name[0] == '\0') {
			return InvalidObjectId();
		}

		if (auto objOpt = ObjectManager::instance().FindObjectByName(name)) {
			return static_cast<uint32_t>(objOpt->EntityId());
		}

		return InvalidObjectId();
	}

	uint32_t Native_Object_FindWithTag(const char* tag) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (tag == nullptr || tag[0] == '\0') {
			return InvalidObjectId();
		}

		if (auto objOpt = ObjectManager::instance().FindObjectByTag(tag)) {
			return static_cast<uint32_t>(objOpt->EntityId());
		}

		return InvalidObjectId();
	}

	const char* Native_MeshRenderer_GetMesh(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		return GetMeshRendererAssetReference(objectId, "meshId", AssetType::Mesh);
	}

	void Native_MeshRenderer_SetMesh(uint32_t objectId, const char* assetReference) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		SetMeshRendererAssetReference(objectId, "meshId", AssetType::Mesh, assetReference);
	}

	const char* Native_MeshRenderer_GetMaterial(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		return GetMeshRendererAssetReference(objectId, "materialId", AssetType::Material);
	}

	void Native_MeshRenderer_SetMaterial(uint32_t objectId, const char* assetReference) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		SetMeshRendererAssetReference(objectId, "materialId", AssetType::Material, assetReference);
	}

    bool Native_HasComponent(uint32_t objectId, const char* componentType) {
        std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            auto& reg = ComponentAccessRegistry();
            if (auto it = reg.find(componentType); it != reg.end()) {
                return it->second.hasComponent(*objOpt);
            }
        }
        return false;
    }

    bool Native_AddComponent(uint32_t objectId, const char* componentType) {
        std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            auto& reg = ComponentAccessRegistry();
            if (auto it = reg.find(componentType); it != reg.end()) {
                it->second.addComponent(*objOpt);
                return true;
            }
        }
        return false;
    }

	bool Native_ScriptComponent_AddBinding(uint32_t objectId, const char* className) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (className == nullptr || className[0] == '\0') {
			return false;
		}

		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			ScriptComponent* scriptComponent = objOpt->getComponent<ScriptComponent>();
			if (scriptComponent == nullptr) {
				scriptComponent = &objOpt->addComponent<ScriptComponent>();
			}

			auto guard = scriptComponent->acquireWriteLock();
			scriptComponent->AddBinding(TerraEngine::EMPTY_UUID, className);
			return true;
		}

		return false;
	}

	bool Native_ScriptComponent_HasBinding(uint32_t objectId, const char* className) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (className == nullptr || className[0] == '\0') {
			return false;
		}

		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			if (ScriptComponent* scriptComponent = objOpt->getComponent<ScriptComponent>()) {
				auto guard = scriptComponent->acquireReadLock();
				return scriptComponent->HasBinding(className);
			}
		}

		return false;
	}

	const char* Native_ScriptComponent_GetBindingPayload(uint32_t objectId, const char* bindingId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		g_ScriptBindingPayloadBuffer.clear();
		if (bindingId == nullptr || bindingId[0] == '\0') {
			return g_ScriptBindingPayloadBuffer.c_str();
		}

		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			if (ScriptComponent* scriptComponent = objOpt->getComponent<ScriptComponent>()) {
				auto guard = scriptComponent->acquireReadLock();
				if (const ScriptBinding* binding = scriptComponent->FindBinding(bindingId)) {
					// Return the authored binding payload only. Live runtime state is
					// reconstructed on demand inside the managed bridge.
					g_ScriptBindingPayloadBuffer = nlohmann::json(*binding).dump();
				}
			}
		}

		return g_ScriptBindingPayloadBuffer.c_str();
	}

	bool Native_ScriptComponent_SetBindingValue(uint32_t objectId, const char* bindingId, const char* fieldPath, const char* valueKind, const char* valueJson, const char* compatibility, const char* sourceTypeFingerprint) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (bindingId == nullptr || bindingId[0] == '\0' || fieldPath == nullptr || fieldPath[0] == '\0' || valueJson == nullptr) {
			return false;
		}

		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			ScriptComponent* scriptComponent = objOpt->getComponent<ScriptComponent>();
			if (scriptComponent == nullptr) {
				return false;
			}

			auto guard = scriptComponent->acquireWriteLock();
			ScriptBinding* binding = ResolveBinding(*objOpt, bindingId);
			if (binding == nullptr) {
				return false;
			}

			ScriptFieldValue& fieldValue = binding->UpsertSerializedValue(fieldPath, valueKind != nullptr ? valueKind : "");
			try {
				// Inspector writes already serialize through JSON, so preserve the
				// structured payload when possible instead of flattening to strings.
				fieldValue.value = nlohmann::json::parse(valueJson);
			} catch (...) {
				fieldValue.value = valueJson;
			}

			fieldValue.fieldPath = fieldPath;
			fieldValue.fieldName = fieldPath;
			if (valueKind != nullptr) {
				fieldValue.valueKind = valueKind;
			}
			fieldValue.compatibility = compatibility != nullptr && compatibility[0] != '\0' ? compatibility : "Current";
			fieldValue.persistenceState = fieldValue.compatibility;
			fieldValue.sourceTypeFingerprint = sourceTypeFingerprint != nullptr ? sourceTypeFingerprint : "";
			scriptComponent->SynchronizeStorageFromBindings();
			return true;
		}

		return false;
	}

	bool Native_ScriptComponent_RemoveBindingValue(uint32_t objectId, const char* bindingId, const char* fieldPath) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (bindingId == nullptr || bindingId[0] == '\0' || fieldPath == nullptr || fieldPath[0] == '\0') {
			return false;
		}

		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			ScriptComponent* scriptComponent = objOpt->getComponent<ScriptComponent>();
			if (scriptComponent == nullptr) {
				return false;
			}

			auto guard = scriptComponent->acquireWriteLock();
			ScriptBinding* binding = ResolveBinding(*objOpt, bindingId);
			if (binding == nullptr || !binding->RemoveSerializedValue(fieldPath)) {
				return false;
			}

			scriptComponent->SynchronizeStorageFromBindings();
			return true;
		}

		return false;
	}

    void Native_GetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* outValue, int type) {
        std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            auto& reg = ComponentAccessRegistry();
            if (auto it = reg.find(componentType); it != reg.end()) {
                TerraEngine::Reflection::Variant val = it->second.getter(*objOpt, propertyName);
                MarshallToNative(val, outValue, static_cast<ScriptType>(type));
            }
        }
    }

    void Native_SetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* inValue, int type) {
        std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
        if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
            auto& reg = ComponentAccessRegistry();
            if (auto it = reg.find(componentType); it != reg.end()) {
                TerraEngine::Reflection::Variant val = MarshallFromNative(inValue, static_cast<ScriptType>(type));
                it->second.setter(*objOpt, propertyName, val);
            }
        }
    }

    void Native_Graphics_SetQualityPreset(uint32_t preset) {
        RenderSystem::Instance().SetQualityPreset(static_cast<QualityPreset>(preset));
    }

    uint32_t Native_Graphics_GetQualityPreset() {
        return static_cast<uint32_t>(RenderSystem::Instance().GetQualityPreset());
    }

    void Native_Graphics_SetRenderSettings(RenderSettings* settings) {
        if (settings) {
            RenderSystem::Instance().SetRenderSettings(*settings);
        }
    }

    void Native_Graphics_GetRenderSettings(RenderSettings* outSettings) {
        if (outSettings) {
            *outSettings = RenderSystem::Instance().GetRenderSettings();
        }
    }

    const char* Native_ResourceManager_GetAssetRoot() {
        static std::string assetRoot;
        assetRoot = ResourceManager::Instance().GetAssetRoot().string();
        return assetRoot.c_str();
    }

	bool Native_Input_IsKeyPressed(int key) {
		return InputSystem::Instance().IsKeyPressed(static_cast<Key>(key));
	}

	bool Native_Input_IsMouseButtonPressed(int button) {
		return InputSystem::Instance().IsMouseButtonPressed(static_cast<MouseButton>(button));
	}

	void Native_Input_GetMousePosition(double* outX, double* outY) {
		auto [x, y] = InputSystem::Instance().GetCursorPosition();
		if (outX) *outX = x;
		if (outY) *outY = y;
	}

	bool Native_UI_WasElementClicked(const char* elementId) {
		if (elementId == nullptr || elementId[0] == '\0') {
			return false;
		}

		return UISystem::Instance().WasElementClicked(elementId);
	}

	bool Native_UI_IsElementHovered(const char* elementId) {
		if (elementId == nullptr || elementId[0] == '\0') {
			return false;
		}

		return UISystem::Instance().IsElementHovered(elementId);
	}

	std::uint64_t Native_UI_GetInteractionFrame() {
		return UISystem::Instance().GetInteractionFrame();
	}

	void* Native_UI_CreateElement(int elementType) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr element = UIElement::CreateDefault(static_cast<UIElementType>(elementType));
		return CreateUIHandle(element, true);
	}

	void Native_UI_ReleaseHandle(void* elementHandle) {
		if (elementHandle == nullptr) {
			return;
		}

		std::lock_guard<std::mutex> lock(g_UIHandleMutex);
		ManagedUIElementHandle* handle = static_cast<ManagedUIElementHandle*>(elementHandle);
		ForgetUIHandleIndexLocked(handle);
		delete handle;
	}

	void* Native_UI_GetRootElement(uint32_t objectId) {
		{
			std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			if (auto object = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
				UIComponent* component = object->getComponent<UIComponent>();
				if (component == nullptr) {
					return nullptr;
				}

				if (UIElement::Ptr root = component->GetRoot()) {
					return CreateUIHandle(root, false);
				}
			}
			else {
				return nullptr;
			}
		}

		if (auto object = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			if (UIComponent* component = object->getComponent<UIComponent>()) {
				UISystem::Instance().EnsureDocumentReady(object->EntityId(), *component);
			}
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (auto object = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			UIComponent* component = object->getComponent<UIComponent>();
			if (component == nullptr) {
				return nullptr;
			}

			UIElement::Ptr root = component->GetRoot();
			if (!root) {
				root = UIElement::Create(UIElementType::Root);
				root->typeName = "UIRoot";
				component->SetRoot(root);
				component->MarkDirty();
			}

			return CreateUIHandle(root, false);
		}

		return nullptr;
	}

	void* Native_UI_FindElementById(void* elementHandle, const char* elementId) {
		if (elementId == nullptr || elementId[0] == '\0') {
			return nullptr;
		}

		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr element = ResolveUIHandle(elementHandle);
		if (!element) {
			return nullptr;
		}

		return CreateUIHandle(element->FindById(elementId), false);
	}

	int Native_UI_GetElementType(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			return static_cast<int>(element->type);
		}

		return static_cast<int>(UIElementType::VisualElement);
	}

	bool Native_UI_IsHandleAlive(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		return ResolveUIHandle(elementHandle) != nullptr;
	}

	const char* Native_UI_GetElementId(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			return WriteUIBridgeReturnText(element->id);
		}

		return WriteUIBridgeReturnText({});
	}

	void Native_UI_SetElementId(void* elementHandle, const char* elementId) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			element->id = elementId != nullptr ? elementId : "";
			element->MarkDirty();
		}
	}

	const char* Native_UI_GetElementText(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			return WriteUIBridgeReturnText(element->text);
		}

		return WriteUIBridgeReturnText({});
	}

	void Native_UI_SetElementText(void* elementHandle, const char* text) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			element->text = text != nullptr ? text : "";
			element->MarkDirty();
		}
	}

	bool Native_UI_GetElementVisible(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			return element->IsVisible();
		}

		return false;
	}

	void Native_UI_SetElementVisible(void* elementHandle, bool visible) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			element->SetVisible(visible);
		}
	}

	void Native_UI_AddChild(void* parentHandle, void* childHandle) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr parent = ResolveUIHandle(parentHandle);
		UIElement::Ptr child = ResolveUIHandle(childHandle);
		if (!parent || !child) {
			return;
		}

		if (parent->InsertChild(parent->children.size(), child)) {
			DropDetachedUIOwnership(child);
		}
	}

	void Native_UI_RemoveChild(void* parentHandle, void* childHandle) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr parent = ResolveUIHandle(parentHandle);
		UIElement::Ptr child = ResolveUIHandle(childHandle);
		if (!parent || !child) {
			return;
		}

		if (parent->RemoveChild(child)) {
			PromoteDetachedUIOwnership(child);
		}
	}

	void Native_UI_ClearChildren(void* parentHandle) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr parent = ResolveUIHandle(parentHandle);
		if (!parent) {
			return;
		}

		std::vector<UIElement::Ptr> detachedChildren = parent->children;
		parent->ClearChildren();
		for (const UIElement::Ptr& child : detachedChildren) {
			PromoteDetachedUIOwnership(child);
		}
	}

	void Native_UI_SetStyleFloat(void* elementHandle, int property, float value) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			switch (static_cast<ManagedUIStyleProperty>(property)) {
			case ManagedUIStyleProperty::Width:
				element->inlineStyle.width = UIValue::Points(value);
				element->computedStyle.width = element->inlineStyle.width;
				break;
			case ManagedUIStyleProperty::Height:
				element->inlineStyle.height = UIValue::Points(value);
				element->computedStyle.height = element->inlineStyle.height;
				break;
			default:
				return;
			}

			RefreshManagedElementStyle(*element);
		}
	}

	float Native_UI_GetStyleFloat(void* elementHandle, int property) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			switch (static_cast<ManagedUIStyleProperty>(property)) {
			case ManagedUIStyleProperty::Width:
				return element->displayStyle.width.has_value() ? element->displayStyle.width->value : 0.0f;
			case ManagedUIStyleProperty::Height:
				return element->displayStyle.height.has_value() ? element->displayStyle.height->value : 0.0f;
			default:
				return 0.0f;
			}
		}

		return 0.0f;
	}

	void Native_UI_SetStyleColor(void* elementHandle, int property, float x, float y, float z, float w) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			const glm::vec4 color(x, y, z, w);
			switch (static_cast<ManagedUIStyleProperty>(property)) {
			case ManagedUIStyleProperty::Color:
				element->inlineStyle.color = color;
				element->computedStyle.color = color;
				break;
			case ManagedUIStyleProperty::BackgroundColor:
				element->inlineStyle.backgroundColor = color;
				element->computedStyle.backgroundColor = color;
				break;
			default:
				return;
			}

			RefreshManagedElementStyle(*element);
		}
	}

	void Native_UI_GetStyleColor(void* elementHandle, int property, float* outX, float* outY, float* outZ, float* outW) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		std::array<float*, 4> outputs = { outX, outY, outZ, outW };
		for (float* output : outputs) {
			if (output != nullptr) {
				*output = 0.0f;
			}
		}

		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			std::optional<glm::vec4> color;
			switch (static_cast<ManagedUIStyleProperty>(property)) {
			case ManagedUIStyleProperty::Color:
				color = element->displayStyle.color;
				break;
			case ManagedUIStyleProperty::BackgroundColor:
				color = element->displayStyle.backgroundColor;
				break;
			default:
				return;
			}

			if (color.has_value()) {
				if (outX) *outX = color->x;
				if (outY) *outY = color->y;
				if (outZ) *outZ = color->z;
				if (outW) *outW = color->w;
			}
		}
	}

	bool Native_UI_SetEventCallback(void* elementHandle, int eventType, bool enabled) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr element = ResolveUIHandle(elementHandle);
		if (!element) {
			return false;
		}

		if (ResolveManagedUIEventDispatcher() == nullptr) {
			return false;
		}

		UIElement::UIEventCallback callback = nullptr;
		if (enabled) {
			switch (static_cast<ManagedUIEventType>(eventType)) {
			case ManagedUIEventType::Click:
				callback = &DispatchManagedClickEvent;
				break;
			case ManagedUIEventType::MouseEnter:
				callback = &DispatchManagedMouseEnterEvent;
				break;
			case ManagedUIEventType::MouseLeave:
				callback = &DispatchManagedMouseLeaveEvent;
				break;
			default:
				return false;
			}
		}

		switch (static_cast<ManagedUIEventType>(eventType)) {
		case ManagedUIEventType::Click:
			element->OnClick = callback;
			break;
		case ManagedUIEventType::MouseEnter:
			element->OnMouseEnter = callback;
			break;
		case ManagedUIEventType::MouseLeave:
			element->OnMouseLeave = callback;
			break;
		default:
			return false;
		}

		return true;
	}

	const char* Native_Scene_LoadScene(const char* path) {
		static thread_local std::string uuidBuffer;
		if (path == nullptr) return "";
		TerraEngine::UUID id = SceneManager::Instance().LoadScene(path);
		uuidBuffer = TerraEngine::ToHexString(id);
		return uuidBuffer.c_str();
	}

	const char* Native_Scene_LoadPrefab(const char* path) {
		static thread_local std::string uuidBuffer;
		if (path == nullptr) return "";
		TerraEngine::UUID id = SceneManager::Instance().LoadPrefab(path);
		uuidBuffer = TerraEngine::ToHexString(id);
		return uuidBuffer.c_str();
	}

	bool Native_Scene_SetActiveScene(const char* uuidHex) {
		if (uuidHex == nullptr) return false;
		auto id = TerraEngine::ParseUUID(uuidHex);
		if (!id) return false;
		return SceneManager::Instance().SetActiveScene(*id);
	}

	float Native_Time_GetDeltaTime() {
		return ScriptingSystem::Instance().GetLastDeltaTime();
	}

}

