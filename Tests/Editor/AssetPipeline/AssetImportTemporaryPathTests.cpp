// Browser admission must reject transaction-private names before filesystem
// status touches staging paths that an import worker may already be removing.
#include "TestHarness.h"
#include "ModelPackagePrivatePaths.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace TerraEngine::EditorTests
{
	void RegisterAssetImportTemporaryPathTests(TestRegistry& registry)
	{
		using namespace TerraEngine::Editor::Assets;
		registry.Add("Renderer.Efficiency.Editor.AssetImportTemporary.PrivateRootsAndDescendantsNeverProbe", [](TestContext& context) {
			const std::array<std::string,2> names{
				".ta-07a1e4880ad8ec0e8e4eb847f8b99962", ".ta-0123456789abcdef"};
			for (const auto& name : names) {
				for (const auto& suffix : {std::filesystem::path{}, std::filesystem::path{"Models"}, std::filesystem::path{"Models/child.gltf"}}) {
					const auto path = std::filesystem::path{"C:/Fixture/Assets"} / name / suffix;
					std::uint32_t probes=0;
					bool threw=false;
					std::optional<bool> result;
					try {
						result=InspectProjectBrowserEntry(path, [&](const auto&) -> bool {
							++probes;
							throw std::filesystem::filesystem_error("status",path,std::make_error_code(std::errc::permission_denied));
						});
					} catch (const std::filesystem::filesystem_error&) { threw=true; }
					context.Expect(!threw && !result.has_value() && probes==0,
						"Exact private transaction roots and nested descendants must be excluded before status can throw.");
				}
			}
		});
		registry.Add("Renderer.Efficiency.Editor.AssetImportTemporary.AuthoredNearMatchesStillProbeAndPropagate", [](TestContext& context) {
			const std::array<std::string,8> names{
				".ta-authored", ".ta-0123456789ABCDEF", ".ta-07A1E4880AD8EC0E8E4EB847F8B99962",
				".ta-0123456789abcdeg", ".ta-0123456789abcde", ".ta-0123456789abcdef0",
				".ta-07a1e4880ad8ec0e8e4eb847f8b9996", ".ta-07a1e4880ad8ec0e8e4eb847f8b999620"};
			for (const auto& name : names) {
				const auto path=std::filesystem::path{"C:/Fixture/Assets"} / name / "authored.gltf";
				std::uint32_t probes=0;
				bool actualDenied=false;
				try {
					(void)InspectProjectBrowserEntry(path,[&](const auto&) -> bool {
						++probes;
						throw std::filesystem::filesystem_error("status",path,std::make_error_code(std::errc::permission_denied));
					});
				} catch (const std::filesystem::filesystem_error& error) {
					actualDenied=error.code()==std::make_error_code(std::errc::permission_denied);
				}
				context.Expect(probes==1 && actualDenied,
					"Authored lookalikes must remain visible to real status probes and permission errors must propagate.");
			}
		});
		registry.Add("Renderer.Efficiency.Editor.AssetImportTemporary.PreservesActualAuthoredDirectoryAndFileStatus", [](TestContext& context) {
			const auto path=std::filesystem::path{"C:/Fixture/Assets/Models/authored.gltf"};
			for (bool directory : {false,true}) {
				std::uint32_t probes=0;
				const auto result=InspectProjectBrowserEntry(path,[&](const auto& observed) {
					++probes;
					context.Expect(observed==path,"The status probe must receive the unchanged authored path.");
					return directory;
				});
				context.Expect(result.has_value() && *result==directory && probes==1,
					"Authored directory/file status must be preserved without an invented fallback.");
			}
		});
	}
}
