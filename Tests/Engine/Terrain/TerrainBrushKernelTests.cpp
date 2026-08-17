// TerrainBrushKernelTests.cpp -- Deterministic mathematical brush-kernel checks.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainBrushEngine.h"

#include <array>

namespace TerraEngine::Tests
{
namespace
{
	using namespace TerraEngine::Editor::Terrain;
	constexpr std::uint32_t kWidth = 5U, kHeight = 5U;
	std::array<std::int32_t, kWidth * kHeight> Apply(TerrainBrushSettings settings, std::span<const std::uint8_t> mask = {})
	{
		std::array<std::int32_t, kWidth * kHeight> source{};
		std::array<std::int32_t, kWidth * kHeight> output{};
		TerrainBrushEngine::ApplyStamp(source, output, kWidth, kHeight, settings, {2.0F, 2.0F}, mask);
		return output;
	}
}

	void RegisterTerrainBrushKernelTests(TestRegistry& registry)
	{
		registry.Add("Terrain.BrushKernel.AllModesUseRealDeterministicKernel", [](TestContext& context) {
			TerrainBrushSettings settings; settings.radiusSamples = 2.0F; settings.strengthMm = 100.0F;
			settings.mode = TerrainBrushMode::Raise; context.Expect(Apply(settings)[12] > 0, "Raise must increase the actual centre sample.");
			settings.mode = TerrainBrushMode::Lower; context.Expect(Apply(settings)[12] < 0, "Lower must decrease the actual centre sample.");
			settings.mode = TerrainBrushMode::Smooth; context.Expect(Apply(settings)[12] == 0, "Smooth must preserve a flat field.");
			settings.flattenHeightMm = 42; settings.mode = TerrainBrushMode::FlattenValue; context.Expect(Apply(settings)[12] == 42, "Flatten value must use the declared millimetre target.");
			settings.mode = TerrainBrushMode::FlattenPicked; context.Expect(Apply(settings)[12] == 42, "Flatten picked shares the explicit picked-height target.");
			settings.terraceStepMm = 25; settings.mode = TerrainBrushMode::Terrace; context.Expect(Apply(settings)[12] == 0, "Terrace must keep an already aligned height stable.");
			settings.mode = TerrainBrushMode::Ramp; context.Expect(Apply(settings)[12] == 42, "Ramp must pass through its declared centre target.");
			settings.mode = TerrainBrushMode::Stamp; context.Expect(Apply(settings)[12] > 0, "Stamp must apply its radial imprint.");
			settings.randomSeed = 7U; settings.mode = TerrainBrushMode::Noise; context.Expect(Apply(settings) == Apply(settings), "Noise must remain deterministic for its seed.");
		});

		registry.Add("Terrain.BrushKernel.FalloffPressureMaskRotationSpacingAndBounds", [](TestContext& context) {
			context.ExpectEqual(1.0F, TerrainBrushEngine::EvaluateFalloff(TerrainBrushFalloff::Linear, 0.0F), "Centre falloff must be full strength.");
			context.ExpectEqual(0.0F, TerrainBrushEngine::EvaluateFalloff(TerrainBrushFalloff::SmoothStep, 1.0F), "Brush edge must have zero smooth falloff.");
			context.Expect(TerrainBrushEngine::IsStrokeSampleDue({0.0F, 0.0F}, {2.0F, 0.0F}, 2.0F), "Spacing must emit exactly at the threshold.");
			std::array<std::uint8_t, kWidth * kHeight> mask{}; mask[12] = 255U;
			TerrainBrushSettings settings; settings.mode = TerrainBrushMode::Raise; settings.radiusSamples = 2.0F; settings.strengthMm = 100.0F; settings.pressure = 0.5F; settings.rotationRadians = 1.5707963F;
			const auto masked = Apply(settings, mask); context.Expect(masked[12] > 0 && masked[0] == 0, "Mask and bounds must restrict writes to valid selected samples.");
			settings.pressure = 0.0F; context.Expect(Apply(settings, mask)[12] == 0, "Zero pressure must suppress the real brush kernel.");
		});
	}
}
