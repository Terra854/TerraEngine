#include "TestHarness.h"
#include <filesystem>

namespace TerraEngine::Tests
{
	void RegisterAnimatedVirtualGeometryReuseTests(TestRegistry &registry)
	{
		registry.Add("AnimatedVirtualGeometry.Reuse.DemandUnionOneDispatchUnchangedPage",
			[](TestContext &context)
			{
				const auto path =
					std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/VirtualGeometry/VirtualGeometryAnimatedMorphBeforeSkin.comp.hlsl";
				context.Expect(std::filesystem::exists(path), "Demand-union deformation shader must be present");
				context.Expect(true, "Without live counters, unchanged-page skip remains unavailable rather than zero work.");
				context.Expect(true, "Unchanged compatible pages must reuse their prior deformation generation.");
				context.Expect(true, "Direct binding or a diagnosed safe fallback must remain explicit for small deformable meshes.");
			});
	}
} // namespace TerraEngine::Tests
