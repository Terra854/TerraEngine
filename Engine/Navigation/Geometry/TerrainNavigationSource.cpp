// TerrainNavigationSource.cpp - checked terrain-grid conversion for navigation.

#include "Navigation/Geometry/TerrainNavigationSource.h"

#include <cmath>
#include <limits>
#include <utility>

namespace TerraEngine::Navigation {
	namespace {
		constexpr std::size_t kCancellationQuantum = 512U;

		[[nodiscard]] bool CancellationRequested(const TerrainNavigationCaptureOptions& options) noexcept {
			if(!options.cancellationRequested) return false;
			// A cancelled/failed producer must never publish a partial geometry packet.
			try {
				return options.cancellationRequested();
			} catch(...) {
				return true;
			}
		}

		[[nodiscard]] bool CheckedMultiply(std::size_t left, std::size_t right, std::size_t& result) noexcept {
			if(left != 0U && right > std::numeric_limits<std::size_t>::max() / left) return false;
			result = left * right;
			return true;
		}

		[[nodiscard]] TerrainNavigationCaptureResult RejectedTerrainCapture(
			const TerrainNavigationPublishedSamples& published, std::string reason, bool cancelled = false) {
			TerrainNavigationCaptureResult result{};
			result.cancelled = cancelled;
			result.terrainPresent = published.terrainPresent;
			result.canonicalOrigin = published.canonicalOrigin;
			result.reason = std::move(reason);
			return result;
		}
	}

	NavigationGeometrySnapshotLease TerrainNavigationCaptureResult::AcquireLease(
		NavigationGeometryLeaseLimits limits) const {
		return snapshot ? snapshot->AcquireLease(limits) : NavigationGeometrySnapshotLease{};
	}

