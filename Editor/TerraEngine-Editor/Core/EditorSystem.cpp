// EditorSystem.cpp — Main editor system implementation
//
// Implements the editor's core logic, including ImGui initialization, workspace layout management, and event callback chaining. It manages the loading of the managed editor assembly and provides the bridge between native engine events and C# UI windows.

#include "EditorSystem.h"
#include "EditorSchedulerDeclaration.h"
#include "CheckedImGuiVulkanBridge.h"
#include "ProjectHubSubsystem.h"
#include "MainEditorSubsystem.h"
#include "ProjectManager.h"
#include "AssetDatabase.h"
#include "../Subsystems/ScriptCompilationSubsystem.h"
#include "../Automation/EditorMcpBridge.h"
#include "Tests/Support/RendererEfficiency/Editor/EditorRendererEfficiencyValidation.h"
#include "Editor/Export/EditorExportOperationController.h"
#include "../Profiling/EditorProfilerService.h"
#include "../Windows/SceneViewWindow.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RHI/Vulkan/VulkanContext.h"
#include "Engine/Window/WindowSystem.h"
#include "Engine/DotNet/DotNetHost.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Core/Scheduling/EcsQueryTaskGraph.h"
#include "Engine/Profiling/ProfilerScope.h"
#include "Engine/Runtime/ExportStartupCoordinator.h"
#include "Engine/Utilities/PlatformUtils.h"
#include "NavigationApplicationValidation.h"

// ImGui
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <ImGuiFileDialog.h>
#include <GLFW/glfw3.h>
#include <array>
#include <chrono>
#include <utility>
#include <fstream>
#include <functional>
#include <exception>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

// Static storage for callback chaining
static std::unordered_map<GLFWwindow*, EditorSystem*> g_EditorSystems;
static GLFWwindowclosefun g_PrevCloseCallback = nullptr;
static GLFWframebuffersizefun g_PrevResizeCallback = nullptr;
static GLFWmousebuttonfun g_PrevMouseButtonCallback = nullptr;
static GLFWscrollfun g_PrevScrollCallback = nullptr;
static GLFWkeyfun g_PrevKeyCallback = nullptr;
static GLFWcharfun g_PrevCharCallback = nullptr;
static GLFWdropfun g_PrevDropCallback = nullptr;

namespace {
	// Editor profiling deliberately avoids ordinary nested profiler scopes. This
	// stack-owned collector measures fixed regions with the steady clock, then
	// publishes one bounded value batch before ProfilerEditorWorkScope closes.
	// When recording is inactive, publication returns before taking a lock.
	class EditorCpuStageBatch final {
	public:
		template<typename Callback>
		auto Measure(const char* stageName, Callback&& callback)
		{
			const auto start = Clock::now();
			if constexpr(std::is_void_v<std::invoke_result_t<Callback>>)
			{
				std::invoke(std::forward<Callback>(callback));
				Record(stageName, start);
			}
			else
			{
				auto result = std::invoke(std::forward<Callback>(callback));
				Record(stageName, start);
				return result;
			}
		}

		~EditorCpuStageBatch()
		{
			TerraEngine::Profiling::Profiler::Instance().RecordEditorStages(
				std::span<const TerraEngine::Profiling::ProfilerEditorStageTiming>(
					stages.data(), stageCount));
		}

	private:
		using Clock = std::chrono::steady_clock;

		void Record(const char* stageName, Clock::time_point start)
		{
			if(stageCount >= stages.size())
			{
				return;
			}
			const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
				Clock::now() - start).count();
			stages[stageCount++] = {
				stageName,
				static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)),
				1u};
		}

		std::array<TerraEngine::Profiling::ProfilerEditorStageTiming, 16u> stages{};
		std::size_t stageCount = 0u;
	};
}

// Resolve the managed editor directory from the executable first because the
// editor can be launched by Visual Studio, MCP, or scripts with different
// working directories. The installed/debug layout always stages the managed
// runtime beside TerraEngine-Editor.exe.
static std::filesystem::path ResolveManagedEditorDirectory() {
	const std::filesystem::path executableDirectory = TerraEngine::PlatformUtils::GetExecutableDirectory();
	const std::filesystem::path currentDirectory = std::filesystem::current_path();
	const std::filesystem::path candidates[] = {
		executableDirectory / "Managed",
		executableDirectory / "Editor" / "Managed",
		executableDirectory.parent_path() / "Editor" / "Managed",
		currentDirectory / "Managed",
		currentDirectory / "Editor" / "Managed"
	};

	for (const std::filesystem::path& candidate : candidates) {
		// A runtimeconfig beside the editor assembly proves this is a staged
		// publish directory rather than the source project folder.
		if (!candidate.empty() && std::filesystem::exists(candidate / "TerraEngine.Editor.runtimeconfig.json")) {
			return std::filesystem::absolute(candidate);
		}
	}

	TERRAENGINE_LOG(LogLevel::Warning, "[DotNet] Could not find TerraEngine.Editor.runtimeconfig.json beside the executable or current working directory.");
	return std::filesystem::absolute(executableDirectory.empty() ? currentDirectory / "Editor" / "Managed" : executableDirectory / "Managed");
}

void EditorSystem::SetStartupProjectPath(const std::filesystem::path& projectPath) {
	startupProjectPath = projectPath;
}

void EditorSystem::SetPhysicsInvariantOutputPath(const std::filesystem::path& outputPath) {
	physicsInvariantOutputPath = outputPath;
}

void EditorSystem::SetTerrainInvariantOutputPath(const std::filesystem::path& outputPath) {
	terrainInvariantOutputPath = outputPath;
}

void EditorSystem::SetNavigationAgentValidationPaths(const std::filesystem::path& optionsPath,
	const std::filesystem::path& reportPath) {
	navigationAgentValidationOptionsPath = optionsPath;
	navigationAgentValidationReportPath = reportPath;
}

void EditorSystem::SetLocomotionValidationPaths(const std::filesystem::path& optionsPath,
	const std::filesystem::path& reportPath) {
	locomotionValidationOptionsPath = optionsPath;
	locomotionValidationReportPath = reportPath;
}

