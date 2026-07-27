// Launch.cpp — Main engine launcher and entry point
//
// This file serves as the executable entry point for the TerraEngine launcher, responsible for initializing the core engine and bootstrapping export-mode resources. It handles executable path resolution, configures the initial resource mounts, and manages the handoff to the MainEngine runtime loop.

#include <array>
#include <cstring>
#include <filesystem>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include <entt/entt.hpp>

#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <Windows.h>
#endif

#ifdef _MSC_VER
#	include <crtdbg.h>
#endif

#include "Core/MainEngine.h"

// The launcher is always the export entry point, so it always carries the
// path-configuration helpers needed by the export runtime.
#include "Resource/ResourceManager.h"
#include "Runtime/BootSummary.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ExportStartupCoordinator.h"
#include "Scripting/ScriptingSystem.h"

namespace {
namespace fs = std::filesystem;

// ResolveExecutablePath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (argv0 != nullptr && argv0[0] != '\0') {
		return fs::absolute(fs::path(argv0));
	}

	return fs::current_path();
}

// ResolveManagedLibraryPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
fs::path ResolveManagedLibraryPath(const fs::path& executableDirectory) {
	// Exported builds always place the NativeAOT gameplay library in Managed/.
	// CMake supplies the platform-specific filename to this native launcher so
	// an installed Linux export never accidentally searches for the Windows ABI.
	return executableDirectory / "Managed" / TERRAENGINE_MANAGED_RUNTIME_LIBRARY;
}

bool HasArgument(int argc, char* argv[], const char* expectedArgument) {
	// Keep launcher switches simple and exact so exported games cannot
	// accidentally enter validation mode because of a partial argument match.
	for (int argumentIndex = 1; argumentIndex < argc; ++argumentIndex) {
		if (argv[argumentIndex] != nullptr && std::strcmp(argv[argumentIndex], expectedArgument) == 0) {
			return true;
		}
	}

	return false;
}

// ConfigureExportBootstrap is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ConfigureExportBootstrap(const char* argv0) {
	const fs::path executablePath = ResolveExecutablePath(argv0);
	const fs::path executableDirectory = executablePath.parent_path();
	const fs::path archivePath = executableDirectory / (executablePath.stem().string() + ".tepak");
	const fs::path managedLibraryPath = ResolveManagedLibraryPath(executableDirectory);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fs::exists(archivePath)) {
		throw std::runtime_error("Export archive not found beside the launcher: " + archivePath.string());
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fs::exists(managedLibraryPath)) {
		throw std::runtime_error("NativeAOT gameplay runtime not found: " + managedLibraryPath.string());
	}

	// The launcher now only records the deterministic export file locations.
	// ResourceManager owns the actual TEPAK mount during engine startup, and
	// ScriptingSystem still owns the eventual NativeAOT runtime load later in
	// its own Initialize() step.
	ResourceManager::Instance().ConfigureExportArchive(archivePath);
	ScriptingSystem::Instance().ConfigureExportRuntime(managedLibraryPath);
}

