// AnimationRenderFallbackTests.cpp - Generation-safe renderer pose fallback tests.

#include "TestHarness.h"

#include "Animation/AnimationSystem.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Renderer/RenderSystem.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::Renderer;

		UUID MakeUuid(std::uint8_t seed) {
			UUID value{};
			for(std::size_t index = 0U; index < value.size(); ++index) {
				value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return value;
		}

		AnimationRenderBindingRequest MakeRequest() {
			AnimationRenderBindingRequest request;
			request.meshId = MakeUuid(10U);
			request.animatorObjectId = MakeUuid(30U);
			request.skeletonAssetId = MakeUuid(50U);
			request.animatorObjectGeneration = 6U;
			request.animationAuthorityGeneration = 9U;
			request.animatorSlotIndex = 2U;
			request.animatorGeneration = 5U;
			request.deformablePayloadGeneration = 2U;
			request.componentGeneration = 3U;
			request.skeletonGeneration = 4U;
			request.skeletonCompatibilityFingerprint = 0x123456789abcdef0ULL;
			request.presentationFrame = std::make_shared<AnimationPresentationFrame>();
			return request;
		}

		std::shared_ptr<AnimationAuthorityRow> MakeAuthorityRow(
			const AnimationRenderBindingRequest& request,
			AnimationGeneration poseGeneration = 7U,
			float modelTranslationX = 0.0F,
			float morphWeight = 0.0F) {
			const AnimatorInstanceHandle animator{2U, 5U};
			AnimationAssetHandle skeleton;
			skeleton.assetId.value = request.skeletonAssetId;
			skeleton.generation = request.skeletonGeneration;

			auto poseBuffer = std::make_shared<PoseBuffer>();
			PoseBufferLayout layout;
			layout.jointCount = 1U;
			layout.morphCount = 1U;
			AnimationPoseGeneration generation;
			generation.animator = animator;
			generation.poseGeneration = poseGeneration;
			generation.skeleton = skeleton;
			generation.programGeneration = 1U;
			poseBuffer->BeginWrite(layout, generation);
			poseBuffer->MutableLocalTransforms()[0U] = {};
			poseBuffer->MutableModelTransforms()[0U] = {};
			poseBuffer->MutableModelTransforms()[0U].translation.x =
				modelTranslationX;
			poseBuffer->MutableMorphValues()[0U] = {9001U, morphWeight};
			poseBuffer->Seal();

			auto snapshot = std::make_shared<AnimationSnapshot>();
			snapshot->animator = animator;
			snapshot->publicationGeneration = poseGeneration;
			snapshot->playback.latestStatus = AnimationStatus::Ok;
			snapshot->pose = std::move(poseBuffer);

			auto publication = std::make_shared<AnimationAuthorityRow>();
			publication->key = {animator.slotIndex, animator.generation, 1U};
			publication->ownerObjectId = request.animatorObjectId;
			publication->objectGeneration = request.animatorObjectGeneration;
			publication->componentGeneration = request.componentGeneration;
			publication->sourceGeneration = request.animationAuthorityGeneration;
			publication->poseGeneration = poseGeneration;
			publication->authorityPublicationGeneration = request.animationAuthorityGeneration;
			publication->skeleton = skeleton;
			auto skeletonAsset = std::make_shared<SkeletonAsset>();
			skeletonAsset->assetId.value = request.skeletonAssetId;
			skeletonAsset->compatibilityFingerprint =
				request.skeletonCompatibilityFingerprint;
			skeletonAsset->source.sourcePackagePath = "fixture.glb";
			SkeletonJoint joint;
			joint.stableId = 42U;
			joint.parentIndex = -1;
			skeletonAsset->joints.push_back(joint);
			AnimationMatrix4x4 inverseBind{};
			inverseBind.values = {
				1.0F, 0.0F, 0.0F, 0.0F,
				0.0F, 1.0F, 0.0F, 0.0F,
				0.0F, 0.0F, 1.0F, 0.0F,
				0.0F, 0.0F, 0.0F, 1.0F};
			skeletonAsset->inverseBindMatrices.push_back(inverseBind);
			publication->skeletonAsset = std::move(skeletonAsset);
			publication->jointStableIds = {42U};
			publication->skeletonCompatibilityFingerprint =
				request.skeletonCompatibilityFingerprint;
			publication->status = AnimationStatus::Ok;
			publication->safeFallback = false;
			publication->snapshot = std::move(snapshot);
			return publication;
		}
	}

	void RegisterAnimationRenderFallbackTests(TestRegistry& registry) {
		registry.Add("Animation.RenderFallback.ValidCurrentPoseKeepsImmutablePublication", [](TestContext& context) {
			AnimationRenderBindingRequest request = MakeRequest();
			const std::shared_ptr<AnimationAuthorityRow> expectedOwner = MakeAuthorityRow(request);
			request.authorityRow = expectedOwner.get();

			const AnimationRenderBinding result = RenderSystem::ResolveAnimationRenderBinding(request);
			context.Expect(result.status == AnimationRenderBindingStatus::Ready
				&& result.source == AnimationRenderPoseSource::CpuPose
				&& result.HasCpuPose() && result.authorityRow == expectedOwner.get()
				&& result.posePublicationGeneration == 7U && !result.safePose,
				"A fully matching publication should remain alive through the copied draw binding.");
		});

		registry.Add("Animation.RenderFallback.CurrentMismatchFailsClosedToBindPose", [](TestContext& context) {
			AnimationRenderBindingRequest request = MakeRequest();
			const std::shared_ptr<AnimationAuthorityRow> current = MakeAuthorityRow(request);
			request.authorityRow = current.get();

			AnimationRenderBindingRequest stale = request;
			++stale.componentGeneration;
			const AnimationRenderBinding staleResult = RenderSystem::ResolveAnimationRenderBinding(stale);
			context.Expect(staleResult.status == AnimationRenderBindingStatus::StaleGeneration
				&& staleResult.source == AnimationRenderPoseSource::BindPose
				&& !staleResult.HasCpuPose(),
				"A replaced AnimatorComponent generation must not reuse the previous publication.");

			AnimationRenderBindingRequest incompatible = request;
			incompatible.skeletonCompatibilityFingerprint ^= 1U;
			const AnimationRenderBinding incompatibleResult =
				RenderSystem::ResolveAnimationRenderBinding(incompatible);
			context.Expect(incompatibleResult.status == AnimationRenderBindingStatus::IncompatibleSkeleton
				&& incompatibleResult.source == AnimationRenderPoseSource::BindPose
				&& !incompatibleResult.HasCpuPose(),
				"A deformable mesh cooked for another skeleton fingerprint must use bind pose.");
		});

		registry.Add("Animation.RenderFallback.SafeCurrentPoseNeverUsesHeldData", [](TestContext& context) {
			AnimationRenderBindingRequest request = MakeRequest();
			const std::shared_ptr<AnimationAuthorityRow> current = MakeAuthorityRow(request);
			request.authorityRow = current.get();
			request.heldBinding = RenderSystem::ResolveAnimationRenderBinding(request);

			auto safePose = std::make_shared<AnimationAuthorityRow>(*current);
			safePose->status = AnimationStatus::NotReady;
			safePose->safeFallback = true;
			request.authorityRow = safePose.get();
			request.fallbackPolicy = AnimationRenderFallbackPolicy::HoldLastCompatiblePose;

			const AnimationRenderBinding result = RenderSystem::ResolveAnimationRenderBinding(request);
			context.Expect(result.status == AnimationRenderBindingStatus::SafePose
				&& result.source == AnimationRenderPoseSource::BindPose
				&& !result.HasCpuPose(),
				"A current dependency/safe-pose publication must visibly fail safe instead of hiding it with stale data.");
		});

		registry.Add("Animation.RenderFallback.HoldLastRequiresExactCompleteIdentity", [](TestContext& context) {
			AnimationRenderBindingRequest request = MakeRequest();
			const std::shared_ptr<AnimationAuthorityRow> current = MakeAuthorityRow(request);
			request.authorityRow = current.get();
			request.heldBinding = RenderSystem::ResolveAnimationRenderBinding(request);
			request.authorityRow = nullptr;
			request.fallbackPolicy = AnimationRenderFallbackPolicy::HoldLastCompatiblePose;

			const AnimationRenderBinding held = RenderSystem::ResolveAnimationRenderBinding(request);
			context.Expect(held.status == AnimationRenderBindingStatus::Ready
				&& held.source == AnimationRenderPoseSource::HeldCpuPose && held.HasCpuPose(),
				"A missing current publication may retain one fully compatible immutable pose.");

			AnimationRenderBindingRequest changedMesh = request;
			++changedMesh.deformablePayloadGeneration;
			const AnimationRenderBinding rejectedMesh =
				RenderSystem::ResolveAnimationRenderBinding(changedMesh);
			AnimationRenderBindingRequest changedSkeleton = request;
			++changedSkeleton.skeletonGeneration;
			const AnimationRenderBinding rejectedSkeleton =
				RenderSystem::ResolveAnimationRenderBinding(changedSkeleton);
			AnimationRenderBindingRequest retiredAuthority = request;
			++retiredAuthority.animationAuthorityGeneration;
			const AnimationRenderBinding rejectedAuthority =
				RenderSystem::ResolveAnimationRenderBinding(retiredAuthority);
			context.Expect(!rejectedMesh.HasCpuPose()
				&& rejectedMesh.source == AnimationRenderPoseSource::BindPose
				&& !rejectedSkeleton.HasCpuPose()
				&& rejectedSkeleton.source == AnimationRenderPoseSource::BindPose
				&& !rejectedAuthority.HasCpuPose()
				&& rejectedAuthority.source == AnimationRenderPoseSource::BindPose,
				"Hold-last must reject changed mesh, skeleton, or animation-authority generations rather than mix publications.");
		});

		registry.Add("Animation.RenderFallback.HeldPublicationCannotHideSafeState", [](TestContext& context) {
			AnimationRenderBindingRequest request = MakeRequest();
			const std::shared_ptr<AnimationAuthorityRow> current = MakeAuthorityRow(request);
			request.authorityRow = current.get();
			request.heldBinding = RenderSystem::ResolveAnimationRenderBinding(request);
			auto safePublication = std::make_shared<AnimationAuthorityRow>(*current);
			safePublication->status = AnimationStatus::NotReady;
			safePublication->safeFallback = true;
			request.heldBinding.authorityRow = safePublication.get();
			request.authorityRow = nullptr;
			request.fallbackPolicy = AnimationRenderFallbackPolicy::HoldLastCompatiblePose;

			const AnimationRenderBinding result = RenderSystem::ResolveAnimationRenderBinding(request);
			context.Expect(result.source == AnimationRenderPoseSource::BindPose
				&& !result.HasCpuPose(),
				"A forged ready outer record must not retain a publication that explicitly reports safe/error state.");
		});

		registry.Add("Animation.RenderFallback.MalformedPublicationIsAtomicBindPose", [](TestContext& context) {
			AnimationRenderBindingRequest request = MakeRequest();
			const std::shared_ptr<AnimationAuthorityRow> valid = MakeAuthorityRow(request);
			auto malformed = std::make_shared<AnimationAuthorityRow>(*valid);
			auto malformedSnapshot = std::make_shared<AnimationSnapshot>(*valid->snapshot);
			++malformedSnapshot->publicationGeneration;
			malformed->snapshot = std::move(malformedSnapshot);
			request.authorityRow = malformed.get();

			const AnimationRenderBinding result = RenderSystem::ResolveAnimationRenderBinding(request);
			context.Expect(result.status == AnimationRenderBindingStatus::InvalidPose
				&& result.source == AnimationRenderPoseSource::BindPose
				&& !result.HasCpuPose() && result.posePublicationGeneration == 0U,
				"A mixed snapshot/pose generation must leave no partial CPU binding in the draw command.");
		});

		registry.Add("Animation.RenderFallback.CpuPoseBuildsChangingGraphicsPalette", [](TestContext& context) {
			const AnimationRenderBindingRequest request = MakeRequest();
			const std::shared_ptr<AnimationAuthorityRow> first =
				MakeAuthorityRow(request, 7U, 0.0F, 0.25F);
			const std::shared_ptr<AnimationAuthorityRow> second =
				MakeAuthorityRow(request, 8U, 2.0F, 0.75F);
			const std::array<std::uint32_t, 1U> paletteJoints{0U};
			const std::array<std::uint64_t, 1U> morphTargetIds{9001U};
			const auto firstDeformation =
				RenderSystem::BuildCpuAnimationDeformation(
					*first, paletteJoints, morphTargetIds);
			const auto secondDeformation =
				RenderSystem::BuildCpuAnimationDeformation(
					*second, paletteJoints, morphTargetIds);

			context.Expect(firstDeformation != nullptr
				&& secondDeformation != nullptr
				&& firstDeformation->IsComplete()
				&& secondDeformation->IsComplete(),
				"A sealed CPU fallback pose must produce a graphics-consumable palette.");
			if(firstDeformation != nullptr && secondDeformation != nullptr) {
				context.Expect(
					firstDeformation->paletteMatrices.front().row0[3U]
						== 0.0F
					&& secondDeformation->paletteMatrices.front().row0[3U]
						== 2.0F,
					"Advancing CPU poses must publish different skinning matrices instead of leaving the mesh in bind pose.");
				context.Expect(firstDeformation->morphWeights
						== std::vector<float>{0.25F}
					&& secondDeformation->morphWeights
						== std::vector<float>{0.75F},
					"Advancing CPU poses must preserve mesh-ordered morph weights for the graphics bridge.");
			}
		});
	}
}
