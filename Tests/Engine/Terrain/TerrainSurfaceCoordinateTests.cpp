// CPU expectations exercise authored coordinate APIs; GPU cases execute actual
// shader instructions in a watched offscreen fixture with fenced readback.
#include "../TestHarness.h"
#include "Utilities/UUID.h"
#include "Tests/Support/ReviewRemediation/TerrainSurfaceCoordinateInputs.h"
#include <cmath>
#include <cstdlib>
#include <glm/geometric.hpp>

namespace TerraEngine::Tests {
	namespace Coordinates = Review::TerrainCoordinates;
	namespace {
		Coordinates::ShaderPaths Paths(std::string_view lane) {
			const char* packages = std::getenv("TERRAENGINE_REVIEW_TERRAIN_SHADER_PACKAGES");
			if(!packages || !*packages) throw std::runtime_error("Coordinate-instruction fixture requires explicit original installed terrain shader packages; installed-pipeline acceptance remains unverified.");
			Coordinates::ShaderPaths result; result.sourceRoot = TERRAENGINE_SOURCE_DIR; result.originalPackages = packages;
			// Exclusive directory reservation isolates concurrent/repeated native
			// children and preserves every prior shader/readback/receipt bundle.
			const auto parent = result.sourceRoot / "out/validation/review-remediation/sol-6/terrain-coordinate-instruction" / lane;
			std::filesystem::create_directories(parent);
			for(unsigned attempt = 0; attempt < 32; ++attempt) {
				result.evidenceRoot = parent / TerraEngine::ToHexString(TerraEngine::GenerateUUID());
				if(std::filesystem::create_directory(result.evidenceRoot)) return result;
			}
			throw std::runtime_error("Could not exclusively reserve terrain coordinate evidence.");
		}
		bool RunInWatchedChild(TestContext& context, const std::string& name) {
			if(Review::VulkanReviewFixture::IsWatchedChild(name)) return true;
			const auto child = Review::VulkanReviewFixture::RunWatchedSelf(name, std::chrono::seconds(90));
			context.Expect(child.started && !child.timedOut && child.exitCode == 0, "Actual coordinate-instruction child must complete successfully: " + child.output);
			return false;
		}
		bool NearColor(const std::array<float, 4>& a, const std::array<float, 4>& b) {
			for(std::size_t i = 0; i < 4; ++i) if(!std::isfinite(a[i]) || std::abs(a[i] - b[i]) > 1.0e-4F) return false;
			return true;
		}
	}
	void RegisterTerrainSurfaceCoordinateTests(TestRegistry& registry) {
		registry.Add("Terrain.SurfaceCoordinates.CpuPhaseMatchesIndependentAuthoredAddress", [](TestContext& context) {
			for(auto value : Coordinates::SurfaceCases()) {
				Terrain::TerrainAreaMaterialPhase phase;
				context.Expect(Terrain::PrepareTerrainAreaMaterialPhase(value.area, value.spacingMetres, value.mapping, phase).Succeeded(), "Production CPU phase preparation must accept authored probe inputs.");
				for(const auto local : {glm::dvec2(0.0, 0.0), glm::dvec2(2.0, 1.0), glm::dvec2(3.0, 0.0)}) {
					value.pageLocal = local;
					const auto expected = Coordinates::AbsoluteTerrainUvOracle(value);
					const auto actual = Terrain::EvaluateTerrainSurfaceUv(phase, value.mapping, value.pageOrigin + local);
					context.Expect(glm::length(actual - expected) < 1.0e-7, "Production CPU mapping must agree with independent full-double authored coordinates.");
				}
			}
		});
		registry.Add("Terrain.SurfaceCoordinates.IndependentMaskBoundsOracle", [](TestContext& context) {
			context.Expect(Coordinates::MaskTexelOracle({-0.5, -0.25}, {17, 9}) == Coordinates::Texel{0, 0}, "Negative UV clamps to the first real texel.");
			context.Expect(Coordinates::MaskTexelOracle({1.0, 1.5}, {17, 9}) == Coordinates::Texel{16, 8}, "Upper UV clamps to each mask's own final texel.");
			context.Expect(Coordinates::MaskTexelOracle({0.5, 0.5}, {17, 9}) == Coordinates::Texel{8, 4}, "Hole dimensions define their own interior address.");
			context.Expect(Coordinates::MaskTexelOracle({0.5, 0.5}, {64, 32}) == Coordinates::Texel{32, 16}, "Fallback dimensions define a different interior address.");
			context.Expect(Coordinates::MaskTexelOracle({1.5, -1.0}, {1, 1}) == Coordinates::Texel{0, 0}, "A real one-texel mask always addresses its only texel.");
		});
		// Broad legacy Engine suites remain CPU-only by default. Native proof is
		// a separately enabled, watched lane and cannot become an implicit pass.
		if(!Review::VulkanReviewFixture::IsNativeReviewEnabled()) return;
		const std::string surface = "Native.ReviewSimulation.TerrainCoordinateInstruction.TriplanarPageOrigin";
		registry.Add(surface, [surface](TestContext& context) {
			if(!RunInWatchedChild(context, surface)) return;
			const auto cases = Coordinates::SurfaceCases(); const auto results = Coordinates::RunSurfaceCoordinateInstructions(cases, Paths("surface"));
			context.ExpectEqual(cases.size(), results.size(), "Every authored surface probe must produce a real GPU readback.");
			for(std::size_t i = 0; i < cases.size(); ++i) {
				const auto& result = results[i];
				context.Expect(result.completion.submitted && result.completion.completed && result.completion.completionSerial != 0, "Shader output requires actual submitted fence completion.");
				context.Expect(result.width == 4U && result.height == 4U && result.color.size() == 16U, "Over-dispatch must return bounded output with full active derivative quads.");
				for(std::uint32_t y = 0; y < result.height; ++y) for(std::uint32_t x = 0; x < result.width; ++x)
					context.Expect(NearColor(result.color[y * result.width + x], Coordinates::PatternOracle(cases[i], {x, y})), "Actual triplanar color must match the independent authored/page-origin oracle: " + cases[i].name);
			}
			context.Expect(NearColor(results[1].color[2], results[2].color[0]), "Adjacent page descriptions of the same authored sample must match continuously.");
			context.Expect(NearColor(results[5].color[3], results[6].color[0]), "Negative-to-zero adjacent areas must agree at the shared authored boundary.");
			context.Expect(NearColor(results[7].color[3], results[8].color[0]), "Zero-to-positive adjacent areas must agree at the shared authored boundary.");
		});
		const std::string visibility = "Native.ReviewSimulation.TerrainCoordinateInstruction.MaskAddressing";
		registry.Add(visibility, [visibility](TestContext& context) {
			if(!RunInWatchedChild(context, visibility)) return;
			const auto cases = Coordinates::MaskCases(); const auto results = Coordinates::RunVisibilityCoordinateInstructions(cases, Paths("visibility"));
			context.ExpectEqual(cases.size(), results.size(), "Every authored mask probe must return actual fragment-stage pixels.");
			for(std::size_t i = 0; i < cases.size(); ++i) {
				const auto& result = results[i]; context.Expect(result.completion.completed && result.completion.submitted, "Fragment output requires actual fence completion.");
				context.Expect(result.color.size() == 2U, "The same authored UV must be sampled at two different screen positions.");
				const std::array<float, 4> clear{0.25F, 0.0F, 0.25F, 1.0F}, healthy{0.1F, 0.4F, 0.8F, 1.0F};
				if(cases[i].hole) context.Expect(NearColor(result.color[0], clear) && NearColor(result.color[1], clear), "Authored hole must discard at both screen positions: " + cases[i].name);
				else if(!cases[i].diagnosticFallback) context.Expect(NearColor(result.color[0], healthy) && NearColor(result.color[1], healthy), "Healthy authored surface must remain visible at both screen positions.");
				else {
					// Inputs deliberately use zero phase, unit spacing and a four-metre
					// tile. Derive the expected diagnostic cell from authored UV, without
					// using any pixel coordinate or shader-produced address as the oracle.
					const auto cell = static_cast<std::int64_t>(std::floor(cases[i].uv.x * 128.0)) + static_cast<std::int64_t>(std::floor(cases[i].uv.y * 128.0));
					const std::array<float, 4> expected = (cell & 1) ? std::array<float, 4>{1.0F, 0.82F, 0.0F, 1.0F} : std::array<float, 4>{0.015F, 0.015F, 0.015F, 1.0F};
					context.Expect(NearColor(result.color[0], expected) && NearColor(result.color[1], expected), "Authored fallback diagnostic must match its independent UV oracle at both screen positions.");
				}
			}
		});
	}
}
