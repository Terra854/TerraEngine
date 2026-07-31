// VirtualGeometryPublicationContractTests.cpp - atomic visibility and rollback contracts.

#include "Resource/VirtualGeometryPagePublication.h"
#include "TestHarness.h"

namespace TerraEngine::Tests
{
	namespace
	{
		TerraEngine::Resource::VirtualGeometryPagePublicationCandidate MakeCandidate(std::uint64_t publicationGeneration)
		{
			using namespace TerraEngine::Resource;
			VirtualGeometryPagePublicationCandidate candidate;
			candidate.candidateId = 500u + publicationGeneration;
			candidate.publicationGeneration = publicationGeneration;
			candidate.key.asset.assetId = *TerraEngine::ParseUUID("11111111222233334444555555555555");
			candidate.key.asset.sourceHash = 0x101u;
			candidate.key.asset.buildRecipeHash = 0x202u;
			candidate.key.pageId = 7u;
			candidate.key.generations.geometryFormatGeneration = publicationGeneration;
			candidate.key.generations.hierarchyGeneration = publicationGeneration;
			candidate.key.generations.pageDirectoryGeneration = publicationGeneration;
			candidate.key.generations.residencyGeneration = publicationGeneration;
			candidate.key.generations.descriptorGeneration = publicationGeneration;
			candidate.key.pageContentGeneration = publicationGeneration;
			candidate.payload.storedByteCount = 4096u;
			candidate.payload.decodedByteCount = 8192u;
			candidate.payload.integrityValue = 0x1234u;
			candidate.payload.storedBytesVerified = true;
			candidate.payload.decodedBlocksVerified = true;
			candidate.hierarchy.clusterIds = { 10u, 11u };
			candidate.hierarchy.fallbackPageId = 0u;
			candidate.hierarchy.fallbackCoverageVerified = true;
			candidate.hierarchy.hierarchyAcyclic = true;
			candidate.descriptor.allocationId = 1000u + publicationGeneration;
			candidate.descriptor.bufferIndex = 4u;
			candidate.descriptor.descriptorIndex = 9u;
			candidate.descriptor.uploadCompleted = true;
			candidate.dependencyPageIds = { 0u };
			candidate.dependenciesVerified = true;
			candidate.residencyTransitionVerified = true;
			return candidate;
		}
	} // namespace

	void RegisterVirtualGeometryPublicationContractTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Publication.CompleteCandidatePublishesAtBoundary",
			[](TestContext &context)
			{
				using namespace TerraEngine::Resource;
				const auto result = PublishVirtualGeometryPageAtomically({}, MakeCandidate(2u), 40u);
				context.Expect(result.accepted, "A complete candidate should publish at the protected boundary.");
				context.Expect(result.visible.IsVisible(), "Accepted page bytes, hierarchy, descriptor, and residency must appear together.");
				context.ExpectEqual<std::uint64_t>(40u, result.visible.publicationFrame, "The accepted row must name its exact visibility boundary.");
			});

		registry.Add("VirtualGeometry.Publication.IncompleteCandidateRollsBackEveryField",
			[](TestContext &context)
			{
				using namespace TerraEngine::Resource;
				const auto first = PublishVirtualGeometryPageAtomically({}, MakeCandidate(2u), 40u);
				auto invalid = MakeCandidate(3u);
				invalid.hierarchy.fallbackCoverageVerified = false;
				invalid.descriptor.allocationId = 9999u;
				const auto rejected = PublishVirtualGeometryPageAtomically(first.visible, invalid, 41u);
				context.Expect(!rejected.accepted, "Missing fallback coverage must reject the complete transaction.");
				context.ExpectEqual<std::uint64_t>(
					first.visible.publicationGeneration, rejected.visible.publicationGeneration, "Rollback must keep the prior generation visible.");
				context.ExpectEqual<std::uint64_t>(first.visible.descriptor.allocationId,
					rejected.visible.descriptor.allocationId,
					"Rollback must not leak the candidate descriptor allocation.");
			});

		registry.Add("VirtualGeometry.Publication.StaleAndConflictingCandidatesAreAtomic",
			[](TestContext &context)
			{
				using namespace TerraEngine::Resource;
				const auto first = PublishVirtualGeometryPageAtomically({}, MakeCandidate(5u), 90u);
				auto stale = MakeCandidate(4u);
				const auto staleResult = PublishVirtualGeometryPageAtomically(first.visible, stale, 91u);
				context.Expect(!staleResult.accepted, "A stale publication must not replace the current row.");

				auto conflict = MakeCandidate(6u);
				conflict.key.pageId = 8u;
				const auto conflictResult = PublishVirtualGeometryPageAtomically(first.visible, conflict, 92u);
				context.Expect(!conflictResult.accepted, "A candidate for a different page cannot overwrite this publication slot.");
				context.ExpectEqual<std::uint32_t>(7u, conflictResult.visible.key.pageId, "Conflict rejection must preserve the prior page identity.");
			});

		registry.Add("VirtualGeometry.Publication.LiveAllocationCannotRetireWithCandidate",
			[](TestContext &context)
			{
				using namespace TerraEngine::Resource;
				auto candidate = MakeCandidate(8u);
				candidate.retirements.push_back({ candidate.descriptor.allocationId, 20u });
				const auto rejected = PublishVirtualGeometryPageAtomically({}, candidate, 100u);
				context.Expect(!rejected.accepted, "A transaction cannot expose an allocation while retiring it.");
				context.Expect(!rejected.visible.IsVisible(), "A rejected first publication must leave the slot missing.");
			});
	}
} // namespace TerraEngine::Tests
