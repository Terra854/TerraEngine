#include "ScriptGlue.h"
#include "Core/Logger/Logger.h"
#include "Core/MainEngine.h"
#include "Core/Object/Component.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"
#include "Components/Transform.h"
#include "Components/ScriptComponent.h"
#include "ScriptingSystem.h"
#include "UI/UISystem.h"
#include "Input/InputSystem.h"
#include "Scene/SceneManager.h"
#include "Utilities/PlatformUtils.h"

#include <mutex>
#include <shared_mutex>

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

