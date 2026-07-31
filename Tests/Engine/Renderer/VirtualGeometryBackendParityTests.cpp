#include "TestHarness.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryBackendParityTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.BackendParity.VulkanMeshComputeIndexedContracts",
			[](TestContext &context)
			{
				const std::filesystem::path root = TERRAENGINE_SOURCE_DIR;
				std::ifstream file(root / "Tools/VirtualGeometry/hardware-matrix.json");
				const std::string matrix{ std::istreambuf_iterator<char>(file), {} };
				context.Expect(matrix.find("Vulkan") != std::string::npos,
					"Backend matrix must name the Vulkan route");
				context.Expect(matrix.find("D3D12-Windows") == std::string::npos,
					"capability validation must not require a deferred D3D12 route");
				context.Expect(
					matrix.find("required-future-lane") != std::string::npos, "Unavailable backend lanes must remain explicit future/unavailable lanes");
				context.Expect(std::filesystem::exists(root / "Engine/Shaders/VirtualGeometry/VirtualGeometryGeneratedIndirect.comp.hlsl"),
					"Indirect parity requires a dedicated generated-work shader");
			});
	}
} // namespace TerraEngine::Tests
