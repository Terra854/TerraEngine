#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"

namespace TerraEngine::EditorTests
{

	void RegisterVirtualGeometryPageProfileTests(TestRegistry &registry)
	{
		registry.Add("Editor.AssetPipeline.VirtualGeometryPageProfile.ExposesThreeBoundedExperimentSizes",
			[](TestContext &context)
			{
				// The profile values are serialized page facts, not editor-only settings.
				context.ExpectEqual(32u * 1024u,
					GetVirtualGeometryPageProfileBytes(VirtualGeometryPageSizeProfile::KiB32),
					"32 KiB prototype pages must retain their exact byte budget.");
				context.ExpectEqual(64u * 1024u,
					GetVirtualGeometryPageProfileBytes(VirtualGeometryPageSizeProfile::KiB64),
					"64 KiB prototype pages must retain their exact byte budget.");
				context.ExpectEqual(128u * 1024u,
					GetVirtualGeometryPageProfileBytes(VirtualGeometryPageSizeProfile::KiB128),
					"128 KiB prototype pages must retain their exact byte budget.");
				context.Expect(IsVirtualGeometryPagePayloadWithinProfile(VirtualGeometryPageSizeProfile::KiB32, 32u * 1024u),
					"A page exactly at the selected profile budget should remain valid.");
				context.Expect(!IsVirtualGeometryPagePayloadWithinProfile(VirtualGeometryPageSizeProfile::KiB32, 32u * 1024u + 1u),
					"A page that exceeds its selected profile must be rejected before publication.");
			});
	}

} // namespace TerraEngine::EditorTests
