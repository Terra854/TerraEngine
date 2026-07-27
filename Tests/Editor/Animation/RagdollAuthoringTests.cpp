// RagdollAuthoringTests.cpp - detached editor authoring and CAS publication.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Windows/RagdollEditorWindow.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace TerraEngine::EditorTests {
	using namespace TerraEngine::Animation;
	using namespace TerraEngine::Editor::Animation;

	namespace {
		AnimationGuid Guid(std::uint8_t seed) {
			AnimationGuid guid;
			guid.value[0U] = seed;
			guid.value[15U] = static_cast<std::uint8_t>(seed ^ 0xa5U);
			return guid;
		}

		SkeletonAsset Skeleton() {
			SkeletonAsset skeleton;
			skeleton.assetId = Guid(2U);
			skeleton.runtimeGeneration = 8U;
			skeleton.compatibilityFingerprint = 0x2222U;
			skeleton.referencePoseHash = 0x3333U;
			skeleton.rootJointIndex = 0U;
			for(std::uint64_t index = 0U; index < 3U; ++index) {
				SkeletonJoint joint;
				joint.stableId = 10U + index * 10U;
				joint.parentIndex = index == 0U ? -1 : static_cast<std::int32_t>(index - 1U);
				joint.deformsMesh = true;
				joint.displayName = "Joint " + std::to_string(index);
				joint.localBindTransform.translation = {
					0.0F, index == 0U ? 0.0F : 0.5F, 0.0F};
				skeleton.joints.push_back(joint);
			}
			return skeleton;
		}

		RagdollProfileAsset EmptyProfile(const SkeletonAsset& skeleton) {
			RagdollProfileAsset profile;
			profile.assetId = Guid(1U);
			profile.skeletonAssetId = skeleton.assetId;
			profile.skeletonFingerprint = skeleton.compatibilityFingerprint;
			profile.referencePoseHash = skeleton.referencePoseHash;
			profile.physicsProviderFingerprint = 0x4a4f4c54U;
			// The asset serializer intentionally rejects body-less profiles. This small
			// valid root record is the durable document an artist can open before asking
			// the generator to replace it with full skeleton coverage.
			profile.rootBodyId = skeleton.joints.front().stableId;
			profile.expectedTotalMassKilograms = 1.0F;
			profile.maximumBodyMassFraction = 1.0F;
			profile.allowedCollisionLayerMask = 1U;
			RagdollBodyRecord body;
			body.bodyId = profile.rootBodyId;
			body.stableJointId = skeleton.joints.front().stableId;
			body.collisionLayer = 0U;
			body.collisionMask = 1U;
			RagdollColliderRecord collider;
			collider.colliderId = 1001U;
			collider.shape = RagdollColliderShape::Sphere;
			collider.dimensions = {0.1F, 0.1F, 0.1F};
			body.colliders.push_back(collider);
			profile.bodies.push_back(body);
			return profile;
		}

		std::vector<RagdollCollisionLayerRule> EnabledLayers() {
			return {{0U, 1U, true}};
		}

		RagdollCollisionLayerProvider EnabledLayerProvider() {
			return []() { return EnabledLayers(); };
		}

		std::uint64_t Hash(std::span<const std::uint8_t> bytes) {
			std::uint64_t hash = 1469598103934665603ULL;
			for(const std::uint8_t byte : bytes) {
				hash ^= byte;
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		AnimationGeneration Generation(std::span<const std::uint8_t> bytes) {
			const std::uint64_t hash = Hash(bytes);
			AnimationGeneration result = static_cast<AnimationGeneration>(hash ^ (hash >> 32U));
			return result == 0U ? 1U : result;
		}

		RagdollAuthoringGenerationSnapshot Snapshot(
			const RagdollProfileAsset& profile,
			const SkeletonAsset& skeleton,
			std::span<const std::uint8_t> bytes) {
			RagdollAuthoringGenerationSnapshot snapshot;
			snapshot.profile = {profile.assetId, Generation(bytes)};
			snapshot.skeleton = {skeleton.assetId, skeleton.runtimeGeneration};
			snapshot.skeletonFingerprint = skeleton.compatibilityFingerprint;
			snapshot.referencePoseHash = skeleton.referencePoseHash;
			snapshot.persistedBytesHash = Hash(bytes);
			return snapshot;
		}

		std::vector<std::uint8_t> Bytes(const RagdollProfileAsset& profile) {
			std::vector<std::uint8_t> bytes;
			static_cast<void>(SerializeRagdollProfileAsset(profile, bytes));
			return bytes;
		}

		bool Write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			output.flush();
			return static_cast<bool>(output);
		}

		std::vector<std::uint8_t> Read(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if(!input) return {};
			const std::streamoff size = input.tellg();
			if(size <= 0) return {};
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
			input.seekg(0, std::ios::beg);
			input.read(reinterpret_cast<char*>(bytes.data()), size);
			return input ? bytes : std::vector<std::uint8_t>{};
		}

		struct TemporaryProfile final {
			TemporaryProfile() {
				static std::atomic_uint64_t sequence{1U};
				path = std::filesystem::temp_directory_path()
					/ ("terra-ragdoll-authoring-" + std::to_string(
						std::chrono::steady_clock::now().time_since_epoch().count())
						+ "-" + std::to_string(sequence.fetch_add(1U)) + ".teragdoll");
			}
			~TemporaryProfile() {
				std::error_code error;
				std::filesystem::remove(path, error);
			}
			std::filesystem::path path;
		};

		RagdollRecoveryClipProvider ClipProvider(
			const std::shared_ptr<std::atomic_uint32_t>& generation,
			AnimationCompatibilityFingerprint fingerprint) {
			return [generation, fingerprint](AnimationGuid id)
				-> std::optional<RagdollRecoveryClipDependency> {
				if(id != Guid(9U)) return std::nullopt;
				return RagdollRecoveryClipDependency{id,
					generation->load(std::memory_order_acquire), fingerprint, "Recovery Clip"};
			};
		}

		RagdollEditorModel GeneratedModel(
			const SkeletonAsset& skeleton,
			const RagdollProfileAsset& profile,
			const std::vector<std::uint8_t>& bytes,
			RagdollCollisionLayerProvider layers = EnabledLayerProvider(),
			RagdollRecoveryClipProvider clips = {}) {
			RagdollEditorModel model;
			static_cast<void>(model.Begin(profile, skeleton, Snapshot(profile, skeleton, bytes),
				std::move(layers), std::move(clips)));
			static_cast<void>(model.GenerateFromSkeleton());
			return model;
		}

		RagdollProfileAsset ValidBaseline(const SkeletonAsset& skeleton) {
			const RagdollProfileAsset empty = EmptyProfile(skeleton);
			const std::vector<std::uint8_t> bytes = Bytes(empty);
			return GeneratedModel(skeleton, empty, bytes).Candidate();
		}

		RagdollAuthoringService::SnapshotProvider DiskSnapshotProvider(
			std::filesystem::path path,
			RagdollProfileAsset profile,
			SkeletonAsset skeleton) {
			return [path = std::move(path), profile = std::move(profile),
				skeleton = std::move(skeleton)]()
				-> std::optional<RagdollAuthoringGenerationSnapshot> {
				const std::vector<std::uint8_t> bytes = Read(path);
				if(bytes.empty()) return std::nullopt;
				RagdollProfileAsset decoded;
				if(DeserializeRagdollProfileAsset(bytes, 0U, decoded) != AnimationStatus::Ok
					|| decoded.assetId != profile.assetId) return std::nullopt;
				return Snapshot(profile, skeleton, bytes);
			};
		}

		RagdollAuthoringPublishResult Save(
			const std::filesystem::path& path,
			RagdollEditorModel& model,
			const RagdollProfileAsset& profile,
			const SkeletonAsset& skeleton) {
			RagdollAuthoringCompileRequest request;
			if(model.PrepareCommit(request) != AnimationAuthoringStatus::Ok) return {};
			const RagdollAuthoringCompileProduct product =
				RagdollAuthoringService::CompileCandidate(request);
			return RagdollAuthoringService::CompletePublish(
				path, model, product, DiskSnapshotProvider(path, profile, skeleton));
		}

		AnimationFloat3 Add(AnimationFloat3 left, AnimationFloat3 right) {
			return {left.x + right.x, left.y + right.y, left.z + right.z};
		}

		AnimationQuaternion Normalize(AnimationQuaternion value) {
			const float length = std::sqrt(value.x * value.x + value.y * value.y
				+ value.z * value.z + value.w * value.w);
			return {value.x / length, value.y / length, value.z / length, value.w / length};
		}

		AnimationQuaternion Multiply(AnimationQuaternion left, AnimationQuaternion right) {
			return Normalize({
				left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
				left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
				left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
				left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z});
		}

		AnimationFloat3 Rotate(AnimationQuaternion rotation, AnimationFloat3 value) {
			rotation = Normalize(rotation);
			const AnimationFloat3 axis{rotation.x, rotation.y, rotation.z};
			const AnimationFloat3 twiceCross{
				2.0F * (axis.y * value.z - axis.z * value.y),
				2.0F * (axis.z * value.x - axis.x * value.z),
				2.0F * (axis.x * value.y - axis.y * value.x)};
			return Add(value, Add({rotation.w * twiceCross.x, rotation.w * twiceCross.y,
				rotation.w * twiceCross.z}, {
				axis.y * twiceCross.z - axis.z * twiceCross.y,
				axis.z * twiceCross.x - axis.x * twiceCross.z,
				axis.x * twiceCross.y - axis.y * twiceCross.x}));
		}

		AnimationTransform Compose(const AnimationTransform& parent, const AnimationTransform& local) {
			AnimationTransform result;
			result.translation = Add(parent.translation, Rotate(parent.rotation, local.translation));
			result.rotation = Multiply(parent.rotation, local.rotation);
			return result;
		}

		bool Near(AnimationFloat3 left, AnimationFloat3 right, float tolerance = 0.0001F) {
			return std::abs(left.x - right.x) <= tolerance
				&& std::abs(left.y - right.y) <= tolerance
				&& std::abs(left.z - right.z) <= tolerance;
		}
	}

	void RegisterRagdollAuthoringTests(TestRegistry& registry) {
		registry.Add("Editor.Animation.RagdollAuthoring.DeterministicSkeletonGenerationAndFrames",
			[](TestContext& context) {
				const SkeletonAsset skeleton = Skeleton();
				const RagdollProfileAsset profile = EmptyProfile(skeleton);
				const std::vector<std::uint8_t> bytes = Bytes(profile);
				RagdollEditorModel first = GeneratedModel(skeleton, profile, bytes);
				RagdollEditorModel second = GeneratedModel(skeleton, profile, bytes);
				bool matchingAnchors = true;
				std::vector<AnimationTransform> jointPoses(skeleton.joints.size());
				for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
					jointPoses[index] = skeleton.joints[index].parentIndex < 0
						? skeleton.joints[index].localBindTransform
						: Compose(jointPoses[static_cast<std::size_t>(skeleton.joints[index].parentIndex)],
							skeleton.joints[index].localBindTransform);
				}
				for(const RagdollConstraintRecord& constraint : first.Candidate().constraints) {
					const auto parent = std::find_if(first.Candidate().bodies.begin(),
						first.Candidate().bodies.end(), [&](const auto& body) {
							return body.bodyId == constraint.parentBodyId;
						});
					const auto child = std::find_if(first.Candidate().bodies.begin(),
						first.Candidate().bodies.end(), [&](const auto& body) {
							return body.bodyId == constraint.childBodyId;
						});
					const auto parentJoint = std::find_if(skeleton.joints.begin(), skeleton.joints.end(),
						[&](const auto& joint) { return joint.stableId == parent->stableJointId; });
					const auto childJoint = std::find_if(skeleton.joints.begin(), skeleton.joints.end(),
						[&](const auto& joint) { return joint.stableId == child->stableJointId; });
					const AnimationTransform parentPose = jointPoses[
						static_cast<std::size_t>(parentJoint - skeleton.joints.begin())];
					const AnimationTransform childPose = jointPoses[
						static_cast<std::size_t>(childJoint - skeleton.joints.begin())];
					const AnimationFloat3 parentAnchor = Add(parentPose.translation,
						Rotate(parentPose.rotation, constraint.parentFrame.localPosition));
					const AnimationFloat3 childAnchor = Add(childPose.translation,
						Rotate(childPose.rotation, constraint.childFrame.localPosition));
					matchingAnchors = matchingAnchors && Near(parentAnchor, childAnchor)
						&& Near(childAnchor, childPose.translation);
				}
				context.Expect(first.Candidate() == second.Candidate()
					&& first.Candidate().bodies.size() == skeleton.joints.size()
					&& first.Candidate().constraints.size() + 1U == skeleton.joints.size()
					&& first.Validation().IsValid() && matchingAnchors,
					"Generation must be deterministic and constraint frames must share one bind-pose world anchor.");
			});

		registry.Add("Editor.Animation.RagdollAuthoring.ProjectLayerPolicyRejectsCandidate",
			[](TestContext& context) {
				const SkeletonAsset skeleton = Skeleton();
				const RagdollProfileAsset profile = EmptyProfile(skeleton);
				const std::vector<std::uint8_t> bytes = Bytes(profile);
				RagdollEditorModel model = GeneratedModel(skeleton, profile, bytes,
					[]() { return std::vector<RagdollCollisionLayerRule>{{0U, 0U, false}}; });
				const bool hasLayerError = std::any_of(model.Validation().diagnostics.begin(),
					model.Validation().diagnostics.end(), [](const RagdollProfileDiagnostic& diagnostic) {
						return diagnostic.code == RagdollProfileDiagnosticCode::CollisionLayerDisabled
							|| diagnostic.code == RagdollProfileDiagnosticCode::CollisionMaskOutsideLayerPolicy;
					});
				context.Expect(!model.Validation().IsValid() && hasLayerError,
					"Ragdoll validation must use injected project collision policy, including disabled layers.");
			});

		registry.Add("Editor.Animation.RagdollAuthoring.RecoveryIdentityFiniteInputsAndToggle",
			[](TestContext& context) {
				const SkeletonAsset skeleton = Skeleton();
				const RagdollProfileAsset profile = EmptyProfile(skeleton);
				const std::vector<std::uint8_t> bytes = Bytes(profile);
				const auto clipGeneration = std::make_shared<std::atomic_uint32_t>(2U);
				RagdollEditorModel model = GeneratedModel(skeleton, profile, bytes,
					EnabledLayerProvider(), ClipProvider(clipGeneration, skeleton.compatibilityFingerprint));
				RagdollRecoveryRecord recovery;
				recovery.candidateId = 77U;
				recovery.clipAssetId = Guid(9U);
				recovery.clipGeneration = 2U;
				recovery.clipSkeletonFingerprint = skeleton.compatibilityFingerprint;
				recovery.requiredReferencePoseHash = skeleton.referencePoseHash;
				recovery.rootJointStableId = skeleton.joints.front().stableId;
				static_cast<void>(model.ReplaceRecovery(recovery));
				const AnimationStatus currentIdentity = model.Validate();
				clipGeneration->store(3U, std::memory_order_release);
				const AnimationStatus staleIdentity = model.Validate();
				const bool refreshed = model.SelectRecoveryClip(77U, Guid(9U));
				RagdollRecoveryRecord unresolved = model.Candidate().recovery.front();
				unresolved.clipAssetId = profile.assetId;
				unresolved.clipGeneration = 1U;
				static_cast<void>(model.ReplaceRecovery(unresolved));
				const AnimationStatus missingIdentity = model.Validate();
				const bool restored = model.SelectRecoveryClip(77U, Guid(9U));
				RagdollRecoveryTestInput nonfinite;
				nonfinite.poseErrorMeters = std::numeric_limits<float>::quiet_NaN();
				const RagdollRecoveryTestResult rejected = model.TestRecovery(77U, nonfinite);
				model.SetActivationPreview(0.25F, true);
				const float activationWeight = model.ActivationPreview().joints.front().physicsWeight;
				model.SetActivationPreview(0.25F, false);
				const float recoveryWeight = model.ActivationPreview().joints.front().physicsWeight;
				context.Expect(currentIdentity == AnimationStatus::Ok
					&& staleIdentity == AnimationStatus::StaleHandle && refreshed
					&& missingIdentity == AnimationStatus::MissingAsset && restored
					&& model.Candidate().recovery.front().clipGeneration == 3U
					&& !rejected.eligible && !rejected.failures.empty()
					&& std::abs(activationWeight - 0.25F) < 0.0001F
					&& std::abs(recoveryWeight - 0.75F) < 0.0001F
					&& !model.ActivationPreview().activating,
					"Recovery identity must re-resolve exactly, reject nonfinite tests, and update immediately on mode toggles.");
			});

		registry.Add("Editor.Animation.RagdollAuthoring.GeometryAndActivationPosePackets",
			[](TestContext& context) {
				const SkeletonAsset skeleton = Skeleton();
				const RagdollProfileAsset profile = EmptyProfile(skeleton);
				const std::vector<std::uint8_t> bytes = Bytes(profile);
				RagdollEditorModel model = GeneratedModel(skeleton, profile, bytes);
				RagdollBodyRecord body = model.Candidate().bodies.front();
				body.jointToBodyOffset = {0.25F, 0.0F, 0.0F};
				body.colliders.front().shape = RagdollColliderShape::Box;
				body.colliders.front().localCenter = {1.0F, 0.0F, 0.0F};
				body.colliders.front().dimensions = {0.2F, 0.3F, 0.4F};
				static_cast<void>(model.ReplaceBody(body));
				model.SelectCollider(body.colliders.front().colliderId);
				const auto entry = std::find_if(model.CollisionVisualization().begin(),
					model.CollisionVisualization().end(), [&](const auto& current) {
						return current.colliderId == body.colliders.front().colliderId;
					});
				const bool finiteLines = std::all_of(model.CollisionGeometry().begin(),
					model.CollisionGeometry().end(), [](const RagdollVisualizationLine& line) {
						return IsFinite(line.start) && IsFinite(line.end);
					});
				const std::size_t selectedColliderLines = static_cast<std::size_t>(std::count_if(
					model.CollisionGeometry().begin(), model.CollisionGeometry().end(),
					[&](const RagdollVisualizationLine& line) {
						return line.colliderId == body.colliders.front().colliderId;
					}));
				context.Expect(entry != model.CollisionVisualization().end()
					&& entry->selected && std::abs(entry->colliderPose.translation.x - 1.25F) < 0.0001F
					&& selectedColliderLines == 12U && finiteLines
					&& model.ActivationPreview().joints.size() == skeleton.joints.size()
					&& model.ActivationPreview().poseLines.size() + 1U == skeleton.joints.size()
					&& std::abs(model.ActivationPreview().joints.front().bodyPose.translation.x - 0.25F) < 0.0001F,
					"Preview packets must contain transformed collider wire geometry and activation body poses.");
			});

		registry.Add("Editor.Animation.RagdollAuthoring.CancelUndoRedoAndStalePublication",
			[](TestContext& context) {
				const SkeletonAsset skeleton = Skeleton();
				const RagdollProfileAsset baseline = ValidBaseline(skeleton);
				const std::vector<std::uint8_t> original = Bytes(baseline);
				TemporaryProfile file;
				static_cast<void>(Write(file.path, original));
				RagdollEditorModel model;
				static_cast<void>(model.Begin(baseline, skeleton, Snapshot(baseline, skeleton, original),
					EnabledLayerProvider()));
				RagdollBodyRecord edit = model.Candidate().bodies.front();
				edit.linearDamping = 0.25F;
				static_cast<void>(model.ReplaceBody(edit));
				RagdollAuthoringCompileRequest canceledRequest;
				const bool prepared = model.PrepareCommit(canceledRequest) == AnimationAuthoringStatus::Ok;
				const bool canceled = model.Cancel() == AnimationAuthoringStatus::Canceled;
				const RagdollAuthoringCompileProduct canceledProduct =
					RagdollAuthoringService::CompileCandidate(canceledRequest);

				edit = model.Candidate().bodies.front();
				edit.linearDamping = 0.35F;
				static_cast<void>(model.ReplaceBody(edit));
				const RagdollAuthoringPublishResult saved = Save(file.path, model, baseline, skeleton);
				const std::vector<std::uint8_t> editedBytes = Read(file.path);
				const auto snapshots = DiskSnapshotProvider(file.path, baseline, skeleton);
				const RagdollAuthoringPublishResult undone =
					RagdollAuthoringService::Undo(file.path, model, snapshots);
				const bool restoredOriginal = Read(file.path) == original;
				const RagdollAuthoringPublishResult redone =
					RagdollAuthoringService::Redo(file.path, model, snapshots);
				const bool restoredEdit = Read(file.path) == editedBytes;

				edit = model.Candidate().bodies.front();
				edit.linearDamping = 0.45F;
				static_cast<void>(model.ReplaceBody(edit));
				RagdollAuthoringCompileRequest staleRequest;
				static_cast<void>(model.PrepareCommit(staleRequest));
				const RagdollAuthoringCompileProduct staleProduct =
					RagdollAuthoringService::CompileCandidate(staleRequest);
				static_cast<void>(Write(file.path, original));
				const RagdollAuthoringPublishResult stale = RagdollAuthoringService::CompletePublish(
					file.path, model, staleProduct, snapshots);
				context.Expect(prepared && canceled && canceledProduct.authoring.canceled
					&& Read(file.path) == original && saved.published
					&& undone.published && restoredOriginal && redone.published && restoredEdit
					&& !stale.published && stale.status == AnimationStatus::StaleHandle,
					"Cancel, grouped history, and stale completion must preserve whole durable revisions.");
			});

		registry.Add("Editor.Animation.RagdollAuthoring.ConcurrentPublicationHasSingleWinner",
			[](TestContext& context) {
				const SkeletonAsset skeleton = Skeleton();
				const RagdollProfileAsset baseline = ValidBaseline(skeleton);
				const std::vector<std::uint8_t> original = Bytes(baseline);
				TemporaryProfile file;
				static_cast<void>(Write(file.path, original));
				RagdollEditorModel first;
				RagdollEditorModel second;
				static_cast<void>(first.Begin(baseline, skeleton, Snapshot(baseline, skeleton, original),
					EnabledLayerProvider()));
				static_cast<void>(second.Begin(baseline, skeleton, Snapshot(baseline, skeleton, original),
					EnabledLayerProvider()));
				RagdollBodyRecord firstBody = first.Candidate().bodies.front();
				RagdollBodyRecord secondBody = second.Candidate().bodies.front();
				firstBody.linearDamping = 0.2F;
				secondBody.linearDamping = 0.3F;
				static_cast<void>(first.ReplaceBody(firstBody));
				static_cast<void>(second.ReplaceBody(secondBody));
				RagdollAuthoringCompileRequest firstRequest;
				RagdollAuthoringCompileRequest secondRequest;
				static_cast<void>(first.PrepareCommit(firstRequest));
				static_cast<void>(second.PrepareCommit(secondRequest));
				const RagdollAuthoringCompileProduct firstProduct =
					RagdollAuthoringService::CompileCandidate(firstRequest);
				const RagdollAuthoringCompileProduct secondProduct =
					RagdollAuthoringService::CompileCandidate(secondRequest);
				const auto snapshots = DiskSnapshotProvider(file.path, baseline, skeleton);
				RagdollAuthoringPublishResult firstResult;
				RagdollAuthoringPublishResult secondResult;
				std::thread firstPublisher([&]() {
					firstResult = RagdollAuthoringService::CompletePublish(
						file.path, first, firstProduct, snapshots);
				});
				std::thread secondPublisher([&]() {
					secondResult = RagdollAuthoringService::CompletePublish(
						file.path, second, secondProduct, snapshots);
				});
				firstPublisher.join();
				secondPublisher.join();
				RagdollProfileAsset decoded;
				const std::vector<std::uint8_t> published = Read(file.path);
				const bool decodedWinner = DeserializeRagdollProfileAsset(published,
					skeleton.compatibilityFingerprint, decoded) == AnimationStatus::Ok
					&& (std::abs(decoded.bodies.front().linearDamping - 0.2F) < 0.0001F
						|| std::abs(decoded.bodies.front().linearDamping - 0.3F) < 0.0001F);
				context.Expect(firstResult.published != secondResult.published
					&& decodedWinner && published != original,
					"The per-destination snapshot-and-replace boundary must allow exactly one concurrent winner.");
			});

		registry.Add("Editor.Animation.RagdollAuthoring.CheckedSkeletonBindingGeneratesAndPreservesLastGood",
			[](TestContext& context) {
				const SkeletonAsset sourceSkeleton = Skeleton();
				SkeletonAsset foxPackageSkeleton = sourceSkeleton;
				foxPackageSkeleton.assetId = Guid(8U);
				foxPackageSkeleton.runtimeGeneration = 23U;
				foxPackageSkeleton.compatibilityFingerprint = 0x8888U;
				foxPackageSkeleton.referencePoseHash = 0x9999U;
				const RagdollProfileAsset baseline = ValidBaseline(sourceSkeleton);
				const std::vector<std::uint8_t> original = Bytes(baseline);
				TemporaryProfile file;
				static_cast<void>(Write(file.path, original));

				RagdollEditorModel model;
				static_cast<void>(model.Begin(baseline, sourceSkeleton,
					Snapshot(baseline, sourceSkeleton, original), EnabledLayerProvider()));
				SkeletonAsset invalidSkeleton;
				const bool rejectedInvalid = !model.RebindSkeleton(invalidSkeleton,
					Snapshot(baseline, sourceSkeleton, original));
				const bool preservedAfterInvalid = Read(file.path) == original;
				const bool boundPackageChild = model.RebindSkeleton(foxPackageSkeleton,
					Snapshot(baseline, foxPackageSkeleton, original));
				const AnimationStatus generated = model.GenerateFromSkeleton();
				const RagdollAuthoringPublishResult saved = Save(file.path, model,
					baseline, foxPackageSkeleton);
				RagdollProfileAsset reopened;
				const std::vector<std::uint8_t> persisted = Read(file.path);
				const bool reopenedBoundProfile = DeserializeRagdollProfileAsset(persisted,
					foxPackageSkeleton.compatibilityFingerprint, reopened) == AnimationStatus::Ok
					&& reopened.skeletonAssetId == foxPackageSkeleton.assetId
					&& reopened.skeletonFingerprint == foxPackageSkeleton.compatibilityFingerprint
					&& reopened.referencePoseHash == foxPackageSkeleton.referencePoseHash
					&& !reopened.bodies.empty() && !reopened.blends.empty();
				context.Expect(rejectedInvalid && preservedAfterInvalid && boundPackageChild
					&& generated == AnimationStatus::Ok && saved.published && reopenedBoundProfile,
					"A checked generated-package skeleton binding must remain detached until generated data validates, then publish atomically with body and blend data.");
			});
	}
}
