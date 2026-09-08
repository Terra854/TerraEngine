#include "TestHarness.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		constexpr std::string_view kApprovedRecastCommit = "6dc1667f580357e8a2154c28b7867bea7e8ad3a7";
		constexpr std::string_view kApprovedRecastUrl = "https://github.com/recastnavigation/recastnavigation.git";

		struct CommandResult {
			int exitCode = -1;
			std::string output;
		};

		std::filesystem::path SourceRoot() {
			// CMake supplies the checkout root. Reading source-controlled inputs
			// makes this contract independent of installed provider artifacts.
			return std::filesystem::path(TERRAENGINE_SOURCE_DIR);
		}

		std::string ReadTextFile(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			if(!input) {
				throw TestFailure("Could not read required navigation dependency file: " + path.string());
			}

			std::ostringstream text;
			text << input.rdbuf();
			return text.str();
		}

		CommandResult CaptureCommand(const std::string& command) {
			// Git's parent gitlink and submodule checkout identify the exact source
			// CMake will compile. Both must match the approved source commit.
#if defined(_WIN32)
			FILE* pipe = _popen(command.c_str(), "r");
#else
			FILE* pipe = popen(command.c_str(), "r");
#endif
			if(pipe == nullptr) {
				return {};
			}

			CommandResult result;
			std::array<char, 512> buffer{};
			while(std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
				result.output.append(buffer.data());
			}

#if defined(_WIN32)
			result.exitCode = _pclose(pipe);
#else
			result.exitCode = pclose(pipe);
#endif
			return result;
		}

		std::string GitExecutable() {
#if defined(_WIN32)
			// Git for Windows is preferred when a long developer PATH would make
			// cmd.exe truncate the normal `git` lookup during a test run.
			const std::filesystem::path installed = "C:/PROGRA~1/Git/cmd/git.exe";
			if(std::filesystem::is_regular_file(installed)) {
				return installed.string();
			}
#endif
			return "git";
		}

		std::string GitLinkCommand(const std::filesystem::path& sourceRoot) {
			return GitExecutable() + " -C \"" + sourceRoot.string()
				+ "\" submodule status -- External/recastnavigation 2>&1";
		}

		std::string ProviderHeadCommand(const std::filesystem::path& sourceRoot) {
			return GitExecutable() + " -C \"" + (sourceRoot / "External/recastnavigation").string()
				+ "\" rev-parse HEAD 2>&1";
		}

		std::string ProviderStatusCommand(const std::filesystem::path& sourceRoot) {
			return GitExecutable() + " -C \"" + (sourceRoot / "External/recastnavigation").string()
				+ "\" status --porcelain=v1 --untracked-files=all 2>&1";
		}

		bool IsCppSource(const std::filesystem::path& path) {
			const std::string extension = path.extension().string();
			return extension == ".h" || extension == ".hpp" || extension == ".inl"
				|| extension == ".cpp" || extension == ".cc" || extension == ".cxx";
		}

		bool IsPrivateNavigationBackendHeader(
			const std::filesystem::path& path,
			const std::filesystem::path& sourceRoot) {
			const std::string normalized = path.lexically_normal().generic_string();
			const std::string privateRoot = (sourceRoot / "Engine/Navigation/Backend").lexically_normal().generic_string() + "/";
			return normalized.starts_with(privateRoot);
		}

		std::string FindPublicProviderExposure(const std::filesystem::path& sourceRoot) {
			const std::filesystem::path engineRoot = sourceRoot / "Engine";
			for(const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(
				engineRoot, std::filesystem::directory_options::skip_permission_denied)) {
				if(!entry.is_regular_file() || !IsCppSource(entry.path())) {
					continue;
				}

				const std::string extension = entry.path().extension().string();
				if((extension != ".h" && extension != ".hpp" && extension != ".inl")
					|| IsPrivateNavigationBackendHeader(entry.path(), sourceRoot)) {
					continue;
				}

				const std::string text = ReadTextFile(entry.path());
				if(text.find("#include <Detour") != std::string::npos
					|| text.find("#include \"Detour") != std::string::npos
					|| text.find("#include <Recast") != std::string::npos
					|| text.find("#include \"Recast") != std::string::npos
					|| text.find("dtNavMesh") != std::string::npos
					|| text.find("dtPolyRef") != std::string::npos
					|| text.find("rcContext") != std::string::npos) {
					return entry.path().lexically_relative(sourceRoot).generic_string();
				}
			}
			return {};
		}

		std::string FindProviderPrivatePool(const std::filesystem::path& sourceRoot) {
			const std::filesystem::path navigationRoot = sourceRoot / "Engine/Navigation";
			if(!std::filesystem::exists(navigationRoot)) {
				return {};
			}

			const std::regex directThreadConstruction(
				R"(std\s*::\s*(?:jthread|thread)\s*(?:[A-Za-z_]\w*\s*)?[\(\{])");
			for(const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(
				navigationRoot, std::filesystem::directory_options::skip_permission_denied)) {
				if(!entry.is_regular_file() || !IsCppSource(entry.path())) {
					continue;
				}

				const std::string text = ReadTextFile(entry.path());
				if(text.find("dtNavMeshQuery") != std::string::npos && std::regex_search(text, directThreadConstruction)) {
					return entry.path().lexically_relative(sourceRoot).generic_string();
				}
			}
			return {};
		}

		void ExpectCoreSourceList(
			TestContext& context,
			const std::string& cmakeText,
			std::string_view sourceDirectory,
			const std::vector<std::string_view>& expectedNames) {
			for(const std::string_view name : expectedNames) {
				const std::string requiredPath = "${_terraengine_recast_root}/" + std::string(sourceDirectory)
					+ "/" + std::string(name);
				context.Expect(cmakeText.find(requiredPath) != std::string::npos,
					"The core-only wrapper must compile approved source " + requiredPath + ".");
			}
		}
	}

	void RegisterNavigationDependencyContractTests(TestRegistry& registry) {
		registry.Add("Engine.Navigation.DependencyContract.ApprovedPristineSourcePin", [](TestContext& context) {
			const std::filesystem::path sourceRoot = SourceRoot();
			const std::string modules = ReadTextFile(sourceRoot / ".gitmodules");
			const std::size_t sectionBegin = modules.find("[submodule \"External/recastnavigation\"]");
			context.Expect(sectionBegin != std::string::npos,
				".gitmodules must declare the approved External/recastnavigation submodule.");

			const std::size_t nextSection = modules.find("[submodule ", sectionBegin + 1U);
			const std::string section = modules.substr(sectionBegin, nextSection - sectionBegin);
			context.Expect(section.find("path = External/recastnavigation") != std::string::npos,
				"The Recast submodule must use the approved External/recastnavigation path.");
			context.Expect(section.find("url = " + std::string(kApprovedRecastUrl)) != std::string::npos,
				"The Recast submodule must use the approved upstream Git repository.");

			const CommandResult gitlink = CaptureCommand(GitLinkCommand(sourceRoot));
			context.ExpectEqual(0, gitlink.exitCode,
				"Git must inspect the Recast parent gitlink used by the working tree.");
			context.Expect(gitlink.output.find(kApprovedRecastCommit) != std::string::npos,
				"The working-tree Recast gitlink must select the approved v1.6.0 source commit.");

			const CommandResult providerHead = CaptureCommand(ProviderHeadCommand(sourceRoot));
			context.ExpectEqual(0, providerHead.exitCode,
				"Git must inspect the Recast checkout CMake will compile.");
			context.Expect(providerHead.output.find(kApprovedRecastCommit) != std::string::npos,
				"The Recast checkout must match the approved parent gitlink.");

			const CommandResult providerStatus = CaptureCommand(ProviderStatusCommand(sourceRoot));
			context.ExpectEqual(0, providerStatus.exitCode,
				"Git must verify that the Recast provider checkout is pristine.");
			context.Expect(providerStatus.output.empty(),
				"External/recastnavigation must have no modified or untracked provider files: " + providerStatus.output);
		});

		registry.Add("Engine.Navigation.DependencyContract.OnlyApprovedStaticCoreModulesBuild", [](TestContext& context) {
			const std::filesystem::path sourceRoot = SourceRoot();
			const std::string rootCmake = ReadTextFile(sourceRoot / "CMakeLists.txt");
			const std::string dependencies = ReadTextFile(sourceRoot / "cmake/TerraEngineDependencies.cmake");
			const std::string wrapper = ReadTextFile(sourceRoot / "cmake/TerraEngineRecast.cmake");

			context.Expect(dependencies.find("TerraEngineRecast.cmake") != std::string::npos,
				"The dependency entry point must include TerraEngine's Recast wrapper.");
			context.Expect(dependencies.find("terraengine_configure_recast_dependency()") != std::string::npos,
				"The dependency entry point must configure the approved Recast core targets.");
			context.Expect(rootCmake.find("TerraEngineRecastCore") != std::string::npos
				&& rootCmake.find("TerraEngineDetourCore") != std::string::npos,
				"The engine target must consume both private Recast generation and Detour query libraries.");
			context.Expect(std::regex_search(wrapper, std::regex(
				R"(add_library\s*\(\s*TerraEngineRecastCore\s+STATIC)", std::regex_constants::icase)),
				"Recast must be compiled as a TerraEngine-owned static library.");
			context.Expect(std::regex_search(wrapper, std::regex(
				R"(add_library\s*\(\s*TerraEngineDetourCore\s+STATIC)", std::regex_constants::icase)),
				"Detour must be compiled as a TerraEngine-owned static library.");
			context.Expect(wrapper.find("add_subdirectory") == std::string::npos,
				"The wrapper must not invoke the provider top-level CMake project.");

			ExpectCoreSourceList(context, wrapper, "Recast/Source", {
				"Recast.cpp", "RecastAlloc.cpp", "RecastArea.cpp", "RecastAssert.cpp",
				"RecastContour.cpp", "RecastFilter.cpp", "RecastLayers.cpp", "RecastMesh.cpp",
				"RecastMeshDetail.cpp", "RecastRasterization.cpp", "RecastRegion.cpp"
			});
			ExpectCoreSourceList(context, wrapper, "Detour/Source", {
				"DetourAlloc.cpp", "DetourAssert.cpp", "DetourCommon.cpp", "DetourNavMesh.cpp",
				"DetourNavMeshBuilder.cpp", "DetourNavMeshQuery.cpp", "DetourNode.cpp"
			});

			const std::array<std::string_view, 6> excludedDirectories{
				"/RecastDemo/", "/DetourCrowd/", "/DetourTileCache/", "/DebugUtils/",
				"/Tests/", "/Examples/"
			};
			for(const std::string_view directory : excludedDirectories) {
				context.Expect(wrapper.find(directory) == std::string::npos,
					"The Recast wrapper must not compile excluded provider directory " + std::string(directory) + ".");
			}
			context.Expect(wrapper.find("Catch2") == std::string::npos && wrapper.find("SDL") == std::string::npos,
				"The Recast integration must not add Catch2 or SDL.");
		});

		registry.Add("Engine.Navigation.DependencyContract.Cxx20AndSixtyFourBitReferencesAreLocked", [](TestContext& context) {
			const std::filesystem::path sourceRoot = SourceRoot();
			const std::string rootCmake = ReadTextFile(sourceRoot / "CMakeLists.txt");
			const std::string wrapper = ReadTextFile(sourceRoot / "cmake/TerraEngineRecast.cmake");
			const std::string detourHeader = ReadTextFile(sourceRoot / "External/recastnavigation/Detour/Include/DetourNavMesh.h");

			context.Expect(std::regex_search(rootCmake, std::regex(
				R"(set\s*\(\s*CMAKE_CXX_STANDARD\s+20\s*\))", std::regex_constants::icase)),
				"The root project must preserve the C++20 language baseline.");
			context.Expect(wrapper.find("target_compile_features(TerraEngineRecastCore PRIVATE cxx_std_20)") != std::string::npos
				&& wrapper.find("target_compile_features(TerraEngineDetourCore PRIVATE cxx_std_20)") != std::string::npos,
				"Both provider cores must explicitly retain the engine's C++20 compile contract.");
			context.Expect(wrapper.find("target_compile_definitions(TerraEngineDetourCore PUBLIC DT_POLYREF64=1)") != std::string::npos,
				"Detour and its private native adapter must consistently compile with 64-bit polygon references.");
			context.Expect(detourHeader.find("typedef uint64_t dtPolyRef;") != std::string::npos,
				"The approved Detour header must provide the 64-bit dtPolyRef branch selected by DT_POLYREF64.");
		});

		registry.Add("Engine.Navigation.DependencyContract.PublicHeadersRemainProviderNeutral", [](TestContext& context) {
			const std::string exposedHeader = FindPublicProviderExposure(SourceRoot());
			context.Expect(exposedHeader.empty(),
				"Public Engine headers must not expose Recast/Detour includes or types; first violation: " + exposedHeader);
		});

		registry.Add("Engine.Navigation.DependencyContract.NoProviderPrivatePoolOrManagedPackage", [](TestContext& context) {
			const std::filesystem::path sourceRoot = SourceRoot();
			const std::string privatePoolSource = FindProviderPrivatePool(sourceRoot);
			context.Expect(privatePoolSource.empty(),
				"Navigation provider work must use TerraEngine's shared ThreadPool; first private-pool violation: " + privatePoolSource);

			const std::filesystem::path managedProject = sourceRoot / "Engine/Managed/TerraEngine.csproj";
			const std::string managedProjectText = ReadTextFile(managedProject);
			context.Expect(managedProjectText.find("Recast") == std::string::npos
				&& managedProjectText.find("Detour") == std::string::npos,
				"The built-in managed facade must not add a provider-owned navigation package.");
		});
	}
}
