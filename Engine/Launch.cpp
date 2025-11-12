#include <iostream>

#include <entt/entt.hpp>

#ifdef _MSC_VER
#    include <crtdbg.h>
#endif

//#include <windows.h>
#include "MainEngine.h"

//int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
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

    // Enable run-time memory check for debug builds.
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

#endif

    Engine engine;

    engine.Run();

    return 0;
}