namespace {
	// Validation reports are written once at process shutdown, after the editor
	// system has observed Engine's ordinary Shutdown lifecycle.
	bool WriteNavigationAgentValidationReport(const std::filesystem::path& path,
		const nlohmann::json& report) {
		std::error_code error;
		if(!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
		if(error) return false;
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if(!output) return false;
		output << report.dump(2);
		output.flush();
		return output.good();
	}

	nlohmann::json NavigationAgentValidationFailure(std::string message) {
		// Preserve the runner's stable scalar report keys even when startup fails
		// before the core validator could observe a workload frame.
		return {{"succeeded", false}, {"pending", false}, {"orderlyShutdown", false},
			{"actualWorkerCount", 0U}, {"executedCaseCount", 0U}, {"failure", std::move(message)}};
	}
}

void EditorSystem::ProcessNavigationAgentValidation(float deltaTime) {
	if(!navigationAgentValidationOptionsPath || !navigationAgentValidationReportPath
		|| navigationAgentValidationTerminalReport.has_value()) return;

	navigationAgentValidationWaitSeconds += std::max(0.0f, deltaTime);
	const bool workspaceReady = context.projectManager && context.projectManager->HasActiveProject()
		&& context.projectManager->GetReadinessState().simulationEssentialReady && mainEditor;
	if(!workspaceReady && navigationAgentValidationWaitSeconds < 120.0f) return;

	if(!navigationAgentValidationOptionsLoaded) {
		try {
			std::ifstream input(*navigationAgentValidationOptionsPath, std::ios::binary);
			if(!input) throw std::runtime_error("Could not open navigation validation options.");
			input >> navigationAgentValidationOptions;
			if(!navigationAgentValidationOptions.is_object())
				throw std::runtime_error("Navigation validation options must be a JSON object.");
			navigationAgentValidationOptionsLoaded = true;
		}
		catch(const std::exception& exception) {
			navigationAgentValidationTerminalReport = NavigationAgentValidationFailure(exception.what());
		}
	}

	if(!navigationAgentValidationTerminalReport && !workspaceReady) {
		navigationAgentValidationTerminalReport = NavigationAgentValidationFailure(
			"Timed out waiting for the editor project and terrain publication.");
	}
	if(!navigationAgentValidationTerminalReport) {
		nlohmann::json report = mainEditor->RunNavigationAgentPlayValidationFrame(navigationAgentValidationOptions);
		// Parse/admission errors return before core session construction. Retain the
		// report schema required by the installed runner in that fail-closed case.
		if(!report.contains("actualWorkerCount")) report["actualWorkerCount"] = 0U;
		if(!report.contains("executedCaseCount")) report["executedCaseCount"] = 0U;
		if(!report.value("pending", false)) navigationAgentValidationTerminalReport = report;
	}
	if(!navigationAgentValidationTerminalReport) return;

	// Stop through the ordinary toolbar lifecycle before asking Engine to leave
	// its loop. The report itself is held until EditorSystem::Shutdown completes.
	navigationAgentValidationRunAttempted = true;
	if(mainEditor) mainEditor->StopPlayMode();
	context.engine->SignalShutdown();
}

void EditorSystem::ProcessLocomotionValidation(float deltaTime) {
	if(!locomotionValidationOptionsPath || !locomotionValidationReportPath
		|| locomotionValidationTerminalReport) return;
	locomotionValidationWaitSeconds += std::max(0.0f, deltaTime);
	const bool ready = context.projectManager && context.projectManager->HasActiveProject()
		&& context.projectManager->GetReadinessState().simulationEssentialReady && mainEditor;
	if(!ready && locomotionValidationWaitSeconds < 120.0f) return;
	if(!locomotionValidationOptionsLoaded) {
		try {
			std::ifstream input(*locomotionValidationOptionsPath, std::ios::binary);
			if(!input) throw std::runtime_error("Could not open locomotion validation options.");
			input >> locomotionValidationOptions;
			if(!locomotionValidationOptions.is_object())
				throw std::runtime_error("Locomotion validation options must be a JSON object.");
			locomotionValidationOptionsLoaded = true;
		} catch(const std::exception& exception) {
			locomotionValidationTerminalReport = NavigationAgentValidationFailure(exception.what());
		}
	}
	if(!locomotionValidationTerminalReport && !ready)
		locomotionValidationTerminalReport = NavigationAgentValidationFailure(
			"Timed out waiting for the locomotion validation project.");
	if(!locomotionValidationTerminalReport) {
		nlohmann::json report = mainEditor->RunLocomotionPlayValidationFrame(locomotionValidationOptions);
		if(!report.value("pending", false)) locomotionValidationTerminalReport = std::move(report);
	}
	if(!locomotionValidationTerminalReport) return;
	// Stop through the same Play lifecycle as the toolbar. Final evidence is
	// persisted only after the engine invokes this system's Shutdown callback.
	if(mainEditor) mainEditor->StopPlayMode();
	context.engine->SignalShutdown();
}

void EditorSystem::ProcessTerrainInvariantValidation(float deltaTime) {
	if(!terrainInvariantOutputPath || terrainInvariantRunAttempted) return;
	terrainInvariantWaitSeconds += std::max(0.0f, deltaTime);
	const bool ready = context.projectManager && context.projectManager->HasActiveProject() && mainEditor;
	if(!ready && terrainInvariantWaitSeconds < 120.0f) return;
	terrainInvariantRunAttempted = true;
	nlohmann::json report;
	if(ready) report = mainEditor->RunTerrainLiveValidationFixture();
	else report = {{"schemaVersion", 1}, {"succeeded", false}, {"failure", "Timed out waiting for the live terrain editor workspace."}};
	try {
		const auto outputPath = std::filesystem::absolute(*terrainInvariantOutputPath);
		if(!outputPath.parent_path().empty()) std::filesystem::create_directories(outputPath.parent_path());
		std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
		if(!output) throw std::runtime_error("Could not open the live terrain invariant output path.");
		const std::string serialized = report.dump(2);
		output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
		output.flush();
		if(!output) throw std::runtime_error("Could not persist the live terrain invariant report.");
		std::cout << "[Editor] Terrain live invariant report: " << serialized << '\n';
		requestedProcessExitCode = report.value("succeeded", false) ? 0 : 1;
	} catch(const std::exception& exception) {
		requestedProcessExitCode = 1;
		TERRAENGINE_LOG(LogLevel::Error, "[EditorSystem] Terrain live validation failed: " << exception.what());
	}
	context.engine->SignalShutdown();
}

// The installed-editor acceptance command shares normal project startup with an
// interactive session. Project startup intentionally builds Assembly-CSharp on
// a worker, so this gate waits for the coordinator to publish and load that DLL
// before it runs the physics fixture and requests orderly engine shutdown.
void EditorSystem::ProcessPhysicsInvariantValidation(float deltaTime) {
	if (!physicsInvariantOutputPath.has_value() || physicsInvariantRunAttempted) {
		return;
	}
	if (deltaTime > 0.0f) {
		physicsInvariantManagedWaitSeconds += deltaTime;
	}

	TerraEngine::Runtime::PhysicsInvariantReport report;
	bool validationReady = false;
	bool runPhysicsFixture = false;
	if (!context.projectManager->HasActiveProject()) {
		report.diagnostic = "The requested startup project could not be opened for editor physics validation.";
		validationReady = true;
	} else if (!mainEditor) {
		report.diagnostic = "The main editor subsystem is unavailable for physics validation.";
		validationReady = true;
	} else {
		const ScriptCompilationStatus& scriptStatus = ScriptCompilationSubsystem::Instance().GetStatus();
		if (scriptStatus.state == ScriptCoordinatorState::Ready && scriptStatus.gameplayAssemblyLoaded) {
			runPhysicsFixture = true;
			validationReady = true;
		} else if (scriptStatus.state == ScriptCoordinatorState::Ready) {
			report.diagnostic = "Assembly-CSharp reported Ready without a loaded gameplay assembly.";
			validationReady = true;
		} else if (scriptStatus.state == ScriptCoordinatorState::Failed
			|| scriptStatus.state == ScriptCoordinatorState::Canceled) {
			report.diagnostic = "Assembly-CSharp did not become ready: " + scriptStatus.lastSummary;
			validationReady = true;
		} else if (physicsInvariantManagedWaitSeconds >= 120.0f) {
			report.diagnostic = "Timed out waiting for Assembly-CSharp managed readiness: " + scriptStatus.lastSummary;
			validationReady = true;
		}
	}
	if (!validationReady) {
		return;
	}

	// Mark the command in progress before the fixture recursively executes engine
	// frames. Those frames include EditorSystem::Update and must not start a
	// second validation run against the same Play Mode state.
	physicsInvariantRunAttempted = true;
	if (runPhysicsFixture) {
		// This calls the same Play Mode lifecycle and PhysicsSystem used by an
		// interactive editor session after managed project readiness is real.
		report = mainEditor->RunPhysicsExportInvariantFixture();
	}
	try {
		const std::filesystem::path outputPath = std::filesystem::absolute(*physicsInvariantOutputPath);
		if (!outputPath.parent_path().empty()) {
			std::filesystem::create_directories(outputPath.parent_path());
		}
		const std::string serialized = TerraEngine::Runtime::SerializePhysicsInvariantReport(report);
		std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
		if (!output) {
			throw std::runtime_error("Could not open the editor physics invariant output path.");
		}
		output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
		output.flush();
		if (!output) {
			throw std::runtime_error("Could not persist the complete editor physics invariant report.");
		}
		std::cout << "[Editor] Physics invariant report: " << serialized << '\n';
		requestedProcessExitCode = report.succeeded && report.orderlyShutdown
			&& report.activePhysicsJobsAfterDrain == 0U && report.queuedPhysicsJobsAfterDrain == 0U ? 0 : 1;
	} catch (const std::exception& exception) {
		requestedProcessExitCode = 1;
		TERRAENGINE_LOG(LogLevel::Error, "[EditorSystem] Physics invariant validation failed: " << exception.what());
	}

	// The engine owns shutdown ordering. Request a normal loop exit only after
	// the evidence file is closed and the managed build is no longer in flight.
	context.engine->SignalShutdown();
}

int EditorSystem::RequestedProcessExitCode() const noexcept {
	return requestedProcessExitCode;
}

// Wraps the OS cursor to the opposite edge while dragging inspector values.
// Input: window handle for size/position queries. Output: cursor warp plus ImGui state sync.
static void WrapCursorForInspectorDrag(GLFWwindow* window) {
	if (!window) {
		return;
	}

	int width = 0;
	int height = 0;
	glfwGetWindowSize(window, &width, &height);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (width <= 4 || height <= 4) {
		return;
	}

	double mouseX = 0.0;
	double mouseY = 0.0;
	glfwGetCursorPos(window, &mouseX, &mouseY);

	const double wrapPadding = 2.0;
	const double maxX = static_cast<double>(width - 1);
	const double maxY = static_cast<double>(height - 1);
	bool wrapped = false;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (mouseX <= wrapPadding) {
		mouseX = maxX - wrapPadding;
		wrapped = true;
	} else if (mouseX >= maxX - wrapPadding) {
		mouseX = wrapPadding;
		wrapped = true;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (mouseY <= wrapPadding) {
		mouseY = maxY - wrapPadding;
		wrapped = true;
	} else if (mouseY >= maxY - wrapPadding) {
		mouseY = wrapPadding;
		wrapped = true;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (wrapped) {
		glfwSetCursorPos(window, mouseX, mouseY);
		ImGuiIO& io = ImGui::GetIO();
		// Keep ImGui deltas small so value drags stay smooth after wrapping.
		io.MousePos = ImVec2(static_cast<float>(mouseX), static_cast<float>(mouseY));
		io.MouseDelta = ImVec2(0.0f, 0.0f);
	}
}

// EditorSystem::Setup is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorSystem::Setup(Engine* enginePtr) {
	if (initialized) return;

	// Setup Context
	context.engine = enginePtr;
	context.projectManager = std::make_shared<ProjectManager>();
	context.editorMcpBridge = std::make_shared<TerraEngine::Editor::Automation::EditorMcpBridge>();
	context.editorMcpBridge->Initialize(&context);
	// AssetDatabase is a singleton today; expose it through EditorContext with a
	// non-owning shared_ptr so automation tools and subsystems use the same live
	// database without changing its lifecycle ownership.
	context.assetDatabase = std::shared_ptr<AssetDatabase>(&AssetDatabase::Instance(), [](AssetDatabase*) {});

	// Create Subsystems
	projectHub = std::make_shared<ProjectHubSubsystem>();
	mainEditor = std::make_shared<MainEditorSubsystem>();

	projectHub->Initialize(context);
	mainEditor->Initialize(context);
	context.editorMcpBridge->SetMainEditor(mainEditor.get());
	context.editorMcpBridge->SetSelectionCallbacks(
		[this]() -> std::optional<entt::entity> {
			return mainEditor ? mainEditor->GetSelectedEntity() : std::nullopt;
		},
		[this](std::optional<entt::entity> entity) {
			if (mainEditor) {
				mainEditor->SetSelectedEntity(entity);
			}
		});
	// MCP startup preferences are applied after editor callbacks are registered
	// so a default-enabled bridge can safely serve selection-aware requests.
	const EditorPreferences& preferences = context.projectManager->GetEditorPreferences();
	const std::string editorMcpWriteModeDefault = preferences.editorMcpWriteModeDefault;
	const bool editorMcpEnabled = preferences.editorMcpEnabled;
	context.editorMcpBridge->SetListenPort(preferences.editorMcpListenPort);
	if (const auto parsedWriteMode = TerraEngine::Editor::Automation::ParseWriteMode(editorMcpWriteModeDefault)) {
		context.editorMcpBridge->SetWriteMode(*parsedWriteMode);
	}
	if (editorMcpEnabled) {
		context.editorMcpBridge->SetEnabled(true);
	}

	// Wire Callbacks
	projectHub->SetOnProjectLoadedCallback([this]() {
		HandleProjectLoaded();
	});
	projectHub->SetOpenPreferencesCallback([this]() {
		// Preferences is editor-wide and remains safe while the project hub is
		// active; lifecycle approvals depend on this access path.
		if (mainEditor) {
			mainEditor->OpenPreferences();
		}
	});

	mainEditor->SetOnProjectClosedCallback([this]() {
		SaveWorkspaceLayout();
		TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(false);
		ScriptingSystem::SetScriptingDiagnosticsRequested(false);

		// Hide all C++ windows
		auto windows = mainEditor->GetWindows();
		for (auto& win : windows) {
			win->SetVisible(false);
		}

		// Hide all C# windows
		HideAllWindows();

		// Clear ImGui state for the next project
		ImGui::LoadIniSettingsFromMemory("");

		AssetDatabase::Instance().Shutdown();
		ScriptingSystem::Instance().ClearGameplayAssembly();
		// Transition logic
	});

	initialized = true;
}

void EditorSystem::ProcessStartupProjectOpen() {
	if (startupProjectOpenAttempted || !startupProjectPath.has_value() || context.projectManager->HasActiveProject()) {
		return;
	}

	startupProjectOpenAttempted = true;
	std::string error;
	const std::filesystem::path projectPath = *startupProjectPath;
	// Process command-line project loading only after editor services are wired
	// so it reaches the same project lifecycle path used by normal hub opens.
	TERRAENGINE_LOG(LogLevel::Info, "[EditorSystem] Opening startup project: " << projectPath.string());
	if (context.projectManager->LoadProject(projectPath, &error)) {
		HandleProjectLoaded();
	} else {
		TERRAENGINE_LOG(LogLevel::Error, "[EditorSystem] Failed to open startup project: " << error);
	}
}

void EditorSystem::HandleProjectLoaded() {
	// Initialize Asset Database with project root
	if (context.projectManager->HasActiveProject()) {
		std::filesystem::path projectRoot = context.projectManager->GetProjectRoot();
		
		// Set Engine ResourceManager's asset root to the build directory so it can find compiled assets
		// We do this BEFORE AssetDatabase::Initialize because that triggers ScanProject/RegisterAssetPath
		ResourceManager::Instance().SetAssetRoot(projectRoot / "Build" / "Assets");
		
		AssetDatabase::Instance().Initialize(projectRoot);
	}
	// Transition logic
	mainEditor->OnProjectOpened();
	pendingLoadLayout = true;
}

// EditorSystem::SaveWorkspaceLayout is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorSystem::SaveWorkspaceLayout() {
	if (!context.projectManager->HasActiveProject()) return;

	std::filesystem::path projectRoot = context.projectManager->GetProjectRoot();
	std::filesystem::path userSettingsDir = projectRoot / "UserSettings";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(userSettingsDir)) {
		std::filesystem::create_directories(userSettingsDir);
	}

