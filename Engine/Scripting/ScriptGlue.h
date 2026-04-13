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
	TERRAENGINE_API void Native_GetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* outValue, int type);
	TERRAENGINE_API void Native_SetComponentProperty(uint32_t objectId, const char* componentType, const char* propertyName, void* inValue, int type);

	// Script binding helpers bypass the generic property layer because gameplay
	// scripts live inside one ordered binding container component.
	TERRAENGINE_API bool Native_ScriptComponent_AddBinding(uint32_t objectId, const char* className);
	TERRAENGINE_API bool Native_ScriptComponent_HasBinding(uint32_t objectId, const char* className);
	TERRAENGINE_API const char* Native_ScriptComponent_GetBindingPayload(uint32_t objectId, const char* bindingId);
	TERRAENGINE_API bool Native_ScriptComponent_SetBindingValue(uint32_t objectId, const char* bindingId, const char* fieldPath, const char* valueKind, const char* valueJson, const char* compatibility, const char* sourceTypeFingerprint);
	TERRAENGINE_API bool Native_ScriptComponent_RemoveBindingValue(uint32_t objectId, const char* bindingId, const char* fieldPath);
}
