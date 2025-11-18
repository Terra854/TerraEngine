#include <iostream>

#include <entt/entt.hpp>

#ifdef _MSC_VER
#	include <crtdbg.h>
#endif

#include "Core/MainEngine.h"

// Entry point responsible for bootstrapping the engine from the launcher executable.
// The engine implementation lives in the TerraEngine-Engine dynamic library and is
// loaded through the shared headers provided to this target.
int main(int argc, char *argv[])
{
	/* Uncomment to open console window for Release build
	// Allocate a console for this app
	AllocConsole();

	// Redirect std IO streams to the console
	freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
	freopen_s((FILE**)stdin, "CONIN$", "r", stdin);
	*/

#if defined(_MSC_VER) && (defined(DEBUG) || defined(_DEBUG))
	// Enable run-time memory check for debug builds so leaks and invalid frees are reported
	// early during development when running via the launcher application.
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	Engine engine;

	engine.Run();

	return 0;
}
