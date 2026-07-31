// AnimationAssetFormatTests.cpp - Concrete cooked animation asset contracts.

#include "TestHarness.h"

#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Assets/MotionDatabaseAsset.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Animation/Codec/AnimationCodec.h"
#include "File Formats/TerraEngineAnimationFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationGuid MakeAssetGuid(std::uint8_t seed) {
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		AnimationTransform MakeTransform(float x, float y, float z) {
			AnimationTransform transform;
			transform.translation = {x, y, z};
			transform.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
			transform.scale = {1.0F, 1.0F, 1.0F};
			return transform;
		}

		AnimationMatrix4x4 MakeIdentityMatrix() {
			AnimationMatrix4x4 matrix{};
			matrix.values[0U] = 1.0F;
			matrix.values[5U] = 1.0F;
			matrix.values[10U] = 1.0F;
			matrix.values[15U] = 1.0F;
			return matrix;
		}

		std::uint64_t AppendConstantCodecTrack(
			std::vector<std::uint8_t>& bytes,
			AnimationClipTrackKind kind,
			AnimationFixedTick segmentStart,
			AnimationFixedTick segmentEnd) {
			// Runtime validation opens the same codec view used by playback. Build a
			// minimal real constant-track record so this format fixture proves both
			// the binary envelope and the source-independent decoder boundary.
			AnimationCodecTrackHeaderV1 header;
			header.flags = kAnimationCodecConstantTrack;
			header.byteSize = sizeof(header);
			header.keyCount = 1U;
			header.segmentStartTick = segmentStart;
			header.segmentEndTick = segmentEnd;
			switch(kind) {
			case AnimationClipTrackKind::Rotation:
				header.encoding = AnimationCodecValueEncoding::QuaternionSmallestThree16;
				header.constantRotation = {0.0F, 0.0F, 0.0F, 1.0F};
				break;
			case AnimationClipTrackKind::Translation:
			case AnimationClipTrackKind::Scale:
			case AnimationClipTrackKind::RootTranslation:
				header.encoding = AnimationCodecValueEncoding::Vector3Unorm16;
				header.constantVector = {0.0F, 0.0F, 0.0F};
				break;
			case AnimationClipTrackKind::MorphWeight:
			case AnimationClipTrackKind::Curve:
			case AnimationClipTrackKind::RootYaw:
				header.encoding = AnimationCodecValueEncoding::ScalarUnorm16;
				header.constantScalar = 0.0F;
				break;
			}

			const std::uint64_t offset = bytes.size();
			const std::size_t oldSize = bytes.size();
			bytes.resize(oldSize + sizeof(header));
			std::memcpy(bytes.data() + oldSize, &header, sizeof(header));
			return offset;
		}

		std::filesystem::path WriteCookedAsset(
			std::string_view fileName,
			std::span<const std::uint8_t> bytes) {
			const std::filesystem::path root = std::filesystem::temp_directory_path()
				/ "TerraEngineAnimationAssetFormatTests";
			std::filesystem::create_directories(root);
			const std::filesystem::path path = root / fileName;
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			return path;
		}

		SkeletonAsset MakeSkeletonAsset(bool reverseSockets = false) {
			SkeletonAsset asset;
			asset.assetId = MakeAssetGuid(10U);
			asset.compatibilityFingerprint = 0x1122334455667788ULL;
			asset.rootJointIndex = 0U;
			asset.referencePoseHash = 0xa0b0c0d0e0f00112ULL;
			asset.source.unitScaleMeters = 0.01F;
			asset.source.upAxis = SkeletonSourceAxis::PositiveY;
			asset.source.forwardAxis = SkeletonSourceAxis::PositiveZ;
			asset.source.sourcePackagePath = "Characters/Hero.fbx";

			// Display names may collide. Stable identifiers and normalized semantic
			// paths, rather than these labels, preserve references across reimport.
			asset.joints.push_back(SkeletonJoint{
				100U, -1, MakeTransform(0.0F, 0.0F, 0.0F), true,
				"Root", "/root", "hips"});
			asset.joints.push_back(SkeletonJoint{
				200U, 0, MakeTransform(0.0F, 1.0F, 0.0F), true,
				"Hand", "/root/left_hand", "left_hand"});
			asset.joints.push_back(SkeletonJoint{
				300U, 0, MakeTransform(0.0F, 1.0F, 0.0F), true,
				"Hand", "/root/right_hand", "right_hand"});
			asset.inverseBindMatrices.assign(asset.joints.size(), MakeIdentityMatrix());

			SkeletonSocket weaponSocket{
				900U, 200U, MakeTransform(0.1F, 0.0F, 0.0F), "Weapon", "weapon_grip"};
			SkeletonSocket cameraSocket{
				800U, 100U, MakeTransform(0.0F, 1.7F, 0.0F), "Camera", "camera_anchor"};
			if(reverseSockets) {
				asset.sockets = {weaponSocket, cameraSocket};
			} else {
				asset.sockets = {cameraSocket, weaponSocket};
			}
			return asset;
		}

		AnimationClipAsset MakeClipAsset(bool reverseTables = false) {
			AnimationClipAsset asset;
			asset.assetId = MakeAssetGuid(40U);
			asset.skeletonAssetId = MakeAssetGuid(10U);
			asset.compatibilityFingerprint = 0x1122334455667788ULL;
			asset.durationTicks = 120;
			asset.sampleRate = 60U;
			asset.compression.decoderVersion = 1U;
			asset.compression.translationErrorMeters = 0.0005F;
			asset.compression.rotationErrorDegrees = 0.1F;
			asset.compression.scaleErrorFraction = 0.0005F;
			asset.compression.morphError = 0.0005F;
			const std::uint64_t translationOffset = AppendConstantCodecTrack(
				asset.streamData, AnimationClipTrackKind::Translation, 0, 60);
			const std::uint64_t rotationOffset = AppendConstantCodecTrack(
				asset.streamData, AnimationClipTrackKind::Rotation, 0, 60);
			const std::uint64_t firstSegmentSize = asset.streamData.size();
			const std::uint64_t secondSegmentOffset = firstSegmentSize;
			const std::uint64_t morphOffset = AppendConstantCodecTrack(
				asset.streamData, AnimationClipTrackKind::MorphWeight, 60, 120);
			const std::uint64_t curveOffset = AppendConstantCodecTrack(
				asset.streamData, AnimationClipTrackKind::Curve, 60, 120);
			asset.segments = {
				AnimationClipSegment{0, 60, 0U, firstSegmentSize, 0U, 2U},
				AnimationClipSegment{60, 120, secondSegmentOffset,
					asset.streamData.size() - secondSegmentOffset, 2U, 2U}
			};

			AnimationClipTrack translation{
				200U, AnimationClipTrackKind::Translation, AnimationInterpolation::Linear,
				0U, translationOffset, 1U, kAnimationCodecVectorBits};
			AnimationClipTrack rotation{
				200U, AnimationClipTrackKind::Rotation, AnimationInterpolation::Cubic,
				0U, rotationOffset, 1U, kAnimationCodecQuaternionBits};
			AnimationClipTrack morph{
				700U, AnimationClipTrackKind::MorphWeight, AnimationInterpolation::Step,
				1U, morphOffset, 1U, kAnimationCodecScalarBits};
			AnimationClipTrack curve{
				600U, AnimationClipTrackKind::Curve, AnimationInterpolation::Linear,
				1U, curveOffset, 1U, kAnimationCodecScalarBits};
			asset.tracks = reverseTables
				? std::vector<AnimationClipTrack>{curve, morph, rotation, translation}
				: std::vector<AnimationClipTrack>{translation, rotation, morph, curve};

			asset.namedChannels = {
				AnimationClipNamedChannel{600U, "Speed"},
				AnimationClipNamedChannel{700U, "Smile"}
			};
			AnimationClipEvent leftFoot{1100U, 15, AnimationEventPayloadType::String, "LeftFoot", true, true};
			AnimationClipEvent rightFoot{1200U, 75, AnimationEventPayloadType::String, "RightFoot", true, true};
			asset.events = reverseTables
				? std::vector<AnimationClipEvent>{rightFoot, leftFoot}
				: std::vector<AnimationClipEvent>{leftFoot, rightFoot};
			asset.rootMotion.enabled = true;
			asset.rootMotion.translationTrackIndex = 0U;
			asset.rootMotion.yawTrackIndex = 1U;
			asset.rootMotion.accumulatedCycleTranslation = {1.0F, 0.0F, 0.0F};
			asset.rootMotion.accumulatedCycleYawRadians = 0.25F;
			return asset;
		}

		template<typename Header, typename Mutator>
		bool MutateConcretePayload(std::vector<std::uint8_t>& bytes, Mutator&& mutate) {
			if(bytes.size() < sizeof(AnimationAssetEnvelope)) {
				return false;
			}
			AnimationAssetEnvelope envelope{};
			std::memcpy(&envelope, bytes.data(), sizeof(envelope));
			if(envelope.chunkTableOffset > bytes.size()) {
				return false;
			}
			for(std::uint32_t index = 0U; index < envelope.chunkCount; ++index) {
				AnimationChunkEntry chunk{};
				const std::size_t entryOffset = static_cast<std::size_t>(envelope.chunkTableOffset)
					+ static_cast<std::size_t>(index) * sizeof(AnimationChunkEntry);
				if(entryOffset > bytes.size() || bytes.size() - entryOffset < sizeof(chunk)) {
					return false;
				}
				std::memcpy(&chunk, bytes.data() + entryOffset, sizeof(chunk));
				if(chunk.type != kAnimationChunkPayload || chunk.offset > bytes.size()
					|| bytes.size() - static_cast<std::size_t>(chunk.offset) < sizeof(Header)) {
					continue;
				}
				Header header{};
				std::memcpy(&header, bytes.data() + static_cast<std::size_t>(chunk.offset), sizeof(header));
				mutate(header);
				std::memcpy(bytes.data() + static_cast<std::size_t>(chunk.offset), &header, sizeof(header));
				envelope.contentHash = ComputeAnimationContentHash(
					std::span<const std::uint8_t>(bytes).subspan(sizeof(AnimationAssetEnvelope)));
				std::memcpy(bytes.data(), &envelope, sizeof(envelope));
				return true;
			}
			return false;
		}

		std::vector<std::uint8_t> RemoveMotionDatabaseStreamChunk(
			std::span<const std::uint8_t> serializedAsset) {
			AnimationAssetReader reader;
			if(reader.Parse(serializedAsset) != AnimationStatus::Ok) return {};
			const std::span<const std::uint8_t> payload =
				reader.FindChunk(kAnimationChunkPayload);
			if(payload.empty()) return {};

			// Rebuild the same checked envelope with only its required DATA chunk.
			// This models a valid pre-STRM v1 artifact without mutating private format
			// structures or weakening the production reader's content-hash checks.
			AnimationAssetWriter writer;
			if(!writer.AddChunk(kAnimationChunkPayload, true, payload)) return {};
			AnimationAssetEnvelope envelope;
			envelope.assetId = reader.Envelope().assetId;
			envelope.compatibilityFingerprint =
				reader.Envelope().compatibilityFingerprint;
			std::vector<std::uint8_t> legacyBytes;
			return writer.Finish(envelope, legacyBytes) == AnimationStatus::Ok
				? legacyBytes : std::vector<std::uint8_t>{};
		}
	}

	void RegisterAnimationAssetFormatTests(TestRegistry& registry) {
		registry.Add("Animation.AssetFormat.SkeletonAndClipRoundTripAllCoreTables", [](TestContext& context) {
			const SkeletonAsset skeleton = MakeSkeletonAsset();
			std::vector<std::uint8_t> skeletonBytes;
			context.Expect(SerializeSkeletonAsset(skeleton, skeletonBytes) == AnimationStatus::Ok,
				"A valid skeleton should serialize into the common checked animation envelope.");
			SkeletonAsset restoredSkeleton;
			context.Expect(DeserializeSkeletonAsset(skeletonBytes, skeleton.compatibilityFingerprint, restoredSkeleton)
				== AnimationStatus::Ok,
				"A matching skeleton compatibility fingerprint should permit publication.");
			context.Expect(restoredSkeleton == skeleton,
				"Skeleton hierarchy, bind pose, inverse binds, semantic identity, sockets, and source metadata must round-trip.");
			context.Expect(restoredSkeleton.joints[1U].displayName == restoredSkeleton.joints[2U].displayName
				&& restoredSkeleton.joints[1U].stableId != restoredSkeleton.joints[2U].stableId,
				"Duplicate display names must remain distinct through stable semantic joint identity.");

			const AnimationClipAsset clip = MakeClipAsset();
			std::vector<std::uint8_t> clipBytes;
			context.Expect(SerializeAnimationClipAsset(clip, clipBytes) == AnimationStatus::Ok,
				"A valid segmented clip should serialize without source-file dependencies.");
			AnimationClipAsset restoredClip;
			context.Expect(DeserializeAnimationClipAsset(clipBytes, clip.compatibilityFingerprint, restoredClip)
				== AnimationStatus::Ok,
				"A clip should load when its skeleton compatibility fingerprint matches.");
			context.Expect(restoredClip == clip,
				"Clip segments, joint/curve/morph tracks, events, root motion, compression, names, and stream bytes must round-trip.");
			context.Expect(std::any_of(restoredClip.tracks.begin(), restoredClip.tracks.end(), [](const AnimationClipTrack& track) {
				return track.kind == AnimationClipTrackKind::MorphWeight;
			}), "Morph target streams are part of the concrete clip binary contract.");

			const std::filesystem::path skeletonPath = WriteCookedAsset("runtime-loader.teskel", skeletonBytes);
			const std::filesystem::path clipPath = WriteCookedAsset("runtime-loader.teanim", clipBytes);
			ResourceManager& resources = ResourceManager::Instance();
			const AnimationResourceValidationResult skeletonValidation = resources.ValidateSkeletonAssetFile(
				skeletonPath, skeleton.compatibilityFingerprint);
			const AnimationResourceValidationResult clipValidation = resources.ValidateAnimationClipAssetFile(
				clipPath, clip.compatibilityFingerprint);
			context.Expect(skeletonValidation.status == AnimationStatus::Ok
				&& skeletonValidation.compatibilityFingerprintPresent
				&& skeletonValidation.jointCount == skeleton.joints.size(),
				"The source-independent runtime skeleton validator must report the cooked fingerprint and joint count.");
			context.Expect(clipValidation.status == AnimationStatus::Ok
				&& clipValidation.compatibilityFingerprintPresent
				&& clipValidation.clipSegmentCount == clip.segments.size(),
				"The source-independent runtime clip validator must report the cooked fingerprint and segment count.");
			const TerraEngine::UUID loadedSkeletonId = resources.LoadSkeleton(
				skeletonPath, skeleton.compatibilityFingerprint);
			const TerraEngine::UUID loadedClipId = resources.LoadAnimationClip(
				clipPath, clip.compatibilityFingerprint);
			context.Expect(loadedSkeletonId != TerraEngine::EMPTY_UUID
				&& loadedClipId != TerraEngine::EMPTY_UUID
				&& resources.GetSkeleton(loadedSkeletonId) != nullptr
				&& resources.GetAnimationClip(loadedClipId) != nullptr,
				"Runtime loaders must cache source-free cooked skeleton and clip assets by their embedded identities.");
			SkeletonAsset publishedSkeleton;
			AnimationClipAsset publishedClip;
			AnimationGeneration publishedSkeletonGeneration = 0U;
			AnimationGeneration publishedClipGeneration = 0U;
			context.Expect(resources.CopySkeletonPublication(
				loadedSkeletonId, publishedSkeleton, publishedSkeletonGeneration)
				&& resources.CopyAnimationClipPublication(
					loadedClipId, publishedClip, publishedClipGeneration)
				&& publishedSkeletonGeneration == resources.GetAnimationAssetRuntimeGeneration(
					loadedSkeletonId, AssetType::Skeleton)
				&& publishedClipGeneration == resources.GetAnimationAssetRuntimeGeneration(
					loadedClipId, AssetType::AnimationClip)
				&& publishedSkeleton.runtimeGeneration == publishedSkeletonGeneration
				&& publishedClip.runtimeGeneration == publishedClipGeneration,
				"Cooked runtime copies must carry the exact cache publication generation observed under the cache lock.");
			resources.DeleteResource(loadedClipId, AssetType::AnimationClip);
			resources.DeleteResource(loadedSkeletonId, AssetType::Skeleton);
		});

		registry.Add("Animation.AssetFormat.DeterministicBytesIgnoreCanonicalTableInputOrder", [](TestContext& context) {
			std::vector<std::uint8_t> skeletonForward;
			std::vector<std::uint8_t> skeletonReverse;
			SerializeSkeletonAsset(MakeSkeletonAsset(false), skeletonForward);
			SerializeSkeletonAsset(MakeSkeletonAsset(true), skeletonReverse);
			context.Expect(skeletonForward == skeletonReverse,
				"Socket insertion order must not change canonical cooked skeleton bytes.");

			std::vector<std::uint8_t> clipForward;
			std::vector<std::uint8_t> clipReverse;
			SerializeAnimationClipAsset(MakeClipAsset(false), clipForward);
			SerializeAnimationClipAsset(MakeClipAsset(true), clipReverse);
			context.Expect(clipForward == clipReverse,
				"Track and event insertion order must not change canonical cooked clip bytes.");
		});

		registry.Add("Animation.AssetFormat.UpgradeMalformedRangesAndAtomicOutput", [](TestContext& context) {
			std::vector<std::uint8_t> legacyBytes;
			const SkeletonAsset source = MakeSkeletonAsset();
			SerializeSkeletonAsset(source, legacyBytes);
			context.Expect(MutateConcretePayload<SkeletonAssetFileHeaderV1>(legacyBytes, [](SkeletonAssetFileHeaderV1& header) {
				header.formatVersion = kSkeletonAssetLegacyVersion;
			}), "The test must locate the concrete skeleton payload inside the common envelope.");
			SkeletonAsset upgraded;
			context.Expect(DeserializeSkeletonAsset(legacyBytes, source.compatibilityFingerprint, upgraded)
				== AnimationStatus::Ok && upgraded.formatVersion == kSkeletonAssetCurrentVersion,
				"A supported legacy skeleton payload should migrate to the current in-memory version.");

			std::vector<std::uint8_t> malformedBytes;
			SerializeSkeletonAsset(source, malformedBytes);
			context.Expect(MutateConcretePayload<SkeletonAssetFileHeaderV1>(malformedBytes, [](SkeletonAssetFileHeaderV1& header) {
				header.jointTableOffset = std::numeric_limits<std::uint64_t>::max() - 1U;
			}), "The malformed fixture should modify only the concrete checked range.");
			SkeletonAsset unchanged = MakeSkeletonAsset();
			unchanged.referencePoseHash = 55U;
			const SkeletonAsset before = unchanged;
			context.Expect(DeserializeSkeletonAsset(malformedBytes, source.compatibilityFingerprint, unchanged)
				== AnimationStatus::RangeOverflow,
				"A concrete table offset overflow must be rejected before allocation or access.");
			context.Expect(unchanged == before,
				"A rejected concrete asset must not partially replace the caller's last-known-good value.");
		});

		registry.Add("Animation.AssetFormat.CompatibilityMismatchRejectsPublication", [](TestContext& context) {
			const SkeletonAsset skeleton = MakeSkeletonAsset();
			std::vector<std::uint8_t> skeletonBytes;
			SerializeSkeletonAsset(skeleton, skeletonBytes);
			SkeletonAsset output;
			context.Expect(DeserializeSkeletonAsset(skeletonBytes, 0xdeadbeefULL, output)
				== AnimationStatus::IncompatibleAsset,
				"A skeleton must not bind to a consumer expecting a different compatibility fingerprint.");

			const AnimationClipAsset clip = MakeClipAsset();
			std::vector<std::uint8_t> clipBytes;
			SerializeAnimationClipAsset(clip, clipBytes);
			AnimationClipAsset clipOutput;
			context.Expect(DeserializeAnimationClipAsset(clipBytes, 0xdeadbeefULL, clipOutput)
				== AnimationStatus::IncompatibleAsset,
				"A clip must not publish against an incompatible skeleton generation.");
		});

		registry.Add("Animation.AssetFormat.PreparedCacheBatchRejectsStaleMemberWithoutPartialSwap", [](TestContext& context) {
			SkeletonAsset skeletonV1 = MakeSkeletonAsset();
			skeletonV1.assetId = MakeAssetGuid(80U);
			AnimationClipAsset clipV1 = MakeClipAsset();
			clipV1.assetId = MakeAssetGuid(100U);
			clipV1.skeletonAssetId = skeletonV1.assetId;
			std::vector<std::uint8_t> skeletonBytes;
			std::vector<std::uint8_t> clipBytes;
			context.Expect(SerializeSkeletonAsset(skeletonV1, skeletonBytes) == AnimationStatus::Ok
				&& SerializeAnimationClipAsset(clipV1, clipBytes) == AnimationStatus::Ok,
				"The atomic cache fixture must serialize its complete first generation.");
			const std::filesystem::path skeletonPath =
				WriteCookedAsset("prepared-batch.teskel", skeletonBytes);
			const std::filesystem::path clipPath =
				WriteCookedAsset("prepared-batch.teanim", clipBytes);
			ResourceManager& resources = ResourceManager::Instance();
			const UUID skeletonId = resources.LoadSkeleton(
				skeletonPath, skeletonV1.compatibilityFingerprint);
			const UUID clipId = resources.LoadAnimationClip(
				clipPath, clipV1.compatibilityFingerprint);
			const AnimationGeneration firstSkeletonGeneration =
				resources.GetAnimationAssetRuntimeGeneration(skeletonId, AssetType::Skeleton);
			const AnimationGeneration firstClipGeneration =
				resources.GetAnimationAssetRuntimeGeneration(clipId, AssetType::AnimationClip);

			SkeletonAsset skeletonV2 = skeletonV1;
			skeletonV2.source.sourcePackagePath = "Characters/HeroReloaded.fbx";
			AnimationClipAsset clipV2 = clipV1;
			clipV2.events.front().payload = "LeftFootReloaded";
			context.Expect(SerializeSkeletonAsset(skeletonV2, skeletonBytes) == AnimationStatus::Ok
				&& SerializeAnimationClipAsset(clipV2, clipBytes) == AnimationStatus::Ok,
				"The replacement generation must remain a valid compatible pair.");
			(void)WriteCookedAsset("prepared-batch.teskel", skeletonBytes);
			(void)WriteCookedAsset("prepared-batch.teanim", clipBytes);
			const AnimationResourceValidationResult stagedSkeletonValidation =
				resources.ValidateSkeletonAssetFile(skeletonPath, skeletonV2.compatibilityFingerprint);
			auto samePrefixDifferentContentHash = stagedSkeletonValidation.contentHash;
			// Preserve the diagnostic uint64 prefix while changing the rest of the
			// authoritative 256-bit identity. The on-disk replacement remains a valid
			// checked asset, so only the complete transaction hash can reject it.
			samePrefixDifferentContentHash[8U] ^= 0x5AU;

			const std::array reloadRequests{
				AnimationAssetReloadRequest{skeletonId, AssetType::Skeleton},
				AnimationAssetReloadRequest{clipId, AssetType::AnimationClip}};
			const std::array mismatchedPublisherRequests{
				AnimationAssetReloadRequest{skeletonId, AssetType::Skeleton,
					skeletonV2.compatibilityFingerprint,
					stagedSkeletonValidation.contentFingerprint, 0U,
					samePrefixDifferentContentHash, true}};
			context.Expect(!resources.PrepareAnimationAssetReloadBatch(
				mismatchedPublisherRequests).Accepted(),
				"A valid same-ID replacement with the same 64-bit prefix but a different complete content hash must be rejected before cache publication.");
			PreparedAnimationAssetReloadBatch staleBatch =
				resources.PrepareAnimationAssetReloadBatch(reloadRequests);
			context.Expect(staleBatch.Accepted(),
				"Both checked replacements should stage without changing either live cache.");
			context.Expect(resources.ReloadSkeleton(skeletonId),
				"The fixture must create one real interleaving generation after preparation.");
			context.Expect(!resources.CommitPreparedAnimationAssetReloadBatch(staleBatch),
				"A changed member generation must reject the whole prepared cache batch.");
			context.Expect(resources.GetAnimationAssetRuntimeGeneration(
					skeletonId, AssetType::Skeleton) == firstSkeletonGeneration + 1U
				&& resources.GetAnimationAssetRuntimeGeneration(
					clipId, AssetType::AnimationClip) == firstClipGeneration,
				"Batch rejection must not advance the untouched clip after the deliberate skeleton interleaving.");
			AnimationClipAsset retainedClip;
			AnimationGeneration retainedClipGeneration = 0U;
			context.Expect(resources.CopyAnimationClipPublication(
					clipId, retainedClip, retainedClipGeneration)
				&& retainedClipGeneration == firstClipGeneration
				&& retainedClip.events.front().payload == clipV1.events.front().payload,
				"The rejected batch must preserve the clip's complete prior payload/generation pair.");

			PreparedAnimationAssetReloadBatch retry =
				resources.PrepareAnimationAssetReloadBatch(reloadRequests);
			const std::size_t retiredPayloadBaseline =
				resources.GetRetiredAnimationPayloadCountForDiagnostics();
			context.Expect(retry.Accepted()
				&& resources.CommitPreparedAnimationAssetReloadBatch(retry),
				"Re-preparing from current generations should atomically publish both replacements.");
			resources.FinalizeCommittedAnimationAssetReloadBatch(retry);
			context.Expect(resources.GetRetiredAnimationPayloadCountForDiagnostics()
					== retiredPayloadBaseline,
				"Direct finalization must reclaim the exact cache generations replaced by the batch.");
			SkeletonAsset publishedSkeleton;
			AnimationClipAsset publishedClip;
			AnimationGeneration publishedSkeletonGeneration = 0U;
			AnimationGeneration publishedClipGeneration = 0U;
			context.Expect(resources.CopySkeletonPublication(
					skeletonId, publishedSkeleton, publishedSkeletonGeneration)
				&& resources.CopyAnimationClipPublication(
					clipId, publishedClip, publishedClipGeneration)
				&& publishedSkeletonGeneration == firstSkeletonGeneration + 2U
				&& publishedClipGeneration == firstClipGeneration + 1U
				&& publishedSkeleton.source.sourcePackagePath == skeletonV2.source.sourcePackagePath
				&& publishedClip.events.front().payload == clipV2.events.front().payload,
				"A successful retry must expose both complete replacement payloads at their next cache generations.");
			resources.DeleteResource(clipId, AssetType::AnimationClip);
			resources.DeleteResource(skeletonId, AssetType::Skeleton);
		});

		registry.Add("Animation.AssetFormat.MotionDatabasePublishedRangesUseExactCachedStrmGeneration", [](TestContext& context) {
			const bool initializedPool = ThreadPool::instance().getThreadCount() == 0U;
			if(initializedPool) ThreadPool::instance().initialize(1U);

			MotionDatabaseAsset database;
			database.assetId = MakeAssetGuid(120U);
			database.compatibilityFingerprint = 0x1234567812345678ULL;
			database.sourceFingerprint = 0x8765432187654321ULL;
			database.quantizationScales.fill(1.0F);
			database.storageLayout.samplesPerBlock = 1U;
			for(std::uint64_t sampleId = 1U; sampleId <= 2U; ++sampleId) {
				MotionDatabaseSample sample;
				sample.stableSampleId = sampleId;
				sample.sourceClipId = 100U + sampleId;
				sample.sourceTick = static_cast<AnimationFixedTick>(sampleId * 10U);
				database.samples.push_back(sample);
			}
			context.Expect(RebuildMotionDatabaseStorageMetadata(database)
					== AnimationStatus::Ok,
				"The range fixture must build two checked one-sample resident blocks.");
			database.featureSchemaFingerprint =
				ComputeMotionDatabaseSchemaFingerprint(database);
			std::vector<std::uint8_t> currentBytes;
			context.Expect(SerializeMotionDatabaseAsset(database, currentBytes)
					== AnimationStatus::Ok,
				"Current motion-database serialization must emit a checked STRM chunk.");

			std::array<MotionDatabaseStreamBlockRange, 2U> expectedRanges;
			context.Expect(ResolveMotionDatabaseStreamBlockRange(
					currentBytes, 0U, expectedRanges[0U]) == AnimationStatus::Ok
				&& ResolveMotionDatabaseStreamBlockRange(
					currentBytes, 1U, expectedRanges[1U]) == AnimationStatus::Ok,
				"The format-owned resolver must expose exact physical ranges for both blocks.");

			const std::filesystem::path currentPath =
				WriteCookedAsset("motion-ranges-current.temotiondb", currentBytes);
			ResourceManager& resources = ResourceManager::Instance();
			const UUID currentId = resources.LoadAnimationAssetAsync(
				currentPath, AssetType::MotionDatabase).get();
			const AnimationGeneration currentGeneration =
				resources.GetAnimationAssetRuntimeGeneration(
					currentId, AssetType::MotionDatabase);
			const AnimationCookedRangeTable currentRanges =
				resources.DescribeMotionDatabaseBlockRanges(
					currentId, currentGeneration);
			context.Expect(currentId == database.assetId.value
				&& currentRanges.status == AnimationCookedRangeStatus::Ok
				&& currentRanges.blocks.size() == expectedRanges.size(),
				"The published generation must expose every bounded current STRM block.");
			for(std::size_t index = 0U;
				index < currentRanges.blocks.size() && index < expectedRanges.size(); ++index) {
				context.Expect(currentRanges.blocks[index].blockIndex == index
					&& currentRanges.blocks[index].byteOffset
						== expectedRanges[index].absoluteByteOffset
					&& currentRanges.blocks[index].byteSize
						== expectedRanges[index].byteSize,
					"ResourceManager ranges must exactly match the checked STRM file intervals.");
			}
			context.Expect(resources.DescribeMotionDatabaseBlockRanges(
					currentId, currentGeneration + 1U).status
					== AnimationCookedRangeStatus::StaleGeneration,
				"A range request for an unpublished generation must fail before exposing bytes.");
			resources.DeleteResource(currentId, AssetType::MotionDatabase);

			const std::vector<std::uint8_t> legacyBytes =
				RemoveMotionDatabaseStreamChunk(currentBytes);
			const std::filesystem::path legacyPath =
				WriteCookedAsset("motion-ranges-legacy.temotiondb", legacyBytes);
			const UUID legacyId = resources.LoadAnimationAssetAsync(
				legacyPath, AssetType::MotionDatabase).get();
			const AnimationGeneration legacyGeneration =
				resources.GetAnimationAssetRuntimeGeneration(
					legacyId, AssetType::MotionDatabase);
			const AnimationCookedRangeTable legacyRanges =
				resources.DescribeMotionDatabaseBlockRanges(
					legacyId, legacyGeneration);
			context.Expect(!legacyBytes.empty() && legacyId == database.assetId.value
				&& legacyRanges.status == AnimationCookedRangeStatus::UnsupportedLayout
				&& legacyRanges.blocks.empty(),
				"A valid DATA-only v1 database must retain safe complete-data fallback without fabricating ranges.");
			resources.DeleteResource(legacyId, AssetType::MotionDatabase);

			std::error_code error;
			std::filesystem::remove(currentPath, error);
			std::filesystem::remove(legacyPath, error);
			if(initializedPool) ThreadPool::instance().shutdown();
		});

		registry.Add("Animation.AssetFormat.TemfProductionHeaderRemainsBinaryStable", [](TestContext& context) {
			// Animation sidecars may reference a mesh generation, but production
			// TEMF never serializes the retired runtime-layout header.
			TemfProductionHeader header{};
			header.assetIdentity = 4096U;
			header.generationIdentity = 3U;
			std::array<std::uint8_t, sizeof(TemfProductionHeader)> bytes{};
			std::memcpy(bytes.data(), &header, sizeof(header));
			TemfProductionHeader restored{};
			std::memcpy(&restored, bytes.data(), sizeof(restored));
			context.Expect(restored.formatVersion == kMeshCurrentVersion
				&& restored.headerBytes == kTemfProductionHeaderBytes
				&& restored.assetIdentity == header.assetIdentity
				&& restored.generationIdentity == header.generationIdentity,
				"TEMFPRD1 keeps its immutable production root stable for animation compatibility checks.");
		});
	}
}