	// 1. Save Window Visibility to JSON
	nlohmann::json layout;
	layout["windows"] = nlohmann::json::array();
	
	auto windows = mainEditor->GetWindows();
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& win : windows) {
		nlohmann::json winJson;
		winJson["title"] = win->GetTitle();
		winJson["visible"] = win->IsVisible();
		layout["windows"].push_back(winJson);
	}

	// 1b. Save Managed (C#) Windows
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (managedGetWindows) {
		static nlohmann::json* s_layoutPtr = nullptr;
		s_layoutPtr = &layout;
		managedGetWindows([](const char* title, int visible) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (s_layoutPtr && title) {
				nlohmann::json winJson;
				winJson["title"] = title;
				winJson["visible"] = (visible != 0);
				winJson["managed"] = true;
				(*s_layoutPtr)["windows"].push_back(winJson);
			}
		});
		s_layoutPtr = nullptr;
	}
	if(auto sceneView = mainEditor->GetWindow<SceneViewWindow>())
	{
		// Scene View camera placement is project workspace state. Keeping it beside
		// window visibility avoids modifying the authored scene on editor close.
		layout["sceneViewCamera"] = sceneView->SerializeWorkspaceState();
	}

	std::ofstream file(userSettingsDir / "EditorLayout.terra");
	file << layout.dump(4);

	// 2. ImGui Docking State (.ini)
	// We explicitly call save to ensure it's synced to our project-specific file.
	ImGui::SaveIniSettingsToDisk((userSettingsDir / "EditorLayout.ini").string().c_str());
}

