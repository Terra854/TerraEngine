#include "TestHarness.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryComputeRasterTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Visibility.ComputeRasterSetupCoverageAtomics",
			[](TestContext &context)
			{
				const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/VirtualGeometry/VirtualGeometryComputeRaster.comp.hlsl";
				std::ifstream file(path);
				const std::string shader{ std::istreambuf_iterator<char>(file), {} };
				context.Expect(shader.find("numthreads") != std::string::npos, "Compute raster requires bounded dispatch");
				context.Expect(shader.find("Interlocked") != std::string::npos, "Compute raster requires atomic depth/tie updates");
				context.Expect(shader.find("pixelCount") != std::string::npos, "Large-triangle coverage must be capacity bounded");
				context.Expect(shader.find("ClipAgainstFrustumPlane") != std::string::npos,
					"Compute raster must clip homogeneous frustum crossings before coverage evaluation.");
				context.Expect(shader.find("IsTopLeftEdge") != std::string::npos,
					"Compute raster must use the top-left rule to avoid shared-edge cracks.");
				context.Expect(shader.find("for(uint y") != std::string::npos && shader.find("for(uint x") != std::string::npos,
					"Each triangle must visit its bounded screen rectangle so large triangles remain complete.");
				context.Expect(shader.find("source.identity.x") != std::string::npos,
					"Equal-depth writes must resolve with a stable primitive identity.");
			});
	}
} // namespace TerraEngine::Tests
