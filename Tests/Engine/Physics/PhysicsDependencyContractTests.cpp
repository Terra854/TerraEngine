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
		constexpr std::string_view kApprovedJoltCommit = "e77f175595e64cb44218cc9d9d56fc365ad0e36a";
		constexpr std::string_view kOfficialJoltUrl = "https://github.com/jrouwe/JoltPhysics.git";

		struct CommandResult {
			int exitCode = -1;
			std::string output;
		};

		std::filesystem::path SourceRoot() {
			// CMake supplies the checkout root to the test target. Keeping all
			// dependency checks relative to it makes the suite independent of the
			// directory from which CTest launches the executable.
			return std::filesystem::path(TERRAENGINE_SOURCE_DIR);
		}

		std::string ReadTextFile(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			if(!input) {
				throw TestFailure("Could not read required dependency contract file: " + path.string());
			}

			std::ostringstream text;
			text << input.rdbuf();
			return text.str();
		}

		std::string Lowercase(std::string value) {
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return value;
		}

		CommandResult CaptureCommand(const std::string& command) {
			// The revision contract is recorded by the parent repository's gitlink.
			// Reading that index entry avoids trusting the mutable HEAD inside the
			// checked-out submodule.
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

		std::string GitLinkCommand(const std::filesystem::path& sourceRoot) {
			// CMAKE_SOURCE_DIR is controlled by the local configure step. Quoting it
			// keeps ordinary checkout paths containing spaces safe on both hosts.
			return "git -C \"" + sourceRoot.string() + "\" ls-files --stage -- External/JoltPhysics 2>&1";
		}

		std::string JoltHeadCommand(const std::filesystem::path& sourceRoot) {
			// The parent gitlink is the immutable revision contract, while this
			// second check proves the files CMake will compile are checked out at it.
			return "git -C \"" + (sourceRoot / "External/JoltPhysics").string()
				+ "\" rev-parse HEAD 2>&1";
		}

		std::string JoltStatusCommand(const std::filesystem::path& sourceRoot) {
			// Include untracked files so a locally patched or augmented provider
			// cannot silently pass while differing from the approved pristine tree.
			return "git -C \"" + (sourceRoot / "External/JoltPhysics").string()
				+ "\" status --porcelain=v1 --untracked-files=all 2>&1";
		}

		bool HasCMakeSetting(const std::string& cmakeText, std::string_view name, std::string_view value) {
			// Accept normal CMake spacing while requiring the exact option/value
			// pair. The trailing boundary prevents OFF from matching a longer token.
			const std::regex setting(
				"set\\s*\\(\\s*" + std::string(name) + "\\s+" + std::string(value) + "(?:\\s|\\))",
				std::regex_constants::icase);
			return std::regex_search(cmakeText, setting);
		}

		bool ProfileDisablesOption(const std::string& profile, std::string_view name) {
			if(HasCMakeSetting(profile, name, "OFF")) {
				return true;
			}

			// The production profile may apply one audited OFF value to a named
			// option list. Prove both halves: the requested option is in that list,
			// and the loop force-writes every member to OFF in the CMake cache.
			std::smatch disabledListMatch;
			const std::regex disabledList(
				R"(set\s*\(\s*_terraengine_jolt_disabled_options([\s\S]*?)\))",
				std::regex_constants::icase);
			if(!std::regex_search(profile, disabledListMatch, disabledList)) {
				return false;
			}

			const std::regex optionToken(
				"(?:^|\\s)" + std::string(name) + "(?:\\s|$)",
				std::regex_constants::icase);
			const std::regex forcedOff(
				R"(set\s*\(\s*\$\{_terraengine_jolt_option\}\s+OFF\s+CACHE\s+BOOL)",
				std::regex_constants::icase);
			return std::regex_search(disabledListMatch[1].str(), optionToken)
				&& std::regex_search(profile, forcedOff);
		}

		bool IsProviderPrivateHeader(const std::filesystem::path& path, const std::filesystem::path& sourceRoot) {
			const std::string normalized = path.lexically_normal().generic_string();
			const std::string privateRoot = (sourceRoot / "Engine/Physics/Backend").lexically_normal().generic_string() + "/";
			return normalized.starts_with(privateRoot);
		}

		bool IsCppSource(const std::filesystem::path& path) {
			const std::string extension = Lowercase(path.extension().string());
			return extension == ".h" || extension == ".hpp" || extension == ".inl" || extension == ".cpp" || extension == ".cc";
		}

		std::string FindPublicJoltExposure(const std::filesystem::path& sourceRoot) {
			const std::filesystem::path engineRoot = sourceRoot / "Engine";
			for(const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(
				engineRoot, std::filesystem::directory_options::skip_permission_denied)) {
				if(!entry.is_regular_file()) {
					continue;
				}

				const std::string extension = Lowercase(entry.path().extension().string());
				if((extension != ".h" && extension != ".hpp" && extension != ".inl")
					|| IsProviderPrivateHeader(entry.path(), sourceRoot)) {
					continue;
				}

				const std::string text = ReadTextFile(entry.path());
				if(text.find("#include <Jolt/") != std::string::npos
					|| text.find("#include \"Jolt/") != std::string::npos
					|| text.find("JPH::") != std::string::npos
					|| text.find("namespace JPH") != std::string::npos) {
					return entry.path().lexically_relative(sourceRoot).generic_string();
				}
			}
			return {};
		}

		std::string FindProviderPrivatePool(const std::filesystem::path& sourceRoot) {
			const std::filesystem::path physicsRoot = sourceRoot / "Engine/Physics";
			if(!std::filesystem::exists(physicsRoot)) {
				return {};
			}

			for(const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(
				physicsRoot, std::filesystem::directory_options::skip_permission_denied)) {
				if(!entry.is_regular_file() || !IsCppSource(entry.path())) {
					continue;
				}

				const std::string text = ReadTextFile(entry.path());
				const std::regex directThreadConstruction(
					R"(std\s*::\s*(?:jthread|thread)\s*(?:[A-Za-z_]\w*\s*)?[\(\{])");
				if(text.find("Jolt/Core/JobSystemThreadPool.h") != std::string::npos
					|| text.find("JPH::JobSystemThreadPool") != std::string::npos
					|| std::regex_search(text, directThreadConstruction)) {
					return entry.path().lexically_relative(sourceRoot).generic_string();
				}
			}
			return {};
		}

		std::string FindPhysicsShaderSurface(const std::filesystem::path& sourceRoot) {
			const std::array<std::filesystem::path, 2> physicsRoots{
				sourceRoot / "Engine/Physics",
				sourceRoot / "Editor/Physics"
			};
			const std::array<std::string_view, 9> shaderExtensions{
				".vert", ".frag", ".comp", ".glsl", ".hlsl", ".metal", ".spv", ".task", ".mesh"
			};
			const std::array<std::string_view, 5> embeddedShaderMarkers{
				"#version ", "gl_Position", "SV_Target", "RWStructuredBuffer", "[numthreads("
			};

			for(const std::filesystem::path& physicsRoot : physicsRoots) {
				if(!std::filesystem::exists(physicsRoot)) {
					continue;
				}

				for(const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(
					physicsRoot, std::filesystem::directory_options::skip_permission_denied)) {
					if(!entry.is_regular_file()) {
						continue;
					}

					const std::string extension = Lowercase(entry.path().extension().string());
					if(std::find(shaderExtensions.begin(), shaderExtensions.end(), extension) != shaderExtensions.end()) {
						return entry.path().lexically_relative(sourceRoot).generic_string();
					}

					if(IsCppSource(entry.path())) {
						const std::string text = ReadTextFile(entry.path());
						for(std::string_view marker : embeddedShaderMarkers) {
							if(text.find(marker) != std::string::npos) {
								return entry.path().lexically_relative(sourceRoot).generic_string();
							}
						}
					}
				}
			}
			return {};
		}
	}

	void RegisterPhysicsDependencyContractTests(TestRegistry& registry) {
		registry.Add("Engine.Physics.DependencyContract.OfficialSubmoduleAndApprovedGitlink", [](TestContext& context) {
			const std::filesystem::path sourceRoot = SourceRoot();
			const std::string modules = ReadTextFile(sourceRoot / ".gitmodules");
			const std::size_t sectionBegin = modules.find("[submodule \"External/JoltPhysics\"]");
			context.Expect(sectionBegin != std::string::npos, ".gitmodules must declare the approved External/JoltPhysics submodule.");

			const std::size_t nextSection = modules.find("[submodule ", sectionBegin + 1U);
			const std::string joltSection = modules.substr(sectionBegin, nextSection - sectionBegin);
			context.Expect(joltSection.find("path = External/JoltPhysics") != std::string::npos,
				"The Jolt submodule must use the approved External/JoltPhysics path.");
			context.Expect(joltSection.find("url = " + std::string(kOfficialJoltUrl)) != std::string::npos,
				"The Jolt submodule must use the official upstream GitHub repository.");

			const CommandResult gitlink = CaptureCommand(GitLinkCommand(sourceRoot));
			context.ExpectEqual(0, gitlink.exitCode,
				"Git must be available so the contract can inspect the immutable parent-repository gitlink.");
			const std::string expectedEntry = "160000 " + std::string(kApprovedJoltCommit) + " 0\tExternal/JoltPhysics";
			context.Expect(gitlink.output.find(expectedEntry) != std::string::npos,
				"The parent repository gitlink must pin External/JoltPhysics to the approved Jolt 5.6.0 commit.");

			const CommandResult providerHead = CaptureCommand(JoltHeadCommand(sourceRoot));
			context.ExpectEqual(0, providerHead.exitCode,
				"Git must be able to inspect the provider checkout used by the build.");
			context.Expect(providerHead.output.find(kApprovedJoltCommit) != std::string::npos,
				"The checked-out Jolt worktree must match the approved parent gitlink.");

			const CommandResult providerStatus = CaptureCommand(JoltStatusCommand(sourceRoot));
			context.ExpectEqual(0, providerStatus.exitCode,
				"Git must be able to verify the provider checkout is pristine.");
			context.Expect(providerStatus.output.empty(),
				"External/JoltPhysics must have no modified or untracked provider files: " + providerStatus.output);
		});

		registry.Add("Engine.Physics.DependencyContract.JoltVersionHeaderIsFiveSixZero", [](TestContext& context) {
			const std::string coreHeader = ReadTextFile(SourceRoot() / "External/JoltPhysics/Jolt/Core/Core.h");
			context.Expect(std::regex_search(coreHeader, std::regex(R"(#define\s+JPH_VERSION_MAJOR\s+5(?:\s|$))")),
				"The vendored Jolt public version header must report major version 5.");
			context.Expect(std::regex_search(coreHeader, std::regex(R"(#define\s+JPH_VERSION_MINOR\s+6(?:\s|$))")),
				"The vendored Jolt public version header must report minor version 6.");
			context.Expect(std::regex_search(coreHeader, std::regex(R"(#define\s+JPH_VERSION_PATCH\s+0(?:\s|$))")),
				"The vendored Jolt public version header must report patch version 0.");
		});

		registry.Add("Engine.Physics.DependencyContract.ProductionProfileIsCoreOnly", [](TestContext& context) {
			const std::filesystem::path sourceRoot = SourceRoot();
			const std::filesystem::path profilePath = sourceRoot / "cmake/TerraEngineDependencies.cmake";
			context.Expect(std::filesystem::exists(profilePath),
				"The production dependency profile must live in cmake/TerraEngineDependencies.cmake.");

			const std::string rootCMake = ReadTextFile(sourceRoot / "CMakeLists.txt");
			context.Expect(rootCMake.find("cmake/TerraEngineDependencies.cmake") != std::string::npos,
				"The root build must include the production dependency profile.");
			const std::string profile = ReadTextFile(profilePath);

			const std::array<std::string_view, 20> expectedOffOptions{
				"JPH_BUILD_SHARED_LIBS",
				"ENABLE_INSTALL",
				"TARGET_UNIT_TESTS",
				"TARGET_HELLO_WORLD",
				"TARGET_PERFORMANCE_TEST",
				"TARGET_SAMPLES",
				"TARGET_VIEWER",
				"ENABLE_OBJECT_STREAM",
				"JPH_USE_DX12",
				"JPH_USE_VK",
				"JPH_USE_MTL",
				"JPH_USE_CPU_COMPUTE",
				"DEBUG_RENDERER_IN_DEBUG_AND_RELEASE",
				"DEBUG_RENDERER_IN_DISTRIBUTION",
				"PROFILER_IN_DEBUG_AND_RELEASE",
				"PROFILER_IN_DISTRIBUTION",
				"JPH_USE_EXTERNAL_PROFILE",
				"OVERRIDE_CXX_FLAGS",
				"ENABLE_ALL_WARNINGS",
				"USE_STATIC_MSVC_RUNTIME_LIBRARY"
			};
			for(std::string_view option : expectedOffOptions) {
				context.Expect(ProfileDisablesOption(profile, option),
					"The core-only Jolt profile must explicitly set " + std::string(option) + " to OFF.");
			}
			context.Expect(ProfileDisablesOption(profile, "DISABLE_CUSTOM_ALLOCATOR"),
				"The production profile must keep Jolt's custom allocator integration enabled.");
			context.Expect(std::regex_search(profile, std::regex(
				R"(set\s*\(\s*_terraengine_jolt_root\s+"[^"]*External/JoltPhysics"\s*\))",
				std::regex_constants::icase)),
				"The production profile must resolve Jolt only from the vendored External/JoltPhysics directory.");
			context.Expect(std::regex_search(profile, std::regex(
				R"(add_subdirectory\s*\(\s*"\$\{_terraengine_jolt_root\}/Build")",
				std::regex_constants::icase)),
				"The production profile must build the pristine vendored Jolt source directly.");
			context.Expect(profile.find("unset(${_terraengine_jolt_option} CACHE)") != std::string::npos,
				"The production profile must clear Jolt's temporary cache options after configuring its target.");

			const std::array<std::string_view, 5> forbiddenNetworkPrimitives{
				"FetchContent", "ExternalProject", "file(DOWNLOAD", "find_package(Jolt", "https://"
			};
			for(std::string_view primitive : forbiddenNetworkPrimitives) {
				context.Expect(profile.find(primitive) == std::string::npos,
					"The production dependency profile must remain offline and may not use "
						+ std::string(primitive) + ".");
			}
		});

		registry.Add("Engine.Physics.DependencyContract.PublicHeadersAreProviderNeutral", [](TestContext& context) {
			const std::string exposedHeader = FindPublicJoltExposure(SourceRoot());
			context.Expect(exposedHeader.empty(),
				"Public Engine headers must not expose Jolt includes or types; first violation: " + exposedHeader);
		});

		registry.Add("Engine.Physics.DependencyContract.NoProviderPrivateWorkerPool", [](TestContext& context) {
			const std::string privatePoolSource = FindProviderPrivatePool(SourceRoot());
			context.Expect(privatePoolSource.empty(),
				"Physics backend code must submit through TerraEngine's shared pool instead of creating provider-private workers; first violation: " + privatePoolSource);
		});

		registry.Add("Engine.Physics.DependencyContract.NoPhysicsShaderSurface", [](TestContext& context) {
			const std::string shaderSource = FindPhysicsShaderSurface(SourceRoot());
			context.Expect(shaderSource.empty(),
				"The core-only physics integration must not add provider compute/render shaders or embed shader source; first violation: " + shaderSource);
		});
	}
}
