// Refresh the actual planner and protected presentation publisher by stable identity.
#include "TestHarness.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Animation/AnimationSystem.h"
#include "Animation/Gpu/GpuAnimationPlanner.h"
#include "Animation/Evaluation/CpuClipEvaluator.h"
#include "AnimationClipCompiler.h"
#include "Core/Scheduling/FrameWorkload.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace TerraEngine::Tests {
namespace {
using namespace Animation;
		[[nodiscard]] GpuAnimationWorkRequest MakeRequest(
			std::uint64_t stableInstanceId,
			std::uint64_t programContentId,
			std::uint32_t submissionOrdinal = 0U)
		{
			GpuAnimationWorkRequest request{};
			request.batchKey.programContentId = programContentId;
			request.batchKey.skeletonTopologyId = 0x7000U;
			request.batchKey.stageMask = GpuAnimationStageMask(GpuAnimationStage::Sample)
				| GpuAnimationStageMask(GpuAnimationStage::Blend)
				| GpuAnimationStageMask(GpuAnimationStage::Hierarchy)
				| GpuAnimationStageMask(GpuAnimationStage::Bounds)
				| GpuAnimationStageMask(GpuAnimationStage::Palette);
			request.batchKey.outputFlags = static_cast<std::uint32_t>(GpuAnimationOutputFlag::Pose)
				| static_cast<std::uint32_t>(GpuAnimationOutputFlag::Bounds)
				| static_cast<std::uint32_t>(GpuAnimationOutputFlag::Palette);
			request.generations.instanceSlot = static_cast<std::uint32_t>(stableInstanceId >> 32U);
			request.generations.instanceGeneration = static_cast<std::uint32_t>(stableInstanceId);
			request.generations.componentGeneration = 1U;
			request.generations.workGeneration = 1U;
			request.generations.programGeneration = 1U;
			request.generations.controllerGeneration = 1U;
			request.generations.skeletonGeneration = 1U;
			request.generations.clipGeneration = 1U;
			request.generations.meshGeneration = 1U;
			request.generations.resourceGeneration = 1U;
			request.generations.rendererGeneration = 1U;
			request.generations.sceneGeneration = 1U;
			request.generations.frameIdLow = 7U;
			request.stableInstanceId = stableInstanceId;
			request.stableSubmissionOrdinal = submissionOrdinal;
			request.clipResourceIndex = 2U;
			request.programResourceIndex = 3U;
			request.skeletonResourceIndex = 4U;
			// One real cooked joint is enough to witness an incorrectly selected pose.
			request.jointCount = 1U;
			request.scratchSizeBytes = 20U;
			request.paletteEntryCount = 1U;
			request.palettePartitionCount = 1U;
			request.flags = static_cast<std::uint32_t>(GpuAnimationWorkFlag::Looping)
				| static_cast<std::uint32_t>(GpuAnimationWorkFlag::UseReferenceFallback);
			request.diagnosticIndex = static_cast<std::uint32_t>(stableInstanceId);
			return request;
		}

struct ArenaPair {
	GpuAnimationArena scratch{GpuAnimationArenaKind::Scratch, 1U << 20U};
	GpuAnimationArena palette{GpuAnimationArenaKind::Palette, 1U << 20U};
};

// This immutable resource is sampled by the production CPU decoder, not a GPU
// emulator. The returned plan selects its resource, evaluation owner and tick;
// the independent oracle below uses the authored straight line x = tick / 10.
// No shader execution or GPU resident-buffer correctness is claimed here.
struct NumericPoseFixture {
	static constexpr std::uint64_t joint = 100U;
	static constexpr std::uint32_t clipSelector = 2U;
	std::shared_ptr<AnimationClipAsset> clip = std::make_shared<AnimationClipAsset>();
	AnimationClipView view;
	explicit NumericPoseFixture(TestContext& context) {
		AssetPipeline::AnimationClipCompileInput input;
		input.assetId.value[0] = 1U; input.skeletonAssetId.value[0] = 2U;
		input.compatibilityFingerprint = 77U;
		input.durationTicks = 200; input.sampleRate = kAnimationFixedTicksPerSecond;
		input.segmentDurationTicks = 200;
		AssetPipeline::AnimationSourceTrack track;
		track.stableTargetId = joint; track.kind = AnimationClipTrackKind::Translation;
		track.interpolation = AnimationInterpolation::Linear;
		AssetPipeline::AnimationSourceKey start, end;
		start.tick = 0; start.vectorValue = {0.0F, 0.0F, 0.0F};
		end.tick = 200; end.vectorValue = {20.0F, 0.0F, 0.0F};
		track.keys = {start, end}; input.tracks = {track};
		AssetPipeline::AnimationClipCompileReport report;
		context.Expect(AssetPipeline::CompileAnimationClip(input, {}, *clip, report) == AnimationStatus::Ok,
			"The numeric refresh witness must use a real cooked linear clip.");
		context.Expect(view.Open(*clip) == AnimationStatus::Ok,
			"The real cooked clip must open before a returned plan can select it.");
	}
	void ExpectSelectedPose(TestContext& context, const GpuAnimationPlan& plan,
		std::uint64_t owner, std::uint32_t ordinal, std::uint32_t authoredTick) const {
		std::uint32_t packetIndex = 0U;
		context.Expect(plan.FindWorkPacketIndex(owner, ordinal, packetIndex),
			"The numeric observer must resolve the actual returned stable packet.");
		const auto& packets = plan.WorkPackets();
		const auto& sources = plan.EvaluationSourcePacketIndices();
		context.Expect(packetIndex < packets.size() && sources.size() == packets.size(),
			"Actual returned packet and evaluation-source tables must be complete.");
		const auto sourceIndex = sources[packetIndex];
		context.Expect(sourceIndex <= packetIndex,
			"The selected numeric pose must have a bounded actual evaluation source.");
		const auto source = plan.ResolvedWorkPacket(sourceIndex);
		context.Expect(source.clipResourceIndex == clipSelector && source.jointCount == 1U
			&& source.paletteOffsetBytes == packets[packetIndex].paletteOffsetBytes,
			"The actual output alias must select the owned cooked resource and source palette.");
		bool samplesSelectedSource = false;
		for(const auto& dispatch : plan.DispatchRecords()) {
			if(dispatch.workPacketIndex == sourceIndex
				&& dispatch.stage == static_cast<std::uint32_t>(GpuAnimationStage::Sample)
				&& dispatch.elementCount == 1U) samplesSelectedSource = true;
		}
		context.Expect(samplesSelectedSource,
			"The returned execution plan must actually sample the selected numeric pose source.");
		for(std::size_t index = 0; index < packets.size(); ++index) {
			if(sources[index] == index && index != sourceIndex)
				context.Expect(packets[index].paletteOffsetBytes != source.paletteOffsetBytes,
					"Independent evaluated poses must not alias the selected palette output.");
		}
		const std::array<std::uint64_t, 1> joints{joint};
		CpuClipEvaluationRequest request;
		// Deliberately sample the returned source lane, never authoredTick. Using
		// the desired tick here would hide both stale sharing and lane permutation.
		request.tick = source.sampleTick; request.jointStableIds = joints;
		request.includeEventsAtPoint = false; request.includeRootMotionPoint = false;
		CpuClipEvaluationResult result;
		context.Expect(CpuClipEvaluator{}.Evaluate(view, request, result) == AnimationStatus::Ok
			&& result.joints.size() == 1U, "The selected real clip sample must decode one joint.");
		const auto& pose = result.joints.front().localTransform;
		const float expectedX = static_cast<float>(authoredTick) / 10.0F;
		// The 20-metre UNORM16 channel has a quantization step below 0.00031m.
		// Keep the existing animation parity translation budget of 0.0005m.
		context.Expect(std::isfinite(pose.translation.x)
			&& std::abs(pose.translation.x - expectedX) <= 0.0005F
			&& std::abs(pose.translation.y) <= 0.0005F && std::abs(pose.translation.z) <= 0.0005F
			&& std::abs(pose.rotation.x) <= 0.0005F && std::abs(pose.rotation.y) <= 0.0005F
			&& std::abs(pose.rotation.z) <= 0.0005F && std::abs(pose.rotation.w - 1.0F) <= 0.0005F
			&& std::abs(pose.scale.x - 1.0F) <= 0.0005F && std::abs(pose.scale.y - 1.0F) <= 0.0005F
			&& std::abs(pose.scale.z - 1.0F) <= 0.0005F,
			"Actual plan-selected numeric pose must match the independent authored linear reference.");
	}
};
GpuAnimationPlan Cold(TestContext& context, const std::vector<GpuAnimationWorkRequest>& requests, ArenaPair& arenas) {
	GpuAnimationPlan plan;
	context.Expect(BuildGpuAnimationPlan(requests, 1U, arenas.scratch, arenas.palette, plan) == GpuAnimationPlanStatus::Ok,
		"The production cold planner must accept the complete request metadata fixture.");
	return plan;
}
GpuAnimationRenderIdentity RenderIdentity(std::uint8_t seed) {
	GpuAnimationRenderIdentity identity;
	identity.meshId[0] = seed; identity.meshHeaderVersion = 1U; identity.deformablePayloadGeneration = 1U;
	identity.rendererGeneration = 1U; identity.sceneGeneration = 1U; identity.skeletonCompatibilityFingerprint = 77U;
	return identity;
}
std::shared_ptr<const AnimationAuthorityFrame> OwnerAuthority(const GpuAnimationOwnerObjectMapping& mapping, std::uint64_t generation) {
	auto sidecars = std::make_shared<AnimationFrameSidecars>(); (void)sidecars->ValidateAndOrder(generation);
	auto page = std::make_shared<AnimationAuthorityPage>(); page->capacity = static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128);
	page->changedMasks.assign(2U, 0U); AnimationAuthorityRow row;
	row.key = {mapping.instanceSlot, mapping.instanceGeneration, 3U}; row.ownerObjectId = mapping.ownerObjectId;
	row.componentGeneration = mapping.componentGeneration; row.sourceGeneration = static_cast<AnimationGeneration>(generation);
	row.poseGeneration = static_cast<AnimationGeneration>(generation); row.authorityPublicationGeneration = generation;
	row.effectiveExecutionMode = AnimationEffectiveExecutionMode::GpuVisual; row.active = true;
	page->rows.push_back(row);
	auto authority = std::make_shared<AnimationAuthorityFrame>(); authority->frameId = generation;
	authority->sessionGeneration = 3U; authority->lifecycleGeneration = 4U; authority->authorityGeneration = generation;
	authority->pages = std::make_shared<const std::vector<std::shared_ptr<const AnimationAuthorityPage>>>(
		std::vector<std::shared_ptr<const AnimationAuthorityPage>>{page});
	authority->sidecars = std::move(sidecars); authority->receipt.outcome = AnimationAuthorityPublicationOutcome::Published;
	authority->receipt.authorityGeneration = generation; return authority;
}

