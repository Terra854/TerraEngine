#include "TestHarness.h"

#include "Runtime/Validation/AnimationParityValidator.h"
#include "Animation/Runtime/PoseBuffer.h"

#include <stdexcept>

namespace TerraEngine::Tests {
	static Animation::PublishedAnimatorPose MakeDigestPose(float x = 1.0F, std::uint64_t state = 99U, std::uint64_t publication = 13U) {
		auto buffer = std::make_shared<Animation::PoseBuffer>();
		Animation::AnimationPoseGeneration generation;
		generation.animator = {0U, 1U};
		generation.poseGeneration = 1U;
		generation.tick = 4;
		if (buffer->BeginWrite({1U, 1U, 0U, 1U, 0U}, generation) != Animation::AnimationStatus::Ok) {
			throw std::runtime_error("Parity digest fixture could not begin a valid pose write.");
		}
		if (buffer->MutableModelTransforms().size() != 1U || buffer->MutableMorphValues().size() != 1U) {
			throw std::runtime_error("Parity digest fixture did not allocate its declared pose channels.");
		}
		buffer->MutableModelTransforms()[0].translation.x = x;
		buffer->MutableMorphValues()[0] = {7U, 0.25F};
		if (buffer->Seal() != Animation::AnimationStatus::Ok) {
			throw std::runtime_error("Parity digest fixture could not seal a valid pose.");
		}
		Animation::PublishedAnimatorPose pose;
		pose.ownerObjectId[0] = 1U;
		pose.controller.assetId.value[0] = 2U;
		pose.clip.assetId.value[0] = 3U;
		pose.skeleton.assetId.value[0] = 4U;
		pose.objectGeneration = 10U; pose.componentGeneration = 11U; pose.authorityPublicationGeneration = 12U;
		auto snapshot = std::make_shared<Animation::AnimationSnapshot>();
		snapshot->publicationGeneration = publication; snapshot->tick = 4; snapshot->pose = buffer;
		snapshot->playback.currentStateId = state;
		pose.snapshot = snapshot;
		return pose;
	}
	void RegisterAnimationParityValidatorTests(TestRegistry& registry) {
		registry.Add("Engine.AnimationParity.CanonicalHashIsStable", [](TestContext& context) {
			context.ExpectEqual(Runtime::CanonicalAnimationParityHash("pose|tick=7"),
				Runtime::CanonicalAnimationParityHash("pose|tick=7"), "Canonical hash must be deterministic.");
		});
		registry.Add("Engine.AnimationParity.CanonicalHashDetectsMutation", [](TestContext& context) {
			context.Expect(Runtime::CanonicalAnimationParityHash("pose|tick=7") !=
				Runtime::CanonicalAnimationParityHash("pose|tick=8"), "Canonical hash must change when canonical input changes.");
		});
		registry.Add("Engine.AnimationParity.EmptyPublishedSetFails", [](TestContext& context) {
			const auto report = Runtime::ValidatePublishedAnimationParity(Animation::AnimationSystem::Instance());
			context.Expect(!report.succeeded && report.animatorCount == 0U,
				"Parity validation must fail closed when no animator is published.");
		});
		registry.Add("Engine.AnimationParity.ReportPreservesRequestedFixtureContract", [](TestContext& context) {
			Runtime::AnimationParityValidationOptions options;
			options.tickCount = 77U;
			options.fixedDeltaSeconds = 0.02F;
			options.mode = "rendered-runtime";
			options.requireRenderedVisualCapture = true;
			const Runtime::AnimationParityReport report =
				Runtime::ValidatePublishedAnimationParity(
					Animation::AnimationSystem::Instance(), options);
			const nlohmann::json json = report.ToJson();
			context.Expect(!report.succeeded && report.requestedTicks == 77U
				&& report.fixedDeltaSeconds == 0.02F && report.mode == "rendered-runtime",
				"A fail-closed report must retain the exact bounded fixture identity.");
			context.Expect(json.at("visualCapture").at("required").is_boolean()
				&& json.at("visualCapture").at("required").get<bool>()
				&& json.at("visualCapture").at("available").is_boolean()
				&& !json.at("visualCapture").at("available").get<bool>(),
				"The rendered-lane capture contract must use typed booleans even when evaluation fails.");
		});
		registry.Add("Engine.AnimationParity.HeadlessLifecycleConfigurationIsExplicit", [](TestContext& context) {
			Animation::AnimationSystem& animation = Animation::AnimationSystem::Instance();
			animation.SetHeadlessExecutionRequired(true);
			context.Expect(animation.IsHeadlessExecutionRequired(),
				"A renderer-free exported runtime must publish an explicit headless execution requirement.");
			animation.SetHeadlessExecutionRequired(false);
			context.Expect(!animation.IsHeadlessExecutionRequired(),
				"Normal rendered startup must be able to clear the headless execution requirement.");
		});
		registry.Add("Engine.AnimationParity.DigestIgnoresRunLocalGenerations", [](TestContext& context) {
			auto pose = MakeDigestPose();
			const auto before = Runtime::ComputeAnimationParityAnimatorDigest(pose);
			pose.objectGeneration = 900U; pose.componentGeneration = 901U; pose.authorityPublicationGeneration = 902U;
			pose.snapshot = MakeDigestPose(1.0F, 99U, 903U).snapshot;
			pose.controller.generation = 904U;
			pose.clip.generation = 905U;
			pose.skeleton.generation = 906U;
			pose.effectiveExecutionMode = Animation::AnimationEffectiveExecutionMode::GpuVisual;
			const auto after = Runtime::ComputeAnimationParityAnimatorDigest(pose);
			context.ExpectEqual(before.stateHash, after.stateHash, "State digest must ignore run-local generations and mode.");
			context.ExpectEqual(before.poseHash, after.poseHash, "Pose digest must ignore run-local generations and mode.");
		});
		registry.Add("Engine.AnimationParity.DigestChangesForSemanticMutation", [](TestContext& context) {
			auto pose = MakeDigestPose();
			const auto before = Runtime::ComputeAnimationParityAnimatorDigest(pose);
			const auto playbackChanged = Runtime::ComputeAnimationParityAnimatorDigest(MakeDigestPose(1.0F, 100U));
			context.Expect(before.stateHash != playbackChanged.stateHash, "Playback mutation must change state digest.");
			const auto poseChanged = Runtime::ComputeAnimationParityAnimatorDigest(MakeDigestPose(2.0F));
			context.Expect(before.poseHash != poseChanged.poseHash, "Pose mutation must change pose digest.");
		});
	}
}
