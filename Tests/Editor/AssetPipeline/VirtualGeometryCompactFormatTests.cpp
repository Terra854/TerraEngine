#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"

#include <cstddef>

namespace TerraEngine::EditorTests
{

	void RegisterVirtualGeometryCompactFormatTests(TestRegistry &registry)
	{
		registry.Add("Editor.AssetPipeline.VirtualGeometryCompactFormat.KeepsExtensionDirectoryByteLayoutsStable",
			[](TestContext &context)
			{
				// These exact records are independently parsed by cook tools and runtime.
				context.ExpectEqual(
					static_cast<std::size_t>(36u), sizeof(VirtualGeometryExtensionDirectoryHeader), "VGXD headers must retain their frozen byte layout.");
				context.ExpectEqual(
					static_cast<std::size_t>(48u), sizeof(VirtualGeometryExtensionDirectoryEntry), "VGXD entries must retain their frozen byte layout.");
				context.ExpectEqual(
					static_cast<std::size_t>(48u), sizeof(VirtualGeometryCompactBlockHeader), "VGCB block headers must retain their frozen byte layout.");
				context.ExpectEqual(static_cast<std::size_t>(56u),
					sizeof(VirtualGeometryCompactBlockDescriptor),
					"VGXD block descriptors must retain their frozen byte layout.");
				context.ExpectEqual(static_cast<std::size_t>(16u),
					offsetof(VirtualGeometryExtensionDirectoryHeader, entryTableOffset),
					"VGXD entry tables must remain discoverable without reinterpretation.");
			});

		registry.Add("Editor.AssetPipeline.VirtualGeometryCompactFormat.PreservesCodecAndCapabilityIdentities",
			[](TestContext &context)
			{
				context.Expect(kVirtualGeometryCodecCapabilityExpanded != kVirtualGeometryCodecCapabilityTerra,
					"Expanded and Terra prototype capabilities must remain distinguishable.");
				context.Expect(kVirtualGeometryCodecCapabilityTerra != kVirtualGeometryCodecCapabilityMeshoptimizer,
					"Terra and meshoptimizer prototype capabilities must remain distinguishable.");
				context.Expect((kVirtualGeometryCodecCapabilityAll & kVirtualGeometryCodecCapabilityMeshoptimizer) != 0u,
					"The declared capability mask must include the vendored meshoptimizer route.");
			});
	}

} // namespace TerraEngine::EditorTests