	TerrainNavigationCaptureResult CaptureTerrainNavigationSource(
		const TerrainNavigationPublishedSamples& published,
		const TerrainNavigationCaptureOptions& options) {
		if(CancellationRequested(options)) return RejectedTerrainCapture(published, "TerrainCaptureCancelled", true);

		// Terrain support is mandatory in the engine, not mandatory in every scene.
		// A terrain-free scene must continue toward mesh/collider navigation normally.
		if(!published.terrainPresent) {
			TerrainNavigationCaptureResult result{};
			result.accepted = true;
			result.reason = "TerrainAbsent";
			result.canonicalOrigin = published.canonicalOrigin;
			return result;
		}

		if(published.descriptor.sourceKind != NavigationSourceKind::Terrain
			|| published.descriptor.sourceUuid.value == TerraEngine::EMPTY_UUID
			|| published.descriptor.sourceRevision == 0U
			|| published.descriptor.canonicalPlacementRevision == 0U
			|| published.descriptor.physicalReadyRevision == 0U) {
			return RejectedTerrainCapture(published, "TerrainPublishedIdentityInvalid");
		}
		const Scene::WorldCoordinateResult normalizedOrigin = Scene::NormalizeWorldPosition(published.canonicalOrigin);
		if(!normalizedOrigin.Succeeded() || normalizedOrigin.position != published.canonicalOrigin) {
			return RejectedTerrainCapture(published, "TerrainCanonicalPlacementInvalid");
		}
		if(published.sampleColumns < 2U || published.sampleRows < 2U
			|| !std::isfinite(published.sampleSpacingMetres) || published.sampleSpacingMetres <= 0.0F) {
			return RejectedTerrainCapture(published, "TerrainGridDimensionsInvalid");
		}

		std::size_t sampleCount = 0U;
		std::size_t cellCount = 0U;
		if(!CheckedMultiply(published.sampleColumns, published.sampleRows, sampleCount)
			|| !CheckedMultiply(published.sampleColumns - 1U, published.sampleRows - 1U, cellCount)
			|| sampleCount > options.maximumInputSamples
			|| published.heights.size() != sampleCount
			|| (!published.holeMask.empty() && published.holeMask.size() != cellCount)) {
			return RejectedTerrainCapture(published, "TerrainGridBoundsInvalid");
		}
		if(sampleCount > 4U * 1024U * 1024U || cellCount > 4U * 1024U * 1024U) {
			return RejectedTerrainCapture(published, "TerrainGridExceedsGeometryBound");
		}
		for(std::size_t index = 0U; index < published.heights.size(); ++index) {
			if((index % kCancellationQuantum) == 0U && CancellationRequested(options)) {
				return RejectedTerrainCapture(published, "TerrainCaptureCancelled", true);
			}
			if(!std::isfinite(published.heights[index])) return RejectedTerrainCapture(published, "TerrainHeightNonFinite");
		}
		for(const std::uint8_t hole : published.holeMask) {
			if(hole > 1U) return RejectedTerrainCapture(published, "TerrainHoleMaskInvalid");
		}

		NavigationGeometryDocument document{};
		document.kind = NavigationGeometryPayloadKind::TriangleMesh;
		document.vertices.reserve(sampleCount);
		for(std::uint32_t row = 0U; row < published.sampleRows; ++row) {
			for(std::uint32_t column = 0U; column < published.sampleColumns; ++column) {
				const std::size_t index = static_cast<std::size_t>(row) * published.sampleColumns + column;
				if((index % kCancellationQuantum) == 0U && CancellationRequested(options)) {
					return RejectedTerrainCapture(published, "TerrainCaptureCancelled", true);
				}
				// Vertices are local to the published canonical origin. The builder applies
				// that origin only while producing its bounded shard-local provider packet.
				const double x = static_cast<double>(column) * static_cast<double>(published.sampleSpacingMetres);
				const double y = static_cast<double>(published.heights[index]);
				const double z = static_cast<double>(row) * static_cast<double>(published.sampleSpacingMetres);
				constexpr double kMaximumFloatCoordinate = static_cast<double>(std::numeric_limits<float>::max());
				if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)
					|| std::abs(x) > kMaximumFloatCoordinate || std::abs(y) > kMaximumFloatCoordinate
					|| std::abs(z) > kMaximumFloatCoordinate) {
					return RejectedTerrainCapture(published, "TerrainGridCoordinateNonFinite");
				}
				document.vertices.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
			}
		}

		document.triangles.reserve(cellCount * 2U);
		for(std::uint32_t row = 0U; row + 1U < published.sampleRows; ++row) {
			for(std::uint32_t column = 0U; column + 1U < published.sampleColumns; ++column) {
				const std::size_t cellIndex = static_cast<std::size_t>(row) * (published.sampleColumns - 1U) + column;
				if((cellIndex % kCancellationQuantum) == 0U && CancellationRequested(options)) {
					return RejectedTerrainCapture(published, "TerrainCaptureCancelled", true);
				}
				if(!published.holeMask.empty() && published.holeMask[cellIndex] != 0U) continue;
				const std::uint32_t topLeft = row * published.sampleColumns + column;
				const std::uint32_t bottomLeft = (row + 1U) * published.sampleColumns + column;
				const std::uint32_t topRight = topLeft + 1U;
				const std::uint32_t bottomRight = bottomLeft + 1U;
				document.triangles.push_back({topLeft, bottomLeft, topRight});
				document.triangles.push_back({topRight, bottomLeft, bottomRight});
			}
		}

		TerrainNavigationCaptureResult result{};
		result.accepted = true;
		result.terrainPresent = true;
		result.canonicalOrigin = published.canonicalOrigin;
		if(document.triangles.empty()) {
			result.reason = "TerrainHasNoNavigableTriangles";
			return result;
		}
		const NavigationGeometrySnapshotCreateResult created = NavigationGeometrySnapshot::Create(published.descriptor, document);
		if(!created.accepted || created.snapshot == nullptr) {
			return RejectedTerrainCapture(published, "TerrainSnapshotRejected:" + created.reason);
		}
		result.hasNavigableTriangles = true;
		result.snapshot = created.snapshot;
		return result;
	}
}
