// MainEditorSubsystem.cpp — Core editor workspace implementation
//
// Implements the primary editor workspace logic, including the main menu bar, toolbars, and global hotkey handling. It manages the lifecycle of all native editor windows and provides the necessary hooks for scene state capture and restoration during play mode.

#include "MainEditorSubsystem.h"
#include "ProjectManager.h"
#include "AssetDatabase.h"
#include "EditorSystem.h"
#include "ScriptCompilationSubsystem.h"
#include "UndoRedoSystem.h"
#include "../Profiling/EditorProfilerService.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RHI/Vulkan/VulkanContext.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/UI/UISystem.h"
#include "Engine/Scripting/ScriptGlue.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>
#include <fstream>

// Include Window Headers
#include "../Windows/HierarchyWindow.h"
#include "../Windows/InspectorWindow.h"
#include "../Windows/ProjectBrowserWindow.h"
#include "../Windows/GameViewWindow.h"
#include "../Windows/SceneViewWindow.h"
#include "../Windows/BuildProfilesWindow.h"
#include "../Windows/ProfilerWindow.h"
#include "../Windows/UIBuilderWindow.h"
#include "../Windows/SkySettingsWindow.h"

namespace
{
	MainEditorSubsystem* g_ActiveMainEditorSubsystem = nullptr;

	// StopPlayModeFromApplicationQuit is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void StopPlayModeFromApplicationQuit()
	{
		if (g_ActiveMainEditorSubsystem != nullptr)
		{
			g_ActiveMainEditorSubsystem->StopPlayMode();
		}
	}
}

// MainEditorSubsystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::Initialize(EditorContext& context) {
	ctx = &context;

	// Set engine root to the executable's directory so default shaders can be found
	// even if the project asset root is changed later.
	ResourceManager::Instance().SetEngineRoot(std::filesystem::current_path());
	
	// Initialize Windows
	context.editorProfiler = std::make_shared<EditorProfilerService>();
	context.editorProfiler->Initialize(context);
	m_Windows.push_back(std::make_shared<HierarchyWindow>(this));
	m_Windows.push_back(std::make_shared<InspectorWindow>(this));
	m_Windows.push_back(std::make_shared<ProjectBrowserWindow>(this));
	m_Windows.push_back(std::make_shared<GameViewWindow>(this));
	m_Windows.push_back(std::make_shared<SceneViewWindow>(this));
	m_Windows.push_back(std::make_shared<BuildProfilesWindow>(this));
	m_Windows.push_back(std::make_shared<ProfilerWindow>(this));
	m_Windows.push_back(std::make_shared<UIBuilderWindow>(this));
	m_Windows.push_back(std::make_shared<SkySettingsWindow>(this));

	UndoRedoSystem::Instance().Initialize(context);
	ScriptCompilationSubsystem::Instance().Initialize(context);

	ObjectManager::instance().SetOnModifiedCallback([]() {
		SceneManager::Instance().ForceMarkActiveSceneDirty();
	});

	ScriptingSystem::Instance().SetRunning(false);
	ClearPlayModeSceneState();
	g_ActiveMainEditorSubsystem = this;
	SetApplicationEditorStopPlayModeCallback(&StopPlayModeFromApplicationQuit);
}

