// TerrainNavigationSource.h - immutable capture of physically published terrain.
//
// Terrain is a required navigation capability, but an individual scene may have
// no terrain source. This boundary therefore turns a published value snapshot
// into provider-neutral triangles; it never reads renderer resources or live
// TerrainComponent storage.

#pragma once

#include "Navigation/Geometry/NavigationGeometrySnapshot.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Navigation {
	// This is a copied value packet published by the terrain/physics boundary.
	// `heights` is row-major and `holeMask` is row-major per grid cell, where one
	// non-zero byte removes both triangles from that cell before navigation sees it.
	struct TerrainNavigationPublishedSamples {
		bool terrainPresent = false;
		NavigationGeometrySnapshotDescriptor descriptor{};
		Scene::WorldPosition canonicalOrigin{};
		std::uint32_t sampleColumns = 0U;
		std::uint32_t sampleRows = 0U;
		float sampleSpacingMetres = 0.0F;
		std::vector<float> heights;
		std::vector<std::uint8_t> holeMask;
	};

	struct TerrainNavigationCaptureOptions {
		// The caller supplies an immutable, already-published packet. This callback
		// lets shared worker preparation stop between bounded conversion quanta.
		std::function<bool()> cancellationRequested;
		std::size_t maximumInputSamples = 4U * 1024U * 1024U;
	};

	struct TerrainNavigationCaptureResult {
		bool accepted = false;
		bool cancelled = false;
		bool terrainPresent = false;
		bool hasNavigableTriangles = false;
		std::string reason;
		Scene::WorldPosition canonicalOrigin{};
		std::shared_ptr<const NavigationGeometrySnapshot> snapshot;

		[[nodiscard]] NavigationGeometrySnapshotLease AcquireLease(
			NavigationGeometryLeaseLimits limits = {}) const;
	};

	// Builds a detached immutable snapshot. A terrain-free scene is successful
	// with `terrainPresent == false`; a terrain made entirely of holes is likewise
	// successful but publishes no walkable triangles. Both cases remain explicit so
	// higher layers can combine terrain with mesh/collider sources without guessing.
	TERRAENGINE_API TerrainNavigationCaptureResult CaptureTerrainNavigationSource(
		const TerrainNavigationPublishedSamples& published,
		const TerrainNavigationCaptureOptions& options = {});
}
