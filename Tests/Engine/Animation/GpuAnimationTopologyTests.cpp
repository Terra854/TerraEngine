// GpuAnimationTopologyTests.cpp - Retained topology and dynamic sidecar contracts.

#include "TestHarness.h"

#include "Animation/Gpu/GpuAnimationPlan.h"

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;

	void RegisterGpuAnimationTopologyTests(TestRegistry& registry)
	{
		registry.Add("Animation.GpuTopology.ReusesOnlyExactStableOwnerLayout", [](TestContext& context) {
			GpuAnimationPlan plan;
			GpuAnimationWorkPacket first{};
			first.stableInstanceId = 9U;
			first.stableSubmissionOrdinal = 2U;
			first.sampleTick = 10U;
			first.paletteOffsetBytes = 64U;
			GpuAnimationWorkPacket second{};
			second.stableInstanceId = 5U;
			second.stableSubmissionOrdinal = 1U;
			second.sampleTick = 11U;
			second.paletteOffsetBytes = 128U;
			plan.workPackets = {first, second};
			plan.dispatchCommands.resize(1U);
			plan.stageSpans.push_back({0U, 1U, 0U, 0U, 0U, 1U, 0U});

			const GpuAnimationTopologyGenerationKey generations{1U, 2U, 3U, 4U, 5U};
			GpuAnimationTopology topology;
			context.Expect(BuildGpuAnimationTopology(plan, generations, 7U, topology),
				"A complete plan should produce one deterministic retained topology.");
			context.Expect(topology.owners.front().stableInstanceId == 5U,
				"Topology owners must publish deterministic identity order instead of worker completion order.");

			GpuAnimationTopologyDynamicState dynamic;
			context.Expect(BuildGpuAnimationTopologyDynamicState(plan, topology, 8U, dynamic),
				"Playback samples and output offsets should remain frame-owned sidecar data.");
			context.Expect(dynamic.sampleTicks.size() == 2U && dynamic.sampleTicks.front() == 11U,
				"The dynamic sidecar must follow retained owner order while reading its original packet.");
			dynamic.topologyGenerations.rendererGeneration++;
			context.Expect(!dynamic.IsCompleteFor(topology),
				"A renderer-generation mismatch must invalidate the complete dynamic envelope.");
		});
	}
}
