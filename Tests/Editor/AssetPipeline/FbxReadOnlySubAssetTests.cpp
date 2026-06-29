#include "TestHarness.h"

#include "FbxTestHelpers.h"

#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		bool PathContainsGeneratedFolder(const std::filesystem::path& path)
		{
			for (const auto& part : path)
			{
				const std::string name = part.string();
				if (name.ends_with("_Generated"))
				{
					return true;
				}
			}
			return false;
		}
	}

	void RegisterFbxReadOnlySubAssetTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.FbxReadOnlySubAsset.GeneratedMaterialsStayPackageOwned", [](TestContext& context) {
			const auto result = ImportFbxFixture("box.fbx");
			context.Expect(!result.generatedMaterials.empty(), "FBX import should generate material sub-assets.");
			context.Expect(PathContainsGeneratedFolder(result.generatedMaterials.front().sourcePath), "Generated material source should live under the package-owned generated folder.");
			context.Expect(result.generatedMaterials.front().sourcePath.extension() == ".tmat", "Generated material should be a selectable material asset.");
		});
	}
}
