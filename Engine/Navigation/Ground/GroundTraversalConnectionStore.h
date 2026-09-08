// GroundTraversalConnectionStore.h - immutable explicit ground-link snapshots.
//
// Connections are authored values. This store never retains scene pointers or
// invokes gameplay callbacks; worker/query code reads copied revisions through
// an immutable snapshot acquired after protected publication.
#pragma once

#include "Components/Navigation/NavigationConnectionComponent.h"
#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationTypes.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace TerraEngine::Navigation::Ground
{
	struct GroundConnectionSupportCertificate
	{
		bool endpointsSupported = false;
		bool clearanceSupported = false;
		float measuredClearanceMetres = 0.0F;

		[[nodiscard]] static GroundConnectionSupportCertificate FlatGround(float clearanceMetres) noexcept
		{
			return {true, clearanceMetres >= 0.0F, clearanceMetres};
		}

		[[nodiscard]] static GroundConnectionSupportCertificate UnsupportedGap() noexcept
		{
			return {false, false, 0.0F};
		}

		[[nodiscard]] static GroundConnectionSupportCertificate InsufficientClearance() noexcept
		{
			return {true, false, 0.0F};
		}
	};

	struct GroundTraversalConnectionUpsertResult
	{
		bool accepted = false;
		std::uint64_t publishedRevision = 0U;
		std::string reason;
	};

	struct GroundTraversalConnectionRecord
	{
		NavigationOwnerHandle owner{};
		NavigationConnectionComponent authored{};
		GroundConnectionSupportCertificate support{};
		std::uint64_t publishedRevision = 0U;
	};

	class GroundTraversalConnectionSnapshot;

	struct GroundTraversalRouteValidation
	{
		bool usable = false;
		NavigationReason reason = NavigationReason::None;
	};

	struct TERRAENGINE_API GroundTraversalRouteResult
	{
		NavigationOutcome outcome = NavigationOutcome::UnavailableCoverage;
		NavigationReason reason = NavigationReason::MissingSupport;
		TerraEngine::UUID connectionId = TerraEngine::EMPTY_UUID;
		std::uint64_t sourceRevision = 0U;

		[[nodiscard]] GroundTraversalRouteValidation ValidateAgainst(const GroundTraversalConnectionSnapshot& snapshot) const;
	};

	class TERRAENGINE_API GroundTraversalConnectionSnapshot
	{
	public:
		GroundTraversalConnectionSnapshot() = default;

		[[nodiscard]] bool Contains(const TerraEngine::UUID& connectionId) const noexcept;
		[[nodiscard]] bool CanTraverse(const TerraEngine::UUID& connectionId,
			NavigationConnectionDirection requestedDirection, std::uint32_t capabilityMask) const noexcept;
		[[nodiscard]] GroundTraversalRouteResult FindRouteUsing(const TerraEngine::UUID& connectionId,
			NavigationConnectionDirection requestedDirection, std::uint32_t capabilityMask,
			const NavigationProfile& profile) const noexcept;
		[[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }

	private:
		friend class GroundTraversalConnectionStore;
		friend struct GroundTraversalRouteResult;
		explicit GroundTraversalConnectionSnapshot(std::shared_ptr<const std::vector<GroundTraversalConnectionRecord>> records,
			std::uint64_t revision)
			: records_(std::move(records)), revision_(revision) {}

		[[nodiscard]] const GroundTraversalConnectionRecord* Find(const TerraEngine::UUID& connectionId) const noexcept;
		std::shared_ptr<const std::vector<GroundTraversalConnectionRecord>> records_;
		std::uint64_t revision_ = 0U;
	};

	class TERRAENGINE_API GroundTraversalConnectionStore
	{
	public:
		using SupportCertificate = GroundConnectionSupportCertificate;

		GroundTraversalConnectionStore();
		GroundTraversalConnectionStore(const GroundTraversalConnectionStore&) = delete;
		GroundTraversalConnectionStore& operator=(const GroundTraversalConnectionStore&) = delete;

		[[nodiscard]] GroundTraversalConnectionUpsertResult Upsert(
			NavigationOwnerHandle owner,
			const NavigationConnectionComponent& authored,
			SupportCertificate support);
		[[nodiscard]] bool Disable(const TerraEngine::UUID& connectionId);
		[[nodiscard]] bool Remove(const TerraEngine::UUID& connectionId);
		[[nodiscard]] GroundTraversalConnectionSnapshot AcquireSnapshot() const;

	private:
		mutable std::mutex publicationMutex_;
		std::shared_ptr<const std::vector<GroundTraversalConnectionRecord>> records_;
		std::uint64_t revision_ = 0U;
	};
}
