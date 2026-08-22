// EditorRuntimeLayout.cpp - Finds the staged editor when a raw build is launched.

#include "EditorRuntimeLayout.h"

#include <array>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace TerraEngine::Editor
{
	namespace
	{
		constexpr std::array<const char*, 4> kRequiredRuntimeFiles{
			"Shaders/Default.mesh.tspf",
			"Shaders/Default.frag.tspf",
			"Managed/TerraEngine.Editor.runtimeconfig.json",
			"Project/Templates/Assembly-CSharp.csproj.in"
		};

		bool HasCompleteEditorRuntime(const std::filesystem::path& candidate)
		{
			if(candidate.empty()) return false;
			std::error_code errorCode;
			for(const char* relativePath : kRequiredRuntimeFiles)
			{
				// Every required artifact must be a regular file. Merely finding the
				// parent directory would still allow a blank renderer or broken SDK host.
				if(!std::filesystem::is_regular_file(candidate / relativePath, errorCode)
					|| errorCode)
				{
					return false;
				}
			}
			return true;
		}

		void AppendCandidate(std::vector<std::filesystem::path>& candidates,
			std::unordered_set<std::string>& seen,
			const std::filesystem::path& candidate)
		{
			if(candidate.empty()) return;
			const std::filesystem::path normalized = candidate.lexically_normal();
			const std::string key = normalized.generic_string();
			if(seen.emplace(key).second) candidates.push_back(normalized);
		}
	}

	EditorRuntimeLayout ResolveEditorRuntimeLayout(
		const std::filesystem::path& executableDirectory,
		const std::filesystem::path& launchDirectory)
	{
		std::vector<std::filesystem::path> candidates;
		std::unordered_set<std::string> seen;
		candidates.reserve(5U);
		seen.reserve(5U);

		// Installed and staged launches resolve immediately. The following build-tree
		// candidates cover single- and multi-config CMake layouts without consulting
		// a globally installed SDK or runtime.
		AppendCandidate(candidates, seen, executableDirectory);
		AppendCandidate(candidates, seen, executableDirectory / "Build" / "Editor");
		AppendCandidate(candidates, seen,
			executableDirectory.parent_path() / "Build" / "Editor");
		AppendCandidate(candidates, seen, launchDirectory);
		AppendCandidate(candidates, seen, launchDirectory / "Build" / "Editor");

		for(const std::filesystem::path& candidate : candidates)
		{
			if(!HasCompleteEditorRuntime(candidate)) continue;
			return {
				candidate,
				true,
				candidate != executableDirectory.lexically_normal(),
				candidate == executableDirectory.lexically_normal()
					? "Editor runtime is complete beside the executable."
					: "Using the staged self-contained editor runtime at "
						+ candidate.generic_string()
			};
		}

		std::ostringstream diagnostic;
		diagnostic << "No complete editor runtime was found. Build and install the project, then launch Build/Editor/TerraEngine-Editor";
#if defined(_WIN32)
		diagnostic << ".exe";
#endif
		diagnostic << ". Required runtime files:";
		for(const char* relativePath : kRequiredRuntimeFiles)
			diagnostic << ' ' << relativePath;
		return {{}, false, false, diagnostic.str()};
	}
}
