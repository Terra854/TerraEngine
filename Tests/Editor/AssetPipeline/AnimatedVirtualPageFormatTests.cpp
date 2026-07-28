// AnimatedVirtualPageFormatTests.cpp - editor-facing physical-page ABI checks.

#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"

#include <cstddef>
#include <cstdint>

namespace TerraEngine::EditorTests {

	void RegisterAnimatedVirtualPageFormatTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.AnimatedVirtualGeometry.FormatIsIndependentAndVersioned", [](TestContext& context)
		{
			context.Expect(kAnimatedVirtualPageDirectoryMagicValue
				!= kDeformableVirtualGeometrySidecarMagicValue,
				"Physical pages must not reinterpret the frozen TDFV sidecar.");
			context.ExpectEqual<std::uint16_t>(1u, kAnimatedVirtualPageFormatVersion,
				"Physical page format changes require an explicit version.");
			context.ExpectEqual<std::uint32_t>(256u, kAnimatedVirtualPageMaximumLocalJoints,
				"Cookers split pages before the compact local palette overflows.");
			context.ExpectEqual<std::size_t>(8u, sizeof(AnimatedVirtualPageSkinRecord),
				"Page-local skin records retain the compact existing encoding.");
		});
	}

}
