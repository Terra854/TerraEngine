// EditorRuntimeLayout.h - Resolves the complete self-contained editor runtime.

#pragma once

#include <filesystem>
#include <string>

namespace TerraEngine::Editor
{
	// Describes the runtime directory selected before any engine systems start.
	// A complete directory owns shaders, managed hosting metadata, and templates.
	struct EditorRuntimeLayout
	{
		std::filesystem::path runtimeDirectory;
		bool complete = false;
		bool redirected = false;
		std::string diagnostic;
	};

	// Resolves the runnable editor directory without changing process state. This
	// keeps startup selection deterministic and makes raw-build fallback testable.
	EditorRuntimeLayout ResolveEditorRuntimeLayout(
		const std::filesystem::path& executableDirectory,
		const std::filesystem::path& launchDirectory);
}
