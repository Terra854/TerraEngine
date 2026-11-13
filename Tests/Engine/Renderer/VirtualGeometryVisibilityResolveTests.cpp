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
					"Surface reconstruction preserves compute route when hardware is unavailable.");
			});
		r.Add("VirtualGeometry.Visibility.ResolveValidatesGenerationAndBarycentrics",
			[](TestContext &context)
			{
				const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine/Shaders/VirtualGeometry/VirtualGeometryVisibilityResolve.comp.hlsl";
				std::ifstream file(path);
				const std::string shader{ std::istreambuf_iterator<char>(file), {} };
				context.Expect(shader.find("DecodeVirtualGeometryPerspectiveWeights") != std::string::npos,
					"Resolve must decode normalized direct perspective weights from the compact V2 record.");
				context.Expect(shader.find("TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_STALE_GENERATION") != std::string::npos,
					"Resolve must reject stale producer generations rather than shading them.");
				context.Expect(shader.find("TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INVALID_PAYLOAD") != std::string::npos,
					"Resolve must diagnose malformed barycentric payloads.");
				// This retained source-contract check is not execution proof. The
				// opt-in native review suite executes actual raster, resolve and the
				// production reconstruction helper against an independent depth oracle.
				context.Expect(shader.find("inputRecord.z") != std::string::npos
					&& shader.find("TE_VIRTUAL_GEOMETRY_VISIBILITY_ENCODING_TOKEN") != std::string::npos,
					"Resolve must preserve canonical raster depth and validate the transient encoding generation.");
				context.Expect(shader.find("sourceTriangle.identity.y") != std::string::npos,
					"Resolve must preserve discrete owner/material identity without interpolation.");
			});
	}
} // namespace TerraEngine::Tests
