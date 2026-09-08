// NavigationGeometrySnapshot.h -- Immutable retained source geometry for workers.

#pragma once

#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/NavigationTypes.h"
#include "Utilities/UUID.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Navigation
{
	// This source UUID remains local to geometry snapshots. Broader authored
	// identities remain owned by NavigationTypes and component work.
	struct NavigationUuid
	{
		TerraEngine::UUID value = TerraEngine::EMPTY_UUID;
		[[nodiscard]] static NavigationUuid FromString(std::string_view text) noexcept;
		[[nodiscard]] constexpr bool operator==(const NavigationUuid&) const noexcept = default;
	};

	struct NavigationGeometrySnapshotDescriptor
	{
		NavigationUuid sourceUuid{};
		std::uint64_t sourceRevision = 0U;
		std::uint64_t canonicalPlacementRevision = 0U;
		std::uint64_t physicalReadyRevision = 0U;
		NavigationSourceKind sourceKind = NavigationSourceKind::Mesh;
		std::uint32_t subshapeIndex = 0U;
	};

	struct NavigationGeometryTriangleRecord
	{
		NavigationGeometryVector3 firstVertex{};
		NavigationGeometryVector3 secondVertex{};
		NavigationGeometryVector3 thirdVertex{};
	};

	struct NavigationGeometryBvhRecord
	{
		NavigationGeometryBounds bounds{};
		std::uint32_t firstPrimitive = 0U;
		std::uint32_t primitiveCount = 0U;
	};

	class TERRAENGINE_API NavigationGeometryBvh final
	{
	public:
		[[nodiscard]] std::size_t PrimitiveRecordCount() const noexcept;
		[[nodiscard]] const NavigationGeometryBvhRecord& RootRecord() const noexcept;
		[[nodiscard]] const std::vector<NavigationGeometryBvhRecord>& Records() const noexcept;

	private:
		friend class NavigationGeometrySnapshot;
		std::vector<NavigationGeometryBvhRecord> records;
		std::size_t primitiveRecordCount = 0U;
	};

	struct NavigationGeometryLeaseLimits
	{
		std::uint32_t maximumFrames = 120U;
		double maximumSeconds = 2.0;
	};

	class NavigationGeometrySnapshot;

	class TERRAENGINE_API NavigationGeometrySnapshotLease final
	{
	public:
		NavigationGeometrySnapshotLease() = default;
		NavigationGeometrySnapshotLease(const NavigationGeometrySnapshotLease&) = delete;
		NavigationGeometrySnapshotLease& operator=(const NavigationGeometrySnapshotLease&) = delete;
		NavigationGeometrySnapshotLease(NavigationGeometrySnapshotLease&& other) noexcept;
		NavigationGeometrySnapshotLease& operator=(NavigationGeometrySnapshotLease&& other) noexcept;
		~NavigationGeometrySnapshotLease();
		[[nodiscard]] bool IsValid() const noexcept;

	private:
		friend class NavigationGeometrySnapshot;
		explicit NavigationGeometrySnapshotLease(std::shared_ptr<const NavigationGeometrySnapshot> snapshot) noexcept;
		std::shared_ptr<const NavigationGeometrySnapshot> snapshot;
	};

	struct NavigationGeometrySnapshotCreateResult
	{
		bool accepted = false;
		std::string reason;
		std::shared_ptr<const NavigationGeometrySnapshot> snapshot;
	};

	// Snapshots own copied source values and a compact BVH. Worker code can retain
	// this object without reading a live component, ResourceManager pointer, or a
	// provider-owned cooked buffer.
	class TERRAENGINE_API NavigationGeometrySnapshot final : public std::enable_shared_from_this<NavigationGeometrySnapshot>
	{
	public:
		[[nodiscard]] static NavigationGeometrySnapshotCreateResult Create(
			const NavigationGeometrySnapshotDescriptor& descriptor, const NavigationGeometryDocument& document);
		[[nodiscard]] const NavigationGeometrySnapshotDescriptor& Descriptor() const noexcept;
		[[nodiscard]] const std::vector<NavigationGeometryTriangleRecord>& Triangles() const noexcept;
		[[nodiscard]] const std::vector<NavigationGeometryPrimitive>& Primitives() const noexcept;
		[[nodiscard]] const NavigationGeometryBvh& Bvh() const noexcept;
		[[nodiscard]] const NavigationGeometryBounds& Bounds() const noexcept;
		[[nodiscard]] std::uint64_t ContentFingerprint() const noexcept;
		[[nodiscard]] NavigationGeometrySnapshotLease AcquireLease(NavigationGeometryLeaseLimits limits) const;
		[[nodiscard]] bool CanRetire() const noexcept;

	private:
		friend class NavigationGeometrySnapshotLease;
		NavigationGeometrySnapshot() = default;
		void ReleaseLease() const noexcept;
		NavigationGeometrySnapshotDescriptor descriptor{};
		std::vector<NavigationGeometryTriangleRecord> triangles;
		std::vector<NavigationGeometryPrimitive> primitives;
		NavigationGeometryBvh bvh;
		NavigationGeometryBounds bounds{};
		std::uint64_t contentFingerprint = 0U;
		mutable std::atomic<std::uint32_t> leaseCount{0U};
	};
}