// EditorSystem::LoadWorkspaceLayout is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorSystem::LoadWorkspaceLayout() {
	if (!context.projectManager->HasActiveProject()) return;

	std::filesystem::path projectRoot = context.projectManager->GetProjectRoot();
	std::filesystem::path layoutPath = projectRoot / "UserSettings" / "EditorLayout.terra";
	std::filesystem::path iniPath = projectRoot / "UserSettings" / "EditorLayout.ini";

	// 1. Load ImGui Docking State
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::exists(iniPath)) {
		ImGui::LoadIniSettingsFromDisk(iniPath.string().c_str());
	}

	// 2. Load Window Visibility
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::exists(layoutPath)) {
		try {
			std::ifstream file(layoutPath);
			nlohmann::json layout;
			file >> layout;

			auto windows = mainEditor->GetWindows();
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& winJson : layout["windows"]) {
				std::string title = winJson["title"];
				bool visible = winJson["visible"];
				bool isManaged = winJson.contains("managed") && winJson["managed"].get<bool>();

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (isManaged) {
					if (managedSetWindowVisibility) {
						managedSetWindowVisibility(title.c_str(), visible);
					}
				} else {
					// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
					for (auto& win : windows) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (win->GetTitle() == title) {
							win->SetVisible(visible);
							break;
						}
					}
				}
			}
			if(layout.contains("sceneViewCamera"))
			{
				if(auto sceneView = mainEditor->GetWindow<SceneViewWindow>())
				{
					std::string cameraError;
					if(!sceneView->RestoreWorkspaceState(layout["sceneViewCamera"], cameraError))
					{
						TERRAENGINE_LOG(LogLevel::Warning,
							"Ignored invalid Scene View camera workspace state: " << cameraError);
					}
				}
			}
		} catch (...) {
			TERRAENGINE_LOG(LogLevel::Warning, "Failed to load EditorLayout.terra");
		}
	}
}

// EditorSystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorSystem::Initialize() {
	// An editor restart cannot carry a request or failed native binding from the
	// preceding lifetime into its newly created ImGui context.
	{ std::lock_guard lock(rendererEfficiencyDeviceRecreationMutex); pendingRendererEfficiencyDeviceRecreation.reset(); rendererEfficiencyDeviceRecreationReceipt = {}; }
	rendererEfficiencyBackendUnavailable = false;
	// Initialize ImGui and Backends
	
	// 1. Get Vulkan Context
	auto& renderSystem = RenderSystem::Instance();
	auto* rhi = renderSystem.GetContext();
	auto* vulkan = dynamic_cast<VulkanContext*>(rhi);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!vulkan) {
		// Log Error
		return;
	}
	
	vulkan->EnableEditorMode();
	VulkanContext::EditorHandles handles;
	vulkan->GetEditorHandles(handles);

	// 2. Get Window
	auto& windowSystem = WindowSystem::Instance();
	GLFWwindow* window = static_cast<GLFWwindow*>(windowSystem.GetNativeHandle());

	// Register Drop Callback safely using static map
	editorCallbackWindow = window;
	g_EditorSystems[window] = this;
	
	// 3. Init ImGui
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ownsImGuiContext = true;
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.IniFilename = nullptr; // Disable default auto-save to imgui.ini, we handle it per-project

	ImGui::StyleColorsDark();

	// 4. Init Backends
	// IMPORTANT: InstallCallbacks = false to prevent overwriting Engine's callbacks
	ImGui_ImplGlfw_InitForVulkan(window, false);
	
	// 5. Chain Callbacks
	g_PrevResizeCallback = glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int width, int height) {
		if (g_PrevResizeCallback) g_PrevResizeCallback(w, width, height);
	});

	g_PrevMouseButtonCallback = glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int mods) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (g_PrevMouseButtonCallback) g_PrevMouseButtonCallback(w, button, action, mods);
		ImGui_ImplGlfw_MouseButtonCallback(w, button, action, mods);
	});

	g_PrevScrollCallback = glfwSetScrollCallback(window, [](GLFWwindow* w, double xoffset, double yoffset) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (g_PrevScrollCallback) g_PrevScrollCallback(w, xoffset, yoffset);
		ImGui_ImplGlfw_ScrollCallback(w, xoffset, yoffset);
	});

	g_PrevKeyCallback = glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (g_PrevKeyCallback) g_PrevKeyCallback(w, key, scancode, action, mods);
		ImGui_ImplGlfw_KeyCallback(w, key, scancode, action, mods);
	});

	g_PrevCharCallback = glfwSetCharCallback(window, [](GLFWwindow* w, unsigned int c) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (g_PrevCharCallback) g_PrevCharCallback(w, c);
		ImGui_ImplGlfw_CharCallback(w, c);
	});

	g_PrevCloseCallback = glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (g_EditorSystems.count(w)) {
			EditorSystem* self = g_EditorSystems[w];
			if (self->mainEditor) {
				// Even a clean scene can own pending receipts or cleanup. Window
				// polling captures intent only; protected PostFrame performs teardown.
				glfwSetWindowShouldClose(w, GLFW_FALSE);
				self->nativeCloseRequested = true;
				return;
			}
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (g_PrevCloseCallback) g_PrevCloseCallback(w);
	});
	
	// Restore/Chain Drop Callback
	g_PrevDropCallback = glfwSetDropCallback(window, [](GLFWwindow* w, int count, const char** paths) {
		if (g_EditorSystems.count(w)) {
			EditorSystem* self = g_EditorSystems[w];
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (self && self->context.projectManager->HasActiveProject()) {
				std::filesystem::path assetRoot = AssetDatabase::Instance().GetAssetRoot();
				for (int i = 0; i < count; i++) {
					std::filesystem::path src = paths[i];
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (std::filesystem::exists(src)) {
						try {
							std::filesystem::copy(src, assetRoot / src.filename(), std::filesystem::copy_options::overwrite_existing);
						} catch (...) {}
					}
				}
			}
		}
	});
	
	ownsWindowCallbacks = true;
	InitializeImGuiVulkanBackend(*vulkan);
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (projectHub) projectHub->OnBackendReady();
	if (mainEditor) mainEditor->OnBackendReady();

	// --- Initialize .NET Host and Load Editor Assembly ---
	std::filesystem::path binDir = ResolveManagedEditorDirectory();
	std::filesystem::path dllPath = binDir / "TerraEngine.Editor.dll";

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!DotNetHost::Instance().Initialize(dllPath)) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to initialize .NET Host with assembly: " << dllPath);
	} else {
		managedInitialize = (ManagedInitFn)DotNetHost::Instance().GetManagedFunction(
			dllPath,
			L"TerraEngine.Editor.EditorApplication, TerraEngine.Editor",
			L"Initialize",
			L"TerraEngine.Editor.EditorInitializeDelegate, TerraEngine.Editor"
		);
		managedUpdate = (ManagedUpdateFn)DotNetHost::Instance().GetManagedFunction(
			dllPath,
			L"TerraEngine.Editor.EditorApplication, TerraEngine.Editor",
			L"Update",
			L"TerraEngine.Editor.EditorUpdateDelegate, TerraEngine.Editor"
		);
		managedShutdown = (ManagedShutdownFn)DotNetHost::Instance().GetManagedFunction(
			dllPath,
			L"TerraEngine.Editor.EditorApplication, TerraEngine.Editor",
			L"Shutdown",
			L"TerraEngine.Editor.EditorShutdownDelegate, TerraEngine.Editor"
		);
		managedDrawWindowMenu = (ManagedDrawWindowMenuFn)DotNetHost::Instance().GetManagedFunction(
			dllPath,
			L"TerraEngine.Editor.EditorApplication, TerraEngine.Editor",
			L"DrawWindowMenu",
			L"TerraEngine.Editor.EditorDrawWindowMenuDelegate, TerraEngine.Editor"
		);
		managedGetWindows = (ManagedGetWindowsFn)DotNetHost::Instance().GetManagedFunction(
			dllPath,
			L"TerraEngine.Editor.EditorApplication, TerraEngine.Editor",
			L"GetManagedWindows",
			L"TerraEngine.Editor.EditorGetWindowsDelegate, TerraEngine.Editor"
		);
		managedSetWindowVisibility = (ManagedSetWindowVisibilityFn)DotNetHost::Instance().GetManagedFunction(
			dllPath,
			L"TerraEngine.Editor.EditorApplication, TerraEngine.Editor",
			L"SetWindowVisibility",
			L"TerraEngine.Editor.EditorSetWindowVisibilityDelegate, TerraEngine.Editor"
		);
		managedHideAllWindows = (ManagedHideAllWindowsFn)DotNetHost::Instance().GetManagedFunction(
			dllPath,
			L"TerraEngine.Editor.EditorApplication, TerraEngine.Editor",
			L"HideAllWindows",
			L"TerraEngine.Editor.EditorHideAllWindowsDelegate, TerraEngine.Editor"
		);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (managedInitialize) {
			managedInitialize(ImGui::GetCurrentContext());
		}
	}

	// Command-line project loading should complete during initialization so
	// automation clients, including MCP, observe the requested project before
	// their first status/resource query. The open still runs after renderer,
	// editor, managed, and callback setup, which keeps it aligned with normal
	// project-hub opens without waiting for the first frame update.
	ProcessStartupProjectOpen();
}

// EditorSystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorSystem::Update(float deltaTime) {
	if (!initialized) return;
	if(nativeCloseRequested && mainEditor) {
		// Both callbacks and this consumer run on the protected main thread.
		// No worker touches the intent or releases project/copy owners.
		nativeCloseRequested = false;
		mainEditor->RequestExit();
	}
	// Editor UI runs as a protected PostFrame system, but it must not appear in
	// the CPU Engine sample tree. This outer boundary measures the complete editor
	// interval separately and suppresses every nested editor-only profiler marker.
	TerraEngine::Profiling::ProfilerEditorWorkScope editorProfilerBoundary;
	EditorCpuStageBatch editorStages;

	// Keep this idempotent fallback so future delayed-startup paths still open
	// the requested project before the editor chooses between hub and workspace UI.
	ProcessStartupProjectOpen();
	if (context.editorProfiler)
	{
		// Profiler filesystem workers only enqueue epoch-tagged immutable results.
		// Accept them at this protected boundary before any profiler window can
		// render, so close/switch invalidation and UI publication are atomic.
		context.editorProfiler->Update(deltaTime);
	}
	// Animation authority has already published through the scheduler by the time
	// this protected PostFrame system runs. Advance durable recorder sessions before
	// any editor-only early return so capture never depends on MCP polling, listener
	// enablement, ImGui visibility, or the installed physics-validation path.
	if (context.editorMcpBridge) {
		context.editorMcpBridge->PumpAnimationRecordingsAtProtectedFrame();
		context.editorMcpBridge->PumpExportJobsAtProtectedFrame();
		context.editorMcpBridge->PumpRendererValidationMutationsAtProtectedFrame();
		context.editorMcpBridge->PumpTerrainAutomationJobsAtProtectedFrame();
	}
	// The opt-in installed acceptance mode must observe the prior frame's
	// script-coordinator publication before beginning another ImGui frame.
	ProcessTerrainInvariantValidation(deltaTime);
	if (terrainInvariantRunAttempted) {
		return;
	}
	ProcessPhysicsInvariantValidation(deltaTime);
	if (physicsInvariantRunAttempted) {
		return;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (pendingLoadLayout) {
		LoadWorkspaceLayout();
		pendingLoadLayout = false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (pendingSaveLayout) {
		SaveWorkspaceLayout();
		pendingSaveLayout = false;
	}

	ProcessRendererEfficiencyDeviceRecreation();
	if (rendererEfficiencyBackendUnavailable) return;
	auto& renderSystem = RenderSystem::Instance();
	auto* rhi = renderSystem.GetContext();
	auto* vulkan = dynamic_cast<VulkanContext*>(rhi);
	if (vulkan) {
		VulkanContext::EditorHandles currentHandles;
		vulkan->GetEditorHandles(currentHandles);
		const auto progress = vulkan->GetEditorSubmissionProgress();
		if (progress.presentationGeneration != imguiPresentationGeneration
			&& currentHandles.minImageCount > 0) {
			const auto refreshResult = TerraEngine::Editor::CheckedImGuiVulkanBridge::Execute(
				TerraEngine::Renderer::Vulkan::EditorUiFrameStage::RecordDrawData,
				[&]() { vulkan->RunWithQueueAccess([&]() {
					ImGui_ImplVulkan_SetMinImageCount(currentHandles.minImageCount);
				}); });
			if (!refreshResult.success) {
				TERRAENGINE_LOG(LogLevel::Error, "Dear ImGui swapchain image-count refresh failed: " << refreshResult.nativeResult);
				return;
			}
			imguiPresentationGeneration = progress.presentationGeneration;
		}
		// The ImGui Vulkan backend may submit font/texture uploads from NewFrame.
		// Serialize that hidden queue access with renderer and asset-upload work.
		const auto newFrameResult = editorStages.Measure("Editor.ImGuiBackendNewFrame", [&]() {
			return TerraEngine::Editor::CheckedImGuiVulkanBridge::Execute(
				TerraEngine::Renderer::Vulkan::EditorUiFrameStage::RecordDrawData,
				[&]() { vulkan->RunWithQueueAccess([]() { ImGui_ImplVulkan_NewFrame(); }); });
		});
		if (!newFrameResult.success) {
			TERRAENGINE_LOG(LogLevel::Error, "Dear ImGui Vulkan NewFrame failed: " << newFrameResult.nativeResult);
			return;
		}
	} else {
		editorStages.Measure("Editor.ImGuiBackendNewFrame", []() {
			ImGui_ImplVulkan_NewFrame();
		});
	}
	editorStages.Measure("Editor.ImGuiFrameStart", []() {
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	});
	if(context.editorProfiler)
	{
		// ImGui's GLFW backend has just published the host wall-clock delta for
		// this editor frame. Store one typed scalar snapshot so open-idle profiler
		// overhead can be measured without enabling profiler recording.
		context.editorProfiler->PublishEditorFrameTiming(
			static_cast<std::uint64_t>(std::max(0, ImGui::GetFrameCount())),
			static_cast<double>(ImGui::GetIO().DeltaTime) * 1000.0);
	}
	
	// MCP must keep processing while the project hub is visible so clients can
	// read status and finish retained jobs after a native project close. The
	// bridge prepares immutable requests off-thread and commits editor-owned work
	// only from this protected editor update boundary.
	if (context.editorMcpBridge) {
		TERRAENGINE_PROFILE_SCOPE("EditorSystem: MCP Bridge", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		editorStages.Measure("Editor.McpBridge", [&]() {
			context.editorMcpBridge->Update(deltaTime);
		});
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (context.projectManager->HasActiveProject()) {
		{
			TERRAENGINE_PROFILE_SCOPE("EditorSystem: Asset Database", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			editorStages.Measure("Editor.AssetDatabase", []() {
				AssetDatabase::Instance().Update();
			});
		}
		{
			TERRAENGINE_PROFILE_SCOPE("EditorSystem: Main Editor Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			editorStages.Measure("Editor.MainUpdate", [&]() {
				mainEditor->Update(deltaTime);
			});
		}
		{
			TERRAENGINE_PROFILE_SCOPE("EditorSystem: Main Editor Draw", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			editorStages.Measure("Editor.MainDraw", [&]() {
				mainEditor->DrawUI();
			});
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (mainEditor && mainEditor->IsInspectorValueDragging()) {
			auto& windowSystem = WindowSystem::Instance();
			GLFWwindow* window = static_cast<GLFWwindow*>(windowSystem.GetNativeHandle());
			WrapCursorForInspectorDrag(window);
		}
	} else {
		editorStages.Measure("Editor.ProjectHub", [&]() {
			ScriptingSystem::Instance().UpdateInspectorWatch(std::nullopt, context.scripting.selectionVersion);
			projectHub->Update(deltaTime);
			projectHub->DrawUI();
			if (mainEditor) {
				mainEditor->DrawProjectHubWindows();
			}
		});
	}

	// Navigation acceptance must observe the same completed editor frame as a
	// user would: AssetDatabase reconciliation, MainEditor update, and Game View
	// request publication all happen above. Running the validator here preserves
	// that normal lifecycle and lets the next frame publish a genuine render
	// result before a terminal report requests shutdown.
	ProcessNavigationAgentValidation(deltaTime);
	ProcessLocomotionValidation(deltaTime);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (managedUpdate) {
		TERRAENGINE_PROFILE_SCOPE("EditorSystem: Managed Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		editorStages.Measure("Editor.ManagedUpdate", [&]() {
			managedUpdate(deltaTime);
		});
	}

	if (context.editorMcpBridge) {
		// A transport call can arrive immediately after the first MCP drain. Give
		// it one more protected handoff before potentially long ImGui rendering and
		// Vulkan presentation, while keeping assistant-command dispatch once/frame.
		TERRAENGINE_PROFILE_SCOPE("EditorSystem: MCP Transport Late Drain", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		editorStages.Measure("Editor.McpTransportLateDrain", [&]() {
			context.editorMcpBridge->PumpPendingTransportToolCallsAtProtectedFrame();
		});
	}

	{
		TERRAENGINE_PROFILE_SCOPE("EditorSystem: ImGui Build Draw Data", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		editorStages.Measure("Editor.ImGuiRender", [&]() {
			const auto startedAt = std::chrono::steady_clock::now();
			ImGui::Render();
			if (context.editorProfiler)
			{
				// Keep draw-data construction distinct from the profiler window's
				// own OnGui work and the later Vulkan presentation wait/submit.
				context.editorProfiler->PublishImGuiRenderTiming(
					static_cast<std::uint64_t>(std::max(0, ImGui::GetFrameCount())),
					std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() - startedAt).count());
			}
		});
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vulkan) {
		TERRAENGINE_PROFILE_SCOPE("EditorSystem: Present Editor UI", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const auto presentResult = editorStages.Measure("Editor.PresentUi", [&]() {
			const auto startedAt = std::chrono::steady_clock::now();
			auto result = vulkan->PresentEditor([&](){
			const auto bridgeResult = TerraEngine::Editor::CheckedImGuiVulkanBridge::Execute(
				TerraEngine::Renderer::Vulkan::EditorUiFrameStage::RecordDrawData,
				[&]() {
					// PresentEditor has already waited this frame slot and holds queue
					// access here. Upload hover/tooltip glyphs before beginning the UI
					// command buffer rather than from inside its active render pass.
					if (ImDrawData* drawData = ImGui::GetDrawData(); drawData != nullptr && drawData->Textures != nullptr)
					{
						for (ImTextureData* texture : *drawData->Textures)
						{
							if (texture != nullptr && texture->Status != ImTextureStatus_OK)
							{
								ImGui_ImplVulkan_UpdateTexture(texture);
							}
						}
					}
					// GetTexID becomes valid after the backend publishes newly-created
					// glyph descriptors. Associate those uses with the submission below.
					if (mainEditor) mainEditor->CaptureViewportDrawDataUses();
				});
			if (!bridgeResult.success) {
				throw std::runtime_error("Dear ImGui Vulkan texture preparation failed with result "
					+ std::to_string(bridgeResult.nativeResult));
			}
		}, [&](void* cmd){
			const auto bridgeResult = TerraEngine::Editor::CheckedImGuiVulkanBridge::Execute(
				TerraEngine::Renderer::Vulkan::EditorUiFrameStage::RecordDrawData,
				[&]() {
					// Texture preparation has already completed outside the render pass,
					// so this callback only records descriptor-backed UI draw commands.
					ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);
				});
			if (!bridgeResult.success) {
				throw std::runtime_error("Dear ImGui Vulkan draw recording failed with result "
					+ std::to_string(bridgeResult.nativeResult));
			}
			});
			if (context.editorProfiler)
			{
				// PresentEditor includes the UI fence/acquire, command recording,
				// queue submit, and compositor handoff. Publishing it separately
				// identifies post-OnGui stalls without changing renderer behavior.
				context.editorProfiler->PublishPresentUiTiming(
					static_cast<std::uint64_t>(std::max(0, ImGui::GetFrameCount())),
					std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() - startedAt).count());
			}
			return result;
		});
		if (presentResult.outcome == TerraEngine::Renderer::Vulkan::EditorPresentOutcome::RecoverableFailure) {
			TERRAENGINE_LOG(LogLevel::Error, "Editor UI presentation stopped safely at "
				<< TerraEngine::Renderer::Vulkan::EditorUiFrameStageName(presentResult.stage)
				<< " with result " << presentResult.nativeResult);
		}
		if (mainEditor) {
			if (presentResult.submissionSerial != 0) {
				mainEditor->CommitViewportDrawDataUses(presentResult.submissionSerial);
			} else {
				mainEditor->AbandonViewportDrawDataUses();
			}
			mainEditor->ReleaseCompletedViewportTextures(
				vulkan->GetEditorSubmissionProgress().completedSerial);
		}
	}
}

// EditorSystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorSystem::PrepareShutdown() {
	// Retain project and managed owners until every export/compiler task has
	// observed cancellation and reaped its owned process tree. One failed drain
	// must not skip the remaining independent producer owners.
	std::exception_ptr firstFailure;
	auto settle = [&](auto&& action) {
		try { action(); } catch (...) { if(!firstFailure) firstFailure = std::current_exception(); }
	};
	if (context.exportController) settle([&] { context.exportController->StopAndDrain(); });
	if (context.editorMcpBridge) settle([&] { context.editorMcpBridge->PumpExportJobsAtProtectedFrame(); });
	settle([] { ScriptCompilationSubsystem::Instance().StopAdmissionsAndDrain(); });
	if(firstFailure) std::rethrow_exception(firstFailure);
}

void EditorSystem::Shutdown() {
	{ std::lock_guard lock(rendererEfficiencyDeviceRecreationMutex); pendingRendererEfficiencyDeviceRecreation.reset();
		if (rendererEfficiencyDeviceRecreationReceipt.status == RendererEfficiencyDeviceRecreationStatus::Pending) {
			rendererEfficiencyDeviceRecreationReceipt.status = RendererEfficiencyDeviceRecreationStatus::Rejected;
			rendererEfficiencyDeviceRecreationReceipt.reason = "Editor lifetime ended before device recreation.";
		} }
	// Keep the first error for Engine's lifecycle report, but release every
	// independent acquired owner before rethrowing. Workspace persistence may
	// fail on a read-only/deleted project and must never bypass native cleanup.
	std::exception_ptr firstFailure;
	auto settle = [&](auto&& action) {
		try { action(); } catch (...) { if(!firstFailure) firstFailure = std::current_exception(); }
	};
	settle([&] { PrepareShutdown(); });
	if (ownsImGuiContext && context.projectManager) settle([&] { SaveWorkspaceLayout(); });
	if (managedShutdown) {
		// Retire the callback before invoking foreign code so reentry or another
		// shutdown attempt cannot call this acquired managed owner twice.
		const auto shutdown = std::exchange(managedShutdown, nullptr);
		settle([&] { shutdown(); });
	}
	managedInitialize = nullptr;
	managedUpdate = nullptr;
	managedDrawWindowMenu = nullptr;
	managedGetWindows = nullptr;
	managedSetWindowVisibility = nullptr;
	managedHideAllWindows = nullptr;

	auto& renderSystem = RenderSystem::Instance();
	if (auto* rhi = renderSystem.GetContext()) settle([&] { rhi->WaitIdle(); });
	if (mainEditor) {
		settle([&] {
			const auto recovery = renderSystem.GetLatestRendererRecoverySnapshot();
			mainEditor->ShutdownViewportTextures(
				recovery.state != TerraEngine::Renderer::RendererRecoveryStateKind::Active);
		});
		settle([&] { mainEditor->Shutdown(); });
	}
	// MainEditor normally releases the profiler. Also settle it independently
	// if a preceding subsystem shutdown exited before reaching that owner.
	if(context.editorProfiler) {
		settle([&] { context.editorProfiler->Shutdown(); });
		context.editorProfiler.reset();
	}

	// Restore native callbacks before releasing ImGui; WindowSystem remains
	// alive until Engine finishes reverse-order system teardown. Each callback
	// is independent, so an unexpected error cannot skip the others.
	if (editorCallbackWindow) {
		auto* window = static_cast<GLFWwindow*>(editorCallbackWindow);
		const bool restoreCallbacks = std::exchange(ownsWindowCallbacks, false);
		editorCallbackWindow = nullptr;
		if (restoreCallbacks) {
			settle([&] { glfwSetFramebufferSizeCallback(window, g_PrevResizeCallback); });
			settle([&] { glfwSetMouseButtonCallback(window, g_PrevMouseButtonCallback); });
			settle([&] { glfwSetScrollCallback(window, g_PrevScrollCallback); });
			settle([&] { glfwSetKeyCallback(window, g_PrevKeyCallback); });
			settle([&] { glfwSetCharCallback(window, g_PrevCharCallback); });
			settle([&] { glfwSetWindowCloseCallback(window, g_PrevCloseCallback); });
			settle([&] { glfwSetDropCallback(window, g_PrevDropCallback); });
		}
		g_EditorSystems.erase(window);
	}
	if (ownsImGuiContext) {
		// A backend can allocate its user data before reporting init failure.
		// Its data pointer proves which partial acquisition needs cleanup.
		auto& io = ImGui::GetIO();
		if (io.BackendRendererUserData) settle([] { ImGui_ImplVulkan_Shutdown(); });
		if (io.BackendPlatformUserData) settle([] { ImGui_ImplGlfw_Shutdown(); });
		ownsImGuiContext = false;
		settle([] { ImGui::DestroyContext(); });
	}
	if (projectHub) settle([&] { projectHub->Shutdown(); });
	if (context.editorMcpBridge) settle([&] { context.editorMcpBridge->Shutdown(); });
	settle([] { AssetDatabase::Instance().Shutdown(); });
	context.editorMcpBridge.reset();
	context.assetDatabase.reset();
	context.projectManager.reset();

	initialized = false;
	// A failed release is reported after all stages have been attempted and
	// cannot produce an orderly-shutdown acceptance claim.
	if(firstFailure) std::rethrow_exception(firstFailure);
	if(navigationAgentValidationTerminalReport) navigationAgentValidationShutdownObserved = true;
	if(locomotionValidationTerminalReport) locomotionValidationShutdownObserved = true;
}

int EditorSystem::FinalizeNavigationAgentValidationAfterEngineShutdown() {
	if(!navigationAgentValidationReportPath)
		return requestedProcessExitCode;
	if(!navigationAgentValidationTerminalReport) {
		// Engine::Run can exit after a controlled startup/runtime failure. A CLI
		// request must still leave explicit failed evidence instead of no report.
		navigationAgentValidationTerminalReport = NavigationAgentValidationFailure(
			"The editor exited before navigation validation reached a terminal result.");
	}

	nlohmann::json report = *navigationAgentValidationTerminalReport;
	report["orderlyShutdown"] = navigationAgentValidationShutdownObserved;
	// Preserve the dedicated host capacity and the core-validated (possibly
	// smaller) cohort size in the final installed-editor evidence document.
	report["configuredMaxAdmittedRequests"] = 4096U;
	report["configuredAgentPopulation"] = report.value("population", 0U);
	const bool succeeded = report.value("succeeded", false)
		&& report.value("orderlyShutdown", false)
		&& !report.value("pending", false)
		&& report.contains("actualWorkerCount")
		&& report.contains("executedCaseCount");
	try {
		if(!WriteNavigationAgentValidationReport(*navigationAgentValidationReportPath, report))
			throw std::runtime_error("Could not persist the navigation agent validation report.");
		std::cout << "[Editor] Navigation agent validation report: " << report.dump() << '\n';
		requestedProcessExitCode = succeeded ? 0 : 1;
	}
	catch(const std::exception& exception) {
		requestedProcessExitCode = 1;
		TERRAENGINE_LOG(LogLevel::Error, "[EditorSystem] Navigation agent validation report failed: " << exception.what());
	}
	return requestedProcessExitCode;
}

int EditorSystem::FinalizeLocomotionValidationAfterEngineShutdown() {
	if(!locomotionValidationReportPath) return requestedProcessExitCode;
	if(!locomotionValidationTerminalReport)
		locomotionValidationTerminalReport = NavigationAgentValidationFailure(
			"The editor exited before locomotion validation reached a terminal result.");
	nlohmann::json report = *locomotionValidationTerminalReport;
	report["orderlyShutdown"] = locomotionValidationShutdownObserved;
	const bool succeeded = report.value("succeeded", false)
		&& report.value("orderlyShutdown", false) && !report.value("pending", false)
		&& report.value("executedCaseCount", 0U) != 0U;
	try {
		if(!WriteNavigationAgentValidationReport(*locomotionValidationReportPath, report))
			throw std::runtime_error("Could not persist the locomotion validation report.");
		requestedProcessExitCode = succeeded ? 0 : 1;
	} catch(const std::exception& exception) {
		requestedProcessExitCode = 1;
		TERRAENGINE_LOG(LogLevel::Error, "[EditorSystem] Locomotion validation report failed: " << exception.what());
	}
	return requestedProcessExitCode;
}

// EditorSystem::SystemName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string EditorSystem::SystemName() {
	return "EditorSystem";
}

TerraEngine::Scheduling::SystemDeclaration EditorSystem::SchedulerDeclaration() {
	return TerraEngine::Editor::MakeEditorSchedulerDeclaration();
}

std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> EditorSystem::DeclareFrameWorkloads(float deltaTime)
{
	using namespace TerraEngine::Scheduling;
	// EditorSystem keeps its historical protected Update workload explicit so
	// adding TerrainAuthoringSystem does not accidentally omit the main editor.
	auto editorUpdate = MakeProtectedCommitPhase("EditorSystemUpdate", SystemName(),
		SchedulerDeclaration().reads, SchedulerDeclaration().writes,
		"Runs the protected editor update and UI lifecycle.", "EditorSystem.Update");
	editorUpdate.stableDescriptorId = "EditorSystem/PostFrame/EditorSystemUpdate";
	editorUpdate.work = [this, deltaTime](JobGraphContext&) { Update(deltaTime); };
	std::vector<FrameWorkloadDeclaration> workloads; workloads.push_back(std::move(editorUpdate));
	if(mainEditor)
	{
		auto terrainWorkloads = mainEditor->DeclareTerrainAuthoringWorkloads(deltaTime);
		workloads.insert(workloads.end(), std::make_move_iterator(terrainWorkloads.begin()), std::make_move_iterator(terrainWorkloads.end()));
	}
	return workloads;
}

// EditorSystem::DrawWindowMenu is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorSystem::DrawWindowMenu() {
	if (managedDrawWindowMenu) {
		managedDrawWindowMenu();
	} else {
		ImGui::TextDisabled("Managed menu hook not loaded");
	}
}

// EditorSystem::HideAllWindows is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorSystem::HideAllWindows() {
	if (managedHideAllWindows) {
		managedHideAllWindows();
	}
}

