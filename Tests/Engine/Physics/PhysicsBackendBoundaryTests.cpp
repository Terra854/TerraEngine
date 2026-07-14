#include "TestHarness.h"

#include "Physics/Backend/IPhysicsBackend.h"

#include <array>
#include <cctype>
#include <concepts>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace TerraEngine::Tests {
	namespace {
		std::filesystem::path SourceRoot() {
			// CMake supplies the checkout root, allowing this boundary test to inspect
			// the installed-source contract without depending on the launch directory.
			return std::filesystem::path(TERRAENGINE_SOURCE_DIR);
		}

		std::string ReadTextFile(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			if(!input) {
				throw TestFailure("Could not read the physics backend contract: " + path.string());
			}

			std::ostringstream text;
			text << input.rdbuf();
			return text.str();
		}

		std::string Lowercase(std::string text) {
			for(char& character : text) {
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			}
			return text;
		}

		template<typename Backend>
		concept ProviderNeutralBackendContract =
			std::is_abstract_v<Backend>
			&& std::has_virtual_destructor_v<Backend>;

		bool ContainsJoltSurface(std::string_view headerText) {
			// These spellings cover provider includes, the Jolt namespace, and the
			// common Jolt macro prefix. Any occurrence would leak provider details
			// into a TerraEngine-owned header.
			constexpr std::array<std::string_view, 5> forbiddenTokens{
				"#include <jolt/",
				"#include \"jolt/",
				"jph::",
				"namespace jph",
				"jph_"
			};
			const std::string lowercaseText = Lowercase(std::string(headerText));
			for(const std::string_view token : forbiddenTokens) {
				if(lowercaseText.find(token) != std::string::npos) {
					return true;
				}
			}
			return false;
		}
	}

	void RegisterPhysicsBackendBoundaryTests(TestRegistry& registry) {
		registry.Add("Engine.Physics.BackendBoundary.CoarseContractIsAbstractAndProviderNeutral", [](TestContext& context) {
			using TerraEngine::Physics::Backend::IPhysicsBackend;

			// The interface is intentionally abstract: gameplay and engine systems
			// own stable TerraEngine values, while only a private implementation owns
			// the selected provider objects and their lifetime.
			static_assert(ProviderNeutralBackendContract<IPhysicsBackend>);
			std::unique_ptr<IPhysicsBackend> backend;
			context.Expect(!backend,
				"A backend boundary test must be able to hold an empty provider-neutral interface pointer without constructing a provider world.");
		});

		registry.Add("Engine.Physics.BackendBoundary.HeaderContainsNoJoltTypesOrIncludes", [](TestContext& context) {
			const std::string headerText = ReadTextFile(SourceRoot() / "Engine/Physics/Backend/IPhysicsBackend.h");
			context.Expect(!ContainsJoltSurface(headerText),
				"IPhysicsBackend.h must expose only TerraEngine-owned values and must not include or name Jolt types.");
		});

		registry.Add("Engine.Physics.BackendBoundary.CoarseLifecycleAndOperationVocabularyIsPresent", [](TestContext& context) {
			const std::string headerText = Lowercase(ReadTextFile(SourceRoot() / "Engine/Physics/Backend/IPhysicsBackend.h"));

			// These coarse verbs are deliberately checked instead of provider classes.
			// They preserve a replaceable backend while covering the lifecycle, shape,
			// body, simulation, query, and extraction responsibilities in the plan.
			constexpr std::array<std::string_view, 14> requiredOperationFragments{
				"createworld", "destroyworld", "cook", "createbody", "updatebody",
				"destroybody", "createconstraint", "destroyconstraint", "createcharacter",
				"destroycharacter", "apply", "step", "query", "extract"
			};
			for(const std::string_view operation : requiredOperationFragments) {
				context.Expect(headerText.find(operation) != std::string::npos,
					"IPhysicsBackend.h must declare the coarse provider-neutral operation '" + std::string(operation) + "'.");
			}
		});
}
}
