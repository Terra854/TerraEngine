// EditorMain.cpp — Native editor entry point
//
// Contains the main function for the native editor executable, responsible for initializing the engine and handing off control to the EditorSystem. it establishes the primary loop and ensures the editor mode is correctly registered with the rendering backend.

#include <iostream>
#include <filesystem>
#include <optional>
#include <vector>

#include "Engine/Core/MainEngine.h"
#include "EditorCommandLine.h"
#include "EditorRuntimeLayout.h"
#include "EditorStartupRendererConfiguration.h"
#include "EditorSystem.h"
#include "Engine/Resource/ResourceManager.h"
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
}

int main(int argc, char** argv) {
	EditorCommandLineOptions commandLine = ParseEditorCommandLine(argc, argv);
	const std::filesystem::path launchDirectory = std::filesystem::current_path();
	MakeCommandLinePathAbsolute(commandLine.startupProjectPath, launchDirectory);
	MakeCommandLinePathAbsolute(commandLine.physicsInvariantOutputPath, launchDirectory);
	MakeCommandLinePathAbsolute(commandLine.terrainInvariantOutputPath, launchDirectory);
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
	// Pass the engine pointer to the editor system so it can access engine state/systems
	editorSystem.Setup(&engine);
	
	// Register the editor system with the engine so the render loop calls EditorSystem::Draw()
	engine.SetEditorMode(&editorSystem);

	engine.Run();

	return editorSystem.RequestedProcessExitCode();
}
