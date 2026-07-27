// AnimationPreviewTests.cpp - Isolated editor preview-world regression coverage.

#include "TestHarness.h"

#include "Animation/Assets/SkeletonAsset.h"
#include "Editor/AssetCompilers/AnimationClipCompiler.h"
#include "Editor/TerraEngine-Editor/Animation/AnimationPreviewController.h"
#include "Engine/Components/AnimatorComponent.h"
#include "Engine/Core/Object/ObjectManager.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace TerraEngine::EditorTests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;
		using namespace TerraEngine::Editor::Animation;

		AnimationGuid MakeGuid(std::uint8_t seed) {
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		struct PreviewFixture {
			AnimationStatus compileStatus = AnimationStatus::InternalError;
			AnimationPreviewSessionDescriptor descriptor;
			AnimationPreviewAssetInput input;
		};

		PreviewFixture MakePreviewFixture(
			const UUID& selectionId,
			std::uint8_t assetSeed,
			AnimationGeneration clipGeneration = 3U) {
			PreviewFixture fixture;
			AnimationClipCompileInput compileInput;
			compileInput.assetId = MakeGuid(assetSeed);
			compileInput.skeletonAssetId = MakeGuid(static_cast<std::uint8_t>(assetSeed + 32U));
			compileInput.compatibilityFingerprint = 0x7a4bd19e8c205f13ULL + assetSeed;
			compileInput.durationTicks = 60;
			compileInput.sampleRate = 60U;
			compileInput.segmentDurationTicks = 60;

			// The real editor compiler produces the test clip. Sampling it through the
			// production CPU evaluator proves renderer-unavailable preview is not a
			// synthetic cursor-only success path.
			AnimationSourceTrack translation;
			translation.stableTargetId = 101U;
			translation.kind = AnimationClipTrackKind::Translation;
			translation.interpolation = AnimationInterpolation::Linear;
			translation.keys = {
				{0, {0.0F, 0.0F, 0.0F}, {}, 0.0F},
				{60, {10.0F, 0.0F, 0.0F}, {}, 0.0F}
			};
			compileInput.tracks.push_back(std::move(translation));

			AnimationClipCompilerSettings settings;
			settings.translationToleranceMeters = 0.0F;
			AnimationClipCompileReport report;
			fixture.compileStatus = CompileAnimationClip(
				compileInput, settings, fixture.input.clip, report);
			fixture.input.clip.runtimeGeneration = clipGeneration;

			SkeletonAsset skeleton;
			skeleton.assetId = compileInput.skeletonAssetId;
			skeleton.runtimeGeneration = 5U;
			skeleton.compatibilityFingerprint = compileInput.compatibilityFingerprint;
			skeleton.referencePoseHash = 0x915bf43aU;
			SkeletonJoint joint;
			joint.stableId = 101U;
			joint.parentIndex = -1;
			joint.localBindTransform.translation = {0.0F, 0.0F, 0.0F};
			joint.displayName = "Root";
			joint.normalizedSourcePath = "root";
			joint.semanticRole = "Root";
			skeleton.joints.push_back(std::move(joint));
			fixture.input.referenceSkeleton = std::move(skeleton);
			fixture.input.jointStableIds = {101U};

			fixture.descriptor.selectionId = selectionId;
			fixture.descriptor.clipAssetId = compileInput.assetId;
			fixture.descriptor.clipGeneration = clipGeneration;
			fixture.descriptor.compatibilityFingerprint = compileInput.compatibilityFingerprint;
			fixture.descriptor.durationTicks = compileInput.durationTicks;
			fixture.descriptor.sampleRate = compileInput.sampleRate;
			fixture.descriptor.displayName = "Preview Fixture";
			fixture.descriptor.looping = true;
			fixture.descriptor.speed = 1.0F;
			return fixture;
		}

		bool NearlyEqual(float left, float right, float tolerance = 0.001F) {
			return std::abs(left - right) <= tolerance;
		}

		float FirstJointX(const AnimationPreviewSessionState& state) {
			return state.output.joints.empty()
				? std::numeric_limits<float>::quiet_NaN()
				: state.output.joints.front().localTransform.translation.x;
		}

		AnimationPreviewLiveInstanceSnapshot MakeLiveSnapshot(
			AnimatorInstanceHandle instance,
			AnimationGeneration poseGeneration,
			std::uint64_t publicationSequence,
			AnimationFixedTick tick,
			float translationX) {
			AnimationPreviewLiveInstanceSnapshot snapshot;
			snapshot.instance = instance;
			snapshot.poseGeneration = poseGeneration;
			snapshot.publicationSequence = publicationSequence;
			snapshot.world.tick = tick;
			snapshot.world.complete = true;
			AnimationPreviewJointOutput joint;
			joint.stableJointId = 101U;
			joint.localTransform.translation.x = translationX;
			snapshot.world.joints.push_back(joint);
			return snapshot;
		}
	}

	void RegisterAnimationPreviewTests(TestRegistry& registry) {
		registry.Add("Editor.Animation.Preview.SessionIsolationAndCopiedInputs", [](TestContext& context) {
			const UUID selectionId = GenerateUUID();
			PreviewFixture firstFixture = MakePreviewFixture(selectionId, 11U);
			PreviewFixture secondFixture = MakePreviewFixture(selectionId, 51U);
			context.Expect(firstFixture.compileStatus == AnimationStatus::Ok
				&& secondFixture.compileStatus == AnimationStatus::Ok,
				"Preview fixtures must be real compiler-produced clips.");

			AnimationPreviewController preview;
			preview.SynchronizeContext(selectionId, false);
			AnimationPreviewSessionHandle first;
			AnimationPreviewSessionHandle second;
			context.Expect(preview.CreateIsolatedSession(
				firstFixture.descriptor, firstFixture.input, true, first) == AnimationStatus::Ok,
				"The first copied asset input should publish an isolated preview world.");
			context.Expect(preview.CreateIsolatedSession(
				secondFixture.descriptor, secondFixture.input, true, second) == AnimationStatus::Ok,
				"The second copied asset input should publish an independent preview world.");

			// Mutating caller-owned inputs after creation must not affect the copied
			// immutable generation retained by either session.
			firstFixture.input.clip.durationTicks = 1;
			firstFixture.input.referenceSkeleton->joints.front().localBindTransform.translation.x = 999.0F;
			context.Expect(preview.Play(first) == AnimationStatus::Ok
				&& preview.Play(second) == AnimationStatus::Ok
				&& preview.SetSpeed(first, 2.0F) == AnimationStatus::Ok,
				"Each isolated session should own its transport controls.");
			context.Expect(preview.TickSession(first, 0.25) == AnimationStatus::Ok
				&& preview.TickSession(second, 0.25) == AnimationStatus::Ok,
				"Independent CPU preparations should publish through separate session slots.");

			AnimationPreviewSessionState firstState;
			AnimationPreviewSessionState secondState;
			context.Expect(preview.GetSessionState(first, firstState) == AnimationStatus::Ok
				&& preview.GetSessionState(second, secondState) == AnimationStatus::Ok,
				"Both live generation-safe handles should resolve to copied output snapshots.");
			context.Expect(firstState.playback.resolvedTick == 30
				&& secondState.playback.resolvedTick == 15
				&& NearlyEqual(FirstJointX(firstState), 5.0F)
				&& NearlyEqual(FirstJointX(secondState), 2.5F),
				"Speed and sampled pose output must remain isolated between preview sessions.");
			context.Expect(preview.SessionCount() == 2U,
				"The controller should expose the bounded count of active private worlds.");
		});

		registry.Add("Editor.Animation.Preview.TransportScrubStepReverseSpeedLoop", [](TestContext& context) {
			const UUID selectionId = GenerateUUID();
			PreviewFixture fixture = MakePreviewFixture(selectionId, 17U);
			AnimationPreviewController preview;
			preview.SynchronizeContext(selectionId, false);
			AnimationPreviewSessionHandle handle;
			context.Expect(fixture.compileStatus == AnimationStatus::Ok
				&& preview.CreateIsolatedSession(
					fixture.descriptor, fixture.input, true, handle) == AnimationStatus::Ok,
				"Transport tests require one valid isolated preview session.");

			context.Expect(preview.Play(handle) == AnimationStatus::Ok
				&& preview.TickSession(handle, 0.25) == AnimationStatus::Ok
				&& preview.Pause(handle) == AnimationStatus::Ok,
				"Play and pause should advance then preserve an exact fixed-tick cursor.");
			AnimationPreviewSessionState paused;
			context.Expect(preview.GetSessionState(handle, paused) == AnimationStatus::Ok,
				"The paused preview snapshot should remain readable through its live handle.");
			context.Expect(preview.TickSession(handle, 1.0) == AnimationStatus::Ok,
				"A paused session may still publish its unchanged complete CPU snapshot.");
			AnimationPreviewSessionState stillPaused;
			context.Expect(preview.GetSessionState(handle, stillPaused) == AnimationStatus::Ok,
				"The unchanged paused snapshot should publish through the protected boundary.");
			context.Expect(paused.playback.resolvedTick == 15
				&& stillPaused.playback.resolvedTick == 15,
				"Paused preview time must not advance on later editor frames.");

			context.Expect(preview.Seek(handle, 45) == AnimationStatus::Ok
				&& preview.Step(handle, -5) == AnimationStatus::Ok,
				"Scrub and frame-step controls should use the checked playback boundary.");
			AnimationPreviewSessionState scrubbed;
			context.Expect(preview.GetSessionState(handle, scrubbed) == AnimationStatus::Ok,
				"A successful scrub should expose its complete copied CPU output.");
			context.Expect(scrubbed.playback.resolvedTick == 40
				&& NearlyEqual(FirstJointX(scrubbed), 6.6667F, 0.002F),
				"CPU output must be republished at the exact scrubbed tick.");

			context.Expect(preview.SetLooping(handle, true) == AnimationStatus::Ok
				&& preview.SetReverse(handle, true) == AnimationStatus::Ok
				&& preview.SetSpeed(handle, 1.0F) == AnimationStatus::Ok
				&& preview.Seek(handle, 5) == AnimationStatus::Ok
				&& preview.Resume(handle) == AnimationStatus::Ok
				&& preview.TickSession(handle, 0.1) == AnimationStatus::Ok,
				"Reverse looping playback should use the same deterministic transport controls.");
			AnimationPreviewSessionState reversed;
			context.Expect(preview.GetSessionState(handle, reversed) == AnimationStatus::Ok,
				"Reverse playback should publish one readable complete snapshot.");
			context.Expect(reversed.playback.resolvedTick == 59
				&& reversed.playback.loopCount == -1,
				"Reverse traversal across zero should wrap to the prior loop exactly once.");
		});

		registry.Add("Editor.Animation.Preview.StaleHandleAndPreparedPublicationRejection", [](TestContext& context) {
			const UUID selectionId = GenerateUUID();
			PreviewFixture fixture = MakePreviewFixture(selectionId, 23U);
			AnimationPreviewController preview;
			preview.SynchronizeContext(selectionId, false);
			AnimationPreviewSessionHandle original;
			context.Expect(fixture.compileStatus == AnimationStatus::Ok
				&& preview.CreateIsolatedSession(
					fixture.descriptor, fixture.input, true, original) == AnimationStatus::Ok
				&& preview.Play(original) == AnimationStatus::Ok,
				"A valid session should be ready for split prepare/publication testing.");

			AnimationPreviewPreparedFrame staleFrame;
			context.Expect(preview.PrepareTick(original, 0.25, staleFrame) == AnimationStatus::Ok,
				"Worker-safe preparation should produce a copied candidate without publication.");
			context.Expect(preview.SetSpeed(original, 2.0F) == AnimationStatus::Ok,
				"A protected transport edit should advance the session revision.");
			context.Expect(preview.PublishPreparedFrame(staleFrame) == AnimationStatus::StaleHandle,
				"A prepared frame from an older session revision must be rejected atomically.");

			AnimationPreviewTeardownResult teardown;
			context.Expect(preview.DestroySession(
				original, AnimationPreviewEndReason::Explicit, &teardown) == AnimationStatus::Ok,
				"Destroying a live handle should retire its slot generation.");
			AnimationPreviewSessionHandle replacement;
			context.Expect(preview.CreateIsolatedSession(
				fixture.descriptor, fixture.input, true, replacement) == AnimationStatus::Ok,
				"A retired slot may be reused only with a new generation.");
			AnimationPreviewSessionState staleState;
			context.Expect(replacement.slotIndex == original.slotIndex
				&& replacement.generation != original.generation
				&& preview.Play(original) == AnimationStatus::StaleHandle
				&& preview.GetSessionState(original, staleState) == AnimationStatus::StaleHandle,
				"Every stale handle path must reject after slot reuse instead of aliasing the replacement.");
		});

		registry.Add("Editor.Animation.Preview.NoAuthoredObjectOrComponentLeakage", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object authoredObject = objects.createObject("Authored Preview Source");
			AnimatorComponent* authored = objects.AddComponent<AnimatorComponent>(authoredObject);
			context.Expect(authored != nullptr,
				"The leakage regression fixture requires one authored AnimatorComponent.");
			authored->controllerAssetId = GenerateUUID();
			authored->defaultClipAssetId = GenerateUUID();
			authored->initialParametersJson = "{\"Speed\":0.75}";
			authored->requestedMode = AnimationExecutionPolicy::ForceHybridAuthoritative;
			const AnimatorComponent authoredBefore = *authored;
			const std::uint64_t componentGenerationBefore =
				objects.getObjectComponentGeneration(authoredObject);

			const UUID selectionId = objects.getUUID(authoredObject);
			PreviewFixture fixture = MakePreviewFixture(selectionId, 29U);
			AnimationPreviewController preview;
			preview.SynchronizeContext(selectionId, false);
			AnimationPreviewSessionHandle handle;
			context.Expect(fixture.compileStatus == AnimationStatus::Ok
				&& preview.CreateIsolatedSession(
					fixture.descriptor, fixture.input, false, handle) == AnimationStatus::Ok
				&& preview.Play(handle) == AnimationStatus::Ok
				&& preview.TickSession(handle, 0.5) == AnimationStatus::Ok
				&& preview.SeekNormalized(handle, 0.75F) == AnimationStatus::Ok,
				"Preview operations should complete entirely inside the private preview world.");

			AnimationPreviewSessionState beforeTeardown;
			context.Expect(preview.GetSessionState(handle, beforeTeardown) == AnimationStatus::Ok,
				"The isolated world should remain readable immediately before teardown.");
			AnimationPreviewTeardownResult teardown;
			context.Expect(preview.DestroySession(
				handle, AnimationPreviewEndReason::Explicit, &teardown) == AnimationStatus::Ok,
				"Teardown should restore and retire the complete private preview world.");
			const AnimatorComponent* authoredAfter = objects.GetComponent<AnimatorComponent>(authoredObject);
			context.Expect(authoredAfter != nullptr
				&& authoredAfter->controllerAssetId == authoredBefore.controllerAssetId
				&& authoredAfter->defaultClipAssetId == authoredBefore.defaultClipAssetId
				&& authoredAfter->initialParametersJson == authoredBefore.initialParametersJson
				&& authoredAfter->requestedMode == authoredBefore.requestedMode
				&& objects.getObjectComponentGeneration(authoredObject) == componentGenerationBefore,
				"Preview creation, playback, scrub, and teardown must not mutate authored component storage.");
			context.Expect(objects.isObjectValid(authoredObject)
				&& objects.getObjectsWithComponentsIncludingInactive<AnimatorComponent>().size() == 1U
				&& teardown.authoredMutationCount == 0U
				&& teardown.restoredOutput == teardown.initialOutput
				&& beforeTeardown.output != teardown.restoredOutput,
				"Teardown must restore only its isolated world and create/delete no authored objects.");
			objects.clear();
		});

		registry.Add("Editor.Animation.Preview.LiveInstanceSelectionIsReadOnly", [](TestContext& context) {
			const UUID selectionId = GenerateUUID();
			PreviewFixture fixture = MakePreviewFixture(selectionId, 37U);
			AnimationPreviewController preview;
			preview.SynchronizeContext(selectionId, true);
			const AnimatorInstanceHandle liveInstance{4U, 7U};
			AnimationPreviewLiveInstanceSnapshot source = MakeLiveSnapshot(
				liveInstance, 9U, 12U, 18, 3.0F);
			AnimationPreviewSessionHandle handle;
			context.Expect(fixture.compileStatus == AnimationStatus::Ok
				&& preview.SelectLiveInstance(
					fixture.descriptor, source, false, handle) == AnimationStatus::Ok,
				"An explicitly selected Play Mode instance should create a copied read-only session.");

			AnimationPreviewSessionState selected;
			context.Expect(preview.GetSessionState(handle, selected) == AnimationStatus::Ok,
				"The selected live-instance copy should be readable without touching runtime state.");
			context.Expect(selected.sourceMode == AnimationPreviewSourceMode::LiveInstanceReadOnly
				&& selected.readOnly
				&& selected.evaluationMode == AnimationPreviewEvaluationMode::LiveInstanceCopy
				&& NearlyEqual(FirstJointX(selected), 3.0F),
				"Live selection must be visibly distinct from an editable isolated asset preview.");
			context.Expect(preview.Play(handle) == AnimationStatus::Rejected
				&& preview.Seek(handle, 30) == AnimationStatus::Rejected
				&& preview.SetSpeed(handle, 2.0F) == AnimationStatus::Rejected,
				"Live-instance sessions must reject transport writes instead of changing runtime state.");

			AnimationPreviewLiveInstanceSnapshot stale = source;
			stale.publicationSequence = 11U;
			context.Expect(preview.UpdateLiveInstance(handle, stale) == AnimationStatus::StaleHandle,
				"An older copied live publication must not replace the selected output.");
			AnimationPreviewLiveInstanceSnapshot next = MakeLiveSnapshot(
				liveInstance, 10U, 13U, 24, 8.0F);
			context.Expect(preview.UpdateLiveInstance(handle, next) == AnimationStatus::Ok,
				"A newer matching live generation should publish as one complete copied snapshot.");
			next.world.joints.front().localTransform.translation.x = 999.0F;
			AnimationPreviewSessionState updated;
			context.Expect(preview.GetSessionState(handle, updated) == AnimationStatus::Ok,
				"The newer live-instance publication should be readable as an immutable copy.");
			context.Expect(updated.output.tick == 24 && NearlyEqual(FirstJointX(updated), 8.0F),
				"Caller mutation after publication must not alter the retained live-instance copy.");

			preview.SynchronizeContext(selectionId, false);
			context.Expect(preview.GetSessionState(handle, updated) == AnimationStatus::StaleHandle
				&& preview.SessionCount() == 0U,
				"Leaving Play Mode must retire every live-instance selection and its generation.");
		});

		registry.Add("Editor.Animation.Preview.IsolatedAssetCoexistsWithPlayMode", [](TestContext& context) {
			const UUID selectionId = GenerateUUID();
			PreviewFixture fixture = MakePreviewFixture(selectionId, 41U);
			AnimationPreviewController preview;
			preview.SynchronizeContext(selectionId, true);
			AnimationPreviewSessionHandle handle;
			context.Expect(fixture.compileStatus == AnimationStatus::Ok
				&& preview.CreateIsolatedSession(
					fixture.descriptor, fixture.input, true, handle) == AnimationStatus::Ok
				&& preview.Play(handle) == AnimationStatus::Ok
				&& preview.TickSession(handle, 0.25) == AnimationStatus::Ok,
				"A copied isolated asset preview should remain usable while Play Mode owns only the live runtime world.");

			preview.SynchronizeContext(selectionId, true);
			AnimationPreviewSessionState duringPlay;
			context.Expect(preview.GetSessionState(handle, duringPlay) == AnimationStatus::Ok
				&& duringPlay.sourceMode == AnimationPreviewSourceMode::IsolatedAsset
				&& !duringPlay.readOnly
				&& duringPlay.output.complete,
				"Repeated Play Mode context synchronization must not retire or redirect the isolated preview world.");

			preview.SynchronizeContext(selectionId, false);
			AnimationPreviewSessionState afterPlay;
			context.Expect(preview.GetSessionState(handle, afterPlay) == AnimationStatus::Ok
				&& afterPlay.output == duringPlay.output,
				"Leaving Play Mode must not change an independent copied asset preview publication.");
		});

		registry.Add("Editor.Animation.Preview.RendererUnavailableUsesCpuReference", [](TestContext& context) {
			const UUID selectionId = GenerateUUID();
			PreviewFixture fixture = MakePreviewFixture(selectionId, 43U);
			AnimationPreviewController preview;
			preview.SynchronizeContext(selectionId, false);
			AnimationPreviewSessionHandle handle;
			context.Expect(fixture.compileStatus == AnimationStatus::Ok
				&& preview.CreateIsolatedSession(
					fixture.descriptor, fixture.input, false, handle) == AnimationStatus::Ok
				&& preview.Play(handle) == AnimationStatus::Ok
				&& preview.TickSession(handle, 0.5) == AnimationStatus::Ok,
				"Renderer loss must not prevent the copied CPU clip evaluator from advancing preview.");
			AnimationPreviewSessionState fallback;
			context.Expect(preview.GetSessionState(handle, fallback) == AnimationStatus::Ok,
				"Renderer loss should still leave a complete readable CPU preview snapshot.");
			context.Expect(!fallback.rendererAvailable
				&& fallback.evaluationMode == AnimationPreviewEvaluationMode::CpuRendererFallback
				&& fallback.output.complete
				&& fallback.output.tick == 30
				&& NearlyEqual(FirstJointX(fallback), 5.0F),
				"Renderer-unavailable output should be complete, inspectable CPU reference data.");

			context.Expect(preview.SetRendererAvailable(handle, true) == AnimationStatus::Ok,
				"Renderer recovery should update capability metadata without replacing the CPU authority.");
			AnimationPreviewSessionState recovered;
			context.Expect(preview.GetSessionState(handle, recovered) == AnimationStatus::Ok,
				"The recovered capability state should retain a readable CPU-authored snapshot.");
			context.Expect(recovered.rendererAvailable
				&& recovered.evaluationMode == AnimationPreviewEvaluationMode::CpuReference
				&& recovered.output == fallback.output,
				"Capability recovery must preserve the complete sampled output and cursor.");
		});
	}
}
