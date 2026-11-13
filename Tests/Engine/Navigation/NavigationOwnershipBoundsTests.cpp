// Real bounded accounting and generation-store ownership; no timing/RSS oracle.
#include "TestHarness.h"
#include "Navigation/NavigationBudget.h"
#include "Navigation/NavigationStores.h"
#include <algorithm>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Navigation;
		template<typename Store, typename Record>
		void VerifyRepeatedRevocation(TestContext& context, bool expired)
		{
			Store store{2u};
			const NavigationWorldHandle world{NavigationWorldId{109u}, 3u};
			const NavigationOwnerHandle oldOwner{world, NavigationOwnerId{41u}, 1u};
			const NavigationOwnerHandle nextOwner{world, NavigationOwnerId{42u}, 2u};
			Record original{}; original.intentGeneration = 11u; original.expiryTick = expired ? 1u : 0u;
			const auto old = store.Create(oldOwner, original);
			context.Expect(old.Succeeded(), "The real bounded store must admit its first owner record.");
			if(expired) context.Expect(store.Expire(1u) == 1u, "The expired control must pass through actual store expiry.");
			const auto baselineBytes = store.MemoryBytes();
			context.Expect(store.RevokeOwner(oldOwner) == 1u, "The first owner revocation must retire exactly its real record.");
			std::size_t repeatedReported = 0u;
			for(unsigned attempt = 0u; attempt < 128u; ++attempt) repeatedReported += store.RevokeOwner(oldOwner);
			context.Expect(store.RevokeOwner(nextOwner) == 0u, "An unrelated owner cannot revoke the retired record.");
			Record firstRecord{}; firstRecord.intentGeneration = 21u;
			Record secondRecord{}; secondRecord.intentGeneration = 22u;
			const auto first = store.Create(nextOwner, firstRecord);
			const auto second = store.Create(nextOwner, secondRecord);
			context.Expect(first.Succeeded() && second.Succeeded(), "Both remaining bounded slots must be available to the next owner.");
			const auto firstRead = store.Read(first.handle), secondRead = store.Read(second.handle);
			context.Expect(first.handle.slotIndex != second.handle.slotIndex && firstRead.HasRecord() && secondRead.HasRecord()
				&& firstRead.record->intentGeneration == 21u && secondRead.record->intentGeneration == 22u
				&& store.AliveCount() == 2u,
				"Repeated revocation must not duplicate a reusable slot or overwrite the first replacement. repeatedReported="
				+ std::to_string(repeatedReported));
			context.Expect(repeatedReported == 0u && store.MemoryBytes() <= baselineBytes,
				"Revocation must be idempotent and retain only the bounded free-slot allocation.");
			context.Expect(store.Create(nextOwner, Record{}).status == NavigationHandleStatus::CapacityExceeded,
				"A duplicate free-list entry must not admit more live records than configured capacity.");
			context.Expect(store.Release(first.handle), "Only the released live replacement slot may become reusable.");
			const auto reused = store.Create(nextOwner, firstRecord);
			context.Expect(reused.Succeeded() && reused.handle.slotIndex == first.handle.slotIndex
				&& reused.handle.slotGeneration > first.handle.slotGeneration
				&& store.Read(second.handle).HasRecord() && store.AliveCount() == 2u,
				"Normal release/reuse must advance generation while preserving the other live record.");
		}
	}

	void RegisterNavigationOwnershipBoundsTests(TestRegistry& registry)
	{
		registry.Add("Navigation.OwnershipBounds.SharedRejectionHistoryAndAccountingStayBounded", [](TestContext& context) {
			NavigationBudgetLimits limits{}; limits.geometryBytes = 64u;
			NavigationBudget budget{limits};
			auto first = budget.TryReserveShared(NavigationBudgetCategory::Geometry, 64u, "retained-source");
			auto lease = budget.TryReserveShared(NavigationBudgetCategory::Geometry, 64u, "retained-source");
			context.Expect(first.accepted && lease.accepted && budget.UsedBytes(NavigationBudgetCategory::Geometry) == 64u,
				"Shared immutable source leases must charge their real allocation only once.");
			for(unsigned attempt = 0u; attempt < 80u; ++attempt) {
				auto rejected = budget.TryReserve(NavigationBudgetCategory::Geometry, 1u, "ordinary-control");
				context.Expect(!rejected.accepted && !rejected.reason.empty(), "Actual full-budget rejection must provide a reason.");
			}
			context.Expect(budget.RejectionReasons().size() == 64u, "The established ordinary rejection-history bound is 64 entries.");
			for(unsigned attempt = 0u; attempt < 96u; ++attempt) {
				auto rejected = budget.TryReserveShared(NavigationBudgetCategory::Geometry, 1u, "overflow-" + std::to_string(attempt));
				context.Expect(!rejected.accepted && !rejected.reason.empty()
					&& budget.UsedBytes(NavigationBudgetCategory::Geometry) == 64u,
					"Rejected shared allocations must leave actual retained ownership accounting unchanged.");
				if(attempt % 8u == 0u) {
					auto ordinary = budget.TryReserve(NavigationBudgetCategory::Geometry, 1u, "interleaved-control");
					context.Expect(!ordinary.accepted, "Interleaved ordinary rejection cannot consume capacity.");
				}
			}
			const auto history = budget.RejectionReasons();
			first.Release();
			context.Expect(budget.UsedBytes(NavigationBudgetCategory::Geometry) == 64u, "The remaining strong lease must retain the shared charge.");
			lease.Release();
			context.Expect(budget.UsedBytes(NavigationBudgetCategory::Geometry) == 0u, "The final lease must release the shared allocation exactly once.");
			auto readmitted = budget.TryReserveShared(NavigationBudgetCategory::Geometry, 64u, "fresh-source");
			context.Expect(readmitted.accepted && budget.UsedBytes(NavigationBudgetCategory::Geometry) == 64u,
				"Rejected reservation pressure must not prevent later valid admission.");
			readmitted.Release();
			context.Expect(history.size() <= 64u && std::all_of(history.begin(), history.end(), [](const auto& reason) { return !reason.empty(); }),
				"Shared and ordinary failures must share the existing bounded diagnostic history. retained=" + std::to_string(history.size()));
		});
		for(const bool expired : {false, true}) {
			registry.Add(expired ? "Navigation.OwnershipBounds.ExpiredRequestsRevokeOnceAndReuseDistinctSlots"
				: "Navigation.OwnershipBounds.RequestsRevokeOnceAndReuseDistinctSlots", [expired](TestContext& context) {
				VerifyRepeatedRevocation<NavigationRequestStore, NavigationRequestRecord>(context, expired);
			});
			registry.Add(expired ? "Navigation.OwnershipBounds.ExpiredRoutesRevokeOnceAndReuseDistinctSlots"
				: "Navigation.OwnershipBounds.RoutesRevokeOnceAndReuseDistinctSlots", [expired](TestContext& context) {
				VerifyRepeatedRevocation<NavigationRouteStore, NavigationRouteRecord>(context, expired);
			});
		}
	}
}
