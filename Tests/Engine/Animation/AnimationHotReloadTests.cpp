// AnimationHotReloadTests.cpp - atomic last-known-good reload contracts.

#include "TestHarness.h"

#include "Animation/Streaming/AnimationHotReload.h"

#include <array>
#include <algorithm>

namespace TerraEngine::Tests
{
	namespace
	{
		UUID ReloadId(std::uint8_t seed)
		{
			UUID id{};
			id[0] = seed;
			return id;
		}

		Animation::AnimationReloadCandidate Candidate(
			UUID id,
			std::uint64_t generation,
			std::uint64_t compatibility,
			Animation::AnimationReloadAssetType type = Animation::AnimationReloadAssetType::AnimationClip,
			bool parsed = true,
			bool dependenciesReady = true)
		{
			Animation::AnimationReloadCandidate candidate;
			candidate.assetId = id;
			candidate.generation = generation;
			candidate.compatibilityFingerprint = compatibility;
			candidate.parsed = parsed;
			candidate.dependenciesReady = dependenciesReady;
			candidate.assetType = type;
			candidate.contentFingerprint = generation * 101u;
			return candidate;
		}

		bool ContainsId(const std::vector<UUID>& values, const UUID& id)
		{
			return std::find(values.begin(), values.end(), id) != values.end();
		}

		struct PrecommitProbe
		{
			bool accept = false;
			std::uint32_t callCount = 0u;
		};

		bool RunPrecommitProbe(void* context) noexcept
		{
			auto& probe = *static_cast<PrecommitProbe*>(context);
			++probe.callCount;
			return probe.accept;
		}

		struct RetirementProbe
		{
			UUID assetId = EMPTY_UUID;
			std::uint64_t generation = 0U;
			std::uint32_t callCount = 0U;
		};

		void RecordRetirement(void* context, const UUID& assetId,
			std::uint64_t generation) noexcept
		{
			auto& probe = *static_cast<RetirementProbe*>(context);
			probe.assetId = assetId;
			probe.generation = generation;
			++probe.callCount;
		}
	}

	void RegisterAnimationHotReloadTests(TestRegistry& registry)
	{
		registry.Add("Animation.HotReload.EveryAssetTypePublishesTypedGeneration", [](TestContext& context) {
			constexpr std::array types = {
				Animation::AnimationReloadAssetType::Skeleton,
				Animation::AnimationReloadAssetType::AnimationClip,
				Animation::AnimationReloadAssetType::AnimationController,
				Animation::AnimationReloadAssetType::AnimationMask,
				Animation::AnimationReloadAssetType::RetargetProfile,
				Animation::AnimationReloadAssetType::MotionDatabase,
				Animation::AnimationReloadAssetType::RagdollProfile
			};
			Animation::AnimationHotReloadPublisher publisher;
			for (std::size_t index = 0u; index < types.size(); ++index)
			{
				const UUID id = ReloadId(static_cast<std::uint8_t>(10u + index));
				const auto outcome = publisher.Publish(Candidate(id, 1u, 0x100u + index, types[index]));
				const auto generation = publisher.QueryGeneration(id);
				context.Expect(outcome.published && outcome.activeGeneration == 1u
					&& generation.has_value() && generation->assetType == types[index]
					&& generation->generation == 1u,
					"Every cooked animation asset type should publish a typed immutable generation.");
			}
			context.Expect(publisher.Snapshot().size() == types.size(),
				"The bounded snapshot should expose every active animation generation.");

			auto invalidType = Candidate(ReloadId(29u), 1u, 0x999u);
			invalidType.assetType = static_cast<Animation::AnimationReloadAssetType>(255u);
			context.Expect(!publisher.Publish(invalidType).published,
				"Unknown serialized reload asset types must fail closed instead of entering the live table.");
		});

		registry.Add("Animation.HotReload.CompatibleSwitchesInstancesAndFailuresRetainLastGood", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher;
			const UUID id = ReloadId(30u);
			const UUID instanceId = ReloadId(31u);
			context.Expect(publisher.Publish(Candidate(id, 1u, 0x88u)).published
				&& publisher.BindInstance(instanceId, id, 1u)
				&& publisher.RetainGeneration(id, 1u),
				"The initial complete generation should publish, bind, and accept a frame/GPU/managed retain.");

			const auto corrupt = publisher.Publish(Candidate(
				id, 2u, 0x88u, Animation::AnimationReloadAssetType::AnimationClip, false));
			context.Expect(corrupt.retainedLastKnownGood && corrupt.activeGeneration == 1u,
				"A corrupt reload must preserve the active generation.");
			const auto incompatible = publisher.Publish(Candidate(id, 2u, 0x99u));
			context.Expect(incompatible.retainedLastKnownGood && incompatible.activeGeneration == 1u,
				"An incompatible candidate must not switch active instances.");

			const auto compatible = publisher.Publish(Candidate(id, 2u, 0x88u));
			const auto instance = publisher.QueryInstance(instanceId);
			context.Expect(compatible.published && compatible.switchedInstanceCount == 1u
				&& instance.has_value() && instance->generation == 2u
				&& instance->switchBoundary == compatible.publicationBoundary,
				"A compatible generation and every bound instance should switch at one protected boundary.");
			context.Expect(publisher.RetireBefore(2u) == 0u,
				"An old generation retained by frame, GPU, or managed work must not retire early.");
			RetirementProbe retirement;
			context.Expect(publisher.ReleaseGeneration(id, 1u)
				&& publisher.RetireBefore(2u, RecordRetirement, &retirement) == 1u
				&& retirement.callCount == 1U && retirement.assetId == id
				&& retirement.generation == 1U,
				"The old immutable generation may retire after its explicit owner releases it.");
		});

