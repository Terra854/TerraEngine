// LocomotionFixtureMaterializer.cpp - deterministic production-codec workload assets.
// Usage: TerraEngine-LocomotionFixtureMaterializer <empty output directory>
// This validation-only tool compiles and serializes the same authored identities
// as LocomotionAnimationFixture.h; it does not invent a private cooked format.

#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Assets/AnimationControllerAsset.h"
#include "Animation/Assets/RagdollProfileAsset.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Animation/Ragdoll/RagdollProfileValidator.h"
#include "AnimationClipCompiler.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
	using namespace TerraEngine;
	using namespace TerraEngine::Animation;
	constexpr std::uint64_t kFingerprint = 0x4c4f434f4d4f5433ULL;

	AnimationGuid Guid(std::uint8_t seed) {
		AnimationGuid value;
		constexpr std::array<std::uint8_t, 13U> prefix{
			'T', 'E', 'R', 'R', 'A', '-', 'L', 'O', 'C', 'O', '0', '6', '0'};
		std::copy(prefix.begin(), prefix.end(), value.value.begin());
		value.value[13U] = seed;
		value.value[14U] = 0xA5U;
		value.value[15U] = 0x5AU;
		return value;
	}

	AnimationMatrix4x4 Identity() {
		AnimationMatrix4x4 value{};
		value.values[0U] = value.values[5U] = value.values[10U] = value.values[15U] = 1.0F;
		return value;
	}

	bool Write(const std::filesystem::path& output, std::span<const std::uint8_t> bytes) {
		std::ofstream stream(output, std::ios::binary | std::ios::trunc);
		if (!stream) return false;
		stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		return stream.good();
	}

	bool CompileClip(const SkeletonAsset& skeleton, std::uint8_t seed, bool rootMotion,
		float endX, float endYaw, AnimationClipAsset& output) {
		AssetPipeline::AnimationClipCompileInput input;
		input.assetId = Guid(seed);
		input.skeletonAssetId = skeleton.assetId;
		input.compatibilityFingerprint = skeleton.compatibilityFingerprint;
		input.durationTicks = 8;
		input.sampleRate = 60U;
		input.segmentDurationTicks = 8;
		AssetPipeline::AnimationSourceTrack translation;
		translation.stableTargetId = 1U;
		translation.kind = rootMotion ? AnimationClipTrackKind::RootTranslation : AnimationClipTrackKind::Translation;
		translation.interpolation = AnimationInterpolation::Linear;
		translation.keys = {{0, {0.0F, 0.0F, 0.0F}}, {8, {endX, 0.0F, 0.0F}}};
		input.tracks.push_back(translation);
		if (rootMotion) {
			AssetPipeline::AnimationSourceTrack yaw;
			yaw.stableTargetId = 1U;
			yaw.kind = AnimationClipTrackKind::RootYaw;
			yaw.interpolation = AnimationInterpolation::Linear;
			AssetPipeline::AnimationSourceKey begin;
			begin.tick = 0;
			begin.scalarValue = 0.0F;
			AssetPipeline::AnimationSourceKey end;
			end.tick = 8;
			end.scalarValue = endYaw;
			yaw.keys = {begin, end};
			input.tracks.push_back(yaw);
		}
		AssetPipeline::AnimationClipCompileReport report;
		return AssetPipeline::CompileAnimationClip(input, {}, output, report) == AnimationStatus::Ok
			&& (!rootMotion || output.rootMotion.enabled);
	}

	bool Materialize(const std::filesystem::path& directory) {
		SkeletonAsset skeleton;
		skeleton.assetId = Guid(60U);
		skeleton.compatibilityFingerprint = kFingerprint;
		skeleton.rootJointIndex = 0U;
		skeleton.referencePoseHash = 0x060060060060060ULL;
		SkeletonJoint rootJoint;
		rootJoint.stableId = 1U;
		rootJoint.parentIndex = -1;
		rootJoint.displayName = "LocomotionRoot";
		rootJoint.normalizedSourcePath = "/root";
		rootJoint.semanticRole = "hips";
		skeleton.joints.push_back(rootJoint);
		skeleton.inverseBindMatrices.push_back(Identity());
		std::vector<std::uint8_t> bytes;
		if (SerializeSkeletonAsset(skeleton, bytes) != AnimationStatus::Ok
			|| !Write(directory / "skeleton.teskel", bytes)) return false;

		AnimationClipAsset idle;
		AnimationClipAsset move;
		AnimationClipAsset root;
		if (!CompileClip(skeleton, 80U, false, 0.0F, 0.0F, idle)
			|| !CompileClip(skeleton, 100U, false, 0.2F, 0.0F, move)
			|| !CompileClip(skeleton, 120U, true, 1.0F, 0.35F, root)) return false;
		const std::array<std::pair<const AnimationClipAsset*, const char*>, 3U> clips{{
			{&idle, "idle.teanim"}, {&move, "move.teanim"}, {&root, "root.teanim"}}};
		for (const auto& [clip, name] : clips) {
			bytes.clear();
			if (SerializeAnimationClipAsset(*clip, bytes) != AnimationStatus::Ok
				|| !Write(directory / name, bytes)) return false;
		}

		AnimationControllerAsset controller;
		controller.assetId = Guid(140U);
		controller.skeletonAssetId = skeleton.assetId;
		controller.compatibilityFingerprint = kFingerprint;
		controller.parameters = {
			{601U, "Speed", AnimationControllerParameterType::Float, 0.0F},
			{602U, "DirectionX", AnimationControllerParameterType::Float, 0.0F},
			{603U, "DirectionZ", AnimationControllerParameterType::Float, 0.0F},
			{604U, "Grounded", AnimationControllerParameterType::Boolean, true},
			{605U, "Moving", AnimationControllerParameterType::Boolean, false},
			{606U, "StartMove", AnimationControllerParameterType::Trigger, std::monostate{}},
			{607U, "StartRootMotion", AnimationControllerParameterType::Trigger, std::monostate{}}};
		// A fresh ResourceManager publication assigns dependency generation one to
		// each cooked clip; no runtime generation is embedded from a test process.
		controller.nodes.push_back({10U, AnimationControllerNodeKind::Clip, 0U, idle.assetId, 1U, kFingerprint});
		controller.nodes.push_back({20U, AnimationControllerNodeKind::Clip, 0U, move.assetId, 1U, kFingerprint});
		controller.nodes.push_back({30U, AnimationControllerNodeKind::Clip, 0U, root.assetId, 1U, kFingerprint});
		controller.states = {{101U, 10U, 0U, true, false, false},
			{102U, 20U, 0U, false, false, false}, {103U, 30U, 0U, false, false, false}};
		AnimationControllerTransition toMove;
		toMove.stableId = 201U;
		toMove.sourceStateId = 101U;
		toMove.destinationStateId = 102U;
		toMove.conditionParameterId = 606U;
		toMove.durationTicks = 2;
		AnimationControllerTransition toRoot = toMove;
		toRoot.stableId = 202U;
		toRoot.destinationStateId = 103U;
		toRoot.conditionParameterId = 607U;
		controller.transitions = {toMove, toRoot};
		controller.layers.push_back({301U, 101U, {}, 0U, 0U, false, 1.0F});
		bytes.clear();
		if (SerializeAnimationControllerAsset(controller, bytes) != AnimationStatus::Ok
			|| !Write(directory / "locomotion.teanimctrl", bytes)) return false;

		RagdollProfileAsset profile;
		profile.assetId = Guid(160U);
		profile.skeletonAssetId = skeleton.assetId;
		profile.skeletonFingerprint = kFingerprint;
		profile.referencePoseHash = skeleton.referencePoseHash;
		profile.physicsProviderFingerprint = 0x4a4f4c54U;
		profile.rootBodyId = 100U;
		profile.expectedTotalMassKilograms = 8.0F;
		profile.expectedMassToleranceFraction = 0.01F;
		profile.minimumBodyMassFraction = 0.9F;
		profile.maximumBodyMassFraction = 1.0F;
		profile.allowedCollisionLayerMask = 0x1U;
		RagdollColliderRecord collider;
		collider.colliderId = 1000U;
		collider.shape = RagdollColliderShape::Sphere;
		collider.dimensions = {0.25F, 0.0F, 0.0F};
		RagdollBodyRecord body;
		body.bodyId = 100U;
		body.stableJointId = 1U;
		body.colliders.push_back(collider);
		body.massKilograms = 8.0F;
		body.gravityFactor = 0.0F;
		body.collisionLayer = 0U;
		body.collisionMask = 0x1U;
		profile.bodies.push_back(std::move(body));
		profile.blends.push_back({1U, 1.0F, RagdollBoundaryPolicy::KinematicAnchor});
		RagdollRecoveryRecord recovery;
		recovery.candidateId = 700U;
		recovery.clipAssetId = root.assetId;
		recovery.requiredContactMask = 0U;
		recovery.clipGeneration = 1U;
		recovery.clipSkeletonFingerprint = kFingerprint;
		recovery.requiredReferencePoseHash = skeleton.referencePoseHash;
		recovery.rootJointStableId = 1U;
		recovery.posture = RagdollRecoveryPosture::Any;
		recovery.maximumPoseErrorMeters = 10.0F;
		recovery.maximumLinearSpeed = 100.0F;
		recovery.maximumAngularSpeed = 100.0F;
		recovery.blendDurationSeconds = 0.05F;
		recovery.clearanceRadiusMeters = 0.25F;
		recovery.clearanceHeightMeters = 1.0F;
		profile.recovery.push_back(std::move(recovery));
		const std::array<RagdollCollisionLayerRule, 1U> layers{{{0U, 0x1U, true}}};
		RagdollProfileValidationOptions options;
		options.collisionLayers = layers;
		if (!ValidateRagdollProfile(profile, skeleton, options).IsValid()) return false;
		bytes.clear();
		return SerializeRagdollProfileAsset(profile, bytes) == AnimationStatus::Ok
			&& Write(directory / "locomotion.teragdoll", bytes);
	}
}

int main(int argc, char** argv) {
	if (argc != 2) {
		std::cerr << "Usage: TerraEngine-LocomotionFixtureMaterializer <empty output directory>\n";
		return 2;
	}
	const std::filesystem::path output = std::filesystem::absolute(argv[1]);
	std::error_code error;
	if (!std::filesystem::is_directory(output, error) || !std::filesystem::is_empty(output, error)) {
		std::cerr << "Output must be an existing empty directory.\n";
		return 2;
	}
	if (!Materialize(output)) {
		std::cerr << "Production asset compilation or serialization failed.\n";
		return 1;
	}
	return 0;
}
