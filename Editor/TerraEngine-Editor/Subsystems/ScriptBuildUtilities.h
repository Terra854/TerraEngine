// ScriptBuildUtilities.h — Shared script build command helpers
//
// Provides small, deterministic helpers for building and reporting the
// Assembly-CSharp dotnet command. Keeping this logic header-only lets the editor
// runtime and unit tests verify the same command text without linking the full
// editor subsystem into the test binary.

#pragma once

#include <filesystem>
#include <sstream>
#include <string>
#include <utility>

namespace TerraEngine::Editor::Scripting
{
	struct ScriptBuildInvocation
	{
		// The project root is used as the process working directory so relative
		// MSBuild paths in the generated Assembly-CSharp project resolve exactly
		// as they do when a developer runs dotnet manually.
		std::filesystem::path workingDirectory;
		std::filesystem::path projectPath;
		std::string profileName;
		std::string commandLine;
	};

	inline std::string QuoteDotNetCommandArgument(const std::filesystem::path& path)
	{
		std::string text = path.string();
		std::string quoted = "\"";
		for (char value : text)
		{
			// Escape embedded quotes so paths from user-created projects cannot
			// terminate the command argument early.
			if (value == '"')
			{
				quoted += "\\\"";
			}
			else
			{
				quoted += value;
			}
		}
		quoted += "\"";
		return quoted;
	}

	inline ScriptBuildInvocation CreateScriptBuildInvocation(const std::filesystem::path& workingDirectory, const std::filesystem::path& projectPath, std::string profileName)
	{
		ScriptBuildInvocation invocation;
		invocation.workingDirectory = workingDirectory;
		invocation.projectPath = projectPath;
		invocation.profileName = std::move(profileName);
		// The editor captures stdout/stderr through a pipe. Persistent .NET build
		// servers can inherit that pipe and keep it open after dotnet exits,
		// leaving the background reader stuck waiting for EOF on Linux. Disable
		// those servers for interactive script builds so project-open cannot hang.
		invocation.commandLine = "dotnet build " + QuoteDotNetCommandArgument(projectPath)
			+ " -c " + invocation.profileName
			+ " -nologo -p:GenerateFullPaths=true -p:UseSharedCompilation=false --disable-build-servers";
		return invocation;
	}

	inline std::string DescribeScriptBuildLaunchFailure(const ScriptBuildInvocation& invocation)
	{
		std::ostringstream stream;
		stream << "Failed to start dotnet build for Assembly-CSharp. Command: " << invocation.commandLine
			<< ". Working directory: " << invocation.workingDirectory.string() << ".";
		return stream.str();
	}

	inline std::string DescribeScriptBuildExitFailure(const ScriptBuildInvocation& invocation, int exitCode, const std::string& capturedOutput)
	{
		std::ostringstream stream;
		stream << "dotnet build failed for Assembly-CSharp with exit code " << exitCode
			<< ". Command: " << invocation.commandLine
			<< ". Working directory: " << invocation.workingDirectory.string() << ".";
		if (!capturedOutput.empty())
		{
			stream << " Captured output:\n" << capturedOutput;
		}
		return stream.str();
	}

	inline std::string DescribeMissingScriptAssembly(const std::filesystem::path& assemblyPath)
	{
		return "dotnet build completed without producing the managed Assembly-CSharp output at " + assemblyPath.string() + ".";
	}
}
