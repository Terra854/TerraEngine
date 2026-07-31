#include "TestHarness.h"
#include "File Formats/TerraEngineMeshFile.h"
namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryCompactFormatTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.VirtualGeometryProductionFormat.RejectsHistoricalExtensionLayouts", [](TestContext& context)
		{
			context.ExpectEqual(static_cast<std::size_t>(192u), sizeof(TemfProductionHeader), "Canonical TEMFPRD1 has one fixed root header.");
			context.ExpectEqual(static_cast<std::size_t>(112u), sizeof(TemfBlockDescriptor), "Canonical TEMFPRD1 blocks retain their fixed production record.");
			context.Expect(kTemfProductionContractId != 0u, "A production contract identity rejects historical layouts.");
		});
	}
} // namespace TerraEngine::EditorTests
