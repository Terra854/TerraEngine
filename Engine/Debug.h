// Debug.h — Contains declarations for the Debug constants, type definitions, and lightweight declarations used by module-specific declarations, data, and implementation details.

#pragma once

#if defined(DEBUG) | defined(_DEBUG)
#define new new ( _NORMAL_BLOCK , __FILE__ , __LINE__ )
#define _CRTDBG_MAP_ALLOC
#endif
