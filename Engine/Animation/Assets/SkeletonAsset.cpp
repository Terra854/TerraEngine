// SkeletonAsset.cpp - Deterministic binary serialization for cooked skeletons.

#include "Animation/Assets/SkeletonAsset.h"

#include "File Formats/TerraEngineAnimationFile.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_set>

namespace TerraEngine::Animation {
	namespace {
		#pragma pack(push, 1)
		struct SerializedStringRef {
			std::uint64_t offset = 0U;
			std::uint64_t size = 0U;
		};

		struct SerializedTransform {
			float translation[3U]{};
			float rotation[4U]{};
			float scale[3U]{};
		};

		struct SerializedSkeletonJoint {
			std::uint64_t stableId = 0U;
			std::int32_t parentIndex = -1;
			std::uint32_t deformsMesh = 0U;
			SerializedTransform localBindTransform{};
			SerializedStringRef displayName{};
			SerializedStringRef normalizedSourcePath{};
			SerializedStringRef semanticRole{};
		};

		struct SerializedSkeletonSocket {
			std::uint64_t stableId = 0U;
			std::uint64_t jointStableId = 0U;
			SerializedTransform localTransform{};
			SerializedStringRef displayName{};
			SerializedStringRef semanticRole{};
		};
		#pragma pack(pop)

		constexpr float kSingularMatrixTolerance = 0.000001F;

		bool IsKnownAxis(SkeletonSourceAxis axis) noexcept {
			return static_cast<std::uint8_t>(axis) <= static_cast<std::uint8_t>(SkeletonSourceAxis::NegativeZ);
		}

		bool IsFiniteTransform(const AnimationTransform& transform) noexcept {
			return IsFinite(transform);
		}

		bool IsNormalizedQuaternion(const AnimationQuaternion& rotation) noexcept {
			return IsNormalized(rotation);
		}

		bool IsFiniteNonSingularMatrix(const AnimationMatrix4x4& matrix) noexcept {
			float working[4U][4U]{};
			for(std::size_t row = 0U; row < 4U; ++row) {
				for(std::size_t column = 0U; column < 4U; ++column) {
					const float value = matrix.values[row * 4U + column];
					if(!std::isfinite(value)) {
						return false;
					}
					working[row][column] = value;
				}
			}

			// Gaussian elimination is small, deterministic validation work. It runs
			// only while cooking/loading, not in the animation evaluation hot path.
			for(std::size_t pivot = 0U; pivot < 4U; ++pivot) {
				std::size_t bestRow = pivot;
				for(std::size_t candidate = pivot + 1U; candidate < 4U; ++candidate) {
					if(std::abs(working[candidate][pivot]) > std::abs(working[bestRow][pivot])) {
						bestRow = candidate;
					}
				}
				if(std::abs(working[bestRow][pivot]) <= kSingularMatrixTolerance) {
					return false;
				}
				if(bestRow != pivot) {
					for(std::size_t column = pivot; column < 4U; ++column) {
						std::swap(working[pivot][column], working[bestRow][column]);
					}
				}
				const float divisor = working[pivot][pivot];
				for(std::size_t row = pivot + 1U; row < 4U; ++row) {
					const float factor = working[row][pivot] / divisor;
					for(std::size_t column = pivot; column < 4U; ++column) {
						working[row][column] -= factor * working[pivot][column];
					}
				}
			}
			return true;
		}

		SerializedTransform ToSerializedTransform(const AnimationTransform& transform) noexcept {
			return SerializedTransform{{transform.translation.x, transform.translation.y, transform.translation.z},
				{transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w},
				{transform.scale.x, transform.scale.y, transform.scale.z}};
		}

		AnimationTransform FromSerializedTransform(const SerializedTransform& transform) noexcept {
			AnimationTransform result;
			result.translation = {transform.translation[0U], transform.translation[1U], transform.translation[2U]};
			result.rotation = {transform.rotation[0U], transform.rotation[1U], transform.rotation[2U], transform.rotation[3U]};
			result.scale = {transform.scale[0U], transform.scale[1U], transform.scale[2U]};
			return result;
		}

