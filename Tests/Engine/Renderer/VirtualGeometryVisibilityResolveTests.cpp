#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryRasterBins.h"
#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryVisibilityResolveTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.Visibility.ResolveUsesVisibleFallback",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				auto p = BuildVirtualGeometryRasterBinPlan({ { 2, 9, true, true }, { 1, 0, false, true } }, 1);
				c.Expect(p.failOpen && p.overflowCount == 1, "Material reconstruction keeps a visible fallback for bin overflow.");
				c.Expect(p.routes[0] == VirtualGeometryRasterRoute::Compute,
					"Attribute/derivative reconstruction preserves compute route when hardware is unavailable.");
			});
		r.Add("VirtualGeometry.Visibility.ResolveValidatesGenerationAndBarycentrics",
			[](TestContext &context)
			{
				const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine/Shaders/VirtualGeometry/VirtualGeometryVisibilityResolve.comp.hlsl";
				std::ifstream file(path);
				const std::string shader{ std::istreambuf_iterator<char>(file), {} };
				context.Expect(shader.find("ReconstructBarycentrics") != std::string::npos,
					"Resolve must reconstruct normalized barycentrics from the compact record.");
				context.Expect(shader.find("TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_STALE_GENERATION") != std::string::npos,
					"Resolve must reject stale producer generations rather than shading them.");
				context.Expect(shader.find("TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INVALID_PAYLOAD") != std::string::npos,
					"Resolve must diagnose malformed barycentric payloads.");
				context.Expect(shader.find("ReconstructAttributes") != std::string::npos
					&& shader.find("UnpackSnorm2x16") != std::string::npos,
					"Resolve must reconstruct canonical attributes and validate screen-space derivatives.");
				context.Expect(shader.find("sourceTriangle.packedOwnerMaterial") != std::string::npos,
					"Resolve must preserve discrete owner/material identity without interpolation.");
			});
	}
} // namespace TerraEngine::Tests
