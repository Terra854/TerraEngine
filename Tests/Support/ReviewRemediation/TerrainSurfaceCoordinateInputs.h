#pragma once

// Validation-only authored inputs and independent CPU expectations. Native
// results below come from the actual shader instructions with an explicitly
// relocated validation descriptor ABI, never the installed terrain pipeline.
#include "Terrain/TerrainMaterial.h"
#include "Terrain/TerrainLayerStack.h"
#include "Tests/Support/ReviewRemediation/VulkanReviewFixture.h"
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Tests::Review::TerrainCoordinates {
	struct Extent { std::uint32_t width = 1, height = 1; };
	struct Texel {
		std::uint32_t x = 0, y = 0;
		bool operator==(const Texel&) const = default;
	};
	struct MaskCase {
		std::string name;
		glm::dvec2 uv{};
		Extent holes{17U, 9U}, fallback{64U, 32U};
		bool hole = false, diagnosticFallback = false;
	};
	struct SurfaceCase {
		std::string name;
		Terrain::TerrainAreaCoordinate area{};
		double spacingMetres = 0.5;
		glm::dvec2 pageOrigin{}, pageLocal{};
		Terrain::TerrainLayerMapping mapping{};
	};
	struct ShaderPaths {
		std::filesystem::path sourceRoot;
		std::filesystem::path originalPackages;
		std::filesystem::path evidenceRoot;
	};
	struct NativePixels {
		std::string name;
		std::uint32_t width = 0, height = 0;
		std::vector<std::array<float, 4>> color;
		VulkanReviewSubmission completion;
	};
	[[nodiscard]] Texel MaskTexelOracle(glm::dvec2 uv, Extent extent);
	[[nodiscard]] glm::dvec2 AbsoluteTerrainUvOracle(const SurfaceCase& value);
	[[nodiscard]] std::array<float, 4> PatternOracle(const SurfaceCase& value, glm::dvec2 pageLocal);
	[[nodiscard]] std::vector<SurfaceCase> SurfaceCases();
	[[nodiscard]] std::vector<MaskCase> MaskCases();
	[[nodiscard]] std::vector<NativePixels> RunSurfaceCoordinateInstructions(std::span<const SurfaceCase> cases, const ShaderPaths& paths);
	[[nodiscard]] std::vector<NativePixels> RunVisibilityCoordinateInstructions(std::span<const MaskCase> cases, const ShaderPaths& paths);
}
