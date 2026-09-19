// TerrainHeightFieldRecipeTests.cpp -- provider-neutral 257-grid terrain collision input.

#include "TestHarness.h"

#include "Physics/Assets/ColliderRecipe.h"
#include "Physics/Backend/IPhysicsBackend.h"
#include "Physics/Terrain/TerrainHeightFieldActiveRect.h"

#include <limits>

namespace TerraEngine::Tests
{
	void RegisterTerrainHeightFieldRecipeTests(TestRegistry& registry)
	{
		registry.Add("Terrain.HeightFieldRecipe.RequiresComplete257GridAndHoleMask", [](TestContext& context) {
			TerraEngine::Physics::TerrainHeightFieldRecipe recipe{};
			recipe.terrainAssetId = TerraEngine::GenerateUUID(); recipe.authoredGeneration = 3u; recipe.authoredHash = 44u;
			recipe.heightMillimetres.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kSampleCount, 0);
			recipe.holeBits.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0u);
			context.Expect(TerraEngine::Physics::ValidateTerrainHeightFieldRecipe(recipe).accepted,
				"Terrain collision accepts only a complete immutable 257 by 257 height grid with canonical 256-cell hole coverage.");
			recipe.heightMillimetres.pop_back();
			context.Expect(!TerraEngine::Physics::ValidateTerrainHeightFieldRecipe(recipe).accepted,
				"Truncated heightfield data must fail before it can reach a provider backend.");
		});

		registry.Add("Terrain.HeightFieldRecipe.PreservesMillimetresScaleDatumHolesAndMaterials", [](TestContext& context) {
			TerraEngine::Physics::TerrainHeightFieldRecipe recipe{};
			recipe.terrainAssetId = TerraEngine::GenerateUUID();
			recipe.areaX = -3;
			recipe.areaZ = 5;
			recipe.authoredGeneration = 8u;
			recipe.authoredHash = 91u;
			recipe.sampleSpacingMetres = 0.25f;
			recipe.heightDatumMetres = -120.5f;
			recipe.heightMillimetres.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kSampleCount, 0);
			// Adjacent one-millimetre values prove the provider-neutral recipe does
			// not quantize authored samples before the backend chooses its encoding.
			recipe.heightMillimetres.front() = -1;
			recipe.heightMillimetres[1u] = 0;
			recipe.heightMillimetres[2u] = 1;
			recipe.heightMillimetres.back() = 32'000'000;
			recipe.holeBits.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0u);
			recipe.holeBits.front() = 0x01u;
			recipe.holeBits.back() = 0x80u;
			recipe.materialIndices.assign(256u * 256u, 17u);
			recipe.materialIndices.back() = 42u;

			const auto validation = TerraEngine::Physics::ValidateTerrainHeightFieldRecipe(recipe);
			const auto providerRecipe = TerraEngine::Physics::Backend::MakeTerrainHeightFieldShapeRecipe(recipe);
			const auto& roundTrip = providerRecipe.terrainHeightField;
			context.Expect(validation.accepted && roundTrip.has_value()
				&& roundTrip->sampleSpacingMetres == 0.25f && roundTrip->heightDatumMetres == -120.5f
				&& roundTrip->heightMillimetres[0u] == -1 && roundTrip->heightMillimetres[2u] == 1
				&& roundTrip->heightMillimetres.back() == 32'000'000
				&& roundTrip->holeBits.front() == 0x01u && roundTrip->holeBits.back() == 0x80u
				&& roundTrip->materialIndices.front() == 17u && roundTrip->materialIndices.back() == 42u,
				"The provider-neutral heightfield packet must retain scale, datum, millimetre samples, cell holes, bounds, and stable material identities exactly.");

			auto invalid = recipe;
			invalid.sampleSpacingMetres = std::numeric_limits<float>::quiet_NaN();
			context.Expect(!TerraEngine::Physics::ValidateTerrainHeightFieldRecipe(invalid).accepted,
				"A non-finite collision scale must fail before provider allocation.");
			invalid = recipe;
			invalid.heightDatumMetres = std::numeric_limits<float>::infinity();
			context.Expect(!TerraEngine::Physics::ValidateTerrainHeightFieldRecipe(invalid).accepted,
				"A non-finite collision datum must fail before provider allocation.");
			invalid = recipe;
			invalid.materialIndices.pop_back();
			context.Expect(!TerraEngine::Physics::ValidateTerrainHeightFieldRecipe(invalid).accepted,
				"A partial cell-material grid must be rejected instead of being padded or misaligned.");
			invalid = recipe;
			invalid.holeBits.pop_back();
			context.Expect(!TerraEngine::Physics::ValidateTerrainHeightFieldRecipe(invalid).accepted,
				"A partial hole mask must be rejected instead of changing collision ownership at the boundary.");
		});

		registry.Add("Terrain.HeightFieldRecipe.MasksPaddedCellsWithoutErasingAuthoredHoles", [](TestContext& context) {
			TerraEngine::Physics::TerrainHeightFieldRecipe recipe{};
			recipe.terrainAssetId = TerraEngine::GenerateUUID();
			recipe.authoredGeneration = 9U;
			recipe.authoredHash = 73U;
			recipe.heightMillimetres.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kSampleCount, 0);
			recipe.holeBits.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0U);
			const auto setHole = [&recipe](std::uint32_t x, std::uint32_t z) {
				const std::uint32_t bit = z * 256U + x;
				recipe.holeBits[bit / 8U] |= static_cast<std::uint8_t>(1U << (bit % 8U));
			};
			const auto isHole = [&recipe](std::uint32_t x, std::uint32_t z) {
				const std::uint32_t bit = z * 256U + x;
				return (recipe.holeBits[bit / 8U] & (1U << (bit % 8U))) != 0U;
			};
			setHole(7U, 9U);
			const TerraEngine::Terrain::TerrainAreaActiveRect authored{0U, 0U, 129U, 129U};
			const bool masked = TerraEngine::Physics::Terrain::ApplyTerrainActiveRectToRecipe(recipe, authored);
			const auto beforeInvalid = recipe.holeBits;
			const TerraEngine::Terrain::TerrainAreaActiveRect invalid{0U, 0U, 257U, 129U};
			const bool rejectedWithoutMutation = !TerraEngine::Physics::Terrain::ApplyTerrainActiveRectToRecipe(recipe, invalid)
				&& recipe.holeBits == beforeInvalid;

			context.Expect(masked && !isHole(0U, 0U) && !isHole(127U, 127U)
				&& isHole(7U, 9U) && isHole(128U, 0U) && isHole(0U, 128U)
				&& isHole(255U, 255U) && rejectedWithoutMutation,
				"A 129-sample authored edge must retain its 128 by 128 cells and authored holes while every padded fixed-grid cell is masked atomically.");
		});
	}
}
