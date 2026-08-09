#include "TestHarness.h"

#include "Animation/Runtime/AnimationFrameSidecars.h"

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationSpecialistRecord Specialist(std::uint32_t slot, std::uint64_t generation,
			std::uint64_t correlationId, std::uint32_t kind)
		{
			// Each sparse payload repeats the slot/session identity so a consumer can
			// reject a recycled animator without consulting mutable instance storage.
			return {{slot, 2U, 9U}, generation, correlationId, AnimationStatus::Ok, kind, 0U};
		}
	}

	void RegisterAnimationSparseSidecarTests(TestRegistry& registry)
	{
		registry.Add("Animation.SparseSidecar.BoundedMetadataOnly", [](TestContext& context) {
			TerraEngine::Animation::AnimationFrameSidecars sidecars;
			context.Expect(sidecars.ValidateAndOrder(1U),
				"Empty sparse sidecars are valid for a published authority generation.");
		});

		registry.Add("Animation.SparseSidecar.SealOrdersSpecialistRecords", [](TestContext& context) {
			using namespace TerraEngine::Animation;
			AnimationFrameSidecars sidecars;
			sidecars.callbacks = {
				{{7U, 2U, 9U}, 5U, 20U, AnimationStatus::Ok, 1U, 0U},
				{{3U, 1U, 9U}, 5U, 10U, AnimationStatus::Ok, 1U, 0U}};
			context.Expect(sidecars.ValidateAndOrder(5U)
				&& sidecars.callbacks.front().key.slot == 3U
				&& sidecars.callbacks.back().key.slot == 7U,
				"Worker sealing must sort sparse callback records once, before immutable root publication.");
		});

		registry.Add("Animation.SparseSidecar.AllSpecialistsRemainSparseAndDeterministic", [](TestContext& context) {
			constexpr std::uint64_t authorityGeneration = 44U;
			AnimationFrameSidecars sidecars;
			// These optional categories deliberately name different animators. Physics
			// carries separate ordinary-physics and ragdoll records in its typed lane.
			// The remaining 3,991 rows in this representative 4,000-animator frame
			// must not allocate an empty per-row specialist payload.
			sidecars.callbacks = {Specialist(91U, authorityGeneration, 2U, 1U)};
			sidecars.physics = {
				Specialist(71U, authorityGeneration, 3U, 2U),
				Specialist(41U, authorityGeneration, 4U, 9U)};
			sidecars.attachments = {Specialist(51U, authorityGeneration, 5U, 3U)};
			sidecars.procedural = {Specialist(31U, authorityGeneration, 6U, 4U)};
			sidecars.rollback = {Specialist(21U, authorityGeneration, 7U, 5U)};
			sidecars.completions = {Specialist(11U, authorityGeneration, 8U, 6U)};
			sidecars.gpuCandidates = {Specialist(81U, authorityGeneration, 9U, 7U)};
			sidecars.diagnostics = {Specialist(61U, authorityGeneration, 10U, 8U)};
			sidecars.events = {
				{{{7U, 2U}, 1U, {}, 2U, 0}, AnimationEventPayloadType::None, {}, 0U, 0U,
					AnimationPlaybackDirection::Forward, 2U, 20U, false},
				{{{3U, 2U}, 1U, {}, 1U, 0}, AnimationEventPayloadType::None, {}, 0U, 0U,
					AnimationPlaybackDirection::Forward, 1U, 10U, false}};
			sidecars.rootMotion = {
				{{8U, 2U}, 1, 2},
				{{4U, 2U}, 1, 2}};
			sidecars.summary.evaluatedRows = 4000U;

			const std::size_t specialistCount = sidecars.callbacks.size() + sidecars.physics.size()
				+ sidecars.attachments.size() + sidecars.procedural.size() + sidecars.rollback.size()
				+ sidecars.completions.size() + sidecars.gpuCandidates.size() + sidecars.diagnostics.size();
			context.Expect(sidecars.ValidateAndOrder(authorityGeneration)
				&& specialistCount == 9U && sidecars.physics.front().key.slot == 41U
				&& sidecars.events.size() == 2U && sidecars.rootMotion.size() == 2U
				&& sidecars.events.front().producerOrder == 10U
				&& sidecars.rootMotion.front().animator.slotIndex == 4U,
				"Events, root motion, and every specialist category must seal in deterministic sparse order.");
			context.Expect(sidecars.summary.evaluatedRows - static_cast<std::uint32_t>(specialistCount) == 3991U,
				"Non-participating animators must have no specialist record and therefore no per-row sidecar cost.");
		});

		registry.Add("Animation.SparseSidecar.RejectsOneStaleSpecialistWithoutPartialOrdering", [](TestContext& context) {
			constexpr std::uint64_t authorityGeneration = 22U;
			AnimationFrameSidecars sidecars;
			sidecars.physics = {Specialist(5U, authorityGeneration, 1U, 1U)};
			sidecars.diagnostics = {Specialist(9U, authorityGeneration - 1U, 2U, 2U)};
			context.Expect(!sidecars.ValidateAndOrder(authorityGeneration)
				&& sidecars.physics.front().key.slot == 5U && sidecars.diagnostics.front().key.slot == 9U,
				"One stale optional payload must reject the complete sidecar candidate before it is published.");
		});
	}
}
