// Launch.cpp — Main engine launcher and entry point
//
// This file serves as the executable entry point for the TerraEngine launcher, responsible for initializing the core engine and bootstrapping export-mode resources. It handles executable path resolution, configures the initial resource mounts, and manages the handoff to the MainEngine runtime loop.

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
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
#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationSystem.h"
#include "NavigationApplicationValidation.h"
#include "LocomotionValidationHarness.h"
#include "ThreadPool/ThreadPool.h"

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

struct NavigationValidationCommand final {
	fs::path optionsPath;
	fs::path reportPath;
};

std::optional<NavigationValidationCommand> FindNavigationValidationCommand(
	int argc, char* argv[], const char* flag = "--validate-export-navigation-agents") {
	for(int index = 1; index < argc; ++index) {
		if(argv[index] == nullptr || std::strcmp(argv[index], flag) != 0) continue;
		if(index + 2 >= argc || argv[index + 1] == nullptr || argv[index + 2] == nullptr) return std::nullopt;
		return NavigationValidationCommand{fs::path(argv[index + 1]), fs::path(argv[index + 2])};
	}
	return std::nullopt;
}

nlohmann::json ReadNavigationValidationOptions(const fs::path& path) {
	std::ifstream input(path, std::ios::binary);
	if(!input) throw std::runtime_error("Could not open navigation validation options: " + path.string());
	nlohmann::json options;
	input >> options;
	if(!options.is_object()) throw std::runtime_error("Navigation validation options must be a JSON object.");
	return options;
}

bool WriteNavigationValidationReport(const fs::path& path, const nlohmann::json& report) {
	std::error_code error;
	if(!path.parent_path().empty()) fs::create_directories(path.parent_path(), error);
	if(error) return false;
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if(!output) return false;
	output << report.dump(2);
	output.flush();
	return output.good();
}

std::optional<std::uint32_t> ReadNavigationValidationWorkerCount(const nlohmann::json& options) {
	constexpr const char* key = "requiredWorkerCount";
	const auto found = options.find(key);
	if(found == options.end()) return std::nullopt;
	if(!found->is_number_unsigned() || found->get<std::uint64_t>() == 0U
		|| found->get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
		throw std::runtime_error(std::string("Navigation validation ") + key + " must be a positive unsigned 32-bit integer.");
	}
	return static_cast<std::uint32_t>(found->get<std::uint64_t>());
}

// Keep this narrow enum in the validation host instead of accepting an arbitrary
// settings object. Gameplay exports never receive a route-budget tuning switch.
enum class ManagedOutcomeProbe : std::uint8_t {
	None,
	Partial,
	Budget
};

ManagedOutcomeProbe ReadManagedOutcomeProbe(const nlohmann::json& options) {
	const auto found = options.find("managedOutcomeProbe");
	if(found == options.end()) return ManagedOutcomeProbe::None;
	if(!found->is_string()) {
		throw std::runtime_error("Navigation validation managedOutcomeProbe must be 'partial' or 'budget'.");
	}
	const std::string& value = found->get_ref<const std::string&>();
	if(value == "partial") return ManagedOutcomeProbe::Partial;
	if(value == "budget") return ManagedOutcomeProbe::Budget;
	throw std::runtime_error("Navigation validation managedOutcomeProbe must be 'partial' or 'budget'.");
}

void ConfigureNavigationValidationBeforeEngineInitialization(const nlohmann::json& options) {
	if(const std::optional<std::uint32_t> workers = ReadNavigationValidationWorkerCount(options)) {
		const std::string value = std::to_string(*workers);
#ifdef _WIN32
		if(_putenv_s("TERRAENGINE_THREADPOOL_WORKERS", value.c_str()) != 0)
			throw std::runtime_error("Could not configure TERRAENGINE_THREADPOOL_WORKERS for navigation validation.");
#else
		if(setenv("TERRAENGINE_THREADPOOL_WORKERS", value.c_str(), 1) != 0)
			throw std::runtime_error("Could not configure TERRAENGINE_THREADPOOL_WORKERS for navigation validation.");
#endif
	}

	// The workload fixture qualifies a 1,000-agent cohort. Keep the documented
	// default unchanged, but raise this dedicated pre-init validation host to the
	// explicit 4,096 request ceiling before NavigationSystem captures settings.
	TerraEngine::Navigation::NavigationSettings settings = TerraEngine::Navigation::NavigationSettings::Defaults();
	settings.maxAdmittedRequests = 4096U;
	if(ReadManagedOutcomeProbe(options) == ManagedOutcomeProbe::Budget) {
		// The one-point cap is a disposable live-fixture preset. It is never
		// serialized into a project or exposed through the managed/gameplay API.
		settings.maxRoutePoints = 1U;
	}
	if(!TerraEngine::Navigation::NavigationSystem::Instance().ConfigureSettings(settings))
		throw std::runtime_error("Navigation validation could not configure its pre-initialization request capacity.");
}

