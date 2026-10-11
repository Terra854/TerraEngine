// Pure native stream policies exercise copied owners and receipts. They do not
// prove Vulkan uploads, shader execution or completed GPU coverage.
#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer;
		using namespace Renderer::Vulkan;
		struct Input
		{
			ShadowBatchPlan batch;
			ShadowGroupedStreamOwner owner{1, 2, 3, 4, 5, 6, 1, 7};
			std::vector<ShadowBatchMember> members{3};
			std::vector<RenderInstanceRecord> expected{3}, stream{2};
			Input()
			{
				batch.view.rendererSessionGeneration = 1;
				batch.view.viewType = RenderViewType::Scene; batch.view.viewId = 1;
				batch.view.lightId[0] = 1; batch.view.allocationGeneration = 8;
				batch.view.extent = {1024, 1024}; batch.coverageComplete = true;
				batch.allSingletonIndices = {0, 1, 2}; batch.singletonIndices = {2};
				ShadowBatchGroup group; group.representativeIndex = 0;
				group.firstInstance = 0; group.instanceCount = 2;
				group.memberIndices = {0, 1}; group.taskGroupsX = 2;
				for(unsigned i = 0; i < 3; ++i)
				{
					members[i].sourceOrdinal = i; members[i].objectId[0] = i + 1;
					members[i].lifetimeGeneration = 1; members[i].segmentId = i + 1;
					members[i].meshletCount = 1;
					members[i].key.meshId[0] = 1; members[i].key.materialId[0] = 2;
					members[i].key.shaderId[0] = 3;
					members[i].key.meshGeneration = members[i].key.materialGeneration = 1;
					members[i].key.shaderGeneration = members[i].key.residencyGeneration = 1;
					members[i].key.pipeline = members[i].key.layout = 1;
					members[i].key.pipelineGeneration = members[i].key.layoutGeneration = 1;
					members[i].key.dispatchMeshletCount = 1;
					members[i].key.descriptors = {1, 2}; members[i].key.descriptorGenerations = {1, 1};
					members[i].key.pushRangeCount = 1; members[i].key.pushRanges[0] = {1, 0, 64};
					members[i].knownDefaultTaskAbi = members[i].supportedRoute = true;
					members[i].deformationBridgeComplete = true;
					expected[i].objectIdentity.x = i;
					expected[i].geometrySelectors.w = 1;
					expected[i].animationSelectors = {10 + i, 20 + i, 30 + i, 40 + i};
					expected[i].virtualGeometryPayloadSelectors = {50 + i, 60 + i, 70 + i, 80 + i};
				}
				group.key = members[0].key; batch.groups.push_back(group);
				stream[0] = expected[0]; stream[1] = expected[1];
			}
			ShadowGroupedStreamPlan Plan() const
			{
				return BuildShadowGroupedStreamPlan(batch, members, expected, stream, owner, 352);
			}
			ShadowGroupedUploadReceipt Upload() const
			{
				return {batch.view, owner, true, true, true, true, true, true};
			}
		};
		void Singles(TestContext& context, const ShadowGroupedExecutionSelection& selected)
		{
			context.Expect(!selected.grouped && selected.groups.empty()
				&& selected.singletonIndices == std::vector<std::uint32_t>({0, 1, 2}),
				"Failed grouping retains every original selected segment as a singleton.");
		}
	}
	void RegisterShadowGroupedSubmissionTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.ShadowGrouped.Exact176ByteSelectors", [](TestContext& context) {
			Input input; const auto plan = input.Plan();
			context.Expect(sizeof(RenderInstanceRecord) == 176 && plan.valid && plan.instanceBytes == 352,
				"Complete selected records retain the existing 176-byte ABI.");
			for(unsigned lane = 0; lane < 3; ++lane)
			{
				auto changed = input;
				if(lane == 0) ++changed.stream[1].animationSelectors.y;
				if(lane == 1) ++changed.stream[1].virtualGeometryPayloadSelectors.z;
				if(lane == 2) ++changed.stream[1].geometrySelectors.w;
				context.Expect(!changed.Plan().valid, "A mismatched animation, page or geometry selector cannot suppress its single.");
			}
		});
		registry.Add("Renderer.Efficiency.ShadowGrouped.QualifiedIndependentRegions", [](TestContext& context) {
			Input input; const auto plan = input.Plan();
			context.Expect(SelectShadowGroupedExecution(plan, input.Upload()).grouped, "Current exact region upload authorizes grouping.");
			for(unsigned lane = 0; lane < 4; ++lane)
			{
				auto receipt = input.Upload();
				if(lane == 0) receipt.view.lightId[0] = 2;
				if(lane == 1) receipt.view.viewType = RenderViewType::Game;
				if(lane == 2) ++receipt.view.viewId;
				if(lane == 3) ++receipt.view.faceIndex;
				Singles(context, SelectShadowGroupedExecution(plan, receipt));
			}
		});
		registry.Add("Renderer.Efficiency.ShadowGrouped.CompleteDisjointCoverage", [](TestContext& context) {
			Input input; context.Expect(input.Plan().valid, "Group and singleton jointly cover every selected segment.");
			for(unsigned lane = 0; lane < 6; ++lane)
			{
				auto changed = input;
				if(lane == 0) changed.batch.singletonIndices.clear();
				if(lane == 1) changed.batch.singletonIndices.push_back(0);
				if(lane == 2) changed.batch.groups[0].memberIndices[1] = 0;
				if(lane == 3) changed.batch.groups.push_back(changed.batch.groups[0]);
				if(lane == 4) changed.batch.allSingletonIndices = {0, 1, 1};
				if(lane == 5) { changed.members[1].objectId = changed.members[0].objectId; changed.members[1].segmentId = changed.members[0].segmentId; }
				const auto rejected = changed.Plan(); context.Expect(!rejected.valid, "Missing, duplicated or overlapping contribution fails atomically.");
			}
		});
		registry.Add("Renderer.Efficiency.ShadowGrouped.CapacityAndSealedAuthority", [](TestContext& context) {
			Input input; context.Expect(input.Plan().valid, "Exact capacity is accepted.");
			context.Expect(!BuildShadowGroupedStreamPlan(input.batch, input.members, input.expected, input.stream, input.owner, 351).valid,
				"One byte short cannot publish any grouped range.");
			auto batch = input.batch; batch.coverageComplete = false;
			context.Expect(!BuildShadowGroupedStreamPlan(batch, input.members, input.expected, input.stream, input.owner, 352).valid, "Incomplete upstream coverage is not repaired by the stream adapter.");
			batch = input.batch; batch.groups[0].firstInstance = UINT32_MAX;
			context.Expect(!BuildShadowGroupedStreamPlan(batch, input.members, input.expected, input.stream, input.owner, 352).valid, "Huge range offset rejects before arithmetic or access.");
			// Several lights share a physical stream: unrelated earlier light rows
			// are legal, while this range must still be completely initialized.
			batch = input.batch; batch.groups[0].firstInstance = 2;
			auto stream = input.stream; stream.insert(stream.begin(), 2, RenderInstanceRecord{});
			context.Expect(BuildShadowGroupedStreamPlan(batch, input.members, input.expected, stream, input.owner, 704).valid,
				"A nonzero firstInstance preserves the independent contiguous light range.");
			auto expected = input.expected; ++expected[1].materialAndTexture.x; stream[3] = expected[1];
			context.Expect(!BuildShadowGroupedStreamPlan(batch, input.members, expected, stream, input.owner, 704).valid,
				"Even byte-identical expected/stream records must match actual grouped material selectors.");
		});
		registry.Add("Renderer.Efficiency.ShadowGrouped.PhysicalFrameSnapshotOwnership", [](TestContext& context) {
			Input input; const auto plan = input.Plan(); context.Expect(plan.valid, "Owned frame/snapshot stream is valid.");
			for(unsigned lane = 0; lane < 8; ++lane)
			{
				auto receipt = input.Upload();
				if(lane == 0) ++receipt.owner.rendererSessionGeneration;
				if(lane == 1) ++receipt.owner.backendGeneration;
				if(lane == 2) ++receipt.owner.allocationId;
				if(lane == 3) ++receipt.owner.allocationGeneration;
				if(lane == 4) ++receipt.owner.frameSerial;
				if(lane == 5) ++receipt.owner.commandSerial;
				if(lane == 6) ++receipt.owner.frameSlot;
				if(lane == 7) --receipt.owner.snapshotSlot;
				Singles(context, SelectShadowGroupedExecution(plan, receipt));
			}
			auto invalid = input; invalid.owner.frameSlot = 3;
			context.Expect(!invalid.Plan().valid, "A nonexistent physical frame slot never owns a stream.");
			invalid = input; invalid.owner.snapshotSlot = 8;
			context.Expect(!invalid.Plan().valid, "A nonexistent physical snapshot slot never owns a stream.");
			invalid = input; invalid.owner.allocationGeneration = 0;
			context.Expect(!invalid.Plan().valid, "Missing actual allocation publication cannot authorize grouping.");
			Input game; game.batch.view.viewType = RenderViewType::Game; game.batch.view.viewId = 0;
			context.Expect(game.Plan().valid, "Actual Game output zero remains a valid qualified identity.");
		});
		registry.Add("Renderer.Efficiency.ShadowGrouped.UploadFailureRestoresAllSingles", [](TestContext& context) {
			Input input; const auto plan = input.Plan(); context.Expect(SelectShadowGroupedExecution(plan, input.Upload()).grouped, "Successful actual upload admits groups.");
			for(unsigned lane = 0; lane < 6; ++lane)
			{
				auto receipt = input.Upload();
				if(lane == 0) receipt.frameSlotSafe = false;
				if(lane == 1) receipt.ownerCurrent = false;
				if(lane == 2) receipt.mapSucceeded = false;
				if(lane == 3) receipt.writeSucceeded = false;
				if(lane == 4) receipt.flushSucceeded = false;
				if(lane == 5) receipt.descriptorPublished = false;
				Singles(context, SelectShadowGroupedExecution(plan, receipt));
			}
		});
		registry.Add("Renderer.Efficiency.ShadowGrouped.CountsRequireActualRecording", [](TestContext& context) {
			Input input; const auto plan = input.Plan(); const auto selected = SelectShadowGroupedExecution(plan, input.Upload());
			ShadowGroupedRecordingReceipt receipt{input.batch.view, input.owner, {0}, {2}, true};
			const auto counts = CountShadowGroupedRecording(plan, selected, receipt);
			context.Expect(counts.valid && counts.groups == 1 && counts.singletons == 1 && counts.draws == 2 && counts.segments == 3,
				"Only actual recorded group/singleton receipts produce execution counters.");
			receipt.recorded = false; context.Expect(!CountShadowGroupedRecording(plan, selected, receipt).valid, "Planned ranges are not recorded evidence.");
			receipt.recorded = true; ++receipt.owner.commandSerial; context.Expect(!CountShadowGroupedRecording(plan, selected, receipt).valid, "Another command cannot publish this stream's execution counts.");
			receipt.owner = input.owner; receipt.recordedGroupIndices.clear();
			context.Expect(!CountShadowGroupedRecording(plan, selected, receipt).valid, "A missing actual group cannot be replaced with a planned count.");
			const auto singles = SelectShadowGroupedExecution(plan, {});
			receipt.recordedSingletonIndices = {0, 1, 2};
			const auto fallback = CountShadowGroupedRecording(plan, singles, receipt);
			context.Expect(fallback.valid && fallback.draws == 3 && fallback.segments == 3 && !fallback.groups,
				"Actual fallback recording reports every original singleton and no planned group.");
			input.owner.allocationId = input.owner.allocationGeneration = 0;
			const auto missingAllocation = input.Plan();
			receipt.owner = input.owner;
			const auto recordedSingles = CountShadowGroupedRecording(missingAllocation,
				SelectShadowGroupedExecution(missingAllocation, input.Upload()), receipt);
			context.Expect(recordedSingles.valid && recordedSingles.draws == 3 && recordedSingles.segments == 3,
				"Allocation creation failure does not erase actual original-singleton recording evidence.");
		});
	}
}