// EditorSystem::GetManagedWindows is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorSystem::GetManagedWindows(void (*callback)(const char*, int)) {
	if (managedGetWindows) {
		managedGetWindows(callback);
	}
}

bool EditorSystem::QueueRendererEfficiencyDeviceRecreation(const RendererEfficiencyDeviceRecreationRequest& request, std::string& reason)
{
	reason.clear();
#if !defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
	reason = "Device recreation is unavailable outside the validation build."; return false;
#else
	// Ingress copies intent only. Current project/fixture ownership is checked
	// again at execution, so a worker cannot carry permission across project close.
	if (!IsCompleteRendererEfficiencyDeviceRecreationRequest(request))
	{ reason = "Device recreation requires a complete validation owner and backend identity."; return false; }
	std::lock_guard lock(rendererEfficiencyDeviceRecreationMutex);
	if (pendingRendererEfficiencyDeviceRecreation || rendererEfficiencyDeviceRecreationReceipt.status == RendererEfficiencyDeviceRecreationStatus::Pending)
	{ reason = "An owned device recreation is already pending."; return false; }
	if (rendererEfficiencyDeviceRecreationReceipt.request.operationId == request.operationId
		&& rendererEfficiencyDeviceRecreationReceipt.request.fixtureStepId == request.fixtureStepId
		&& rendererEfficiencyDeviceRecreationReceipt.request.expectedRendererSessionGeneration == request.expectedRendererSessionGeneration
		&& rendererEfficiencyDeviceRecreationReceipt.request.expectedBackendResourceGeneration == request.expectedBackendResourceGeneration)
	{ reason = "This operation step already requested device recreation."; return false; }
	pendingRendererEfficiencyDeviceRecreation = request;
	rendererEfficiencyDeviceRecreationReceipt = {}; rendererEfficiencyDeviceRecreationReceipt.request = request;
	rendererEfficiencyDeviceRecreationReceipt.status = RendererEfficiencyDeviceRecreationStatus::Pending;
	return true;
#endif
}

