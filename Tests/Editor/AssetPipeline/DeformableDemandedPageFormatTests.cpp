// DeformableDemandedPageFormatTests.cpp - canonical page ABI checks.

#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"

#include <cstddef>
#include <cstdint>

namespace TerraEngine::EditorTests {

	void RegisterDeformableDemandedPageFormatTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.DeformableDemandedPageFormat.CanonicalLayout", [](TestContext& context)
		{
			context.ExpectEqual<std::size_t>(40u,
				sizeof(TemfDeformableDemandedPageDirectory),
				"Canonical demanded-page directory metadata must stay byte exact.");
			context.ExpectEqual<std::uint32_t>(256u, kTemfMaximumPaletteJoints,
				"Cookers split pages before the compact local palette overflows.");
			context.ExpectEqual<std::size_t>(8u, sizeof(TemfDeformablePageSkinRecord),
				"Canonical page-local skin records retain the compact encoding.");
		});
	}

}
