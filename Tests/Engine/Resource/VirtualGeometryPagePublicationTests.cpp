// VirtualGeometryPagePublicationTests.cpp - batch rollback and conflict contracts.

#include "Resource/VirtualGeometryPagePublication.h"
#include "TestHarness.h"

namespace TerraEngine::Tests
{
	namespace
	{
		Resource::VirtualGeometryPagePublicationCandidate MakeCandidate(
			const TerraEngine::UUID& assetId,
			std::uint32_t pageId,
			std::uint64_t allocationId)
		{
			Resource::VirtualGeometryPagePublicationCandidate candidate;
			candidate.candidateId = allocationId + 10u;
			candidate.publicationGeneration = 1u;
			candidate.key.asset.assetId = assetId;
			candidate.key.asset.sourceHash = 1u;
			candidate.key.asset.buildRecipeHash = 1u;
			candidate.key.pageId = pageId;
			candidate.key.generations = { 1u, 1u, 1u, 1u, 1u };
			candidate.key.pageContentGeneration = 1u;
			candidate.payload = { 1u, 1u, 1u, true, true };
			candidate.hierarchy = { { 0u }, 0u, true, true };
			candidate.descriptor = { allocationId, 0u, 0u, true };
			candidate.dependenciesVerified = true;
			candidate.residencyTransitionVerified = true;
			return candidate;
		}
	}

	void RegisterVirtualGeometryPagePublicationTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Streaming.BatchPublicationRollsBackOnInvalidRow",
			[](TestContext& context)
			{
				const auto asset = TerraEngine::GenerateUUID();
				auto candidate = MakeCandidate(asset, 2u, 1u);
				auto invalid = MakeCandidate(asset, 3u, 2u);
				invalid.payload.decodedBlocksVerified = false;
				const auto result = Resource::PublishVirtualGeometryPagesAtomically({}, { candidate, invalid }, 5u);
				context.Expect(!result.accepted,
					"One invalid row must reject the complete protected publication batch.");
				context.Expect(result.visible.empty(),
					"Rejected batches must not leak an earlier candidate into visibility.");
			});

		registry.Add("VirtualGeometry.Streaming.BatchPublicationRejectsCrossRowAllocationConflict",
			[](TestContext& context)
			{
				const auto asset = TerraEngine::GenerateUUID();
				const auto first = MakeCandidate(asset, 1u, 77u);
				const auto conflicting = MakeCandidate(asset, 2u, 77u);
				const auto result = Resource::PublishVirtualGeometryPagesAtomically({}, { first, conflicting }, 8u);
				context.Expect(!result.accepted,
					"Two rows cannot expose the same descriptor allocation in one atomic batch.");
				context.Expect(result.visible.empty(),
					"Cross-row conflict rejection must retain the entire prior visible set.");
			});

		registry.Add("VirtualGeometry.Streaming.BatchPublicationCannotStealVisibleAllocation",
			[](TestContext& context)
			{
				const auto asset = TerraEngine::GenerateUUID();
				const auto existing = MakeCandidate(asset, 1u, 91u);
				const auto published = Resource::PublishVirtualGeometryPagesAtomically({}, { existing }, 9u);
				const auto stealing = MakeCandidate(asset, 2u, 91u);
				const auto rejected = Resource::PublishVirtualGeometryPagesAtomically(
					published.visible, { stealing }, 10u);
				context.Expect(!rejected.accepted,
					"A new page cannot reuse an allocation still owned by another visible page.");
				context.ExpectEqual(static_cast<std::size_t>(1u), rejected.visible.size(),
					"Allocation conflict rejection must retain the complete prior publication set.");
			});

		registry.Add("VirtualGeometry.Streaming.BatchPublicationCannotRetireAnotherVisiblePage",
			[](TestContext& context)
			{
				const auto asset = TerraEngine::GenerateUUID();
				const auto existing = MakeCandidate(asset, 1u, 101u);
				const auto published = Resource::PublishVirtualGeometryPagesAtomically({}, { existing }, 11u);
				auto candidate = MakeCandidate(asset, 2u, 102u);
				candidate.retirements = { { 101u, 12u } };
				const auto rejected = Resource::PublishVirtualGeometryPagesAtomically(
					published.visible, { candidate }, 12u);
				context.Expect(!rejected.accepted,
					"A page transaction cannot retire the allocation owned by another visible page.");
				context.ExpectEqual(static_cast<std::size_t>(1u), rejected.visible.size(),
					"Cross-page retirement conflict must retain the prior visible set.");
			});
	}
} // namespace TerraEngine::Tests
