// AnimationWindowTests.cpp - production registration and detached handle checks.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Animation/AnimationSceneHandles.h"
#include "Editor/TerraEngine-Editor/Windows/AnimatorInspectorMetadata.h"
#include "Editor/TerraEngine-Editor/Windows/RetargetEditorWindow.h"
#include "Engine/Animation/Assets/RetargetProfileAsset.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace TerraEngine::EditorTests {
	using namespace TerraEngine::Editor::Animation;
	using namespace TerraEngine::Animation;

	namespace {
		AnimationGuid MakeAnimationGuid(std::uint8_t seed) {
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		SkeletonAsset MakeRetargetSkeleton(
			std::uint8_t guidSeed,
			AnimationGeneration generation,
			std::uint64_t fingerprint,
			std::uint64_t poseHash,
			bool includeCorrectionJoint = false) {
			SkeletonAsset skeleton;
			skeleton.assetId = MakeAnimationGuid(guidSeed);
			skeleton.runtimeGeneration = generation;
			skeleton.compatibilityFingerprint = fingerprint;
			skeleton.referencePoseHash = poseHash;
			skeleton.rootJointIndex = 0U;
			SkeletonJoint root;
			root.stableId = 100U + guidSeed;
			root.parentIndex = -1;
			root.displayName = "Root";
			root.semanticRole = "hips";
			skeleton.joints.push_back(root);
			SkeletonJoint hand;
			hand.stableId = 200U + guidSeed;
			hand.parentIndex = 0;
			hand.localBindTransform.translation = {0.5F, 1.0F, 0.0F};
			hand.displayName = "Hand";
			hand.semanticRole = "right_hand";
			skeleton.joints.push_back(hand);
			if(includeCorrectionJoint) {
				SkeletonJoint correction = hand;
				correction.stableId = 300U + guidSeed;
				correction.localBindTransform.translation = {0.65F, 0.9F, 0.0F};
				correction.displayName = "Hand correction";
				correction.semanticRole = "right_hand_correction";
				skeleton.joints.push_back(correction);
			}
			return skeleton;
		}

		RetargetProfileAsset MakeRetargetProfile(
			const SkeletonAsset& source,
			const SkeletonAsset& target) {
			RetargetProfileAsset profile;
			profile.assetId = MakeAnimationGuid(90U);
			profile.compatibilityFingerprint = 0xabcddcbaU;
			profile.sourceSkeletonAssetId = source.assetId;
			profile.targetSkeletonAssetId = target.assetId;
			profile.sourceSkeletonFingerprint = source.compatibilityFingerprint;
			profile.targetSkeletonFingerprint = target.compatibilityFingerprint;
			profile.sourceReferencePoseHash = source.referencePoseHash;
			profile.targetReferencePoseHash = target.referencePoseHash;
			profile.root.sourceRootJointId = source.joints.front().stableId;
			profile.root.targetRootJointId = target.joints.front().stableId;
			profile.proportion.sourceReferenceHeightMeters = 1.0F;
			profile.proportion.targetReferenceHeightMeters = 1.0F;
			return profile;
		}

		std::uint64_t HashRetargetBytes(std::span<const std::uint8_t> bytes) {
			std::uint64_t hash = 1469598103934665603ULL;
			for(const std::uint8_t byte : bytes) {
				hash ^= byte;
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		std::string RetargetBytesString(std::span<const std::uint8_t> bytes) {
			return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
		}

		AnimationAuthoringRevision MakeRetargetRevision(
			const RetargetProfileAsset& profile,
			std::uint64_t generation,
			std::span<const std::uint8_t> bytes) {
			return {profile.assetId, generation, RetargetBytesString(bytes)};
		}

		RetargetAuthoringGenerationSnapshot MakeRetargetSnapshot(
			const RetargetProfileAsset& profile,
			const SkeletonAsset& source,
			const SkeletonAsset& target,
			std::span<const std::uint8_t> persistedBytes) {
			RetargetAuthoringGenerationSnapshot snapshot;
			snapshot.profile = {profile.assetId, 7U};
			snapshot.sourceSkeleton = {source.assetId, source.runtimeGeneration};
			snapshot.targetSkeleton = {target.assetId, target.runtimeGeneration};
			snapshot.sourceFingerprint = source.compatibilityFingerprint;
			snapshot.targetFingerprint = target.compatibilityFingerprint;
			snapshot.sourceReferencePoseHash = source.referencePoseHash;
			snapshot.targetReferencePoseHash = target.referencePoseHash;
			snapshot.persistedBytesHash = HashRetargetBytes(persistedBytes);
			return snapshot;
		}

		bool WriteRetargetBytes(
			const std::filesystem::path& path,
			std::span<const std::uint8_t> bytes) {
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			return static_cast<bool>(output);
		}

		std::vector<std::uint8_t> ReadRetargetBytes(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if(!input) return {};
			const std::streamoff size = input.tellg();
			if(size <= 0) return {};
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
			input.seekg(0, std::ios::beg);
			input.read(reinterpret_cast<char*>(bytes.data()), size);
			return input ? bytes : std::vector<std::uint8_t>{};
		}

		struct RetargetTestFile final {
			RetargetTestFile() {
				const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
				path = std::filesystem::temp_directory_path()
					/ ("terra-retarget-authoring-" + std::to_string(stamp) + ".teretarget");
			}
			~RetargetTestFile() {
				std::error_code error;
				std::filesystem::remove(path, error);
				for(const std::filesystem::directory_entry& entry :
					std::filesystem::directory_iterator(path.parent_path(), error)) {
					if(entry.path().filename().string().starts_with(path.filename().string()
						+ ".retarget-")) {
						std::filesystem::remove(entry.path(), error);
					}
				}
			}
			std::filesystem::path path;
		};
	}

	void RegisterAnimationWindowTests(TestRegistry& registry) {
		registry.Add("Editor.Animation.Inspector.AnimatorPropertyGuidanceIsComplete", [](TestContext& context) {
			using TerraEngine::Editor::Animation::AnimatorInspectorControlKind;
			using TerraEngine::Editor::Animation::FindAnimatorInspectorProperty;

			// Every durable reflected Animator field must have a creator-facing
			// name and practical instructions. Adding a raw property to reflection
			// without extending this list deliberately fails the editor test.
			constexpr std::array<std::string_view, 19U> reflectedProperties{
				"schemaVersion",
				"controllerAssetId",
				"defaultClipAssetId",
				"loopDefaultClip",
				"playAutomatically",
				"defaultClipStartFrame",
				"skeletonOverrideAssetId",
				"avatarOverrideAssetId",
				"retargetProfileOverrideAssetId",
				"ragdollProfileOverrideAssetId",
				"motionDatabaseOverrideAssetId",
				"requestedMode",
				"rootMotionPolicy",
				"updatePolicy",
				"timePolicy",
				"initialParameters",
				"eventPolicy",
				"cullingPolicy",
				"diagnosticOverrides"
			};
			for(const std::string_view stableName : reflectedProperties) {
				const auto* metadata = FindAnimatorInspectorProperty(stableName);
				context.Expect(metadata != nullptr,
					"Every reflected Animator field must have Inspector metadata.");
				if(metadata == nullptr) continue;
				context.Expect(metadata->label != stableName
					&& metadata->label.find(' ') != std::string_view::npos,
					"Animator fields must use readable labels instead of reflection variable names.");
				context.Expect(metadata->explanation.size() >= 24U
					&& metadata->usage.size() >= 32U,
					"Animator guidance must explain both purpose and practical usage.");
				if(metadata->control == AnimatorInspectorControlKind::Choice) {
					context.Expect(metadata->choices.size() >= 3U,
						"Every Animator policy must expose named choices instead of a numeric editor.");
					for(const auto& choice : metadata->choices) {
						context.Expect(!choice.label.empty() && choice.guidance.size() >= 20U,
							"Every policy choice must explain its behavior.");
					}
				}
			}

			const auto* schema = FindAnimatorInspectorProperty("schemaVersion");
			const auto* diagnostics = FindAnimatorInspectorProperty("diagnosticOverrides");
			const auto* startFrame = FindAnimatorInspectorProperty("defaultClipStartFrame");
			const auto* looping = FindAnimatorInspectorProperty("loopDefaultClip");
			const auto* autoplay = FindAnimatorInspectorProperty("playAutomatically");
			context.Expect(schema != nullptr
				&& schema->control == AnimatorInspectorControlKind::ReadOnly,
				"The serialization version must be clearly editor-managed and read-only.");
			context.Expect(diagnostics != nullptr
				&& diagnostics->control == AnimatorInspectorControlKind::ReadOnly,
				"Undefined diagnostic bits must not be exposed as a guessable numeric field.");
			context.Expect(startFrame != nullptr
				&& startFrame->label == "Default Clip Start Frame"
				&& startFrame->usage.find("60") != std::string_view::npos
				&& startFrame->usage.find("Controller") != std::string_view::npos,
				"The persistent start-frame control must document its fixed 60 Hz timeline and controller entry-clip behavior.");
			context.Expect(looping != nullptr
				&& looping->control == AnimatorInspectorControlKind::Toggle
				&& looping->usage.find("Controller") != std::string_view::npos,
				"The persistent loop control must explain how it applies to controller state clips.");
			context.Expect(autoplay != nullptr
				&& autoplay->label == "Play Automatically"
				&& autoplay->control == AnimatorInspectorControlKind::Toggle,
				"The Inspector must expose a documented persistent control for automatic animation playback.");

			const AnimatorInspectorFrameRange bounded =
				BuildAnimatorInspectorFrameRange(75U, 60);
			context.Expect(bounded.currentFrame == 60U
				&& bounded.maximumFrame == 60U
				&& FormatAnimatorInspectorFrameRange(bounded) == "60/60",
				"The start-frame editor must display current/max and prevent current from exceeding the clip duration.");
			const AnimatorInspectorFrameRange underflowBounded =
				BuildAnimatorInspectorFrameRange(-1, 60);
			context.Expect(underflowBounded.currentFrame == 0U
				&& underflowBounded.maximumFrame == 60U,
				"A leftward start-frame drag must saturate at zero instead of wrapping through an unsigned value to the maximum frame.");
			const AnimatorInspectorFrameRange loopEndpointBounded =
				BuildAnimatorInspectorFrameRange(60, 60, true);
			context.Expect(loopEndpointBounded.currentFrame == 59U
				&& loopEndpointBounded.maximumFrame == 59U
				&& FormatAnimatorInspectorFrameRange(loopEndpointBounded) == "59/59",
				"A looping start-frame slider must stop at the last distinct frame instead of exposing the duration endpoint that aliases frame zero.");
			const AnimatorInspectorFrameRange pointClipBounded =
				BuildAnimatorInspectorFrameRange(1, 0, true);
			context.Expect(pointClipBounded.currentFrame == 0U
				&& pointClipBounded.maximumFrame == 0U,
				"A zero-duration looping point clip must retain its single valid frame without underflowing the maximum.");
		});

		registry.Add("Editor.Animation.Windows.DeclaredAuthoringSurfaces", [](TestContext& context) {
			const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Windows";
			const auto registryPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp";
			std::ifstream registrySource(registryPath);
			const std::string source(
				(std::istreambuf_iterator<char>(registrySource)),
				std::istreambuf_iterator<char>());
			for(const char* file : {
				"SkeletonEditorWindow.h", "AnimationClipEditorWindow.h",
				"AnimationControllerWindow.h", "RetargetEditorWindow.h",
				"MotionDatabaseWindow.h", "RagdollEditorWindow.h",
				"AnimationDiagnosticsWindow.h"}) {
				context.Expect(std::filesystem::exists(root / file),
					"Every advanced animation authoring window must remain a registered-source candidate.");
			}
			for(const char* type : {
				"SkeletonEditorWindow", "AnimationClipEditorWindow",
				"AnimationControllerWindow", "RetargetEditorWindow",
				"MotionDatabaseWindow", "RagdollEditorWindow",
				"AnimationDiagnosticsWindow"}) {
				context.Expect(source.find(std::string("make_shared<") + type + ">")
					!= std::string::npos,
					"Every animation window must be registered through the persistent native workspace list.");
			}
			context.Expect(source.find("SkeletonEditorWindow>(this)") != std::string::npos,
				"Animation windows must observe the real workspace selection rather than disconnected placeholder state.");
		});

		registry.Add("Editor.Animation.Windows.ProjectBrowserAnimationMenu", [](TestContext& context) {
			const auto browserPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Windows/ProjectBrowserWindow.cpp";
			std::ifstream browserSource(browserPath);
			const std::string source(
				(std::istreambuf_iterator<char>(browserSource)),
				std::istreambuf_iterator<char>());
			context.Expect(source.find("BeginMenu(\"Animation\")") != std::string::npos
				&& source.find("SupportedKinds()") != std::string::npos
				&& source.find("OpenCreateAnimationAssetModal") != std::string::npos,
				"Project Browser must expose one discoverable typed Animation menu rather than test-only actions.");
			context.Expect(source.find("SetAssetRoot(root)") != std::string::npos
				&& source.find("AssetDatabase remains the project-wide registration/import owner")
					!= std::string::npos,
				"Browser staging must preserve AssetDatabase ownership and watcher-based discovery.");
		});

		registry.Add("Editor.Animation.SceneHandles.AtomicDetachedEditAndStaleRejection", [](TestContext& context) {
			AnimationSceneHandles handles;
			AnimationSceneHandle goal;
			goal.kind = AnimationHandleKind::IkGoal;
			goal.ownerId = 77U;
			goal.x = 1.0F;
			goal.y = 2.0F;
			goal.z = 3.0F;
			goal.sourceGeneration = 9U;
			goal.label = "Right hand IK";
			goal.editable = true;
			context.Expect(handles.Set({goal}),
				"A finite bounded handle packet should publish atomically.");

			AnimationSceneHandle invalid = goal;
			invalid.x = std::numeric_limits<float>::infinity();
			context.Expect(!handles.Set({invalid})
				&& handles.Snapshot().size() == 1U
				&& handles.Snapshot().front() == goal,
				"A rejected handle packet must leave the prior complete packet intact.");

			context.Expect(handles.Select(AnimationHandleKind::IkGoal, 77U)
				&& !handles.BeginEdit(8U)
				&& handles.BeginEdit(9U),
				"Detached editing must require the exact copied source generation.");
			context.Expect(!handles.UpdateEditPosition(
				std::numeric_limits<float>::quiet_NaN(), 4.0F, 5.0F),
				"Non-finite numeric controls must not replace the detached candidate.");
			context.Expect(handles.UpdateEditPosition(4.0F, 5.0F, 6.0F),
				"Finite numeric controls should update only the detached candidate.");
			context.Expect(handles.Snapshot().front() == goal,
				"Dragging a detached handle must not mutate the published runtime snapshot.");

			AnimationSceneHandle committed;
			context.Expect(!handles.CommitEdit(10U, committed)
				&& handles.ActiveEdit() != nullptr,
				"A stale Apply must reject atomically and keep the candidate inspectable.");
			context.Expect(handles.CommitEdit(9U, committed)
				&& committed.x == 4.0F && committed.y == 5.0F && committed.z == 6.0F,
				"An exact-generation Apply should return one complete candidate for the typed command queue.");
		});

		registry.Add("Editor.Animation.RetargetAuthoring.DeterministicSemanticAutoMap", [](TestContext& context) {
			const SkeletonAsset source = MakeRetargetSkeleton(10U, 3U, 101U, 201U);
			const SkeletonAsset target = MakeRetargetSkeleton(20U, 4U, 102U, 202U, true);
			const RetargetProfileAsset profile = MakeRetargetProfile(source, target);
			std::vector<std::uint8_t> bytes;
			context.Expect(SerializeRetargetProfileAsset(profile, bytes) == AnimationStatus::Ok,
				"The deterministic auto-map fixture profile should serialize.");
			const RetargetAuthoringGenerationSnapshot snapshot =
				MakeRetargetSnapshot(profile, source, target, bytes);
			RetargetEditorModel first;
			RetargetEditorModel second;
			context.Expect(first.Begin(profile, source, target, snapshot)
				&& second.Begin(profile, source, target, snapshot),
				"Both authoring models should capture the same detached generation snapshot.");
			context.Expect(first.AutoMap() == AnimationStatus::Ok
				&& second.AutoMap() == AnimationStatus::Ok
				&& first.Candidate().semanticMappings == second.Candidate().semanticMappings
				&& first.Candidate().semanticMappings.size() == 2U,
				"Semantic auto-map ordering and IDs must be deterministic for identical rigs.");
		});

		registry.Add("Editor.Animation.RetargetAuthoring.ManualMappingAndContactCorrection", [](TestContext& context) {
			const SkeletonAsset source = MakeRetargetSkeleton(11U, 5U, 111U, 211U);
			const SkeletonAsset target = MakeRetargetSkeleton(21U, 6U, 112U, 212U, true);
			const RetargetProfileAsset profile = MakeRetargetProfile(source, target);
			std::vector<std::uint8_t> bytes;
			SerializeRetargetProfileAsset(profile, bytes);
			RetargetEditorModel model;
			context.Expect(model.Begin(profile, source, target,
				MakeRetargetSnapshot(profile, source, target, bytes))
				&& model.AutoMap() == AnimationStatus::Ok,
				"The manual correction fixture should start from a valid semantic map.");
			const std::uint64_t handMappingId = model.Candidate().semanticMappings.back().mappingId;
			const std::uint64_t correctionTarget = target.joints.back().stableId;
			context.Expect(model.SetMappingTarget(handMappingId, correctionTarget)
				&& model.Candidate().semanticMappings.back().targetJointId == correctionTarget,
				"A target combo correction must update the detached mapping row.");
			context.Expect(model.AddContact(handMappingId),
				"The selected mapping should create an editable contact rule.");
			const std::uint64_t contactId = model.Candidate().contacts.front().contactId;
			context.Expect(model.SetContactTolerance(contactId, 0.005F)
				&& model.SetContactPolicy(contactId, true, true, false)
				&& model.Candidate().contacts.front().toleranceMeters == 0.005F
				&& model.Candidate().contacts.front().preserveOrientation
				&& !model.Candidate().contacts.front().required,
				"Contact numeric and policy controls must update the same detached candidate.");
		});

		registry.Add("Editor.Animation.RetargetAuthoring.StarterBindingChainsAndAtomicLastKnownGood", [](TestContext& context) {
			const SkeletonAsset source = MakeRetargetSkeleton(51U, 23U, 501U, 601U);
			const SkeletonAsset target = MakeRetargetSkeleton(61U, 24U, 502U, 602U);
			RetargetProfileAsset starter = MakeRetargetProfile(source, target);
			// New profiles deliberately contain serializable placeholders. The bind
			// path must reject those placeholders as ordinary live dependencies, then
			// replace them only in the detached starter candidate.
			starter.sourceSkeletonAssetId = MakeAnimationGuid(71U);
			starter.targetSkeletonAssetId = MakeAnimationGuid(81U);
			starter.sourceSkeletonFingerprint = 1U;
			starter.targetSkeletonFingerprint = 1U;
			starter.sourceReferencePoseHash = 1U;
			starter.targetReferencePoseHash = 1U;
			std::vector<std::uint8_t> original;
			context.Expect(SerializeRetargetProfileAsset(starter, original) == AnimationStatus::Ok,
				"A serializable retarget starter should be available before selecting real skeleton children.");
			const RetargetAuthoringGenerationSnapshot bindingSnapshot =
				MakeRetargetSnapshot(starter, source, target, original);
			RetargetEditorModel model;
			context.Expect(!model.Begin(starter, source, target, bindingSnapshot)
				&& model.Begin(starter, source, target, bindingSnapshot, true)
				&& model.Candidate().sourceSkeletonAssetId == source.assetId
				&& model.Candidate().targetSkeletonAssetId == target.assetId,
				"Only the explicit starter-binding path may replace non-resolving skeleton placeholders.");
			RetargetAuthoringGenerationSnapshot invalidBinding = bindingSnapshot;
			invalidBinding.sourceSkeleton.assetId = MakeAnimationGuid(91U);
			context.Expect(!model.Begin(starter, source, target, invalidBinding, true),
				"A selected skeleton whose GUID does not match its checked payload must fail closed.");

			context.Expect(model.AutoMap() == AnimationStatus::Ok
				&& model.AddChainToEffectors(source.joints.back().stableId,
					target.joints.back().stableId),
				"A bound starter should author deterministic mappings and a root-to-effector target chain.");
			const std::uint64_t mappingId = model.Candidate().semanticMappings.back().mappingId;
			const std::uint64_t chainId = model.Candidate().chainMappings.back().chainId;
			context.Expect(model.AddContact(mappingId)
				&& model.SetContactTargetChain(model.Candidate().contacts.back().contactId, chainId)
				&& model.Validation().publishable,
				"A contact can be bound only to a chain that reaches its target effector.");

			RetargetTestFile file;
			context.Expect(WriteRetargetBytes(file.path, original),
				"The starter binding fixture should persist a last-known-good source revision.");
			RetargetAuthoringService service;
			context.Expect(service.Open(file.path, starter, source, target, bindingSnapshot,
				original, true) == AnimationAuthoringStatus::Ok
				&& service.BeginEdit() == AnimationAuthoringStatus::Ok,
				"The service should open a starter as a detached binding transaction.");
			RetargetEditorModel* editable = service.EditableModel();
			context.Expect(editable != nullptr && editable->AutoMap() == AnimationStatus::Ok
				&& editable->AddChainToEffectors(source.joints.back().stableId,
					target.joints.back().stableId),
				"The detached service candidate should reproduce the authored starter workflow.");
			const std::uint64_t serviceMapping = editable->Candidate().semanticMappings.back().mappingId;
			const std::uint64_t serviceChain = editable->Candidate().chainMappings.back().chainId;
			context.Expect(editable->AddContact(serviceMapping)
				&& editable->SetContactTargetChain(editable->Candidate().contacts.back().contactId,
					serviceChain),
				"A service candidate should retain a reachable target-chain contact before publication.");
			AnimationAuthoringCompileRequest request;
			context.Expect(service.PrepareCommit(request) == AnimationAuthoringStatus::Ok,
				"Binding, mapping, chain, and contact changes should form one atomic candidate revision.");
			const RetargetAuthoringCompileProduct product = RetargetAuthoringService::CompileCandidate(
				request, source, target);
			auto currentSnapshot = [&service]() -> std::optional<RetargetAuthoringGenerationSnapshot> {
				return service.Model().Generation();
			};
			context.Expect(service.CompleteCommit(product, currentSnapshot)
				== AnimationAuthoringStatus::Ok && ReadRetargetBytes(file.path) != original,
				"A validated bound starter must atomically replace the original bytes.");
			RetargetProfileAsset published;
			context.Expect(DeserializeRetargetProfileAsset(ReadRetargetBytes(file.path), 0U, published)
				== AnimationStatus::Ok && published.sourceSkeletonAssetId == source.assetId
				&& published.targetSkeletonAssetId == target.assetId
				&& published.chainMappings.size() == 1U
				&& published.contacts.front().targetChainId == published.chainMappings.front().chainId,
				"The atomic result must contain the checked bindings and reachable contact chain, never a partial starter update.");
		});

		registry.Add("Editor.Animation.RetargetAuthoring.StaleGenerationRejected", [](TestContext& context) {
			const SkeletonAsset source = MakeRetargetSkeleton(12U, 7U, 121U, 221U);
			const SkeletonAsset target = MakeRetargetSkeleton(22U, 8U, 122U, 222U);
			const RetargetProfileAsset profile = MakeRetargetProfile(source, target);
			std::vector<std::uint8_t> original;
			SerializeRetargetProfileAsset(profile, original);
			RetargetTestFile file;
			context.Expect(WriteRetargetBytes(file.path, original),
				"The stale rejection fixture should write its last-known-good bytes.");
			const RetargetAuthoringGenerationSnapshot expected =
				MakeRetargetSnapshot(profile, source, target, original);
			RetargetEditorModel model;
			model.Begin(profile, source, target, expected);
			model.AutoMap();
			std::vector<std::uint8_t> candidateBytes;
			SerializeRetargetProfileAsset(model.Candidate(), candidateBytes);
			const AnimationAuthoringRevision before = MakeRetargetRevision(
				profile, expected.profile.generation, original);
			const AnimationAuthoringRevision after = MakeRetargetRevision(
				profile, before.generation + 1U, candidateBytes);
			RetargetAuthoringGenerationSnapshot stale = expected;
			++stale.targetSkeleton.generation;
			const RetargetAuthoringPublishResult result = RetargetAuthoringService::Publish(
				file.path, before, after, source, target, expected,
				[stale]() { return std::optional{stale}; });
			context.Expect(result.status == AnimationStatus::StaleHandle && !result.published
				&& ReadRetargetBytes(file.path) == original,
				"A stale source/target generation must reject before replacing prior bytes.");
		});

		registry.Add("Editor.Animation.RetargetAuthoring.InvalidCandidateRollback", [](TestContext& context) {
			const SkeletonAsset source = MakeRetargetSkeleton(13U, 9U, 131U, 231U);
			const SkeletonAsset target = MakeRetargetSkeleton(23U, 10U, 132U, 232U);
			const RetargetProfileAsset profile = MakeRetargetProfile(source, target);
			std::vector<std::uint8_t> original;
			SerializeRetargetProfileAsset(profile, original);
			RetargetTestFile file;
			WriteRetargetBytes(file.path, original);
			const RetargetAuthoringGenerationSnapshot expected =
				MakeRetargetSnapshot(profile, source, target, original);
			RetargetEditorModel model;
			model.Begin(profile, source, target, expected);
			model.AutoMap();
			RetargetProfileAsset invalid = model.Candidate();
			invalid.semanticMappings.back().targetJointId = std::numeric_limits<std::uint64_t>::max();
			std::vector<std::uint8_t> invalidBytes;
			SerializeRetargetProfileAsset(invalid, invalidBytes);
			const AnimationAuthoringRevision before = MakeRetargetRevision(
				profile, expected.profile.generation, original);
			const AnimationAuthoringRevision after = MakeRetargetRevision(
				profile, before.generation + 1U, invalidBytes);
			const RetargetAuthoringPublishResult result = RetargetAuthoringService::Publish(
				file.path, before, after, source, target, expected,
				[expected]() { return std::optional{expected}; });
			context.Expect(!result.published && result.status != AnimationStatus::Ok
				&& ReadRetargetBytes(file.path) == original,
				"A cross-skeleton validation failure must preserve the entire prior profile.");
		});

		registry.Add("Editor.Animation.RetargetAuthoring.AtomicSuccessfulPublication", [](TestContext& context) {
			const SkeletonAsset source = MakeRetargetSkeleton(14U, 11U, 141U, 241U);
			const SkeletonAsset target = MakeRetargetSkeleton(24U, 12U, 142U, 242U);
			const RetargetProfileAsset profile = MakeRetargetProfile(source, target);
			std::vector<std::uint8_t> original;
			SerializeRetargetProfileAsset(profile, original);
			RetargetTestFile file;
			WriteRetargetBytes(file.path, original);
			const RetargetAuthoringGenerationSnapshot expected =
				MakeRetargetSnapshot(profile, source, target, original);
			RetargetEditorModel model;
			model.Begin(profile, source, target, expected);
			context.Expect(model.AutoMap() == AnimationStatus::Ok && model.Validation().publishable,
				"The successful fixture should compile before it reaches persistence.");
			std::vector<std::uint8_t> candidateBytes;
			SerializeRetargetProfileAsset(model.Candidate(), candidateBytes);
			const AnimationAuthoringRevision before = MakeRetargetRevision(
				profile, expected.profile.generation, original);
			const AnimationAuthoringRevision after = MakeRetargetRevision(
				profile, before.generation + 1U, candidateBytes);
			const RetargetAuthoringPublishResult result = RetargetAuthoringService::Publish(
				file.path, before, after, source, target, expected,
				[expected]() { return std::optional{expected}; });
			RetargetProfileAsset persisted;
			const std::vector<std::uint8_t> publishedBytes = ReadRetargetBytes(file.path);
			const AnimationStatus decode = DeserializeRetargetProfileAsset(
				publishedBytes, 0U, persisted);
			context.Expect(result.status == AnimationStatus::Ok && result.published
				&& decode == AnimationStatus::Ok
				&& persisted.semanticMappings == model.Candidate().semanticMappings
				&& publishedBytes != original,
				"A fresh valid candidate should replace the file once with checked serialized bytes.");
		});

		registry.Add("Editor.Animation.RetargetAuthoring.AmbiguousAutoMapPreservesAuthoredRules", [](TestContext& context) {
			SkeletonAsset source = MakeRetargetSkeleton(31U, 13U, 301U, 401U, true);
			SkeletonAsset target = MakeRetargetSkeleton(41U, 14U, 302U, 402U, true);
			// Two joints with the same semantic are intentionally ambiguous. A tool
			// may explain the ambiguity, but it must not guess over a manual row.
			source.joints.back().semanticRole = "right_hand";
			target.joints.back().semanticRole = "right_hand";
			RetargetProfileAsset profile = MakeRetargetProfile(source, target);
			profile.explicitMappings.push_back({50U, 0U, 51U, 61U,
				source.joints[1U].stableId, target.joints[1U].stableId, true});
			profile.orientationBases.push_back({51U, source.joints[1U].stableId,
				target.joints[1U].stableId, {}, {}});
			profile.fallbackRules.push_back({61U, 50U, 0U,
				RetargetFallbackMode::ReferencePose, {}, true});
			profile.contacts.push_back({71U, source.joints[1U].stableId,
				target.joints[1U].stableId, 0U, 0.010F, true, true, true});
			const auto explicitBefore = profile.explicitMappings;
			const auto basesBefore = profile.orientationBases;
			const auto fallbacksBefore = profile.fallbackRules;
			const auto contactsBefore = profile.contacts;
			std::vector<std::uint8_t> bytes;
			SerializeRetargetProfileAsset(profile, bytes);
			RetargetEditorModel model;
			context.Expect(model.Begin(profile, source, target,
				MakeRetargetSnapshot(profile, source, target, bytes)),
				"The ambiguous semantic fixture should open as a detached profile.");
			static_cast<void>(model.AutoMap());
			context.Expect(model.Candidate().explicitMappings == explicitBefore
				&& model.Candidate().orientationBases == basesBefore
				&& model.Candidate().fallbackRules == fallbacksBefore
				&& model.Candidate().contacts == contactsBefore,
				"Auto-map assistance must preserve manual mappings, bases, fallbacks, and contacts byte-for-byte.");
			context.Expect(!model.AutoMapDiagnostics().empty()
				&& model.AutoMapDiagnostics().front().targetJointIds.size() == 2U,
				"Ambiguous semantic candidates must be diagnosed instead of silently choosing one target.");
		});

		registry.Add("Editor.Animation.RetargetAuthoring.GeometricPreviewUsesBasisScaleAndContacts", [](TestContext& context) {
			SkeletonAsset source = MakeRetargetSkeleton(32U, 15U, 311U, 411U);
			SkeletonAsset target = MakeRetargetSkeleton(42U, 16U, 312U, 412U);
			source.joints[1U].localBindTransform.translation = {1.0F, 0.0F, 0.0F};
			target.joints[1U].localBindTransform.translation = {0.0F, 2.0F, 0.0F};
			RetargetProfileAsset profile = MakeRetargetProfile(source, target);
			const float halfSqrt = std::sqrt(0.5F);
			profile.explicitMappings.push_back({1U, 0U, 2U, 0U,
				source.joints[1U].stableId, target.joints[1U].stableId, true});
			profile.orientationBases.push_back({2U, source.joints[1U].stableId,
				target.joints[1U].stableId, {}, {0.0F, 0.0F, halfSqrt, halfSqrt}});
			RetargetChainMapping chain;
			chain.chainId = 3U;
			chain.semantic = "arm";
			chain.sourceJointIds = {source.joints[0U].stableId, source.joints[1U].stableId};
			chain.targetJointIds = {target.joints[0U].stableId, target.joints[1U].stableId};
			chain.translationScaleMode = RetargetTranslationScaleMode::PerChainLength;
			profile.chainMappings.push_back(chain);
			profile.contacts.push_back({4U, source.joints[1U].stableId,
				target.joints[1U].stableId, chain.chainId, 0.010F, true, false, true});
			std::vector<std::uint8_t> bytes;
			SerializeRetargetProfileAsset(profile, bytes);
			RetargetEditorModel model;
			context.Expect(model.Begin(profile, source, target,
				MakeRetargetSnapshot(profile, source, target, bytes)),
				"The geometric preview fixture should produce a complete copied packet.");
			const RetargetQuantitativeErrorReport& errors = model.Errors();
			const RetargetPosePreviewPacket& preview = model.Preview();
			context.Expect(errors.mappings.size() == 1U
				&& std::abs(errors.mappings.front().appliedTranslationScale - 2.0F) < 0.0001F
				&& errors.mappings.front().usesAuthoredOrientationBasis
				&& errors.mappings.front().positionErrorMeters < 0.0001F,
				"Quantitative mapping errors must rotate through the authored basis and use per-chain proportions.");
			context.Expect(preview.sourceJoints.size() == source.joints.size()
				&& preview.targetJoints.size() == target.joints.size()
				&& preview.contacts.size() == 1U
				&& preview.contacts.front().withinTolerance
				&& preview.contacts.front().positionErrorMeters < 0.0001F,
				"The provider-neutral preview must contain both geometric poses and a correct visible contact-error packet.");
		});

		registry.Add("Editor.Animation.RetargetAuthoring.GroupedUndoRedoCancelAndStale", [](TestContext& context) {
			const SkeletonAsset source = MakeRetargetSkeleton(33U, 17U, 321U, 421U);
			const SkeletonAsset target = MakeRetargetSkeleton(43U, 18U, 322U, 422U);
			RetargetProfileAsset profile = MakeRetargetProfile(source, target);
			profile.semanticMappings.push_back({1U, 0U, 0U, 0U,
				source.joints[0U].stableId, target.joints[0U].stableId,
				source.joints[0U].semanticRole, target.joints[0U].semanticRole, true});
			profile.semanticMappings.push_back({2U, 1U, 0U, 0U,
				source.joints[1U].stableId, target.joints[1U].stableId,
				source.joints[1U].semanticRole, target.joints[1U].semanticRole, true});
			std::vector<std::uint8_t> original;
			SerializeRetargetProfileAsset(profile, original);
			RetargetTestFile file;
			WriteRetargetBytes(file.path, original);
			const RetargetAuthoringGenerationSnapshot initial =
				MakeRetargetSnapshot(profile, source, target, original);
			RetargetAuthoringService service;
			context.Expect(service.Open(file.path, profile, source, target, initial, original)
				== AnimationAuthoringStatus::Ok
				&& service.BeginEdit() == AnimationAuthoringStatus::Ok,
				"A retarget document should begin one grouped shared authoring transaction.");
			RetargetEditorModel* editable = service.EditableModel();
			context.Expect(editable != nullptr && editable->AutoMap() == AnimationStatus::Ok,
				"The grouped transaction should accept multiple detached model changes.");
			const std::uint64_t mappingId = editable->Candidate().semanticMappings.back().mappingId;
			context.Expect(editable->SetMappingRequired(mappingId, false),
				"A second correction should remain inside the same grouped transaction.");
			auto currentSnapshot = [&service]() -> std::optional<RetargetAuthoringGenerationSnapshot> {
				return service.Model().Generation();
			};
			AnimationAuthoringCompileRequest request;
			context.Expect(service.PrepareCommit(request) == AnimationAuthoringStatus::Ok,
				"The complete candidate should produce one copied validation packet.");
			const RetargetAuthoringCompileProduct product = RetargetAuthoringService::CompileCandidate(
				request, source, target);
			context.Expect(service.CompleteCommit(product, currentSnapshot)
				== AnimationAuthoringStatus::Ok && service.UndoDepth() == 1U,
				"Publishing multiple corrections must create exactly one complete history entry.");
			const std::vector<std::uint8_t> committed = ReadRetargetBytes(file.path);
			context.Expect(committed != original && service.Generation() == 8U,
				"The grouped commit should replace the exact file and advance the logical generation once.");
			context.Expect(service.Undo(currentSnapshot) == AnimationAuthoringStatus::Ok
				&& ReadRetargetBytes(file.path) == original && service.Generation() == 9U,
				"Undo must republish the complete original bytes without rewinding generation.");
			context.Expect(service.Redo(currentSnapshot) == AnimationAuthoringStatus::Ok
				&& ReadRetargetBytes(file.path) == committed && service.Generation() == 10U,
				"Redo must republish the complete grouped candidate as another generation.");

			context.Expect(service.BeginEdit() == AnimationAuthoringStatus::Ok,
				"A later detached edit should start from the redone durable revision.");
			editable = service.EditableModel();
			const bool required = editable->Candidate().semanticMappings.back().required;
			context.Expect(editable->SetMappingRequired(mappingId, !required),
				"The cancellation fixture should contain a real byte change.");
			AnimationAuthoringCompileRequest canceledRequest;
			context.Expect(service.PrepareCommit(canceledRequest) == AnimationAuthoringStatus::Ok
				&& service.Cancel() == AnimationAuthoringStatus::Canceled
				&& canceledRequest.cancellationRequested->load(std::memory_order_acquire),
				"Cancel must signal the copied worker token and restore the durable candidate.");
			const RetargetAuthoringCompileProduct canceledProduct =
				RetargetAuthoringService::CompileCandidate(canceledRequest, source, target);
			context.Expect(service.CompleteCommit(canceledProduct, currentSnapshot)
				== AnimationAuthoringStatus::Canceled
				&& ReadRetargetBytes(file.path) == committed,
				"A canceled completion must never replace or append history to the durable file.");

			context.Expect(service.BeginEdit() == AnimationAuthoringStatus::Ok,
				"Stale validation should be testable in a fresh detached transaction.");
			editable = service.EditableModel();
			context.Expect(editable->SetMappingRequired(mappingId,
				!editable->Candidate().semanticMappings.back().required),
				"The stale fixture should prepare a distinct valid candidate.");
			AnimationAuthoringCompileRequest staleRequest;
			service.PrepareCommit(staleRequest);
			const RetargetAuthoringCompileProduct staleProduct =
				RetargetAuthoringService::CompileCandidate(staleRequest, source, target);
			auto staleSnapshot = [&service]() -> std::optional<RetargetAuthoringGenerationSnapshot> {
				RetargetAuthoringGenerationSnapshot stale = service.Model().Generation();
				++stale.targetSkeleton.generation;
				return stale;
			};
			context.Expect(service.CompleteCommit(staleProduct, staleSnapshot)
				== AnimationAuthoringStatus::Stale
				&& ReadRetargetBytes(file.path) == committed,
				"A changed dependency generation must reject the copied validation result before publication.");
			static_cast<void>(service.Cancel());
		});

		registry.Add("Editor.Animation.RetargetAuthoring.ConcurrentSameRevisionHasOneWinner", [](TestContext& context) {
			const SkeletonAsset source = MakeRetargetSkeleton(34U, 19U, 331U, 431U);
			const SkeletonAsset target = MakeRetargetSkeleton(44U, 20U, 332U, 432U);
			const RetargetProfileAsset profile = MakeRetargetProfile(source, target);
			std::vector<std::uint8_t> original;
			SerializeRetargetProfileAsset(profile, original);
			RetargetTestFile file;
			WriteRetargetBytes(file.path, original);
			const RetargetAuthoringGenerationSnapshot expected =
				MakeRetargetSnapshot(profile, source, target, original);
			RetargetEditorModel first;
			RetargetEditorModel second;
			first.Begin(profile, source, target, expected);
			second.Begin(profile, source, target, expected);
			first.AutoMap();
			second.AutoMap();
			const std::uint64_t mappingId = first.Candidate().semanticMappings.back().mappingId;
			first.SetMappingRequired(mappingId, false);
			second.AddContact(mappingId);
			std::vector<std::uint8_t> firstBytes;
			std::vector<std::uint8_t> secondBytes;
			SerializeRetargetProfileAsset(first.Candidate(), firstBytes);
			SerializeRetargetProfileAsset(second.Candidate(), secondBytes);
			const AnimationAuthoringRevision before = MakeRetargetRevision(
				profile, expected.profile.generation, original);
			const std::array<AnimationAuthoringRevision, 2U> replacements{
				MakeRetargetRevision(profile, before.generation + 1U, firstBytes),
				MakeRetargetRevision(profile, before.generation + 1U, secondBytes)};
			std::array<RetargetAuthoringPublishResult, 2U> results;
			std::barrier start(3);
			std::array<std::thread, 2U> publishers;
			for(std::size_t index = 0U; index < publishers.size(); ++index) {
				publishers[index] = std::thread([&, index]() {
					start.arrive_and_wait();
					results[index] = RetargetAuthoringService::Publish(file.path, before,
						replacements[index], source, target, expected,
						[expected]() { return std::optional{expected}; });
				});
			}
			start.arrive_and_wait();
			for(std::thread& publisher : publishers) {
				publisher.join();
			}
			const std::size_t winners = static_cast<std::size_t>(results[0U].published)
				+ static_cast<std::size_t>(results[1U].published);
			const std::vector<std::uint8_t> finalBytes = ReadRetargetBytes(file.path);
			context.Expect(winners == 1U,
				"The protected exact-byte compare-and-swap must allow exactly one same-revision publisher.");
			context.Expect((results[0U].status == AnimationStatus::StaleHandle)
				!= (results[1U].status == AnimationStatus::StaleHandle),
				"The losing same-revision publication must report a stale compare-and-swap.");
			context.Expect(finalBytes == firstBytes || finalBytes == secondBytes,
				"Concurrent publication must leave one complete validated candidate, never mixed or partial bytes.");
		});
	}
}
