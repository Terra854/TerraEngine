// PhysicsQueries.h - bounded, provider-neutral spatial query contracts.
//
// This layer intentionally has no physics-provider headers. A backend converts a
// prepared request to its own query API, then returns engine-owned candidate hits
// to PhysicsQueryManager for filtering, canonical ordering, and safe retention.

#pragma once

#include "Core/Export.h"
#include "PhysicsSettings.h"
#include "PhysicsTypes.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Physics {
	// A body-type mask lets gameplay ask broad questions such as "only dynamic
	// bodies" without exposing provider object-layer identifiers.
	enum class PhysicsQueryBodyTypeMask : std::uint8_t {
		None = 0U,
		Static = 1U << 0U,
		Dynamic = 1U << 1U,
		Kinematic = 1U << 2U,
		All = (1U << 0U) | (1U << 1U) | (1U << 2U)
	};

	constexpr PhysicsQueryBodyTypeMask operator|(PhysicsQueryBodyTypeMask left, PhysicsQueryBodyTypeMask right) noexcept {
		return static_cast<PhysicsQueryBodyTypeMask>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
	}

	constexpr bool HasFlag(PhysicsQueryBodyTypeMask value, PhysicsQueryBodyTypeMask flag) noexcept {
		return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0U;
	}

	enum class PhysicsQueryRejectionReason : std::uint8_t {
		None,
		InvalidRequest,
		BatchLimit,
		StaleWorld,
		Expired,
		Cancelled,
		StaleHandle,
		ResultUnavailable
	};

	// Casts may stop at the first provider hit, retain the closest hit, or retain
	// a calibrated bounded all-hit set. The value is provider-neutral and is
	// honored by both immediate and prepared query paths.
	enum class PhysicsQueryHitMode : std::uint8_t {
		Any,
		Closest,
		All
	};

	// Filters use only stable engine identities. Empty UUID and invalid body
	// handles mean "do not exclude", avoiding optional provider-side pointers.
	struct PhysicsQueryFilter {
		std::uint32_t collisionMask = 0xFFFFFFFFU;
		PhysicsQueryBodyTypeMask bodyTypes = PhysicsQueryBodyTypeMask::All;
		TerraEngine::UUID excludedObjectId{};
		PhysicsGeneration excludedObjectGeneration = 0U;
		PhysicsBodyHandle excludedBody;
		bool includeSensors = false;
	};

	// The request is copied into a prepared batch. It may safely originate on a
	// worker because it owns no scene or provider memory.
	struct PhysicsQueryRequest {
		PhysicsWorldHandle world;
		PhysicsQueryKind kind = PhysicsQueryKind::RayCast;
		PhysicsQueryHitMode hitMode = PhysicsQueryHitMode::Closest;
		PhysicsPose pose;
		PhysicsVector3 direction{1.0F, 0.0F, 0.0F};
		PhysicsVector3 shapeExtent;
		float distance = 0.0F;
		std::uint32_t maximumHits = 1U;
		PhysicsQueryFilter filter;
		PhysicsTick submittedTick = 0U;
		// Zero asks the manager to use the settings-derived retention deadline.
		PhysicsTick expiryTick = 0U;
		std::uint64_t cancellationToken = 0U;
		std::string correlationId;
	};

	// Hits contain enough stable context to be useful after backend data has been
	// discarded. Material and surface identifiers stay engine-owned asset data.
	struct PhysicsQueryHit {
		PhysicsBodyHandle body;
		TerraEngine::UUID objectId{};
		PhysicsGeneration objectGeneration = 0U;
		PhysicsGeneration componentGeneration = 0U;
		TerraEngine::UUID materialId{};
		std::string surfaceTag;
		PhysicsVector3 position;
		PhysicsVector3 normal;
		float distance = 0.0F;
		float fraction = 0.0F;
		std::uint32_t subShapeKey = 0U;
		std::uint8_t collisionLayer = 0U;
		PhysicsMotionType motionType = PhysicsMotionType::Static;
		bool sensor = false;
	};

	// The key is deliberately public so all backend adapters use one definition
	// of duplicate identity rather than each provider inventing its own policy.
	struct PhysicsQueryHitDedupKey {
		PhysicsBodyHandle body;
		TerraEngine::UUID objectId{};
		PhysicsGeneration objectGeneration = 0U;
		std::uint32_t subShapeKey = 0U;

		auto operator<=>(const PhysicsQueryHitDedupKey&) const = default;
	};

	struct PhysicsQueryResult {
		PhysicsQueryHandle handle;
		PhysicsWorldHandle world;
		PhysicsTick tick = 0U;
		PhysicsResultStatus status = PhysicsResultStatus::Accepted;
		PhysicsQueryRejectionReason rejectionReason = PhysicsQueryRejectionReason::None;
		bool truncated = false;
		std::string correlationId;
		std::string diagnostic;
		// The shared const vector makes retained results cheap to copy while keeping
		// consumer code unable to alter observed hit ordering or contents.
		std::shared_ptr<const std::vector<PhysicsQueryHit>> hits;
	};

	struct PhysicsQueryReceipt {
		bool accepted = false;
		PhysicsQueryHandle handle;
		PhysicsQueryRejectionReason rejectionReason = PhysicsQueryRejectionReason::None;
		std::string diagnostic;
	};

	struct PhysicsQueryBatchSubmission {
		bool accepted = false;
		PhysicsQueryRejectionReason rejectionReason = PhysicsQueryRejectionReason::None;
		std::vector<PhysicsQueryReceipt> receipts;
	};

	struct PhysicsPreparedQuery {
		PhysicsQueryHandle handle;
		PhysicsQueryRequest request;
	};

	struct PhysicsPreparedQueryBatch {
		PhysicsWorldHandle world;
		PhysicsTick tick = 0U;
		std::vector<PhysicsPreparedQuery> queries;
		std::vector<PhysicsQueryResult> rejected;
		std::size_t reservedResultHitCapacity = 0U;
	};

	struct PhysicsQueryDiagnostics {
		std::uint64_t acceptedCount = 0U;
		std::uint64_t completedCount = 0U;
		std::uint64_t cancelledCount = 0U;
		std::uint64_t expiredCount = 0U;
		std::uint64_t staleWorldCount = 0U;
		std::uint64_t rejectedLimitCount = 0U;
		std::size_t queuedDepth = 0U;
		std::size_t preparedDepth = 0U;
		std::size_t retainedResultCount = 0U;
		std::size_t peakLiveQueryCount = 0U;
		std::size_t peakRetainedHitCount = 0U;
		std::size_t maximumLiveQueryCount = 0U;
	};

	// This class provides the engine-owned lifetime boundary around query work.
	// It does not execute a spatial query itself; the PhysicsSystem gives prepared
	// values to a backend and later calls Complete with converted engine hits.
	class TERRAENGINE_API PhysicsQueryManager final {
	public:
		explicit PhysicsQueryManager(PhysicsSettings settings = PhysicsSettings::Defaults());
		~PhysicsQueryManager();

		PhysicsQueryManager(const PhysicsQueryManager&) = delete;
		PhysicsQueryManager& operator=(const PhysicsQueryManager&) = delete;

		PhysicsQueryReceipt Submit(PhysicsQueryRequest request, PhysicsWorldHandle activeWorld, PhysicsTick currentTick);
		PhysicsQueryBatchSubmission SubmitBatch(std::span<const PhysicsQueryRequest> requests, PhysicsWorldHandle activeWorld, PhysicsTick currentTick);
		bool Cancel(PhysicsQueryHandle handle, PhysicsWorldHandle activeWorld);
		PhysicsPreparedQueryBatch Prepare(PhysicsWorldHandle activeWorld, PhysicsTick currentTick);
		bool Complete(
			const PhysicsPreparedQuery& prepared,
			PhysicsWorldHandle activeWorld,
			PhysicsTick completionTick,
			PhysicsResultStatus status,
			std::span<const PhysicsQueryHit> candidates,
			bool providerTruncated = false,
			std::string diagnostic = {});
		PhysicsQueryResult Read(PhysicsQueryHandle handle, PhysicsWorldHandle activeWorld, PhysicsTick currentTick) const;
		void Expire(PhysicsWorldHandle activeWorld, PhysicsTick currentTick);
		void ResetForWorld(PhysicsWorldHandle activeWorld);
		PhysicsQueryDiagnostics GetDiagnostics() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};

	TERRAENGINE_API PhysicsQueryHitDedupKey DedupKey(const PhysicsQueryHit& hit) noexcept;
	TERRAENGINE_API bool IsStableHitOrder(const PhysicsQueryHit& left, const PhysicsQueryHit& right) noexcept;
	TERRAENGINE_API std::string_view ToString(PhysicsQueryRejectionReason reason);
}
