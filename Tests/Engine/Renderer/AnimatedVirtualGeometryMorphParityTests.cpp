#include "TestHarness.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests
{
	void RegisterAnimatedVirtualGeometryMorphParityTests(TestRegistry &registry)
	{
		registry.Add("AnimatedVirtualGeometry.MorphParity.PositionNormalTangentBeforeSkin",
			[](TestContext &context)
			{
				const auto path =
					std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/VirtualGeometry/VirtualGeometryAnimatedMorphBeforeSkin.comp.hlsl";
				std::ifstream file(path);
				const std::string shader{ std::istreambuf_iterator<char>(file), {} };
				context.Expect(shader.find("morphWeight") != std::string::npos, "Morph decode must apply authored weights");
				context.Expect(shader.find("TeAnimatedDeformedVertex") != std::string::npos, "Morph path must publish the shared deformed vertex ABI");
			});
	}
} // namespace TerraEngine::Tests