// MainEditorSubsystem::OnProjectOpened is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::OnProjectOpened() {
	if (!ctx || !ctx->projectManager->HasActiveProject()) return;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto browser = GetWindow<ProjectBrowserWindow>()) {
		browser->Reset();
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto buildProfiles = GetWindow<BuildProfilesWindow>()) {
		buildProfiles->OnProjectOpened();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx->editorProfiler) {
		ctx->editorProfiler->OnProjectOpened();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto uiBuilder = GetWindow<UIBuilderWindow>()) {
		// Reset the builder when projects change so hidden internal asset paths
		// never leak across workspaces and the next open starts from local state.
		uiBuilder->ResetForProjectOpen();
	}

	auto projectRoot = ctx->projectManager->GetProjectRoot();
	std::filesystem::path assetRoot = projectRoot / "Assets";
	std::filesystem::path sceneRoot = assetRoot / "Scenes";
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(sceneRoot)) {
		std::filesystem::create_directories(sceneRoot);
	}

	// Set Scene Root to Assets folder so we can load scenes from anywhere inside Assets
	SceneManager::Instance().SetSceneRoot(assetRoot.string());
	ResourceManager::Instance().SetAssetRoot(assetRoot);
	RenderSystem::Instance().LoadSettings();
	
	// Check for last opened scene
	std::filesystem::path lastScenePath = projectRoot / "Library" / "LastScene.txt";
	bool loaded = false;
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::exists(lastScenePath)) {
		std::ifstream in(lastScenePath);
		std::string relPath;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::getline(in, relPath) && !relPath.empty()) {
			// Verify it still exists
			if (std::filesystem::exists(assetRoot / relPath)) {
				OpenScene(relPath);
				if (sceneLoaded) loaded = true;
			}
		}
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!loaded) {
		bool foundAny = false;
		if (std::filesystem::exists(assetRoot)) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot)) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (entry.is_regular_file() && entry.path().extension() == ".tescene") {
					std::error_code ec;
					auto rel = std::filesystem::relative(entry.path(), assetRoot, ec);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (!ec) {
						OpenScene(rel);
						foundAny = true;
						break;
					}
				}
			}
		}
		
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!foundAny) {
			// Maybe create a new one? For now just do nothing or log.
		}
	}

	ScriptCompilationSubsystem::Instance().OnProjectOpened();
}

// MainEditorSubsystem::CapturePlayModeSceneState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::CapturePlayModeSceneState()
{
	m_HasPlayModeSceneSnapshot = false;
	m_PlayModeSceneSnapshotJson.clear();
	m_PlayModeSelectedEntityUuid.reset();
	m_PlayModeSceneWasDirty = SceneManager::Instance().IsActiveSceneDirty();

	nlohmann::json snapshotJson;
	const SceneDocument* activeScene = SceneManager::Instance().GetActiveScene();
	snapshotJson["name"] = activeScene != nullptr ? activeScene->name : "ActiveScene";
	snapshotJson["sceneSettings"] = Serializer::SerializeSceneRenderSettings(SceneManager::Instance().GetActiveSceneRenderSettings());
	snapshotJson["objects"] = nlohmann::json::array();

	// Capture the full live hierarchy and reflected component payloads before
	// play starts so Stop can rebuild the exact authored scene state without
	// touching the on-disk scene file.
	for (Object rootObject : ObjectManager::instance().getRootObjects())
	{
		snapshotJson["objects"].push_back(Serializer::SerializeObject(rootObject));
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (selectedEntity.has_value())
	{
		m_PlayModeSelectedEntityUuid = ObjectManager::instance().getUUID(*selectedEntity);
	}

	m_PlayModeSceneSnapshotJson = snapshotJson.dump();
	m_HasPlayModeSceneSnapshot = true;
}

// MainEditorSubsystem::RestorePlayModeSceneState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::RestorePlayModeSceneState()
{
	if (!m_HasPlayModeSceneSnapshot)
	{
		return;
	}

	nlohmann::json snapshotJson;
	try
	{
		snapshotJson = nlohmann::json::parse(m_PlayModeSceneSnapshotJson);
	}
	catch (const nlohmann::json::parse_error& parseError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to parse the pre-play scene snapshot at byte " << parseError.byte << ": " << parseError.what());
		ClearPlayModeSceneState();
		SetSelectedEntity(std::nullopt);
		return;
	}

	ObjectManager::instance().clear();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (snapshotJson.contains("sceneSettings"))
	{
		SceneManager::Instance().SetActiveSceneRenderSettings(Serializer::DeserializeSceneRenderSettings(snapshotJson["sceneSettings"]));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (snapshotJson.contains("objects") && snapshotJson["objects"].is_array())
	{
		for (const nlohmann::json& objectJson : snapshotJson["objects"])
		{
			Serializer::DeserializeObject(objectJson);
		}
	}

	ScriptingSystem::Instance().RebuildRestoredSceneBindings();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_PlayModeSceneWasDirty)
	{
		SceneManager::Instance().ForceMarkActiveSceneDirty();
	}
	else
	{
		SceneManager::Instance().ClearActiveSceneDirty();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_PlayModeSelectedEntityUuid.has_value())
	{
		if (auto restoredObject = ObjectManager::instance().getObject(*m_PlayModeSelectedEntityUuid))
		{
			SetSelectedEntity(restoredObject->EntityId());
		}
		else
		{
			SetSelectedEntity(std::nullopt);
		}
	}
	else
	{
		SetSelectedEntity(std::nullopt);
	}

	ClearPlayModeSceneState();
}

// MainEditorSubsystem::ClearPlayModeSceneState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::ClearPlayModeSceneState()
{
	m_HasPlayModeSceneSnapshot = false;
	m_PlayModeSceneWasDirty = false;
	m_PlayModeSceneSnapshotJson.clear();
	m_PlayModeSelectedEntityUuid.reset();
}

// MainEditorSubsystem::OpenScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::OpenScene(const std::filesystem::path& path) {
	TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene(path);
		if (sceneId != TerraEngine::EMPTY_UUID) {
			SceneManager::Instance().SetActiveScene(sceneId);
			sceneLoaded = true;
			m_CurrentScenePath = path;
			ClearPlayModeSceneState();
			UndoRedoSystem::Instance().Clear();
	
			// Save to LastScene.txt
	
		if (ctx && ctx->projectManager->HasActiveProject()) {
			auto projectRoot = ctx->projectManager->GetProjectRoot();
			std::filesystem::path libDir = projectRoot / "Library";
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!std::filesystem::exists(libDir)) {
				std::filesystem::create_directories(libDir);
			}
			
			std::ofstream out(libDir / "LastScene.txt");
			// path is typically relative to SceneRoot.
			// If path is absolute, we should make it relative if possible,
			// but SceneManager usually takes relative paths.
			// Assuming path is relative to SceneRoot (Assets/Scenes)
			out << path.generic_string();
		}
	}
}

// MainEditorSubsystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::Shutdown() {
	ClearPlayModeSceneState();
	if (g_ActiveMainEditorSubsystem == this)
	{
		g_ActiveMainEditorSubsystem = nullptr;
	}
	SetApplicationEditorStopPlayModeCallback(nullptr);
	ScriptCompilationSubsystem::Instance().Shutdown();
	UndoRedoSystem::Instance().Shutdown();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx != nullptr && ctx->editorProfiler) {
		ctx->editorProfiler->Shutdown();
		ctx->editorProfiler.reset();
	}
	ctx = nullptr;
	m_Windows.clear();
}

// MainEditorSubsystem::StopPlayMode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::StopPlayMode()
{
	if (!ScriptingSystem::Instance().IsRunning())
	{
		return;
	}

	// Route every stop request through one helper so toolbar clicks and
	// Application.Quit() both restore the same cached pre-play scene snapshot.
	ScriptingSystem::Instance().SetRunning(false);
	ScriptingSystem::Instance().SetPaused(false);
	RestorePlayModeSceneState();
}

// MainEditorSubsystem::OnBackendReady is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::OnBackendReady() {
	if (!ctx || !ctx->engine) return;
	
	RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!renderSystem) return;

	RHIContext* rhi = renderSystem->GetContext();
	VulkanContext* vulkan = dynamic_cast<VulkanContext*>(rhi);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!vulkan) return;

	VulkanContext::EditorHandles handles;
	vulkan->GetEditorHandles(handles);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (handles.sceneImageView && handles.sceneSampler) {
		sceneTextureID = (void*)ImGui_ImplVulkan_AddTexture(
			(VkSampler)handles.sceneSampler,
			(VkImageView)handles.sceneImageView,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		);
		cachedSceneImageView = handles.sceneImageView;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (handles.gameImageView && handles.gameSampler) {
		gameTextureID = (void*)ImGui_ImplVulkan_AddTexture(
			(VkSampler)handles.gameSampler,
			(VkImageView)handles.gameImageView,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		);
		cachedGameImageView = handles.gameImageView;
	}
}

// MainEditorSubsystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::Update(float deltaTime) {
	// Advance the scripting coordinator after asset watcher events have been
	// processed so queued C# rebuilds can start and completed async builds can
	// clear their in-flight state during the same editor frame.
	ScriptCompilationSubsystem::Instance().Update(deltaTime);

	// Ensure UI system updates even when the game is not playing so we get live previews.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ScriptingSystem::Instance().IsRunning())
	{
		UISystem::Instance().Update(deltaTime);
	}

	// A blocking script compile pauses editor-side interactions until the build
	// either reloads the new assembly or restores the previous working one.
	if (ScriptCompilationSubsystem::Instance().ShouldBlockEditorInteraction()) {
		return;
	}

	// Logic update

	if (ctx != nullptr)
	{
		std::optional<uint32_t> watchedObjectId;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (selectedEntity.has_value())
		{
			watchedObjectId = static_cast<uint32_t>(*selectedEntity);
		}

		ctx->scripting.hasWatchedObject = watchedObjectId.has_value();
		ctx->scripting.watchedObjectId = watchedObjectId.value_or(0);
		ScriptingSystem::Instance().UpdateInspectorWatch(watchedObjectId, ctx->scripting.selectionVersion);
	}

	// Process editor-wide hotkeys.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) {
		if (ImGui::IsKeyPressed(ImGuiKey_S)) {
			SceneManager::Instance().SaveScene();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
			UndoRedoSystem::Instance().Undo();
		}
		// Traditional Windows redo shortcut (Ctrl+Y).
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
			UndoRedoSystem::Instance().Redo();
		}
		
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsKeyPressed(ImGuiKey_B)) {
			if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)) {
				if (auto win = GetWindow<BuildProfilesWindow>()) {
					win->SetVisible(true);
				}
			} else {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (auto win = GetWindow<BuildProfilesWindow>()) {
					win->SetVisible(true);
					win->StartExportAndRun();
				}
			}
		}
	}
}

// MainEditorSubsystem::SetOnProjectClosedCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::SetOnProjectClosedCallback(std::function<void()> callback) {
	onProjectClosed = callback;
}

// MainEditorSubsystem::SetSelectedEntity is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::SetSelectedEntity(std::optional<entt::entity> entity)
{
	selectedEntity = entity;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (entity)
	{
		selectedAsset = std::nullopt;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx != nullptr)
	{
		ctx->scripting.selectionVersion++;
		ctx->scripting.hasWatchedObject = entity.has_value();
		ctx->scripting.watchedObjectId = entity.has_value() ? static_cast<std::uint32_t>(*entity) : 0;
	}
}

// MainEditorSubsystem::SetSelectedAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::SetSelectedAsset(std::optional<std::filesystem::path> asset)
{
	selectedAsset = asset;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (asset)
	{
		selectedEntity = std::nullopt;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx != nullptr)
	{
		ctx->scripting.selectionVersion++;
		ctx->scripting.hasWatchedObject = false;
		ctx->scripting.watchedObjectId = 0;
	}
}

// MainEditorSubsystem::IsInspectorValueDragging is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool MainEditorSubsystem::IsInspectorValueDragging() const {
	auto inspector = const_cast<MainEditorSubsystem*>(this)->GetWindow<InspectorWindow>();
	if (inspector) return inspector->IsInspectorValueDragging();
	return false;
}

// MainEditorSubsystem::TryGetGameViewRenderStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool MainEditorSubsystem::TryGetGameViewRenderStats(RenderStatsSnapshot& outStats) const {
	if (!ctx || !ctx->engine) {
		return false;
	}

	RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!renderSystem) {
		return false;
	}

	outStats = renderSystem->GetLatestRenderStats(RenderViewType::Game);
	return outStats.valid;
}

