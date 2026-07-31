// ImplementationCheck tests exercise the public provider-neutral recipe contract.
#include "../TestHarness.h"
#include "Physics/Assets/ColliderRecipe.h"

#include <cmath>
#include <memory>
#include <limits>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Physics;

	namespace
	{
		TerraEngine::UUID Id(const char* text)
		{
			return *TerraEngine::ParseUUID(text);
		}

		ColliderRecipe ValidConvex()
		{
			ColliderRecipe recipe;
			recipe.kind = ColliderRecipeKind::ConvexHull;
			recipe.provenance.sourceAssetId = Id("11111111111111111111111111111111");
			recipe.provenance.sourceContentHash = 17U;
			recipe.provenance.settingsHash = 23U;
			recipe.sourceScale = {1.0F, 1.0F, 1.0F};
			recipe.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
			return recipe;
		}

		void ExpectCode(TestContext& context, const ColliderRecipe& recipe, ColliderRecipeValidationCode code)
		{
			const auto result = recipe.Validate();
			context.Expect(!result.accepted, "Invalid recipe must be rejected.");
			context.ExpectEqual(static_cast<int>(code), static_cast<int>(result.code), "Recipe rejection code must identify the cause.");
		}
	}

	void RegisterColliderRecipeTests(TestRegistry& registry)
	{
		registry.Add("Physics.ColliderRecipe.NormalizeAndStableHash", [](TestContext& context) {
			ColliderRecipe first = ValidConvex();
			ColliderRecipe second = first;
			second.vertices = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0, 0, 0}};
			ColliderRecipe normalized;
			ColliderRecipeValidationResult validation;
			context.Expect(first.TryNormalize(normalized, validation), "Valid convex recipe must normalize.");
			context.ExpectEqual(first.StableHash(), second.StableHash(), "Equivalent normalized geometry must have a stable hash.");
			context.Expect(first.StableHash() != 0U, "Valid recipe hash must be nonzero.");
		});

		registry.Add("Physics.ColliderRecipe.ScaleAndGeometryValidation", [](TestContext& context) {
			ColliderRecipe recipe = ValidConvex();
			recipe.sourceScale = {1.0F, 2.0F, 1.0F};
			ExpectCode(context, recipe, ColliderRecipeValidationCode::InvalidScale);
			recipe = ValidConvex(); recipe.vertices[1].x = std::numeric_limits<float>::quiet_NaN();
			ExpectCode(context, recipe, ColliderRecipeValidationCode::NonFiniteGeometry);
			recipe = ValidConvex(); recipe.vertices = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}};
			ExpectCode(context, recipe, ColliderRecipeValidationCode::DegenerateGeometry);
			recipe = ValidConvex(); recipe.indices = {0, 1, 99};
			ExpectCode(context, recipe, ColliderRecipeValidationCode::InvalidConvexGeometry);
		});

		registry.Add("Physics.ColliderRecipe.LimitsAndMaterialMapping", [](TestContext& context) {
			ColliderRecipe recipe = ValidConvex();
			recipe.vertices.push_back({1.0F, 1.0F, 1.0F});
			recipe.cookSettings.maximumConvexVertices = 4U;
			ColliderRecipeLimits limits; limits.maximumVertices = 4U;
			const auto result = recipe.Validate(limits);
			context.ExpectEqual(static_cast<int>(ColliderRecipeValidationCode::VertexLimitExceeded), static_cast<int>(result.code), "Vertex limit must be enforced.");
			recipe = ValidConvex(); recipe.kind = ColliderRecipeKind::TriangleMesh;
			recipe.indices = {0U, 1U, 2U};
			recipe.materialSlots.push_back({2U, Id("22222222222222222222222222222222")});
			recipe.triangleMaterialSlots = {2U};
			context.Expect(recipe.Validate().accepted, "Valid material slot mappings must be accepted.");
			recipe.triangleMaterialSlots = {1U};
			ExpectCode(context, recipe, ColliderRecipeValidationCode::InvalidMaterialMapping);
		});

		registry.Add("Physics.ColliderRecipe.CompoundSafety", [](TestContext& context) {
			ColliderRecipe child = ValidConvex();
			ColliderRecipe compound; compound.kind = ColliderRecipeKind::Compound; compound.provenance = child.provenance;
			compound.compoundChildren.push_back({Id("33333333333333333333333333333333"), {}, std::make_shared<const ColliderRecipe>(child)});
			context.Expect(compound.Validate().accepted, "Compound with a valid child must validate.");
			compound.compoundChildren.push_back(compound.compoundChildren.front());
			ExpectCode(context, compound, ColliderRecipeValidationCode::DuplicateCompoundChildId);
			compound.compoundChildren.resize(1U);
			compound.compoundChildren[0].localPose.rotation = {0.0F, 0.0F, 0.0F, 0.0F};
			ExpectCode(context, compound, ColliderRecipeValidationCode::InvalidCompoundPose);
			compound.compoundChildren[0].localPose.rotation.w = 1.0F;
			ColliderRecipeLimits limits; limits.maximumCompoundChildren = 0U;
			context.ExpectEqual(static_cast<int>(ColliderRecipeValidationCode::CompoundChildLimitExceeded), static_cast<int>(compound.Validate(limits).code), "Compound child limit must be enforced.");
			ColliderRecipe cyclic = ValidConvex();
			cyclic.kind = ColliderRecipeKind::Compound;
			cyclic.vertices.clear();
			cyclic.compoundChildren.clear();
			auto cycle = std::make_shared<ColliderRecipe>(); *cycle = cyclic;
			cycle->compoundChildren.push_back({Id("44444444444444444444444444444444"), {}, cycle});
			context.ExpectEqual(static_cast<int>(ColliderRecipeValidationCode::CompoundCycle), static_cast<int>(cycle->Validate().code), "Compound cycles must be rejected.");
		});
	}
}
