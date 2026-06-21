#include "TestHarness.h"

#include "Scene/SpatialBounds.h"

#include <cmath>
#include <limits>
#include <optional>

namespace TerraEngine::Tests {
	namespace {
		bool NearlyEqual(float left, float right, float epsilon = 0.001f) {
			return std::fabs(left - right) <= epsilon;
		}

		bool NearlyEqualVec3(const glm::vec3& left, const glm::vec3& right, float epsilon = 0.001f) {
			return NearlyEqual(left.x, right.x, epsilon)
				&& NearlyEqual(left.y, right.y, epsilon)
				&& NearlyEqual(left.z, right.z, epsilon);
		}
	}

	void RegisterSpatialBoundsTests(TestRegistry& registry) {
		using namespace TerraEngine::Scene;

		registry.Add("Engine.SpatialBounds.MakeAABBNormalizesCornersAndRejectsNonFiniteInput", [](TestContext& context) {
			const SpatialAABB bounds = MakeAABB(glm::vec3(3.0f, 2.0f, 1.0f), glm::vec3(-1.0f, 4.0f, -2.0f), SpatialBoundsSource::Authored);
			const SpatialAABB invalid = MakeAABB(
				glm::vec3(0.0f, std::numeric_limits<float>::infinity(), 0.0f),
				glm::vec3(1.0f),
				SpatialBoundsSource::Mesh);

			context.Expect(bounds.state == SpatialBoundsState::Valid, "Finite authored bounds should be marked valid.");
			context.Expect(NearlyEqualVec3(bounds.min, glm::vec3(-1.0f, 2.0f, -2.0f)), "MakeAABB should normalize inverted minimum corners.");
			context.Expect(NearlyEqualVec3(bounds.max, glm::vec3(3.0f, 4.0f, 1.0f)), "MakeAABB should normalize inverted maximum corners.");
			context.Expect(invalid.state == SpatialBoundsState::Invalid, "Non-finite bounds should be rejected.");
			context.Expect(invalid.source == SpatialBoundsSource::Mesh, "Invalid bounds should preserve the source that produced them.");
		});

		registry.Add("Engine.SpatialBounds.FallbackMergeAndExpandPreserveUsability", [](TestContext& context) {
			SpatialAABB fallback = MakeFallbackBounds(glm::vec3(1.0f), 2.0f);
			const SpatialAABB invalidFallback = MakeFallbackBounds(glm::vec3(0.0f), -1.0f);
			const SpatialAABB authored = MakeAABB(glm::vec3(-4.0f, -1.0f, -1.0f), glm::vec3(-2.0f, 1.0f, 1.0f), SpatialBoundsSource::Authored);

			Expand(fallback, 0.5f);
			const SpatialAABB merged = Merge(fallback, authored);
			const SpatialAABB ignoredInvalid = Merge(MakeInvalidBounds(), authored);

			context.Expect(fallback.state == SpatialBoundsState::Fallback, "Fallback bounds should remain marked as fallback after expansion.");
			context.Expect(NearlyEqualVec3(fallback.min, glm::vec3(-1.5f)), "Expand should pad the minimum corner of usable bounds.");
			context.Expect(NearlyEqualVec3(fallback.max, glm::vec3(3.5f)), "Expand should pad the maximum corner of usable bounds.");
			context.Expect(invalidFallback.state == SpatialBoundsState::Invalid, "Negative fallback radii should produce invalid bounds.");
			context.Expect(merged.state == SpatialBoundsState::Fallback, "Merging with fallback bounds should keep the aggregate fallback state.");
			context.Expect(NearlyEqualVec3(merged.min, glm::vec3(-4.0f, -1.5f, -1.5f)), "Merged bounds should cover both input boxes.");
			context.Expect(NearlyEqualVec3(ignoredInvalid.min, authored.min) && NearlyEqualVec3(ignoredInvalid.max, authored.max), "Merging an invalid left side should return the usable right side.");
		});

		registry.Add("Engine.SpatialBounds.TransformAABBAppliesMatrixAndRejectsInvalidTransforms", [](TestContext& context) {
			const SpatialAABB local = MakeAABB(glm::vec3(-1.0f), glm::vec3(1.0f), SpatialBoundsSource::Mesh);
			const glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 4.0f))
				* glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 0.5f));
			glm::mat4 invalidTransform(1.0f);
			invalidTransform[3][0] = std::numeric_limits<float>::infinity();

			const SpatialAABB transformed = TransformAABB(local, transform, 0.0f);
			const SpatialAABB invalid = TransformAABB(local, invalidTransform, 0.0f);

			context.Expect(transformed.state == SpatialBoundsState::Valid, "A finite transform should keep usable bounds valid.");
			context.Expect(NearlyEqualVec3(transformed.min, glm::vec3(0.0f, 2.0f, 3.5f)), "TransformAABB should calculate the transformed minimum corner.");
			context.Expect(NearlyEqualVec3(transformed.max, glm::vec3(4.0f, 4.0f, 4.5f)), "TransformAABB should calculate the transformed maximum corner.");
			context.Expect(invalid.state == SpatialBoundsState::Invalid, "Non-finite transforms should invalidate transformed bounds.");
		});

		registry.Add("Engine.SpatialBounds.DirtyTransformRecomputesWorldBoundsFromLatestMatrix", [](TestContext& context) {
			const SpatialAABB local = MakeAABB(glm::vec3(-0.5f), glm::vec3(0.5f), SpatialBoundsSource::Mesh);
			const glm::mat4 first = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			const glm::mat4 second = glm::translate(glm::mat4(1.0f), glm::vec3(8.0f, 0.0f, 0.0f));

			const SpatialAABB firstWorld = TransformAABB(local, first, 0.0f);
			const SpatialAABB secondWorld = TransformAABB(local, second, 0.0f);

			context.Expect(NearlyEqualVec3(firstWorld.Center(), glm::vec3(1.0f, 0.0f, 0.0f)), "Initial bounds should use the first world transform.");
			context.Expect(NearlyEqualVec3(secondWorld.Center(), glm::vec3(8.0f, 0.0f, 0.0f)), "Dirty transform updates should recompute bounds from the latest world transform.");
		});

		registry.Add("Engine.SpatialBounds.IntersectionHelpersHandleHitsMissesAndFrustumPlanes", [](TestContext& context) {
			const SpatialAABB box = MakeAABB(glm::vec3(1.0f, -1.0f, -1.0f), glm::vec3(3.0f, 1.0f, 1.0f), SpatialBoundsSource::Authored);
			const SpatialSphere touchingSphere{glm::vec3(0.0f), 1.0f};
			const SpatialSphere missedSphere{glm::vec3(-3.0f, 0.0f, 0.0f), 1.0f};
			const SpatialRay hitRay{glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 10.0f};
			const SpatialRay missedRay{glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 10.0f};
			const SpatialFrustum identityFrustum = ExtractFrustum(glm::mat4(1.0f));
			const SpatialAABB insideClipSpace = MakeAABB(glm::vec3(-0.25f, -0.25f, 0.25f), glm::vec3(0.25f, 0.25f, 0.75f), SpatialBoundsSource::Authored);
			const SpatialAABB outsideClipSpace = MakeAABB(glm::vec3(2.0f, -0.25f, 0.25f), glm::vec3(3.0f, 0.25f, 0.75f), SpatialBoundsSource::Authored);

			const std::optional<float> hitDistance = Intersects(hitRay, box);
			const std::optional<float> missDistance = Intersects(missedRay, box);

			context.Expect(Intersects(touchingSphere, box), "Sphere/AABB tests should count touching volumes as intersections.");
			context.Expect(!Intersects(missedSphere, box), "Sphere/AABB tests should reject separated volumes.");
			context.Expect(hitDistance.has_value() && NearlyEqual(*hitDistance, 1.0f), "Ray/AABB tests should return the entry distance for a hit.");
			context.Expect(!missDistance.has_value(), "Ray/AABB tests should reject parallel rays outside an axis slab.");
			context.Expect(Intersects(identityFrustum, insideClipSpace), "Extracted identity frustum planes should contain clip-space boxes inside the volume.");
			context.Expect(!Intersects(identityFrustum, outsideClipSpace), "Extracted identity frustum planes should reject boxes outside the volume.");
		});
	}
}
