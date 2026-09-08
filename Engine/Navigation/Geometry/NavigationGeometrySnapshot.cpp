// NavigationGeometrySnapshot.cpp -- Copied immutable source records and leases.

#include "Navigation/Geometry/NavigationGeometrySnapshot.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace TerraEngine::Navigation
{
	namespace
	{
		[[nodiscard]] std::uint64_t HashSnapshotDocument(const NavigationGeometryDocument& document) noexcept
		{
			// This fingerprint is an ownership identity, not a cryptographic promise.
			std::uint64_t hash = 14695981039346656037ULL;
			auto mix = [&hash](const auto& value) {
				const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
				for(std::size_t index = 0U; index < sizeof(value); ++index) {
					hash ^= bytes[index];
					hash *= 1099511628211ULL;
				}
			};
			for(const NavigationGeometryVector3& vertex : document.vertices) { mix(vertex.x); mix(vertex.y); mix(vertex.z); }
			for(const NavigationGeometryTriangle& triangle : document.triangles) { mix(triangle.firstVertex); mix(triangle.secondVertex); mix(triangle.thirdVertex); }
			for(const NavigationGeometryPrimitive& primitive : document.primitives) {
				mix(primitive.kind); mix(primitive.center.x); mix(primitive.center.y); mix(primitive.center.z);
				mix(primitive.halfExtents.x); mix(primitive.halfExtents.y); mix(primitive.halfExtents.z); mix(primitive.radius); mix(primitive.height);
			}
			return hash;
		}
	}

	NavigationUuid NavigationUuid::FromString(std::string_view text) noexcept
	{
		NavigationUuid result{};
		std::string compact;
		compact.reserve(text.size());
		for(const char character : text) if(character != '-') compact.push_back(character);
		if(const auto parsed = TerraEngine::ParseUUID(compact)) result.value = *parsed;
		return result;
	}

	std::size_t NavigationGeometryBvh::PrimitiveRecordCount() const noexcept { return primitiveRecordCount; }
	const NavigationGeometryBvhRecord& NavigationGeometryBvh::RootRecord() const noexcept
	{
		static const NavigationGeometryBvhRecord empty{};
		return records.empty() ? empty : records.front();
	}
	const std::vector<NavigationGeometryBvhRecord>& NavigationGeometryBvh::Records() const noexcept { return records; }

	NavigationGeometrySnapshotLease::NavigationGeometrySnapshotLease(std::shared_ptr<const NavigationGeometrySnapshot> retained) noexcept
		: snapshot(std::move(retained)) {}
	NavigationGeometrySnapshotLease::NavigationGeometrySnapshotLease(NavigationGeometrySnapshotLease&& other) noexcept
		: snapshot(std::move(other.snapshot)) {}
	NavigationGeometrySnapshotLease& NavigationGeometrySnapshotLease::operator=(NavigationGeometrySnapshotLease&& other) noexcept
	{
		if(this == &other) return *this;
		if(snapshot) snapshot->ReleaseLease();
		snapshot = std::move(other.snapshot);
		return *this;
	}
	NavigationGeometrySnapshotLease::~NavigationGeometrySnapshotLease() { if(snapshot) snapshot->ReleaseLease(); }
	bool NavigationGeometrySnapshotLease::IsValid() const noexcept { return snapshot != nullptr; }

	NavigationGeometrySnapshotCreateResult NavigationGeometrySnapshot::Create(
		const NavigationGeometrySnapshotDescriptor& descriptor, const NavigationGeometryDocument& document)
	{
		NavigationGeometrySnapshotCreateResult result{};
		NavigationGeometryBounds checkedBounds{};
		if(!ValidateNavigationGeometryDocument(document, checkedBounds, result.reason)) return result;
		auto snapshot = std::shared_ptr<NavigationGeometrySnapshot>(new NavigationGeometrySnapshot());
		snapshot->descriptor = descriptor;
		snapshot->bounds = checkedBounds;
		snapshot->contentFingerprint = HashSnapshotDocument(document);
		snapshot->triangles.reserve(document.triangles.size());
		for(const NavigationGeometryTriangle& triangle : document.triangles) {
			snapshot->triangles.push_back({ document.vertices[triangle.firstVertex], document.vertices[triangle.secondVertex], document.vertices[triangle.thirdVertex] });
		}
		snapshot->primitives = document.primitives;
		const std::size_t primitiveCount = snapshot->triangles.size() + snapshot->primitives.size();
		if(primitiveCount > std::numeric_limits<std::uint32_t>::max()) { result.reason = "SnapshotPrimitiveCountExceedsBound"; return result; }
		snapshot->bvh.primitiveRecordCount = primitiveCount;
		if(primitiveCount != 0U) snapshot->bvh.records.push_back({ checkedBounds, 0U, static_cast<std::uint32_t>(primitiveCount) });
		result.accepted = true;
		result.snapshot = std::move(snapshot);
		return result;
	}

	const NavigationGeometrySnapshotDescriptor& NavigationGeometrySnapshot::Descriptor() const noexcept { return descriptor; }
	const std::vector<NavigationGeometryTriangleRecord>& NavigationGeometrySnapshot::Triangles() const noexcept { return triangles; }
	const std::vector<NavigationGeometryPrimitive>& NavigationGeometrySnapshot::Primitives() const noexcept { return primitives; }
	const NavigationGeometryBvh& NavigationGeometrySnapshot::Bvh() const noexcept { return bvh; }
	const NavigationGeometryBounds& NavigationGeometrySnapshot::Bounds() const noexcept { return bounds; }
	std::uint64_t NavigationGeometrySnapshot::ContentFingerprint() const noexcept { return contentFingerprint; }
	NavigationGeometrySnapshotLease NavigationGeometrySnapshot::AcquireLease(NavigationGeometryLeaseLimits limits) const
	{
		if(limits.maximumFrames == 0U || limits.maximumFrames > 120U || !std::isfinite(limits.maximumSeconds)
			|| limits.maximumSeconds <= 0.0 || limits.maximumSeconds > 2.0) return {};
		leaseCount.fetch_add(1U, std::memory_order_acq_rel);
		return NavigationGeometrySnapshotLease(shared_from_this());
	}
	bool NavigationGeometrySnapshot::CanRetire() const noexcept { return leaseCount.load(std::memory_order_acquire) == 0U; }
	void NavigationGeometrySnapshot::ReleaseLease() const noexcept { leaseCount.fetch_sub(1U, std::memory_order_acq_rel); }
}
