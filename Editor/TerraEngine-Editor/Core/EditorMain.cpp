// EditorMain.cpp — Native editor entry point
//
// Contains the main function for the native editor executable, responsible for initializing the engine and handing off control to the EditorSystem. it establishes the primary loop and ensures the editor mode is correctly registered with the rendering backend.

#include <iostream>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "Engine/Core/MainEngine.h"
#include "EditorCommandLine.h"
#include "EditorRuntimeLayout.h"
#include "EditorStartupRendererConfiguration.h"
#include "EditorSystem.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Navigation/NavigationSettings.h"
#include "Engine/Navigation/NavigationSystem.h"
#include "Engine/Utilities/PlatformUtils.h"

namespace
{
	void MakeCommandLinePathAbsolute(std::optional<std::filesystem::path>& path,
		const std::filesystem::path& launchDirectory)
	{
		// Runtime redirection changes current_path below. Capture relative user input
		// against the original shell directory so project and evidence paths retain
		// exactly the same meaning after the staged runtime becomes active.
		if(path && path->is_relative())
			*path = (launchDirectory / *path).lexically_normal();
	}

	void MakeCommandLinePathAbsolute(std::filesystem::path& path,
		const std::filesystem::path& launchDirectory)
	{
		if(path.is_relative()) path = (launchDirectory / path).lexically_normal();
	}

	// Managed live probes use only two fixed host presets. Keeping this parsing
	// here prevents a validation JSON file from becoming a general gameplay
	// NavigationSettings override or a new managed-facing configuration surface.
	enum class ManagedOutcomeProbe : std::uint8_t
	{
		None,
		Partial,
		Budget
	};

	ManagedOutcomeProbe ReadManagedOutcomeProbe(const nlohmann::json& options)
	{
		const auto found = options.find("managedOutcomeProbe");
		if(found == options.end()) return ManagedOutcomeProbe::None;
		if(!found->is_string())
			throw std::runtime_error("Editor navigation validation managedOutcomeProbe must be 'partial' or 'budget'.");
		const std::string& value = found->get_ref<const std::string&>();
		if(value == "partial") return ManagedOutcomeProbe::Partial;
		if(value == "budget") return ManagedOutcomeProbe::Budget;
		throw std::runtime_error("Editor navigation validation managedOutcomeProbe must be 'partial' or 'budget'.");
	}

