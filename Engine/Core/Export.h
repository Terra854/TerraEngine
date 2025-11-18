// Centralized export/import macro for the TerraEngine shared library.
// When building the engine DLL on Windows, we must explicitly mark all public
// symbols with dllexport so consumers such as the launcher can link against
// the generated import library. Outside Windows the macro resolves to a no-op
// because other platforms rely on ELF visibility instead of __declspec.
#pragma once

#if defined(_WIN32)
#if defined(TERRAENGINE_ENGINE_BUILD)
// Building the engine itself: export symbols for consumers.
#define TERRAENGINE_API __declspec(dllexport)
#else
// Using the engine from another target: import previously exported symbols.
#define TERRAENGINE_API __declspec(dllimport)
#endif
#else
// Non-Windows platforms rely on default visibility; keep API macro empty.
#define TERRAENGINE_API
#endif