int RunExportNavigationAgentValidation(const NavigationValidationCommand& command, bool headlessCpuFallback) {
	nlohmann::json report = {{"succeeded", false}, {"orderlyShutdown", false}};
	Engine validationEngine;
	bool initialized = false;
	try {
		nlohmann::json options = ReadNavigationValidationOptions(command.optionsPath);
		if(ReadManagedOutcomeProbe(options) != ManagedOutcomeProbe::None) {
			// The C# fixture is deliberately verified in an interactive editor Play
			// session. Export validation owns a native workload loop and cannot make
			// an honest MCP/UI observation of its managed log markers.
			throw std::runtime_error("managedOutcomeProbe is editor-only; launch the installed editor validation Play workflow instead.");
		}
		ConfigureNavigationValidationBeforeEngineInitialization(options);
		options["executionMode"] = "export";
		options["headlessCpuFallback"] = headlessCpuFallback;
		// A non-headless export must prove a real rendered presentation; the core
		// helper reads renderer statistics rather than trusting this host flag.
		options["requireRenderedPresentation"] = !headlessCpuFallback;
		if(headlessCpuFallback) validationEngine.ConfigureHeadlessCpuFallback();
		validationEngine.Initialize();
		initialized = true;
		// Export mode owns its engine updates, so keep advancing bounded batches
		// until the shared validator reaches a terminal state. Editor Play Mode
		// deliberately does only one observation from each normal editor frame.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
		do {
			report = TerraEngine::Navigation::Validation::RunNavigationApplicationValidation(validationEngine, options);
			if(report.value("pending", false) && std::chrono::steady_clock::now() >= deadline) {
				report["succeeded"] = false;
				report["pending"] = false;
				report["failure"] = "The export host exceeded its bounded navigation validation deadline.";
			}
		} while(report.value("pending", false));
		validationEngine.Shutdown();
		initialized = false;
		report["orderlyShutdown"] = true;
	}
	catch(const std::exception& exception) {
		if(initialized) {
			try { validationEngine.Shutdown(); report["orderlyShutdown"] = true; }
			catch(...) { report["orderlyShutdown"] = false; }
		}
		else {
			// Scheduler validation can fail before Engine::Initialize enters its
			// system-unwind block, but after starting the worker pool. Join those
			// workers before returning a failed report instead of terminating while
			// DLL statics still own running threads. This is not a successful run.
			ThreadPool::instance().shutdown();
		}
		report["succeeded"] = false;
		report["diagnostic"] = std::string("Export navigation validation failed: ") + exception.what();
	}
	// Option validation can fail before the shared helper constructs its normal
	// report. Keep the installed runner's required evidence keys explicit there.
	if(!report.contains("actualWorkerCount")) report["actualWorkerCount"] = 0U;
	if(!report.contains("executedCaseCount")) report["executedCaseCount"] = 0U;
	// Make the host-only pre-init capacity explicit in acceptance evidence. The
	// core report supplies the validated population, including smaller cohorts.
	report["configuredMaxAdmittedRequests"] = 4096U;
	report["configuredAgentPopulation"] = report.value("population", 0U);
	if(!WriteNavigationValidationReport(command.reportPath, report)) {
		std::cerr << "[Launcher] Could not write navigation validation report: " << command.reportPath << std::endl;
		return 1;
	}
	const bool succeeded = report.value("succeeded", false) && report.value("orderlyShutdown", false)
		&& !report.value("pending", false);
	// Runtime validation records its actionable reason in failure; startup
	// exceptions use diagnostic. Preserve either instead of hiding runtime errors.
	if(!succeeded) std::cerr << "[Launcher] Export navigation validation failed: "
		<< report.value("diagnostic", report.value("failure", std::string("unknown failure"))) << std::endl;
	return succeeded ? 0 : 1;
}

