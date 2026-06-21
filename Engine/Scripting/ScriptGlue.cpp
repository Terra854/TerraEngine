// ScriptGlue.cpp — Contains the Script Glue file-specific types, helper functions, and module integration code implementation for native script glue, exported C# vtables, and scripting system lifecycle state.

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
#include "Scene/SceneMutationQueue.h"
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

// ManagedUIStyleProperty names the supported states explicitly so serialization and switches stay readable.
enum class ManagedUIStyleProperty : int
{
	Width = 0,
	Height = 1,
	Color = 2,
	BackgroundColor = 3
};

// struct ManagedUIElementHandle groups related state and operations so callers have one documented contract.
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

// ManagedUIEventType names the supported states explicitly so serialization and switches stay readable.
enum class ManagedUIEventType : int
{
	Click = 0,
	MouseEnter = 1,
	MouseLeave = 2
};

using ManagedUIDispatchEventFn = void (*)(void*, int, float, float);
ManagedUIDispatchEventFn g_ManagedUIDispatchEvent = nullptr;

// ScriptType names the supported states explicitly so serialization and switches stay readable.
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

// MarshallFromNative is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// MarshallToNative is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void MarshallToNative(const TerraEngine::Reflection::Variant& var, void* data, ScriptType type)
{
	if (!TerraEngine::Reflection::IsValid(var)) return;

	// Dispatch by explicit engine state so each case documents one supported behavior.
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

static nlohmann::json MarshallNativeValueToJson(void* data, ScriptType type)
{
	switch (type)
	{
	case ScriptType::Bool: return *static_cast<bool*>(data);
	case ScriptType::Int: return *static_cast<int*>(data);
	case ScriptType::Float: return *static_cast<float*>(data);
	case ScriptType::EntityID: return *static_cast<uint32_t*>(data);
	case ScriptType::Vector2:
	{
		const glm::vec2 value = *static_cast<glm::vec2*>(data);
		return nlohmann::json::array({ value.x, value.y });
	}
	case ScriptType::Vector3:
	{
		const glm::vec3 value = *static_cast<glm::vec3*>(data);
		return nlohmann::json::array({ value.x, value.y, value.z });
	}
	case ScriptType::Vector4:
	{
		const glm::vec4 value = *static_cast<glm::vec4*>(data);
		return nlohmann::json::array({ value.x, value.y, value.z, value.w });
	}
	case ScriptType::Quaternion:
	{
		const glm::quat value = *static_cast<glm::quat*>(data);
		return nlohmann::json::array({ value.w, value.x, value.y, value.z });
	}
	}
	return {};
}

// ResolveBinding is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static ScriptBinding* ResolveBinding(Object& object, const char* bindingId)
{
	if (bindingId == nullptr || bindingId[0] == '\0')
	{
		return nullptr;
	}

	ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (scriptComponent == nullptr)
	{
		return nullptr;
	}

	return scriptComponent->FindBinding(bindingId);
}

// InvalidObjectId is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static uint32_t InvalidObjectId()
{
	return static_cast<uint32_t>(entt::null);
}

// WriteApplicationReturnText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static const char* WriteApplicationReturnText(std::string text)
{
	g_ApplicationReturnBuffer = std::move(text);
	return g_ApplicationReturnBuffer.c_str();
}

// WriteUIBridgeReturnText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static const char* WriteUIBridgeReturnText(std::string text)
{
	g_UIBridgeReturnBuffer = std::move(text);
	return g_UIBridgeReturnBuffer.c_str();
}

// GetApplicationHostEngine is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static Engine* GetApplicationHostEngine()
{
	std::lock_guard<std::mutex> lock(g_ApplicationBridgeMutex);
	return g_ApplicationHostEngine;
}

// GetApplicationEditorStopCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static ApplicationEditorStopPlayModeCallback GetApplicationEditorStopCallback()
{
	std::lock_guard<std::mutex> lock(g_ApplicationBridgeMutex);
	return g_ApplicationEditorStopCallback;
}

// GetMeshRendererAssetReference is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static const char* GetMeshRendererAssetReference(uint32_t objectId, std::string_view propertyName, AssetType type)
{
	static thread_local std::string assetReferenceBuffer;
	assetReferenceBuffer.clear();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId)))
	{
		auto& registry = ComponentAccessRegistry();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto it = registry.find("MeshRenderComponent"); it != registry.end())
		{
			TerraEngine::Reflection::Variant value = it->second.getter(*objOpt, propertyName);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// SetMeshRendererAssetReference is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void SetMeshRendererAssetReference(uint32_t objectId, std::string_view propertyName, AssetType type, const char* assetReference)
{
	if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId)))
	{
		auto& registry = ComponentAccessRegistry();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto it = registry.find("MeshRenderComponent"); it != registry.end())
		{
			TerraEngine::UUID resolvedId = TerraEngine::EMPTY_UUID;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (assetReference != nullptr && assetReference[0] != '\0')
			{
				resolvedId = ResourceManager::Instance().ResolveAssetReference(assetReference, type);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// ForgetUIHandleIndexLocked is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void ForgetUIHandleIndexLocked(ManagedUIElementHandle* handle)
{
	for (auto iterator = g_UIHandleIndex.begin(); iterator != g_UIHandleIndex.end();)
	{
		iterator->second.erase(handle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// BindUIHandleToElement is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void BindUIHandleToElement(ManagedUIElementHandle* handle, const std::shared_ptr<UIElement>& element, bool keepDetachedOwnership)
{
	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	ForgetUIHandleIndexLocked(handle);
	handle->weakElement = element;
	handle->ownedElement = keepDetachedOwnership ? element : nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (element)
	{
		g_UIHandleIndex[element.get()].insert(handle);
	}
}

// CreateUIHandle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ResolveUIHandle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static std::shared_ptr<UIElement> ResolveUIHandle(void* handlePointer)
{
	if (handlePointer == nullptr)
	{
		return nullptr;
	}

	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	ManagedUIElementHandle* handle = static_cast<ManagedUIElementHandle*>(handlePointer);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (handle->ownedElement)
	{
		return handle->ownedElement;
	}

	return handle->weakElement.lock();
}

// PromoteDetachedUIOwnership is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void PromoteDetachedUIOwnership(const std::shared_ptr<UIElement>& element)
{
	if (!element)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto iterator = g_UIHandleIndex.find(element.get()); iterator != g_UIHandleIndex.end())
	{
		for (ManagedUIElementHandle* handle : iterator->second)
		{
			handle->ownedElement = element;
		}
	}
}

// DropDetachedUIOwnership is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void DropDetachedUIOwnership(const std::shared_ptr<UIElement>& element)
{
	if (!element)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto iterator = g_UIHandleIndex.find(element.get()); iterator != g_UIHandleIndex.end())
	{
		for (ManagedUIElementHandle* handle : iterator->second)
		{
			handle->ownedElement.reset();
		}
	}
}

// RefreshManagedElementStyle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void RefreshManagedElementStyle(UIElement& element)
{
	// Runtime setters act like inline style overrides layered on top of the
	// already-authored computed style, so only the touched property needs to be
	// copied through before the display style is refreshed.
	element.RefreshDisplayStyle();
	element.MarkDirty();
}

// ResolveManagedBridgeAssemblyPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static std::filesystem::path ResolveManagedBridgeAssemblyPath()
{
	const std::filesystem::path current = std::filesystem::current_path();
	const std::filesystem::path candidates[] = {
		current / "Managed" / "TerraEngine.CSharp.dll",
		current / "Editor" / "Managed" / "TerraEngine.CSharp.dll",
		current.parent_path() / "Editor" / "Managed" / "TerraEngine.CSharp.dll"
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const std::filesystem::path& candidate : candidates)
	{
		if (std::filesystem::exists(candidate))
		{
			return candidate;
		}
	}

	return candidates[0];
}

// ResolveManagedUIEventDispatcher is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static ManagedUIDispatchEventFn ResolveManagedUIEventDispatcher()
{
	std::call_once(g_UIEventDispatchResolveOnce, []() {
		g_UIEventDispatchResolveAttempted = true;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!DotNetHost::Instance().IsInitialized() || DotNetHost::Instance().IsExportMode())
		{
			return;
		}

		g_ManagedUIDispatchEvent = reinterpret_cast<ManagedUIDispatchEventFn>(DotNetHost::Instance().GetManagedFunction(
			ResolveManagedBridgeAssemblyPath(),
			L"TerraEngine.InternalCalls, TerraEngine.CSharp",
			L"DispatchNativeUIEvent"));
	});

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (g_ManagedUIDispatchEvent == nullptr && g_UIEventDispatchResolveAttempted && !g_UIEventDispatchUnavailableLogged)
	{
		g_UIEventDispatchUnavailableLogged = true;
		const LogLevel level = DotNetHost::Instance().IsExportMode() ? LogLevel::Debug : LogLevel::Warning;
		TERRAENGINE_LOG(level, "[UI] Managed UI event dispatch is unavailable. Falling back to managed polling events where possible.");
	}

	return g_ManagedUIDispatchEvent;
}

// FindFirstUIHandleForElement is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static ManagedUIElementHandle* FindFirstUIHandleForElement(UIElement& element)
{
	std::lock_guard<std::mutex> lock(g_UIHandleMutex);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto iterator = g_UIHandleIndex.find(&element); iterator != g_UIHandleIndex.end() && !iterator->second.empty())
	{
		return *iterator->second.begin();
	}

	return nullptr;
}

// DispatchManagedUIEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void DispatchManagedUIEvent(UIElement& element, ManagedUIEventType eventType, const glm::vec2& pointerPosition)
{
	ManagedUIDispatchEventFn dispatcher = ResolveManagedUIEventDispatcher();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (dispatcher == nullptr)
	{
		return;
	}

	ManagedUIElementHandle* handle = FindFirstUIHandleForElement(element);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (handle == nullptr)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "[UI] Tried to dispatch an event for element '" << element.id << "' without an active managed handle.");
		return;
	}

	dispatcher(handle, static_cast<int>(eventType), pointerPosition.x, pointerPosition.y);
}

// DispatchManagedClickEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void DispatchManagedClickEvent(UIElement& element, const glm::vec2& pointerPosition)
{
	DispatchManagedUIEvent(element, ManagedUIEventType::Click, pointerPosition);
}

// DispatchManagedMouseEnterEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void DispatchManagedMouseEnterEvent(UIElement& element, const glm::vec2& pointerPosition)
{
	DispatchManagedUIEvent(element, ManagedUIEventType::MouseEnter, pointerPosition);
}

// DispatchManagedMouseLeaveEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void DispatchManagedMouseLeaveEvent(UIElement& element, const glm::vec2& pointerPosition)
{
	DispatchManagedUIEvent(element, ManagedUIEventType::MouseLeave, pointerPosition);
}

// SetApplicationHostEngine is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SetApplicationHostEngine(Engine* engine)
{
	std::lock_guard<std::mutex> lock(g_ApplicationBridgeMutex);
	g_ApplicationHostEngine = engine;
}

// SetApplicationEditorStopPlayModeCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SetApplicationEditorStopPlayModeCallback(ApplicationEditorStopPlayModeCallback callback)
{
	std::lock_guard<std::mutex> lock(g_ApplicationBridgeMutex);
	g_ApplicationEditorStopCallback = callback;
}

extern "C" {

	// Native_Log is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Log(int level, const char* message, const char* function, int line) {
		LogLevel logLevel = static_cast<LogLevel>(level);
		Logger::Instance().Log(logLevel, "C#", line, function, message, true);
	}

	// Native_Application_Quit is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Application_Quit() {
		if (ScriptingSystem::Instance().IsExportMode()) {
			if (Engine* hostEngine = GetApplicationHostEngine()) {
				hostEngine->SignalShutdown();
				return;
			}

			TERRAENGINE_LOG(LogLevel::Warning, "Application.Quit() was called in export mode before the host engine was registered.");
			return;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// Native_Application_OpenURL is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Application_OpenURL(const char* url) {
		TerraEngine::PlatformUtils::OpenUrl(url != nullptr ? url : "");
	}

	// Native_Application_GetPersistentDataPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Application_GetPersistentDataPath() {
		const std::filesystem::path persistentDataPath = TerraEngine::PlatformUtils::GetPersistentDataPath(
			ResourceManager::Instance().GetProductName());
		return WriteApplicationReturnText(persistentDataPath.string());
	}

	// Native_Application_GetStreamingAssetsPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Application_GetStreamingAssetsPath() {
		return WriteApplicationReturnText(ResourceManager::Instance().GetAssetRoot().string());
	}

	// Native_Application_IsEditor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_Application_IsEditor() {
		return !ScriptingSystem::Instance().IsExportMode();
	}

	// Native_Application_GetPlatform is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::uint32_t Native_Application_GetPlatform() {
		return static_cast<std::uint32_t>(TerraEngine::PlatformUtils::GetRuntimePlatform());
	}

	void Native_Scripting_PushDispatchContext(uint32_t objectId, const char* bindingId, std::uint64_t frameOrder, int workerDispatch, int exportMode, std::uint64_t objectGeneration, std::uint64_t componentGeneration, std::uint64_t bindingGeneration) {
		ScriptingSystem::ScriptDispatchContext context;
		context.workerDispatch = workerDispatch != 0;
		context.exportMode = exportMode != 0;
		context.sourceObjectId = objectId;
		context.sourceBindingId = bindingId != nullptr ? bindingId : "";
		context.frameOrder = frameOrder;
		context.objectGeneration = objectGeneration;
		context.componentGeneration = componentGeneration;
		context.bindingGeneration = bindingGeneration;
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			context.sourceObjectUuid = objOpt->getUUID();
		}
		ScriptingSystem::SetCurrentScriptDispatchContext(std::move(context));
	}

	void Native_Scripting_PopDispatchContext() {
		ScriptingSystem::ClearCurrentScriptDispatchContext();
	}

	void Native_Scripting_ReportBridgeCacheCounters(std::uint64_t hits, std::uint64_t misses, std::uint64_t avoidedCalls) {
		ScriptingSystem::RecordBridgeCacheCounters(
			static_cast<std::size_t>(hits),
			static_cast<std::size_t>(misses),
			static_cast<std::size_t>(avoidedCalls));
	}

	// Native_Application_GetProductName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Application_GetProductName() {
		return WriteApplicationReturnText(ResourceManager::Instance().GetProductName());
	}

	// Native_Application_GetVersion is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Application_GetVersion() {
		return WriteApplicationReturnText(ResourceManager::Instance().GetVersion());
	}

	// Native_Object_Create is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	uint32_t Native_Object_Create(const char* name) {
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value() && context->workerDispatch) {
			ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
			scriptingSystem.RecordScriptMutationRequest(scriptingSystem.BuildScriptMutationRequest(
				TerraEngine::Scene::SceneMutationOperation::CreateObject,
				*context,
				TerraEngine::EMPTY_UUID,
				name ? name : "New Object"));
			return InvalidObjectId();
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		// Managed object creation keeps its immediate gameplay behavior today,
		// but the operation is also expressible as a SceneMutationQueue request
		// for future worker-originated script bridges. Keeping that contract in
		// this boundary prevents C# authors from needing scheduler annotations.
		Object obj = ObjectManager::instance().createObject(name ? name : "New Object");
		return static_cast<uint32_t>(obj.EntityId());
	}

	// Native_Object_Destroy is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Object_Destroy(uint32_t objectId) {
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value() && context->workerDispatch) {
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				scriptingSystem.RecordScriptMutationRequest(scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::DeleteObject,
					*context,
					objOpt->getUUID()));
			}
			return;
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		// Destruction remains immediate for existing C# gameplay compatibility.
		// Worker jobs must use SceneMutationQueue instead, so no managed callback
		// observes object storage changing from a background thread.
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			ObjectManager::instance().removeObject(*objOpt);
		}
	}

	// Native_Object_GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Object_GetName(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		static thread_local std::string nameBuffer;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			nameBuffer = ObjectManager::instance().getObjectName(*objOpt);
			return nameBuffer.c_str();
		}
		return "";
	}

	// Native_Object_SetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Object_SetName(uint32_t objectId, const char* name) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			ObjectManager::instance().setObjectName(*objOpt, name ? name : "");
		}
	}

	// Native_Object_Find is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	uint32_t Native_Object_Find(const char* name) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (name == nullptr || name[0] == '\0') {
			return InvalidObjectId();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto objOpt = ObjectManager::instance().FindObjectByName(name)) {
			return static_cast<uint32_t>(objOpt->EntityId());
		}

		return InvalidObjectId();
	}

	// Native_Object_FindWithTag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	uint32_t Native_Object_FindWithTag(const char* tag) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (tag == nullptr || tag[0] == '\0') {
			return InvalidObjectId();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto objOpt = ObjectManager::instance().FindObjectByTag(tag)) {
			return static_cast<uint32_t>(objOpt->EntityId());
		}

		return InvalidObjectId();
	}

	// Native_MeshRenderer_GetMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_MeshRenderer_GetMesh(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		return GetMeshRendererAssetReference(objectId, "meshId", AssetType::Mesh);
	}

	// Native_MeshRenderer_SetMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_MeshRenderer_SetMesh(uint32_t objectId, const char* assetReference) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		SetMeshRendererAssetReference(objectId, "meshId", AssetType::Mesh, assetReference);
	}

	// Native_MeshRenderer_GetMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_MeshRenderer_GetMaterial(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		return GetMeshRendererAssetReference(objectId, "materialId", AssetType::Material);
	}

	// Native_MeshRenderer_SetMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_MeshRenderer_SetMaterial(uint32_t objectId, const char* assetReference) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		SetMeshRendererAssetReference(objectId, "materialId", AssetType::Material, assetReference);
	}

	// Native_HasComponent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_HasComponent(uint32_t objectId, const char* componentType) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			auto& reg = ComponentAccessRegistry();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto it = reg.find(componentType); it != reg.end()) {
				return it->second.hasComponent(*objOpt);
			}
		}
		return false;
	}

	// Native_AddComponent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_AddComponent(uint32_t objectId, const char* componentType) {
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value() && context->workerDispatch) {
			if (componentType == nullptr || componentType[0] == '\0') {
				return false;
			}
			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				scriptingSystem.RecordScriptMutationRequest(scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::AddComponent,
					*context,
					objOpt->getUUID(),
					{},
					componentType));
				return true;
			}
			return false;
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			auto& reg = ComponentAccessRegistry();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto it = reg.find(componentType); it != reg.end()) {
				it->second.addComponent(*objOpt);
				ObjectManager::instance().notifyObjectStructureChanged(*objOpt);
				return true;
			}
		}
		return false;
	}

	std::uint64_t Native_Object_GetComponentGeneration(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			return ObjectManager::instance().getObjectComponentGeneration(*objOpt);
		}
		return 0;
	}

	std::uint64_t Native_Object_GetMetadataGeneration(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			return ObjectManager::instance().getObjectMetadataGeneration(*objOpt);
		}
		return 0;
	}

	std::uint64_t Native_Object_GetBindingGeneration(uint32_t objectId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			return ObjectManager::instance().getObjectBindingGeneration(*objOpt);
		}
		return 0;
	}

	// Native_ScriptComponent_AddBinding is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_ScriptComponent_AddBinding(uint32_t objectId, const char* className) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (className == nullptr || className[0] == '\0') {
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			ScriptComponent* scriptComponent = objOpt->getComponent<ScriptComponent>();
			if (scriptComponent == nullptr) {
				scriptComponent = &objOpt->addComponent<ScriptComponent>();
			}

			auto guard = scriptComponent->acquireWriteLock();
			scriptComponent->AddBinding(TerraEngine::EMPTY_UUID, className);
			ObjectManager::instance().notifyObjectStructureChanged(*objOpt);
			return true;
		}

		return false;
	}

	// Native_ScriptComponent_HasBinding is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_ScriptComponent_HasBinding(uint32_t objectId, const char* className) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (className == nullptr || className[0] == '\0') {
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			if (ScriptComponent* scriptComponent = objOpt->getComponent<ScriptComponent>()) {
				auto guard = scriptComponent->acquireReadLock();
				return scriptComponent->HasBinding(className);
			}
		}

		return false;
	}

	// Native_ScriptComponent_GetBindingPayload is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_ScriptComponent_GetBindingPayload(uint32_t objectId, const char* bindingId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		g_ScriptBindingPayloadBuffer.clear();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (bindingId == nullptr || bindingId[0] == '\0') {
			return g_ScriptBindingPayloadBuffer.c_str();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			if (ScriptComponent* scriptComponent = objOpt->getComponent<ScriptComponent>()) {
				auto guard = scriptComponent->acquireReadLock();
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (const ScriptBinding* binding = scriptComponent->FindBinding(bindingId)) {
					// Return the authored binding payload only. Live runtime state is
					// reconstructed on demand inside the managed bridge.
					g_ScriptBindingPayloadBuffer = nlohmann::json(*binding).dump();
				}
			}
		}

		return g_ScriptBindingPayloadBuffer.c_str();
	}

	std::uint64_t Native_ScriptComponent_GetBindingVersion(uint32_t objectId, const char* bindingId) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (bindingId == nullptr || bindingId[0] == '\0') {
			return 0;
		}

		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			if (ScriptComponent* scriptComponent = objOpt->getComponent<ScriptComponent>()) {
				auto guard = scriptComponent->acquireReadLock();
				if (const ScriptBinding* binding = scriptComponent->FindBinding(bindingId)) {
					return binding->authoredStateVersion;
				}
			}
		}

		return 0;
	}

	// Native_ScriptComponent_SetBindingValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_ScriptComponent_SetBindingValue(uint32_t objectId, const char* bindingId, const char* fieldPath, const char* valueKind, const char* valueJson, const char* compatibility, const char* sourceTypeFingerprint) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (bindingId == nullptr || bindingId[0] == '\0' || fieldPath == nullptr || fieldPath[0] == '\0' || valueJson == nullptr) {
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			ScriptComponent* scriptComponent = objOpt->getComponent<ScriptComponent>();
			if (scriptComponent == nullptr) {
				return false;
			}

			{
				auto guard = scriptComponent->acquireWriteLock();
				ScriptBinding* binding = ResolveBinding(*objOpt, bindingId);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (valueKind != nullptr) {
					fieldValue.valueKind = valueKind;
				}
				fieldValue.compatibility = compatibility != nullptr && compatibility[0] != '\0' ? compatibility : "Current";
				fieldValue.persistenceState = fieldValue.compatibility;
				fieldValue.sourceTypeFingerprint = sourceTypeFingerprint != nullptr ? sourceTypeFingerprint : "";
				scriptComponent->SynchronizeStorageFromBindings();
			}
			ObjectManager::instance().notifyObjectStructureChanged(*objOpt);
			return true;
		}

		return false;
	}

	// Native_ScriptComponent_RemoveBindingValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_ScriptComponent_RemoveBindingValue(uint32_t objectId, const char* bindingId, const char* fieldPath) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (bindingId == nullptr || bindingId[0] == '\0' || fieldPath == nullptr || fieldPath[0] == '\0') {
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			ScriptComponent* scriptComponent = objOpt->getComponent<ScriptComponent>();
			if (scriptComponent == nullptr) {
				return false;
			}

			{
				auto guard = scriptComponent->acquireWriteLock();
				ScriptBinding* binding = ResolveBinding(*objOpt, bindingId);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (binding == nullptr || !binding->RemoveSerializedValue(fieldPath)) {
					return false;
				}

				scriptComponent->SynchronizeStorageFromBindings();
			}
			ObjectManager::instance().notifyObjectStructureChanged(*objOpt);
			return true;
		}

		return false;
	}

	// Native_GetComponentProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_GetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* outValue, int type) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			auto& reg = ComponentAccessRegistry();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto it = reg.find(componentType); it != reg.end()) {
				TerraEngine::Reflection::Variant val = it->second.getter(*objOpt, propertyName);
				MarshallToNative(val, outValue, static_cast<ScriptType>(type));
			}
		}
	}

	// Native_SetComponentProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_SetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* inValue, int type) {
		if (auto context = ScriptingSystem::CurrentScriptDispatchContext(); context.has_value() && context->workerDispatch) {
			if (componentType == nullptr || componentType[0] == '\0' || propertyName == nullptr || propertyName[0] == '\0' || inValue == nullptr) {
				return;
			}

			std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				TerraEngine::Scene::SceneMutationRequest request = scriptingSystem.BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::UpdateComponent,
					*context,
					objOpt->getUUID(),
					{},
					componentType);
				request.typedComponentPayload.propertyName = propertyName;
				request.typedComponentPayload.valueKind = type;
				request.typedComponentPayload.value = MarshallFromNative(inValue, static_cast<ScriptType>(type));
				request.typedComponentPayload.valid = TerraEngine::Reflection::IsValid(request.typedComponentPayload.value);
				if (!request.typedComponentPayload.valid) {
					nlohmann::json payload{
						{ "propertyName", propertyName },
						{ "valueKind", type },
						{ "value", MarshallNativeValueToJson(inValue, static_cast<ScriptType>(type)) }
					};
					request.payloadJson = payload.dump();
					request.usedJsonFallback = true;
				}
				request.coalescingKey = std::to_string(context->frameOrder)
					+ "|" + TerraEngine::ToHexString(objOpt->getUUID())
					+ "|" + componentType
					+ "|" + propertyName;
				scriptingSystem.RecordScriptMutationRequest(std::move(request));
			}
			return;
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (auto objOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			auto& reg = ComponentAccessRegistry();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (componentType != nullptr && propertyName != nullptr && inValue != nullptr && componentType[0] != '\0' && propertyName[0] != '\0') {
				if (auto it = reg.find(componentType); it != reg.end()) {
					TerraEngine::Reflection::Variant val = MarshallFromNative(inValue, static_cast<ScriptType>(type));
					it->second.setter(*objOpt, propertyName, val);
					if (std::string_view(componentType) == Transform::kComponentName) {
						ObjectManager::instance().MarkObjectTransformDirty(*objOpt);
					} else {
						ObjectManager::instance().notifyObjectStructureChanged(*objOpt);
					}
				}
			}
		}
	}

	// Native_Graphics_SetQualityPreset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Graphics_SetQualityPreset(uint32_t preset) {
		RenderSystem::Instance().SetQualityPreset(static_cast<QualityPreset>(preset));
	}

	// Native_Graphics_GetQualityPreset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	uint32_t Native_Graphics_GetQualityPreset() {
		return static_cast<uint32_t>(RenderSystem::Instance().GetQualityPreset());
	}

	// Native_Graphics_SetRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Graphics_SetRenderSettings(RenderSettings* settings) {
		if (settings) {
			RenderSystem::Instance().SetRenderSettings(*settings);
		}
	}

	// Native_Graphics_GetRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Graphics_GetRenderSettings(RenderSettings* outSettings) {
		if (outSettings) {
			*outSettings = RenderSystem::Instance().GetRenderSettings();
		}
	}

	// Native_Graphics_GetLightingDiagnosticsSummary is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Graphics_GetLightingDiagnosticsSummary(uint32_t viewType) {
		static std::string summary;
		const RenderViewType renderView = viewType == 1u ? RenderViewType::Scene : RenderViewType::Game;
		const LightingDiagnosticsSnapshot diagnostics = RenderSystem::Instance().GetLatestLightingDiagnostics(renderView);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!diagnostics.valid) {
			summary = "Lighting diagnostics are not available for the latest completed frame.";
			return summary.c_str();
		}

		summary.clear();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const LightingDiagnosticEntry& entry : diagnostics.entries) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!summary.empty()) {
				summary += "\n";
			}
			summary += entry.message;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!entry.recommendedAction.empty()) {
				summary += " ";
				summary += entry.recommendedAction;
			}
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (summary.empty()) {
			summary = "Lighting diagnostics are active and no degraded state was reported.";
		}
		return summary.c_str();
	}

	// Native_ResourceManager_GetAssetRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_ResourceManager_GetAssetRoot() {
		static std::string assetRoot;
		assetRoot = ResourceManager::Instance().GetAssetRoot().string();
		return assetRoot.c_str();
	}

	// Native_Input_IsKeyPressed is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_Input_IsKeyPressed(int key) {
		return InputSystem::Instance().IsKeyPressed(static_cast<Key>(key));
	}

	// Native_Input_IsMouseButtonPressed is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_Input_IsMouseButtonPressed(int button) {
		return InputSystem::Instance().IsMouseButtonPressed(static_cast<MouseButton>(button));
	}

	// Native_Input_GetMousePosition is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_Input_GetMousePosition(double* outX, double* outY) {
		auto [x, y] = InputSystem::Instance().GetCursorPosition();
		if (outX) *outX = x;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outY) *outY = y;
	}

	// Native_UI_WasElementClicked is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_UI_WasElementClicked(const char* elementId) {
		if (elementId == nullptr || elementId[0] == '\0') {
			return false;
		}

		return UISystem::Instance().WasElementClicked(elementId);
	}

	// Native_UI_IsElementHovered is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_UI_IsElementHovered(const char* elementId) {
		if (elementId == nullptr || elementId[0] == '\0') {
			return false;
		}

		return UISystem::Instance().IsElementHovered(elementId);
	}

	// Native_UI_GetInteractionFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::uint64_t Native_UI_GetInteractionFrame() {
		return UISystem::Instance().GetInteractionFrame();
	}

	// Native_UI_CreateElement is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void* Native_UI_CreateElement(int elementType) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr element = UIElement::CreateDefault(static_cast<UIElementType>(elementType));
		return CreateUIHandle(element, true);
	}

	// Native_UI_ReleaseHandle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_ReleaseHandle(void* elementHandle) {
		if (elementHandle == nullptr) {
			return;
		}

		std::lock_guard<std::mutex> lock(g_UIHandleMutex);
		ManagedUIElementHandle* handle = static_cast<ManagedUIElementHandle*>(elementHandle);
		ForgetUIHandleIndexLocked(handle);
		delete handle;
	}

	// Native_UI_GetRootElement is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void* Native_UI_GetRootElement(uint32_t objectId) {
		{
			std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto object = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
				UIComponent* component = object->getComponent<UIComponent>();
				if (component == nullptr) {
					return nullptr;
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (UIElement::Ptr root = component->GetRoot()) {
					return CreateUIHandle(root, false);
				}
			}
			else {
				return nullptr;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto object = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			if (UIComponent* component = object->getComponent<UIComponent>()) {
				UISystem::Instance().EnsureDocumentReady(object->EntityId(), *component);
			}
		}

		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto object = ObjectManager::instance().getObject(static_cast<entt::entity>(objectId))) {
			UIComponent* component = object->getComponent<UIComponent>();
			if (component == nullptr) {
				return nullptr;
			}

			UIElement::Ptr root = component->GetRoot();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// Native_UI_FindElementById is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void* Native_UI_FindElementById(void* elementHandle, const char* elementId) {
		if (elementId == nullptr || elementId[0] == '\0') {
			return nullptr;
		}

		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr element = ResolveUIHandle(elementHandle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!element) {
			return nullptr;
		}

		return CreateUIHandle(element->FindById(elementId), false);
	}

	// Native_UI_GetElementType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	int Native_UI_GetElementType(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			return static_cast<int>(element->type);
		}

		return static_cast<int>(UIElementType::VisualElement);
	}

	// Native_UI_IsHandleAlive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_UI_IsHandleAlive(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		return ResolveUIHandle(elementHandle) != nullptr;
	}

	// Native_UI_GetElementId is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_UI_GetElementId(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			return WriteUIBridgeReturnText(element->id);
		}

		return WriteUIBridgeReturnText({});
	}

	// Native_UI_SetElementId is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_SetElementId(void* elementHandle, const char* elementId) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			element->id = elementId != nullptr ? elementId : "";
			element->MarkDirty();
		}
	}

	// Native_UI_GetElementText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_UI_GetElementText(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			return WriteUIBridgeReturnText(element->text);
		}

		return WriteUIBridgeReturnText({});
	}

	// Native_UI_SetElementText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_SetElementText(void* elementHandle, const char* text) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			element->text = text != nullptr ? text : "";
			element->MarkDirty();
		}
	}

	// Native_UI_GetElementVisible is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_UI_GetElementVisible(void* elementHandle) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			return element->IsVisible();
		}

		return false;
	}

	// Native_UI_SetElementVisible is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_SetElementVisible(void* elementHandle, bool visible) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			element->SetVisible(visible);
		}
	}

	// Native_UI_AddChild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_AddChild(void* parentHandle, void* childHandle) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr parent = ResolveUIHandle(parentHandle);
		UIElement::Ptr child = ResolveUIHandle(childHandle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!parent || !child) {
			return;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (parent->InsertChild(parent->children.size(), child)) {
			DropDetachedUIOwnership(child);
		}
	}

	// Native_UI_RemoveChild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_RemoveChild(void* parentHandle, void* childHandle) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr parent = ResolveUIHandle(parentHandle);
		UIElement::Ptr child = ResolveUIHandle(childHandle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!parent || !child) {
			return;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (parent->RemoveChild(child)) {
			PromoteDetachedUIOwnership(child);
		}
	}

	// Native_UI_ClearChildren is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_ClearChildren(void* parentHandle) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr parent = ResolveUIHandle(parentHandle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!parent) {
			return;
		}

		std::vector<UIElement::Ptr> detachedChildren = parent->children;
		parent->ClearChildren();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const UIElement::Ptr& child : detachedChildren) {
			PromoteDetachedUIOwnership(child);
		}
	}

	// Native_UI_SetStyleFloat is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_SetStyleFloat(void* elementHandle, int property, float value) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			// Dispatch by explicit engine state so each case documents one supported behavior.
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

	// Native_UI_GetStyleFloat is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	float Native_UI_GetStyleFloat(void* elementHandle, int property) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			// Dispatch by explicit engine state so each case documents one supported behavior.
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

	// Native_UI_SetStyleColor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_SetStyleColor(void* elementHandle, int property, float x, float y, float z, float w) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		if (UIElement::Ptr element = ResolveUIHandle(elementHandle)) {
			const glm::vec4 color(x, y, z, w);
			// Dispatch by explicit engine state so each case documents one supported behavior.
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

	// Native_UI_GetStyleColor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Native_UI_GetStyleColor(void* elementHandle, int property, float* outX, float* outY, float* outZ, float* outW) {
		std::shared_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		std::array<float*, 4> outputs = { outX, outY, outZ, outW };
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (float* output : outputs) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (output != nullptr) {
				*output = 0.0f;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (color.has_value()) {
				if (outX) *outX = color->x;
				if (outY) *outY = color->y;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (outZ) *outZ = color->z;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (outW) *outW = color->w;
			}
		}
	}

	// Native_UI_SetEventCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_UI_SetEventCallback(void* elementHandle, int eventType, bool enabled) {
		std::unique_lock<std::shared_mutex> lock(g_ScriptGlueMutex);
		UIElement::Ptr element = ResolveUIHandle(elementHandle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!element) {
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ResolveManagedUIEventDispatcher() == nullptr) {
			return false;
		}

		UIElement::UIEventCallback callback = nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (enabled) {
			// Dispatch by explicit engine state so each case documents one supported behavior.
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

		// Dispatch by explicit engine state so each case documents one supported behavior.
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

	// Native_Scene_LoadScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Scene_LoadScene(const char* path) {
		static thread_local std::string uuidBuffer;
		if (path == nullptr) return "";
		TerraEngine::UUID id = SceneManager::Instance().LoadScene(path);
		uuidBuffer = TerraEngine::ToHexString(id);
		return uuidBuffer.c_str();
	}

	// Native_Scene_LoadPrefab is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const char* Native_Scene_LoadPrefab(const char* path) {
		static thread_local std::string uuidBuffer;
		if (path == nullptr) return "";
		TerraEngine::UUID id = SceneManager::Instance().LoadPrefab(path);
		uuidBuffer = TerraEngine::ToHexString(id);
		return uuidBuffer.c_str();
	}

	// Native_Scene_SetActiveScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool Native_Scene_SetActiveScene(const char* uuidHex) {
		if (uuidHex == nullptr) return false;
		auto id = TerraEngine::ParseUUID(uuidHex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!id) return false;
		return SceneManager::Instance().SetActiveScene(*id);
	}

	// Native_Time_GetDeltaTime is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	float Native_Time_GetDeltaTime() {
		return ScriptingSystem::Instance().GetLastDeltaTime();
	}

}
