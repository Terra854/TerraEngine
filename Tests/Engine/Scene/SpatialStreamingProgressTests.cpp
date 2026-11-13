#include "TestHarness.h"
#include "Resource/ResourceManager.h"
#include "Scene/SpatialIndex.h"
#include "Scene/WorldStreamingSystem.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace Scene;
		SpatialObjectRecord ProgressRecord(std::uint8_t id, float center, bool isStatic = false) {
			SpatialObjectRecord record;
			record.objectId.fill(id);
			record.entity = static_cast<entt::entity>(id);
			record.isStatic = isStatic;
			record.revision = 1U;
			record.localBounds = MakeAABB(glm::vec3(center - 0.25F), glm::vec3(center + 0.25F), SpatialBoundsSource::Authored);
			return record;
		}
		SpatialAABB ProgressBounds(float center, float extent = 2.0F) {
			return MakeAABB(glm::vec3(center - extent), glm::vec3(center + extent), SpatialBoundsSource::Authored);
		}
		struct RegionProgressFixture final {
			WorldStreamingSystem& streaming = WorldStreamingSystem::Instance();
			RegionProgressFixture() { streaming.Initialize(); streaming.SetBudget({64U, 16U}); }
			~RegionProgressFixture() { streaming.Shutdown(); streaming.SetBudget({64U, 16U}); }
			WorldRegionDirectoryEntry Prepared(TestContext& context) {
				// Directory entries outlive Initialize/Shutdown, so every case uses
				// its own sparse key rather than clearing another producer's entries.
				static std::atomic<std::int64_t> serial{9'000'000'000LL};
				WorldRegionDirectoryEntry entry;
				entry.key = {serial.fetch_add(1), 0, 0};
				entry.artifactPath = "Regions/ReviewProgress.teregion";
				entry.contentHash.assign(64U, 'a');
				entry.revision = 1U;
				context.Expect(streaming.Directory().Upsert(entry), "A uniquely owned sparse directory entry must be accepted.");
				for (const auto state : {WorldRegionState::Requested, WorldRegionState::Loading, WorldRegionState::Prepared})
					context.Expect(streaming.Directory().Transition(entry.key, state), "The fixture must use real region preparation transitions.");
				return *streaming.Directory().Find(entry.key);
			}
			void Queue(const WorldRegionDirectoryEntry& entry, bool cancelled = false) {
				auto snapshot = std::make_shared<WorldRegionSnapshot>();
				snapshot->key = entry.key;
				snapshot->directoryRevision = entry.revision;
				snapshot->contentHash = entry.contentHash;
				streaming.SubmitPreparedRegion({entry.key, entry.revision, 1U, cancelled, {}, std::move(snapshot)});
			}
			void MakeQueuedRevisionStale(TestContext& context, const WorldRegionDirectoryEntry& entry) {
				auto newer = entry;
				++newer.revision;
				newer.contentHash.assign(64U, 'b');
				context.Expect(streaming.Directory().Upsert(newer), "The real directory must replace the queued candidate's observed revision.");
			}
		};
	}
	void RegisterSpatialStreamingProgressTests(TestRegistry& registry) {
		registry.Add("Engine.SpatialProgress.LastDuplicateWinsBeforePartitioning", [](TestContext& context) {
			SpatialIndex index;
			auto first = ProgressRecord(231U, -50.0F, true);
			auto winner = ProgressRecord(231U, 50.0F, false);
			winner.entity = static_cast<entt::entity>(232U);
			index.Rebuild({first, winner});
			const auto found = index.QueryAABB(ProgressBounds(0.0F, 100.0F));
			context.ExpectEqual(std::size_t{1}, found.hits.size(), "One UUID must produce exactly one logical hit.");
			context.Expect(found.hits.front().entity == winner.entity && found.hits.front().bounds.min == winner.localBounds.min, "The last UUID occurrence must win consistently across static/movable partitions.");
			const auto diagnostics = index.GetDiagnosticsSnapshot();
			context.Expect(diagnostics.indexedObjectCount == 1U && diagnostics.staticObjectCount == 0U && diagnostics.dynamicObjectCount == 1U, "Metadata, membership, and diagnostic counts must describe the same winning record.");
		});
		registry.Add("Engine.SpatialProgress.DuplicateRemovalLeavesNoStaleNodeIds", [](TestContext& context) {
			SpatialIndex index;
			std::vector<SpatialObjectRecord> anchors;
			for (unsigned id = 1U; id <= 32U; ++id) anchors.push_back(ProgressRecord(static_cast<std::uint8_t>(id), -90.0F + static_cast<float>(id) * 5.0F));
			auto records = anchors;
			records.push_back(ProgressRecord(233U, -100.0F));
			records.push_back(ProgressRecord(233U, 100.0F));
			index.Rebuild(records);
			// Refresh is the production removal seam. The split tree is retained;
			// querying every former location must not dereference an erased map ID.
			index.Refresh(anchors);
			for (const float location : {-100.0F, 100.0F}) {
				const auto result = index.QueryAABB(ProgressBounds(location));
				context.Expect(result.hits.empty(), "Removing the winning UUID must clear every former duplicate membership.");
			}
			context.ExpectEqual(anchors.size(), index.QueryAABB(ProgressBounds(0.0F, 200.0F)).hits.size(), "All unrelated anchors must survive the incremental removal.");
		});
		registry.Add("Engine.StreamingProgress.StaleHeadCannotStarveLaterCompletion", [](TestContext& context) {
			RegionProgressFixture fixture;
			fixture.streaming.SetBudget({64U, 1U});
			const auto stale = fixture.Prepared(context);
			fixture.Queue(stale);
			fixture.MakeQueuedRevisionStale(context, stale);
			const auto before = fixture.streaming.CurrentResidency();
			context.Expect(!fixture.streaming.PublishAtFrameEnd(ResourceManager::Instance(), 1U), "A wholly stale prepared batch must reject.");
			const auto rejected = fixture.streaming.CurrentResidency();
			context.Expect(rejected.generation == before.generation && rejected.residentRegions.empty(), "Rejection must leave the complete published residency unchanged.");
			const auto diagnostics = fixture.streaming.GetPublicationDiagnostics();
			context.Expect(diagnostics.rejectedCount == 1U && diagnostics.recentRejections.size() == 1U
				&& diagnostics.recentRejections.front().key == stale.key
				&& diagnostics.recentRejections.front().observedDirectoryRevision == 1U
				&& diagnostics.recentRejections.front().currentDirectoryRevision == 2U,
				"The rejected stale revision must remain observable without failing its newer directory entry.");
			const auto valid = fixture.Prepared(context);
			fixture.Queue(valid);
			context.Expect(fixture.streaming.PublishAtFrameEnd(ResourceManager::Instance(), 2U), "Rejected head work must release its bounded slot so a later valid completion can publish.");
			const auto after = fixture.streaming.CurrentResidency();
			context.Expect(after.residentRegions.contains(valid.key) && !after.residentRegions.contains(stale.key), "Only the valid completion may become visible at the later boundary.");
			const auto newer = fixture.streaming.Directory().Find(stale.key);
			context.Expect(newer && newer->revision == 2U && newer->lastFailure.empty(), "Rejecting old work may not mark the newer directory revision failed.");
		});
		registry.Add("Engine.StreamingProgress.StaleHeadCannotStarveRetirement", [](TestContext& context) {
			RegionProgressFixture fixture;
			const auto resident = fixture.Prepared(context);
			fixture.Queue(resident);
			context.Expect(fixture.streaming.PublishAtFrameEnd(ResourceManager::Instance(), 1U), "The reader fixture must first publish a real region.");
			const auto retainedReader = fixture.streaming.CurrentResidency();
			const auto stale = fixture.Prepared(context);
			fixture.Queue(stale);
			fixture.MakeQueuedRevisionStale(context, stale);
			fixture.streaming.Update(0.0F);
			context.Expect(!fixture.streaming.PublishAtFrameEnd(ResourceManager::Instance(), 2U), "The first stale publication must reject atomically.");
			context.Expect(fixture.streaming.CurrentResidency().residentRegions.contains(resident.key), "A rejected batch may not partially retire a visible region.");
			context.Expect(fixture.streaming.PublishAtFrameEnd(ResourceManager::Instance(), 3U), "Retirement must progress after invalid queued work has been rejected.");
			const auto after = fixture.streaming.CurrentResidency();
			context.Expect(!after.residentRegions.contains(resident.key)
				&& std::find(after.invalidatedRegions.begin(), after.invalidatedRegions.end(), resident.key) != after.invalidatedRegions.end(), "The next boundary must publish the retirement and its invalidation together.");
			context.Expect(retainedReader.residentRegions.at(resident.key)->key == resident.key, "A reader retaining the prior immutable snapshot must remain valid after retirement.");
		});
		registry.Add("Engine.StreamingProgress.RejectionEvidenceIsBounded", [](TestContext& context) {
			RegionProgressFixture fixture;
			const auto entry = fixture.Prepared(context);
			for (std::uint64_t frame = 1U; frame <= 70U; ++frame) {
				fixture.Queue(entry, true);
				context.Expect(!fixture.streaming.PublishAtFrameEnd(ResourceManager::Instance(), frame), "Cancelled-only work must never publish a partial residency.");
			}
			const auto diagnostics = fixture.streaming.GetPublicationDiagnostics();
			context.Expect(diagnostics.rejectedCount >= 70U && diagnostics.recentRejections.size() == 64U, "Copied rejection evidence must retain a bounded recent window plus its cumulative count.");
			context.Expect(!diagnostics.recentRejections.back().reason.empty() && fixture.streaming.CurrentResidency().residentRegions.empty(), "Bounded diagnostics must retain a concrete reason without exposing invalid regions.");
		});
		registry.Add("Engine.StreamingProgress.MixedRejectedBatchKeepsValidWorkQueued", [](TestContext& context) {
			RegionProgressFixture fixture;
			const auto first = fixture.Prepared(context);
			const auto invalid = fixture.Prepared(context);
			const auto last = fixture.Prepared(context);
			fixture.Queue(first);
			fixture.Queue(invalid, true);
			fixture.Queue(last);
			const auto before = fixture.streaming.CurrentResidency();
			context.Expect(!fixture.streaming.PublishAtFrameEnd(ResourceManager::Instance(), 1U), "A mixed batch containing rejected work must reject its whole visibility boundary.");
			const auto rejected = fixture.streaming.CurrentResidency();
			context.Expect(rejected.generation == before.generation && rejected.residentRegions.empty(), "Valid neighbors of rejected work may not leak into the rejected publication.");
			context.Expect(fixture.streaming.PublishAtFrameEnd(ResourceManager::Instance(), 2U), "Valid queued work must survive removal of its invalid neighbor.");
			const auto after = fixture.streaming.CurrentResidency();
			context.Expect(after.residentRegions.size() == 2U && after.residentRegions.contains(first.key)
				&& after.residentRegions.contains(last.key) && !after.residentRegions.contains(invalid.key), "The next protected boundary must publish exactly the retained valid work.");
		});
		registry.Add("Engine.StreamingProgress.LeaseDefersThenAllowsRetirement", [](TestContext& context) {
			RegionProgressFixture fixture;
			const auto entry = fixture.Prepared(context);
			fixture.Queue(entry);
			context.Expect(fixture.streaming.PublishAtFrameEnd(ResourceManager::Instance(), 1U), "The lease control must publish a real resident first.");
			context.Expect(fixture.streaming.Directory().AcquireLease(entry.key, {"review-reader", 1U, 1000U, 0U}), "A real frame-bounded lease must be accepted.");
			fixture.streaming.Update(0.0F);
			context.Expect(!fixture.streaming.PublishAtFrameEnd(ResourceManager::Instance(), 2U)
				&& fixture.streaming.CurrentResidency().residentRegions.contains(entry.key), "An active reader lease must protect its resident region.");
			context.Expect(fixture.streaming.Directory().ReleaseLease(entry.key, "review-reader", 1U), "The fixture must release its own exact lease generation.");
			context.Expect(fixture.streaming.PublishAtFrameEnd(ResourceManager::Instance(), 3U)
				&& !fixture.streaming.CurrentResidency().residentRegions.contains(entry.key), "Retirement must succeed after the protecting lease is released.");
		});
	}
}
