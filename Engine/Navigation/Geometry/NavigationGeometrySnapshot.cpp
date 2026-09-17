// NavigationGeometrySnapshot.cpp -- Copied immutable source records and leases.

#include "Navigation/Geometry/NavigationGeometrySnapshot.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace TerraEngine::Navigation
{
	namespace
	{
		constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
		constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

		void MixBytes(std::uint64_t& hash, const void* data, std::size_t count) noexcept
		{
			const auto* bytes = static_cast<const unsigned char*>(data);
			for(std::size_t index = 0U; index < count; ++index) {
				hash ^= bytes[index];
				hash *= kFnvPrime;
			}
		}

		template<typename Value>
		void MixValue(std::uint64_t& hash, const Value& value) noexcept
		{
			MixBytes(hash, &value, sizeof(value));
		}

		void MixUuid(std::uint64_t& hash, const TerraEngine::UUID& value) noexcept
		{
			MixBytes(hash, value.data(), value.size());
		}

		void MixIdentity(std::uint64_t& hash, const NavigationEligibleSourceIdentity& identity) noexcept
		{
			MixUuid(hash, identity.sourceId);
			MixUuid(hash, identity.objectId);
			MixValue(hash, identity.objectLifetimeGeneration);
			MixUuid(hash, identity.assetId);
			MixValue(hash, identity.kind);
			MixValue(hash, identity.subshapeIndex);
		}

		[[nodiscard]] bool IsSupportedEligibleKind(NavigationSourceKind kind) noexcept
		{
			return kind == NavigationSourceKind::Terrain || kind == NavigationSourceKind::Collider
				|| kind == NavigationSourceKind::Mesh || kind == NavigationSourceKind::Procedural;
		}

		[[nodiscard]] bool LessIdentity(
			const NavigationEligibleSourceIdentity& left,
			const NavigationEligibleSourceIdentity& right) noexcept
		{
			return std::tie(left.sourceId, left.objectId, left.objectLifetimeGeneration,
				left.assetId, left.kind, left.subshapeIndex)
				< std::tie(right.sourceId, right.objectId, right.objectLifetimeGeneration,
					right.assetId, right.kind, right.subshapeIndex);
		}

		[[nodiscard]] bool IsSamePhysicalRepresentation(
			const NavigationEligibleSourceIdentity& left,
			const NavigationEligibleSourceIdentity& right) noexcept
		{
			// Collider-first preference applies only to alternative mesh/collider
			// views of one object subshape. Terrain can share an owning object in
			// tests or tools, but it is independent ground support and must remain.
			const bool leftIsMeshOrCollider = left.kind == NavigationSourceKind::Mesh
				|| left.kind == NavigationSourceKind::Collider;
			const bool rightIsMeshOrCollider = right.kind == NavigationSourceKind::Mesh
				|| right.kind == NavigationSourceKind::Collider;
			return leftIsMeshOrCollider && rightIsMeshOrCollider
				&& left.objectId != TerraEngine::EMPTY_UUID && left.objectId == right.objectId
				&& left.objectLifetimeGeneration == right.objectLifetimeGeneration
				&& left.subshapeIndex == right.subshapeIndex;
		}

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

	NavigationGeneration ComputeNavigationEligibleSourceRevision(
		const NavigationEligibleSourceIdentity& identity,
		const NavigationSourceRevisionEvidence& revisions) noexcept
	{
		std::uint64_t hash = kFnvOffset;
		MixIdentity(hash, identity);
		MixValue(hash, revisions.objectLifetimeGeneration);
		MixValue(hash, revisions.transformRevision);
		MixValue(hash, revisions.geometryRevision);
		MixValue(hash, revisions.physicalPublicationGeneration);
		MixValue(hash, revisions.resourcePublicationGeneration);
		// Zero is the invalid/unobserved generation sentinel throughout navigation.
		return hash == 0U ? 1U : hash;
	}

	const std::vector<NavigationEligibleSourceRecord>& NavigationEligibleSourceIndex::Records() const noexcept
	{
		return records_;
	}

	const std::vector<NavigationEligibleSourceSelection>& NavigationEligibleSourceIndex::Exclusions() const noexcept
	{
		return exclusions_;
	}

	NavigationGeneration NavigationEligibleSourceIndex::Revision() const noexcept { return revision_; }
	bool NavigationEligibleSourceIndex::HasCurrentTerrain() const noexcept { return hasCurrentTerrain_; }

	NavigationEligibleSourceIndexBuildResult BuildNavigationEligibleSourceIndex(
		std::span<const NavigationEligibleSourceCandidate> candidates)
	{
		NavigationEligibleSourceIndexBuildResult result{};
		std::vector<NavigationEligibleSourceCandidate> ordered{candidates.begin(), candidates.end()};
		std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
			return LessIdentity(left.identity, right.identity);
		});

		std::set<TerraEngine::UUID> seenSourceIds;
		for(const NavigationEligibleSourceCandidate& candidate : ordered) {
			NavigationEligibleSourceSelectionCode exclusion = NavigationEligibleSourceSelectionCode::Selected;
			if(!candidate.identity.IsValid()) exclusion = NavigationEligibleSourceSelectionCode::InvalidIdentity;
			else if(!IsSupportedEligibleKind(candidate.identity.kind)) exclusion = NavigationEligibleSourceSelectionCode::UnsupportedKind;
			else if(!candidate.activeInHierarchy) exclusion = NavigationEligibleSourceSelectionCode::Inactive;
			else if(!candidate.physicalBackingCurrent) exclusion = NavigationEligibleSourceSelectionCode::PhysicalBackingUnavailable;
			else if(!candidate.revisions.IsComplete()
				|| candidate.revisions.objectLifetimeGeneration != candidate.identity.objectLifetimeGeneration) {
				exclusion = NavigationEligibleSourceSelectionCode::IncompleteRevisionEvidence;
			} else if(candidate.geometry == nullptr
				|| candidate.geometry->Descriptor().sourceUuid.value != candidate.identity.sourceId
				|| candidate.geometry->Descriptor().sourceKind != candidate.identity.kind
				|| candidate.geometry->Descriptor().sourceRevision
					!= ComputeNavigationEligibleSourceRevision(candidate.identity, candidate.revisions)) {
				exclusion = NavigationEligibleSourceSelectionCode::SnapshotMismatch;
			} else if(!seenSourceIds.insert(candidate.identity.sourceId).second) {
				exclusion = NavigationEligibleSourceSelectionCode::DuplicateSourceIdentity;
			}
			if(exclusion != NavigationEligibleSourceSelectionCode::Selected) {
				result.index.exclusions_.push_back({candidate.identity, exclusion});
				continue;
			}

			auto equivalent = std::find_if(result.index.records_.begin(), result.index.records_.end(),
				[&candidate](const NavigationEligibleSourceRecord& current) {
					return IsSamePhysicalRepresentation(current.identity, candidate.identity);
				});
			if(equivalent != result.index.records_.end()) {
				if(candidate.identity.kind == NavigationSourceKind::Collider
					&& equivalent->identity.kind == NavigationSourceKind::Mesh) {
					result.index.exclusions_.push_back({equivalent->identity,
						NavigationEligibleSourceSelectionCode::SupersededByCollider});
					*equivalent = {candidate.identity, candidate.revisions, candidate.geometry};
					continue;
				}
				if(candidate.identity.kind == NavigationSourceKind::Mesh
					&& equivalent->identity.kind == NavigationSourceKind::Collider) {
					result.index.exclusions_.push_back({candidate.identity,
						NavigationEligibleSourceSelectionCode::SupersededByCollider});
					continue;
				}
				result.index.exclusions_.push_back({candidate.identity,
					NavigationEligibleSourceSelectionCode::DuplicateSourceIdentity});
				continue;
			}
			result.index.records_.push_back({candidate.identity, candidate.revisions, candidate.geometry});
		}

		std::sort(result.index.records_.begin(), result.index.records_.end(),
			[](const auto& left, const auto& right) { return LessIdentity(left.identity, right.identity); });
		std::sort(result.index.exclusions_.begin(), result.index.exclusions_.end(),
			[](const auto& left, const auto& right) {
				return std::tie(left.identity.sourceId, left.code) < std::tie(right.identity.sourceId, right.code);
			});

		std::uint64_t revision = kFnvOffset;
		for(const NavigationEligibleSourceRecord& record : result.index.records_) {
			MixIdentity(revision, record.identity);
			MixValue(revision, record.revisions.objectLifetimeGeneration);
			MixValue(revision, record.revisions.transformRevision);
			MixValue(revision, record.revisions.geometryRevision);
			MixValue(revision, record.revisions.physicalPublicationGeneration);
			MixValue(revision, record.revisions.resourcePublicationGeneration);
			MixValue(revision, record.geometry->ContentFingerprint());
			if(record.identity.kind == NavigationSourceKind::Terrain) result.index.hasCurrentTerrain_ = true;
		}
		result.index.revision_ = revision == 0U ? 1U : revision;
		result.accepted = true;
		result.reason = "Captured immutable eligible navigation sources.";
		return result;
	}

	std::string_view ToString(NavigationEligibleSourceSelectionCode code) noexcept
	{
		switch(code) {
		case NavigationEligibleSourceSelectionCode::Selected: return "Selected";
		case NavigationEligibleSourceSelectionCode::Inactive: return "Inactive";
		case NavigationEligibleSourceSelectionCode::PhysicalBackingUnavailable: return "PhysicalBackingUnavailable";
		case NavigationEligibleSourceSelectionCode::InvalidIdentity: return "InvalidIdentity";
		case NavigationEligibleSourceSelectionCode::IncompleteRevisionEvidence: return "IncompleteRevisionEvidence";
		case NavigationEligibleSourceSelectionCode::SnapshotMismatch: return "SnapshotMismatch";
		case NavigationEligibleSourceSelectionCode::UnsupportedKind: return "UnsupportedKind";
		case NavigationEligibleSourceSelectionCode::DuplicateSourceIdentity: return "DuplicateSourceIdentity";
		case NavigationEligibleSourceSelectionCode::SupersededByCollider: return "SupersededByCollider";
		}
		return "UnknownNavigationEligibleSourceSelectionCode";
	}
}
