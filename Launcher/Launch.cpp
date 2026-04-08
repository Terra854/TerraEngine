#include <array>
#include <filesystem>
#include <exception>
#include <iostream>
#include <string>

#include <entt/entt.hpp>

#ifdef _MSC_VER
#	include <crtdbg.h>
#endif

#include "Core/MainEngine.h"

// The launcher is always the export entry point, so it always carries the
// path-configuration helpers needed by the export runtime.
#include "Resource/ResourceManager.h"
#include "Scripting/ScriptingSystem.h"

namespace {
namespace fs = std::filesystem;

fs::path ResolveExecutablePath(const char* argv0) {
#ifdef _WIN32
	// Query the OS for the real executable path so exported builds keep working
	// even when the process is launched from a different current directory.
	std::array<wchar_t, 32768> buffer{};
	const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (length > 0 && length < buffer.size()) {
		return fs::path(std::wstring(buffer.data(), length));
	}
#endif

	if (argv0 != nullptr && argv0[0] != '\0') {
		return fs::absolute(fs::path(argv0));
	}

	return fs::current_path();
}

fs::path ResolveManagedLibraryPath(const fs::path& executableDirectory) {
	// Exported builds always place the NativeAOT gameplay library in Managed/
	// using the fixed Assembly-CSharp name, so the launcher can derive it
	// without duplicating that path inside game.json.
	return executableDirectory / "Managed" / "Assembly-CSharp.dll";
}

void ConfigureExportBootstrap(const char* argv0) {
	const fs::path executablePath = ResolveExecutablePath(argv0);
	const fs::path executableDirectory = executablePath.parent_path();
	const fs::path archivePath = executableDirectory / (executablePath.stem().string() + ".tepak");

	// The launcher now only records the deterministic export file locations.
	// ResourceManager owns the actual TEPAK mount during engine startup, and
	// ScriptingSystem still owns the eventual NativeAOT runtime load later in
	// its own Initialize() step.
	ResourceManager::Instance().ConfigureExportArchive(archivePath);
	ScriptingSystem::Instance().ConfigureExportRuntime(ResolveManagedLibraryPath(executableDirectory));
}
} // namespace

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

	// --- Export-mode bootstrap ---
	// Exported builds are discovered by looking for "<ExeStem>.tepak" beside the
	// executable, but the archive is now mounted later by ResourceManager during
	// engine startup instead of before Engine::Run() begins.
	ConfigureExportBootstrap(argc > 0 ? argv[0] : nullptr);

	Engine engine;

	try {
		engine.Run();
	}
	catch (const std::exception& e) {
		std::cerr << "[Launcher] Fatal startup error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