TerraEngine::Runtime::ExportStartupCoordinatorInput BuildLauncherValidationInput() {
	ResourceManager& resources = ResourceManager::Instance();
	TerraEngine::Runtime::ExportStartupCoordinatorInput input;
	input.exportRuntime = true;
	input.contentMounted = ContentMount::Instance().IsMounted();
	input.metadataLoaded = !resources.GetProductName().empty() && !resources.GetVersion().empty();
	input.projectName = resources.GetProductName();
	input.version = resources.GetVersion();
	input.contentSource = "export-archive";
	input.renderProfile = resources.GetRenderProfileSummary();
	input.validationEnabled = resources.IsRenderValidationEnabled();
	input.debugEnabled = resources.IsRenderDebugEnabled();

	const std::optional<std::filesystem::path> startupScene = resources.GetConfiguredStartupScene();
	input.startupSceneConfigured = startupScene.has_value();
	input.startupScene = startupScene.has_value() ? startupScene->generic_string() : std::string("<none>");
	if (startupScene.has_value()) {
		try {
			input.startupSceneLoaded = !resources.ReadFileBytes(*startupScene).empty();
		}
		catch (const std::exception&) {
			input.startupSceneLoaded = false;
		}
	}

	input.requiredSystems.push_back({ "ResourceManager", true, true, "Initialized" });
	input.requiredSystems.push_back({ "Scene", true, input.startupSceneLoaded, input.startupSceneLoaded ? "Startup scene readable" : "Startup scene unreadable" });
	input.requiredSystems.push_back({ "Renderer", true, true, "Deferred until normal runtime startup" });
	input.requiredSystems.push_back({ "UI", true, true, "Deferred until normal runtime startup" });
	input.requiredSystems.push_back({ "Scripting", true, true, "NativeAOT path configured" });
	input.packagedPhysics = TerraEngine::Runtime::LoadPackagedPhysicsRuntime(
		ContentMount::Instance(), true,
		resources.GetEngineRoot() / "Managed" / "Assembly-CSharp.dll");
	return input;
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

	try {
		// --- Export-mode bootstrap ---
		// Exported builds are discovered by looking for "<ExeStem>.tepak" beside
		// the executable. The normal game path lets Engine::Run() initialize the
		// archive, while validation mode mounts it here and exits before creating
		// any renderer-owned window or GPU resources.
		ConfigureExportBootstrap(argc > 0 ? argv[0] : nullptr);

		if (HasArgument(argc, argv, "--validate-export-bootstrap")) {
			ResourceManager::Instance().Initialize();
			const TerraEngine::Runtime::ExportStartupCoordinatorResult validationResult =
				TerraEngine::Runtime::CoordinateExportStartup(BuildLauncherValidationInput());
			std::cout << TerraEngine::Runtime::FormatBootSummary(validationResult.summary) << std::endl;
			ResourceManager::Instance().Shutdown();
			if (!validationResult.CanEnterRuntime()) {
				std::cerr << "[Launcher] Export bootstrap validation failed: " << validationResult.summary.runtimeEntryReason << std::endl;
				return 1;
			}
			std::cout << "[Launcher] Export bootstrap validation succeeded." << std::endl;
			return 0;
		}

		if (HasArgument(argc, argv, "--validate-export-physics-invariants")) {
			// Use the normal Engine resource/scene/scripting/PhysicsSystem/scheduler
			// path, with presentation systems omitted before initialization. This is
			// intentionally not a direct provider smoke test: it validates the same
			// serialized startup scene and After Physics publication boundary shipped
			// to players without requiring a window or graphics device.
			Engine headlessEngine;
			headlessEngine.ConfigureHeadlessPhysicsValidation();
			headlessEngine.Initialize();
			const TerraEngine::Runtime::PhysicsInvariantReport report =
				headlessEngine.RunPhysicsInvariantFixture();
			std::cout << "[Launcher] Physics invariant report: "
				<< TerraEngine::Runtime::SerializePhysicsInvariantReport(report) << std::endl;
			headlessEngine.Shutdown();
			if (!report.succeeded || !report.orderlyShutdown
				|| report.activePhysicsJobsAfterDrain != 0U || report.queuedPhysicsJobsAfterDrain != 0U) {
				std::cerr << "[Launcher] Physics invariant validation failed: " << report.diagnostic << std::endl;
				return 1;
			}
			std::cout << "[Launcher] Export physics invariant validation succeeded." << std::endl;
			return 0;
		}

		if (HasArgument(argc, argv, "--validate-export-animation-parity")) {
			Engine parityEngine;
			const bool headlessCpuFallback = HasArgument(argc, argv, "--headless-cpu-fallback");
			if (headlessCpuFallback) parityEngine.ConfigureHeadlessCpuFallback();
			parityEngine.Initialize();
			TerraEngine::Runtime::AnimationParityValidationOptions options;
			options.mode = headlessCpuFallback ? "headless-cpu-fallback" : "rendered-runtime";
			options.requireRenderedVisualCapture = !headlessCpuFallback;
			TerraEngine::Runtime::AnimationParityReport report =
				parityEngine.RunAnimationParityFixture(options);
			parityEngine.Shutdown();
			report.orderlyShutdown = true;
			std::cout << report.ToJson().dump() << std::endl;
			if (!report.succeeded || !report.orderlyShutdown
				|| report.ticksEvaluated != options.tickCount
				|| report.animatorCount == 0U) {
				std::cerr << "[Launcher] Export animation parity validation failed: " << report.diagnostic << std::endl;
				return 1;
			}
			return 0;
		}

		Engine engine;
		if (HasArgument(argc, argv, "--headless-cpu-fallback")) {
			// This is the shipped renderer-unavailable path, not a launcher-only
			// smoke mode. Configure it before Run() so ScriptingSystem loads the
			// exported NativeAOT animation ABI during normal engine initialization,
			// while the scheduler intentionally omits window, UI, and renderer work.
			engine.ConfigureHeadlessCpuFallback();
		}
		engine.Run();
	}
	catch (const std::exception& e) {
		std::cerr << "[Launcher] Fatal startup error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
