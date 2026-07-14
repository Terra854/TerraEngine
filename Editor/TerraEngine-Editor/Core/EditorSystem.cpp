// EditorSystem.cpp — Main editor system implementation
//
// Implements the editor's core logic, including ImGui initialization, workspace layout management, and event callback chaining. It manages the loading of the managed editor assembly and provides the bridge between native engine events and C# UI windows.

#include "EditorSystem.h"
#include "CheckedImGuiVulkanBridge.h"
#include "ProjectHubSubsystem.h"
#include "MainEditorSubsystem.h"
#include "ProjectManager.h"
#include "AssetDatabase.h"
#include "../Automation/EditorMcpBridge.h"
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

// ImGui
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <ImGuiFileDialog.h>
#include <GLFW/glfw3.h>
#include <utility>
#include <fstream>
#include <iostream>
#include <stdexcept>
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
		} catch (...) {
			TERRAENGINE_LOG(LogLevel::Warning, "Failed to load EditorLayout.terra");
		}
	}
}

// EditorSystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorSystem::Initialize() {
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
	g_EditorSystems[window] = this;
	
	// 3. Init ImGui
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
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
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (SceneManager::Instance().IsActiveSceneDirty() || AssetDatabase::Instance().IsCompiling()) {
					glfwSetWindowShouldClose(w, GLFW_FALSE); // Stop immediate closure
					self->mainEditor->RequestExit();
					return;
				}
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
	ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, [](const char* function_name, void* user_data) {
		auto* p = (std::pair<PFN_vkGetInstanceProcAddr, VkInstance>*)user_data;
		return p->first(p->second, function_name);
	}, &loaderParams);
	
	const auto backendInit = TerraEngine::Editor::CheckedImGuiVulkanBridge::Execute(
		TerraEngine::Renderer::Vulkan::EditorUiFrameStage::RecordDrawData,
		[&]() { ImGui_ImplVulkan_Init(&init_info); });
	if (!backendInit.success) {
		throw std::runtime_error("Dear ImGui Vulkan backend initialization failed with result "
			+ std::to_string(backendInit.nativeResult));
	}
	imguiPresentationGeneration = vulkan->GetEditorSubmissionProgress().presentationGeneration;
	
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

	if (physicsInvariantOutputPath.has_value() && !physicsInvariantRunAttempted) {
		physicsInvariantRunAttempted = true;
		TerraEngine::Runtime::PhysicsInvariantReport report;
		try {
			if (!context.projectManager->HasActiveProject()) {
				report.diagnostic = "The requested startup project could not be opened for editor physics validation.";
			} else if (!mainEditor) {
				report.diagnostic = "The main editor subsystem is unavailable for physics validation.";
			} else {
				// This calls the same Play Mode lifecycle and PhysicsSystem used by
				// an interactive editor session after renderer and managed startup.
				report = mainEditor->RunPhysicsExportInvariantFixture();
			}

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
		// The engine owns shutdown ordering, so request a normal loop exit after
		// the evidence file is closed instead of tearing systems down in-place.
		context.engine->SignalShutdown();
	}
}

// EditorSystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorSystem::Update(float deltaTime) {
	if (!initialized) return;
	// Editor UI runs as a protected PostFrame system, but it must not appear in
	// the CPU Engine sample tree. This outer boundary measures the complete editor
	// interval separately and suppresses every nested editor-only profiler marker.
	TerraEngine::Profiling::ProfilerEditorWorkScope editorProfilerBoundary;

	// Keep this idempotent fallback so future delayed-startup paths still open
	// the requested project before the editor chooses between hub and workspace UI.
	ProcessStartupProjectOpen();

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
		const auto newFrameResult = TerraEngine::Editor::CheckedImGuiVulkanBridge::Execute(
			TerraEngine::Renderer::Vulkan::EditorUiFrameStage::RecordDrawData,
			[&]() { vulkan->RunWithQueueAccess([]() { ImGui_ImplVulkan_NewFrame(); }); });
		if (!newFrameResult.success) {
			TERRAENGINE_LOG(LogLevel::Error, "Dear ImGui Vulkan NewFrame failed: " << newFrameResult.nativeResult);
			return;
		}
	} else {
		ImGui_ImplVulkan_NewFrame();
	}
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
	
	// MCP must keep processing while the project hub is visible so clients can
	// read status and finish retained jobs after a native project close. The
	// bridge prepares immutable requests off-thread and commits editor-owned work
	// only from this protected editor update boundary.
	if (context.editorMcpBridge) {
		TERRAENGINE_PROFILE_SCOPE("EditorSystem: MCP Bridge", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		context.editorMcpBridge->Update(deltaTime);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (context.projectManager->HasActiveProject()) {
		{
			TERRAENGINE_PROFILE_SCOPE("EditorSystem: Asset Database", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			AssetDatabase::Instance().Update();
		}
		{
			TERRAENGINE_PROFILE_SCOPE("EditorSystem: Main Editor Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			mainEditor->Update(deltaTime);
		}
		{
			TERRAENGINE_PROFILE_SCOPE("EditorSystem: Main Editor Draw", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			mainEditor->DrawUI();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (mainEditor && mainEditor->IsInspectorValueDragging()) {
			auto& windowSystem = WindowSystem::Instance();
			GLFWwindow* window = static_cast<GLFWwindow*>(windowSystem.GetNativeHandle());
			WrapCursorForInspectorDrag(window);
		}
	} else {
		ScriptingSystem::Instance().UpdateInspectorWatch(std::nullopt, context.scripting.selectionVersion);
		projectHub->Update(deltaTime);
		projectHub->DrawUI();
		if (mainEditor) {
			mainEditor->DrawProjectHubWindows();
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (managedUpdate) {
		TERRAENGINE_PROFILE_SCOPE("EditorSystem: Managed Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		managedUpdate(deltaTime);
	}

	{
		TERRAENGINE_PROFILE_SCOPE("EditorSystem: ImGui Build Draw Data", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		ImGui::Render();
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vulkan) {
		TERRAENGINE_PROFILE_SCOPE("EditorSystem: Present Editor UI", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const auto presentResult = vulkan->PresentEditor([&](){
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
void EditorSystem::Shutdown() {
	SaveWorkspaceLayout();
	if (managedShutdown) {
		managedShutdown();
	}

	auto& renderSystem = RenderSystem::Instance();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto* rhi = renderSystem.GetContext()) {
		rhi->WaitIdle();
	}
	if (mainEditor) {
		const auto recovery = renderSystem.GetLatestRendererRecoverySnapshot();
		mainEditor->ShutdownViewportTextures(
			recovery.state != TerraEngine::Renderer::RendererRecoveryStateKind::Active);
		mainEditor->Shutdown();
	}

	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (projectHub) projectHub->Shutdown();
	if (context.editorMcpBridge) context.editorMcpBridge->Shutdown();
	
	AssetDatabase::Instance().Shutdown();
	context.editorMcpBridge.reset();
	context.assetDatabase.reset();
	context.projectManager.reset();
	
	initialized = false;
}

// EditorSystem::SystemName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string EditorSystem::SystemName() {
	return "EditorSystem";
}

TerraEngine::Scheduling::SystemDeclaration EditorSystem::SchedulerDeclaration() {
	TerraEngine::Scheduling::SystemDeclaration declaration;
	declaration.name = SystemName();
	declaration.phase = "PostFrame";
	declaration.reads = {
		"RendererOutput", "PresentationState", "WindowSnapshot", "InputSnapshot",
		"ProjectState", "EditorState", "ImGuiDrawData", "EditorAutomationQueue",
		"EditorAutomationSession"
	};
	declaration.writes = {
		"EditorUiPresentation", "ViewportTextureRegistry", "EditorSubmissionCompletion",
		"EditorDiagnostics", "EditorAutomationState", "EditorAutomationSession",
		"SceneMutationQueue"
	};
	declaration.mainThread = true;
	declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
	declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::Protected;
	declaration.hardBoundary = true;
	declaration.safeSubwork = "Automation schema validation, immutable request preparation, reference-impact scans, log filtering, and editor subsystem snapshots may run on workers; ImGui context mutation remains protected.";
	declaration.protectedCommit = "Prepared automation request commit at post_editor_automation_flush, SceneMutationQueue publication, project and Play Mode transitions, ImGui frame mutation, checked Vulkan backend recording, descriptor mutation, acquire, submit, present, and retirement publication.";
	declaration.protectedReason = declaration.protectedCommit;
	declaration.profilingLabel = declaration.name;
	declaration.usesDefaultMetadata = false;
	return declaration;
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
