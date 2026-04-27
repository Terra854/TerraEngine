// ComponentRegistry.h — Global interface for engine component registration.
//
// This file provides the entry point for initializing the engine's reflection
// database with all supported component types. It is typically called during
// engine startup to ensure that the reflection system is fully populated before
// any scenes or objects are loaded.

#pragma once
#include "Core/Export.h"

namespace ComponentRegistry {
	TERRAENGINE_API void RegisterAllComponents();
}