// MainEditorSubsystem::DrawUI is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::DrawUI() {
	// Check for resize/recreation of offscreen resources
	if (ctx && ctx->engine) {
		RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
		if (renderSystem) {
			VulkanContext* vulkan = dynamic_cast<VulkanContext*>(renderSystem->GetContext());
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vulkan) {
				VulkanContext::EditorHandles handles;
				vulkan->GetEditorHandles(handles);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (handles.sceneImageView != cachedSceneImageView) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (handles.sceneImageView && handles.sceneSampler) {
						sceneTextureID = (void*)ImGui_ImplVulkan_AddTexture(
							(VkSampler)handles.sceneSampler,
							(VkImageView)handles.sceneImageView,
							VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
						);
						cachedSceneImageView = handles.sceneImageView;
					}
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (handles.gameImageView != cachedGameImageView) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (handles.gameImageView && handles.gameSampler) {
						gameTextureID = (void*)ImGui_ImplVulkan_AddTexture(
							(VkSampler)handles.gameSampler,
							(VkImageView)handles.gameImageView,
							VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
						);
						cachedGameImageView = handles.gameImageView;
					}
				}
			}
		}
	}

	// Setup Dockspace
	static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;
	ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
	
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->Pos);
	ImGui::SetNextWindowSize(viewport->Size);
	ImGui::SetNextWindowViewport(viewport->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
	window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("TerraEngine DockSpace", nullptr, window_flags);
	ImGui::PopStyleVar();
	ImGui::PopStyleVar(2);

	DrawMainMenuBar();

	// Toolbar
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 2));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0, 0));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		auto& colors = ImGui::GetStyle().Colors;
		const auto& buttonHovered = colors[ImGuiCol_ButtonHovered];
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(buttonHovered.x, buttonHovered.y, buttonHovered.z, 0.5f));
		const auto& buttonActive = colors[ImGuiCol_ButtonActive];
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(buttonActive.x, buttonActive.y, buttonActive.z, 0.5f));

		float size = ImGui::GetWindowHeight() - 4.0f; // Constrain to not take too much space? No, this is the main window.
		// We want a small toolbar strip.
		// Since we are inside the DockSpace window, but before DockSpace node, we are just drawing at the top.
		
		// Let's use a fixed height for the toolbar area to ensure DockSpace doesn't overlap weirdly or just let flow handle it.
		// But we want to center the buttons.
		
		float availWidth = ImGui::GetContentRegionAvail().x;
		// Buttons: Play/Stop, Pause, Step
		float buttonSize = 20.0f; // Approx height
		const char* playLabel = ScriptingSystem::Instance().IsRunning() ? "Stop" : "Play";
		const char* pauseLabel = "Pause";
		const char* stepLabel = "Next";
		
		// Calculate width
		ImVec2 playSize = ImGui::CalcTextSize(playLabel);
		ImVec2 pauseSize = ImGui::CalcTextSize(pauseLabel);
		ImVec2 stepSize = ImGui::CalcTextSize(stepLabel);
		
		float padding = 10.0f;
		float totalWidth = playSize.x + pauseSize.x + stepSize.x + (padding * 2) + (ImGui::GetStyle().ItemSpacing.x * 2);
		
		ImGui::SetCursorPosX((availWidth - totalWidth) * 0.5f);
		
		// Play/Stop
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button(playLabel)) {
			bool running = ScriptingSystem::Instance().IsRunning();
			if (!running) {
				CapturePlayModeSceneState();
				ScriptingSystem::Instance().SetRunning(true);
				ScriptingSystem::Instance().SetPaused(false);
			} else {
				StopPlayMode();
			}
		}
		ImGui::SameLine();
		
		// Pause
		// Highlight if paused
		bool isPaused = ScriptingSystem::Instance().IsPaused();
		if (isPaused) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
		if (ImGui::Button(pauseLabel)) {
			ScriptingSystem::Instance().SetPaused(!isPaused);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (isPaused) ImGui::PopStyleColor();
		ImGui::SameLine();
		
		// Step
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button(stepLabel)) {
			if (ScriptingSystem::Instance().IsRunning() && ScriptingSystem::Instance().IsPaused()) {
				ScriptingSystem::Instance().Step(1);
			}
		}

		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(2);
	}

	ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
	ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& win : m_Windows) {
		win->Draw();
	}

	DrawSaveConfirmationModal();
	DrawScriptCompilationModal();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (showDemoWindow) ImGui::ShowDemoWindow(&showDemoWindow);

	ImGui::End();
}

