// Real public ragdoll admission rejects malformed values before state changes.
#include "TestHarness.h"
#include "Tests/Support/ProcessCommandLine.h"
#include "Animation/Ragdoll/PhysicsPoseSnapshot.h"
#include "Animation/Ragdoll/RagdollActivation.h"
#include "Animation/Ragdoll/RagdollState.h"
#include "Animation/Ragdoll/RagdollProfileValidator.h"
#include "Utilities/OwnedProcess.h"
#include "Utilities/PlatformUtils.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <functional>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		using namespace Animation;
		RagdollGenerationStamp Generation() { return {1U, 2U, 3U, 4U, 5}; }
		PhysicsPoseSnapshot Pose(bool duplicate = false) {
			return {Generation(), {{1U, 5U, {}}, {2U, 7U, {}}, {3U, duplicate ? 5U : 9U, {}}}, true};
		}
		AnimationGuid Guid(std::uint8_t seed) { AnimationGuid result; result.value[0] = seed; return result; }
		SkeletonAsset Skeleton() {
			SkeletonAsset result; result.assetId = Guid(2U); result.compatibilityFingerprint = 22U;
			result.referencePoseHash = 33U; result.rootJointIndex = 0U;
			for(std::uint32_t index = 0U; index < 3U; ++index) {
				SkeletonJoint joint; joint.stableId = 5U + index * 2U;
				joint.parentIndex = static_cast<std::int32_t>(index) - 1;
				joint.displayName = "joint" + std::to_string(index); joint.normalizedSourcePath = joint.displayName;
				result.joints.push_back(std::move(joint));
			}
			result.inverseBindMatrices.resize(3U); return result;
		}
		RagdollProfileAsset Profile(const SkeletonAsset& skeleton) {
			RagdollProfileAsset result; result.assetId = Guid(1U); result.runtimeGeneration = 2U;
			result.skeletonAssetId = skeleton.assetId; result.skeletonFingerprint = skeleton.compatibilityFingerprint;
			result.referencePoseHash = skeleton.referencePoseHash; result.physicsProviderFingerprint = 44U;
			result.rootBodyId = 1U; result.allowedCollisionLayerMask = 1U;
			for(std::uint32_t index = 0U; index < 2U; ++index) {
				RagdollBodyRecord body; body.bodyId = index + 1U; body.stableJointId = 5U + index * 2U;
				body.massKilograms = 1.0F; body.collisionMask = 1U;
				RagdollColliderRecord collider; collider.colliderId = 10U + index;
				collider.shape = RagdollColliderShape::Sphere; collider.dimensions = {0.25F, 0.25F, 0.25F};
				body.colliders.push_back(collider); result.bodies.push_back(std::move(body));
			}
			RagdollConstraintRecord constraint; constraint.constraintId = 3U;
			constraint.parentBodyId = 1U; constraint.childBodyId = 2U; result.constraints.push_back(constraint);
			return result;
		}
		RagdollProfileValidationReport Validate(const RagdollProfileAsset& profile, const SkeletonAsset& skeleton) {
			const std::array<RagdollCollisionLayerRule, 1U> layers{{{0U, 1U, true}}};
			RagdollProfileValidationOptions options; options.collisionLayers = layers;
			return ValidateRagdollProfile(profile, skeleton, options);
		}
		bool InCpuChild(std::string_view name) {
			const char* child = std::getenv("TERRAENGINE_RAGDOLL_REVIEW_CHILD");
			return child && name == child;
		}
		void InBoundedChild(TestContext& context, const std::string& name, const std::function<void()>& body) {
			if(InCpuChild(name)) { body(); return; }
			const auto executable = PlatformUtils::GetExecutablePath();
			Utilities::OwnedProcessRequest request; request.workingDirectory = executable.parent_path();
			request.commandLine = QuoteProcessPath(executable); request.timeout = std::chrono::seconds(10);
			request.environment = {{"TERRAENGINE_RAGDOLL_REVIEW_CHILD", name}, {"TERRAENGINE_TEST_FILTER", name}, {"TERRAENGINE_TEST_EXCLUDE_FILTER", ""}};
			const std::atomic_bool cancellation{false}; const auto child = Utilities::OwnedProcess::Run(request, cancellation);
			// A bounded timeout is a RED outcome; it is never accepted as rejection.
			context.Expect(child.started && !child.timedOut && child.exitCode == 0,
				"Public ancestry validation must return rejected in its owned child. TimedOut=" + std::to_string(child.timedOut) + " Output=" + child.output);
		}
	}
	void RegisterRagdollMalformedStateTests(TestRegistry& registry) {
		registry.Add("Animation.Review.Ragdoll.NonadjacentJointDuplicatesRejectBeforeActivation", [](TestContext& context) {
			const auto valid = Pose(); context.Expect(ValidatePhysicsPoseSnapshot(valid, Generation()) == AnimationStatus::Ok, "Unique bodies and joints are the positive control.");
			const auto duplicate = Pose(true);
			const auto basic = ValidatePhysicsPoseSnapshot(duplicate, Generation());
			const std::array<PhysicsPoseExpectedBodyGeneration, 3U> expected{{{1U, 1U}, {2U, 1U}, {3U, 1U}}};
			const auto exact = ValidatePhysicsPoseSnapshot(duplicate, Generation(), expected);
			RagdollActivationPlan output; output.generation = {9U, 9U, 9U, 9U, 9};
			const auto activation = PrepareRagdollActivation(duplicate, Generation(), output);
			context.Expect(basic != AnimationStatus::Ok && exact != AnimationStatus::Ok && activation != AnimationStatus::Ok
				&& output.generation.worldGeneration == 9U, "J5,J7,J5 must reject through both real validators and activation without replacing prior output.");
		});
		registry.Add("Animation.Review.Ragdoll.DuplicatePublicationPreservesHistory", [](TestContext& context) {
			RagdollState state; RagdollStateSnapshot prior; prior.lifecycle = RagdollLifecycleState::Active;
			prior.generation = Generation(); prior.physicsPose = Pose(); prior.blendWeights = {1.0F, 1.0F, 1.0F};
			context.Expect(state.Publish(prior) == AnimationStatus::Ok, "The unique complete state must publish first.");
			const auto oldHash = state.Current().deterministicHash; const auto oldHistory = state.HistorySize();
			auto bad = prior; bad.generation.fixedStep++; bad.physicsPose = Pose(true); bad.physicsPose.generation = bad.generation;
			const auto status = state.Publish(bad);
			context.Expect(status != AnimationStatus::Ok && state.Current().deterministicHash == oldHash && state.HistorySize() == oldHistory,
				"Duplicate joint rejection must preserve the actual last-good state and rollback history.");
		});
		registry.Add("Animation.Review.Ragdoll.MalformedAncestryRejectsBoundedly", [](TestContext& context) {
			const std::string name = "Animation.Review.Ragdoll.MalformedAncestryRejectsBoundedly";
			InBoundedChild(context, name, [&] {
				auto skeleton = Skeleton(); const auto profile = Profile(skeleton);
				context.Expect(Validate(profile, skeleton).IsValid(), "The real acyclic profile fixture must pass before changing ancestry.");
				// The sought root0 is outside child1->joint2->child1. All indices
				// remain within allocation; this targets the public ancestry walk only.
				skeleton.joints[1U].parentIndex = 2; skeleton.joints[2U].parentIndex = 1;
				std::cout << "ragdoll-public-cycle-entered\n" << std::flush;
				const auto result = Validate(profile, skeleton);
				context.Expect(!result.IsValid() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& row) {
					return row.code == RagdollProfileDiagnosticCode::ConstraintSkeletonOrder;
				}), "A malformed parent cycle must terminate with the real incompatible ancestry diagnostic.");
			});
		});
		for(std::uint32_t value = 0U; value <= static_cast<std::uint32_t>(RagdollLifecycleState::Rejected); ++value) {
			registry.Add("Animation.Review.Ragdoll.LifecycleWeightsRejectAtomically." + std::to_string(value), [value](TestContext& context) {
				for(const std::size_t badCount : {1U, 4U}) {
					RagdollState state; RagdollStateSnapshot prior; prior.lifecycle = static_cast<RagdollLifecycleState>(value);
					prior.generation = Generation(); prior.physicsPose = Pose(); prior.blendWeights = {1.0F, 0.5F, 0.25F};
					context.Expect(state.Publish(prior) == AnimationStatus::Ok, "Matching complete weights must be accepted in this lifecycle.");
					const auto oldHash = state.Current().deterministicHash; const auto oldHistory = state.HistorySize();
					auto bad = prior; bad.generation.fixedStep++; bad.physicsPose.generation = bad.generation; bad.blendWeights.assign(badCount, 0.5F);
					const auto status = state.Publish(bad);
					// Never index or capture the unsafe pre-fix short vector. Incorrect
					// admission is an ordinary assertion failure at this safe boundary.
					context.Expect(status != AnimationStatus::Ok, "Every lifecycle must reject nonempty weight/body dimension mismatch before rollback indexing.");
					context.Expect(state.Current().deterministicHash == oldHash && state.HistorySize() == oldHistory, "Rejected dimensions must not mutate current state or history.");
					std::vector<AnimationRagdollRollbackState> records;
					context.Expect(state.CaptureRollback(99U, records) == AnimationStatus::Ok && records.size() == 3U, "Last-good rollback capture must remain safe and complete after rejection.");
				}
			});
		}
		registry.Add("Animation.Review.Ragdoll.EmptyWeightsRemainImplicitFullBlend", [](TestContext& context) {
			for(std::uint32_t value = 0U; value <= static_cast<std::uint32_t>(RagdollLifecycleState::Rejected); ++value) {
				RagdollState state; RagdollStateSnapshot good; good.lifecycle = static_cast<RagdollLifecycleState>(value);
				good.generation = Generation(); good.physicsPose = Pose();
				context.Expect(state.Publish(good) == AnimationStatus::Ok, "Empty weights are the supported implicit-full-blend representation.");
				std::vector<AnimationRagdollRollbackState> records;
				context.Expect(state.CaptureRollback(99U, records) == AnimationStatus::Ok && records.size() == 3U
					&& std::all_of(records.begin(), records.end(), [](const auto& row) { return row.blendWeightBits == std::bit_cast<std::uint32_t>(1.0F); }), "Valid implicit weights must capture every actual body safely.");
			}
		});
	}
}
