// RetargetValidationTests.cpp - Mapping diagnostics and hot-reload publication coverage.

#include "TestHarness.h"

#include "Animation/Retargeting/RetargetProfilePublisher.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationGuid Guid(std::uint8_t seed) {
			AnimationGuid value;
			for(std::size_t index = 0U; index < value.value.size(); ++index) {
				value.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return value;
		}

		SkeletonJoint Joint(
			std::uint64_t stableId,
			std::int32_t parentIndex,
			AnimationFloat3 translation,
			std::string_view semantic) {
			SkeletonJoint joint;
			joint.stableId = stableId;
			joint.parentIndex = parentIndex;
			joint.localBindTransform.translation = translation;
			joint.deformsMesh = true;
			joint.displayName = std::string(semantic);
			joint.normalizedSourcePath = std::string(semantic);
			joint.semanticRole = std::string(semantic);
			return joint;
		}

		SkeletonAsset MakeSourceSkeleton() {
			SkeletonAsset skeleton;
			skeleton.assetId = Guid(20U);
			skeleton.runtimeGeneration = 3U;
			skeleton.compatibilityFingerprint = 0x1111U;
			skeleton.referencePoseHash = 0x3333U;
			skeleton.joints = {
				Joint(1U, -1, {0.0F, 0.0F, 0.0F}, "Root"),
				Joint(2U, 0, {0.0F, 1.0F, 0.0F}, "Hips"),
				Joint(3U, 1, {0.0F, -0.5F, 0.0F}, "LeftLeg"),
				Joint(4U, 2, {0.0F, -0.5F, 0.0F}, "LeftFoot"),
				Joint(5U, 1, {0.5F, 0.3F, 0.0F}, "LeftArm"),
				Joint(6U, 4, {0.5F, 0.0F, 0.0F}, "LeftHand"),
				Joint(7U, 1, {0.0F, 0.0F, -0.4F}, "OptionalTail")};
			return skeleton;
		}

		SkeletonAsset MakeTargetSkeleton() {
			SkeletonAsset skeleton;
			skeleton.assetId = Guid(40U);
			skeleton.runtimeGeneration = 4U;
			skeleton.compatibilityFingerprint = 0x2222U;
			skeleton.referencePoseHash = 0x4444U;
			skeleton.joints = {
				Joint(101U, -1, {0.0F, 0.0F, 0.0F}, "Root"),
				Joint(102U, 0, {0.0F, 1.5F, 0.0F}, "Hips"),
				Joint(103U, 1, {0.0F, -0.75F, 0.0F}, "LeftLeg"),
				Joint(104U, 2, {0.0F, -0.25F, 0.0F}, "LegDetail"),
				Joint(105U, 3, {0.0F, -0.5F, 0.0F}, "LeftFoot"),
				Joint(106U, 1, {0.0F, 0.5F, 0.0F}, "LeftArm"),
				Joint(107U, 5, {0.0F, 0.25F, 0.0F}, "ArmTwist"),
				Joint(108U, 6, {0.0F, 0.25F, 0.0F}, "LeftHand")};
			return skeleton;
		}

		RetargetProfileAsset MakeProfile(
			const SkeletonAsset& source,
			const SkeletonAsset& target) {
			RetargetProfileAsset profile;
			profile.assetId = Guid(1U);
			profile.compatibilityFingerprint = 0x7788U;
			profile.sourceSkeletonAssetId = source.assetId;
			profile.targetSkeletonAssetId = target.assetId;
			profile.sourceSkeletonFingerprint = source.compatibilityFingerprint;
			profile.targetSkeletonFingerprint = target.compatibilityFingerprint;
			profile.sourceReferencePoseHash = source.referencePoseHash;
			profile.targetReferencePoseHash = target.referencePoseHash;
			profile.semanticMappings = {
				{1U, 0U, 0U, 0U, 1U, 101U, "Root", "Root", true},
				{2U, 1U, 0U, 0U, 2U, 102U, "Hips", "Hips", true},
				{3U, 2U, 0U, 0U, 3U, 103U, "LeftLeg", "LeftLeg", true},
				{4U, 3U, 0U, 0U, 4U, 105U, "LeftFoot", "LeftFoot", true},
				{5U, 2U, 10U, 0U, 5U, 106U, "LeftArm", "LeftArm", true},
				{6U, 5U, 0U, 0U, 6U, 108U, "LeftHand", "LeftHand", true}};
			// The optional target is deliberately absent. Its explicit fallback proves
			// reduced-fidelity profiles can remain publishable without hidden behavior.
			profile.explicitMappings = {{7U, 2U, 0U, 50U, 7U, 999U, false}};
			profile.chainMappings = {
				{10U, "LeftLeg", {2U, 3U, 4U}, {102U, 103U, 105U},
					RetargetTranslationScaleMode::PerChainLength, 1.0F, true},
				{11U, "LeftArm", {5U, 6U}, {106U, 108U},
					RetargetTranslationScaleMode::PerChainLength, 1.0F, true}};
			profile.orientationBases = {{10U, 5U, 106U, {},
				{0.0F, 0.0F, 0.70710677F, 0.70710677F}}};
			profile.proportion.sourceReferenceHeightMeters = 1.0F;
			profile.proportion.targetReferenceHeightMeters = 1.5F;
			profile.root = {1U, 101U, RetargetRootMotionMode::ScaleByProportion,
				true, true, 1.0F, 0.0F};
			profile.contacts = {{20U, 4U, 105U, 10U,
				kRetargetDefaultContactToleranceMeters, true, true, true}};
			profile.twistRules = {{30U, 5U, 6U, {1.0F, 0.0F, 0.0F},
				{107U}, {1.0F}}};
			profile.fallbackRules = {{50U, 7U, 0U,
				RetargetFallbackMode::ReferencePose, {108U}, true}};
			profile.quality.tier = RetargetQualityTier::Verified;
			profile.quality.authoredCoverage = 0.9F;
			profile.quality.expectedMeanPositionErrorMeters = 0.003F;
			profile.quality.expectedMaximumPositionErrorMeters = 0.010F;
			profile.quality.expectedMaximumRotationErrorRadians = 0.01F;
			profile.quality.validationProfileHash = 0xabcddcbaU;
			profile.quality.reviewReason = "Synthetic humanoid retarget validation";
			return profile;
		}

		bool HasCode(
			const RetargetValidationResult& result,
			RetargetDiagnosticCode code) {
			return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
				[code](const RetargetValidationDiagnostic& diagnostic) {
					return diagnostic.code == code;
				});
		}

		class ManualRetargetExecutor final : public RetargetProfileCompileExecutor {
		public:
			void Enqueue(std::function<void()> work) override {
				work_.push_back(std::move(work));
			}

			[[nodiscard]] std::size_t PendingCount() const noexcept {
				return work_.size();
			}

			void RunNext() {
				std::function<void()> work = std::move(work_.front());
				work_.erase(work_.begin());
				work();
			}

			void RunLast() {
				std::function<void()> work = std::move(work_.back());
				work_.pop_back();
				work();
			}

		private:
			std::vector<std::function<void()>> work_;
		};

		RetargetProfileCompileRequest MakeRequest(AnimationGeneration sourceGeneration = 1U) {
			RetargetProfileCompileRequest request;
			request.sourceSkeleton = MakeSourceSkeleton();
			request.targetSkeleton = MakeTargetSkeleton();
			request.profile = MakeProfile(request.sourceSkeleton, request.targetSkeleton);
			request.sourceGeneration = sourceGeneration;
			return request;
		}

		RetargetProfileGenerationSnapshot SnapshotFor(
			const RetargetProfileCompileRequest& request) {
			RetargetProfileGenerationSnapshot snapshot;
			snapshot.profileAssetId = request.profile.assetId;
			snapshot.sourceGeneration = request.sourceGeneration;
			snapshot.sourceSkeleton = {request.sourceSkeleton.assetId,
				request.sourceSkeleton.runtimeGeneration};
			snapshot.targetSkeleton = {request.targetSkeleton.assetId,
				request.targetSkeleton.runtimeGeneration};
			snapshot.sourceSkeletonFingerprint =
				request.sourceSkeleton.compatibilityFingerprint;
			snapshot.targetSkeletonFingerprint =
				request.targetSkeleton.compatibilityFingerprint;
			snapshot.sourceReferencePoseHash = request.sourceSkeleton.referencePoseHash;
			snapshot.targetReferencePoseHash = request.targetSkeleton.referencePoseHash;
			return snapshot;
		}
	}

	void RegisterRetargetValidationTests(TestRegistry& registry) {
		registry.Add("Animation.Retarget.ValidationAcceptsExplicitFallbackWithQuantitativeMetrics",
			[](TestContext& context) {
				const SkeletonAsset source = MakeSourceSkeleton();
				const SkeletonAsset target = MakeTargetSkeleton();
				const RetargetProfileAsset profile = MakeProfile(source, target);
				RetargetValidationResult validation;
				RetargetCompiledProfile compiled;
				context.Expect(CompileRetargetProfile(profile, source, target,
						validation, compiled) == AnimationStatus::Ok
					&& validation.publishable
					&& validation.metrics.requiredCoverage == 1.0F
					&& validation.metrics.optionalCoverage == 0.0F
					&& validation.metrics.maximumContactToleranceMeters
						<= kRetargetDefaultContactToleranceMeters
					&& HasCode(validation, RetargetDiagnosticCode::ReducedFidelity)
					&& compiled.mappings.size() == 6U
					&& compiled.chains.size() == 2U
					&& compiled.stableHash != 0U,
					"A fully mapped required rig plus a declared optional fallback should publish with quantitative reduced-fidelity diagnostics.");
			});

		registry.Add("Animation.Retarget.ValidationClassifiesEveryRequiredFailureMode",
			[](TestContext& context) {
				const SkeletonAsset source = MakeSourceSkeleton();
				const SkeletonAsset target = MakeTargetSkeleton();
				auto expectFailure = [&context, &source, &target](
					RetargetProfileAsset profile,
					SkeletonAsset selectedTarget,
					AnimationStatus expectedStatus,
					RetargetDiagnosticCode expectedCode,
					const char* message) {
					RetargetValidationResult validation;
					const AnimationStatus status = ValidateRetargetProfile(
						profile, source, selectedTarget, validation);
					context.Expect(status == expectedStatus
						&& !validation.publishable && HasCode(validation, expectedCode),
						message);
				};

				RetargetProfileAsset missing = MakeProfile(source, target);
				missing.semanticMappings[3].targetSemantic = "MissingRequiredFoot";
				expectFailure(missing, target, AnimationStatus::Rejected,
					RetargetDiagnosticCode::MissingMapping,
					"A missing required semantic must be diagnosed and reject publication.");

				SkeletonAsset ambiguousTarget = target;
				ambiguousTarget.joints.push_back(Joint(109U, 3,
					{0.1F, -0.2F, 0.0F}, "LeftFoot"));
				expectFailure(MakeProfile(source, target), ambiguousTarget,
					AnimationStatus::Rejected, RetargetDiagnosticCode::AmbiguousMapping,
					"A semantic that resolves to two target joints must fail explicitly as ambiguous.");

				SkeletonAsset malformedHierarchy = target;
				malformedHierarchy.joints[3].parentIndex = 999;
				expectFailure(MakeProfile(source, target), malformedHierarchy,
					AnimationStatus::Rejected, RetargetDiagnosticCode::InvalidHierarchy,
					"Malformed parent indices must produce safe hierarchy diagnostics without entering metric collection out of bounds.");

				RetargetProfileAsset cyclic = MakeProfile(source, target);
				cyclic.semanticMappings[0].parentMappingId = 2U;
				cyclic.semanticMappings[1].parentMappingId = 1U;
				expectFailure(cyclic, target, AnimationStatus::Rejected,
					RetargetDiagnosticCode::CyclicMapping,
					"A mapping parent cycle must be bounded and reported deterministically.");

				RetargetProfileAsset duplicate = MakeProfile(source, target);
				duplicate.semanticMappings[1].mappingId = 1U;
				expectFailure(duplicate, target, AnimationStatus::Rejected,
					RetargetDiagnosticCode::DuplicateMapping,
					"Duplicate stable mapping IDs must reject instead of choosing insertion order.");

				SkeletonAsset incompatibleTarget = target;
				++incompatibleTarget.compatibilityFingerprint;
				expectFailure(MakeProfile(source, target), incompatibleTarget,
					AnimationStatus::IncompatibleAsset,
					RetargetDiagnosticCode::IncompatibleSkeleton,
					"A target fingerprint mismatch must fail as an incompatible rig generation.");

				RetargetProfileAsset unsupported = MakeProfile(source, target);
				unsupported.root.motionMode = static_cast<RetargetRootMotionMode>(0xffU);
				expectFailure(unsupported, target, AnimationStatus::Unsupported,
					RetargetDiagnosticCode::UnsupportedMapping,
					"Unknown persisted policy values must fail closed as unsupported.");

				RetargetProfileAsset oversized = MakeProfile(source, target);
				oversized.semanticMappings.resize(kRetargetProfileMaximumMappings + 1U);
				expectFailure(oversized, target, AnimationStatus::HardLimitExceeded,
					RetargetDiagnosticCode::HardLimitExceeded,
					"Oversized mapping tables must stop at the declared hard allocation bound.");
			});

		registry.Add("Animation.Retarget.ValidationDiagnosticsIgnoreAuthoredInsertionOrder",
			[](TestContext& context) {
				const SkeletonAsset source = MakeSourceSkeleton();
				const SkeletonAsset target = MakeTargetSkeleton();
				RetargetProfileAsset forward = MakeProfile(source, target);
				forward.semanticMappings[0].parentMappingId = 2U;
				forward.semanticMappings[1].parentMappingId = 1U;
				forward.semanticMappings[3].targetSemantic = "MissingRequiredFoot";
				RetargetProfileAsset reversed = forward;
				std::reverse(reversed.semanticMappings.begin(), reversed.semanticMappings.end());
				RetargetValidationResult first;
				RetargetValidationResult second;
				context.Expect(ValidateRetargetProfile(forward, source, target, first)
						== AnimationStatus::Rejected
					&& ValidateRetargetProfile(reversed, source, target, second)
						== AnimationStatus::Rejected
					&& first == second,
					"Stable diagnostic code and element ordering must make invalid reports byte-for-byte reproducible.");
			});

		registry.Add("Animation.Retarget.PublisherPreservesLastGoodAndBoundsOldGenerations",
			[](TestContext& context) {
				auto executor = std::make_shared<ManualRetargetExecutor>();
				RetargetProfilePublisher publisher(executor, 2U, 1U);
				RetargetProfileCompileRequest first = MakeRequest(1U);
				context.Expect(publisher.RequestPublication(first) == AnimationStatus::Queued
					&& executor->PendingCount() == 1U
					&& publisher.PollAndPublish(SnapshotFor(first)).status
						== AnimationStatus::NotReady,
					"The protected boundary must poll without waiting for worker validation.");
				executor->RunNext();
				const RetargetProfilePublishPollResult firstPoll =
					publisher.PollAndPublish(SnapshotFor(first));
				const auto firstRecord = publisher.GetPublishedRecord();
				context.Expect(firstPoll.published && firstRecord.has_value()
					&& firstRecord->profileGeneration == 1U
					&& firstRecord->dependentInvalidationGeneration == 1U
					&& firstRecord->compiledProfile != nullptr
					&& firstRecord->compiledProfile->profile.runtimeGeneration == 1U,
					"A complete current worker result should atomically publish one immutable generation and dependent invalidation stamp.");

				RetargetProfileCompileRequest invalid = MakeRequest(2U);
				invalid.profile.semanticMappings[3].targetSemantic = "MissingRequiredFoot";
				context.Expect(publisher.RequestPublication(invalid) == AnimationStatus::Queued,
					"Mapping validation belongs to bounded worker preparation.");
				executor->RunNext();
				context.Expect(publisher.PollAndPublish(SnapshotFor(invalid)).status
						== AnimationStatus::Rejected
					&& publisher.GetPublishedRecord() == firstRecord,
					"An invalid hot reload must preserve the complete last-known-good record.");

				RetargetProfileCompileRequest incompatible = MakeRequest(3U);
				++incompatible.targetSkeleton.compatibilityFingerprint;
				context.Expect(publisher.RequestPublication(incompatible) == AnimationStatus::Queued,
					"Cross-rig compatibility checks should also execute on the worker.");
				executor->RunNext();
				context.Expect(publisher.PollAndPublish(SnapshotFor(incompatible)).status
						== AnimationStatus::IncompatibleAsset
					&& publisher.GetPublishedRecord() == firstRecord,
					"An incompatible target rig must not replace the last-known-good profile.");

				std::vector<std::uint8_t> bytes;
				static_cast<void>(SerializeRetargetProfileAsset(first.profile, bytes));
				bytes.back() ^= 0x55U;
				context.Expect(publisher.RequestSerializedPublication(bytes,
						first.profile.compatibilityFingerprint, first.sourceSkeleton,
						first.targetSkeleton, 4U) == AnimationStatus::ContentHashMismatch
					&& publisher.GetPublishedRecord() == firstRecord,
					"Corrupt serialized reload bytes must fail before enqueue and leave publication untouched.");

				RetargetProfileCompileRequest cancelled = MakeRequest(5U);
				context.Expect(publisher.RequestPublication(cancelled) == AnimationStatus::Queued,
					"A valid reload should enter the bounded pending queue.");
				publisher.CancelPending();
				executor->RunNext();
				context.Expect(publisher.PollAndPublish(SnapshotFor(cancelled)).status
						== AnimationStatus::Cancelled
					&& publisher.GetPublishedRecord() == firstRecord,
					"Cancellation after enqueue must not publish a partial worker result.");

				RetargetProfileCompileRequest second = MakeRequest(6U);
				second.profile.quality.reviewReason = "Compatible hot reload two";
				static_cast<void>(publisher.RequestPublication(second));
				executor->RunNext();
				const RetargetProfilePublishPollResult secondPoll =
					publisher.PollAndPublish(SnapshotFor(second));
				RetargetProfileCompileRequest third = MakeRequest(7U);
				third.profile.quality.reviewReason = "Compatible hot reload three";
				static_cast<void>(publisher.RequestPublication(third));
				executor->RunNext();
				const RetargetProfilePublishPollResult thirdPoll =
					publisher.PollAndPublish(SnapshotFor(third));
				const auto finalRecord = publisher.GetPublishedRecord();
				context.Expect(secondPoll.published && thirdPoll.published
					&& finalRecord.has_value() && finalRecord->profileGeneration == 3U
					&& finalRecord->dependentInvalidationGeneration == 3U
					&& publisher.GetRetiredGenerations().size() == 1U,
					"Each accepted hot reload must invalidate dependents and retain no more old immutable generations than configured.");
				publisher.RetireThrough(2U);
				context.Expect(publisher.GetRetiredGenerations().empty(),
					"The consumer fence callback should explicitly release bounded old generations.");
			});

		registry.Add("Animation.Retarget.PublisherRejectsLateWorkerGenerationRollback",
			[](TestContext& context) {
				auto executor = std::make_shared<ManualRetargetExecutor>();
				RetargetProfilePublisher publisher(executor, 2U, 2U);
				RetargetProfileCompileRequest older = MakeRequest(1U);
				RetargetProfileCompileRequest newer = MakeRequest(2U);
				newer.profile.quality.reviewReason = "Newer compatible profile generation";
				context.Expect(publisher.RequestPublication(older) == AnimationStatus::Queued
					&& publisher.RequestPublication(newer) == AnimationStatus::Queued
					&& publisher.RequestPublication(MakeRequest(3U))
						== AnimationStatus::Rejected,
					"Two immutable generations may validate concurrently, while a third must stop at the configured pending-work bound.");
				executor->RunLast();
				context.Expect(publisher.PollAndPublish(SnapshotFor(newer)).published,
					"The newest completed generation should publish even if an older worker is still running.");
				const auto newestRecord = publisher.GetPublishedRecord();
				executor->RunNext();
				context.Expect(publisher.PollAndPublish(SnapshotFor(newer)).status
						== AnimationStatus::StaleHandle
					&& publisher.GetPublishedRecord() == newestRecord,
					"A late older worker completion must never roll publication back to a mixed generation.");
			});
	}
}