// MainEditorSubsystem::DrawMainMenuBar is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::DrawMainMenuBar() {
	if (ImGui::BeginMenuBar()) {
		if (ImGui::BeginMenu("File")) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (SceneManager::Instance().SaveScene(m_CurrentScenePath)) {
					UndoRedoSystem::Instance().SetSavePoint();
				}
			}
			ImGui::Separator();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Build Profiles...", "Ctrl+Shift+B")) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (auto win = GetWindow<BuildProfilesWindow>()) {
					win->SetVisible(true);
				}
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Build and Run", "Ctrl+B")) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (auto win = GetWindow<BuildProfilesWindow>()) {
					win->SetVisible(true);
					win->StartExportAndRun();
				}
			}
			ImGui::Separator();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Close Project")) {
				RequestCloseProject();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Exit")) {
				RequestExit();
			}
			ImGui::EndMenu();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginMenu("Edit")) {
			// Context-aware menu enabling ensures users know when history is 
			// available or exhausted.
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Undo", "Ctrl+Z", false, UndoRedoSystem::Instance().CanUndo())) {
				UndoRedoSystem::Instance().Undo();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Redo", "Ctrl+Y", false, UndoRedoSystem::Instance().CanRedo())) {
				UndoRedoSystem::Instance().Redo();
			}
			ImGui::EndMenu();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginMenu("View")) {
			ImGui::MenuItem("ImGui Demo", nullptr, &showDemoWindow);
			ImGui::EndMenu();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginMenu("Scripting")) {
			// Keep script actions reachable from the menu, but leave build status
			// text out of the chrome so attaching scripts stays drag-and-drop only.
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Open Workspace")) {
				ScriptCompilationSubsystem::Instance().OpenWorkspace();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Regenerate Workspace")) {
				ScriptCompilationSubsystem::Instance().RegenerateWorkspace();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Rebuild Scripts")) {
				ScriptCompilationSubsystem::Instance().RequestBuild(ScriptBuildReason::ManualRefresh);
			}
			ImGui::EndMenu();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginMenu("Windows")) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (auto& win : m_Windows) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::MenuItem(win->GetTitle().c_str())) {
					win->SetVisible(true);
				}
			}
			
			ImGui::Separator();

			EditorSystem* editorSystem = nullptr;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ctx && ctx->engine) {
				editorSystem = dynamic_cast<EditorSystem*>(ctx->engine->GetEditorSystem());
			}
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (editorSystem) {
				editorSystem->DrawWindowMenu();
			}
			
			ImGui::EndMenu();
		}
		ImGui::EndMenuBar();
	}
}

// MainEditorSubsystem::RequestOpenScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::RequestOpenScene(const std::filesystem::path& path) {
	if (SceneManager::Instance().IsActiveSceneDirty()) {
		m_PendingAction = PendingAction::OpenScene;
		m_PendingScenePath = path;
		m_ShowSaveConfirmation = true;
	} else {
		OpenScene(path);
	}
}

// MainEditorSubsystem::RequestCloseProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::RequestCloseProject() {
	if (SceneManager::Instance().IsActiveSceneDirty()) {
		m_PendingAction = PendingAction::CloseProject;
		m_ShowSaveConfirmation = true;
	} else {
		CloseCurrentProject();
	}
}

// MainEditorSubsystem::RequestExit is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::RequestExit() {
	if (SceneManager::Instance().IsActiveSceneDirty()) {
		m_PendingAction = PendingAction::Exit;
		m_ShowSaveConfirmation = true;
	} else {
		CloseCurrentProject();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ctx && ctx->engine) ctx->engine->SignalShutdown();
	}
}