		AnimationStatus ValidateSkeletonAsset(const SkeletonAsset& asset) noexcept {
			if(!asset.assetId.IsValid() || !std::isfinite(asset.source.unitScaleMeters)
				|| asset.source.unitScaleMeters <= 0.0F || !IsKnownAxis(asset.source.upAxis)
				|| !IsKnownAxis(asset.source.forwardAxis) || asset.source.upAxis == asset.source.forwardAxis) {
				return AnimationStatus::InvalidArgument;
			}
			if(ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, asset.joints.size()) != AnimationStatus::Ok) {
				return AnimationStatus::HardLimitExceeded;
			}
			if(asset.joints.empty() || asset.inverseBindMatrices.size() != asset.joints.size()
				|| asset.rootJointIndex >= asset.joints.size()) {
				return AnimationStatus::InvalidArgument;
			}
			if(asset.sockets.size() > std::numeric_limits<std::uint32_t>::max()) {
				return AnimationStatus::HardLimitExceeded;
			}

			std::unordered_set<std::uint64_t> jointIds;
			std::unordered_set<std::string> jointPaths;
			std::unordered_set<std::string> jointSemantics;
			std::size_t rootCount = 0U;
			for(std::size_t index = 0U; index < asset.joints.size(); ++index) {
				const SkeletonJoint& joint = asset.joints[index];
				if(joint.stableId == 0U || joint.normalizedSourcePath.empty() || !IsFiniteTransform(joint.localBindTransform)
					|| !IsNormalizedQuaternion(joint.localBindTransform.rotation)
					|| !jointIds.insert(joint.stableId).second || !jointPaths.insert(joint.normalizedSourcePath).second) {
					return AnimationStatus::InvalidArgument;
				}
				if(!joint.semanticRole.empty() && !jointSemantics.insert(joint.semanticRole).second) {
					return AnimationStatus::InvalidArgument;
				}
				if(joint.parentIndex == -1) {
					++rootCount;
					if(index != asset.rootJointIndex) {
						return AnimationStatus::InvalidArgument;
					}
				} else if(joint.parentIndex < 0 || static_cast<std::size_t>(joint.parentIndex) >= index) {
					return AnimationStatus::InvalidArgument;
				}
				if(!IsFiniteNonSingularMatrix(asset.inverseBindMatrices[index])) {
					return AnimationStatus::InvalidArgument;
				}
			}
			if(rootCount != 1U || asset.joints[asset.rootJointIndex].parentIndex != -1) {
				return AnimationStatus::InvalidArgument;
			}

			std::unordered_set<std::uint64_t> socketIds;
			std::unordered_set<std::string> socketSemantics;
			for(const SkeletonSocket& socket : asset.sockets) {
				if(socket.stableId == 0U || socket.jointStableId == 0U || socket.semanticRole.empty()
					|| !IsFiniteTransform(socket.localTransform) || !IsNormalizedQuaternion(socket.localTransform.rotation)
					|| !socketIds.insert(socket.stableId).second || !socketSemantics.insert(socket.semanticRole).second
					|| !jointIds.contains(socket.jointStableId)) {
					return AnimationStatus::InvalidArgument;
				}
			}
			return AnimationStatus::Ok;
		}

		bool SocketLess(const SkeletonSocket& left, const SkeletonSocket& right) noexcept {
			if(left.stableId != right.stableId) {
				return left.stableId < right.stableId;
			}
			if(left.semanticRole != right.semanticRole) {
				return left.semanticRole < right.semanticRole;
			}
			return left.jointStableId < right.jointStableId;
		}