int RunExportLocomotionValidation(const NavigationValidationCommand& command, bool headlessCpuFallback) {
	// This host owns a normal exported Engine lifecycle. The shared validator
	// advances real scheduler frames and only returns a terminal copied report.
	nlohmann::json report = {{"succeeded", false}, {"pending", false}, {"orderlyShutdown", false}};
	Engine validationEngine;
	bool initialized = false;
	try {
		nlohmann::json options = ReadNavigationValidationOptions(command.optionsPath);
		const auto workers = ReadNavigationValidationWorkerCount(options);
		if(!workers) throw std::runtime_error("Locomotion validation requires requiredWorkerCount.");
		const std::string workerText = std::to_string(*workers);
#ifdef _WIN32
		if(_putenv_s("TERRAENGINE_THREADPOOL_WORKERS", workerText.c_str()) != 0)
			throw std::runtime_error("Could not configure locomotion validation workers.");
#else
		if(setenv("TERRAENGINE_THREADPOOL_WORKERS", workerText.c_str(), 1) != 0)
			throw std::runtime_error("Could not configure locomotion validation workers.");
#endif
		options["executionMode"] = "export";
		options["headlessCpuFallback"] = headlessCpuFallback;
		if(headlessCpuFallback) validationEngine.ConfigureHeadlessCpuFallback();
		validationEngine.Initialize();
		initialized = true;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
		do {
			report = TerraEngine::Locomotion::Validation::RunLocomotionApplicationValidation(validationEngine, options);
			if(report.value("pending", false) && std::chrono::steady_clock::now() >= deadline) {
				report["pending"] = false;
				report["succeeded"] = false;
				report["failure"] = "The exported locomotion run exceeded its bounded deadline.";
			}
		} while(report.value("pending", false));
		validationEngine.Shutdown();
		initialized = false;
		report["orderlyShutdown"] = true;
	} catch(const std::exception& exception) {
		if(initialized) {
			try { validationEngine.Shutdown(); report["orderlyShutdown"] = true; }
			catch(...) { report["orderlyShutdown"] = false; }
		} else {
			// A failed pre-initialize path may still have started worker threads.
			ThreadPool::instance().shutdown();
		}
		report["pending"] = false;
		report["succeeded"] = false;
		report["failure"] = std::string("Export locomotion validation failed: ") + exception.what();
	}
	if(!report.contains("actualWorkerCount")) report["actualWorkerCount"] = 0U;
	if(!report.contains("executedCaseCount")) report["executedCaseCount"] = 0U;
	if(!WriteNavigationValidationReport(command.reportPath, report)) {
		std::cerr << "[Launcher] Could not write locomotion validation report: " << command.reportPath << std::endl;
		return 1;
	}
	return report.value("succeeded", false) && report.value("orderlyShutdown", false)
		&& !report.value("pending", false) ? 0 : 1;
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

		if (HasArgument(argc, argv, "--validate-export-navigation-agents")) {
			const std::optional<NavigationValidationCommand> command = FindNavigationValidationCommand(argc, argv);
			if(!command) {
				std::cerr << "[Launcher] --validate-export-navigation-agents requires <options-json> <report-json>." << std::endl;
				return 1;
			}
			// Configure the worker environment and navigation request capacity before
			// Initialize. This runs the installed package through its normal engine
			// lifecycle, never through a direct NavigationSystem test shortcut.
			return RunExportNavigationAgentValidation(*command,
				HasArgument(argc, argv, "--headless-cpu-fallback"));
		}

		if (HasArgument(argc, argv, "--validate-export-locomotion")) {
			const auto command = FindNavigationValidationCommand(argc, argv,
				"--validate-export-locomotion");
			if(!command) {
				std::cerr << "[Launcher] --validate-export-locomotion requires <options-json> <report-json>." << std::endl;
				return 1;
			}
			return RunExportLocomotionValidation(*command,
				HasArgument(argc, argv, "--headless-cpu-fallback"));
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