// MainEditorSubsystem::ExecutePendingAction is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::ExecutePendingAction() {
	switch (m_PendingAction) {
		case PendingAction::OpenScene:
			OpenScene(m_PendingScenePath);
			break;
		case PendingAction::CloseProject:
			CloseCurrentProject();
			break;
		case PendingAction::Exit:
			CloseCurrentProject();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ctx && ctx->engine) ctx->engine->SignalShutdown();
			break;
		default:
			break;
	}
	m_PendingAction = PendingAction::None;
	m_PendingScenePath.clear();
}

// MainEditorSubsystem::DrawSaveConfirmationModal is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::DrawSaveConfirmationModal() {
	if (m_ShowSaveConfirmation) {
		ImGui::OpenPopup("Save Changes?");
		m_ShowSaveConfirmation = false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginPopupModal("Save Changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("There are unsaved changes to the current scene.\nDo you want to save them?");
		ImGui::Separator();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("Yes", ImVec2(120, 0))) {
			SceneManager::Instance().SaveScene();
			UndoRedoSystem::Instance().SetSavePoint();
			ExecutePendingAction();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SetItemDefaultFocus();
		ImGui::SameLine();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("No", ImVec2(120, 0))) {
			ExecutePendingAction();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			m_PendingAction = PendingAction::None;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

// MainEditorSubsystem::DrawScriptCompilationModal is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::DrawScriptCompilationModal() {
	const ScriptCompilationSubsystem& scriptCompilation = ScriptCompilationSubsystem::Instance();
	const ScriptCompilationStatus& scriptStatus = scriptCompilation.GetStatus();
	const bool isCompilingScripts = scriptCompilation.IsBusy();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (isCompilingScripts) {
		ImGui::OpenPopup("Compiling C# Scripts");
	}

	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginPopupModal("Compiling C# Scripts", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!isCompilingScripts) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		ImGui::TextWrapped("TerraEngine paused gameplay script execution while Assembly-CSharp recompiles.");
		ImGui::Spacing();
		ImGui::TextUnformatted(scriptStatus.lastSummary.c_str());
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.6f, 0.8f, 1.0f));
		const char* progressLabel = scriptStatus.state == ScriptCoordinatorState::Reloading ? "Reloading..." : "Compiling...";
		// Use a negative fraction animated by ImGui time to render an
		// indeterminate progress bar while the build runs in the background.
		ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(360.0f, 0.0f), progressLabel);
		ImGui::PopStyleColor();
		ImGui::EndPopup();
	}
}

// MainEditorSubsystem::CloseCurrentProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::CloseCurrentProject() {
	ScriptingSystem::Instance().SetRunning(false);
	ClearPlayModeSceneState();
	ScriptCompilationSubsystem::Instance().OnProjectClosed();

	// Clear Scene
	SceneManager::Instance().UnloadAll();
	
	// Clear Objects
	ObjectManager::instance().clear();

	// Clear Undo History
	UndoRedoSystem::Instance().Clear();
	
	// Clear Resources
	ResourceManager::Instance().ClearCaches();
	ResourceManager::Instance().LoadDefaultResources();
	
	// Clear Renderer Resources
	// This ensures GPU resources are flushed and binding indices are reset.
	if (ctx && ctx->engine) {
		RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
		if (renderSystem) {
			renderSystem->Clear();
		}
	}

	// Clear Selection
	selectedEntity = std::nullopt;
	selectedAsset = std::nullopt;
	if (ctx != nullptr)
	{
		ctx->scripting.selectionVersion++;
		ctx->scripting.hasWatchedObject = false;
		ctx->scripting.watchedObjectId = 0;
		ScriptingSystem::Instance().UpdateInspectorWatch(std::nullopt, ctx->scripting.selectionVersion);
	}
	
	// Trigger callbacks BEFORE closing the project in ProjectManager
	// so that subsystems can save data to the project directory.
	if (ctx && ctx->editorProfiler) {
		ctx->editorProfiler->OnProjectClosed();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (onProjectClosed) onProjectClosed();

	// Close Project
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx && ctx->projectManager) {
		ctx->projectManager->CloseProject();
	}
}
