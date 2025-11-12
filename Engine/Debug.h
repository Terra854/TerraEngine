#pragma once

#if defined(DEBUG) | defined(_DEBUG)
#define new new ( _NORMAL_BLOCK , __FILE__ , __LINE__ )
#define _CRTDBG_MAP_ALLOC
#endif
