// AnimationResidencyTests.cpp - bounded animation residency and streaming contracts.

#include "TestHarness.h"

#include "Animation/Streaming/AnimationStreaming.h"
#include "Animation/Assets/MotionDatabaseAsset.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourceResidency.h"
#include "ThreadPool/ThreadPool.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		UUID AnimationId(std::uint8_t seed)
		{
			UUID id{};
			id[0] = seed;
			return id;
		}

		constexpr std::array<Resource::ResidencyAssetType, 7u> kAnimationResidencyTypes = {
			Resource::ResidencyAssetType::Skeleton,
			Resource::ResidencyAssetType::AnimationClip,
			Resource::ResidencyAssetType::AnimationController,
			Resource::ResidencyAssetType::AnimationMask,
			Resource::ResidencyAssetType::RetargetProfile,
			Resource::ResidencyAssetType::MotionDatabase,
			Resource::ResidencyAssetType::RagdollProfile
		};

		Animation::AnimationStreamRequest StreamRequest(
			UUID id,
			Animation::AnimationStreamPayloadKind kind,
			std::uint32_t block,
			std::uint64_t bytes,
			std::uint64_t generation,
			bool gameplayCritical = false,
			bool prefetch = false,
			std::int32_t priority = 0)
		{
			return {id, kind, block, bytes, generation, gameplayCritical, prefetch, priority};
		}
	}

	void RegisterAnimationResidencyTests(TestRegistry& registry)
	{
		registry.Add("Animation.Residency.EveryAssetTypeAccountsAndDiagnosesExactBytes", [](TestContext& context) {
			Resource::AnimationResidencyTracker tracker;
			std::uint64_t expectedBytes = 0u;
			for (std::size_t index = 0u; index < kAnimationResidencyTypes.size(); ++index)
			{
				const std::uint64_t bytes = 32u + index;
				expectedBytes += bytes;
				const Resource::AnimationResidencyRequest request{
					AnimationId(static_cast<std::uint8_t>(10u + index)),
					kAnimationResidencyTypes[index], bytes,
					Resource::StreamingPriorityClass::Normal, false, false, 1u};
				const Resource::AnimationResidencyDecision queued = tracker.Request(request);
				context.Expect(queued.accepted && queued.state == Resource::ResidencyState::Loading
					&& queued.safeFallbackRequired,
					"Every cooked animation asset type should enter the shared asynchronous request path with fallback active.");
				context.Expect(tracker.Complete(request.assetId, request.requestGeneration, true),
					"Each matching animation generation should publish at the protected completion boundary.");
			}

			const Resource::AnimationResidencyDiagnostics diagnostics = tracker.BuildDiagnostics(expectedBytes);
			context.Expect(diagnostics.types.size() == kAnimationResidencyTypes.size()
				&& diagnostics.residentBytes == expectedBytes && diagnostics.evictableBytes == expectedBytes,
				"Residency diagnostics should report exact aggregate bytes and one stable bucket for every animation type.");
			for (std::size_t index = 0u; index < diagnostics.types.size(); ++index)
			{
				context.Expect(diagnostics.types[index].assetType == kAnimationResidencyTypes[index]
					&& diagnostics.types[index].trackedGenerations == 1u
					&& diagnostics.types[index].residentGenerations == 1u
					&& diagnostics.types[index].residentBytes == 32u + index,
					"Each animation diagnostic bucket should preserve its typed generation and byte accounting.");
			}
		});

		registry.Add("Animation.Residency.VisibilityAndAuthorityHaveSeparateRetention", [](TestContext& context) {
			Resource::AnimationResidencyTracker tracker;
			const UUID visibleId = AnimationId(30u);
			const Resource::AnimationResidencyRequest visible{
				visibleId, Resource::ResidencyAssetType::AnimationClip, 16u,
				Resource::StreamingPriorityClass::Background, false, false, 1u};
			context.Expect(tracker.Request(visible).accepted && tracker.Complete(visibleId, 1u, true)
				&& tracker.Retain(visibleId, 1u, Resource::AnimationResidencyUsage::Visible),
				"A visible evaluator should explicitly retain the generation it reads.");
			context.Expect(tracker.ApplyBudget(0u).empty(),
				"A visible retained generation must not disappear during the frame that consumes it.");
			context.Expect(tracker.Release(visibleId, 1u, Resource::AnimationResidencyUsage::Visible)
				&& tracker.ApplyBudget(0u).size() == 1u,
				"Once visibility ends, a non-authoritative background generation may be evicted and use its safe fallback.");

			const UUID offscreenId = AnimationId(31u);
			const Resource::AnimationResidencyRequest offscreenAuthority{
				offscreenId, Resource::ResidencyAssetType::AnimationController, 24u,
				Resource::StreamingPriorityClass::Background, false, true, 1u};
			tracker.Request(offscreenAuthority);
			tracker.Complete(offscreenId, 1u, true);

			const UUID consumersId = AnimationId(32u);
			const Resource::AnimationResidencyRequest consumers{
				consumersId, Resource::ResidencyAssetType::MotionDatabase, 40u,
				Resource::StreamingPriorityClass::Background, false, false, 1u};
			tracker.Request(consumers);
			tracker.Complete(consumersId, 1u, true);
			constexpr Resource::AnimationResidencyUsage authorityUsages =
				Resource::AnimationResidencyUsage::Events
				| Resource::AnimationResidencyUsage::RootMotion
				| Resource::AnimationResidencyUsage::Physics
				| Resource::AnimationResidencyUsage::Recording
				| Resource::AnimationResidencyUsage::Replication
				| Resource::AnimationResidencyUsage::Rollback;
			context.Expect(tracker.Retain(consumersId, 1u, authorityUsages),
				"Every authoritative offscreen consumer should be expressible as an explicit typed retain.");
			context.Expect(tracker.ApplyBudget(0u).empty(),
				"Offscreen authority and gameplay consumers must remain resident even when visual work is culled.");
			const auto retained = tracker.Query(consumersId, 1u);
			context.Expect(retained.has_value() && retained->retainedForAuthority
				&& retained->state == Resource::ResidencyState::Pinned,
				"The query should distinguish authoritative retention from ordinary visibility.");
			context.Expect(tracker.Release(consumersId, 1u, authorityUsages)
				&& tracker.ApplyBudget(0u).size() == 1u,
				"A generation becomes evictable only after all authoritative consumer retains are released.");
			context.Expect(tracker.Query(offscreenId, 1u)->state == Resource::ResidencyState::Pinned,
				"Request-level offscreen authority should continue protecting its active generation.");
		});

		registry.Add("Animation.Residency.CancellationSupersessionAndBoundsRejectStalePublication", [](TestContext& context) {
			Resource::AnimationResidencyTracker tracker(2u);
			const UUID id = AnimationId(40u);
			Resource::AnimationResidencyRequest request{id, Resource::ResidencyAssetType::AnimationClip, 64u,
				Resource::StreamingPriorityClass::Background, false, false, 1u};
			context.Expect(tracker.Request(request).accepted, "The initial generation should queue.");
			request.requestGeneration = 2u;
			context.Expect(tracker.Request(request).accepted && !tracker.Complete(id, 1u, true),
				"A newer request should supersede unfinished older IO and reject its stale completion.");
			context.Expect(tracker.Cancel(id, 2u) && !tracker.Complete(id, 2u, true),
				"Cancellation should win atomically over a late worker completion.");

			const Resource::AnimationResidencyRequest replacement{
				AnimationId(41u), Resource::ResidencyAssetType::Skeleton, 8u,
				Resource::StreamingPriorityClass::Normal, false, false, 1u};
			context.Expect(tracker.Request(replacement).accepted,
				"Released generations should be pruned so bounded tracking can continue.");
			Resource::AnimationResidencyRequest conflicting = replacement;
			conflicting.byteSize = 9u;
			context.Expect(!tracker.Request(conflicting).accepted,
				"Reusing one generation with different immutable request data must fail closed.");
			const Resource::AnimationResidencyDiagnostics diagnostics = tracker.BuildDiagnostics(0u);
			context.Expect(diagnostics.cancelledRequests >= 2u && diagnostics.staleCompletions >= 2u
				&& diagnostics.rejectedRequests >= 1u,
				"Bounded diagnostics should expose supersession, cancellation, stale workers, and conflicts.");
		});

		registry.Add("Animation.Streaming.ClipAndMotionBlocksUseBoundedFallbackCancellationAndRetention", [](TestContext& context) {
			Animation::AnimationStreamingCoordinator coordinator(2u, 4u, 64u, 16u);
			const UUID clipId = AnimationId(50u);
			Animation::AnimationStreamRequest clip = StreamRequest(
				clipId, Animation::AnimationStreamPayloadKind::ClipSegment, 0u, 16u, 1u);
			const Animation::AnimationStreamDecision queued = coordinator.Request(clip);
			context.Expect(queued.accepted && queued.state == Animation::AnimationStreamState::Requested
				&& queued.useSafeFallback
				&& queued.fallbackKind == Animation::AnimationStreamFallbackKind::ReferencePose,
				"A missing clip segment should queue asynchronously while reference pose remains safe.");
			Animation::AnimationStreamRequest malformedCompletion = clip;
			malformedCompletion.byteSize = 17u;
			context.Expect(!coordinator.Complete(malformedCompletion, true)
				&& coordinator.Complete(clip, true),
				"Completion must match all immutable request data before the segment can publish.");

			clip.generation = 2u;
			const Animation::AnimationStreamDecision replacement = coordinator.Request(clip);
			context.Expect(replacement.fallbackKind == Animation::AnimationStreamFallbackKind::LastResidentGeneration
				&& replacement.fallbackGeneration == 1u,
				"A replacement segment should keep its prior resident generation as the fallback.");
			context.Expect(coordinator.Cancel(clip) && !coordinator.Complete(clip, true)
				&& coordinator.Query(StreamRequest(clipId, Animation::AnimationStreamPayloadKind::ClipSegment,
					0u, 16u, 1u))->activeGeneration,
				"Cancelling replacement IO must leave the old resident segment active and reject late publication.");

			const UUID motionId = AnimationId(51u);
			const Animation::AnimationStreamRequest motion = StreamRequest(
				motionId, Animation::AnimationStreamPayloadKind::MotionDatabaseBlock, 3u, 20u, 1u);
			const Animation::AnimationStreamDecision motionQueued = coordinator.Request(motion);
			context.Expect(motionQueued.fallbackKind == Animation::AnimationStreamFallbackKind::DeterministicMotionSearch
				&& coordinator.Complete(motion, false),
				"Unavailable motion-database blocks should use deterministic search fallback and preserve it after failure.");

			const UUID criticalId = AnimationId(52u);
			const Animation::AnimationStreamRequest critical = StreamRequest(
				criticalId, Animation::AnimationStreamPayloadKind::ClipSegment, 1u, 24u, 1u, true);
			coordinator.Request(critical);
			coordinator.Complete(critical, true);
			const UUID ordinaryId = AnimationId(53u);
			const Animation::AnimationStreamRequest ordinary = StreamRequest(
				ordinaryId, Animation::AnimationStreamPayloadKind::ClipSegment, 2u, 8u, 1u);
			coordinator.Request(ordinary);
			coordinator.Complete(ordinary, true);
			const std::vector<Animation::AnimationStreamDecision> released = coordinator.ApplyResidentBudget(0u);
			context.Expect(released.size() >= 2u && coordinator.Query(critical)->state == Animation::AnimationStreamState::Resident,
				"Pressure may release ordinary blocks and old inactive generations, but gameplay-critical blocks stay resident.");
			const Animation::AnimationStreamingDiagnostics diagnostics = coordinator.BuildDiagnostics();
			context.Expect(diagnostics.staleCompletions >= 2u && diagnostics.failedPayloads == 1u
				&& diagnostics.cancelledPayloads >= 1u && diagnostics.protectedBytes == critical.byteSize,
				"Streaming diagnostics should expose fallback, cancellation, failure, stale completion, and protected bytes.");
			context.Expect(coordinator.SetGameplayCritical(critical, false)
				&& !coordinator.ApplyResidentBudget(0U).empty()
				&& coordinator.Query(critical)->state == Animation::AnimationStreamState::Missing,
				"Protected bindings must be able to demote a retired gameplay dependency so bounded pressure can release it.");
		});

		registry.Add("Animation.Streaming.PrefetchWindowsAreAtomicAndStrictlyBounded", [](TestContext& context) {
			Animation::AnimationStreamingCoordinator atomicBytes(4u, 4u, 15u, 16u);
			const std::array byteOverflow = {
				StreamRequest(AnimationId(60u), Animation::AnimationStreamPayloadKind::ClipSegment, 0u, 8u, 1u, false, true),
				StreamRequest(AnimationId(60u), Animation::AnimationStreamPayloadKind::ClipSegment, 1u, 8u, 1u, false, true)
			};
			const auto rejected = atomicBytes.RequestBatch(byteOverflow);
			context.Expect(rejected.size() == 2u && !rejected[0].accepted && !rejected[1].accepted
				&& atomicBytes.Snapshot().empty(),
				"A prefetch window that exceeds the byte bound should queue no partial work.");

			Animation::AnimationStreamingCoordinator horizon(2u, 4u, 64u, 16u);
			const std::array acceptedWindow = {
				StreamRequest(AnimationId(61u), Animation::AnimationStreamPayloadKind::ClipSegment, 0u, 8u, 1u, false, true),
				StreamRequest(AnimationId(61u), Animation::AnimationStreamPayloadKind::ClipSegment, 1u, 8u, 1u, false, true)
			};
			const auto accepted = horizon.RequestBatch(acceptedWindow);
			context.Expect(accepted.size() == 2u && accepted[0].accepted && accepted[1].accepted,
				"A complete prefetch window inside all bounds should queue atomically.");
			const auto beyondHorizon = horizon.Request(StreamRequest(
				AnimationId(61u), Animation::AnimationStreamPayloadKind::ClipSegment, 2u, 8u, 1u, false, true));
			context.Expect(!beyondHorizon.accepted && horizon.BuildDiagnostics().pendingPrefetchPayloads == 2u,
				"The prefetch horizon should remain bounded while leaving demand-request capacity explicit.");

			const std::array duplicateWindow = {acceptedWindow[0], acceptedWindow[0]};
			Animation::AnimationStreamingCoordinator duplicates(4u, 4u, 64u, 16u);
			const auto duplicateRejected = duplicates.RequestBatch(duplicateWindow);
			context.Expect(!duplicateRejected[0].accepted && !duplicateRejected[1].accepted
				&& duplicates.Snapshot().empty(),
				"Duplicate identities should reject an entire prefetch transaction without partial queue state.");
		});

		registry.Add("Animation.Streaming.ReadsRealBoundedCookedRangesBeforeProtectedPublication", [](TestContext& context) {
			const std::filesystem::path path = std::filesystem::temp_directory_path()
				/ ("terra-stream-range-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()) + ".teanim");
			const std::array<std::uint8_t, 8U> source{{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}};
			{
				std::ofstream output(path, std::ios::binary | std::ios::trunc);
				output.write(reinterpret_cast<const char*>(source.data()),
					static_cast<std::streamsize>(source.size()));
			}

			Animation::AnimationStreamingCoordinator coordinator;
			Animation::AnimationStreamRequest request = StreamRequest(
				AnimationId(75U), Animation::AnimationStreamPayloadKind::ClipSegment,
				0U, 3U, 1U);
			request.logicalPath = path;
			request.byteOffset = 2U;
			request.expectedContentFingerprint =
				Animation::ComputeAnimationStreamContentFingerprint(
					std::span<const std::uint8_t>(source).subspan(2U, 3U));
			Animation::AnimationStreamReadJob job = coordinator.QueueRead(
				ResourceManager::Instance(), request);
			const Animation::AnimationStreamReadResult result = job.completion.get();
			context.Expect(job.decision.accepted && result.Succeeded()
				&& result.bytes == std::vector<std::uint8_t>({3U, 4U, 5U}),
				"The worker path must read only the requested bytes from a real cooked artifact.");
			context.Expect(coordinator.Query(request)->useSafeFallback
				&& coordinator.LoadResidencySnapshot()->Find(request) == nullptr
				&& coordinator.PublishReadResult(result)
				&& !coordinator.Query(request)->useSafeFallback,
				"Read bytes remain hidden behind fallback until the caller publishes the exact generation.");
			const std::shared_ptr<const Animation::AnimationStreamingResidencySnapshot> published =
				coordinator.LoadResidencySnapshot();
			const Animation::AnimationResidentPayload* payload = published->Find(request);
			context.Expect(payload != nullptr && payload->bytes
				&& *payload->bytes == std::vector<std::uint8_t>({3U, 4U, 5U}),
				"Protected publication must expose an immutable snapshot containing the exact checked bytes.");

			Animation::AnimationStreamRequest cancelled = request;
			cancelled.generation = 2U;
			Animation::AnimationStreamReadJob cancelledJob = coordinator.QueueRead(
				ResourceManager::Instance(), cancelled);
			context.Expect(cancelledJob.decision.accepted && coordinator.Cancel(cancelled),
				"A replacement generation must remain cancellable until protected publication.");
			const Animation::AnimationStreamReadResult late = cancelledJob.completion.get();
			context.Expect(!coordinator.PublishReadResult(late)
				&& coordinator.LoadResidencySnapshot()->Find(request) != nullptr
				&& coordinator.LoadResidencySnapshot()->Find(cancelled) == nullptr,
				"Cancellation must reject late worker bytes without changing the prior immutable snapshot.");

			std::error_code error;
			std::filesystem::remove(path, error);
		});

		registry.Add("Animation.Streaming.EvictedGenerationRequeuesExactImmutableRange", [](TestContext& context) {
			const std::filesystem::path path = std::filesystem::temp_directory_path()
				/ ("terra-stream-reactivate-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()) + ".teanim");
			const std::array<std::uint8_t, 4U> source{{9U, 8U, 7U, 6U}};
			{
				std::ofstream output(path, std::ios::binary | std::ios::trunc);
				output.write(reinterpret_cast<const char*>(source.data()),
					static_cast<std::streamsize>(source.size()));
			}

			Animation::AnimationStreamingCoordinator coordinator;
			Animation::AnimationStreamRequest request = StreamRequest(
				AnimationId(77U), Animation::AnimationStreamPayloadKind::ClipSegment,
				0U, source.size(), 1U);
			request.logicalPath = path;
			request.expectedContentFingerprint =
				Animation::ComputeAnimationStreamContentFingerprint(source);
			Animation::AnimationStreamReadJob first = coordinator.QueueRead(
				ResourceManager::Instance(), request);
			const Animation::AnimationStreamReadResult firstResult = first.completion.get();
			context.Expect(first.decision.accepted
				&& coordinator.PublishReadResult(firstResult)
				&& coordinator.ApplyResidentBudget(0U).size() == 1U
				&& coordinator.Query(request)->state == Animation::AnimationStreamState::Missing,
				"Pressure should expose an exact same-generation payload as recoverable Missing state.");

			Animation::AnimationStreamReadJob reactivated = coordinator.QueueRead(
				ResourceManager::Instance(), request);
			context.Expect(reactivated.decision.accepted
				&& reactivated.decision.state == Animation::AnimationStreamState::Requested
				&& coordinator.Cancel(request),
				"An exact immutable evicted range must queue fresh IO and remain cancellable.");
			const Animation::AnimationStreamReadResult late = reactivated.completion.get();
			const Animation::AnimationStreamDecision cancelledRetry = coordinator.Request(request);
			context.Expect(!coordinator.PublishReadResult(late)
				&& !cancelledRetry.accepted
				&& cancelledRetry.state == Animation::AnimationStreamState::Cancelled
				&& cancelledRetry.reason.find("newer asset generation") != std::string::npos,
				"Cancellation stays terminal for that generation and rejects its late fresh-token completion.");

			std::error_code error;
			std::filesystem::remove(path, error);
		});

		registry.Add("Animation.Streaming.MotionDatabaseV1PublishesExactStrmBlockRanges", [](TestContext& context) {
			const bool initializedPool = ThreadPool::instance().getThreadCount() == 0U;
			if(initializedPool) ThreadPool::instance().initialize(1U);
			const std::filesystem::path path = std::filesystem::temp_directory_path()
				/ ("terra-motion-block-range-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()) + ".temotiondb");
			Animation::MotionDatabaseAsset database;
			database.assetId.value = AnimationId(76U);
			database.compatibilityFingerprint = 0x1234567812345678ULL;
			database.sourceFingerprint = 0x8765432187654321ULL;
			database.quantizationScales.fill(1.0F);
			Animation::MotionDatabaseSample sample;
			sample.stableSampleId = 1U;
			sample.sourceClipId = 2U;
			database.samples.push_back(sample);
			database.featureSchemaFingerprint =
				Animation::ComputeMotionDatabaseSchemaFingerprint(database);
			context.Expect(Animation::RebuildMotionDatabaseStorageMetadata(database)
				== Animation::AnimationStatus::Ok,
				"The motion streaming seam requires a valid resident-block metadata fixture.");
			std::vector<std::uint8_t> source;
			context.Expect(Animation::SerializeMotionDatabaseAsset(database, source)
				== Animation::AnimationStatus::Ok,
				"The motion streaming seam must use a valid cooked v1 database.");
			Animation::MotionDatabaseStreamBlockRange expectedRange;
			context.Expect(Animation::ResolveMotionDatabaseStreamBlockRange(
				source, 0U, expectedRange) == Animation::AnimationStatus::Ok,
				"Current v1 serialization must expose its exact checked STRM block range.");
			{
				std::ofstream output(path, std::ios::binary | std::ios::trunc);
				output.write(reinterpret_cast<const char*>(source.data()),
					static_cast<std::streamsize>(source.size()));
			}

			ResourceManager& resources = ResourceManager::Instance();
			const UUID loadedId = resources.LoadAnimationAssetAsync(
				path, AssetType::MotionDatabase).get();
			const Animation::AnimationGeneration generation =
				resources.GetAnimationAssetRuntimeGeneration(
					loadedId, AssetType::MotionDatabase);
			const AnimationCookedRangeTable ranges =
				resources.DescribeMotionDatabaseBlockRanges(loadedId, generation);
			context.Expect(loadedId == database.assetId.value
				&& ranges.status == AnimationCookedRangeStatus::Ok
				&& ranges.blocks.size() == 1U
				&& ranges.blocks.front().blockIndex == expectedRange.blockIndex
				&& ranges.blocks.front().byteOffset == expectedRange.absoluteByteOffset
				&& ranges.blocks.front().byteSize == expectedRange.byteSize,
				"Current v1 STRM metadata must publish the exact checked physical block range.");
			resources.DeleteResource(loadedId, AssetType::MotionDatabase);

			std::error_code error;
			std::filesystem::remove(path, error);
			if(initializedPool) ThreadPool::instance().shutdown();
		});
	}
}