		registry.Add("Animation.HotReload.PrepareHasNoVisibilityAndCommitSwapsWholeBatch", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher;
			const UUID firstId = ReloadId(32u);
			const UUID secondId = ReloadId(33u);
			const UUID firstInstance = ReloadId(34u);
			const UUID secondInstance = ReloadId(35u);
			const std::array initial = {
				Candidate(firstId, 1u, 0x101u),
				Candidate(secondId, 1u, 0x202u)
			};
			context.Expect(publisher.PublishBatch(initial, 1u)[0].published
				&& publisher.BindInstance(firstInstance, firstId, 1u)
				&& publisher.BindInstance(secondInstance, secondId, 1u),
				"The initial generations and instance bindings should be live before staged reload preparation.");

			const std::array replacements = {
				Candidate(firstId, 2u, 0x101u),
				Candidate(secondId, 2u, 0x202u)
			};
			auto prepared = publisher.PrepareBatch(replacements, 7u);
			const auto preparedOutcomes = prepared.Outcomes();
			context.Expect(prepared.Accepted() && !prepared.Committed()
				&& prepared.PublicationBoundary() == 7u && preparedOutcomes.size() == 2u
				&& preparedOutcomes[0].prepared && preparedOutcomes[1].prepared
				&& !preparedOutcomes[0].published && !preparedOutcomes[1].published,
				"Preparation should expose accepted outcomes while making no generation visible.");
			context.Expect(publisher.Query(firstId).activeGeneration == 1u
				&& publisher.Query(secondId).activeGeneration == 1u
				&& publisher.QueryInstance(firstInstance)->generation == 1u
				&& publisher.QueryInstance(secondInstance)->generation == 1u,
				"Allocation-heavy preparation must leave every live generation and binding unchanged.");

			const auto commitStatus = publisher.CommitPreparedBatch(prepared);
			context.Expect(commitStatus == Animation::AnimationReloadCommitStatus::Committed
				&& prepared.Committed() && prepared.Outcomes()[0].published
				&& prepared.Outcomes()[1].published,
				"The protected commit should publish the complete accepted batch.");
			context.Expect(publisher.Query(firstId).activeGeneration == 2u
				&& publisher.Query(secondId).activeGeneration == 2u
				&& publisher.QueryInstance(firstInstance)->generation == 2u
				&& publisher.QueryInstance(secondInstance)->generation == 2u
				&& publisher.QueryInstance(firstInstance)->switchBoundary == 7u
				&& publisher.QueryInstance(secondInstance)->switchBoundary == 7u,
				"All generations and instance bindings should switch together at one boundary.");
		});

		registry.Add("Animation.HotReload.StalePreparationCannotOverwriteInterveningState", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher;
			const UUID assetId = ReloadId(36u);
			const UUID instanceId = ReloadId(37u);
			context.Expect(publisher.PublishBatch(std::array{Candidate(assetId, 1u, 0x303u)}, 1u)[0].published,
				"A last-known-good generation should exist before stale-preparation testing.");

			auto prepared = publisher.PrepareBatch(
				std::array{Candidate(assetId, 2u, 0x303u)}, 2u);
			context.Expect(prepared.Accepted() && publisher.BindInstance(instanceId, assetId, 1u),
				"An intervening live binding mutation should advance the publisher preparation epoch.");
			const auto status = publisher.CommitPreparedBatch(prepared);
			const auto instance = publisher.QueryInstance(instanceId);
			context.Expect(status == Animation::AnimationReloadCommitStatus::RejectedStalePreparation
				&& prepared.CommitStatus() == Animation::AnimationReloadCommitStatus::RejectedStalePreparation
				&& !prepared.Outcomes()[0].published
				&& publisher.Query(assetId).activeGeneration == 1u
				&& instance.has_value() && instance->generation == 1u,
				"A stale prepared snapshot must neither publish nor erase intervening live state.");
		});

		registry.Add("Animation.HotReload.ExternalPrecommitFailurePreservesPublisherState", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher;
			const UUID assetId = ReloadId(38u);
			context.Expect(publisher.PublishBatch(std::array{Candidate(assetId, 1u, 0x404u)}, 1u)[0].published,
				"A live generation should exist before coordinated external commit testing.");

			auto prepared = publisher.PrepareBatch(
				std::array{Candidate(assetId, 2u, 0x404u)}, 2u);
			PrecommitProbe probe;
			const auto rejected = publisher.CommitPreparedBatch(prepared, &RunPrecommitProbe, &probe);
			context.Expect(rejected == Animation::AnimationReloadCommitStatus::RejectedExternalPrecommit
				&& probe.callCount == 1u && publisher.Query(assetId).activeGeneration == 1u
				&& !prepared.Outcomes()[0].published,
				"A failed external resource commit must leave the publisher's last-good state untouched.");

			probe.accept = true;
			const auto committed = publisher.CommitPreparedBatch(prepared, &RunPrecommitProbe, &probe);
			context.Expect(committed == Animation::AnimationReloadCommitStatus::Committed
				&& probe.callCount == 2u && publisher.Query(assetId).activeGeneration == 2u
				&& prepared.Outcomes()[0].published,
				"A later successful no-throw external precommit may join the same prepared protected boundary.");
		});

		registry.Add("Animation.HotReload.AtomicBatchInvalidatesDependentsAndRetiresByBoundary", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher;
			const UUID skeletonId = ReloadId(40u);
			const UUID clipId = ReloadId(41u);
			const UUID controllerId = ReloadId(42u);
			const UUID skeletonInstance = ReloadId(43u);
			const UUID clipInstance = ReloadId(44u);
			const std::array initial = {
				Candidate(skeletonId, 1u, 0xabcU, Animation::AnimationReloadAssetType::Skeleton),
				Candidate(clipId, 1u, 0xabcU, Animation::AnimationReloadAssetType::AnimationClip),
				Candidate(controllerId, 1u, 0xabcU, Animation::AnimationReloadAssetType::AnimationController)
			};
			const auto initialOutcomes = publisher.PublishBatch(initial, 5u);
			context.Expect(initialOutcomes.size() == 3u && initialOutcomes[0].published
				&& initialOutcomes[1].published && initialOutcomes[2].published,
				"A complete dependency set should become visible atomically at one explicit boundary.");
			context.Expect(publisher.RegisterDependency(clipId, skeletonId)
				&& publisher.RegisterDependency(controllerId, clipId)
				&& publisher.BindInstance(skeletonInstance, skeletonId, 1u)
				&& publisher.BindInstance(clipInstance, clipId, 1u)
				&& publisher.RetainGeneration(skeletonId, 1u),
				"Reload dependencies and active instances should be registered before source publication.");

			const auto skeletonReload = publisher.PublishBatch(
				std::array{Candidate(skeletonId, 2u, 0xabcU, Animation::AnimationReloadAssetType::Skeleton)}, 10u);
			context.Expect(skeletonReload.size() == 1u && skeletonReload[0].published
				&& skeletonReload[0].switchedInstanceCount == 1u
				&& ContainsId(skeletonReload[0].invalidatedDependents, clipId)
				&& ContainsId(skeletonReload[0].invalidatedDependents, controllerId),
				"Reloading a dependency should switch its instances and invalidate every transitive dependent not rebuilt in the batch.");
			const auto clipInvalidated = publisher.QueryGeneration(clipId);
			const auto controllerInvalidated = publisher.QueryGeneration(controllerId);
			context.Expect(clipInvalidated->dependencyInvalidated && controllerInvalidated->dependencyInvalidated,
				"Invalidated dependents should retain immutable last-good data but require safe evaluator fallback.");
			context.Expect(publisher.RetireThroughBoundary(9u) == 0u
				&& publisher.RetireThroughBoundary(10u) == 0u,
				"Retirement must wait for both the completed boundary and outstanding old-generation references.");
			context.Expect(publisher.ReleaseGeneration(skeletonId, 1u)
				&& publisher.RetireThroughBoundary(10u) == 1u,
				"Once references retire, the completed publication boundary may reclaim the replaced skeleton generation.");

			auto skeleton3 = Candidate(skeletonId, 3u, 0xabcU, Animation::AnimationReloadAssetType::Skeleton);
			auto clip2 = Candidate(clipId, 2u, 0xabcU, Animation::AnimationReloadAssetType::AnimationClip);
			clip2.dependencies.push_back({skeletonId, 3u, 0xabcU, true});
			const std::array rebuilt = {skeleton3, clip2};
			const auto rebuiltOutcomes = publisher.PublishBatch(rebuilt, 20u);
			context.Expect(rebuiltOutcomes.size() == 2u && rebuiltOutcomes[0].published
				&& rebuiltOutcomes[1].published
				&& !ContainsId(rebuiltOutcomes[0].invalidatedDependents, clipId)
				&& ContainsId(rebuiltOutcomes[0].invalidatedDependents, controllerId),
				"A dependent rebuilt in the same compatible batch should not be invalidated, while unreconstructed transitive users should be.");
			const auto rebuiltClip = publisher.QueryGeneration(clipId);
			const auto switchedClipInstance = publisher.QueryInstance(clipInstance);
			context.Expect(!rebuiltClip->dependencyInvalidated && rebuiltClip->generation == 2u
				&& switchedClipInstance->generation == 2u && switchedClipInstance->switchBoundary == 20u,
				"The rebuilt dependent and its active instance should switch together at the batch boundary.");
			context.Expect(publisher.RetireThroughBoundary(19u) == 0u
				&& publisher.RetireThroughBoundary(20u) == 2u,
				"Both replaced generations should stay immutable until the exact protected boundary completes.");
		});

		registry.Add("Animation.HotReload.ManualDependenciesSurviveUnspecifiedReloadAndDeclaredEmptyClears", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher;
			const UUID dependent = ReloadId(45u);
			const UUID dependency = ReloadId(46u);
			context.Expect(publisher.PublishBatch(std::array{
				Candidate(dependent, 1u, 0x606u), Candidate(dependency, 1u, 0x707u)}, 1u)[0].published
				&& publisher.RegisterDependency(dependent, dependency),
				"A manually registered dependency must exist before reload provenance is tested.");
			context.Expect(publisher.PublishBatch(std::array{Candidate(dependent, 2u, 0x606u)}, 2u)[0].published
				&& publisher.PublishBatch(std::array{Candidate(dependency, 2u, 0x707u)}, 3u)[0].published,
				"An unspecified legacy reload must preserve manual reverse dependency edges.");
			const auto preserved = publisher.QueryGeneration(dependent);
			context.Expect(preserved.has_value() && preserved->dependencyInvalidated,
				"Reloading a manually registered dependency must still invalidate its dependent after an unspecified reload.");
			context.Expect(publisher.UnregisterDependency(dependent, dependency),
				"The manual edge must be removed before compiler-owned reconciliation is tested independently.");
			auto compilerOwned = Candidate(dependent, 3u, 0x606u);
			compilerOwned.dependenciesAreCompleteDeclaration = true;
			compilerOwned.dependencies.push_back({dependency, 2u, 0x707u, true});
			context.Expect(publisher.PublishBatch(std::array{compilerOwned}, 4u)[0].published,
				"A complete compiler declaration must install its own reverse dependency edge.");
			auto declaredEmpty = Candidate(dependent, 4u, 0x606u);
			declaredEmpty.dependenciesAreCompleteDeclaration = true;
			context.Expect(publisher.PublishBatch(std::array{declaredEmpty}, 5u)[0].published
				&& publisher.PublishBatch(std::array{Candidate(dependency, 3u, 0x707u)}, 6u)[0].published,
				"A compiler-owned explicit empty declaration must replace prior outgoing dependencies.");
			const auto cleared = publisher.QueryGeneration(dependent);
			context.Expect(cleared.has_value() && !cleared->dependencyInvalidated,
				"A declared-empty dependency list must remove stale compiler-owned reverse edges.");
		});

		registry.Add("Animation.HotReload.RejectedBatchLeavesNoPartialLiveState", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher;
			const UUID firstId = ReloadId(50u);
			const UUID secondId = ReloadId(51u);
			const std::array initial = {Candidate(firstId, 1u, 0x11u), Candidate(secondId, 1u, 0x22u)};
			publisher.PublishBatch(initial, 1u);

			auto valid = Candidate(firstId, 2u, 0x11u);
			auto corrupt = Candidate(secondId, 2u, 0x22u);
			corrupt.parsed = false;
			const std::array mixed = {valid, corrupt};
			const auto mixedOutcomes = publisher.PublishBatch(mixed, 2u);
			context.Expect(!mixedOutcomes[0].published && !mixedOutcomes[1].published
				&& publisher.Query(firstId).activeGeneration == 1u
				&& publisher.Query(secondId).activeGeneration == 1u,
				"One corrupt member should reject the complete batch without exposing a partial generation.");

			auto missingDependency = Candidate(firstId, 2u, 0x11u);
			missingDependency.dependencies.push_back({ReloadId(99u), 1u, 0x33u, true});
			context.Expect(!publisher.PublishBatch(std::array{missingDependency}, 2u)[0].published
				&& publisher.Query(firstId).activeGeneration == 1u,
				"A missing required dependency should preserve all active last-known-good state.");
			context.Expect(!publisher.PublishBatch(std::array{valid}, 1u)[0].published,
				"A stale or reused publication boundary should fail without changing live state.");
			const std::array duplicate = {valid, valid};
			context.Expect(!publisher.PublishBatch(duplicate, 2u)[0].published
				&& publisher.Query(firstId).activeGeneration == 1u,
				"Duplicate candidate identities should reject an atomic batch.");

			auto incompatibleType = valid;
			incompatibleType.assetType = Animation::AnimationReloadAssetType::Skeleton;
			context.Expect(!publisher.PublishBatch(std::array{incompatibleType}, 2u)[0].published
				&& publisher.Query(firstId).activeGeneration == 1u,
				"Changing the asset type or compatibility identity must retain the prior generation.");
		});

		registry.Add("Animation.HotReload.TablesAndRetirementQueuesStayBounded", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher(1u, 1u, 1u, 1u);
			const UUID id = ReloadId(60u);
			const UUID other = ReloadId(61u);
			context.Expect(publisher.PublishBatch(std::array{Candidate(id, 1u, 0x55u)}, 1u)[0].published,
				"The first asset should fit the bounded live table.");
			context.Expect(!publisher.PublishBatch(std::array{Candidate(other, 1u, 0x66u)}, 2u)[0].published,
				"The publisher should reject a second live asset when its explicit asset bound is full.");
			context.Expect(publisher.BindInstance(ReloadId(62u), id, 1u)
				&& !publisher.BindInstance(ReloadId(63u), id, 1u),
				"The active-instance table should enforce its explicit bound.");
			context.Expect(publisher.RegisterDependency(id, other)
				&& !publisher.RegisterDependency(other, id),
				"The dependency graph should reject edges beyond its configured bound.");

			context.Expect(publisher.PublishBatch(std::array{Candidate(id, 2u, 0x55u)}, 3u)[0].published,
				"One replaced generation should fit the retirement queue.");
			context.Expect(!publisher.PublishBatch(std::array{Candidate(id, 3u, 0x55u)}, 4u)[0].published
				&& publisher.Query(id).activeGeneration == 2u,
				"A full old-generation queue should reject publication atomically until retirement advances.");
			context.Expect(publisher.RetireThroughBoundary(3u) == 1u
				&& publisher.PublishBatch(std::array{Candidate(id, 3u, 0x55u)}, 4u)[0].published,
				"Completing the protected retirement boundary should free bounded capacity for the next generation.");
		});
	}
}
