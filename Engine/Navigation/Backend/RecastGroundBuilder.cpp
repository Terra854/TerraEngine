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
#include <memory>
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

		void AppendTriangle(std::vector<float>& vertices, std::vector<int>& triangles,
			NavigationGeometryVector3 first, NavigationGeometryVector3 second, NavigationGeometryVector3 third) {
			const int base = static_cast<int>(vertices.size() / 3U);
			vertices.insert(vertices.end(), {first.x, first.y, first.z, second.x, second.y, second.z, third.x, third.y, third.z});
			triangles.insert(triangles.end(), {base, base + 1, base + 2});
		}

		void AppendBox(std::vector<float>& vertices, std::vector<int>& triangles,
			const NavigationGeometryPrimitive& box) {
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
			AppendTriangle(vertices, triangles, p010, p111, p110); AppendTriangle(vertices, triangles, p010, p011, p111);
			AppendTriangle(vertices, triangles, p000, p100, p101); AppendTriangle(vertices, triangles, p000, p101, p001);
			AppendTriangle(vertices, triangles, p001, p101, p111); AppendTriangle(vertices, triangles, p001, p111, p011);
			AppendTriangle(vertices, triangles, p000, p010, p110); AppendTriangle(vertices, triangles, p000, p110, p100);
			AppendTriangle(vertices, triangles, p100, p110, p111); AppendTriangle(vertices, triangles, p100, p111, p101);
			AppendTriangle(vertices, triangles, p000, p001, p011); AppendTriangle(vertices, triangles, p000, p011, p010);
		}

		[[nodiscard]] bool GatherGeometry(const RecastGroundBuildInput& input,
			std::vector<float>& vertices, std::vector<int>& triangles,
			std::vector<std::uint64_t>& fingerprints, std::set<GroundTileKey>& keys,
			std::string& reason) {
			const std::uint64_t buildKey = MakeGroundProfileBuildKey(input.profile);
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
				const int minX = static_cast<int>(std::floor(bounds.minimum.x / kGroundTileEdgeMetres));
				const int maxX = static_cast<int>(std::floor(bounds.maximum.x / kGroundTileEdgeMetres));
				const int minZ = static_cast<int>(std::floor(bounds.minimum.z / kGroundTileEdgeMetres));
				const int maxZ = static_cast<int>(std::floor(bounds.maximum.z / kGroundTileEdgeMetres));
				const int minBand = static_cast<int>(std::floor(bounds.minimum.y / kGroundVerticalBandMetres));
				const int maxBand = static_cast<int>(std::floor(bounds.maximum.y / kGroundVerticalBandMetres));
				for(int band = minBand; band <= maxBand; ++band) for(int z = minZ; z <= maxZ; ++z) for(int x = minX; x <= maxX; ++x) {
					keys.insert({x, z, band, buildKey});
					if(keys.size() > input.maximumTiles) { reason = "CanonicalTileCountExceedsBound"; return false; }
				}
				for(const auto& triangle : source->Triangles()) {
					if(vertices.size() / 3U > static_cast<std::size_t>(std::numeric_limits<int>::max() - 3)) {
						reason = "SourceVertexCountExceedsProviderBound"; return false;
					}
					AppendTriangle(vertices, triangles, triangle.firstVertex, triangle.secondVertex, triangle.thirdVertex);
				}
				for(const auto& primitive : source->Primitives()) {
					if(primitive.kind != NavigationGeometryPrimitiveKind::Box) {
						reason = "UnsupportedAnalyticPrimitiveForRecast"; return false;
					}
					AppendBox(vertices, triangles, primitive);
				}
				fingerprints.push_back(source->ContentFingerprint());
			}
			std::sort(fingerprints.begin(), fingerprints.end());
			if(keys.empty() || triangles.empty()) { reason = "NoCanonicalTileGeometry"; return false; }
			return true;
		}

		[[nodiscard]] RecastGroundBuildStatus BuildTile(const RecastGroundBuildInput& input,
			const GroundTileKey& key, const std::vector<float>& vertices,
			const std::vector<int>& triangles, const std::vector<std::uint64_t>& fingerprints,
			GroundTileBlob& blob, std::string& reason) {
			rcConfig config{};
			config.cs = static_cast<float>(input.profile.horizontalResolutionMetres);
			config.ch = static_cast<float>(input.profile.verticalResolutionMetres);
			config.walkableSlopeAngle = static_cast<float>(input.profile.maxSlopeDegrees);
			config.walkableHeight = static_cast<int>(std::ceil(input.profile.heightMetres / config.ch));
			config.walkableClimb = static_cast<int>(std::floor(input.profile.stepHeightMetres / config.ch));
			config.walkableRadius = static_cast<int>(std::ceil((input.profile.radiusMetres + input.profile.clearanceMarginMetres) / config.cs));
			config.maxEdgeLen = static_cast<int>(kGroundTileEdgeMetres / config.cs);
			config.maxSimplificationError = 1.3F;
			config.minRegionArea = 64;
			config.mergeRegionArea = 400;
			config.maxVertsPerPoly = 6;
			config.tileSize = static_cast<int>(std::ceil(kGroundTileEdgeMetres / config.cs));
			config.borderSize = config.walkableRadius + 3;
			config.width = config.tileSize + config.borderSize * 2;
			config.height = config.tileSize + config.borderSize * 2;
			config.detailSampleDist = config.cs * 6.0F;
			config.detailSampleMaxError = config.ch;
			const float horizontalHalo = static_cast<float>(config.borderSize) * config.cs;
			const float verticalHalo = static_cast<float>(input.profile.heightMetres + input.profile.stepHeightMetres + input.profile.clearanceMarginMetres);
			config.bmin[0] = static_cast<float>(key.x) * kGroundTileEdgeMetres - horizontalHalo;
			config.bmin[1] = static_cast<float>(key.verticalBand) * kGroundVerticalBandMetres - verticalHalo;
			config.bmin[2] = static_cast<float>(key.z) * kGroundTileEdgeMetres - horizontalHalo;
			config.bmax[0] = static_cast<float>(key.x + 1) * kGroundTileEdgeMetres + horizontalHalo;
			config.bmax[1] = static_cast<float>(key.verticalBand + 1) * kGroundVerticalBandMetres + verticalHalo;
			config.bmax[2] = static_cast<float>(key.z + 1) * kGroundTileEdgeMetres + horizontalHalo;
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
			std::vector<unsigned char> areas(static_cast<std::size_t>(triangleCount), RC_NULL_AREA);
			rcMarkWalkableTriangles(&context, config.walkableSlopeAngle, vertices.data(),
				static_cast<int>(vertices.size() / 3U), triangles.data(), triangleCount, areas.data());
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
			parameters.tileX = key.x;
			parameters.tileY = key.z;
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

	RecastGroundBuildResult BuildRecastGroundTiles(const RecastGroundBuildInput& input) {
		RecastGroundBuildResult result{};
		if(Cancelled(input)) { result.status = RecastGroundBuildStatus::Cancelled; result.reason = "CancelledBeforeRecast"; return result; }
		if(!input.profile.Validate().accepted) { result.status = RecastGroundBuildStatus::InvalidProfile; result.reason = "InvalidNavigationProfile"; return result; }
		if(input.sources.empty() || input.maximumTiles == 0U) { result.status = RecastGroundBuildStatus::InvalidGeometry; result.reason = "NoImmutableGeometrySourcesOrTileCapacity"; return result; }
		std::vector<float> vertices;
		std::vector<int> triangles;
		std::vector<std::uint64_t> fingerprints;
		std::set<GroundTileKey> keys;
		if(!GatherGeometry(input, vertices, triangles, fingerprints, keys, result.reason)) {
			result.status = result.reason.starts_with("Cancelled") ? RecastGroundBuildStatus::Cancelled
				: result.reason == "CanonicalTileCountExceedsBound" ? RecastGroundBuildStatus::BudgetExceeded
				: result.reason == "SourceBoundsOutsideCanonicalTileRange" ? RecastGroundBuildStatus::LocalizationFailed
				: RecastGroundBuildStatus::InvalidGeometry;
			return result;
		}
		for(const GroundTileKey& key : keys) {
			if(Cancelled(input)) { result.status = RecastGroundBuildStatus::Cancelled; result.reason = "CancelledDuringTileBuild"; result.tiles.clear(); return result; }
			GroundTileBlob blob{};
			result.status = BuildTile(input, key, vertices, triangles, fingerprints, blob, result.reason);
			if(result.status != RecastGroundBuildStatus::Success) { result.tiles.clear(); return result; }
			if(input.budget != nullptr && !blob.pristineBytes.empty()) {
				auto reservation = std::make_shared<NavigationBudgetReservation>(input.budget->TryReserve(
					NavigationBudgetCategory::Geometry, blob.pristineBytes.size(), "pristine Recast ground tile"));
				if(!reservation->accepted) { result.status = RecastGroundBuildStatus::BudgetExceeded; result.reason = reservation->reason; result.tiles.clear(); return result; }
				blob.budgetReservation = std::move(reservation);
			}
			result.tiles.push_back(std::move(blob));
		}
		result.status = RecastGroundBuildStatus::Success;
		result.reason.clear();
		return result;
	}
}