	[[nodiscard]] ManagedOutcomeProbe ConfigureNavigationValidationBeforeEngineInitialization(
		const std::optional<EditorCommandLineOptions::NavigationAgentValidationRequest>& request)
	{
		if(!request) return ManagedOutcomeProbe::None;
		std::ifstream input(request->optionsPath, std::ios::binary);
		if(!input) throw std::runtime_error("Could not open editor navigation validation options: " + request->optionsPath.string());
		nlohmann::json options;
		input >> options;
		if(!options.is_object()) throw std::runtime_error("Editor navigation validation options must be a JSON object.");
		// The validation core checks this exact requirement after ThreadPool has
		// initialized. The host must configure the same single public option first.
		const auto found = options.find("requiredWorkerCount");
		if(found != options.end())
		{
			if(!found->is_number_unsigned() || found->get<std::uint64_t>() == 0U
				|| found->get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
				throw std::runtime_error("Editor navigation validation requiredWorkerCount must be a positive unsigned 32-bit integer.");
			const std::string value = std::to_string(found->get<std::uint64_t>());
#ifdef _WIN32
			if(_putenv_s("TERRAENGINE_THREADPOOL_WORKERS", value.c_str()) != 0)
				throw std::runtime_error("Could not configure TERRAENGINE_THREADPOOL_WORKERS for editor navigation validation.");
#else
			if(setenv("TERRAENGINE_THREADPOOL_WORKERS", value.c_str(), 1) != 0)
				throw std::runtime_error("Could not configure TERRAENGINE_THREADPOOL_WORKERS for editor navigation validation.");
#endif
		}

		// This opt-in acceptance process is the only host that raises request
		// capacity for the documented 1,000-agent cohort. Normal editor projects
		// keep NavigationSettings defaults and their normal fair reservations.
		TerraEngine::Navigation::NavigationSettings settings = TerraEngine::Navigation::NavigationSettings::Defaults();
		settings.maxAdmittedRequests = 4096U;
		const ManagedOutcomeProbe managedProbe = ReadManagedOutcomeProbe(options);
		if(managedProbe == ManagedOutcomeProbe::Budget) {
			// This cap exists only for the disposable C# BudgetExhausted fixture. It
			// is selected before NavigationSystem::Initialize and never persists to
			// the project, editor preferences, or managed navigation facade.
			settings.maxRoutePoints = 1U;
		}
		if(!TerraEngine::Navigation::NavigationSystem::Instance().ConfigureSettings(settings))
			throw std::runtime_error("Could not configure navigation request capacity before editor initialization.");
		if(managedProbe != ManagedOutcomeProbe::None) {
			const char* probeName = managedProbe == ManagedOutcomeProbe::Budget ? "budget" : "partial";
			std::cout << "[Editor navigation validation] Interactive MCP verification probe '" << probeName
				<< "' selected; maxRoutePoints=" << settings.maxRoutePoints
				<< ". No automated validation report is generated; inspect NAVMANAGED logs through MCP.\n";
		}
		return managedProbe;
	}
}

int main(int argc, char** argv) {
	EditorCommandLineOptions commandLine = ParseEditorCommandLine(argc, argv);
	if(commandLine.navigationAgentValidationArgumentInvalid)
	{
		std::cerr << "[Editor navigation validation] --validate-editor-navigation-agents requires <options-json> <report-json>.\n";
		return 2;
	}
	const std::filesystem::path launchDirectory = std::filesystem::current_path();
	MakeCommandLinePathAbsolute(commandLine.startupProjectPath, launchDirectory);
	MakeCommandLinePathAbsolute(commandLine.physicsInvariantOutputPath, launchDirectory);
	MakeCommandLinePathAbsolute(commandLine.terrainInvariantOutputPath, launchDirectory);
	if(commandLine.navigationAgentValidation)
	{
		MakeCommandLinePathAbsolute(commandLine.navigationAgentValidation->optionsPath, launchDirectory);
		MakeCommandLinePathAbsolute(commandLine.navigationAgentValidation->reportPath, launchDirectory);
	}
	ManagedOutcomeProbe managedOutcomeProbe = ManagedOutcomeProbe::None;
	try
	{
		// ThreadPool snapshots its environment during Engine::Initialize, so read
		// the immutable acceptance options before renderer/editor setup begins.
		managedOutcomeProbe = ConfigureNavigationValidationBeforeEngineInitialization(commandLine.navigationAgentValidation);
	}
	catch(const std::exception& exception)
	{
		std::cerr << "[Editor navigation validation] " << exception.what() << '\n';
		return 2;
	}
	const TerraEngine::Editor::EditorRuntimeLayout runtimeLayout =
		TerraEngine::Editor::ResolveEditorRuntimeLayout(
			TerraEngine::PlatformUtils::GetExecutableDirectory(), launchDirectory);
	if(!runtimeLayout.complete)
	{
		std::cerr << "[Editor runtime] " << runtimeLayout.diagnostic << '\n';
		return 2;
	}
	if(runtimeLayout.redirected)
	{
		std::error_code errorCode;
		std::filesystem::current_path(runtimeLayout.runtimeDirectory, errorCode);
		if(errorCode)
		{
			std::cerr << "[Editor runtime] Could not activate staged runtime directory '"
				<< runtimeLayout.runtimeDirectory << "': " << errorCode.message() << '\n';
			return 2;
		}
		std::cerr << "[Editor runtime] " << runtimeLayout.diagnostic << '\n';
	}
	const TerraEngine::Editor::EditorStartupRendererConfiguration startupRenderer =
		TerraEngine::Editor::ResolveEditorStartupRendererConfiguration(commandLine.startupProjectPath);
	ResourceManager::Instance().ConfigureEditorStartupProfile(
		startupRenderer.renderSettings,
		startupRenderer.validationEnabled,
		startupRenderer.debugEnabled,
		startupRenderer.projectRoot,
		startupRenderer.origin == TerraEngine::Editor::StartupRendererOrigin::DirectProject
			? "DirectProject" : "HubDefaults");
	for (const std::string& diagnostic : startupRenderer.diagnostics) {
		std::cerr << "[Editor startup renderer] " << diagnostic << '\n';
	}
	Engine engine;
	
	EditorSystem editorSystem;
	if (commandLine.startupProjectPath.has_value()) {
		editorSystem.SetStartupProjectPath(*commandLine.startupProjectPath);
	}
	if (commandLine.physicsInvariantOutputPath.has_value()) {
		editorSystem.SetPhysicsInvariantOutputPath(*commandLine.physicsInvariantOutputPath);
	}
	if (commandLine.terrainInvariantOutputPath.has_value()) {
		editorSystem.SetTerrainInvariantOutputPath(*commandLine.terrainInvariantOutputPath);
	}
	if(commandLine.navigationAgentValidation && managedOutcomeProbe == ManagedOutcomeProbe::None) {
		editorSystem.SetNavigationAgentValidationPaths(
			commandLine.navigationAgentValidation->optionsPath,
			commandLine.navigationAgentValidation->reportPath);
	}
	// Pass the engine pointer to the editor system so it can access engine state/systems
	editorSystem.Setup(&engine);
	
	// Register the editor system with the engine so the render loop calls EditorSystem::Draw()
	engine.SetEditorMode(&editorSystem);

	engine.Run();

	// The navigation command writes `orderlyShutdown` only after Engine has run
	// EditorSystem::Shutdown; ordinary editor and other validation modes retain
	// their existing exit-code behavior.
	return editorSystem.FinalizeNavigationAgentValidationAfterEngineShutdown();
}
