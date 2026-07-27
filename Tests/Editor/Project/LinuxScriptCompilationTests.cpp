#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Subsystems/ScriptBuildUtilities.h"

#include <filesystem>
#include <string>

namespace TerraEngine::EditorTests {
	void RegisterLinuxScriptCompilationTests(TestRegistry& registry) {
		registry.Add("Editor.LinuxScriptCompilation.BuildCommandUsesDotnetAndWorkspaceProject", [](TestContext& context) {
			const std::filesystem::path projectRoot = std::filesystem::path("/tmp") / "Terra Project";
			const std::filesystem::path projectPath = projectRoot / "Assembly-CSharp.csproj";

			const TerraEngine::Editor::Scripting::ScriptBuildInvocation invocation =
				TerraEngine::Editor::Scripting::CreateScriptBuildInvocation(projectRoot, projectPath, "Debug");

			context.ExpectEqual(projectRoot, invocation.projectRoot, "Script builds should retain the canonical project root.");
			context.ExpectEqual(projectRoot, invocation.processWorkingDirectory, "Direct script builds should run from the project root.");
			context.Expect(invocation.commandLine.find("dotnet build") != std::string::npos, "Script builds should invoke dotnet build.");
			context.Expect(invocation.commandLine.find("\"/tmp/Terra Project/Assembly-CSharp.csproj\"") != std::string::npos, "Project paths with spaces should be quoted.");
			context.Expect(invocation.commandLine.find("-c Debug") != std::string::npos, "The active script build profile should be passed to dotnet.");
			context.Expect(invocation.commandLine.find("-p:GenerateFullPaths=true") != std::string::npos, "Compiler diagnostics should include full paths.");
			context.Expect(invocation.commandLine.find("-p:UseSharedCompilation=false") != std::string::npos, "Script builds should not start a persistent Roslyn compiler server while output is captured.");
			context.Expect(invocation.commandLine.find("--disable-build-servers") != std::string::npos, "Script builds should disable persistent .NET build servers so captured pipes close promptly.");
		});

		registry.Add("Editor.LinuxScriptCompilation.DiagnosticsNameCommandDirectoryExitCodeAndOutput", [](TestContext& context) {
			const std::filesystem::path projectRoot = std::filesystem::path("/tmp") / "Terra Project";
			const std::filesystem::path projectPath = projectRoot / "Assembly-CSharp.csproj";
			const TerraEngine::Editor::Scripting::ScriptBuildInvocation invocation =
				TerraEngine::Editor::Scripting::CreateScriptBuildInvocation(projectRoot, projectPath, "Release");

			const std::string failure = TerraEngine::Editor::Scripting::DescribeScriptBuildExitFailure(invocation, 42, "CS1002: ; expected");

			context.Expect(failure.find("exit code 42") != std::string::npos, "Exit-code diagnostics should name the failing process status.");
			context.Expect(failure.find(invocation.commandLine) != std::string::npos, "Exit-code diagnostics should include the exact dotnet command.");
			context.Expect(failure.find(projectRoot.string()) != std::string::npos, "Exit-code diagnostics should include the working directory.");
			context.Expect(failure.find("CS1002") != std::string::npos, "Exit-code diagnostics should preserve captured compiler output.");
		});

		registry.Add("Editor.LinuxScriptCompilation.ShortProcessDirectoryBuildsThroughAliasAndRetainsCanonicalState", [](TestContext& context) {
			const std::filesystem::path projectRoot = std::filesystem::path("C:/Projects") / "Deep Terra Project";
			const std::filesystem::path projectPath = projectRoot / "Assembly-CSharp.csproj";
			const std::filesystem::path processWorkingDirectory = std::filesystem::path("C:/Temp") / "TEAOT-private";
			const std::filesystem::path buildProjectPath = processWorkingDirectory / "Assembly-CSharp.csproj";
			const TerraEngine::Editor::Scripting::ScriptBuildInvocation invocation =
				TerraEngine::Editor::Scripting::CreateScriptBuildInvocation(
					projectRoot,
					projectPath,
					"Release",
					processWorkingDirectory,
					buildProjectPath);

			context.ExpectEqual(projectRoot, invocation.projectRoot, "A short process alias must not replace the canonical project root snapshot.");
			context.ExpectEqual(projectPath, invocation.projectPath, "The build request must retain the canonical Assembly-CSharp project identity.");
			context.ExpectEqual(processWorkingDirectory, invocation.processWorkingDirectory, "The build request should accept a private short process directory.");
			context.ExpectEqual(buildProjectPath, invocation.buildProjectPath, "Nested MSBuild tools must receive the short alias project path.");
			context.Expect(invocation.commandLine.find(TerraEngine::Editor::Scripting::QuoteDotNetCommandArgument(buildProjectPath)) != std::string::npos, "The dotnet command must build through the short project alias so csc.exe does not recover the deep canonical working directory.");
			context.Expect(invocation.commandLine.find(TerraEngine::Editor::Scripting::QuoteDotNetCommandArgument(projectPath)) == std::string::npos, "The deep canonical project argument must not be passed back to MSBuild.");

			const std::string failure = TerraEngine::Editor::Scripting::DescribeScriptBuildLaunchFailure(
				invocation,
				"CreateProcessW failed with Windows error 267: The directory name is invalid.");
			context.Expect(failure.find(projectRoot.string()) != std::string::npos, "Launch diagnostics should retain the canonical project root.");
			context.Expect(failure.find(processWorkingDirectory.string()) != std::string::npos, "Launch diagnostics should identify the exact process working directory.");
			context.Expect(failure.find("Windows error 267") != std::string::npos, "Launch diagnostics should preserve the native process-launch error.");
		});

		registry.Add("Editor.LinuxScriptCompilation.MissingOutputDiagnosticUsesManagedAssemblyLanguage", [](TestContext& context) {
			const std::filesystem::path assemblyPath = std::filesystem::path("/tmp") / "Project" / "Build" / "Assembly-CSharp" / "Assembly-CSharp.dll";
			const std::string failure = TerraEngine::Editor::Scripting::DescribeMissingScriptAssembly(assemblyPath);

			context.Expect(failure.find("managed Assembly-CSharp output") != std::string::npos, "Missing-output diagnostics should distinguish live managed DLLs from exported NativeAOT payloads.");
			context.Expect(failure.find("Assembly-CSharp.dll") != std::string::npos, "Live script compilation should still expect the managed DLL output.");
		});
	}
}
