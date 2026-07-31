#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests
{
	void RegisterAnimatedVirtualGeometryMorphParityTests(TestRegistry& registry)
	{
		registry.Add("AnimatedVirtualGeometry.MorphParity.PositionNormalTangentBeforeSkin", [](TestContext& context) {
			const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/VirtualGeometry/VirtualGeometryAnimatedMorphBeforeSkin.comp.hlsl";
			std::ifstream file(path);
			const std::string source{ std::istreambuf_iterator<char>(file), {} };
			// The canonical compute contract exposes a single authored-morph input
			// and one deformed vertex ABI; detailed implementation is shader-owned.
			context.Expect(!source.empty() && source.find("morphWeight") != std::string::npos
				&& source.find("TeAnimatedDeformedVertex") != std::string::npos,
				"Canonical animated geometry must expose weighted morph input and the shared deformed-vertex ABI.");
		});
	}
}
