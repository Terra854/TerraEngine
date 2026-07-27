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
		// Canonical paths remain the editor's durable identity. A Windows build
		// may additionally name the same project through a verified short
		// junction so nested MSBuild tools such as csc.exe also inherit a short
		// working directory.
		std::filesystem::path projectRoot;
		std::filesystem::path processWorkingDirectory;
		std::filesystem::path projectPath;
		std::filesystem::path buildProjectPath;
		std::string profileName;
		std::string commandLine;
	};

	inline std::string QuoteDotNetCommandArgument(const std::filesystem::path& path)
	{
		// Use generic separators so command text remains stable when tests model
		// Linux project paths on a Windows host. dotnet accepts forward slashes on
		// Windows, and Linux/macOS already use this representation naturally.
		std::string text = path.generic_string();
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

	inline ScriptBuildInvocation CreateScriptBuildInvocation(
		const std::filesystem::path& projectRoot,
		const std::filesystem::path& projectPath,
		std::string profileName,
		const std::filesystem::path& processWorkingDirectory = {},
		const std::filesystem::path& buildProjectPath = {})
	{
		ScriptBuildInvocation invocation;
		invocation.projectRoot = projectRoot;
		invocation.processWorkingDirectory = processWorkingDirectory.empty()
			? projectRoot
			: processWorkingDirectory;
		invocation.projectPath = projectPath;
		invocation.buildProjectPath = buildProjectPath.empty()
			? projectPath
			: buildProjectPath;
		invocation.profileName = std::move(profileName);
		// The editor captures stdout/stderr through a pipe. Persistent .NET build
		// servers can inherit that pipe and keep it open after dotnet exits,
		// leaving the background reader stuck waiting for EOF on Linux. Disable
		// those servers for interactive script builds so project-open cannot hang.
		invocation.commandLine = "dotnet build " + QuoteDotNetCommandArgument(invocation.buildProjectPath)
			+ " -c " + invocation.profileName
			+ " -nologo -p:GenerateFullPaths=true -p:UseSharedCompilation=false --disable-build-servers";
		return invocation;
	}

	inline std::string DescribeScriptBuildLaunchFailure(
		const ScriptBuildInvocation& invocation,
		const std::string& processError = {})
	{
		std::ostringstream stream;
		stream << "Failed to start dotnet build for Assembly-CSharp. Command: " << invocation.commandLine
			<< ". Project root: " << invocation.projectRoot.string()
			<< ". Process working directory: " << invocation.processWorkingDirectory.string() << ".";
		if (!processError.empty())
		{
			stream << " Process launch details: " << processError;
		}
		return stream.str();
	}

	inline std::string DescribeScriptBuildExitFailure(const ScriptBuildInvocation& invocation, int exitCode, const std::string& capturedOutput)
	{
		std::ostringstream stream;
		stream << "dotnet build failed for Assembly-CSharp with exit code " << exitCode
			<< ". Command: " << invocation.commandLine
			<< ". Project root: " << invocation.projectRoot.string()
			<< ". Process working directory: " << invocation.processWorkingDirectory.string() << ".";
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