RendererEfficiencyDeviceRecreationReceipt EditorSystem::GetRendererEfficiencyDeviceRecreationReceipt() const
{
	std::lock_guard lock(rendererEfficiencyDeviceRecreationMutex);
	return rendererEfficiencyDeviceRecreationReceipt;
}

void EditorSystem::ProcessRendererEfficiencyDeviceRecreation()
{
	std::optional<RendererEfficiencyDeviceRecreationRequest> request;
	{ std::lock_guard lock(rendererEfficiencyDeviceRecreationMutex); request.swap(pendingRendererEfficiencyDeviceRecreation); }
	if (!request) return;
	RendererEfficiencyDeviceRecreationReceipt receipt; receipt.request = *request;
	receipt.status = RendererEfficiencyDeviceRecreationStatus::Rejected;
	const auto publish = [&] { std::lock_guard lock(rendererEfficiencyDeviceRecreationMutex); rendererEfficiencyDeviceRecreationReceipt = receipt; };
	auto& renderer = RenderSystem::Instance();
	auto* vulkan = dynamic_cast<VulkanContext*>(renderer.GetContext());
	std::string ownershipError;
	const auto intent = renderer.GetRendererEfficiencyFixtureControlSnapshot();
	if (!initialized || !ownsImGuiContext || rendererEfficiencyBackendUnavailable || !vulkan || !context.projectManager || !context.editorMcpBridge
		|| !context.projectManager->HasActiveProject() || intent.correlation.operationId != request->operationId
		|| intent.correlation.fixtureStepId != request->fixtureStepId
		|| !TerraEngine::Editor::Automation::ValidateRendererEfficiencyOwnedProject(context.projectManager->GetProjectRoot(), request->fixtureId, request->fixtureHash, ownershipError)
		|| !context.editorMcpBridge->ValidateRendererEfficiencyDeviceRecreationOwner(request->projectRoot, request->projectGeneration, request->operationId, request->fixtureStepId, ownershipError))
	{ receipt.reason = ownershipError.empty() ? "Current protected fixture ownership is unavailable." : ownershipError; publish(); return; }
	receipt.oldRendererSessionGeneration = vulkan->GetLatestRendererRecoverySnapshot().renderSessionGeneration;
	receipt.oldBackendResourceGeneration = vulkan->GetBackendResourceGeneration();
	if (!MatchesRendererEfficiencyDeviceRecreationBackend(*request, receipt.oldRendererSessionGeneration, receipt.oldBackendResourceGeneration))
	{ receipt.reason = "Device recreation request belongs to an obsolete backend lifetime."; publish(); return; }
	bool backendDetached = false;
	try
	{
		// Device recreation is deliberately sequential and validation-only. Drain
		// admitted resource workers before replacing their native upload owner;
		// regular frames keep their task-based preparation and never perform this wait.
		auto& resources = ResourceManager::Instance();
		struct ReopenResourceRequests {
			ResourceManager& owner; bool reopened = false;
			~ReopenResourceRequests() { if (!reopened) { try { owner.BeginRequestSession(); } catch (...) {} } }
		} reopen{resources};
		resources.StopAdmissionsAndDrain();
		if (!vulkan->PrepareRendererEfficiencyDeviceRecreation())
		{ receipt.reason = "Healthy native GPU idle could not be proven; original UI bindings retained."; publish(); return; }
		receipt.idleProven = true;
		// Mark unavailable before entering a release that could partially complete.
		// After this point no ImGui call may use the original device handles.
		backendDetached = true; rendererEfficiencyBackendUnavailable = true;
		auto* primaryWindow = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle());
		if (!primaryWindow || primaryWindow != editorCallbackWindow || !ImGui::GetIO().BackendPlatformUserData)
			throw std::runtime_error("Original editor platform window is unavailable before device recreation.");
		if (mainEditor) { mainEditor->AbandonViewportDrawDataUses(); mainEditor->ShutdownViewportTextures(false); }
		const auto stopped = TerraEngine::Editor::CheckedImGuiVulkanBridge::Execute(
			TerraEngine::Renderer::Vulkan::EditorUiFrameStage::RecordDrawData,
			[&] { vulkan->RunWithQueueAccess([] { ImGui_ImplVulkan_Shutdown(); }); });
		if (!stopped.success) throw std::runtime_error("Dear ImGui Vulkan release failed before device recreation.");
		// Vulkan shutdown destroys every ImGui platform viewport, including the
		// main viewport's GLFW handle/userdata, while keeping its application
		// window alive. Recreate the platform owner on that same window before
		// any NewFrame can query it. This sequential protected lifecycle keeps
		// frame/event execution outside the partially rebound renderer boundary.
		const auto platformReady = [primaryWindow] {
			const auto* viewport = ImGui::GetMainViewport();
			return ImGui::GetIO().BackendPlatformUserData && viewport
				&& viewport->PlatformUserData && viewport->PlatformHandle == primaryWindow;
		};
		if (!RebindRendererEfficiencyPlatformBackend(primaryWindow,
			[] { ImGui_ImplGlfw_Shutdown(); },
			[primaryWindow] { return ImGui_ImplGlfw_InitForVulkan(primaryWindow, false); }, platformReady))
			throw std::runtime_error("Dear ImGui GLFW platform rebind failed before device recreation.");
		// Fresh Vulkan initialization loads its bundled shaders through the
		// resource manager. Reopen once after all old physical workers are drained;
		// the protected editor owner prevents fixture jobs from entering meanwhile.
		resources.BeginRequestSession(); reopen.reopened = true;
		if (!vulkan->RecreateRendererEfficiencyDevice()) throw std::runtime_error("Native Vulkan device recreation did not complete.");
		vulkan->EnableEditorMode();
		InitializeImGuiVulkanBackend(*vulkan);
		if (projectHub) projectHub->OnBackendReady();
		if (mainEditor) mainEditor->OnBackendReady();
		if (!platformReady() || !ImGui::GetIO().BackendRendererUserData)
			throw std::runtime_error("Recreated editor platform and renderer owners are not ready for NewFrame.");
		receipt.newRendererSessionGeneration = vulkan->GetLatestRendererRecoverySnapshot().renderSessionGeneration;
		receipt.newBackendResourceGeneration = vulkan->GetBackendResourceGeneration();
		if (receipt.newRendererSessionGeneration <= receipt.oldRendererSessionGeneration || receipt.newBackendResourceGeneration <= receipt.oldBackendResourceGeneration)
			throw std::runtime_error("Recreated native backend did not advance its real lifetime identities.");
		receipt.rebound = true; receipt.status = RendererEfficiencyDeviceRecreationStatus::Completed;
		rendererEfficiencyBackendUnavailable = false;
	}
	catch (const std::exception& error) { receipt.status = RendererEfficiencyDeviceRecreationStatus::Unavailable; receipt.reason = error.what(); }
	catch (...) { receipt.status = RendererEfficiencyDeviceRecreationStatus::Unavailable; receipt.reason = "Native device recreation ended exceptionally."; }
	if (backendDetached && !receipt.rebound) rendererEfficiencyBackendUnavailable = true;
	publish();
}

