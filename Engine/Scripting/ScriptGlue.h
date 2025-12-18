#pragma once

#include "Core/Export.h"
#include <cstdint>

extern "C" {
    // Logging
    TERRAENGINE_API void Native_Log(int level, const char* message);

    // Entity Management (Placeholders for now)
    TERRAENGINE_API uint32_t Native_Entity_Create(const char* name);
    TERRAENGINE_API void Native_Entity_Destroy(uint32_t entityId);

    // Transform
    TERRAENGINE_API void Native_Transform_GetPosition(uint32_t entityId, float* x, float* y, float* z);
    TERRAENGINE_API void Native_Transform_SetPosition(uint32_t entityId, float x, float y, float z);
}
