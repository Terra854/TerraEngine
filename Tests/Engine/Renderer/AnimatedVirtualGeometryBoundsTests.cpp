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

		registry.Add("AnimatedVirtualGeometry.Bounds.QuantizationErrorIsMonotonic",
			[](TestContext &context)
			{
				// The cook-side model owns error accumulation. Checking its named
				// contract here ensures animated bounds integration cannot later
				// assume an underestimated parent error without adding an engine-to-
				// editor dependency.
				const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR) /
					"Editor/AssetCompilers/VirtualGeometryErrorModel.cpp";
				std::ifstream file(path);
				const std::string source{ std::istreambuf_iterator<char>(file), {} };
				context.Expect(source.find("BuildVirtualGeometryAccumulatedDetailError") != std::string::npos,
					"Animated bounds must consume a named accumulated quantization-error contract.");
				context.Expect(source.find("std::max(result.accumulatedError, childError)") != std::string::npos,
					"Accumulated detail error must never be lower than any child error.");
			});
	}
} // namespace TerraEngine::Tests
