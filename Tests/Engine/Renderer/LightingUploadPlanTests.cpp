// LightingUploadPlanTests.cpp — pure upload bytes and range regressions.
// These tests never map a GPU allocation or manufacture a successful upload.
#include "../TestHarness.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/RHI/Vulkan/VulkanLightingUploadPlan.h"

#include <bit>
#include <limits>
#include <memory>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer::VulkanLightingUpload;
		PhysicalOwner Owner(std::uint64_t offset = 0)
		{
			PhysicalOwner owner{};
			owner.allocationId = owner.allocationGeneration = owner.rendererSessionGeneration = 1;
			owner.viewType = RenderViewType::Scene;
			owner.outputId = 1;
			owner.regionOffsetBytes = offset;
			return owner;
		}
		MappingBounds Bounds(bool coherent = false)
		{
			return {kBlockBytes * 3, 0, kBlockBytes * 3, 256, coherent};
		}
		CanonicalBlock Block(std::uint32_t count)
		{
			CanonicalBlock block{};
			block.activeCount = count;
			for(std::uint32_t index = 0; index < count; ++index)
				block.records[index][0] = index + 1;
			return block;
		}
		std::unique_ptr<CanonicalBlock> HeapBlock(std::uint32_t count)
		{
			// Tail transitions retain three complete blocks at once. Own those
			// fixtures on the heap so the test uses the normal executable stack.
			auto block = std::make_unique<CanonicalBlock>();
			block->activeCount = count;
			for(std::uint32_t index = 0; index < count; ++index)
				block->records[index][0] = index + 1;
			return block;
		}
		std::unique_ptr<Mirror> Initialized(const PhysicalOwner& owner, const CanonicalBlock& block)
		{
			// Large immutable blocks are test-owned on the heap, not nested stack copies.
			auto mirror = std::make_unique<Mirror>();
			mirror->owner = owner; mirror->block = block; mirror->revision = 7; mirror->initialized = true;
			return mirror;
		}
		std::unique_ptr<DirtyRangePlan> Plan(TestContext& context, const PhysicalOwner& owner,
			const MappingBounds& bounds, const CanonicalBlock& desired, const Mirror& mirror)
		{
			auto plan = std::make_unique<DirtyRangePlan>();
			context.Expect(BuildLightingDirtyRangePlan(owner, bounds, desired, mirror, *plan) && plan->valid,
				"A valid owned and mapped lighting block must produce a valid plan.");
			return plan;
		}
		void Runs(TestContext& context, const std::unique_ptr<DirtyRangePlan>& plan, std::vector<RecordRun> expected)
		{
			context.Expect(plan->logicalRuns == expected, "Only exact adjacent dirty records may share a logical run.");
			std::uint64_t records = 0;
			for(const auto& run : expected) records += run.recordCount;
			context.ExpectEqual(records * kRecordBytes, plan->logicalBytes, "Logical bytes describe actual records written.");
		}
	}

	void RegisterLightingUploadPlanTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.LightingUploadPlan.PreservesEveryShaderLane", [](TestContext& context) {
			GPULight light{};
			const std::array<glm::vec4*, 11> fields{&light.positionAndType, &light.directionAndRange,
				&light.colorAndIntensity, &light.coneAnglesAndArea, &light.shadowAndPadding, &light.shadowRegion,
				&light.extraParams, &light.shadowMatrixC0, &light.shadowMatrixC1, &light.shadowMatrixC2, &light.shadowMatrixC3};
			for(std::size_t field = 0; field < fields.size(); ++field)
				for(int lane = 0; lane < 4; ++lane) (*fields[field])[lane] = static_cast<float>(field * 4 + lane + 1);
			CanonicalBlock baseline{};
			CanonicalizeLighting(std::span(&light, 1), baseline);
			for(std::size_t word = 0; word < kWordsPerRecord; ++word)
			{
				context.ExpectEqual(std::bit_cast<std::uint32_t>(static_cast<float>(word + 1)), baseline.records[0][word],
					"Every shader lane, including LTC selectors and matrix columns, is preserved.");
				const auto original = (*fields[word / 4])[static_cast<int>(word % 4)];
				(*fields[word / 4])[static_cast<int>(word % 4)] = original + 1.0f;
				CanonicalBlock changed{};
				CanonicalizeLighting(std::span(&light, 1), changed);
				context.Expect(changed.records[0] != baseline.records[0], "Changing any consumed lane changes canonical content.");
				(*fields[word / 4])[static_cast<int>(word % 4)] = original;
			}
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.PreservesSignedZeroAndNonfiniteBits", [](TestContext& context) {
			GPULight light{};
			light.positionAndType = {std::bit_cast<float>(0x80000000U), std::bit_cast<float>(0x7fc12345U),
				std::bit_cast<float>(0x7f800000U), std::bit_cast<float>(0xff800000U)};
			CanonicalBlock block{};
			CanonicalizeLighting(std::span(&light, 1), block);
			context.Expect(block.records[0][0] == 0x80000000U && block.records[0][1] == 0x7fc12345U &&
				block.records[0][2] == 0x7f800000U && block.records[0][3] == 0xff800000U,
				"Canonicalization must not normalize signed zero or rewrite nonfinite shader inputs.");
			auto changed = block;
			changed.records[0][1] = 0x7fc12346U;
			Runs(context, Plan(context, Owner(), Bounds(), changed, *Initialized(Owner(), block)), {{0, 1}});
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.InactiveTailIsAllZero", [](TestContext& context) {
			CanonicalBlock block{};
			block.records.back().fill(0xffffffffU);
			GPULight light{};
			CanonicalizeLighting(std::span(&light, 1), block);
			context.ExpectEqual(1U, block.activeCount, "The active default light remains active.");
			context.Expect(block.records[0][22] != 0, "Active default UV scale remains one, rather than zeroed.");
			for(std::size_t index = 1; index < kRecordCount; ++index)
				context.Expect(block.records[index] == CanonicalRecord{}, "Every inactive record must contain initialized zero bytes.");
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.CanonicalCapacityClamp", [](TestContext& context) {
			std::vector<GPULight> lights(kRecordCount + 1);
			lights.back().positionAndType.x = 42;
			CanonicalBlock block{};
			CanonicalizeLighting(lights, block);
			context.ExpectEqual(kRecordCount, block.activeCount, "Native input retains the existing 1024-record cap.");
			context.ExpectEqual(0U, block.records.back()[0], "The over-cap record cannot overwrite the last accepted light.");
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.FirstInitializationWritesFullBlock", [](TestContext& context) {
			const auto desired = Block(2);
			const auto plan = Plan(context, Owner(), Bounds(), desired, Mirror{});
			Runs(context, plan, {{0, kRecordCount}});
			context.Expect(plan->fullInitialization && plan->desiredBlock == desired, "First use owns complete initialized replacement bytes.");
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.UnchangedWritesNothing", [](TestContext& context) {
			const auto block = Block(4);
			const auto plan = Plan(context, Owner(), Bounds(), block, *Initialized(Owner(), block));
			Runs(context, plan, {});
			context.Expect(!plan->fullInitialization && plan->flushRanges.empty() && plan->physicalFlushBytes == 0,
				"Successful unchanged content creates neither logical writes nor physical flushes.");
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.AdjacentAndDisjointDirtyRuns", [](TestContext& context) {
			const auto original = Block(12);
			auto changed = original;
			for(const auto index : {1, 2, 6, 9, 10}) changed.records[index][3] = 99;
			Runs(context, Plan(context, Owner(), Bounds(), changed, *Initialized(Owner(), original)), {{1, 2}, {6, 1}, {9, 2}});
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.AdditionPreservesOrder", [](TestContext& context) {
			const auto original = Block(3);
			const auto changed = Block(5);
			const auto plan = Plan(context, Owner(), Bounds(), changed, *Initialized(Owner(), original));
			Runs(context, plan, {{3, 2}});
			context.ExpectEqual(5U, plan->desiredBlock.activeCount, "The caller's active count is retained exactly.");
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.ReorderingDirtiesActualPositions", [](TestContext& context) {
			const auto original = Block(6);
			auto changed = original;
			std::swap(changed.records[1], changed.records[4]);
			Runs(context, Plan(context, Owner(), Bounds(), changed, *Initialized(Owner(), original)), {{1, 1}, {4, 1}});
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.ShrinkAndZeroClearOldTail", [](TestContext& context) {
			const auto original = HeapBlock(6);
			const auto reduced = HeapBlock(2);
			Runs(context, Plan(context, Owner(), Bounds(), *reduced, *Initialized(Owner(), *original)), {{2, 4}});
			const auto empty = HeapBlock(0);
			const auto plan = Plan(context, Owner(), Bounds(), *empty, *Initialized(Owner(), *reduced));
			Runs(context, plan, {{0, 2}});
			context.Expect(plan->desiredBlock.records[0] == CanonicalRecord{} && plan->desiredBlock.activeCount == 0,
				"Zero active lights replace the old active tail with zero bytes.");
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.CountOnlyChangeDoesNotInventWrites", [](TestContext& context) {
			CanonicalBlock original{};
			original.activeCount = 1;
			auto changed = original;
			changed.activeCount = 2;
			const auto plan = Plan(context, Owner(), Bounds(), changed, *Initialized(Owner(), original));
			Runs(context, plan, {});
			context.ExpectEqual(2U, plan->desiredBlock.activeCount, "An all-zero active light changes count, not identical buffer bytes.");
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.CoherentHasNoFlush", [](TestContext& context) {
			const auto original = Block(3);
			auto changed = original;
			changed.records[1][0]++;
			const auto plan = Plan(context, Owner(), Bounds(true), changed, *Initialized(Owner(), original));
			Runs(context, plan, {{1, 1}});
			context.Expect(plan->flushRanges.empty() && plan->physicalFlushBytes == 0 && plan->coherent,
				"Coherent host writes are reported without inventing an explicit flush.");
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.AllocationRelativeAtomAlignment", [](TestContext& context) {
			const auto owner = Owner(80);
			const auto original = Block(3);
			auto changed = original;
			changed.records[1][0]++;
			const auto plan = Plan(context, owner, Bounds(), changed, *Initialized(owner, original));
			Runs(context, plan, {{1, 1}});
			context.Expect(plan->flushRanges == std::vector<FlushRange>{{256, 256}} && plan->physicalFlushBytes == 256,
				"Flush uses allocation offset80+record176, rather than aligning a region-relative offset.");
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.FlushOverlapCoalescesSeparately", [](TestContext& context) {
			const auto original = Block(4);
			auto changed = original;
			changed.records[0][0]++;
			changed.records[2][0]++;
			const auto plan = Plan(context, Owner(), Bounds(), changed, *Initialized(Owner(), original));
			Runs(context, plan, {{0, 1}, {2, 1}});
			context.Expect(plan->flushRanges == std::vector<FlushRange>{{0, 768}} && plan->physicalFlushBytes == 768,
				"Adjacent aligned flush atoms may merge without writing the clean middle record.");
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.RejectsInvalidBoundsAndOverflow", [](TestContext& context) {
			const auto block = Block(2);
			auto rejects = [&](PhysicalOwner owner, MappingBounds bounds, const CanonicalBlock& desired) {
				auto output = std::make_unique<DirtyRangePlan>();
				context.Expect(!BuildLightingDirtyRangePlan(owner, bounds, desired, Mirror{}, *output) && !output->valid,
					"Unsafe counts, ranges and arithmetic must fail before a writable plan exists.");
			};
			auto shortBounds = Bounds(); shortBounds.mappedSizeBytes = kBlockBytes - 1;
			rejects(Owner(), shortBounds, block);
			auto outsideAllocation = Bounds(); outsideAllocation.allocationSizeBytes = kBlockBytes - 1;
			rejects(Owner(), outsideAllocation, block);
			auto overflowOwner = Owner(std::numeric_limits<std::uint64_t>::max() - 8);
			rejects(overflowOwner, Bounds(), block);
			auto invalidCount = block; invalidCount.activeCount = kRecordCount + 1;
			rejects(Owner(), Bounds(), invalidCount);
			auto noAtom = Bounds(); noAtom.nonCoherentAtomSize = 0;
			rejects(Owner(), noAtom, block);
		});
		registry.Add("Renderer.Efficiency.LightingUploadPlan.FlushCannotEscapeMappedRange", [](TestContext& context) {
			const auto owner = Owner(80);
			auto bounds = Bounds(); bounds.mappedOffsetBytes = 80; bounds.mappedSizeBytes = kBlockBytes;
			const auto original = Block(2);
			auto changed = original; changed.records[0][0]++;
			auto plan = std::make_unique<DirtyRangePlan>();
			context.Expect(!BuildLightingDirtyRangePlan(owner, bounds, changed, *Initialized(owner, original), *plan) && !plan->valid,
				"An atom-aligned flush before mapped offset80 must be rejected, never clipped or sent out of bounds.");
		});
	}
}
