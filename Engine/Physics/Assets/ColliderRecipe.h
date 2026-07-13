// ColliderRecipe.h - normalized, provider-neutral input for collider cooking.
//
// A recipe describes only stable project data. It intentionally contains no
// provider handle, provider enum, or serialized provider memory, so it can be
// validated on workers and persisted across provider upgrades.

#pragma once

#include "Core/Export.h"
#include "Physics/PhysicsTypes.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Physics
{
	inline constexpr std::uint32_t kColliderRecipeSchemaVersion = 1U;
	inline constexpr std::string_view kColliderRecipeExtension = ".tecollider";
	inline constexpr std::size_t kColliderRecipeMaximumDiagnostics = 16U;

	// Limits are kept in one value so editor, headless, and runtime callers use
	// the same bounded input contract before provider work begins.
	struct ColliderRecipeLimits
	{
		std::size_t maximumVertices = 1'000'000U;
		std::size_t maximumIndices = 3'000'000U;
		std::size_t maximumMaterialSlots = 256U;
		std::size_t maximumCompoundChildren = 256U;
		std::size_t maximumCompoundDepth = 8U;
		std::size_t maximumSelectedSubmeshLength = 256U;
		std::size_t maximumPlatformProfileLength = 128U;
		float maximumCoordinateMagnitude = 1'000'000.0F;
		float degenerateEpsilon = 0.00001F;
	};

	enum class ColliderRecipeKind : std::uint8_t
	{
		ConvexHull,
		TriangleMesh,
		Compound
	};

	enum class ColliderRecipeValidationCode : std::uint8_t
	{
		None,
		UnsupportedSchema,
		UnsupportedKind,
		MissingSourceAsset,
		MissingSourceHash,
		InvalidProvenance,
		InvalidScale,
		NonFiniteGeometry,
		CoordinateLimitExceeded,
		VertexLimitExceeded,
		IndexLimitExceeded,
		MaterialSlotLimitExceeded,
		CompoundChildLimitExceeded,
		CompoundRecursionLimitExceeded,
		CompoundCycle,
		MissingCompoundChild,
		DuplicateCompoundChildId,
		InvalidCompoundPose,
		InvalidConvexGeometry,
		InvalidMeshGeometry,
		DegenerateGeometry,
		InvalidIndex,
		InvalidMaterialMapping,
		InvalidCookSettings
	};

	struct ColliderRecipeValidationResult
	{
		bool accepted = false;
		ColliderRecipeValidationCode code = ColliderRecipeValidationCode::None;
		std::string message;
		std::size_t location = 0U;
	};

	// Source identifiers make a recipe repairable after assets move or are
	// renamed. Source hashes bind a published cook to immutable source content.
	struct ColliderRecipeProvenance
	{
		TerraEngine::UUID sourceAssetId = TerraEngine::EMPTY_UUID;
		std::uint64_t sourceContentHash = 0U;
		std::uint64_t settingsHash = 0U;
		std::string selectedSubmesh;
		std::string platformProfile;
	};

	// These settings are engine-owned rather than provider knobs. A backend may
	// translate them, but it must not add provider state to the recipe format.
	struct ColliderRecipeCookSettings
	{
		float weldTolerance = 0.0001F;
		float simplificationRatio = 1.0F;
		std::uint32_t maximumConvexVertices = 256U;
		bool weldVertices = true;
	};

	struct ColliderRecipeMaterialSlot
	{
		std::uint32_t sourceMaterialSlot = 0U;
		TerraEngine::UUID materialAssetId = TerraEngine::EMPTY_UUID;
	};

	struct ColliderRecipe;

	// A compound child carries its stable authored identity. The shared immutable
	// recipe reference expresses a tree without exposing mutable provider shapes.
	struct ColliderRecipeCompoundChild
	{
		TerraEngine::UUID childId = TerraEngine::EMPTY_UUID;
		PhysicsPose localPose;
		std::shared_ptr<const ColliderRecipe> recipe;
	};

	// This is a value record while authoring. Callers publish only the result of
	// TryNormalize, then treat that result as immutable shared worker input.
	struct ColliderRecipe
	{
		std::uint32_t schemaVersion = kColliderRecipeSchemaVersion;
		ColliderRecipeKind kind = ColliderRecipeKind::ConvexHull;
		ColliderRecipeProvenance provenance;
		ColliderRecipeCookSettings cookSettings;
		PhysicsVector3 sourceScale{1.0F, 1.0F, 1.0F};
		std::vector<PhysicsVector3> vertices;
		std::vector<std::uint32_t> indices;
		std::vector<ColliderRecipeMaterialSlot> materialSlots;
		std::vector<std::uint32_t> triangleMaterialSlots;
		std::vector<ColliderRecipeCompoundChild> compoundChildren;

		[[nodiscard]] ColliderRecipeValidationResult Validate(
			const ColliderRecipeLimits& limits = {}) const;
		[[nodiscard]] bool TryNormalize(
			ColliderRecipe& normalized,
			ColliderRecipeValidationResult& validation,
			const ColliderRecipeLimits& limits = {}) const;
		// JSON is restricted to engine-owned schema keys. The opaque provider
		// payload belongs only in CookedColliderAsset, never in this recipe file.
		[[nodiscard]] nlohmann::json ToJson() const;
		[[nodiscard]] static bool LoadJson(
			const nlohmann::json& source,
			ColliderRecipe& destination,
			ColliderRecipeValidationResult& validation,
			const ColliderRecipeLimits& limits = {});
		// Zero means validation failed. Successful hashes are canonical FNV-1a
		// hashes of normalized project-owned fields, never provider cooked bytes.
		[[nodiscard]] std::uint64_t StableHash(const ColliderRecipeLimits& limits = {}) const;
	};

	TERRAENGINE_API std::string_view ToString(ColliderRecipeKind kind);
	TERRAENGINE_API std::string_view ToString(ColliderRecipeValidationCode code);
}
