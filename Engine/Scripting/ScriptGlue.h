#pragma once

#include "Core/Export.h"
#include <cstdint>

extern "C" {
	// Logging
	TERRAENGINE_API void Native_Log(int level, const char* message, const char* function, int line);

	// Object Management
	TERRAENGINE_API uint32_t Native_Object_Create(const char* name);
	TERRAENGINE_API void Native_Object_Destroy(uint32_t objectId);

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
