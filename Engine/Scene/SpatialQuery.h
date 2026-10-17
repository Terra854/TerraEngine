// SpatialQuery.h — Spatial Query Data Structures
//
// Defines the types and filter structures used to perform spatial queries against the scene.
// It supports various query shapes such as frustums, spheres, and raycasts for visibility and picking.
// The results are used by the renderer for culling and by the editor for object selection.

#pragma once

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "Scene/SpatialBounds.h"
#include "Utilities/UUID.h"

namespace TerraEngine::Scene
{
	// SpatialObjectCategory names the supported states explicitly so serialization and switches stay readable.
	enum SpatialObjectCategory : std::uint32_t
	{
		SpatialCategoryNone = 0u,
		SpatialCategoryRenderable = 1u << 0,
		SpatialCategoryEditorSelectable = 1u << 1,
		SpatialCategoryLightAffecting = 1u << 2,
		SpatialCategoryDiagnostic = 1u << 3,
		SpatialCategoryAll = 0xffffffffu
	};

	// SpatialSortMode names the supported states explicitly so serialization and switches stay readable.
	enum class SpatialSortMode : std::uint8_t
	{
		StableIdentity,
		DistanceAscending
	};


	struct SpatialObjectRecord
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		entt::entity entity = entt::null;
		bool active = true;
		bool isStatic = false;
		std::uint32_t categoryMask = SpatialCategoryRenderable;
		// Exact ownership remains canonical; localBounds are valid only in the
		// index reference frame identified below and are never global floats.
		WorldPosition canonicalOwner{};
		SpatialAABB localBounds{};
		std::uint64_t frameGeneration = 0;
		std::uint64_t revision = 0;
	};


	struct SpatialQueryFilter
	{
		bool includeInactive = false;
		std::uint32_t categoryMask = SpatialCategoryAll;
		SpatialSortMode sortMode = SpatialSortMode::StableIdentity;
		std::uint32_t maxResults = 0;
		bool collectDiagnostics = false;
	};


	struct SpatialQueryHit
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		entt::entity entity = entt::null;
		SpatialAABB bounds{};
		WorldPosition canonicalOwner{};
		std::uint64_t frameGeneration = 0;
		float distance = 0.0f;
		bool fallback = false;
	};


	struct SpatialQueryResult
	{
		std::vector<SpatialQueryHit> hits;
		std::uint32_t rejectedCount = 0;
		std::uint32_t fallbackCount = 0;
		bool validInput = true;
		WorldCoordinateError coordinateError = WorldCoordinateError::None;
	};
}
