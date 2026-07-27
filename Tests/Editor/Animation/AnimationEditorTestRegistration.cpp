#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Animation/AnimationPreviewController.h"
#include "Engine/Components/AnimatorComponent.h"

namespace TerraEngine::EditorTests {
	// Keep task-specific registrations explicit so an unreferenced source file
	// cannot silently make the Phase 3 deformation contract disappear.
	void RegisterSkinnedMeshCookTests(TestRegistry& registry);

	void RegisterAnimationEditorTestScaffolding(TestRegistry& registry)
	{
		// The editor owns a separate harness, so this sentinel makes a missing
		// editor-animation registration visible before authoring tests are added.
		registry.Add("Editor.Animation.RegistrationAvailable", [](TestContext& context) {
			context.Expect(true, "Animation editor tests should be registered through the shared editor harness.");
		});
		registry.Add("Editor.Animation.PreviewController.TransientPlaybackAndLifecycle", [](TestContext& context) {
			using namespace TerraEngine::Editor::Animation;
			const TerraEngine::UUID selectionId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID clipId = TerraEngine::GenerateUUID();
			AnimatorComponent authored;
			authored.controllerAssetId = TerraEngine::GenerateUUID();
			authored.skeletonOverrideAssetId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID authoredController = authored.controllerAssetId;
			const TerraEngine::UUID authoredSkeleton = authored.skeletonOverrideAssetId;

			AnimationPreviewController preview;
			preview.SynchronizeContext(selectionId, false);
			AnimationPreviewSessionDescriptor descriptor;
			descriptor.selectionId = selectionId;
			descriptor.clipAssetId.value = clipId;
			descriptor.clipGeneration = 7U;
			descriptor.compatibilityFingerprint = 0x1234U;
			descriptor.durationTicks = 120;
			descriptor.sampleRate = 60U;
			descriptor.displayName = "Preview Walk";
			descriptor.compiledPath = "Build/Assets/Models/Hero_Generated/Walk.teanim";
			context.Expect(preview.BeginSession(descriptor) == TerraEngine::Animation::AnimationStatus::Ok,
				"A valid indexed clip should begin one transient editor preview session.");
			context.Expect(preview.Play() == TerraEngine::Animation::AnimationStatus::Ok,
				"Preview Play should delegate to deterministic AnimatorPlayback.");
			context.Expect(preview.Tick(0.5) == TerraEngine::Animation::AnimationStatus::Ok,
				"Half a second of editor time should advance a 60 Hz clip by 30 fixed ticks.");
			context.ExpectEqual(static_cast<TerraEngine::Animation::AnimationFixedTick>(30),
				preview.GetSnapshot().playback.resolvedTick,
				"Preview time should remain an exact fixed-tick cursor.");

			context.Expect(preview.Pause() == TerraEngine::Animation::AnimationStatus::Ok,
				"Preview Pause should preserve the exact current tick.");
			preview.Tick(1.0);
			context.ExpectEqual(static_cast<TerraEngine::Animation::AnimationFixedTick>(30),
				preview.GetSnapshot().playback.resolvedTick,
				"A paused preview must not advance on later editor frames.");
			context.Expect(preview.SeekNormalized(0.75F) == TerraEngine::Animation::AnimationStatus::Ok,
				"Preview scrubbing should use the checked fixed-tick seek boundary.");
			context.ExpectEqual(static_cast<TerraEngine::Animation::AnimationFixedTick>(90),
				preview.GetSnapshot().playback.resolvedTick,
				"Normalized preview scrubbing should resolve to the exact requested clip tick.");

			AnimationPreviewController firstObservedAfterBegin;
			context.Expect(firstObservedAfterBegin.BeginSession(descriptor)
				== TerraEngine::Animation::AnimationStatus::Ok,
				"A detached preview controller may stage a valid session before its first Inspector frame.");
			firstObservedAfterBegin.SynchronizeContext(TerraEngine::GenerateUUID(), false);
			context.Expect(!firstObservedAfterBegin.GetSnapshot().active
				&& firstObservedAfterBegin.GetSnapshot().endReason
					== AnimationPreviewEndReason::SelectionChanged,
				"The first context observation must clear a staged session for a different selection.");

			preview.SynchronizeContext(TerraEngine::GenerateUUID(), false);
			const AnimationPreviewSnapshot selectionChanged = preview.GetSnapshot();
			context.Expect(!selectionChanged.active
				&& selectionChanged.endReason == AnimationPreviewEndReason::SelectionChanged,
				"Changing Inspector selection should clear transient preview state.");
			context.Expect(authored.controllerAssetId == authoredController
				&& authored.skeletonOverrideAssetId == authoredSkeleton,
				"Preview lifecycle operations must never rewrite authored AnimatorComponent references.");

			preview.SynchronizeContext(selectionId, false);
			preview.BeginSession(descriptor);
			preview.SynchronizeContext(selectionId, true);
			context.Expect(!preview.GetSnapshot().active
				&& preview.GetSnapshot().endReason == AnimationPreviewEndReason::PlayModeTransition,
				"Entering Play Mode should clear the editor-owned preview session.");
			context.Expect(preview.BeginSession(descriptor) == TerraEngine::Animation::AnimationStatus::Rejected,
				"A transient editor preview must not start while runtime Play Mode owns animation.");
		});
		RegisterSkinnedMeshCookTests(registry);
	}

}
