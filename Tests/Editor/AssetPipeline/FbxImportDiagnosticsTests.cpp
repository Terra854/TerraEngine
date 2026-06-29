#include "TestHarness.h"

#include "FbxTestHelpers.h"

#include <algorithm>

namespace TerraEngine::EditorTests
{
	void RegisterFbxImportDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.FbxImportDiagnostics.WarnsForUnsupportedAnimationAndSkeleton", [](TestContext& context) {
			const auto result = ImportFbxFixture("animation_with_skeleton.fbx");
			context.Expect(result.status == TerraEngine::AssetPipeline::ModelImportStatus::SucceededWithWarnings, "Animated FBX should import static data with warnings.");
			const bool hasAnimationWarning = std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic) {
				return diagnostic.code == "UnsupportedAnimationIgnored";
			});
			const bool hasSkeletonWarning = std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic) {
				return diagnostic.code == "UnsupportedSkeletonIgnored";
			});
			context.Expect(hasAnimationWarning, "Importer should report ignored animation data.");
			context.Expect(hasSkeletonWarning, "Importer should report ignored skeleton data.");
		});
	}
}
