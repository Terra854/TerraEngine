// AdvancedAnimationFixtureMaterializer.cpp - Deterministic production-codec fixtures.
//
// This standalone tool creates the small, mutually compatible advanced-animation
// asset set used by installed-editor validation. It deliberately calls the same
// compilers, serializers, decoders, and semantic validators used by production
// code. The output is therefore real cooked content, not renamed semantic JSON or
// a generic animation envelope with placeholder payload bytes.

#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Assets/MotionDatabaseAsset.h"
#include "Animation/Assets/RagdollProfileAsset.h"
#include "Animation/Assets/RetargetProfileAsset.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Animation/Codec/AnimationCodec.h"
#include "Animation/Ragdoll/RagdollProfileValidator.h"
#include "Animation/Retargeting/RetargetValidator.h"
#include "AnimationClipCompiler.h"
#include "MotionDatabaseCompiler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
	using namespace TerraEngine::Animation;
	using namespace TerraEngine::AssetPipeline;

	constexpr AnimationCompatibilityFingerprint kSourceSkeletonFingerprint =
		0x043a000000001001ULL;
	constexpr AnimationCompatibilityFingerprint kTargetSkeletonFingerprint =
		0x043a000000002001ULL;
	constexpr AnimationCompatibilityFingerprint kRetargetProfileFingerprint =
		0x043a000000007001ULL;
	constexpr std::uint64_t kSourceReferencePoseHash = 0x043a000000001101ULL;
	constexpr std::uint64_t kTargetReferencePoseHash = 0x043a000000002101ULL;
	constexpr std::uint64_t kPhysicsProviderFingerprint = 0x000000004a4f4c54ULL;
	constexpr std::uint32_t kMotionDatabaseSamplesPerBlock = 2U;

	// These suffixes are part of the fixture identity contract. The first fifteen
	// bytes are readable ("TEAN043ADVFIX") and the final byte names one asset.
	enum class FixtureGuidKind : std::uint8_t {
		SourceSkeleton = 1U,
		TargetSkeleton = 2U,
		SourceLocomotionClip = 3U,
		TargetRecoveryClip = 4U,
		RetargetProfile = 5U,
		MotionDatabase = 6U,
		RagdollProfile = 7U,
		RecordingSourceAnimator = 8U
	};

	struct GeneratedFile {
		std::string fileName;
		std::string assetType;
		AnimationGuid assetId{};
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		std::vector<std::uint8_t> bytes;
	};

	struct FixtureBundle {
		std::vector<GeneratedFile> files;
		std::uint64_t motionSourceClipId = 0U;
		std::uint64_t motionSourceClipFingerprint = 0U;
		std::uint64_t retargetCompiledHash = 0U;
		std::uint64_t motionContentFingerprint = 0U;
		std::string manifest;
	};

	[[noreturn]] void Fail(std::string_view message) {
		throw std::runtime_error(std::string(message));
	}

	void Require(bool condition, std::string_view message) {
		if(!condition) {
			Fail(message);
		}
	}

	void RequireStatus(AnimationStatus status, std::string_view operation) {
		if(status == AnimationStatus::Ok) {
			return;
		}
		std::ostringstream message;
		message << operation << " failed with " << ToString(status) << '.';
		Fail(message.str());
	}

	AnimationGuid MakeFixtureGuid(FixtureGuidKind kind) {
		AnimationGuid result;
		constexpr std::array<std::uint8_t, 15U> prefix{
			'T', 'E', 'A', 'N', '0', '4', '3', 'A', 'D', 'V', 'F', 'I', 'X', 0U, 0U};
		std::copy(prefix.begin(), prefix.end(), result.value.begin());
		result.value[15U] = static_cast<std::uint8_t>(kind);
		return result;
	}

	std::string HexBytes(std::span<const std::uint8_t> bytes) {
		std::ostringstream text;
		text << std::hex << std::setfill('0');
		for(const std::uint8_t byte : bytes) {
			text << std::setw(2) << static_cast<unsigned int>(byte);
		}
		return text.str();
	}

	std::string GuidHex(const AnimationGuid& guid) {
		return HexBytes(guid.value);
	}

	std::string Hex64(std::uint64_t value) {
		std::ostringstream text;
		text << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
		return text.str();
	}

	std::uint64_t Fnv1a64(std::span<const std::uint8_t> bytes) noexcept {
		std::uint64_t hash = 14695981039346656037ULL;
		for(const std::uint8_t byte : bytes) {
			hash = (hash ^ byte) * 1099511628211ULL;
		}
		return hash;
	}

	std::uint64_t StableIdFromGuid(const AnimationGuid& guid) noexcept {
		return Fnv1a64(guid.value);
	}

	// The fixture manifest uses SHA-256 because external validation scripts and
	// reviewers can reproduce it with ordinary repository tools. Keeping this
	// tiny implementation local avoids adding a dependency solely for fixture
	// generation.
	std::array<std::uint8_t, 32U> Sha256(std::span<const std::uint8_t> input) {
		constexpr std::array<std::uint32_t, 64U> roundConstants{
			0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
			0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
			0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
			0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
			0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
			0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
			0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
			0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
			0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
			0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
			0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
			0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
			0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
			0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
			0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
			0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
		std::array<std::uint32_t, 8U> state{
			0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
			0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

		Require(input.size() <= std::numeric_limits<std::uint64_t>::max() / 8U,
			"SHA-256 input is too large to represent its bit count.");
		std::vector<std::uint8_t> padded(input.begin(), input.end());
		padded.push_back(0x80U);
		while((padded.size() % 64U) != 56U) {
			padded.push_back(0U);
		}
		const std::uint64_t bitCount = static_cast<std::uint64_t>(input.size()) * 8U;
		for(int shift = 56; shift >= 0; shift -= 8) {
			padded.push_back(static_cast<std::uint8_t>(bitCount >> shift));
		}

		for(std::size_t blockOffset = 0U; blockOffset < padded.size();
			blockOffset += 64U) {
			std::array<std::uint32_t, 64U> words{};
			for(std::size_t index = 0U; index < 16U; ++index) {
				const std::size_t offset = blockOffset + index * 4U;
				words[index] = (static_cast<std::uint32_t>(padded[offset]) << 24U)
					| (static_cast<std::uint32_t>(padded[offset + 1U]) << 16U)
					| (static_cast<std::uint32_t>(padded[offset + 2U]) << 8U)
					| static_cast<std::uint32_t>(padded[offset + 3U]);
			}
			for(std::size_t index = 16U; index < words.size(); ++index) {
				const std::uint32_t first = std::rotr(words[index - 15U], 7)
					^ std::rotr(words[index - 15U], 18)
					^ (words[index - 15U] >> 3U);
				const std::uint32_t second = std::rotr(words[index - 2U], 17)
					^ std::rotr(words[index - 2U], 19)
					^ (words[index - 2U] >> 10U);
				words[index] = words[index - 16U] + first
					+ words[index - 7U] + second;
			}

			std::uint32_t a = state[0U];
			std::uint32_t b = state[1U];
			std::uint32_t c = state[2U];
			std::uint32_t d = state[3U];
			std::uint32_t e = state[4U];
			std::uint32_t f = state[5U];
			std::uint32_t g = state[6U];
			std::uint32_t h = state[7U];
			for(std::size_t index = 0U; index < words.size(); ++index) {
				const std::uint32_t choose = (e & f) ^ (~e & g);
				const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
				const std::uint32_t bigSigma0 =
					std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
				const std::uint32_t bigSigma1 =
					std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
				const std::uint32_t first = h + bigSigma1 + choose
					+ roundConstants[index] + words[index];
				const std::uint32_t second = bigSigma0 + majority;
				h = g;
				g = f;
				f = e;
				e = d + first;
				d = c;
				c = b;
				b = a;
				a = first + second;
			}
			state[0U] += a;
			state[1U] += b;
			state[2U] += c;
			state[3U] += d;
			state[4U] += e;
			state[5U] += f;
			state[6U] += g;
			state[7U] += h;
		}

		std::array<std::uint8_t, 32U> digest{};
		for(std::size_t index = 0U; index < state.size(); ++index) {
			digest[index * 4U] = static_cast<std::uint8_t>(state[index] >> 24U);
			digest[index * 4U + 1U] = static_cast<std::uint8_t>(state[index] >> 16U);
			digest[index * 4U + 2U] = static_cast<std::uint8_t>(state[index] >> 8U);
			digest[index * 4U + 3U] = static_cast<std::uint8_t>(state[index]);
		}
		return digest;
	}

	std::string Sha256Hex(std::span<const std::uint8_t> bytes) {
		const std::array<std::uint8_t, 32U> digest = Sha256(bytes);
		return HexBytes(digest);
	}

	AnimationTransform Transform(float x, float y, float z) {
		AnimationTransform result;
		result.translation = {x, y, z};
		return result;
	}

	AnimationMatrix4x4 InverseTranslation(float x, float y, float z) {
		AnimationMatrix4x4 result;
		result.values[0U] = 1.0F;
		result.values[5U] = 1.0F;
		result.values[10U] = 1.0F;
		result.values[15U] = 1.0F;
		result.values[12U] = -x;
		result.values[13U] = -y;
		result.values[14U] = -z;
		return result;
	}

	SkeletonJoint Joint(
		std::uint64_t stableId,
		std::int32_t parentIndex,
		AnimationFloat3 localTranslation,
		std::string_view displayName,
		std::string_view sourcePath,
		std::string_view semanticRole) {
		SkeletonJoint result;
		result.stableId = stableId;
		result.parentIndex = parentIndex;
		result.localBindTransform = Transform(
			localTranslation.x, localTranslation.y, localTranslation.z);
		result.deformsMesh = true;
		result.displayName = std::string(displayName);
		result.normalizedSourcePath = std::string(sourcePath);
		result.semanticRole = std::string(semanticRole);
		return result;
	}

	SkeletonSocket Socket(
		std::uint64_t stableId,
		std::uint64_t jointStableId,
		std::string_view displayName,
		std::string_view semanticRole) {
		SkeletonSocket result;
		result.stableId = stableId;
		result.jointStableId = jointStableId;
		result.displayName = std::string(displayName);
		result.semanticRole = std::string(semanticRole);
		return result;
	}

	SkeletonAsset MakeSourceSkeleton() {
		SkeletonAsset skeleton;
		skeleton.assetId = MakeFixtureGuid(FixtureGuidKind::SourceSkeleton);
		skeleton.compatibilityFingerprint = kSourceSkeletonFingerprint;
		skeleton.rootJointIndex = 0U;
		skeleton.referencePoseHash = kSourceReferencePoseHash;
		skeleton.source.sourcePackagePath = "AdvancedAssets/SourceHumanoid.synthetic";
		skeleton.joints = {
			Joint(1U, -1, {0.0F, 0.0F, 0.0F}, "Root", "/root", "Root"),
			Joint(2U, 0, {0.0F, 1.0F, 0.0F}, "Hips", "/root/hips", "Hips"),
			Joint(3U, 1, {0.0F, -0.5F, 0.0F}, "LeftLeg",
				"/root/hips/left_leg", "LeftLeg"),
			Joint(4U, 2, {0.0F, -0.5F, 0.0F}, "LeftFoot",
				"/root/hips/left_leg/left_foot", "LeftFoot"),
			Joint(5U, 1, {0.45F, 0.30F, 0.0F}, "LeftArm",
				"/root/hips/left_arm", "LeftArm"),
			Joint(6U, 4, {0.45F, 0.0F, 0.0F}, "LeftHand",
				"/root/hips/left_arm/left_hand", "LeftHand"),
			Joint(7U, 1, {0.0F, 0.0F, -0.4F}, "OptionalTail",
				"/root/hips/optional_tail", "OptionalTail")};
		skeleton.inverseBindMatrices = {
			InverseTranslation(0.0F, 0.0F, 0.0F),
			InverseTranslation(0.0F, 1.0F, 0.0F),
			InverseTranslation(0.0F, 0.5F, 0.0F),
			InverseTranslation(0.0F, 0.0F, 0.0F),
			InverseTranslation(0.45F, 1.3F, 0.0F),
			InverseTranslation(0.90F, 1.3F, 0.0F),
			InverseTranslation(0.0F, 1.0F, -0.4F)};
		skeleton.sockets = {
			Socket(10001U, 4U, "Left Foot Contact", "left_foot_contact"),
			Socket(10002U, 6U, "Left Hand Grip", "left_hand_grip")};
		return skeleton;
	}

	SkeletonAsset MakeTargetSkeleton() {
		SkeletonAsset skeleton;
		skeleton.assetId = MakeFixtureGuid(FixtureGuidKind::TargetSkeleton);
		skeleton.compatibilityFingerprint = kTargetSkeletonFingerprint;
		skeleton.rootJointIndex = 0U;
		skeleton.referencePoseHash = kTargetReferencePoseHash;
		skeleton.source.sourcePackagePath = "AdvancedAssets/TargetHumanoid.synthetic";
		skeleton.joints = {
			Joint(101U, -1, {0.0F, 0.0F, 0.0F}, "Root", "/root", "Root"),
			Joint(102U, 0, {0.0F, 1.5F, 0.0F}, "Hips", "/root/hips", "Hips"),
			Joint(103U, 1, {0.0F, -0.75F, 0.0F}, "LeftLeg",
				"/root/hips/left_leg", "LeftLeg"),
			Joint(104U, 2, {0.0F, -0.25F, 0.0F}, "LegDetail",
				"/root/hips/left_leg/detail", "LegDetail"),
			Joint(105U, 3, {0.0F, -0.5F, 0.0F}, "LeftFoot",
				"/root/hips/left_leg/detail/left_foot", "LeftFoot"),
			Joint(106U, 1, {0.55F, 0.45F, 0.0F}, "LeftArm",
				"/root/hips/left_arm", "LeftArm"),
			Joint(107U, 5, {0.30F, 0.0F, 0.0F}, "ArmTwist",
				"/root/hips/left_arm/twist", "ArmTwist"),
			Joint(108U, 6, {0.30F, 0.0F, 0.0F}, "LeftHand",
				"/root/hips/left_arm/twist/left_hand", "LeftHand")};
		skeleton.inverseBindMatrices = {
			InverseTranslation(0.0F, 0.0F, 0.0F),
			InverseTranslation(0.0F, 1.5F, 0.0F),
			InverseTranslation(0.0F, 0.75F, 0.0F),
			InverseTranslation(0.0F, 0.5F, 0.0F),
			InverseTranslation(0.0F, 0.0F, 0.0F),
			InverseTranslation(0.55F, 1.95F, 0.0F),
			InverseTranslation(0.85F, 1.95F, 0.0F),
			InverseTranslation(1.15F, 1.95F, 0.0F)};
		skeleton.sockets = {
			Socket(20001U, 105U, "Left Foot Contact", "left_foot_contact"),
			Socket(20002U, 108U, "Left Hand Grip", "left_hand_grip")};
		return skeleton;
	}

	void ValidateSkeletonSemantics(const SkeletonAsset& skeleton) {
		Require(skeleton.assetId.IsValid(), "A fixture skeleton must have a valid asset ID.");
		Require(skeleton.rootJointIndex < skeleton.joints.size(),
			"A fixture skeleton root index must address a real joint.");
		Require(skeleton.inverseBindMatrices.size() == skeleton.joints.size(),
			"A fixture skeleton needs one inverse bind matrix per joint.");
		Require(skeleton.joints[skeleton.rootJointIndex].parentIndex == -1,
			"The declared fixture root must have no parent.");
		for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
			const SkeletonJoint& joint = skeleton.joints[index];
			Require(joint.stableId != 0U && !joint.normalizedSourcePath.empty()
					&& !joint.semanticRole.empty(),
				"Every fixture joint needs stable identity, path, and semantic data.");
			Require(joint.parentIndex < 0
					|| static_cast<std::size_t>(joint.parentIndex) < index,
				"Fixture joints must remain in deterministic parent-before-child order.");
		}
	}

	std::vector<std::uint8_t> SerializeSkeletonChecked(const SkeletonAsset& skeleton) {
		ValidateSkeletonSemantics(skeleton);
		std::vector<std::uint8_t> first;
		std::vector<std::uint8_t> second;
		RequireStatus(SerializeSkeletonAsset(skeleton, first),
			"SerializeSkeletonAsset(first)");
		RequireStatus(SerializeSkeletonAsset(skeleton, second),
			"SerializeSkeletonAsset(second)");
		Require(first == second,
			"Identical skeleton input must produce byte-identical production output.");
		SkeletonAsset decoded;
		RequireStatus(DeserializeSkeletonAsset(
			first, skeleton.compatibilityFingerprint, decoded),
			"DeserializeSkeletonAsset");
		ValidateSkeletonSemantics(decoded);
		Require(decoded == skeleton,
			"Every durable skeleton field must survive production round-trip decode.");
		std::vector<std::uint8_t> roundTrip;
		RequireStatus(SerializeSkeletonAsset(decoded, roundTrip),
			"SerializeSkeletonAsset(round-trip)");
		Require(roundTrip == first,
			"Decoded skeleton data must serialize back to the exact same bytes.");
		return first;
	}

	AnimationSourceKey VectorKey(AnimationFixedTick tick, AnimationFloat3 value) {
		AnimationSourceKey key;
		key.tick = tick;
		key.vectorValue = value;
		return key;
	}

	AnimationSourceKey RotationKey(AnimationFixedTick tick, AnimationQuaternion value) {
		AnimationSourceKey key;
		key.tick = tick;
		key.rotationValue = value;
		return key;
	}

	AnimationSourceKey ScalarKey(AnimationFixedTick tick, float value) {
		AnimationSourceKey key;
		key.tick = tick;
		key.scalarValue = value;
		return key;
	}

	AnimationClipCompilerSettings FixtureClipCompilerSettings() {
		AnimationClipCompilerSettings settings;
		settings.translationToleranceMeters = 0.0005F;
		settings.rotationToleranceDegrees = 0.1F;
		settings.scaleToleranceFraction = 0.0005F;
		settings.morphTolerance = 0.0005F;
		settings.curveTolerance = 0.0005F;
		return settings;
	}

	AnimationClipCompileInput MakeSourceLocomotionInput(const SkeletonAsset& source) {
		AnimationClipCompileInput input;
		input.assetId = MakeFixtureGuid(FixtureGuidKind::SourceLocomotionClip);
		input.skeletonAssetId = source.assetId;
		input.compatibilityFingerprint = source.compatibilityFingerprint;
		input.durationTicks = 120;
		input.sampleRate = 60U;
		input.segmentDurationTicks = 30;

		AnimationSourceTrack hipsTranslation;
		hipsTranslation.stableTargetId = 2U;
		hipsTranslation.kind = AnimationClipTrackKind::Translation;
		hipsTranslation.interpolation = AnimationInterpolation::Linear;
		hipsTranslation.keys = {
			VectorKey(0, {0.0F, 1.0F, 0.0F}),
			VectorKey(30, {0.0F, 1.04F, 0.0F}),
			VectorKey(60, {0.0F, 1.0F, 0.0F}),
			VectorKey(90, {0.0F, 1.04F, 0.0F}),
			VectorKey(120, {0.0F, 1.0F, 0.0F})};

		AnimationSourceTrack footRotation;
		footRotation.stableTargetId = 4U;
		footRotation.kind = AnimationClipTrackKind::Rotation;
		footRotation.interpolation = AnimationInterpolation::Linear;
		footRotation.keys = {
			RotationKey(0, {0.0F, 0.0F, 0.0F, 1.0F}),
			RotationKey(30, {0.0F, 0.0F, 0.08715574F, 0.99619472F}),
			RotationKey(60, {0.0F, 0.0F, 0.0F, 1.0F}),
			RotationKey(90, {0.0F, 0.0F, -0.08715574F, 0.99619472F}),
			RotationKey(120, {0.0F, 0.0F, 0.0F, 1.0F})};

		AnimationSourceTrack handRotation = footRotation;
		handRotation.stableTargetId = 6U;

		AnimationSourceTrack speedCurve;
		speedCurve.stableTargetId = 5001U;
		speedCurve.kind = AnimationClipTrackKind::Curve;
		speedCurve.interpolation = AnimationInterpolation::Linear;
		speedCurve.keys = {
			ScalarKey(0, 0.0F),
			ScalarKey(60, 2.0F),
			ScalarKey(120, 4.0F)};

		AnimationSourceTrack rootTranslation;
		rootTranslation.stableTargetId = 1U;
		rootTranslation.kind = AnimationClipTrackKind::RootTranslation;
		rootTranslation.interpolation = AnimationInterpolation::Linear;
		rootTranslation.keys = {
			VectorKey(0, {0.0F, 0.0F, 0.0F}),
			VectorKey(60, {0.6F, 0.0F, 0.0F}),
			VectorKey(120, {1.2F, 0.0F, 0.0F})};

		AnimationSourceTrack rootYaw;
		rootYaw.stableTargetId = 1U;
		rootYaw.kind = AnimationClipTrackKind::RootYaw;
		rootYaw.interpolation = AnimationInterpolation::Linear;
		rootYaw.keys = {
			ScalarKey(0, 0.0F),
			ScalarKey(60, 0.1F),
			ScalarKey(120, 0.2F)};

		input.tracks = {
			hipsTranslation, footRotation, handRotation,
			speedCurve, rootTranslation, rootYaw};
		input.namedChannels = {{5001U, "RecordedSpeed"}};
		input.events = {
			{6001U, 30, AnimationEventPayloadType::String,
				"LeftFootPlant", true, true},
			{6002U, 90, AnimationEventPayloadType::String,
				"RightFootPlant", true, true}};
		return input;
	}

	AnimationClipCompileInput MakeTargetRecoveryInput(const SkeletonAsset& target) {
		AnimationClipCompileInput input;
		input.assetId = MakeFixtureGuid(FixtureGuidKind::TargetRecoveryClip);
		input.skeletonAssetId = target.assetId;
		input.compatibilityFingerprint = target.compatibilityFingerprint;
		input.durationTicks = 60;
		input.sampleRate = 60U;
		input.segmentDurationTicks = 30;

		AnimationSourceTrack hipsRotation;
		hipsRotation.stableTargetId = 102U;
		hipsRotation.kind = AnimationClipTrackKind::Rotation;
		hipsRotation.interpolation = AnimationInterpolation::Linear;
		hipsRotation.keys = {
			RotationKey(0, {0.0F, 0.0F, 0.70710677F, 0.70710677F}),
			RotationKey(30, {0.0F, 0.0F, 0.38268343F, 0.92387950F}),
			RotationKey(60, {0.0F, 0.0F, 0.0F, 1.0F})};

		AnimationSourceTrack recoveryCurve;
		recoveryCurve.stableTargetId = 5101U;
		recoveryCurve.kind = AnimationClipTrackKind::Curve;
		recoveryCurve.interpolation = AnimationInterpolation::Linear;
		recoveryCurve.keys = {
			ScalarKey(0, 0.0F),
			ScalarKey(30, 0.5F),
			ScalarKey(60, 1.0F)};

		AnimationSourceTrack rootTranslation;
		rootTranslation.stableTargetId = 101U;
		rootTranslation.kind = AnimationClipTrackKind::RootTranslation;
		rootTranslation.interpolation = AnimationInterpolation::Linear;
		rootTranslation.keys = {
			VectorKey(0, {0.0F, 0.0F, 0.0F}),
			VectorKey(30, {0.25F, 0.0F, 0.0F}),
			VectorKey(60, {0.5F, 0.0F, 0.0F})};

		AnimationSourceTrack rootYaw;
		rootYaw.stableTargetId = 101U;
		rootYaw.kind = AnimationClipTrackKind::RootYaw;
		rootYaw.interpolation = AnimationInterpolation::Linear;
		rootYaw.keys = {
			ScalarKey(0, 0.0F),
			ScalarKey(30, 0.0F),
			ScalarKey(60, 0.0F)};

		input.tracks = {hipsRotation, recoveryCurve, rootTranslation, rootYaw};
		input.namedChannels = {{5101U, "RecoveryPhase"}};
		input.events = {
			{6101U, 55, AnimationEventPayloadType::String,
				"RecoveryReady", true, false}};
		return input;
	}

	AnimationClipAsset CompileClipDeterministically(
		const AnimationClipCompileInput& input) {
		const AnimationClipCompilerSettings settings = FixtureClipCompilerSettings();
		AnimationClipAsset first;
		AnimationClipAsset second;
		AnimationClipCompileReport firstReport;
		AnimationClipCompileReport secondReport;
		RequireStatus(CompileAnimationClip(input, settings, first, firstReport),
			"CompileAnimationClip(first)");
		RequireStatus(CompileAnimationClip(input, settings, second, secondReport),
			"CompileAnimationClip(second)");
		Require(first == second,
			"Identical normalized clip input must produce the same cooked asset.");
		Require(firstReport.sourceKeyCount == secondReport.sourceKeyCount
				&& firstReport.retainedKeyCount == secondReport.retainedKeyCount
				&& firstReport.removedKeyCount == secondReport.removedKeyCount,
			"Clip compiler reports must be deterministic for the fixture input.");
		return first;
	}

	void AddRecordingProvenance(AnimationClipAsset& clip) {
		AnimationClipRecordingProvenance provenance;
		provenance.sourceAnimatorId =
			MakeFixtureGuid(FixtureGuidKind::RecordingSourceAnimator);
		provenance.sourceSceneIdentity = "AdvancedAssets/AdvancedAnimationFixture.tescene";
		provenance.sourceSessionIdentity = "animation-production-validation-deterministic-recording";
		provenance.sourceFirstTick = 0;
		provenance.sourceLastTick = clip.durationTicks;
		provenance.sourceSampleRate = clip.sampleRate;
		provenance.clock = AnimationClipRecordingClock::FixedSimulation;
		provenance.capturedSampleCount = 3U;
		provenance.capturedEventCount = clip.events.size();
		provenance.sourceKeyCount = 3U;
		provenance.retainedKeyCount = 3U;
		provenance.removedKeyCount = 0U;
		provenance.reduction.translationToleranceMeters = 0.0005F;
		provenance.reduction.rotationToleranceDegrees = 0.1F;
		provenance.reduction.scaleToleranceFraction = 0.0005F;
		provenance.reduction.morphTolerance = 0.0005F;
		provenance.reduction.curveTolerance = 0.0005F;
		provenance.channels = {{
			5001U,
			5001U,
			"RecordedSpeed",
			AnimationClipRecordedChannelKind::Curve,
			AnimationInterpolation::Linear}};
		clip.recordingProvenance = std::move(provenance);
	}

	bool ApproximatelyEqual(float left, float right, float tolerance) noexcept {
		return std::abs(left - right) <= tolerance;
	}

	std::vector<std::uint8_t> SerializeClipChecked(
		const AnimationClipAsset& clip,
		const SkeletonAsset& expectedSkeleton,
		AnimationFixedTick sampleTick,
		std::uint64_t rootJointId,
		float expectedRootX,
		std::uint64_t curveId,
		float expectedCurve,
		bool expectRecording) {
		RequireStatus(ValidateAnimationClipAsset(clip), "ValidateAnimationClipAsset");
		Require(clip.skeletonAssetId == expectedSkeleton.assetId
				&& clip.compatibilityFingerprint
					== expectedSkeleton.compatibilityFingerprint,
			"A fixture clip must bind the exact expected skeleton identity.");
		Require(clip.rootMotion.enabled,
			"Both fixture clips need production root-motion tracks.");
		Require(clip.recordingProvenance.has_value() == expectRecording,
			"Only the source locomotion clip should carry recording provenance.");

		AnimationCodecView codec;
		RequireStatus(codec.Open(clip), "AnimationCodecView::Open");
		AnimationDecodedValue rootValue;
		RequireStatus(codec.Decode(rootJointId,
			AnimationClipTrackKind::RootTranslation,
			sampleTick, false, rootValue), "Decode root translation");
		Require(ApproximatelyEqual(rootValue.vectorValue.x, expectedRootX, 0.0006F),
			"Decoded root translation must match the authored sample within contract tolerance.");
		AnimationDecodedValue curveValue;
		RequireStatus(codec.Decode(curveId, AnimationClipTrackKind::Curve,
			sampleTick, false, curveValue), "Decode fixture curve");
		Require(ApproximatelyEqual(curveValue.scalarValue, expectedCurve, 0.0006F),
			"Decoded curve must match the authored sample within contract tolerance.");

		std::vector<std::uint8_t> first;
		std::vector<std::uint8_t> second;
		RequireStatus(SerializeAnimationClipAsset(clip, first),
			"SerializeAnimationClipAsset(first)");
		RequireStatus(SerializeAnimationClipAsset(clip, second),
			"SerializeAnimationClipAsset(second)");
		Require(first == second,
			"Identical cooked clips must serialize to byte-identical files.");
		AnimationClipAsset decoded;
		RequireStatus(DeserializeAnimationClipAsset(
			first, expectedSkeleton.compatibilityFingerprint, decoded),
			"DeserializeAnimationClipAsset");
		RequireStatus(ValidateAnimationClipAsset(decoded),
			"Validate decoded AnimationClipAsset");
		Require(decoded.assetId == clip.assetId
				&& decoded.skeletonAssetId == expectedSkeleton.assetId
				&& decoded.events == clip.events
				&& decoded.recordingProvenance == clip.recordingProvenance,
			"Clip identity, skeleton, events, and provenance must survive round-trip decode.");
		std::vector<std::uint8_t> roundTrip;
		RequireStatus(SerializeAnimationClipAsset(decoded, roundTrip),
			"SerializeAnimationClipAsset(round-trip)");
		Require(roundTrip == first,
			"Decoded clip data must serialize back to the exact same bytes.");
		return first;
	}

	RetargetProfileAsset MakeRetargetProfile(
		const SkeletonAsset& source,
		const SkeletonAsset& target) {
		RetargetProfileAsset profile;
		profile.assetId = MakeFixtureGuid(FixtureGuidKind::RetargetProfile);
		profile.compatibilityFingerprint = kRetargetProfileFingerprint;
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
		// OptionalTail has no target peer. An explicit, finite fallback keeps the
		// profile publishable while making the reduced-fidelity choice observable.
		profile.explicitMappings = {{7U, 2U, 0U, 50U, 7U, 999U, false}};
		profile.chainMappings = {
			{10U, "LeftLeg", {2U, 3U, 4U}, {102U, 103U, 105U},
				RetargetTranslationScaleMode::PerChainLength, 1.0F, true},
			{11U, "LeftArm", {5U, 6U}, {106U, 108U},
				RetargetTranslationScaleMode::PerChainLength, 1.0F, true}};
		profile.orientationBases = {{
			10U, 5U, 106U, {},
			{0.0F, 0.0F, 0.70710677F, 0.70710677F}}};
		profile.proportion.sourceReferenceHeightMeters = 1.0F;
		profile.proportion.targetReferenceHeightMeters = 1.5F;
		profile.root = {
			1U, 101U, RetargetRootMotionMode::ScaleByProportion,
			true, true, 1.0F, 0.0F};
		profile.contacts = {{
			20U, 4U, 105U, 10U,
			kRetargetDefaultContactToleranceMeters, true, true, true}};
		profile.twistRules = {{
			30U, 5U, 6U, {1.0F, 0.0F, 0.0F}, {107U}, {1.0F}}};
		profile.fallbackRules = {{
			50U, 7U, 0U, RetargetFallbackMode::ReferencePose, {108U}, true}};
		profile.quality.tier = RetargetQualityTier::Verified;
		profile.quality.authoredCoverage = 0.9F;
		profile.quality.expectedMeanPositionErrorMeters = 0.003F;
		profile.quality.expectedMaximumPositionErrorMeters = 0.010F;
		profile.quality.expectedMaximumRotationErrorRadians = 0.01F;
		profile.quality.validationProfileHash = 0x043a000000007101ULL;
		profile.quality.reviewReason =
			"Reviewed synthetic humanoid retarget fixture for animation production validation";
		return profile;
	}

	std::vector<std::uint8_t> SerializeRetargetChecked(
		const RetargetProfileAsset& profile,
		const SkeletonAsset& source,
		const SkeletonAsset& target,
		std::uint64_t& compiledHash) {
		RequireStatus(ValidateRetargetProfileAssetStructure(profile),
			"ValidateRetargetProfileAssetStructure");
		RetargetValidationResult validation;
		RetargetCompiledProfile compiled;
		RequireStatus(CompileRetargetProfile(
			profile, source, target, validation, compiled),
			"CompileRetargetProfile");
		Require(validation.publishable
				&& validation.metrics.requiredCoverage == 1.0F
				&& validation.metrics.maximumContactToleranceMeters
					<= kRetargetDefaultContactToleranceMeters
				&& compiled.mappings.size() == 6U
				&& compiled.chains.size() == 2U
				&& compiled.stableHash != 0U,
			"The fixture retarget profile must be quantitatively publishable.");
		compiledHash = compiled.stableHash;

		std::vector<std::uint8_t> first;
		std::vector<std::uint8_t> second;
		RequireStatus(SerializeRetargetProfileAsset(profile, first),
			"SerializeRetargetProfileAsset(first)");
		RequireStatus(SerializeRetargetProfileAsset(profile, second),
			"SerializeRetargetProfileAsset(second)");
		Require(first == second,
			"Identical retarget profiles must serialize to byte-identical files.");
		RetargetProfileAsset decoded;
		RequireStatus(DeserializeRetargetProfileAsset(
			first, profile.compatibilityFingerprint, decoded),
			"DeserializeRetargetProfileAsset");
		RetargetValidationResult decodedValidation;
		RetargetCompiledProfile decodedCompiled;
		RequireStatus(CompileRetargetProfile(
			decoded, source, target, decodedValidation, decodedCompiled),
			"Compile decoded RetargetProfileAsset");
		Require(decodedValidation.publishable
				&& decodedCompiled.stableHash == compiled.stableHash,
			"Round-trip retarget decode must preserve publishable compiled semantics.");
		std::vector<std::uint8_t> roundTrip;
		RequireStatus(SerializeRetargetProfileAsset(decoded, roundTrip),
			"SerializeRetargetProfileAsset(round-trip)");
		Require(roundTrip == first,
			"Decoded retarget data must serialize back to the exact same bytes.");
		return first;
	}

	MotionDatabaseCompileInput MakeMotionDatabaseInput(
		const AnimationClipAsset& sourceClip,
		std::span<const std::uint8_t> sourceClipBytes,
		std::uint64_t& sourceClipId,
		std::uint64_t& sourceClipFingerprint) {
		MotionDatabaseCompileInput input;
		input.assetId = MakeFixtureGuid(FixtureGuidKind::MotionDatabase);
		input.compatibilityFingerprint = kSourceSkeletonFingerprint;
		sourceClipId = StableIdFromGuid(sourceClip.assetId);
		sourceClipFingerprint = Fnv1a64(sourceClipBytes);
		Require(sourceClipId != 0U && sourceClipFingerprint != 0U,
			"Motion database source identities must be nonzero.");
		input.sourceFingerprint = sourceClipFingerprint;
		input.samplesPerBlock = kMotionDatabaseSamplesPerBlock;

		MotionDatabaseSourceClipInput clip;
		clip.sourceClipId = sourceClipId;
		clip.sourceClipFingerprint = sourceClipFingerprint;
		clip.baseTagMask = 0x1U;
		clip.contactRanges = {
			{0, 30, 0x1U},
			{60, 90, 0x2U}};
		clip.tagRanges = {{60, 90, 0x4U}};
		clip.exclusionRanges = {{90, 90, 0x8U}};
		constexpr std::array<AnimationFixedTick, 4U> ticks{0, 30, 60, 90};
		for(std::size_t sampleIndex = 0U; sampleIndex < ticks.size(); ++sampleIndex) {
			const AnimationFixedTick tick = ticks[sampleIndex];
			MotionDatabaseClipSampleInput sample;
			sample.sourceTick = tick;
			sample.phaseQuantized =
				static_cast<std::uint16_t>(sampleIndex * 16384U);
			sample.transitionFlags = sampleIndex == 3U ? 0x2U : 0x1U;
			sample.continuityGroup = 77U;
			for(std::size_t horizon = 0U; horizon < sample.trajectory.size(); ++horizon) {
				sample.trajectory[horizon].positionX =
					static_cast<float>(tick) * 0.01F
					+ static_cast<float>(horizon) * 0.25F;
				sample.trajectory[horizon].positionZ =
					static_cast<float>(horizon) * 0.10F;
				sample.trajectory[horizon].facingX =
					static_cast<float>(sampleIndex) * 0.05F;
				sample.trajectory[horizon].facingZ = 1.0F;
			}
			for(std::size_t point = 0U; point < sample.posePoints.size(); ++point) {
				sample.posePoints[point].x =
					static_cast<float>(sampleIndex) * 0.1F;
				sample.posePoints[point].y =
					static_cast<float>(point) * 0.05F;
				sample.posePoints[point].z =
					static_cast<float>(tick) * 0.002F;
			}
			for(std::size_t point = 0U; point < sample.velocityPoints.size(); ++point) {
				sample.velocityPoints[point].x =
					0.5F + static_cast<float>(sampleIndex) * 0.1F;
				sample.velocityPoints[point].z =
					static_cast<float>(point) * 0.2F;
			}
			sample.facing = {
				static_cast<float>(sampleIndex) * 0.05F, 0.0F, 1.0F};
			clip.samples.push_back(sample);
		}
		input.sourceClips.push_back(std::move(clip));
		return input;
	}

	MotionDatabaseAsset CompileMotionDatabaseDeterministically(
		const MotionDatabaseCompileInput& input,
		std::uint64_t expectedSourceClipId,
		std::uint64_t& contentFingerprint) {
		MotionDatabaseAsset first;
		MotionDatabaseAsset second;
		MotionDatabaseCompileReport firstReport;
		MotionDatabaseCompileReport secondReport;
		RequireStatus(CompileMotionDatabase(input, first, firstReport),
			"CompileMotionDatabase(first)");
		RequireStatus(CompileMotionDatabase(input, second, secondReport),
			"CompileMotionDatabase(second)");
		Require(first == second
				&& firstReport.contentFingerprint == secondReport.contentFingerprint,
			"Identical structured motion input must produce the same cooked database.");
		Require(first.samples.size() == 4U && first.residentBlocks.size() == 2U,
			"The advanced fixture needs four samples in two streamable blocks.");
		Require(std::all_of(first.samples.begin(), first.samples.end(),
			[expectedSourceClipId](const MotionDatabaseSample& sample) {
				return sample.sourceClipId == expectedSourceClipId;
			}), "Every motion sample must reference the source locomotion clip identity.");
		const auto sampleAt = [&](AnimationFixedTick tick) {
			return std::find_if(first.samples.begin(), first.samples.end(),
				[tick](const MotionDatabaseSample& sample) {
					return sample.sourceTick == tick;
				});
		};
		const auto tick0 = sampleAt(0);
		const auto tick30 = sampleAt(30);
		const auto tick60 = sampleAt(60);
		const auto tick90 = sampleAt(90);
		Require(tick0 != first.samples.end() && tick30 != first.samples.end()
				&& tick60 != first.samples.end() && tick90 != first.samples.end()
				&& tick0->continuitySuccessorId == tick30->stableSampleId
				&& tick90->continuityPredecessorId == tick60->stableSampleId,
			"The compiler must preserve deterministic locomotion continuity edges.");
		RequireStatus(ValidateMotionDatabaseAsset(first),
			"ValidateMotionDatabaseAsset");
		contentFingerprint = firstReport.contentFingerprint;
		return first;
	}

	std::vector<std::uint8_t> SerializeMotionDatabaseChecked(
		const MotionDatabaseAsset& database) {
		std::vector<std::uint8_t> first;
		std::vector<std::uint8_t> second;
		RequireStatus(SerializeMotionDatabaseAsset(database, first),
			"SerializeMotionDatabaseAsset(first)");
		RequireStatus(SerializeMotionDatabaseAsset(database, second),
			"SerializeMotionDatabaseAsset(second)");
		Require(first == second,
			"Identical motion databases must serialize to byte-identical files.");
		MotionDatabaseAsset decoded;
		RequireStatus(DeserializeMotionDatabaseAsset(
			first, database.compatibilityFingerprint, decoded),
			"DeserializeMotionDatabaseAsset");
		Require(decoded == database,
			"Every structured motion sample and resident block must round-trip.");
		AnimationAssetReader reader;
		RequireStatus(reader.Parse(first), "Parse motion database envelope");
		const std::span<const std::uint8_t> stream =
			reader.FindChunk(kAnimationChunkStreamData);
		Require(!stream.empty(), "A structured motion database must include STRM bytes.");
		RequireStatus(ValidateMotionDatabaseStreamChunk(decoded, stream),
			"ValidateMotionDatabaseStreamChunk");
		std::vector<MotionDatabaseSample> blockSamples;
		RequireStatus(DecodeMotionDatabaseStreamBlock(
			decoded, 0U, stream.first(
				static_cast<std::size_t>(decoded.residentBlocks[0U].cpuByteSize)),
			blockSamples), "DecodeMotionDatabaseStreamBlock");
		Require(blockSamples.size() == decoded.residentBlocks[0U].sampleCount
				&& std::equal(blockSamples.begin(), blockSamples.end(),
					decoded.samples.begin()),
			"Decoded motion stream block must match authoritative DATA samples.");
		std::vector<std::uint8_t> roundTrip;
		RequireStatus(SerializeMotionDatabaseAsset(decoded, roundTrip),
			"SerializeMotionDatabaseAsset(round-trip)");
		Require(roundTrip == first,
			"Decoded motion database data must serialize back to the exact same bytes.");
		return first;
	}

	RagdollColliderRecord CapsuleCollider(
		std::uint64_t colliderId,
		float radius,
		float cylinderHalfHeight) {
		RagdollColliderRecord collider;
		collider.colliderId = colliderId;
		collider.shape = RagdollColliderShape::Capsule;
		collider.dimensions = {radius, cylinderHalfHeight, radius};
		collider.friction = 0.6F;
		return collider;
	}

	RagdollBodyRecord Body(
		std::uint64_t bodyId,
		std::uint64_t jointId,
		float mass,
		RagdollColliderRecord collider) {
		RagdollBodyRecord body;
		body.bodyId = bodyId;
		body.stableJointId = jointId;
		body.colliders.push_back(std::move(collider));
		body.massKilograms = mass;
		body.collisionLayer = 0U;
		body.collisionMask = 0x1U;
		body.linearDamping = 0.1F;
		body.angularDamping = 0.1F;
		return body;
	}

	RagdollConstraintRecord Constraint(
		std::uint64_t constraintId,
		std::uint64_t parentBodyId,
		std::uint64_t childBodyId) {
		RagdollConstraintRecord constraint;
		constraint.constraintId = constraintId;
		constraint.parentBodyId = parentBodyId;
		constraint.childBodyId = childBodyId;
		RagdollAxisLimit& angularLimit = constraint.limits[
			static_cast<std::size_t>(RagdollConstraintAxis::AngularX)];
		angularLimit.mode = RagdollLimitMode::Limited;
		angularLimit.minimum = -0.6F;
		angularLimit.maximum = 0.6F;
		RagdollAxisMotor& motor = constraint.motors[
			static_cast<std::size_t>(RagdollConstraintAxis::AngularX)];
		motor.mode = RagdollMotorMode::Position;
		motor.stiffness = 20.0F;
		motor.damping = 4.0F;
		motor.maximumForce = 500.0F;
		return constraint;
	}

	RagdollProfileAsset MakeRagdollProfile(
		const SkeletonAsset& target,
		const AnimationClipAsset& recoveryClip) {
		RagdollProfileAsset profile;
		profile.assetId = MakeFixtureGuid(FixtureGuidKind::RagdollProfile);
		profile.skeletonAssetId = target.assetId;
		profile.skeletonFingerprint = target.compatibilityFingerprint;
		profile.referencePoseHash = target.referencePoseHash;
		profile.physicsProviderFingerprint = kPhysicsProviderFingerprint;
		profile.rootBodyId = 100U;
		profile.coveragePolicy = RagdollCoveragePolicy::RequiredJointSet;
		profile.requiredBodyJointStableIds = {
			101U, 102U, 103U, 105U, 106U, 108U};
		profile.expectedTotalMassKilograms = 45.0F;
		profile.expectedMassToleranceFraction = 0.001F;
		profile.minimumBodyMassFraction = 0.05F;
		profile.maximumBodyMassFraction = 0.40F;
		profile.allowedCollisionLayerMask = 0x1U;
		profile.bodies = {
			Body(100U, 101U, 12.0F, CapsuleCollider(1000U, 0.18F, 0.20F)),
			Body(200U, 102U, 10.0F, CapsuleCollider(2000U, 0.22F, 0.35F)),
			Body(300U, 103U, 8.0F, CapsuleCollider(3000U, 0.14F, 0.30F)),
			Body(400U, 105U, 5.0F, CapsuleCollider(4000U, 0.12F, 0.18F)),
			Body(500U, 106U, 6.0F, CapsuleCollider(5000U, 0.12F, 0.25F)),
			Body(600U, 108U, 4.0F, CapsuleCollider(6000U, 0.10F, 0.16F))};
		profile.constraints = {
			Constraint(10000U, 100U, 200U),
			Constraint(10001U, 200U, 300U),
			Constraint(10002U, 300U, 400U),
			Constraint(10003U, 200U, 500U),
			Constraint(10004U, 500U, 600U)};
		profile.blends = {
			{101U, 1.0F, RagdollBoundaryPolicy::KinematicAnchor},
			{102U, 0.85F, RagdollBoundaryPolicy::MotorToAnimation},
			{103U, 1.0F, RagdollBoundaryPolicy::MatchAnimation},
			{105U, 1.0F, RagdollBoundaryPolicy::MatchAnimation},
			{106U, 0.75F, RagdollBoundaryPolicy::MotorToAnimation},
			{108U, 1.0F, RagdollBoundaryPolicy::MatchAnimation}};
		RagdollRecoveryRecord recovery;
		recovery.candidateId = 20000U;
		recovery.clipAssetId = recoveryClip.assetId;
		recovery.requiredContactMask = 0x1U;
		recovery.clipGeneration = 1U;
		recovery.clipSkeletonFingerprint = target.compatibilityFingerprint;
		recovery.requiredReferencePoseHash = target.referencePoseHash;
		recovery.rootJointStableId = 101U;
		recovery.contactJointStableIds = {105U, 108U};
		recovery.posture = RagdollRecoveryPosture::FaceUp;
		profile.recovery.push_back(recovery);
		return profile;
	}

	RagdollProfileValidationReport ValidateRagdoll(
		const RagdollProfileAsset& profile,
		const SkeletonAsset& target) {
		const std::array<RagdollCollisionLayerRule, 1U> collisionLayers{{
			{0U, 0x1U, true}}};
		RagdollProfileValidationOptions options;
		options.collisionLayers = collisionLayers;
		return ValidateRagdollProfile(profile, target, options);
	}

	std::vector<std::uint8_t> SerializeRagdollChecked(
		const RagdollProfileAsset& profile,
		const SkeletonAsset& target) {
		RequireStatus(ValidateRagdollProfileAssetStructure(profile),
			"ValidateRagdollProfileAssetStructure");
		const RagdollProfileValidationReport validation =
			ValidateRagdoll(profile, target);
		Require(validation.IsValid()
				&& validation.rootBodyIds == std::vector<std::uint64_t>{100U}
				&& validation.totalMassKilograms == profile.expectedTotalMassKilograms,
			"The primitive-collider ragdoll must pass full skeleton and physics-policy validation.");

		std::vector<std::uint8_t> first;
		std::vector<std::uint8_t> second;
		RequireStatus(SerializeRagdollProfileAsset(profile, first),
			"SerializeRagdollProfileAsset(first)");
		RequireStatus(SerializeRagdollProfileAsset(profile, second),
			"SerializeRagdollProfileAsset(second)");
		Require(first == second,
			"Identical ragdoll profiles must serialize to byte-identical files.");
		RagdollProfileAsset decoded;
		RequireStatus(DeserializeRagdollProfileAsset(
			first, target.compatibilityFingerprint, decoded),
			"DeserializeRagdollProfileAsset");
		const RagdollProfileValidationReport decodedValidation =
			ValidateRagdoll(decoded, target);
		Require(decodedValidation.IsValid()
				&& decodedValidation.totalMassKilograms
					== validation.totalMassKilograms,
			"Round-trip ragdoll decode must preserve full validator semantics.");
		std::vector<std::uint8_t> roundTrip;
		RequireStatus(SerializeRagdollProfileAsset(decoded, roundTrip),
			"SerializeRagdollProfileAsset(round-trip)");
		Require(roundTrip == first,
			"Decoded ragdoll data must serialize back to the exact same bytes.");
		return first;
	}

	AnimationAssetReader ReadEnvelope(const GeneratedFile& file) {
		AnimationAssetReader reader;
		RequireStatus(reader.Parse(file.bytes),
			std::string("Parse envelope for ") + file.fileName);
		Require(reader.Envelope().assetId == file.assetId
				&& reader.Envelope().compatibilityFingerprint
					== file.compatibilityFingerprint,
			"Manifest identity must match the production animation envelope.");
		return reader;
	}

	std::string JsonQuote(std::string_view value) {
		std::ostringstream escaped;
		escaped << '"';
		for(const unsigned char byte : value) {
			switch(byte) {
			case '"': escaped << "\\\""; break;
			case '\\': escaped << "\\\\"; break;
			case '\b': escaped << "\\b"; break;
			case '\f': escaped << "\\f"; break;
			case '\n': escaped << "\\n"; break;
			case '\r': escaped << "\\r"; break;
			case '\t': escaped << "\\t"; break;
			default:
				if(byte < 0x20U) {
					escaped << "\\u" << std::hex << std::setfill('0')
						<< std::setw(4) << static_cast<unsigned int>(byte)
						<< std::dec;
				} else {
					escaped << static_cast<char>(byte);
				}
				break;
			}
		}
		escaped << '"';
		return escaped.str();
	}

	std::string BuildManifest(const FixtureBundle& bundle) {
		std::ostringstream json;
		json << "{\n"
			<< "\t\"schemaVersion\": 1,\n"
			<< "\t\"representation\": \"production-cooked-animation-assets\",\n"
			<< "\t\"generator\": \"Tools/Animation/AdvancedAnimationFixtureMaterializer.cpp\",\n"
			<< "\t\"deterministic\": true,\n"
			<< "\t\"files\": [\n";
		for(std::size_t index = 0U; index < bundle.files.size(); ++index) {
			const GeneratedFile& file = bundle.files[index];
			const AnimationAssetReader envelope = ReadEnvelope(file);
			json << "\t\t{\n"
				<< "\t\t\t\"file\": " << JsonQuote(file.fileName) << ",\n"
				<< "\t\t\t\"assetType\": " << JsonQuote(file.assetType) << ",\n"
				<< "\t\t\t\"assetGuidHex\": " << JsonQuote(GuidHex(file.assetId)) << ",\n"
				<< "\t\t\t\"compatibilityFingerprint\": "
				<< JsonQuote(Hex64(file.compatibilityFingerprint)) << ",\n"
				<< "\t\t\t\"envelopeContentHash\": "
				<< JsonQuote(HexBytes(envelope.Envelope().contentHash)) << ",\n"
				<< "\t\t\t\"sha256\": " << JsonQuote(Sha256Hex(file.bytes)) << ",\n"
				<< "\t\t\t\"sizeBytes\": " << file.bytes.size() << ",\n"
				<< "\t\t\t\"envelopeDependencies\": [";
			const std::span<const AnimationDependencyEntry> dependencies =
				envelope.Dependencies();
			if(!dependencies.empty()) {
				json << '\n';
				for(std::size_t dependencyIndex = 0U;
					dependencyIndex < dependencies.size(); ++dependencyIndex) {
					const AnimationDependencyEntry& dependency =
						dependencies[dependencyIndex];
					json << "\t\t\t\t{\n"
						<< "\t\t\t\t\t\"assetGuidHex\": "
						<< JsonQuote(GuidHex(dependency.assetId)) << ",\n"
						<< "\t\t\t\t\t\"expectedAssetType\": "
						<< dependency.expectedAssetType << ",\n"
						<< "\t\t\t\t\t\"expectedGeneration\": "
						<< dependency.expectedGeneration << "\n"
						<< "\t\t\t\t}"
						<< (dependencyIndex + 1U == dependencies.size() ? "\n" : ",\n");
				}
				json << "\t\t\t";
			}
			json << "]\n"
				<< "\t\t}" << (index + 1U == bundle.files.size() ? "\n" : ",\n");
		}
		json << "\t],\n"
			<< "\t\"relationships\": {\n"
			<< "\t\t\"sourceSkeleton\": {\n"
			<< "\t\t\t\"assetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(FixtureGuidKind::SourceSkeleton))) << ",\n"
			<< "\t\t\t\"compatibilityFingerprint\": "
			<< JsonQuote(Hex64(kSourceSkeletonFingerprint)) << ",\n"
			<< "\t\t\t\"referencePoseHash\": "
			<< JsonQuote(Hex64(kSourceReferencePoseHash)) << "\n"
			<< "\t\t},\n"
			<< "\t\t\"targetSkeleton\": {\n"
			<< "\t\t\t\"assetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(FixtureGuidKind::TargetSkeleton))) << ",\n"
			<< "\t\t\t\"compatibilityFingerprint\": "
			<< JsonQuote(Hex64(kTargetSkeletonFingerprint)) << ",\n"
			<< "\t\t\t\"referencePoseHash\": "
			<< JsonQuote(Hex64(kTargetReferencePoseHash)) << "\n"
			<< "\t\t},\n"
			<< "\t\t\"sourceLocomotionClip\": {\n"
			<< "\t\t\t\"assetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(
				FixtureGuidKind::SourceLocomotionClip))) << ",\n"
			<< "\t\t\t\"skeletonAssetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(FixtureGuidKind::SourceSkeleton))) << ",\n"
			<< "\t\t\t\"recordingProvenance\": true\n"
			<< "\t\t},\n"
			<< "\t\t\"targetRecoveryClip\": {\n"
			<< "\t\t\t\"assetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(
				FixtureGuidKind::TargetRecoveryClip))) << ",\n"
			<< "\t\t\t\"skeletonAssetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(FixtureGuidKind::TargetSkeleton))) << "\n"
			<< "\t\t},\n"
			<< "\t\t\"retargetProfile\": {\n"
			<< "\t\t\t\"assetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(FixtureGuidKind::RetargetProfile))) << ",\n"
			<< "\t\t\t\"sourceSkeletonAssetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(FixtureGuidKind::SourceSkeleton))) << ",\n"
			<< "\t\t\t\"targetSkeletonAssetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(FixtureGuidKind::TargetSkeleton))) << ",\n"
			<< "\t\t\t\"compiledStableHash\": "
			<< JsonQuote(Hex64(bundle.retargetCompiledHash)) << "\n"
			<< "\t\t},\n"
			<< "\t\t\"motionDatabase\": {\n"
			<< "\t\t\t\"assetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(FixtureGuidKind::MotionDatabase))) << ",\n"
			<< "\t\t\t\"sourceClipAssetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(
				FixtureGuidKind::SourceLocomotionClip))) << ",\n"
			<< "\t\t\t\"sourceClipId\": "
			<< JsonQuote(Hex64(bundle.motionSourceClipId)) << ",\n"
			<< "\t\t\t\"sourceClipFingerprintFnv1a64\": "
			<< JsonQuote(Hex64(bundle.motionSourceClipFingerprint)) << ",\n"
			<< "\t\t\t\"contentFingerprint\": "
			<< JsonQuote(Hex64(bundle.motionContentFingerprint)) << "\n"
			<< "\t\t},\n"
			<< "\t\t\"ragdollProfile\": {\n"
			<< "\t\t\t\"assetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(FixtureGuidKind::RagdollProfile))) << ",\n"
			<< "\t\t\t\"skeletonAssetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(FixtureGuidKind::TargetSkeleton))) << ",\n"
			<< "\t\t\t\"recoveryClipAssetGuidHex\": "
			<< JsonQuote(GuidHex(MakeFixtureGuid(
				FixtureGuidKind::TargetRecoveryClip))) << ",\n"
			<< "\t\t\t\"physicsProviderFingerprint\": "
			<< JsonQuote(Hex64(kPhysicsProviderFingerprint)) << "\n"
			<< "\t\t}\n"
			<< "\t},\n"
			<< "\t\"validation\": [\n"
			<< "\t\t\"Each writer was invoked twice and produced byte-identical output.\",\n"
			<< "\t\t\"Each checked-in file was decoded and reserialized to identical bytes.\",\n"
			<< "\t\t\"Both clips were sampled through AnimationCodecView within authored tolerances.\",\n"
			<< "\t\t\"The retarget profile passed CompileRetargetProfile with complete required coverage.\",\n"
			<< "\t\t\"The motion database passed DATA and STRM validation and block decode.\",\n"
			<< "\t\t\"The ragdoll profile passed structure and full skeleton/physics-policy validation.\"\n"
			<< "\t],\n"
			<< "\t\"limitations\": [\n"
			<< "\t\t\"Primitive ragdoll collider recipes need no external geometry asset; Jolt body creation remains a live runtime operation.\",\n"
			<< "\t\t\"IK goals are runtime/controller commands rather than a standalone cooked asset and are intentionally outside this codec bundle.\"\n"
			<< "\t]\n"
			<< "}\n";
		return json.str();
	}

	void ValidateDependencyCounts(const FixtureBundle& bundle) {
		for(const GeneratedFile& file : bundle.files) {
			const AnimationAssetReader reader = ReadEnvelope(file);
			std::size_t expected = 0U;
			if(file.assetType == "RetargetProfile") {
				expected = 2U;
			} else if(file.assetType == "RagdollProfile") {
				expected = 2U;
			}
			Require(reader.Dependencies().size() == expected,
				"Envelope dependency count does not match the fixture relationship contract.");
		}
	}

	FixtureBundle BuildFixtureBundle() {
		// A known-answer test catches accidental mistakes in the local SHA-256
		// implementation before it can write incorrect pins to manifest.json.
		const std::array<std::uint8_t, 0U> empty{};
		Require(Sha256Hex(empty)
				== "e3b0c44298fc1c149afbf4c8996fb924"
					"27ae41e4649b934ca495991b7852b855",
			"Local SHA-256 known-answer self-test failed.");

		FixtureBundle bundle;
		const SkeletonAsset sourceSkeleton = MakeSourceSkeleton();
		const SkeletonAsset targetSkeleton = MakeTargetSkeleton();
		const std::vector<std::uint8_t> sourceSkeletonBytes =
			SerializeSkeletonChecked(sourceSkeleton);
		const std::vector<std::uint8_t> targetSkeletonBytes =
			SerializeSkeletonChecked(targetSkeleton);

		AnimationClipAsset sourceClip =
			CompileClipDeterministically(MakeSourceLocomotionInput(sourceSkeleton));
		AddRecordingProvenance(sourceClip);
		const std::vector<std::uint8_t> sourceClipBytes = SerializeClipChecked(
			sourceClip, sourceSkeleton, 60, 1U, 0.6F, 5001U, 2.0F, true);
		const AnimationClipAsset recoveryClip =
			CompileClipDeterministically(MakeTargetRecoveryInput(targetSkeleton));
		const std::vector<std::uint8_t> recoveryClipBytes = SerializeClipChecked(
			recoveryClip, targetSkeleton, 30, 101U, 0.25F, 5101U, 0.5F, false);

		const RetargetProfileAsset retarget =
			MakeRetargetProfile(sourceSkeleton, targetSkeleton);
		const std::vector<std::uint8_t> retargetBytes = SerializeRetargetChecked(
			retarget, sourceSkeleton, targetSkeleton, bundle.retargetCompiledHash);

		std::uint64_t motionSourceClipId = 0U;
		std::uint64_t motionSourceClipFingerprint = 0U;
		const MotionDatabaseCompileInput motionInput = MakeMotionDatabaseInput(
			sourceClip, sourceClipBytes,
			motionSourceClipId, motionSourceClipFingerprint);
		const MotionDatabaseAsset motionDatabase =
			CompileMotionDatabaseDeterministically(
				motionInput, motionSourceClipId, bundle.motionContentFingerprint);
		const std::vector<std::uint8_t> motionDatabaseBytes =
			SerializeMotionDatabaseChecked(motionDatabase);
		bundle.motionSourceClipId = motionSourceClipId;
		bundle.motionSourceClipFingerprint = motionSourceClipFingerprint;

		const RagdollProfileAsset ragdoll =
			MakeRagdollProfile(targetSkeleton, recoveryClip);
		const std::vector<std::uint8_t> ragdollBytes =
			SerializeRagdollChecked(ragdoll, targetSkeleton);

		bundle.files = {
			{"source-humanoid.teskel", "Skeleton", sourceSkeleton.assetId,
				sourceSkeleton.compatibilityFingerprint, sourceSkeletonBytes},
			{"target-humanoid.teskel", "Skeleton", targetSkeleton.assetId,
				targetSkeleton.compatibilityFingerprint, targetSkeletonBytes},
			{"source-locomotion.teanim", "AnimationClip", sourceClip.assetId,
				sourceClip.compatibilityFingerprint, sourceClipBytes},
			{"target-recovery.teanim", "AnimationClip", recoveryClip.assetId,
				recoveryClip.compatibilityFingerprint, recoveryClipBytes},
			{"humanoid-retarget.teretarget", "RetargetProfile", retarget.assetId,
				retarget.compatibilityFingerprint, retargetBytes},
			{"locomotion.temotiondb", "MotionDatabase", motionDatabase.assetId,
				motionDatabase.compatibilityFingerprint, motionDatabaseBytes},
			{"target-humanoid.teragdoll", "RagdollProfile", ragdoll.assetId,
				ragdoll.skeletonFingerprint, ragdollBytes}};
		ValidateDependencyCounts(bundle);
		bundle.manifest = BuildManifest(bundle);
		return bundle;
	}

	std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
		std::error_code error;
		const std::uintmax_t size = std::filesystem::file_size(path, error);
		if(error || size > std::numeric_limits<std::size_t>::max()) {
			Fail(std::string("Unable to determine file size for ") + path.string());
		}
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
		std::ifstream stream(path, std::ios::binary);
		if(!stream) {
			Fail(std::string("Unable to open file for reading: ") + path.string());
		}
		if(!bytes.empty()) {
			stream.read(reinterpret_cast<char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
		}
		if(!stream) {
			Fail(std::string("Unable to read complete file: ") + path.string());
		}
		return bytes;
	}

	void WriteFile(
		const std::filesystem::path& path,
		std::span<const std::uint8_t> bytes) {
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if(!stream) {
			Fail(std::string("Unable to open file for writing: ") + path.string());
		}
		if(!bytes.empty()) {
			stream.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
		}
		stream.close();
		if(!stream) {
			Fail(std::string("Unable to write complete file: ") + path.string());
		}
	}

	std::vector<std::uint8_t> StringBytes(const std::string& value) {
		return {
			reinterpret_cast<const std::uint8_t*>(value.data()),
			reinterpret_cast<const std::uint8_t*>(value.data() + value.size())};
	}

	void VerifyCheckedInBytes(
		const std::filesystem::path& outputDirectory,
		const FixtureBundle& bundle) {
		for(const GeneratedFile& file : bundle.files) {
			const std::filesystem::path path = outputDirectory / file.fileName;
			Require(ReadFile(path) == file.bytes,
				std::string("Checked-in bytes differ from deterministic output: ")
					+ file.fileName);
		}
		const std::vector<std::uint8_t> expectedManifest =
			StringBytes(bundle.manifest);
		Require(ReadFile(outputDirectory / "manifest.json") == expectedManifest,
			"Checked-in manifest.json differs from deterministic output.");
	}

	void WriteFixtureBundle(
		const std::filesystem::path& outputDirectory,
		const FixtureBundle& bundle) {
		std::error_code error;
		std::filesystem::create_directories(outputDirectory, error);
		if(error) {
			Fail(std::string("Unable to create fixture directory: ")
				+ outputDirectory.string());
		}
		for(const GeneratedFile& file : bundle.files) {
			WriteFile(outputDirectory / file.fileName, file.bytes);
		}
		const std::vector<std::uint8_t> manifestBytes =
			StringBytes(bundle.manifest);
		WriteFile(outputDirectory / "manifest.json", manifestBytes);
		VerifyCheckedInBytes(outputDirectory, bundle);
	}

	void PrintSummary(const FixtureBundle& bundle, bool verificationOnly) {
		std::cout << (verificationOnly ? "Verified" : "Materialized")
			<< " production advanced-animation fixtures:\n";
		for(const GeneratedFile& file : bundle.files) {
			std::cout << "  " << file.fileName
				<< "  bytes=" << file.bytes.size()
				<< "  sha256=" << Sha256Hex(file.bytes) << '\n';
		}
		std::cout << "  manifest.json  bytes=" << bundle.manifest.size()
			<< "  sha256=" << Sha256Hex(StringBytes(bundle.manifest)) << '\n';
	}
}

int main(int argc, char** argv) {
	try {
		if(argc < 2 || argc > 3) {
			std::cerr
				<< "Usage: AdvancedAnimationFixtureMaterializer "
				<< "<output-directory> [--verify-only]\n";
			return 2;
		}
		const bool verificationOnly =
			argc == 3 && std::string_view(argv[2]) == "--verify-only";
		if(argc == 3 && !verificationOnly) {
			Fail("The only supported optional argument is --verify-only.");
		}
		const std::filesystem::path outputDirectory =
			std::filesystem::path(argv[1]).lexically_normal();
		Require(!outputDirectory.empty(),
			"Output directory must not be empty.");
		const FixtureBundle bundle = BuildFixtureBundle();
		if(verificationOnly) {
			VerifyCheckedInBytes(outputDirectory, bundle);
		} else {
			WriteFixtureBundle(outputDirectory, bundle);
		}
		PrintSummary(bundle, verificationOnly);
		return 0;
	} catch(const std::exception& error) {
		std::cerr << "Advanced fixture materialization failed: "
			<< error.what() << '\n';
		return 1;
	}
}
