#include "TestHarness.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryShadingBinTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Visibility.ShadingBinsCapacityAndVisibleFallback",
			[](TestContext &context)
			{
				const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/VirtualGeometry/VirtualGeometryShadingBins.comp.hlsl";
				std::ifstream file(path);
				const std::string shader{ std::istreambuf_iterator<char>(file), {} };
				context.Expect(shader.find("binCapacity") != std::string::npos, "Shading bins must declare capacity");
				context.Expect(shader.find("TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_PACKET_OVERFLOW") != std::string::npos, "Bin overflow must publish diagnostics");
			});
	}
} // namespace TerraEngine::Tests