void EditorSystem::InitializeImGuiVulkanBackend(VulkanContext& vulkan)
{
	VulkanContext::EditorHandles handles; vulkan.GetEditorHandles(handles);
	ImGui_ImplVulkan_InitInfo init_info = {};
	init_info.Instance = (VkInstance)handles.instance;
	init_info.PhysicalDevice = (VkPhysicalDevice)handles.physicalDevice;
	init_info.Device = (VkDevice)handles.device;
	init_info.QueueFamily = handles.queueFamily;
	init_info.Queue = (VkQueue)handles.queue;
	init_info.PipelineCache = VK_NULL_HANDLE;
	init_info.DescriptorPool = (VkDescriptorPool)handles.descriptorPool;
	init_info.MinImageCount = handles.minImageCount;
	init_info.ImageCount = handles.imageCount;
	init_info.Allocator = nullptr;
	init_info.CheckVkResultFn = [](VkResult result) {
		TerraEngine::Editor::CheckedImGuiVulkanBridge::CheckVkResult(result);
	};
	init_info.PipelineInfoMain.RenderPass = (VkRenderPass)handles.renderPass;
	init_info.PipelineInfoMain.Subpass = 0;
	init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	// Load Vulkan functions
	std::pair<PFN_vkGetInstanceProcAddr, VkInstance> loaderParams = {
		(PFN_vkGetInstanceProcAddr)handles.vkGetInstanceProcAddr,
		(VkInstance)handles.instance
	};
	const bool functionsLoaded = ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, [](const char* function_name, void* user_data) {
		auto* p = (std::pair<PFN_vkGetInstanceProcAddr, VkInstance>*)user_data;
		return p->first(p->second, function_name);
	}, &loaderParams);
	// A missing dispatch function is a startup failure, not a usable partial
	// backend. Throw while the engine still owns callback/context cleanup.
	if (!functionsLoaded) {
		throw std::runtime_error("Dear ImGui Vulkan function loading failed.");
	}

	const auto backendInit = TerraEngine::Editor::CheckedImGuiVulkanBridge::Execute(
		TerraEngine::Renderer::Vulkan::EditorUiFrameStage::RecordDrawData,
		[&]() { ImGui_ImplVulkan_Init(&init_info); });
	if (!backendInit.success) {
		throw std::runtime_error("Dear ImGui Vulkan backend initialization failed with result "
			+ std::to_string(backendInit.nativeResult));
	}
	imguiPresentationGeneration = vulkan.GetEditorSubmissionProgress().presentationGeneration;
}
