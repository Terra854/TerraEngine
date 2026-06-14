#include "TestHarness.h"

#include "ExportScriptBindingRewriter.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests {
	namespace {
		class TempStagingDirectory {
		public:
			TempStagingDirectory()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "ExportScriptBindingRewriter") {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(root_);
			}

			~TempStagingDirectory() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] std::filesystem::path Root() const {
				return root_;
			}

			[[nodiscard]] std::filesystem::path ScenePath() const {
				return root_ / "Scenes" / "Startup.tescene";
			}

			void WriteScene(const nlohmann::json& scene) const {
				const std::filesystem::path path = ScenePath();
				std::filesystem::create_directories(path.parent_path());
				std::ofstream stream(path, std::ios::binary);
				stream << scene.dump(4);
			}

		private:
			std::filesystem::path root_;
		};

		nlohmann::json ReadJsonFile(const std::filesystem::path& path) {
			std::ifstream stream(path, std::ios::binary);
			return nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()));
		}
	}

	void RegisterExportScriptBindingRewriterTests(TestRegistry& registry) {
		registry.Add("Editor.ExportScriptBindingRewriter.RewritesClassNamesToScriptIdsAndPreservesValues", [](TestContext& context) {
			TempStagingDirectory staging;
			const std::string className = "Game.PlayerController";
			const nlohmann::json binding = {
				{ "bindingId", "player-binding" },
				{ "className", className },
				{ "enabled", true },
				{ "serializedValues", nlohmann::json::array({
					{
						{ "fieldName", "Speed" },
						{ "fieldPath", "Speed" },
						{ "displayName", "Speed" },
						{ "valueKind", "Float" },
						{ "value", 12.5 }
					}
				}) }
			};
			nlohmann::json scene = {
				{ "name", "Script Rewrite Test" },
				{ "objects", nlohmann::json::array({
					{
						{ "name", "Player" },
						{ "components", {
							{ "Script", {
								{ "BindingsJson", nlohmann::json::array({ binding }).dump() }
							} }
						} }
					}
				}) }
			};
			staging.WriteScene(scene);

			std::string error;
			const bool rewritten = RewriteStagedScriptBindings(staging.Root(), error);
			const nlohmann::json rewrittenScene = ReadJsonFile(staging.ScenePath());
			const std::string bindingsText = rewrittenScene["objects"][0]["components"]["Script"]["BindingsJson"].get<std::string>();
			const nlohmann::json rewrittenBindings = nlohmann::json::parse(bindingsText);

			context.Expect(rewritten, error.empty() ? "The staged scene should rewrite successfully." : error);
			context.ExpectEqual(1u, static_cast<unsigned int>(rewrittenBindings.size()), "The binding count should be preserved.");
			context.Expect(!rewrittenBindings[0].contains("className"), "Export staging should remove editor-only class names from staged bindings.");
			context.ExpectEqual(ComputeExportScriptBindingId(className), rewrittenBindings[0]["scriptId"].get<std::uint64_t>(), "The staged binding should receive the deterministic NativeAOT script ID.");
			context.ExpectEqual(std::string("player-binding"), rewrittenBindings[0]["bindingId"].get<std::string>(), "The rewrite should preserve binding identity.");
			context.ExpectEqual(std::string("Speed"), rewrittenBindings[0]["serializedValues"][0]["fieldPath"].get<std::string>(), "The rewrite should preserve authored field paths.");
			context.ExpectEqual(12.5, rewrittenBindings[0]["serializedValues"][0]["value"].get<double>(), "The rewrite should preserve authored field values.");
		});
	}
}
