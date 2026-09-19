// RecastGroundBuilder.cpp - private, checked Recast tile production.

#include "Navigation/Backend/RecastGroundBuilder.h"

// Provider types stay private. Public callers receive copied bytes and
// engine-owned identities only.
#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <Recast.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace TerraEngine::Navigation {
	namespace {
		constexpr unsigned short kGroundPolygonFlag = 1U;
		constexpr unsigned char kGroundArea = 0U;
		struct HeightfieldDeleter { void operator()(rcHeightfield* value) const noexcept { rcFreeHeightField(value); } };
		struct CompactDeleter { void operator()(rcCompactHeightfield* value) const noexcept { rcFreeCompactHeightfield(value); } };
		struct ContourDeleter { void operator()(rcContourSet* value) const noexcept { rcFreeContourSet(value); } };
		struct PolyMeshDeleter { void operator()(rcPolyMesh* value) const noexcept { rcFreePolyMesh(value); } };
		struct DetailMeshDeleter { void operator()(rcPolyMeshDetail* value) const noexcept { rcFreePolyMeshDetail(value); } };

		[[nodiscard]] std::uint64_t Mix(std::uint64_t hash, const void* bytes, std::size_t size) noexcept {
			const auto* data = static_cast<const unsigned char*>(bytes);
			for(std::size_t index = 0U; index < size; ++index) {
				hash ^= data[index];
				hash *= 1099511628211ULL;
			}
			return hash;
		}
		template<typename Value> void MixValue(std::uint64_t& hash, const Value& value) noexcept { hash = Mix(hash, &value, sizeof(value)); }
		[[nodiscard]] bool Cancelled(const RecastGroundBuildInput& input) noexcept {
			try { return input.cancellationRequested && input.cancellationRequested(); }
			catch(...) { return true; }
		}
		[[nodiscard]] std::uint64_t Checksum(const std::vector<std::byte>& bytes) noexcept {
			return Mix(14695981039346656037ULL, bytes.data(), bytes.size());
		}
		[[nodiscard]] std::uint64_t FingerprintSources(std::span<const std::uint64_t> fingerprints) noexcept {
			// Persist source IDs as little-endian bytes so a cache descriptor is stable
			// across hosts that share the same immutable captured geometry.
			std::uint64_t hash = 14695981039346656037ULL;
			for(const std::uint64_t fingerprint : fingerprints) {
				for(std::size_t byte = 0U; byte < sizeof(fingerprint); ++byte) {
					hash ^= static_cast<unsigned char>((fingerprint >> (byte * 8U)) & 0xffU);
					hash *= 1099511628211ULL;
				}
			}
			return hash;
		}

		[[nodiscard]] std::uint64_t FingerprintSourceSemantics(
			const NavigationGeometrySnapshot& source) noexcept {
			// Content alone is insufficient: the same triangles are walkable support
			// for terrain/mesh input but an obstruction for a collider. Do not add
			// mutable revision IDs here; reload-compatible cache identity is content
			// plus this stable interpretation role only.
			std::uint64_t hash = 14695981039346656037ULL;
			const std::uint64_t content = source.ContentFingerprint();
			const std::uint8_t sourceKind = static_cast<std::uint8_t>(source.Descriptor().sourceKind);
			const Scene::WorldPosition& frame = source.Descriptor().canonicalFrameOrigin;
			hash = Mix(hash, &content, sizeof(content));
			hash = Mix(hash, &sourceKind, sizeof(sourceKind));
			// The same local mesh at another canonical cell is a different source for
			// tile coverage and saved-cache validation. Hash fields individually to
			// avoid compiler padding becoming persisted identity.
			hash = Mix(hash, &frame.cell.x, sizeof(frame.cell.x));
			hash = Mix(hash, &frame.cell.y, sizeof(frame.cell.y));
			hash = Mix(hash, &frame.cell.z, sizeof(frame.cell.z));
			hash = Mix(hash, &frame.offsetMetres.x, sizeof(frame.offsetMetres.x));
			hash = Mix(hash, &frame.offsetMetres.y, sizeof(frame.offsetMetres.y));
			return Mix(hash, &frame.offsetMetres.z, sizeof(frame.offsetMetres.z));
		}

		[[nodiscard]] std::int64_t FloorDivide(std::int64_t value, std::int64_t divisor) noexcept {
			const std::int64_t quotient = value / divisor;
			const std::int64_t remainder = value % divisor;
			return remainder < 0 ? quotient - 1LL : quotient;
		}

		[[nodiscard]] std::optional<std::int64_t> CanonicalTileCoordinate(
			std::int64_t cell, double metres) noexcept {
			constexpr std::int64_t kTilesPerCell = static_cast<std::int64_t>(
				Scene::kWorldCellEdgeMetres / kGroundTileEdgeMetres);
			if(!std::isfinite(metres) || cell > std::numeric_limits<std::int64_t>::max() / kTilesPerCell
				|| cell < std::numeric_limits<std::int64_t>::min() / kTilesPerCell) return std::nullopt;
			const double localDouble = std::floor(metres / kGroundTileEdgeMetres);
			if(localDouble < static_cast<double>(std::numeric_limits<std::int64_t>::min())
				|| localDouble > static_cast<double>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
			const std::int64_t local = static_cast<std::int64_t>(localDouble);
			const std::int64_t scaled = cell * kTilesPerCell;
			if((local > 0 && scaled > std::numeric_limits<std::int64_t>::max() - local)
				|| (local < 0 && scaled < std::numeric_limits<std::int64_t>::min() - local)) return std::nullopt;
			return scaled + local;
		}

		[[nodiscard]] std::optional<std::int64_t> CanonicalVerticalBand(
			std::int64_t cell, double metres) noexcept {
			constexpr std::int64_t kBandsPerCell = static_cast<std::int64_t>(
				Scene::kWorldCellEdgeMetres / kGroundVerticalBandMetres);
			if(!std::isfinite(metres) || cell > std::numeric_limits<std::int64_t>::max() / kBandsPerCell
				|| cell < std::numeric_limits<std::int64_t>::min() / kBandsPerCell) return std::nullopt;
			const double localDouble = std::floor(metres / kGroundVerticalBandMetres);
			if(localDouble < static_cast<double>(std::numeric_limits<std::int64_t>::min())
				|| localDouble > static_cast<double>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
			const std::int64_t local = static_cast<std::int64_t>(localDouble);
			const std::int64_t scaled = cell * kBandsPerCell;
			if((local > 0 && scaled > std::numeric_limits<std::int64_t>::max() - local)
				|| (local < 0 && scaled < std::numeric_limits<std::int64_t>::min() - local)) return std::nullopt;
			return scaled + local;
		}

		[[nodiscard]] Scene::WorldPosition ShardOrigin(
			std::int64_t shardX, std::int64_t shardZ, std::int32_t verticalBand) noexcept {
			Scene::WorldPosition origin{};
			origin.cell.x = FloorDivide(shardX, 256LL);
			origin.cell.z = FloorDivide(shardZ, 256LL);
			origin.cell.y = FloorDivide(static_cast<std::int64_t>(verticalBand), 512LL);
			origin.offsetMetres.x = static_cast<double>(shardX - origin.cell.x * 256LL) * 256.0;
			origin.offsetMetres.z = static_cast<double>(shardZ - origin.cell.z * 256LL) * 256.0;
			origin.offsetMetres.y = static_cast<double>(static_cast<std::int64_t>(verticalBand)
				- origin.cell.y * 512LL) * kGroundVerticalBandMetres;
			return origin;
		}


		void AppendTriangle(std::vector<float>& vertices, std::vector<int>& triangles,
			std::vector<unsigned char>& obstructionTriangles, NavigationGeometryVector3 first,
			NavigationGeometryVector3 second, NavigationGeometryVector3 third, bool obstruction) {
			const int base = static_cast<int>(vertices.size() / 3U);
			vertices.insert(vertices.end(), {first.x, first.y, first.z, second.x, second.y, second.z, third.x, third.y, third.z});
			triangles.insert(triangles.end(), {base, base + 1, base + 2});
			obstructionTriangles.push_back(obstruction ? 1U : 0U);
		}

		void AppendBox(std::vector<float>& vertices, std::vector<int>& triangles,
			std::vector<unsigned char>& obstructionTriangles,
			const NavigationGeometryPrimitive& box, bool obstruction) {
			const auto point = [&box](float x, float y, float z) {
				return NavigationGeometryVector3{box.center.x + x * box.halfExtents.x,
					box.center.y + y * box.halfExtents.y, box.center.z + z * box.halfExtents.z};
			};
			const auto p000 = point(-1, -1, -1), p001 = point(-1, -1, 1);
			const auto p010 = point(-1, 1, -1), p011 = point(-1, 1, 1);
			const auto p100 = point(1, -1, -1), p101 = point(1, -1, 1);
			const auto p110 = point(1, 1, -1), p111 = point(1, 1, 1);
			// Final clearance checks retain the analytic box; these faces provide
			// conservative voxel support and obstruction during Recast generation.
			AppendTriangle(vertices, triangles, obstructionTriangles, p010, p111, p110, obstruction); AppendTriangle(vertices, triangles, obstructionTriangles, p010, p011, p111, obstruction);
			AppendTriangle(vertices, triangles, obstructionTriangles, p000, p100, p101, obstruction); AppendTriangle(vertices, triangles, obstructionTriangles, p000, p101, p001, obstruction);
			AppendTriangle(vertices, triangles, obstructionTriangles, p001, p101, p111, obstruction); AppendTriangle(vertices, triangles, obstructionTriangles, p001, p111, p011, obstruction);
			AppendTriangle(vertices, triangles, obstructionTriangles, p000, p010, p110, obstruction); AppendTriangle(vertices, triangles, obstructionTriangles, p000, p110, p100, obstruction);
			AppendTriangle(vertices, triangles, obstructionTriangles, p100, p110, p111, obstruction); AppendTriangle(vertices, triangles, obstructionTriangles, p100, p111, p101, obstruction);
			AppendTriangle(vertices, triangles, obstructionTriangles, p000, p001, p011, obstruction); AppendTriangle(vertices, triangles, obstructionTriangles, p000, p011, p010, obstruction);
		}

		[[nodiscard]] bool GatherGeometryMetadata(const RecastGroundBuildInput& input,
			std::vector<std::uint64_t>& fingerprints, std::set<GroundTileKey>& keys,
			std::string& reason) {
			const std::uint64_t buildKey = MakeGroundProfileBuildKey(input.profile);
			// Both cache validation and production need the same canonical keys and
			// fingerprints, but neither consumes a global float geometry buffer. Count
			// provider vertices without allocating/copying them; only the later shard
			// localization pass materializes triangles in the provider's local frame.
			std::size_t vertexCount = 0U;
			constexpr std::size_t maximumVertices = static_cast<std::size_t>(std::numeric_limits<int>::max());
			if(input.requestedTiles.size() > input.maximumTiles) {
				reason = "CanonicalTileCountExceedsBound"; return false;
			}
			for(const GroundTileKey& requested : input.requestedTiles) {
				if(requested.profileBuildKey != buildKey) {
					reason = "RequestedTileProfileMismatch"; return false;
				}
			}
			for(const auto& source : input.sources) {
				if(Cancelled(input)) { reason = "CancelledDuringSourceEnumeration"; return false; }
				if(!source || (source->Triangles().empty() && source->Primitives().empty())) {
					reason = "SourceHasNoGeometry"; return false;
				}
				const auto& bounds = source->Bounds();
				const float maximumTileCoordinate = static_cast<float>(std::numeric_limits<int>::max()) * kGroundTileEdgeMetres;
				const float maximumBandCoordinate = static_cast<float>(std::numeric_limits<int>::max()) * kGroundVerticalBandMetres;
				if(!std::isfinite(bounds.minimum.x) || !std::isfinite(bounds.maximum.x)
					|| !std::isfinite(bounds.minimum.y) || !std::isfinite(bounds.maximum.y)
					|| !std::isfinite(bounds.minimum.z) || !std::isfinite(bounds.maximum.z)
					|| std::fabs(bounds.minimum.x) >= maximumTileCoordinate || std::fabs(bounds.maximum.x) >= maximumTileCoordinate
					|| std::fabs(bounds.minimum.z) >= maximumTileCoordinate || std::fabs(bounds.maximum.z) >= maximumTileCoordinate
					|| std::fabs(bounds.minimum.y) >= maximumBandCoordinate || std::fabs(bounds.maximum.y) >= maximumBandCoordinate) {
					reason = "SourceBoundsOutsideCanonicalTileRange"; return false;
				}
				const Scene::WorldPosition& frame = source->Descriptor().canonicalFrameOrigin;
				const auto minX = CanonicalTileCoordinate(frame.cell.x, frame.offsetMetres.x + bounds.minimum.x);
				const auto maxX = CanonicalTileCoordinate(frame.cell.x, frame.offsetMetres.x + bounds.maximum.x);
				const auto minZ = CanonicalTileCoordinate(frame.cell.z, frame.offsetMetres.z + bounds.minimum.z);
				const auto maxZ = CanonicalTileCoordinate(frame.cell.z, frame.offsetMetres.z + bounds.maximum.z);
				if(!minX || !maxX || !minZ || !maxZ) { reason = "SourceCanonicalTileAddressOverflow"; return false; }
				const auto minBand = CanonicalVerticalBand(frame.cell.y, frame.offsetMetres.y + bounds.minimum.y);
				const auto maxBand = CanonicalVerticalBand(frame.cell.y, frame.offsetMetres.y + bounds.maximum.y);
				if(!minBand || !maxBand || *minBand < std::numeric_limits<std::int32_t>::min()
					|| *maxBand > std::numeric_limits<std::int32_t>::max()) {
					reason = "SourceCanonicalVerticalBandOutOfRange"; return false;
				}
				if(input.requestedTiles.empty()) {
					for(std::int64_t band = *minBand; band <= *maxBand; ++band) for(std::int64_t z = *minZ; z <= *maxZ; ++z) for(std::int64_t x = *minX; x <= *maxX; ++x) {
						keys.insert({x, z, static_cast<std::int32_t>(band), buildKey});
						if(keys.size() > input.maximumTiles) { reason = "CanonicalTileCountExceedsBound"; return false; }
					}
				} else {
					// Select addresses, not sources: clipping the source list here could
					// omit a nearby obstacle that affects a selected tile's border halo.
					// This also bounds enumeration for a large source spanning many tiles.
					for(const GroundTileKey& requested : input.requestedTiles) {
						if(requested.x >= *minX && requested.x <= *maxX
							&& requested.z >= *minZ && requested.z <= *maxZ
							&& requested.verticalBand >= *minBand && requested.verticalBand <= *maxBand)
							keys.insert(requested);
					}
				}
				// Division before multiplication keeps even pathological source sizes
				// checked without overflowing the count or allocating a huge scratch buffer.
				if(source->Triangles().size() > (maximumVertices - vertexCount) / 3U) {
					reason = "SourceVertexCountExceedsProviderBound"; return false;
				}
				vertexCount += source->Triangles().size() * 3U;
				for(const auto& primitive : source->Primitives()) {
					if(primitive.kind != NavigationGeometryPrimitiveKind::Box) {
						reason = "UnsupportedAnalyticPrimitiveForRecast"; return false;
					}
					// AppendBox emits twelve independent triangles, or 36 vertices.
					if(vertexCount > maximumVertices - 36U) {
						reason = "SourceVertexCountExceedsProviderBound"; return false;
					}
					vertexCount += 36U;
				}
				fingerprints.push_back(FingerprintSourceSemantics(*source));
			}
			std::sort(fingerprints.begin(), fingerprints.end());
			if(keys.empty() || vertexCount == 0U) { reason = "NoCanonicalTileGeometry"; return false; }
			return true;
		}

		[[nodiscard]] std::optional<NavigationGeometryBounds> RequestedRasterBounds(
			const NavigationProfile& profile, std::span<const GroundTileKey> selectedTiles) noexcept {
			// An empty selection is the ordinary full build. Returning no bounds keeps
			// that path unchanged and avoids accidentally filtering a complete source.
			if(selectedTiles.empty()) return std::nullopt;
			// Reuse the public calculation that BuildTile and dynamic dirty admission
			// already share. This keeps source culling aligned when Recast limits change.
			const std::optional<GroundTileBuildHalo> halo = ComputeGroundTileBuildHalo(profile);
			if(!halo) return std::nullopt;
			const float horizontalHalo = static_cast<float>(halo->horizontalMetres);
			const float verticalHalo = static_cast<float>(halo->verticalMetres);
			const auto localTile = [](std::int64_t coordinate) {
				const std::int64_t remainder = coordinate % 8LL;
				return static_cast<float>(remainder < 0 ? remainder + 8LL : remainder);
			};
			NavigationGeometryBounds bounds{
				{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()},
				{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()}};
			for(const GroundTileKey& key : selectedTiles) {
				// Keep the additions in the same order as BuildTile's config.bmin/bmax;
				// using minimum + 32 + 2*halo can round a boundary differently.
				const float localX = localTile(key.x) * kGroundTileEdgeMetres;
				const float localZ = localTile(key.z) * kGroundTileEdgeMetres;
				const float minimumX = localX - horizontalHalo;
				const float minimumZ = localZ - horizontalHalo;
				const float maximumX = localX + kGroundTileEdgeMetres + horizontalHalo;
				const float maximumZ = localZ + kGroundTileEdgeMetres + horizontalHalo;
				bounds.minimum.x = std::min(bounds.minimum.x, minimumX);
				bounds.minimum.y = -verticalHalo;
				bounds.minimum.z = std::min(bounds.minimum.z, minimumZ);
				bounds.maximum.x = std::max(bounds.maximum.x, maximumX);
				bounds.maximum.y = kGroundVerticalBandMetres + verticalHalo;
				bounds.maximum.z = std::max(bounds.maximum.z, maximumZ);
			}
			return bounds;
		}

		[[nodiscard]] bool GatherShardLocalGeometry(const RecastGroundBuildInput& input,
			std::int64_t shardX, std::int64_t shardZ, std::int32_t verticalBand,
			std::vector<float>& vertices, std::vector<int>& triangles,
			std::vector<unsigned char>& obstructionTriangles,
			std::vector<NavigationGeometryBounds>& obstructionBounds,
			std::span<const GroundTileKey> selectedTiles, std::string& reason) {
			// A Detour shard is deliberately a small float island. Convert each immutable
			// source once per shard, then reuse this packet for the shard's up-to-64 tile
			// builds. No provider call can observe canonical int64 world coordinates.
			const Scene::WorldPosition origin = ShardOrigin(shardX, shardZ, verticalBand);
			constexpr double kMaximumLocalExtentMetres = 512.0;
			const std::optional<NavigationGeometryBounds> rasterBounds = RequestedRasterBounds(
				input.profile, selectedTiles);
			struct ExpandedRasterBounds {
				double minimumX = 0.0;
				double minimumY = 0.0;
				double minimumZ = 0.0;
				double maximumX = 0.0;
				double maximumY = 0.0;
				double maximumZ = 0.0;
			};
			std::optional<ExpandedRasterBounds> expandedRasterBounds;
			if(rasterBounds) {
				const auto outwardMinimum = [](float value) {
					return static_cast<double>(std::nextafter(value, -std::numeric_limits<float>::infinity()));
				};
				const auto outwardMaximum = [](float value) {
					return static_cast<double>(std::nextafter(value, std::numeric_limits<float>::infinity()));
				};
				expandedRasterBounds = ExpandedRasterBounds{
					outwardMinimum(rasterBounds->minimum.x), outwardMinimum(rasterBounds->minimum.y),
					outwardMinimum(rasterBounds->minimum.z), outwardMaximum(rasterBounds->maximum.x),
					outwardMaximum(rasterBounds->maximum.y), outwardMaximum(rasterBounds->maximum.z)};
			}
			for(const auto& source : input.sources) {
				if(Cancelled(input)) { reason = "CancelledDuringShardLocalization"; return false; }
				if(!source) { reason = "SourceHasNoGeometry"; return false; }
				const Scene::WorldLocalizationResult localized = Scene::LocalizeWorldPosition(
					source->Descriptor().canonicalFrameOrigin, origin, kMaximumLocalExtentMetres);
				// This source belongs to another shard. Its independently built packet
				// will be gathered when that shard is processed.
				if(!localized.Succeeded()) continue;
				// Primitive boxes are transformed by rounding their centers before
				// expanding half-extents. Keep such sources in the packet unless a
				// future exact primitive-bound proof is added; triangles use the exact
				// snapshot bounds below and benefit from whole-source culling.
				if(expandedRasterBounds && source->Primitives().empty()) {
					const NavigationGeometryBounds& sourceBounds = source->Bounds();
					const auto finite = [](double x, double y, double z) {
						return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
					};
					const double localMinimumX = localized.offsetMetres.x + static_cast<double>(sourceBounds.minimum.x);
					const double localMinimumY = localized.offsetMetres.y + static_cast<double>(sourceBounds.minimum.y);
					const double localMinimumZ = localized.offsetMetres.z + static_cast<double>(sourceBounds.minimum.z);
					const double localMaximumX = localized.offsetMetres.x + static_cast<double>(sourceBounds.maximum.x);
					const double localMaximumY = localized.offsetMetres.y + static_cast<double>(sourceBounds.maximum.y);
					const double localMaximumZ = localized.offsetMetres.z + static_cast<double>(sourceBounds.maximum.z);
					// Bounds are derived from every source primitive/vertex. If a value ever
					// becomes non-finite during localization, retain the source conservatively.
					if(finite(localMinimumX, localMinimumY, localMinimumZ)
						&& finite(localMaximumX, localMaximumY, localMaximumZ)
						&& (localMaximumX < expandedRasterBounds->minimumX
							|| localMinimumX > expandedRasterBounds->maximumX
							|| localMaximumY < expandedRasterBounds->minimumY
							|| localMinimumY > expandedRasterBounds->maximumY
							|| localMaximumZ < expandedRasterBounds->minimumZ
							|| localMinimumZ > expandedRasterBounds->maximumZ)) continue;
				}
				const auto point = [&localized](NavigationGeometryVector3 value) {
					return NavigationGeometryVector3{static_cast<float>(localized.offsetMetres.x + value.x),
						static_cast<float>(localized.offsetMetres.y + value.y),
						static_cast<float>(localized.offsetMetres.z + value.z)};
				};
				const auto intersectsRaster = [&expandedRasterBounds](
					NavigationGeometryVector3 first, NavigationGeometryVector3 second,
					NavigationGeometryVector3 third) {
					if(!expandedRasterBounds) return true;
					const float minimumX = std::min({first.x, second.x, third.x});
					const float minimumY = std::min({first.y, second.y, third.y});
					const float minimumZ = std::min({first.z, second.z, third.z});
					const float maximumX = std::max({first.x, second.x, third.x});
					const float maximumY = std::max({first.y, second.y, third.y});
					const float maximumZ = std::max({first.z, second.z, third.z});
					return !(static_cast<double>(maximumX) < expandedRasterBounds->minimumX
						|| static_cast<double>(minimumX) > expandedRasterBounds->maximumX
						|| static_cast<double>(maximumY) < expandedRasterBounds->minimumY
						|| static_cast<double>(minimumY) > expandedRasterBounds->maximumY
						|| static_cast<double>(maximumZ) < expandedRasterBounds->minimumZ
						|| static_cast<double>(minimumZ) > expandedRasterBounds->maximumZ);
				};
				const bool obstruction = source->Descriptor().sourceKind == NavigationSourceKind::Collider;
				for(const NavigationGeometryTriangleRecord& triangle : source->Triangles()) {
					const NavigationGeometryVector3 first = point(triangle.firstVertex);
					const NavigationGeometryVector3 second = point(triangle.secondVertex);
					const NavigationGeometryVector3 third = point(triangle.thirdVertex);
					if(!std::isfinite(first.x) || !std::isfinite(first.y) || !std::isfinite(first.z)
						|| !std::isfinite(second.x) || !std::isfinite(second.y) || !std::isfinite(second.z)
						|| !std::isfinite(third.x) || !std::isfinite(third.y) || !std::isfinite(third.z)) {
						reason = "SourceShardLocalizationNonFinite"; return false;
					}
					// Recast clips triangles against this same heightfield bounds before
					// rasterization. A conservative triangle AABB test therefore removes
					// only work that cannot contribute to the selected tiles.
					if(!intersectsRaster(first, second, third)) continue;
					AppendTriangle(vertices, triangles, obstructionTriangles, first, second, third, obstruction);
				}
				for(const NavigationGeometryPrimitive& primitive : source->Primitives()) {
					NavigationGeometryPrimitive local = primitive;
					local.center = point(primitive.center);
					if(local.kind != NavigationGeometryPrimitiveKind::Box) { reason = "UnsupportedAnalyticPrimitiveForRecast"; return false; }
					AppendBox(vertices, triangles, obstructionTriangles, local, obstruction);
					if(obstruction) obstructionBounds.push_back({
						{local.center.x - local.halfExtents.x, local.center.y - local.halfExtents.y, local.center.z - local.halfExtents.z},
						{local.center.x + local.halfExtents.x, local.center.y + local.halfExtents.y, local.center.z + local.halfExtents.z}});
				}
			}
			if(triangles.empty()) {
				// A requested partial tile may legitimately have no source intersecting
				// its exact raster bounds. Let BuildTile emit authoritative empty coverage;
				// the ordinary full-build path retains its historical failure behavior.
				if(rasterBounds) return true;
				reason = "ShardHasNoLocalizedGeometry";
				return false;
			}
			return true;
		}

		[[nodiscard]] RecastGroundBuildStatus BuildTile(const RecastGroundBuildInput& input,
			const GroundTileKey& key, const std::vector<float>& vertices,
			const std::vector<int>& triangles, const std::vector<unsigned char>& obstructionTriangles,
			const std::vector<NavigationGeometryBounds>& obstructionBounds,
			const std::vector<std::uint64_t>& fingerprints,
			GroundTileBlob& blob, std::string& reason) {
			rcConfig config{};
			// Public profiles use doubles; a positive resolution may underflow when
			// copied into Recast's floats, and derived voxel counts may exceed int.
			// Reject before any narrowing or allocation rather than casting infinity.
			const double maximumFloat = std::numeric_limits<float>::max();
			if(input.profile.horizontalResolutionMetres > maximumFloat
				|| input.profile.verticalResolutionMetres > maximumFloat
				|| input.profile.horizontalResolutionMetres < std::numeric_limits<float>::min()
				|| input.profile.verticalResolutionMetres < std::numeric_limits<float>::min()) {
				reason = "RecastGridDimensionsOutsideBound"; return RecastGroundBuildStatus::RecastConfigurationFailed;
			}
			config.cs = static_cast<float>(input.profile.horizontalResolutionMetres);
			config.ch = static_cast<float>(input.profile.verticalResolutionMetres);
			const double heightVoxels = std::ceil(input.profile.heightMetres / config.ch);
			const double climbVoxels = std::floor(input.profile.stepHeightMetres / config.ch);
			const double radiusVoxels = std::ceil((input.profile.radiusMetres + input.profile.clearanceMarginMetres) / config.cs);
			const double tileVoxels = std::ceil(static_cast<double>(kGroundTileEdgeMetres) / config.cs);
			const double gridVoxels = tileVoxels + 2.0 * (radiusVoxels + 3.0);
			const double verticalExtent = input.profile.heightMetres + input.profile.stepHeightMetres + input.profile.clearanceMarginMetres;
			if(!std::isfinite(gridVoxels) || gridVoxels <= 0.0 || gridVoxels > 4096.0
				|| !std::isfinite(heightVoxels) || heightVoxels > std::numeric_limits<int>::max()
				|| !std::isfinite(climbVoxels) || climbVoxels > std::numeric_limits<int>::max()
				|| !std::isfinite(verticalExtent) || verticalExtent > maximumFloat / 2.0) {
				reason = "RecastGridDimensionsOutsideBound"; return RecastGroundBuildStatus::RecastConfigurationFailed;
			}
			config.walkableSlopeAngle = static_cast<float>(input.profile.maxSlopeDegrees);
			config.walkableHeight = static_cast<int>(heightVoxels);
			config.walkableClimb = static_cast<int>(climbVoxels);
			config.walkableRadius = static_cast<int>(radiusVoxels);
			config.maxEdgeLen = static_cast<int>(kGroundTileEdgeMetres / config.cs);
			config.maxSimplificationError = 1.3F;
			config.minRegionArea = 64;
			config.mergeRegionArea = 400;
			config.maxVertsPerPoly = 6;
			config.tileSize = static_cast<int>(tileVoxels);
			config.borderSize = config.walkableRadius + 3;
			config.width = config.tileSize + config.borderSize * 2;
			config.height = config.tileSize + config.borderSize * 2;
			config.detailSampleDist = config.cs * 6.0F;
			config.detailSampleMaxError = config.ch;
			const float horizontalHalo = static_cast<float>(config.borderSize) * config.cs;
			const float verticalHalo = static_cast<float>(input.profile.heightMetres + input.profile.stepHeightMetres + input.profile.clearanceMarginMetres);
			const auto localTile = [](std::int64_t coordinate) {
				const std::int64_t remainder = coordinate % 8LL;
				return static_cast<float>(remainder < 0 ? remainder + 8LL : remainder);
			};
			const float localX = localTile(key.x) * kGroundTileEdgeMetres;
			const float localZ = localTile(key.z) * kGroundTileEdgeMetres;
			config.bmin[0] = localX - horizontalHalo;
			config.bmin[1] = -verticalHalo;
			config.bmin[2] = localZ - horizontalHalo;
			config.bmax[0] = localX + kGroundTileEdgeMetres + horizontalHalo;
			config.bmax[1] = kGroundVerticalBandMetres + verticalHalo;
			config.bmax[2] = localZ + kGroundTileEdgeMetres + horizontalHalo;
			if(config.width <= 0 || config.height <= 0 || config.width > 4096 || config.height > 4096) {
				reason = "RecastGridDimensionsOutsideBound"; return RecastGroundBuildStatus::RecastConfigurationFailed;
			}

			rcContext context(false);
			std::unique_ptr<rcHeightfield, HeightfieldDeleter> heightfield(rcAllocHeightfield());
			if(!heightfield || !rcCreateHeightfield(&context, *heightfield, config.width, config.height,
				config.bmin, config.bmax, config.cs, config.ch)) {
				reason = "RecastHeightfieldCreationFailed"; return RecastGroundBuildStatus::RecastHeightfieldFailed;
			}
			if(Cancelled(input)) { reason = "CancelledBeforeRasterization"; return RecastGroundBuildStatus::Cancelled; }
			const int triangleCount = static_cast<int>(triangles.size() / 3U);
			if(obstructionTriangles.size() != static_cast<std::size_t>(triangleCount)) {
				reason = "RecastTriangleRoleCountMismatch"; return RecastGroundBuildStatus::InvalidGeometry;
			}
			std::vector<unsigned char> areas(static_cast<std::size_t>(triangleCount), RC_NULL_AREA);
			rcMarkWalkableTriangles(&context, config.walkableSlopeAngle, vertices.data(),
				static_cast<int>(vertices.size() / 3U), triangles.data(), triangleCount, areas.data());
			for(std::size_t index = 0U; index < areas.size(); ++index) {
				if(obstructionTriangles[index] != 0U) areas[index] = RC_NULL_AREA;
			}
			if(!rcRasterizeTriangles(&context, vertices.data(), static_cast<int>(vertices.size() / 3U),
				triangles.data(), areas.data(), triangleCount, *heightfield, config.walkableClimb)) {
				reason = "RecastTriangleRasterizationFailed"; return RecastGroundBuildStatus::RecastRasterizationFailed;
			}
			rcFilterLowHangingWalkableObstacles(&context, config.walkableClimb, *heightfield);
			rcFilterLedgeSpans(&context, config.walkableHeight, config.walkableClimb, *heightfield);
			rcFilterWalkableLowHeightSpans(&context, config.walkableHeight, *heightfield);
			std::unique_ptr<rcCompactHeightfield, CompactDeleter> compact(rcAllocCompactHeightfield());
			if(!compact || !rcBuildCompactHeightfield(&context, config.walkableHeight, config.walkableClimb, *heightfield, *compact)) {
				reason = "RecastCompactHeightfieldBuildFailed"; return RecastGroundBuildStatus::RecastCompactHeightfieldFailed;
			}
			heightfield.reset();
			for(const NavigationGeometryBounds& obstruction : obstructionBounds) {
				const float minimum[] = {obstruction.minimum.x, obstruction.minimum.y, obstruction.minimum.z};
				const float maximum[] = {obstruction.maximum.x, obstruction.maximum.y, obstruction.maximum.z};
				// A collider is a filled physical volume, while Recast triangle input is
				// surface-only. Marking its compact cells removes the ground beneath a box
				// and prevents a route from crossing a locally captured static obstacle.
				rcMarkBoxArea(&context, minimum, maximum, RC_NULL_AREA, *compact);
			}
			if(Cancelled(input)) { reason = "CancelledAfterCompactHeightfield"; return RecastGroundBuildStatus::Cancelled; }
			if(!rcErodeWalkableArea(&context, config.walkableRadius, *compact)
				|| !rcBuildDistanceField(&context, *compact)
				|| !rcBuildRegions(&context, *compact, config.borderSize, config.minRegionArea, config.mergeRegionArea)) {
				reason = "RecastWalkableRegionBuildFailed"; return RecastGroundBuildStatus::ProviderFailure;
			}
			std::unique_ptr<rcContourSet, ContourDeleter> contours(rcAllocContourSet());
			if(!contours || !rcBuildContours(&context, *compact, config.maxSimplificationError, config.maxEdgeLen, *contours)) {
				reason = "RecastContourBuildFailed"; return RecastGroundBuildStatus::ProviderFailure;
			}
			blob.key = key;
			blob.sourceFingerprints = fingerprints;
			blob.supportComplete = true;
			blob.clearanceComplete = true;
			if(contours->nconts == 0) {
				// Authoritative empty coverage remains distinct from missing coverage.
				blob.empty = true;
				blob.checksum = Checksum(blob.pristineBytes);
				return RecastGroundBuildStatus::Success;
			}
			std::unique_ptr<rcPolyMesh, PolyMeshDeleter> polyMesh(rcAllocPolyMesh());
			if(!polyMesh || !rcBuildPolyMesh(&context, *contours, config.maxVertsPerPoly, *polyMesh)) {
				reason = "RecastPolygonMeshBuildFailed"; return RecastGroundBuildStatus::ProviderFailure;
			}
			std::unique_ptr<rcPolyMeshDetail, DetailMeshDeleter> detailMesh(rcAllocPolyMeshDetail());
			if(!detailMesh || !rcBuildPolyMeshDetail(&context, *polyMesh, *compact,
				config.detailSampleDist, config.detailSampleMaxError, *detailMesh)) {
				reason = "RecastDetailMeshBuildFailed"; return RecastGroundBuildStatus::ProviderFailure;
			}
			if(polyMesh->nverts >= 0xffff || polyMesh->nvp > DT_VERTS_PER_POLYGON) {
				reason = "RecastPolygonMeshOutsideDetourBound"; return RecastGroundBuildStatus::ProviderFailure;
			}
			for(int index = 0; index < polyMesh->npolys; ++index) {
				if(polyMesh->areas[index] == RC_WALKABLE_AREA) polyMesh->areas[index] = kGroundArea;
				polyMesh->flags[index] = kGroundPolygonFlag;
			}
			dtNavMeshCreateParams parameters{};
			parameters.verts = polyMesh->verts;
			parameters.vertCount = polyMesh->nverts;
			parameters.polys = polyMesh->polys;
			parameters.polyAreas = polyMesh->areas;
			parameters.polyFlags = polyMesh->flags;
			parameters.polyCount = polyMesh->npolys;
			parameters.nvp = polyMesh->nvp;
			parameters.detailMeshes = detailMesh->meshes;
			parameters.detailVerts = detailMesh->verts;
			parameters.detailVertsCount = detailMesh->nverts;
			parameters.detailTris = detailMesh->tris;
			parameters.detailTriCount = detailMesh->ntris;
			parameters.walkableHeight = static_cast<float>(input.profile.heightMetres);
			parameters.walkableRadius = static_cast<float>(input.profile.radiusMetres + input.profile.clearanceMarginMetres);
			parameters.walkableClimb = static_cast<float>(input.profile.stepHeightMetres);
			parameters.tileX = static_cast<int>(localTile(key.x));
			parameters.tileY = static_cast<int>(localTile(key.z));
			parameters.tileLayer = 0;
			rcVcopy(parameters.bmin, polyMesh->bmin);
			rcVcopy(parameters.bmax, polyMesh->bmax);
			parameters.cs = config.cs;
			parameters.ch = config.ch;
			parameters.buildBvTree = true;
			unsigned char* providerBytes = nullptr;
			int providerByteCount = 0;
			if(!dtCreateNavMeshData(&parameters, &providerBytes, &providerByteCount)
				|| providerBytes == nullptr || providerByteCount <= 0) {
				if(providerBytes != nullptr) dtFree(providerBytes);
				reason = "DetourNavigationDataBuildFailed"; return RecastGroundBuildStatus::ProviderFailure;
			}
			blob.pristineBytes.resize(static_cast<std::size_t>(providerByteCount));
			std::memcpy(blob.pristineBytes.data(), providerBytes, static_cast<std::size_t>(providerByteCount));
			dtFree(providerBytes);
			blob.checksum = Checksum(blob.pristineBytes);
			return RecastGroundBuildStatus::Success;
		}
	}

	std::uint64_t MakeGroundProfileBuildKey(const NavigationProfile& profile) noexcept {
		std::uint64_t hash = 14695981039346656037ULL;
		MixValue(hash, profile.radiusMetres); MixValue(hash, profile.heightMetres);
		MixValue(hash, profile.stepHeightMetres); MixValue(hash, profile.maxSlopeDegrees);
		MixValue(hash, profile.clearanceMarginMetres); MixValue(hash, profile.horizontalResolutionMetres);
		MixValue(hash, profile.verticalResolutionMetres); MixValue(hash, profile.excludedAreasMask);
		for(const double penalty : profile.areaPenalties) MixValue(hash, penalty);
		return hash;
	}

	std::optional<GroundTileBuildHalo> ComputeGroundTileBuildHalo(const NavigationProfile& profile) noexcept {
		// Keep this calculation bit-for-bit aligned with BuildTile's rcConfig setup.
		// A generic dirty expansion is insufficient because a larger profile radius or
		// finer resolution can make Recast inspect geometry beyond that fixed margin.
		if(!profile.Validate().accepted) return std::nullopt;
		const double maximumFloat = std::numeric_limits<float>::max();
		if(profile.horizontalResolutionMetres > maximumFloat
			|| profile.verticalResolutionMetres > maximumFloat
			|| profile.horizontalResolutionMetres < std::numeric_limits<float>::min()
			|| profile.verticalResolutionMetres < std::numeric_limits<float>::min()) return std::nullopt;
		const float cellSize = static_cast<float>(profile.horizontalResolutionMetres);
		const double radiusVoxels = std::ceil((profile.radiusMetres + profile.clearanceMarginMetres) / cellSize);
		const double tileVoxels = std::ceil(static_cast<double>(kGroundTileEdgeMetres) / cellSize);
		const double gridVoxels = tileVoxels + 2.0 * (radiusVoxels + 3.0);
		const double verticalExtent = profile.heightMetres + profile.stepHeightMetres + profile.clearanceMarginMetres;
		if(!std::isfinite(radiusVoxels) || radiusVoxels < 0.0
			|| radiusVoxels > std::numeric_limits<int>::max()
			|| !std::isfinite(gridVoxels) || gridVoxels <= 0.0 || gridVoxels > 4096.0
			|| !std::isfinite(verticalExtent) || verticalExtent < 0.0 || verticalExtent > maximumFloat / 2.0) return std::nullopt;
		const int borderSize = static_cast<int>(radiusVoxels) + 3;
		const float horizontalHalo = static_cast<float>(borderSize) * cellSize;
		const float verticalHalo = static_cast<float>(verticalExtent);
		if(!std::isfinite(horizontalHalo) || !std::isfinite(verticalHalo)
			|| horizontalHalo < 0.0F || verticalHalo < 0.0F) return std::nullopt;
		return GroundTileBuildHalo{static_cast<double>(horizontalHalo), static_cast<double>(verticalHalo)};
	}

	NavigationGeometryBounds CanonicalGroundTileBounds(const GroundTileKey& key) noexcept {
		// The key itself is the cache's world-space address. Persisting these exact
		// bounds prevents a valid provider byte sequence being replayed for another
		// cell whose descriptor happens to share a profile and source fingerprint.
		const auto localTile = [](std::int64_t coordinate) {
			const std::int64_t remainder = coordinate % 8LL;
			return static_cast<float>(remainder < 0 ? remainder + 8LL : remainder);
		};
		const float minimumX = localTile(key.x) * kGroundTileEdgeMetres;
		const float minimumY = 0.0F;
		const float minimumZ = localTile(key.z) * kGroundTileEdgeMetres;
		return {{minimumX, minimumY, minimumZ},
			{minimumX + kGroundTileEdgeMetres, minimumY + kGroundVerticalBandMetres,
				minimumZ + kGroundTileEdgeMetres}};
	}

	GroundCoverageEvidence CaptureGroundCoverageEvidence(const RecastGroundBuildInput& input) {
		GroundCoverageEvidence evidence{};
		if(Cancelled(input)) {
			evidence.reason = "CancelledBeforeCoverageEvidence";
			return evidence;
		}
		if(!input.profile.Validate().accepted) {
			evidence.reason = "InvalidNavigationProfile";
			return evidence;
		}
		if(input.sources.empty() || input.maximumTiles == 0U) {
			evidence.reason = "NoImmutableGeometrySourcesOrTileCapacity";
			return evidence;
		}

		// The metadata pass is shared with production, but avoids constructing
		// throwaway provider geometry merely to validate a saved cache descriptor.
		std::set<GroundTileKey> keys;
		if(!GatherGeometryMetadata(input,
			evidence.sourceFingerprints, keys, evidence.reason)) return evidence;

		evidence.tileKeys.assign(keys.begin(), keys.end());
		evidence.sourceFingerprint = FingerprintSources(evidence.sourceFingerprints);
		evidence.settingsFingerprint = MakeGroundProfileBuildKey(input.profile);
		if(evidence.sourceFingerprint == 0U || evidence.settingsFingerprint == 0U) {
			evidence.reason = "CoverageEvidenceFingerprintInvalid";
			evidence.sourceFingerprints.clear();
			evidence.tileKeys.clear();
			return evidence;
		}
		evidence.accepted = true;
		return evidence;
	}

	SavedGroundTileExportResult BuildSavedNavigationChunks(std::span<const GroundTileBlob> tiles) {
		SavedGroundTileExportResult result{};
		if(tiles.empty() || tiles.size() > kMaximumSavedNavigationChunks) {
			result.reason = "SavedGroundTileExportEmptyOrOverCapacity";
			return result;
		}
		result.chunks.reserve(tiles.size());
		for(const GroundTileBlob& tile : tiles) {
			if(tile.generation == 0U || tile.sourceFingerprints.empty()
				|| tile.sourceFingerprints.size() > kMaximumSavedNavigationSourceFingerprints
				|| !std::is_sorted(tile.sourceFingerprints.begin(), tile.sourceFingerprints.end())
				|| !tile.supportComplete || !tile.clearanceComplete
				|| (tile.empty ? !tile.pristineBytes.empty() : tile.pristineBytes.empty())
				|| tile.checksum != Checksum(tile.pristineBytes)) {
				result.reason = "SavedGroundTileExportInvalidTile";
				result.chunks.clear();
				return result;
			}
			SavedNavigationChunk chunk{};
			chunk.key = {tile.key.x, tile.key.z, tile.key.verticalBand, tile.key.profileBuildKey};
			chunk.generation = tile.generation;
			chunk.sourceFingerprints = tile.sourceFingerprints;
			chunk.bounds = CanonicalGroundTileBounds(tile.key);
			chunk.bytes = tile.pristineBytes;
			chunk.checksum = tile.checksum;
			chunk.empty = tile.empty;
			chunk.supportComplete = tile.supportComplete;
			chunk.clearanceComplete = tile.clearanceComplete;
			result.chunks.push_back(std::move(chunk));
		}
		std::sort(result.chunks.begin(), result.chunks.end(), [](const SavedNavigationChunk& left, const SavedNavigationChunk& right) {
			return std::tie(left.key.profileBuildKey, left.key.verticalBand, left.key.shardZ, left.key.shardX)
				< std::tie(right.key.profileBuildKey, right.key.verticalBand, right.key.shardZ, right.key.shardX);
		});
		for(std::size_t index = 1U; index < result.chunks.size(); ++index) {
			if(result.chunks[index - 1U].key == result.chunks[index].key) {
				result.reason = "SavedGroundTileExportDuplicateKey";
				result.chunks.clear();
				return result;
			}
		}
		result.accepted = true;
		return result;
	}

	SavedGroundTileImportResult ImportSavedGroundTiles(
		const SavedNavigationReadResult& read,
		const GroundCoverageEvidence& evidence,
		NavigationBudget* budget) {
		SavedGroundTileImportResult result{};
		if(!read.accepted || !read.matchesFreshRuntimeGeneration || !evidence.accepted
			|| read.descriptor.formatVersion != kSavedNavigationFormatVersion
			|| std::string_view(read.descriptor.backendAbi) != kSavedNavigationBackendAbi
			|| read.descriptor.sourceFingerprint != evidence.sourceFingerprint
			|| read.descriptor.settingsFingerprint != evidence.settingsFingerprint
			|| read.chunks.size() != evidence.tileKeys.size() || read.chunks.empty()) {
			result.reason = "SavedGroundTileImportEvidenceMismatch";
			return result;
		}

		std::set<GroundTileKey> expected(evidence.tileKeys.begin(), evidence.tileKeys.end());
		if(expected.size() != evidence.tileKeys.size()) {
			result.reason = "SavedGroundTileImportDuplicateEvidenceKey";
			return result;
		}
		result.tiles.reserve(read.chunks.size());
		for(const SavedNavigationChunk& chunk : read.chunks) {
			const GroundTileKey key{chunk.key.shardX, chunk.key.shardZ,
				chunk.key.verticalBand, chunk.key.profileBuildKey};
			if(!expected.erase(key) || chunk.generation == 0U
				|| chunk.sourceFingerprints != evidence.sourceFingerprints
				|| chunk.bounds != CanonicalGroundTileBounds(key)
				|| !std::is_sorted(chunk.sourceFingerprints.begin(), chunk.sourceFingerprints.end())
				|| !chunk.supportComplete || !chunk.clearanceComplete
				|| (chunk.empty ? !chunk.bytes.empty() : chunk.bytes.empty())
				|| chunk.checksum != Checksum(chunk.bytes)) {
				result.reason = "SavedGroundTileImportTileMismatch";
				result.tiles.clear();
				return result;
			}
			GroundTileBlob tile{};
			tile.key = key;
			tile.generation = chunk.generation;
			tile.sourceFingerprints = chunk.sourceFingerprints;
			tile.pristineBytes = chunk.bytes;
			tile.checksum = chunk.checksum;
			tile.empty = chunk.empty;
			tile.supportComplete = chunk.supportComplete;
			tile.clearanceComplete = chunk.clearanceComplete;
			if(budget != nullptr && !tile.pristineBytes.empty()) {
				auto reservation = std::make_shared<NavigationBudgetReservation>(budget->TryReserve(
					NavigationBudgetCategory::Geometry, tile.pristineBytes.size(), "imported pristine Recast ground tile"));
				if(!reservation->accepted) {
					result.reason = reservation->reason;
					result.tiles.clear();
					return result;
				}
				tile.budgetReservation = std::move(reservation);
			}
			result.tiles.push_back(std::move(tile));
		}
		if(!expected.empty()) {
			result.reason = "SavedGroundTileImportMissingExpectedTile";
			result.tiles.clear();
			return result;
		}
		result.accepted = true;
		return result;
	}

	RecastGroundBuildResult BuildRecastGroundTiles(const RecastGroundBuildInput& input) {
		RecastGroundBuildResult result{};
		if(Cancelled(input)) { result.status = RecastGroundBuildStatus::Cancelled; result.reason = "CancelledBeforeRecast"; return result; }
		if(!input.profile.Validate().accepted) { result.status = RecastGroundBuildStatus::InvalidProfile; result.reason = "InvalidNavigationProfile"; return result; }
		if(input.sources.empty() || input.maximumTiles == 0U) { result.status = RecastGroundBuildStatus::InvalidGeometry; result.reason = "NoImmutableGeometrySourcesOrTileCapacity"; return result; }
		std::vector<std::uint64_t> fingerprints;
		std::set<GroundTileKey> keys;
		if(!GatherGeometryMetadata(input,
			fingerprints, keys, result.reason)) {
			result.status = result.reason.starts_with("Cancelled") ? RecastGroundBuildStatus::Cancelled
				: result.reason == "CanonicalTileCountExceedsBound" ? RecastGroundBuildStatus::BudgetExceeded
				: result.reason == "SourceBoundsOutsideCanonicalTileRange" ? RecastGroundBuildStatus::LocalizationFailed
				: RecastGroundBuildStatus::InvalidGeometry;
			return result;
		}
		std::map<std::tuple<std::int64_t, std::int64_t, std::int32_t>, std::vector<GroundTileKey>> shards;
		for(const GroundTileKey& key : keys) {
			shards[{FloorDivide(key.x, 8LL), FloorDivide(key.z, 8LL), key.verticalBand}].push_back(key);
		}
		for(const auto& [address, shardTiles] : shards) {
			std::vector<float> shardVertices;
			std::vector<int> shardTriangles;
			std::vector<unsigned char> shardObstructionTriangles;
			std::vector<NavigationGeometryBounds> shardObstructionBounds;
			const std::span<const GroundTileKey> selectedTiles = input.requestedTiles.empty()
				? std::span<const GroundTileKey>{}
				: std::span<const GroundTileKey>(shardTiles.data(), shardTiles.size());
			if(!GatherShardLocalGeometry(input, std::get<0>(address), std::get<1>(address), std::get<2>(address),
				shardVertices, shardTriangles, shardObstructionTriangles, shardObstructionBounds,
				selectedTiles, result.reason)) {
				result.status = result.reason.starts_with("Cancelled") ? RecastGroundBuildStatus::Cancelled
					: RecastGroundBuildStatus::LocalizationFailed;
				result.tiles.clear(); return result;
			}
			for(const GroundTileKey& key : shardTiles) {
				if(Cancelled(input)) { result.status = RecastGroundBuildStatus::Cancelled; result.reason = "CancelledDuringTileBuild"; result.tiles.clear(); return result; }
				GroundTileBlob blob{};
				result.status = BuildTile(input, key, shardVertices, shardTriangles, shardObstructionTriangles,
					shardObstructionBounds, fingerprints, blob, result.reason);
				if(result.status != RecastGroundBuildStatus::Success) { result.tiles.clear(); return result; }
				if(input.budget != nullptr && !blob.pristineBytes.empty()) {
					auto reservation = std::make_shared<NavigationBudgetReservation>(input.budget->TryReserve(
						NavigationBudgetCategory::Geometry, blob.pristineBytes.size(), "pristine Recast ground tile"));
					if(!reservation->accepted) { result.status = RecastGroundBuildStatus::BudgetExceeded; result.reason = reservation->reason; result.tiles.clear(); return result; }
					blob.budgetReservation = std::move(reservation);
				}
				result.tiles.push_back(std::move(blob));
			}
		}
		result.status = RecastGroundBuildStatus::Success;
		result.reason.clear();
		return result;
	}
}