void QualifyFollower(TestContext& context, bool parameter) {
	NumericPoseFixture poses(context);
	unsigned failedPaths = 0U;
	for(bool generated : {false, true}) {
		std::vector<GpuAnimationWorkRequest> requests{MakeRequest((1ULL << 32U) | 1U, 1U), MakeRequest((2ULL << 32U) | 1U, 1U)};
		for(auto& request : requests) { request.sampleTick = 10U; request.diagnosticIndex = 0x80000000U; request.renderIdentity = RenderIdentity(1U); }
		ArenaPair arenas;
		const auto previous = Cold(context, requests, arenas);
		context.Expect(previous.EvaluationSourcePacketIndices().size() == 2U && previous.EvaluationSourcePacketIndices()[1U] == 0U,
			"The precondition must actually share one evaluation source and palette.");
		if(parameter) requests[1U].evaluationParameterFingerprint = 0xBEEFU; else requests[1U].sampleTick = 30U;
		ArenaPair referenceArenas; const auto reference = Cold(context, requests, referenceArenas);
		context.Expect(reference.EvaluationSourcePacketIndices()[1U] == 1U, "A cold plan must independently evaluate the changed follower.");
		GpuAnimationPlan refreshed; refreshed.generation = 919U;
		auto producer = [](std::size_t index, GpuAnimationWorkRequest& output, void* context) noexcept {
			output = (*static_cast<const std::vector<GpuAnimationWorkRequest>*>(context))[index]; return true;
		};
		const bool accepted = generated
			? TryRefreshGeneratedGpuAnimationPlan(requests.size(), producer, &requests, 2U, arenas.scratch, arenas.palette, previous, refreshed)
			: TryRefreshGpuAnimationPlan(requests, 2U, arenas.scratch, arenas.palette, previous, refreshed);
		if(accepted && refreshed.EvaluationSourcePacketIndices() != reference.EvaluationSourcePacketIndices())
			failedPaths |= generated ? 2U : 1U;
		if(!accepted) context.Expect(refreshed.generation == 919U, "Rejected refresh must preserve the caller's previous output.");
		// A rejected refresh's actual cold-build result is the supported recovery.
		// Parameter fingerprints are opaque admission metadata, not numeric shader
		// parameters: only the tick-changing case has different authored poses.
		// The parameter case retains the independent source/rejection assertion.
		ArenaPair rebuiltArenas;
		GpuAnimationPlan rebuilt;
		if(!accepted) rebuilt = Cold(context, requests, rebuiltArenas);
		const auto& selected = accepted ? refreshed : rebuilt;
		for(const auto& request : requests)
			poses.ExpectSelectedPose(context, selected, request.stableInstanceId,
				request.stableSubmissionOrdinal, request.sampleTick);
	}
	context.Expect(failedPaths == 0U,
		"Both actual refresh paths must requalify tick/parameter equivalence or reject for cold rebuild. Failed paths (direct=1, generated=2): " + std::to_string(failedPaths));
}
void CompareLanes(TestContext& context, const GpuAnimationPlan& actual, const GpuAnimationPlan& expected) {
	for(std::size_t index = 0U; index < expected.WorkPackets().size(); ++index) {
		const auto& packet = expected.WorkPackets()[index]; std::uint32_t resolved = 0U;
		context.Expect(actual.FindWorkPacketIndex(packet.stableInstanceId, packet.stableSubmissionOrdinal, resolved), "Every cold-plan stable identity must remain present after refresh.");
		const auto& actualPacket = actual.WorkPackets()[resolved];
		const auto& generation = actual.GenerationRecord(actualPacket.generationRecordIndex);
		const auto& reference = expected.GenerationRecord(packet.generationRecordIndex);
		context.Expect(actual.SampleTick(resolved) == expected.SampleTick(index)
			&& std::memcmp(&generation, &reference, sizeof(generation)) == 0
			&& actual.RenderIdentity(resolved) == expected.RenderIdentity(index),
			"Dynamic tick, complete generation and render lanes must follow stable packet identity rather than producer position.");
	}
}
struct PresentationFixture {
	AnimationPresentationSystem& system = AnimationPresentationSystem::Instance();
	AnimationSystem& animation = AnimationSystem::Instance();
	AnimatorInstanceHandle handle{};
	PresentationFixture() {
		animation.Initialize(); AnimatorInstanceDescriptor descriptor;
		descriptor.ownerObjectId[0] = 40U; descriptor.componentGeneration = 1U;
		handle = animation.CreateAnimatorInstance(descriptor);
		system.Shutdown(); system.Initialize();
	}
	~PresentationFixture() { (void)animation.InvalidateAnimatorInstance(handle); system.Shutdown(); system.Initialize(); }
	void Publish(TestContext& context, std::shared_ptr<const GpuAnimationPresentationFrame> frame) {
		// The scheduler declares authority first to refresh activation metadata.
		// This isolated publisher case supplies its own pinned immutable authority
		// input, so it executes only presentation's production workloads below.
		const auto authorityDeclarations = animation.DeclareFrameWorkloads(1.0F / 60.0F);
		context.Expect(!authorityDeclarations.empty(), "The live authority owner must supply current scheduler activation metadata.");
		context.Expect(system.SubmitGpuAnimationPresentationFrame(std::move(frame)) == AnimationStatus::Ok, "The current immutable presentation source must be admitted.");
		auto workloads = system.DeclareFrameWorkloads(1.0F / 60.0F);
		context.Expect(!workloads.empty(), "A live animator must activate the real presentation workloads; an empty compatibility plan is not qualification.");
		const auto result = Scheduling::ExecuteFrameWorkloadsCompatibility(std::move(workloads), "review-refresh-identities");
		context.Expect(result.valid && result.completed, "The actual protected presentation workloads must complete.");
	}
};
}
void RegisterAnimationRefreshIdentityTests(TestRegistry& registry) {
	registry.Add("Animation.Review.Refresh.FollowerTickChangeRequalifiesBothRefreshPaths", [](TestContext& context) { QualifyFollower(context, false); });
	registry.Add("Animation.Review.Refresh.FollowerParameterChangeRequalifiesBothRefreshPaths", [](TestContext& context) { QualifyFollower(context, true); });
	registry.Add("Animation.Review.Refresh.InterleavedInputPermutationMatchesColdStableLanes", [](TestContext& context) {
		NumericPoseFixture poses(context);
		std::vector<GpuAnimationWorkRequest> requests;
		for(std::uint32_t index = 0; index < 4U; ++index) {
			// Two legal submissions per owner exercise the complete stable packet
			// identity while preserving interleaved program batches/permutations.
			auto request = MakeRequest((static_cast<std::uint64_t>(index / 2U + 1U) << 32U) | 1U, (index % 2U) + 1U, index % 2U);
			request.sampleTick = 10U + index; request.renderIdentity = RenderIdentity(static_cast<std::uint8_t>(index + 1U)); requests.push_back(request);
		}
		ArenaPair arenas; auto previous = Cold(context, requests, arenas);
		auto payload = std::make_shared<const GpuAnimationResidentPayloads>(); previous.sharedResidentPayloads = payload;
		std::vector<GpuAnimationLoweringInput> inputs;
		for(const auto& request : requests) {
			GpuAnimationLoweringInput input; input.program.programContentId = request.batchKey.programContentId;
			input.program.programResourceIndex = request.programResourceIndex; input.visual.generations = request.generations;
			input.visual.stableInstanceId = request.stableInstanceId; input.visual.stableSubmissionOrdinal = request.stableSubmissionOrdinal;
			input.visual.skeletonTopologyId = request.batchKey.skeletonTopologyId; input.visual.sampleTick = request.sampleTick;
			input.visual.renderIdentity = request.renderIdentity; input.visual.jointCount = request.jointCount;
			input.effectiveMode = AnimationEffectiveExecutionMode::GpuVisual; input.residentPayloads = payload;
			input.prevalidatedStageMask = request.batchKey.stageMask; inputs.push_back(input);
		}
		GpuAnimationLoweringTopologySignatureBuilder before; for(const auto& input : inputs) before.Include(input);
		previous.topologyInputSignature = before.Complete();
		std::reverse(inputs.begin(), inputs.end()); std::reverse(requests.begin(), requests.end());
		for(std::size_t index = 0; index < inputs.size(); ++index) {
			inputs[index].visual.sampleTick += 100U; requests[index].sampleTick += 100U;
			inputs[index].visual.generations.componentGeneration += 20U; requests[index].generations.componentGeneration += 20U;
		}
		GpuAnimationLoweringTopologySignatureBuilder after; for(const auto& input : inputs) after.Include(input);
		context.Expect(before.Complete() == after.Complete(), "Only producer order and dynamic lanes may change in the retained-topology fixture.");
		ArenaPair coldArenas; const auto reference = Cold(context, requests, coldArenas);
		GpuAnimationPlan refreshed;
		const bool accepted = TryRefreshLoweredGpuAnimationDynamicPlan(inputs, after.Complete(), 2U, arenas.scratch, arenas.palette, previous, refreshed);
		context.Expect(accepted, "An unchanged set of stable topology inputs must support dynamic refresh despite producer permutation.");
		CompareLanes(context, refreshed, reference);
		for(const auto& request : requests) {
			poses.ExpectSelectedPose(context, refreshed, request.stableInstanceId,
				request.stableSubmissionOrdinal, request.sampleTick);
			poses.ExpectSelectedPose(context, reference, request.stableInstanceId,
				request.stableSubmissionOrdinal, request.sampleTick);
		}
	});
	registry.Add("Animation.Review.Refresh.DynamicShellOwnerChangePublishesCompleteMappingsOrFailsClosed", [](TestContext& context) {
		PresentationFixture fixture;
		auto request = MakeRequest((static_cast<std::uint64_t>(fixture.handle.slotIndex) << 32U) | fixture.handle.generation, 1U); request.renderIdentity = RenderIdentity(1U);
		ArenaPair arenas; auto retained = std::make_shared<GpuAnimationPlan>(Cold(context, {request}, arenas));
		retained->topologyInputSignature = 17U;
		auto source = std::make_shared<GpuAnimationPresentationFrame>();
		source->sourceGeneration = 1U; source->planGeneration = 1U; source->ownerMappingSignature = 10U; source->topologyInputSignature = 17U;
		source->retainedStaticPlan = retained; source->preparedDynamicPlanComplete = true;
		source->preparedDynamicLanes.generationRecords = retained->generationRecords;
		source->preparedDynamicLanes.renderIdentities = retained->renderIdentities;
		for(std::size_t index = 0; index < retained->WorkPackets().size(); ++index)
			source->preparedDynamicLanes.sampleTicks.push_back(retained->SampleTick(index));
		GpuAnimationOwnerObjectMapping mapping; mapping.ownerObjectId[0] = 40U; mapping.stableInstanceId = request.stableInstanceId;
		mapping.instanceSlot = request.generations.instanceSlot; mapping.instanceGeneration = request.generations.instanceGeneration; mapping.componentGeneration = 1U;
		source->ownerObjectMappings.push_back(mapping);
		source->authority = OwnerAuthority(mapping, 1U);
		fixture.Publish(context, source);
		const auto first = fixture.system.LoadPublishedGpuAnimationPresentationPlan();
		context.Expect(first && !first->safeFallback && first->FindFirstOwnerObjectMapping(mapping.ownerObjectId) != nullptr,
			"The protected initial publication must expose its actual complete owner mapping. Present=" + std::to_string(static_cast<bool>(first))
			+ " Fallback=" + std::to_string(first ? first->safeFallback : true) + " Packets=" + std::to_string(first ? first->plan.WorkPackets().size() : 0U));
		auto changed = std::make_shared<GpuAnimationPresentationFrame>(*source);
		changed->sourceGeneration = 2U; changed->planGeneration = 2U; changed->ownerMappingSignature = 11U;
		auto changedOwner = mapping; changedOwner.ownerObjectId[0] = 41U; changed->authority = OwnerAuthority(changedOwner, 2U);
		// The compact producer deliberately omits mappings; changed ownership must
		// reject rather than publish a nonempty plan with an empty lookup root.
		changed->ownerObjectMappings.clear(); fixture.Publish(context, changed);
		const auto publication = fixture.system.LoadPublishedGpuAnimationPresentationPlan();
		context.Expect(publication && publication->safeFallback && publication->plan.WorkPackets().empty(),
			"A changed-owner compact shell without current mappings must fail closed as a complete safe publication.");
		context.Expect(first->FindFirstOwnerObjectMapping(mapping.ownerObjectId) != nullptr, "Pinned prior mappings must retain their immutable lifetime.");
	});
}
}
