#include "File Formats/TerraEngineMeshFile.h"
#include "TestHarness.h"
namespace TerraEngine::EditorTests
{
	void RegisterAnimatedVirtualGeometryCompactFormatTests(TestRegistry &r)
	{
		r.Add("Editor.AssetPipeline.AnimatedVirtualGeometry.CompactStreamsAreVersioned",
			[](TestContext &c)
			{
				AnimatedVirtualGeometryCompactStreamDescriptor d{};
				d.stableTopologyId = 7;
				d.stableIdentityHash = 9;
				d.requiredCapabilityMask = 1;
				c.Expect(sizeof(d) == 40 && d.version == kAnimatedVirtualGeometryCompactStreamVersion && d.stableTopologyId != 0 && d.stableIdentityHash != 0,
					"Compact stream metadata preserves version/capability/identity while legacy sidecars stay optional.");
				AnimatedVirtualGeometryCompactStreamDescriptor normal = d;
				normal.kind = AnimatedVirtualGeometryCompactStreamKind::Normal;
				normal.byteOffset = 64u;
				normal.storedByteSize = 32u;
				normal.decodedByteSize = 96u;
				AnimatedVirtualGeometryCompactStreamDescriptor tangent = normal;
				tangent.kind = AnimatedVirtualGeometryCompactStreamKind::Tangent;
				c.Expect(normal.byteOffset + normal.storedByteSize <= 128u && tangent.kind == AnimatedVirtualGeometryCompactStreamKind::Tangent,
					"Position, normal, and tangent streams retain bounded byte ranges and distinct kinds.");
			});
	}
} // namespace TerraEngine::EditorTests
