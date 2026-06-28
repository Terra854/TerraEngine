// ScriptGlue.h — Contains declarations for the Script Glue file-specific types, helper functions, and module integration code used by native script glue, exported C# vtables, and scripting system lifecycle state.

#pragma once

#include "Core/Export.h"
#include <cstdint>

class Engine;

// The editor executable registers a plain function pointer so gameplay code can
// stop Play Mode without the engine DLL depending directly on editor headers.
using ApplicationEditorStopPlayModeCallback = void (*)();

// MainEngine registers the active host engine so Application.Quit() can signal
// the standalone runtime loop from inside the script bridge.
TERRAENGINE_API void SetApplicationHostEngine(Engine* engine);
TERRAENGINE_API void SetApplicationEditorStopPlayModeCallback(ApplicationEditorStopPlayModeCallback callback);

extern "C" {
	// Logging
	TERRAENGINE_API void Native_Log(int level, const char* message, const char* function, int line);

	// Application control and metadata
	TERRAENGINE_API void Native_Application_Quit();
	TERRAENGINE_API void Native_Application_OpenURL(const char* url);
	TERRAENGINE_API const char* Native_Application_GetPersistentDataPath();
	TERRAENGINE_API const char* Native_Application_GetStreamingAssetsPath();
	TERRAENGINE_API bool Native_Application_IsEditor();
	TERRAENGINE_API std::uint32_t Native_Application_GetPlatform();
	TERRAENGINE_API const char* Native_Application_GetProductName();
	TERRAENGINE_API const char* Native_Application_GetVersion();

	// Object Management
	TERRAENGINE_API uint32_t Native_Object_Create(const char* name);
	TERRAENGINE_API void Native_Object_Destroy(uint32_t objectId);
	TERRAENGINE_API const char* Native_Object_GetName(uint32_t objectId);
	TERRAENGINE_API void Native_Object_SetName(uint32_t objectId, const char* name);
	TERRAENGINE_API uint32_t Native_Object_Find(const char* name);
	TERRAENGINE_API uint32_t Native_Object_FindWithTag(const char* tag);
	TERRAENGINE_API const char* Native_MeshRenderer_GetMesh(uint32_t objectId);
	TERRAENGINE_API void Native_MeshRenderer_SetMesh(uint32_t objectId, const char* assetReference);
	TERRAENGINE_API const char* Native_MeshRenderer_GetMaterial(uint32_t objectId);
	TERRAENGINE_API void Native_MeshRenderer_SetMaterial(uint32_t objectId, const char* assetReference);

	// Reflection-backed component access
	TERRAENGINE_API bool Native_HasComponent(uint32_t objectId, const char* componentType);
	TERRAENGINE_API bool Native_AddComponent(uint32_t objectId, const char* componentType);
	TERRAENGINE_API std::uint64_t Native_Object_GetMetadataGeneration(uint32_t objectId);
	TERRAENGINE_API std::uint64_t Native_Object_GetComponentGeneration(uint32_t objectId);
	TERRAENGINE_API std::uint64_t Native_Object_GetBindingGeneration(uint32_t objectId);
	TERRAENGINE_API void Native_GetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* outValue, int type);
	TERRAENGINE_API void Native_SetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* inValue, int type);

	// Runtime UI queries let gameplay scripts react to authored .tuiml buttons
	// without reaching into editor-only state or native engine internals.
	TERRAENGINE_API bool Native_UI_WasElementClicked(const char* elementId);
	TERRAENGINE_API bool Native_UI_IsElementHovered(const char* elementId);
	TERRAENGINE_API std::uint64_t Native_UI_GetInteractionFrame();
	TERRAENGINE_API void* Native_UI_CreateElement(int elementType);
	TERRAENGINE_API void Native_UI_ReleaseHandle(void* elementHandle);
	TERRAENGINE_API void* Native_UI_GetRootElement(uint32_t objectId);
	TERRAENGINE_API void* Native_UI_FindElementById(void* elementHandle, const char* elementId);
	TERRAENGINE_API int Native_UI_GetElementType(void* elementHandle);
	TERRAENGINE_API bool Native_UI_IsHandleAlive(void* elementHandle);
	TERRAENGINE_API const char* Native_UI_GetElementId(void* elementHandle);
	TERRAENGINE_API void Native_UI_SetElementId(void* elementHandle, const char* elementId);
	TERRAENGINE_API const char* Native_UI_GetElementText(void* elementHandle);
	TERRAENGINE_API void Native_UI_SetElementText(void* elementHandle, const char* text);
	TERRAENGINE_API bool Native_UI_GetElementVisible(void* elementHandle);
	TERRAENGINE_API void Native_UI_SetElementVisible(void* elementHandle, bool visible);
	TERRAENGINE_API void Native_UI_AddChild(void* parentHandle, void* childHandle);
	TERRAENGINE_API void Native_UI_RemoveChild(void* parentHandle, void* childHandle);
	TERRAENGINE_API void Native_UI_ClearChildren(void* parentHandle);
	TERRAENGINE_API void Native_UI_SetStyleFloat(void* elementHandle, int property, float value);
	TERRAENGINE_API float Native_UI_GetStyleFloat(void* elementHandle, int property);
	TERRAENGINE_API void Native_UI_SetStyleColor(void* elementHandle, int property, float x, float y, float z, float w);
	TERRAENGINE_API void Native_UI_GetStyleColor(void* elementHandle, int property, float* outX, float* outY, float* outZ, float* outW);
	TERRAENGINE_API bool Native_UI_SetEventCallback(void* elementHandle, int eventType, bool enabled);

	// Scene transactions let managed callbacks submit structural/reflection
	// writes as one validated batch instead of mutating live ObjectManager state.
	TERRAENGINE_API const char* Native_Scene_SubmitTransactionJson(const char* transactionJson);

	// Script binding helpers bypass the generic property layer because gameplay
	// scripts live inside one ordered binding container component.
	TERRAENGINE_API bool Native_ScriptComponent_AddBinding(uint32_t objectId, const char* className);
	TERRAENGINE_API bool Native_ScriptComponent_HasBinding(uint32_t objectId, const char* className);
	TERRAENGINE_API const char* Native_ScriptComponent_GetBindingPayload(uint32_t objectId, const char* bindingId);
	TERRAENGINE_API std::uint64_t Native_ScriptComponent_GetBindingVersion(uint32_t objectId, const char* bindingId);
	TERRAENGINE_API void Native_ScriptComponent_ReportObservedVersion(uint32_t objectId, const char* bindingId, std::uint64_t observedVersion);
	TERRAENGINE_API bool Native_ScriptComponent_SetBindingValue(uint32_t objectId, const char* bindingId, const char* fieldPath, const char* valueKind, const char* valueJson, const char* compatibility, const char* sourceTypeFingerprint);
	TERRAENGINE_API bool Native_ScriptComponent_RemoveBindingValue(uint32_t objectId, const char* bindingId, const char* fieldPath);
	TERRAENGINE_API void Native_Scripting_PushDispatchContext(uint32_t objectId, const char* bindingId, std::uint64_t frameOrder, int workerDispatch, int exportMode, std::uint64_t objectGeneration, std::uint64_t componentGeneration, std::uint64_t bindingGeneration);
	TERRAENGINE_API void Native_Scripting_PopDispatchContext();
	TERRAENGINE_API void Native_Scripting_ReportBridgeCacheCounters(std::uint64_t hits, std::uint64_t misses, std::uint64_t avoidedCalls);
}