		AnimationStatus AppendBytes(std::vector<std::uint8_t>& output, std::span<const std::byte> bytes) {
			std::uint64_t nextSize = 0U;
			if(!CheckedAdd(output.size(), bytes.size(), nextSize) || nextSize > std::numeric_limits<std::size_t>::max()) {
				return AnimationStatus::RangeOverflow;
			}
			const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.data());
			output.insert(output.end(), begin, begin + bytes.size());
			return AnimationStatus::Ok;
		}

		template<typename Type>
		AnimationStatus AppendPod(std::vector<std::uint8_t>& output, const Type& value) {
			return AppendBytes(output, std::as_bytes(std::span<const Type>(&value, 1U)));
		}

		AnimationStatus AppendString(std::vector<std::uint8_t>& output, const std::string& value, SerializedStringRef& result) {
			result.offset = output.size();
			result.size = value.size();
			return AppendBytes(output, std::as_bytes(std::span(value.data(), value.size())));
		}

		AnimationStatus ValidatePayloadRange(std::span<const std::uint8_t> payload,
			std::uint64_t offset, std::uint64_t size) noexcept {
			return ValidateAnimationRange(payload.size(), offset, size);
		}

		template<typename Type>
		AnimationStatus ReadTable(std::span<const std::uint8_t> payload, std::uint64_t offset,
			std::uint32_t count, std::vector<Type>& output) {
			std::uint64_t size = 0U;
			if(!CheckedMultiply(count, sizeof(Type), size)) {
				return AnimationStatus::RangeOverflow;
			}
			if(const AnimationStatus status = ValidatePayloadRange(payload, offset, size); status != AnimationStatus::Ok) {
				return status;
			}
			if(count > std::numeric_limits<std::size_t>::max()) {
				return AnimationStatus::RangeOverflow;
			}
			output.resize(count);
			if(!output.empty()) {
				std::memcpy(output.data(), payload.data() + static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
			}
			return AnimationStatus::Ok;
		}

		AnimationStatus ReadString(std::span<const std::uint8_t> payload, const SerializedStringRef& reference,
			std::string& output) {
			if(const AnimationStatus status = ValidatePayloadRange(payload, reference.offset, reference.size);
				status != AnimationStatus::Ok) {
				return status;
			}
			if(reference.size > std::numeric_limits<std::size_t>::max()) {
				return AnimationStatus::RangeOverflow;
			}
			output.assign(reinterpret_cast<const char*>(payload.data() + static_cast<std::size_t>(reference.offset)),
				static_cast<std::size_t>(reference.size));
			return AnimationStatus::Ok;
		}
	}

	AnimationStatus SerializeSkeletonAsset(const SkeletonAsset& asset, std::vector<std::uint8_t>& output) {
		if(const AnimationStatus status = ValidateSkeletonAsset(asset); status != AnimationStatus::Ok) {
			return status;
		}

		SkeletonAsset canonical = asset;
		canonical.formatVersion = kSkeletonAssetCurrentVersion;
		std::sort(canonical.sockets.begin(), canonical.sockets.end(), SocketLess);
		std::vector<std::uint8_t> payload(sizeof(SkeletonAssetFileHeaderV1), 0U);
		SkeletonAssetFileHeaderV1 header{};
		header.headerSize = sizeof(SkeletonAssetFileHeaderV1);
		header.assetId = canonical.assetId;
		header.compatibilityFingerprint = canonical.compatibilityFingerprint;
		header.referencePoseHash = canonical.referencePoseHash;
		header.rootJointIndex = canonical.rootJointIndex;
		header.jointCount = static_cast<std::uint32_t>(canonical.joints.size());
		header.inverseBindMatrixCount = static_cast<std::uint32_t>(canonical.inverseBindMatrices.size());
		header.socketCount = static_cast<std::uint32_t>(canonical.sockets.size());
		header.unitScaleMeters = canonical.source.unitScaleMeters;
		header.upAxis = canonical.source.upAxis;
		header.forwardAxis = canonical.source.forwardAxis;

		header.jointTableOffset = payload.size();
		std::vector<SerializedSkeletonJoint> serializedJoints(canonical.joints.size());
		for(std::size_t index = 0U; index < canonical.joints.size(); ++index) {
			const SkeletonJoint& joint = canonical.joints[index];
			serializedJoints[index].stableId = joint.stableId;
			serializedJoints[index].parentIndex = joint.parentIndex;
			serializedJoints[index].deformsMesh = joint.deformsMesh ? 1U : 0U;
			serializedJoints[index].localBindTransform = ToSerializedTransform(joint.localBindTransform);
		}
		if(const AnimationStatus status = AppendBytes(payload, std::as_bytes(std::span(serializedJoints))); status != AnimationStatus::Ok) {
			return status;
		}

		header.inverseBindMatrixTableOffset = payload.size();
		if(const AnimationStatus status = AppendBytes(payload, std::as_bytes(std::span(canonical.inverseBindMatrices))); status != AnimationStatus::Ok) {
			return status;
		}
		header.socketTableOffset = payload.size();
		std::vector<SerializedSkeletonSocket> serializedSockets(canonical.sockets.size());
		for(std::size_t index = 0U; index < canonical.sockets.size(); ++index) {
			const SkeletonSocket& socket = canonical.sockets[index];
			serializedSockets[index].stableId = socket.stableId;
			serializedSockets[index].jointStableId = socket.jointStableId;
			serializedSockets[index].localTransform = ToSerializedTransform(socket.localTransform);
		}
		if(const AnimationStatus status = AppendBytes(payload, std::as_bytes(std::span(serializedSockets))); status != AnimationStatus::Ok) {
			return status;
		}

		SerializedStringRef sourcePackagePath;
		if(const AnimationStatus status = AppendString(payload, canonical.source.sourcePackagePath, sourcePackagePath); status != AnimationStatus::Ok) {
			return status;
		}
		header.sourcePackagePathOffset = sourcePackagePath.offset;
		header.sourcePackagePathSize = sourcePackagePath.size;
		for(std::size_t index = 0U; index < canonical.joints.size(); ++index) {
			if(const AnimationStatus status = AppendString(payload, canonical.joints[index].displayName, serializedJoints[index].displayName); status != AnimationStatus::Ok) return status;
			if(const AnimationStatus status = AppendString(payload, canonical.joints[index].normalizedSourcePath, serializedJoints[index].normalizedSourcePath); status != AnimationStatus::Ok) return status;
			if(const AnimationStatus status = AppendString(payload, canonical.joints[index].semanticRole, serializedJoints[index].semanticRole); status != AnimationStatus::Ok) return status;
		}
		for(std::size_t index = 0U; index < canonical.sockets.size(); ++index) {
			if(const AnimationStatus status = AppendString(payload, canonical.sockets[index].displayName, serializedSockets[index].displayName); status != AnimationStatus::Ok) return status;
			if(const AnimationStatus status = AppendString(payload, canonical.sockets[index].semanticRole, serializedSockets[index].semanticRole); status != AnimationStatus::Ok) return status;
		}

		std::memcpy(payload.data() + header.jointTableOffset, serializedJoints.data(), serializedJoints.size() * sizeof(SerializedSkeletonJoint));
		std::memcpy(payload.data() + header.socketTableOffset, serializedSockets.data(), serializedSockets.size() * sizeof(SerializedSkeletonSocket));
		std::memcpy(payload.data(), &header, sizeof(header));

		AnimationAssetWriter writer;
		if(!writer.AddChunk(kAnimationChunkPayload, true, payload)) {
			return AnimationStatus::HardLimitExceeded;
		}
		AnimationAssetEnvelope envelope{};
		envelope.assetId = canonical.assetId;
		envelope.compatibilityFingerprint = canonical.compatibilityFingerprint;
		return writer.Finish(envelope, output);
	}

	AnimationStatus DeserializeSkeletonAsset(std::span<const std::uint8_t> input,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint, SkeletonAsset& output) {
		AnimationAssetReader reader;
		if(const AnimationStatus status = reader.Parse(input); status != AnimationStatus::Ok) {
			return status;
		}
		if(reader.Envelope().compatibilityFingerprint != expectedCompatibilityFingerprint) {
			return AnimationStatus::IncompatibleAsset;
		}
		const std::span<const std::uint8_t> payload = reader.FindChunk(kAnimationChunkPayload);
		if(payload.size() < sizeof(SkeletonAssetFileHeaderV1)) {
			return AnimationStatus::TruncatedInput;
		}

		SkeletonAssetFileHeaderV1 header{};
		std::memcpy(&header, payload.data(), sizeof(header));
		if(header.magic != kSkeletonAssetMagic || header.headerSize != sizeof(SkeletonAssetFileHeaderV1)) {
			return AnimationStatus::InvalidEnvelope;
		}
		if(header.formatVersion != kSkeletonAssetLegacyVersion && header.formatVersion != kSkeletonAssetCurrentVersion) {
			return AnimationStatus::UnsupportedMajorVersion;
		}
		if(header.compatibilityFingerprint != reader.Envelope().compatibilityFingerprint
			|| header.assetId != reader.Envelope().assetId) {
			return AnimationStatus::InvalidEnvelope;
		}
		if(const AnimationStatus limit = ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, header.jointCount);
			limit != AnimationStatus::Ok) {
			return limit;
		}
		if(header.inverseBindMatrixCount != header.jointCount || header.rootJointIndex >= header.jointCount) {
			return AnimationStatus::InvalidArgument;
		}

		std::vector<SerializedSkeletonJoint> serializedJoints;
		std::vector<AnimationMatrix4x4> matrices;
		std::vector<SerializedSkeletonSocket> serializedSockets;
		if(const AnimationStatus status = ReadTable(payload, header.jointTableOffset, header.jointCount, serializedJoints); status != AnimationStatus::Ok) return status;
		if(const AnimationStatus status = ReadTable(payload, header.inverseBindMatrixTableOffset, header.inverseBindMatrixCount, matrices); status != AnimationStatus::Ok) return status;
		if(const AnimationStatus status = ReadTable(payload, header.socketTableOffset, header.socketCount, serializedSockets); status != AnimationStatus::Ok) return status;

		SkeletonAsset staged;
		staged.assetId = header.assetId;
		staged.compatibilityFingerprint = header.compatibilityFingerprint;
		staged.rootJointIndex = header.rootJointIndex;
		staged.referencePoseHash = header.referencePoseHash;
		staged.source.unitScaleMeters = header.unitScaleMeters;
		staged.source.upAxis = header.upAxis;
		staged.source.forwardAxis = header.forwardAxis;
		staged.inverseBindMatrices = std::move(matrices);
		staged.formatVersion = kSkeletonAssetCurrentVersion; // v0 uses the same payload but publishes the current in-memory contract.
		if(const AnimationStatus status = ReadString(payload,
			SerializedStringRef{header.sourcePackagePathOffset, header.sourcePackagePathSize}, staged.source.sourcePackagePath); status != AnimationStatus::Ok) return status;

		staged.joints.resize(serializedJoints.size());
		for(std::size_t index = 0U; index < serializedJoints.size(); ++index) {
			const SerializedSkeletonJoint& source = serializedJoints[index];
			SkeletonJoint& joint = staged.joints[index];
			joint.stableId = source.stableId;
			joint.parentIndex = source.parentIndex;
			joint.deformsMesh = source.deformsMesh == 1U;
			if(source.deformsMesh > 1U) return AnimationStatus::InvalidArgument;
			joint.localBindTransform = FromSerializedTransform(source.localBindTransform);
			if(const AnimationStatus status = ReadString(payload, source.displayName, joint.displayName); status != AnimationStatus::Ok) return status;
			if(const AnimationStatus status = ReadString(payload, source.normalizedSourcePath, joint.normalizedSourcePath); status != AnimationStatus::Ok) return status;
			if(const AnimationStatus status = ReadString(payload, source.semanticRole, joint.semanticRole); status != AnimationStatus::Ok) return status;
		}
		staged.sockets.resize(serializedSockets.size());
		for(std::size_t index = 0U; index < serializedSockets.size(); ++index) {
			const SerializedSkeletonSocket& source = serializedSockets[index];
			SkeletonSocket& socket = staged.sockets[index];
			socket.stableId = source.stableId;
			socket.jointStableId = source.jointStableId;
			socket.localTransform = FromSerializedTransform(source.localTransform);
			if(const AnimationStatus status = ReadString(payload, source.displayName, socket.displayName); status != AnimationStatus::Ok) return status;
			if(const AnimationStatus status = ReadString(payload, source.semanticRole, socket.semanticRole); status != AnimationStatus::Ok) return status;
		}
		std::sort(staged.sockets.begin(), staged.sockets.end(), SocketLess);
		if(const AnimationStatus status = ValidateSkeletonAsset(staged); status != AnimationStatus::Ok) {
			return status;
		}
		output = std::move(staged);
		return AnimationStatus::Ok;
	}
}
