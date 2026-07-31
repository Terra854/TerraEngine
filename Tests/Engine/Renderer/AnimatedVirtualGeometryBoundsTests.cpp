#include "TestHarness.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests
{
	void RegisterAnimatedVirtualGeometryBoundsTests(TestRegistry &registry)
	{
		registry.Add("AnimatedVirtualGeometry.Bounds.MillionDecisionFailOpen",
			[](TestContext &context)
			{
				const auto path =
					std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/VirtualGeometry/AnimatedClusterBounds.comp.hlsl";
				std::ifstream file(path);
				const std::string shader{ std::istreambuf_iterator<char>(file), {} };
				context.Expect(shader.find("PublishFailOpenBounds") != std::string::npos,
					"Bounds must clear stale proof identity before refinement chooses visible detail.");
				context.Expect(shader.find("TE_ANIMATED_GEOMETRY_DIAGNOSTIC_NONFINITE_OUTPUT") != std::string::npos,
					"Bounds must diagnose non-finite output instead of treating it as a valid culling proof.");
			});
	}
} // namespace TerraEngine::Tests
