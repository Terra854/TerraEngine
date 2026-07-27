// NativeAotProjectWorkspaceTests.cpp - private workspace ownership regressions.

#include "TestHarness.h"

#include "NativeAotProjectWorkspace.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace TerraEngine::EditorTests {
	namespace {
		class NativeAotWorkspaceFixture final {
		public:
			NativeAotWorkspaceFixture()
			{
				const auto identity = std::chrono::steady_clock::now()
					.time_since_epoch().count();
				root_ = std::filesystem::temp_directory_path()
					/ "TerraEngineEditorTests"
					/ ("NativeAotWorkspace-" + std::to_string(identity));
				while(std::filesystem::absolute(root_).native().size() < 210U) {
					root_ /= "segment-abcdefghijklmnopqrstuvwxyz";
				}
				std::filesystem::create_directories(root_);
				std::ofstream project(root_ / "Assembly-CSharp.csproj");
				project << "<Project Sdk=\"Microsoft.NET.Sdk\" />";
			}

			~NativeAotWorkspaceFixture()
			{
				// root_ is a unique child created by this fixture. Workspace
				// cleanup must already have removed every junction before this
				// authored-project cleanup is allowed to recurse.
				std::error_code ignored;
				std::filesystem::remove_all(root_, ignored);
			}

			[[nodiscard]] const std::filesystem::path& Root() const noexcept
			{
				return root_;
			}

		private:
			std::filesystem::path root_;
		};

		bool IsWorkspaceDirectory(
			const std::filesystem::path& workspaceRoot,
			const std::filesystem::path& directory)
		{
			return std::filesystem::is_directory(directory)
				&& directory.parent_path() == workspaceRoot;
		}
	}

	void RegisterNativeAotProjectWorkspaceTests(TestRegistry& registry)
	{
		registry.Add(
			"Editor.NativeAotProjectWorkspace.ConcurrentOperationsStayPrivate",
			[](TestContext& context) {
				NativeAotWorkspaceFixture fixture;
				const std::filesystem::path sourceRoot =
					std::filesystem::absolute(fixture.Root()).lexically_normal();
				NativeAotProjectWorkspace first(sourceRoot);
				NativeAotProjectWorkspace second(sourceRoot);
				std::string firstError;
				std::string secondError;
				context.Expect(first.Prepare(firstError),
					"The first NativeAOT workspace should prepare: " + firstError);
				context.Expect(second.Prepare(secondError),
					"The concurrent NativeAOT workspace should prepare: " + secondError);

				context.Expect(!first.WorkspaceRoot().empty()
						&& !second.WorkspaceRoot().empty()
						&& first.WorkspaceRoot() != second.WorkspaceRoot(),
					"Concurrent operations must receive unpredictable unique workspace roots.");
				context.Expect(first.IntermediateOutputDirectory()
						!= second.IntermediateOutputDirectory()
						&& first.BinaryOutputDirectory()
							!= second.BinaryOutputDirectory()
						&& first.PublishDirectory() != second.PublishDirectory()
						&& first.PackageCacheDirectory()
							!= second.PackageCacheDirectory()
						&& first.DotnetCliHomeDirectory()
							!= second.DotnetCliHomeDirectory(),
					"Concurrent operations must not share obj, bin, publish, package, or CLI-home state.");

				context.Expect(
					IsWorkspaceDirectory(
						first.WorkspaceRoot(),
						first.IntermediateOutputDirectory())
						&& IsWorkspaceDirectory(
							first.WorkspaceRoot(),
							first.BinaryOutputDirectory())
						&& IsWorkspaceDirectory(
							first.WorkspaceRoot(),
							first.PublishDirectory())
						&& IsWorkspaceDirectory(
							first.WorkspaceRoot(),
							first.PackageCacheDirectory())
						&& IsWorkspaceDirectory(
							first.WorkspaceRoot(),
							first.DotnetCliHomeDirectory()),
					"Every generated NativeAOT directory must be an owned child of the private workspace.");

#ifdef _WIN32
				context.Expect(first.UsesShortAlias()
						&& second.UsesShortAlias()
						&& first.BuildProjectRoot() != second.BuildProjectRoot()
						&& first.BuildProjectRoot().parent_path()
							== first.WorkspaceRoot()
						&& first.BuildProjectRoot().filename() == "p"
						&& first.BuildProjectRoot().native().size()
							< sourceRoot.native().size(),
					"Windows must always provide a unique short project junction, even for concurrent builds.");
#else
				context.Expect(!first.UsesShortAlias()
						&& !second.UsesShortAlias()
						&& first.BuildProjectRoot() == sourceRoot
						&& second.BuildProjectRoot() == sourceRoot,
					"Non-Windows builds should use the authored source path with private generated state.");
#endif

				std::error_code equivalentError;
				context.Expect(
					std::filesystem::equivalent(
						first.BuildProjectRoot() / "Assembly-CSharp.csproj",
						sourceRoot / "Assembly-CSharp.csproj",
						equivalentError)
						&& !equivalentError,
					"The build path must resolve to the exact authored project file.");

				const std::filesystem::path firstWorkspace = first.WorkspaceRoot();
				const std::filesystem::path secondWorkspace = second.WorkspaceRoot();
				context.Expect(first.Cleanup(firstError),
					"Explicit first-workspace cleanup should succeed: " + firstError);
				context.Expect(second.Cleanup(secondError),
					"Explicit second-workspace cleanup should succeed: " + secondError);
				context.Expect(!std::filesystem::exists(firstWorkspace)
						&& !std::filesystem::exists(secondWorkspace)
						&& std::filesystem::is_regular_file(
							sourceRoot / "Assembly-CSharp.csproj"),
					"Explicit cleanup must remove both private workspaces while preserving the source project.");
			});

		registry.Add(
			"Editor.NativeAotProjectWorkspace.DestructorFallsBackToSafeCleanup",
			[](TestContext& context) {
				NativeAotWorkspaceFixture fixture;
				const std::filesystem::path sourceRoot =
					std::filesystem::absolute(fixture.Root()).lexically_normal();
				std::filesystem::path workspaceRoot;
				{
					NativeAotProjectWorkspace workspace(sourceRoot);
					std::string error;
					context.Expect(workspace.Prepare(error),
						"The destructor-fallback workspace should prepare: " + error);
					workspaceRoot = workspace.WorkspaceRoot();
				}
				context.Expect(!std::filesystem::exists(workspaceRoot)
						&& std::filesystem::is_regular_file(
							sourceRoot / "Assembly-CSharp.csproj"),
					"Destructor fallback must remove only private state and preserve authored content.");
			});
	}
}
