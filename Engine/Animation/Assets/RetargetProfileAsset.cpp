// RetargetProfileAsset.cpp - Checked deterministic retarget profile persistence.

#include "Animation/Assets/RetargetProfileAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		inline constexpr std::uint32_t kRetargetSkeletonDependencyType = 6U;

		class ByteWriter {
		public:
			template<typename Type>
			bool Put(const Type& value) {
				static_assert(std::is_trivially_copyable_v<Type>);
				std::uint64_t nextSize = 0U;
				if(!CheckedAdd(bytes_.size(), sizeof(Type), nextSize)
					|| nextSize > kRetargetProfileMaximumPayloadBytes
					|| nextSize > std::numeric_limits<std::size_t>::max()) {
					return false;
				}
				const std::size_t offset = bytes_.size();
				bytes_.resize(static_cast<std::size_t>(nextSize));
				std::memcpy(bytes_.data() + offset, &value, sizeof(Type));
				return true;
			}

			template<typename Enum>
			bool PutEnum(Enum value) {
				return Put(static_cast<std::underlying_type_t<Enum>>(value));
			}

			bool PutBool(bool value) {
				return Put(static_cast<std::uint8_t>(value ? 1U : 0U));
			}

			bool PutString(const std::string& value) {
				if(value.size() > kRetargetProfileMaximumStringBytes
					|| value.size() > std::numeric_limits<std::uint32_t>::max()
					|| !Put(static_cast<std::uint32_t>(value.size()))) {
					return false;
				}
				std::uint64_t nextSize = 0U;
				if(!CheckedAdd(bytes_.size(), value.size(), nextSize)
					|| nextSize > kRetargetProfileMaximumPayloadBytes
					|| nextSize > std::numeric_limits<std::size_t>::max()) {
					return false;
				}
				const std::size_t offset = bytes_.size();
				bytes_.resize(static_cast<std::size_t>(nextSize));
				if(!value.empty()) {
					std::memcpy(bytes_.data() + offset, value.data(), value.size());
				}
				return true;
			}

			bool PutIds(const std::vector<std::uint64_t>& values) {
				if(values.size() > std::numeric_limits<std::uint32_t>::max()
					|| !Put(static_cast<std::uint32_t>(values.size()))) {
					return false;
				}
				for(const std::uint64_t value : values) {
					if(!Put(value)) {
						return false;
					}
				}
				return true;
			}

			bool PutFloats(const std::vector<float>& values) {
				if(values.size() > std::numeric_limits<std::uint32_t>::max()
					|| !Put(static_cast<std::uint32_t>(values.size()))) {
					return false;
				}
				for(const float value : values) {
					if(!Put(value)) {
						return false;
					}
				}
				return true;
			}

			std::vector<std::uint8_t> Take() noexcept {
				return std::move(bytes_);
			}

		private:
			std::vector<std::uint8_t> bytes_;
		};

		class ByteReader {
		public:
			explicit ByteReader(std::span<const std::uint8_t> bytes)
				: bytes_(bytes) {}

			template<typename Type>
			bool Get(Type& value) noexcept {
				static_assert(std::is_trivially_copyable_v<Type>);
				if(cursor_ > bytes_.size() || bytes_.size() - cursor_ < sizeof(Type)) {
					return false;
				}
				std::memcpy(&value, bytes_.data() + cursor_, sizeof(Type));
				cursor_ += sizeof(Type);
				return true;
			}

			template<typename Enum>
			bool GetEnum(Enum& value) noexcept {
				std::underlying_type_t<Enum> raw{};
				if(!Get(raw)) {
					return false;
				}
				value = static_cast<Enum>(raw);
				return true;
			}

			bool GetBool(bool& value) noexcept {
				std::uint8_t raw = 0U;
				if(!Get(raw) || raw > 1U) {
					return false;
				}
				value = raw != 0U;
				return true;
			}

			bool GetString(std::string& value) {
				std::uint32_t size = 0U;
				if(!Get(size) || size > kRetargetProfileMaximumStringBytes
					|| cursor_ > bytes_.size() || bytes_.size() - cursor_ < size) {
					return false;
				}
				value.assign(reinterpret_cast<const char*>(bytes_.data() + cursor_), size);
				cursor_ += size;
				return true;
			}

			bool GetIds(std::vector<std::uint64_t>& values, std::size_t maximum) {
				std::uint32_t count = 0U;
				if(!Get(count) || count > maximum) {
					return false;
				}
				values.resize(count);
				for(std::uint64_t& value : values) {
					if(!Get(value)) {
						return false;
					}
				}
				return true;
			}

			bool GetFloats(std::vector<float>& values, std::size_t maximum) {
				std::uint32_t count = 0U;
				if(!Get(count) || count > maximum) {
					return false;
				}
				values.resize(count);
				for(float& value : values) {
					if(!Get(value)) {
						return false;
					}
				}
				return true;
			}

			bool Finished() const noexcept {
				return cursor_ == bytes_.size();
			}

		private:
			std::span<const std::uint8_t> bytes_;
			std::size_t cursor_ = 0U;
		};

		template<typename Enum>
		bool IsSupported(Enum) noexcept {
			return false;
		}

		template<>
		bool IsSupported(RetargetTranslationScaleMode value) noexcept {
			switch(value) {
			case RetargetTranslationScaleMode::None:
			case RetargetTranslationScaleMode::UniformHeight:
			case RetargetTranslationScaleMode::PerChainLength:
			case RetargetTranslationScaleMode::Authored:
				return true;
			}
			return false;
		}

		template<>
		bool IsSupported(RetargetRootMotionMode value) noexcept {
			switch(value) {
			case RetargetRootMotionMode::PreserveDisplacement:
			case RetargetRootMotionMode::ScaleByProportion:
			case RetargetRootMotionMode::InPlace:
				return true;
			}
			return false;
		}

		template<>
		bool IsSupported(RetargetTrajectoryMode value) noexcept {
			switch(value) {
			case RetargetTrajectoryMode::PreserveFacingAndDisplacement:
			case RetargetTrajectoryMode::PreserveFacing:
			case RetargetTrajectoryMode::PreserveDisplacement:
			case RetargetTrajectoryMode::Ignore:
				return true;
			}
			return false;
		}

		template<>
		bool IsSupported(RetargetFallbackMode value) noexcept {
			switch(value) {
			case RetargetFallbackMode::ReferencePose:
			case RetargetFallbackMode::NearestMappedAncestor:
			case RetargetFallbackMode::DeclaredTargetChain:
			case RetargetFallbackMode::DropOptional:
				return true;
			}
			return false;
		}

		template<>
		bool IsSupported(RetargetQualityTier value) noexcept {
			switch(value) {
			case RetargetQualityTier::Unrated:
			case RetargetQualityTier::Reduced:
			case RetargetQualityTier::Production:
			case RetargetQualityTier::Verified:
				return true;
			}
			return false;
		}

		bool IsFinitePositive(float value) noexcept {
			return std::isfinite(value) && value > 0.0F;
		}

		bool IsFiniteNonNegative(float value) noexcept {
			return std::isfinite(value) && value >= 0.0F;
		}

		bool IsUsableAxis(AnimationFloat3 value) noexcept {
			if(!IsFinite(value)) {
				return false;
			}
			const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
			return std::isfinite(lengthSquared) && lengthSquared > 1.0e-8F;
		}

		bool StringIsBounded(const std::string& value) noexcept {
			return value.size() <= kRetargetProfileMaximumStringBytes;
		}

		bool CheckedAccumulate(std::size_t& total, std::size_t value) noexcept {
			std::uint64_t sum = 0U;
			if(!CheckedAdd(total, value, sum) || sum > std::numeric_limits<std::size_t>::max()) {
				return false;
			}
			total = static_cast<std::size_t>(sum);
			return true;
		}

		void Canonicalize(RetargetProfileAsset& asset) {
			std::sort(asset.semanticMappings.begin(), asset.semanticMappings.end());
			std::sort(asset.explicitMappings.begin(), asset.explicitMappings.end());
			std::sort(asset.chainMappings.begin(), asset.chainMappings.end());
			std::sort(asset.orientationBases.begin(), asset.orientationBases.end());
			std::sort(asset.contacts.begin(), asset.contacts.end());
			std::sort(asset.twistRules.begin(), asset.twistRules.end());
			std::sort(asset.fallbackRules.begin(), asset.fallbackRules.end());
		}

		bool PutProfile(ByteWriter& writer, const RetargetProfileAsset& asset) {
			const std::uint16_t reserved = 0U;
			if(!writer.Put(kRetargetProfilePayloadMagic)
				|| !writer.Put(asset.formatVersion)
				|| !writer.Put(reserved)
				|| !writer.Put(asset.assetId)
				|| !writer.Put(asset.compatibilityFingerprint)
				|| !writer.Put(asset.sourceSkeletonAssetId)
				|| !writer.Put(asset.targetSkeletonAssetId)
				|| !writer.Put(asset.sourceSkeletonFingerprint)
				|| !writer.Put(asset.targetSkeletonFingerprint)
				|| !writer.Put(asset.sourceReferencePoseHash)
				|| !writer.Put(asset.targetReferencePoseHash)
				|| !writer.PutEnum(asset.proportion.translationScaleMode)
				|| !writer.Put(asset.proportion.sourceReferenceHeightMeters)
				|| !writer.Put(asset.proportion.targetReferenceHeightMeters)
				|| !writer.Put(asset.proportion.authoredTranslationScale)
				|| !writer.Put(asset.proportion.minimumScale)
				|| !writer.Put(asset.proportion.maximumScale)
				|| !writer.Put(asset.root.sourceRootJointId)
				|| !writer.Put(asset.root.targetRootJointId)
				|| !writer.PutEnum(asset.root.motionMode)
				|| !writer.PutBool(asset.root.preserveFacing)
				|| !writer.PutBool(asset.root.preserveVerticalDisplacement)
				|| !writer.Put(asset.root.displacementScale)
				|| !writer.Put(asset.root.yawOffsetRadians)
				|| !writer.PutEnum(asset.trajectory.mode)
				|| !writer.Put(asset.trajectory.sourceForwardAxis)
				|| !writer.Put(asset.trajectory.targetForwardAxis)
				|| !writer.Put(asset.trajectory.horizontalScale)
				|| !writer.Put(asset.trajectory.verticalScale)
				|| !writer.PutEnum(asset.quality.tier)
				|| !writer.Put(asset.quality.authoredCoverage)
				|| !writer.Put(asset.quality.expectedMeanPositionErrorMeters)
				|| !writer.Put(asset.quality.expectedMaximumPositionErrorMeters)
				|| !writer.Put(asset.quality.expectedMaximumRotationErrorRadians)
				|| !writer.Put(asset.quality.reachableEndEffectorToleranceMeters)
				|| !writer.Put(asset.quality.facingToleranceRadians)
				|| !writer.Put(asset.quality.rootDisplacementToleranceMeters)
				|| !writer.Put(asset.quality.timingToleranceTicks)
				|| !writer.Put(asset.quality.validationProfileHash)
				|| !writer.PutString(asset.quality.reviewReason)) {
				return false;
			}

			if(!writer.Put(static_cast<std::uint32_t>(asset.semanticMappings.size()))) {
				return false;
			}
			for(const RetargetSemanticMapping& mapping : asset.semanticMappings) {
				if(!writer.Put(mapping.mappingId) || !writer.Put(mapping.parentMappingId)
					|| !writer.Put(mapping.basisId) || !writer.Put(mapping.fallbackRuleId)
					|| !writer.Put(mapping.sourceJointId) || !writer.Put(mapping.targetJointId)
					|| !writer.PutString(mapping.sourceSemantic)
					|| !writer.PutString(mapping.targetSemantic)
					|| !writer.PutBool(mapping.required)) {
					return false;
				}
			}

			if(!writer.Put(static_cast<std::uint32_t>(asset.explicitMappings.size()))) {
				return false;
			}
			for(const RetargetExplicitJointMapping& mapping : asset.explicitMappings) {
				if(!writer.Put(mapping.mappingId) || !writer.Put(mapping.parentMappingId)
					|| !writer.Put(mapping.basisId) || !writer.Put(mapping.fallbackRuleId)
					|| !writer.Put(mapping.sourceJointId) || !writer.Put(mapping.targetJointId)
					|| !writer.PutBool(mapping.required)) {
					return false;
				}
			}

			if(!writer.Put(static_cast<std::uint32_t>(asset.chainMappings.size()))) {
				return false;
			}
			for(const RetargetChainMapping& mapping : asset.chainMappings) {
				if(!writer.Put(mapping.chainId) || !writer.PutString(mapping.semantic)
					|| !writer.PutIds(mapping.sourceJointIds)
					|| !writer.PutIds(mapping.targetJointIds)
					|| !writer.PutEnum(mapping.translationScaleMode)
					|| !writer.Put(mapping.authoredTranslationScale)
					|| !writer.PutBool(mapping.required)) {
					return false;
				}
			}

			if(!writer.Put(static_cast<std::uint32_t>(asset.orientationBases.size()))) {
				return false;
			}
			for(const RetargetOrientationBasis& basis : asset.orientationBases) {
				if(!writer.Put(basis.basisId) || !writer.Put(basis.sourceJointId)
					|| !writer.Put(basis.targetJointId) || !writer.Put(basis.sourceBasis)
					|| !writer.Put(basis.targetBasis)) {
					return false;
				}
			}

			if(!writer.Put(static_cast<std::uint32_t>(asset.contacts.size()))) {
				return false;
			}
			for(const RetargetContactRule& contact : asset.contacts) {
				if(!writer.Put(contact.contactId) || !writer.Put(contact.sourceJointId)
					|| !writer.Put(contact.targetJointId) || !writer.Put(contact.targetChainId)
					|| !writer.Put(contact.toleranceMeters)
					|| !writer.PutBool(contact.preservePosition)
					|| !writer.PutBool(contact.preserveOrientation)
					|| !writer.PutBool(contact.required)) {
					return false;
				}
			}

			if(!writer.Put(static_cast<std::uint32_t>(asset.twistRules.size()))) {
				return false;
			}
			for(const RetargetTwistRule& twist : asset.twistRules) {
				if(!writer.Put(twist.twistId) || !writer.Put(twist.sourceJointId)
					|| !writer.Put(twist.sourceChildJointId)
					|| !writer.Put(twist.sourceTwistAxis)
					|| !writer.PutIds(twist.targetJointIds)
					|| !writer.PutFloats(twist.targetWeights)) {
					return false;
				}
			}

			if(!writer.Put(static_cast<std::uint32_t>(asset.fallbackRules.size()))) {
				return false;
			}
			for(const RetargetFallbackRule& fallback : asset.fallbackRules) {
				if(!writer.Put(fallback.fallbackRuleId)
					|| !writer.Put(fallback.sourceMappingId)
					|| !writer.Put(fallback.nextFallbackRuleId)
					|| !writer.PutEnum(fallback.mode)
					|| !writer.PutIds(fallback.targetJointIds)
					|| !writer.PutBool(fallback.reportReducedFidelity)) {
					return false;
				}
			}
			return true;
		}

		template<typename Type, typename Reader>
		bool ReadCollection(
			ByteReader& reader,
			std::vector<Type>& output,
			std::size_t maximum,
			Reader&& readRecord) {
			std::uint32_t count = 0U;
			if(!reader.Get(count) || count > maximum) {
				return false;
			}
			output.resize(count);
			for(Type& value : output) {
				if(!readRecord(value)) {
					return false;
				}
			}
			return true;
		}

		bool ReadProfile(ByteReader& reader, RetargetProfileAsset& asset) {
			std::uint32_t magic = 0U;
			std::uint16_t reserved = 0U;
			if(!reader.Get(magic) || !reader.Get(asset.formatVersion) || !reader.Get(reserved)
				|| magic != kRetargetProfilePayloadMagic || reserved != 0U
				|| !reader.Get(asset.assetId) || !reader.Get(asset.compatibilityFingerprint)
				|| !reader.Get(asset.sourceSkeletonAssetId)
				|| !reader.Get(asset.targetSkeletonAssetId)
				|| !reader.Get(asset.sourceSkeletonFingerprint)
				|| !reader.Get(asset.targetSkeletonFingerprint)
				|| !reader.Get(asset.sourceReferencePoseHash)
				|| !reader.Get(asset.targetReferencePoseHash)
				|| !reader.GetEnum(asset.proportion.translationScaleMode)
				|| !reader.Get(asset.proportion.sourceReferenceHeightMeters)
				|| !reader.Get(asset.proportion.targetReferenceHeightMeters)
				|| !reader.Get(asset.proportion.authoredTranslationScale)
				|| !reader.Get(asset.proportion.minimumScale)
				|| !reader.Get(asset.proportion.maximumScale)
				|| !reader.Get(asset.root.sourceRootJointId)
				|| !reader.Get(asset.root.targetRootJointId)
				|| !reader.GetEnum(asset.root.motionMode)
				|| !reader.GetBool(asset.root.preserveFacing)
				|| !reader.GetBool(asset.root.preserveVerticalDisplacement)
				|| !reader.Get(asset.root.displacementScale)
				|| !reader.Get(asset.root.yawOffsetRadians)
				|| !reader.GetEnum(asset.trajectory.mode)
				|| !reader.Get(asset.trajectory.sourceForwardAxis)
				|| !reader.Get(asset.trajectory.targetForwardAxis)
				|| !reader.Get(asset.trajectory.horizontalScale)
				|| !reader.Get(asset.trajectory.verticalScale)
				|| !reader.GetEnum(asset.quality.tier)
				|| !reader.Get(asset.quality.authoredCoverage)
				|| !reader.Get(asset.quality.expectedMeanPositionErrorMeters)
				|| !reader.Get(asset.quality.expectedMaximumPositionErrorMeters)
				|| !reader.Get(asset.quality.expectedMaximumRotationErrorRadians)
				|| !reader.Get(asset.quality.reachableEndEffectorToleranceMeters)
				|| !reader.Get(asset.quality.facingToleranceRadians)
				|| !reader.Get(asset.quality.rootDisplacementToleranceMeters)
				|| !reader.Get(asset.quality.timingToleranceTicks)
				|| !reader.Get(asset.quality.validationProfileHash)
				|| !reader.GetString(asset.quality.reviewReason)) {
				return false;
			}

			if(!ReadCollection(reader, asset.semanticMappings, kRetargetProfileMaximumMappings,
				[&reader](RetargetSemanticMapping& mapping) {
					return reader.Get(mapping.mappingId) && reader.Get(mapping.parentMappingId)
						&& reader.Get(mapping.basisId) && reader.Get(mapping.fallbackRuleId)
						&& reader.Get(mapping.sourceJointId) && reader.Get(mapping.targetJointId)
						&& reader.GetString(mapping.sourceSemantic)
						&& reader.GetString(mapping.targetSemantic)
						&& reader.GetBool(mapping.required);
				})) {
				return false;
			}
			if(!ReadCollection(reader, asset.explicitMappings, kRetargetProfileMaximumMappings,
				[&reader](RetargetExplicitJointMapping& mapping) {
					return reader.Get(mapping.mappingId) && reader.Get(mapping.parentMappingId)
						&& reader.Get(mapping.basisId) && reader.Get(mapping.fallbackRuleId)
						&& reader.Get(mapping.sourceJointId) && reader.Get(mapping.targetJointId)
						&& reader.GetBool(mapping.required);
				})) {
				return false;
			}
			if(!ReadCollection(reader, asset.chainMappings, kRetargetProfileMaximumChains,
				[&reader](RetargetChainMapping& mapping) {
					return reader.Get(mapping.chainId) && reader.GetString(mapping.semantic)
						&& reader.GetIds(mapping.sourceJointIds, kRetargetProfileMaximumMappings)
						&& reader.GetIds(mapping.targetJointIds, kRetargetProfileMaximumMappings)
						&& reader.GetEnum(mapping.translationScaleMode)
						&& reader.Get(mapping.authoredTranslationScale)
						&& reader.GetBool(mapping.required);
				})) {
				return false;
			}
			if(!ReadCollection(reader, asset.orientationBases, kRetargetProfileMaximumMappings,
				[&reader](RetargetOrientationBasis& basis) {
					return reader.Get(basis.basisId) && reader.Get(basis.sourceJointId)
						&& reader.Get(basis.targetJointId) && reader.Get(basis.sourceBasis)
						&& reader.Get(basis.targetBasis);
				})) {
				return false;
			}
			const std::size_t maximumConstraints = static_cast<std::size_t>(
				GetAnimationRuntimeLimit(AnimationLimit::AnimatorProceduralGoals).hardMaximum);
			if(!ReadCollection(reader, asset.contacts, maximumConstraints,
				[&reader](RetargetContactRule& contact) {
					return reader.Get(contact.contactId) && reader.Get(contact.sourceJointId)
						&& reader.Get(contact.targetJointId) && reader.Get(contact.targetChainId)
						&& reader.Get(contact.toleranceMeters)
						&& reader.GetBool(contact.preservePosition)
						&& reader.GetBool(contact.preserveOrientation)
						&& reader.GetBool(contact.required);
				})) {
				return false;
			}
			if(!ReadCollection(reader, asset.twistRules, kRetargetProfileMaximumMappings,
				[&reader](RetargetTwistRule& twist) {
					return reader.Get(twist.twistId) && reader.Get(twist.sourceJointId)
						&& reader.Get(twist.sourceChildJointId)
						&& reader.Get(twist.sourceTwistAxis)
						&& reader.GetIds(twist.targetJointIds, kRetargetProfileMaximumMappings)
						&& reader.GetFloats(twist.targetWeights, kRetargetProfileMaximumMappings);
				})) {
				return false;
			}
			return ReadCollection(reader, asset.fallbackRules, kRetargetProfileMaximumMappings,
				[&reader](RetargetFallbackRule& fallback) {
					return reader.Get(fallback.fallbackRuleId)
						&& reader.Get(fallback.sourceMappingId)
						&& reader.Get(fallback.nextFallbackRuleId)
						&& reader.GetEnum(fallback.mode)
						&& reader.GetIds(fallback.targetJointIds, kRetargetProfileMaximumMappings)
						&& reader.GetBool(fallback.reportReducedFidelity);
				});
		}
	}

	AnimationStatus ValidateRetargetProfileAssetStructure(
		const RetargetProfileAsset& asset) noexcept {
		if(asset.formatVersion != kRetargetProfileAssetCurrentVersion) {
			return AnimationStatus::UnsupportedMajorVersion;
		}
		if(!asset.assetId.IsValid() || !asset.sourceSkeletonAssetId.IsValid()
			|| !asset.targetSkeletonAssetId.IsValid()
			|| asset.compatibilityFingerprint == 0U
			|| asset.sourceSkeletonFingerprint == 0U
			|| asset.targetSkeletonFingerprint == 0U
			|| asset.sourceReferencePoseHash == 0U
			|| asset.targetReferencePoseHash == 0U) {
			return AnimationStatus::InvalidArgument;
		}

		std::size_t mappingCount = 0U;
		std::size_t totalChainJoints = 0U;
		if(!CheckedAccumulate(mappingCount, asset.semanticMappings.size())
			|| !CheckedAccumulate(mappingCount, asset.explicitMappings.size())
			|| mappingCount > kRetargetProfileMaximumMappings
			|| asset.chainMappings.size() > kRetargetProfileMaximumChains
			|| asset.orientationBases.size() > kRetargetProfileMaximumMappings
			|| asset.twistRules.size() > kRetargetProfileMaximumMappings
			|| asset.fallbackRules.size() > kRetargetProfileMaximumMappings
			|| asset.contacts.size() > static_cast<std::size_t>(
				GetAnimationRuntimeLimit(AnimationLimit::AnimatorProceduralGoals).hardMaximum)) {
			return AnimationStatus::HardLimitExceeded;
		}
		for(const RetargetChainMapping& chain : asset.chainMappings) {
			if(chain.sourceJointIds.size() > kRetargetProfileMaximumMappings
				|| chain.targetJointIds.size() > kRetargetProfileMaximumMappings
				|| !CheckedAccumulate(totalChainJoints, chain.sourceJointIds.size())
				|| !CheckedAccumulate(totalChainJoints, chain.targetJointIds.size())
				|| totalChainJoints > kRetargetProfileMaximumTotalChainJoints) {
				return AnimationStatus::HardLimitExceeded;
			}
		}

		if(!IsSupported(asset.proportion.translationScaleMode)
			|| !IsSupported(asset.root.motionMode) || !IsSupported(asset.trajectory.mode)
			|| !IsSupported(asset.quality.tier)) {
			return AnimationStatus::Unsupported;
		}
		if(!IsFinitePositive(asset.proportion.sourceReferenceHeightMeters)
			|| !IsFinitePositive(asset.proportion.targetReferenceHeightMeters)
			|| !IsFinitePositive(asset.proportion.authoredTranslationScale)
			|| !IsFinitePositive(asset.proportion.minimumScale)
			|| !IsFinitePositive(asset.proportion.maximumScale)
			|| asset.proportion.minimumScale > asset.proportion.maximumScale
			|| !IsFinitePositive(asset.root.displacementScale)
			|| !std::isfinite(asset.root.yawOffsetRadians)
			|| !IsUsableAxis(asset.trajectory.sourceForwardAxis)
			|| !IsUsableAxis(asset.trajectory.targetForwardAxis)
			|| !IsFinitePositive(asset.trajectory.horizontalScale)
			|| !IsFinitePositive(asset.trajectory.verticalScale)) {
			return AnimationStatus::InvalidArgument;
		}

		if(!std::isfinite(asset.quality.authoredCoverage)
			|| asset.quality.authoredCoverage < 0.0F
			|| asset.quality.authoredCoverage > 1.0F
			|| !IsFiniteNonNegative(asset.quality.expectedMeanPositionErrorMeters)
			|| !IsFiniteNonNegative(asset.quality.expectedMaximumPositionErrorMeters)
			|| !IsFiniteNonNegative(asset.quality.expectedMaximumRotationErrorRadians)
			|| !IsFinitePositive(asset.quality.reachableEndEffectorToleranceMeters)
			|| asset.quality.reachableEndEffectorToleranceMeters
				> kRetargetDefaultReachableEndEffectorToleranceMeters
			|| !IsFiniteNonNegative(asset.quality.facingToleranceRadians)
			|| !IsFiniteNonNegative(asset.quality.rootDisplacementToleranceMeters)
			|| asset.quality.timingToleranceTicks != 0U
			|| !StringIsBounded(asset.quality.reviewReason)) {
			return AnimationStatus::InvalidArgument;
		}

		for(const RetargetSemanticMapping& mapping : asset.semanticMappings) {
			if(!StringIsBounded(mapping.sourceSemantic)
				|| !StringIsBounded(mapping.targetSemantic)) {
				return AnimationStatus::HardLimitExceeded;
			}
		}
		for(const RetargetChainMapping& chain : asset.chainMappings) {
			if(!StringIsBounded(chain.semantic)
				|| !IsSupported(chain.translationScaleMode)
				|| !IsFinitePositive(chain.authoredTranslationScale)) {
				return !IsSupported(chain.translationScaleMode)
					? AnimationStatus::Unsupported
					: AnimationStatus::InvalidArgument;
			}
		}
		for(const RetargetOrientationBasis& basis : asset.orientationBases) {
			if(!IsNormalized(basis.sourceBasis) || !IsNormalized(basis.targetBasis)) {
				return AnimationStatus::InvalidArgument;
			}
		}
		for(const RetargetContactRule& contact : asset.contacts) {
			if(!IsFinitePositive(contact.toleranceMeters)
				|| contact.toleranceMeters > kRetargetDefaultContactToleranceMeters) {
				return AnimationStatus::InvalidArgument;
			}
		}
		const bool tightensContact = std::any_of(asset.contacts.begin(),
			asset.contacts.end(), [](const RetargetContactRule& contact) {
				return contact.toleranceMeters
					< kRetargetDefaultContactToleranceMeters;
			});
		const bool tightensReachableEffector =
			asset.quality.reachableEndEffectorToleranceMeters
				< kRetargetDefaultReachableEndEffectorToleranceMeters;
		// Defaults are the frozen acceptance contract. A profile can demand a
		// stricter result only when it carries a review explanation for tooling.
		if((tightensContact || tightensReachableEffector)
			&& asset.quality.reviewReason.empty()) {
			return AnimationStatus::InvalidArgument;
		}
		for(const RetargetTwistRule& twist : asset.twistRules) {
			if(!IsUsableAxis(twist.sourceTwistAxis)
				|| twist.targetJointIds.size() != twist.targetWeights.size()
				|| twist.targetJointIds.size() > kRetargetProfileMaximumMappings) {
				return AnimationStatus::InvalidArgument;
			}
			for(const float weight : twist.targetWeights) {
				if(!IsFiniteNonNegative(weight)) {
					return AnimationStatus::InvalidArgument;
				}
			}
		}
		for(const RetargetFallbackRule& fallback : asset.fallbackRules) {
			if(!IsSupported(fallback.mode)
				|| fallback.targetJointIds.size() > kRetargetProfileMaximumMappings) {
				return !IsSupported(fallback.mode)
					? AnimationStatus::Unsupported
					: AnimationStatus::HardLimitExceeded;
			}
		}
		return AnimationStatus::Ok;
	}

	AnimationStatus SerializeRetargetProfileAsset(
		const RetargetProfileAsset& asset,
		std::vector<std::uint8_t>& output) {
		const AnimationStatus structure = ValidateRetargetProfileAssetStructure(asset);
		if(structure != AnimationStatus::Ok) {
			return structure;
		}
		try {
			RetargetProfileAsset canonical = asset;
			canonical.runtimeGeneration = 0U;
			Canonicalize(canonical);
			ByteWriter payloadWriter;
			if(!PutProfile(payloadWriter, canonical)) {
				return AnimationStatus::RangeOverflow;
			}
			const std::vector<std::uint8_t> payload = payloadWriter.Take();
			AnimationAssetWriter writer;
			if(!writer.AddDependency({canonical.sourceSkeletonAssetId,
					kRetargetSkeletonDependencyType, 1U})
				|| !writer.AddDependency({canonical.targetSkeletonAssetId,
					kRetargetSkeletonDependencyType, 1U})) {
				return AnimationStatus::RangeOverflow;
			}
			if(!writer.AddChunk(kAnimationChunkPayload, true, payload)) {
				return AnimationStatus::InternalError;
			}
			AnimationAssetEnvelope envelope{};
			envelope.assetId = canonical.assetId;
			envelope.compatibilityFingerprint = canonical.compatibilityFingerprint;
			return writer.Finish(envelope, output);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus DeserializeRetargetProfileAsset(
		std::span<const std::uint8_t> input,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		RetargetProfileAsset& output) {
		try {
			if(input.size() > kRetargetProfileMaximumPayloadBytes
				+ sizeof(AnimationAssetEnvelope) + 4U * sizeof(AnimationChunkEntry)) {
				return AnimationStatus::HardLimitExceeded;
			}
			AnimationAssetReader reader;
			if(const AnimationStatus status = reader.Parse(input); status != AnimationStatus::Ok) {
				return status;
			}
			if(expectedCompatibilityFingerprint != 0U
				&& reader.Envelope().compatibilityFingerprint
					!= expectedCompatibilityFingerprint) {
				return AnimationStatus::IncompatibleAsset;
			}
			const std::span<const AnimationDependencyEntry> dependencies =
				reader.Dependencies();
			if(dependencies.size() != 2U) {
				return AnimationStatus::InvalidEnvelope;
			}
			const std::span<const std::uint8_t> payload = reader.FindChunk(kAnimationChunkPayload);
			if(payload.empty() || payload.size() > kRetargetProfileMaximumPayloadBytes) {
				return AnimationStatus::InvalidChunk;
			}
			RetargetProfileAsset staged;
			ByteReader payloadReader(payload);
			if(!ReadProfile(payloadReader, staged) || !payloadReader.Finished()) {
				return AnimationStatus::TruncatedInput;
			}
			if(staged.assetId != reader.Envelope().assetId
				|| staged.compatibilityFingerprint
					!= reader.Envelope().compatibilityFingerprint) {
				return AnimationStatus::InvalidEnvelope;
			}
			auto hasSkeletonDependency = [&dependencies](const AnimationGuid& assetId) {
				return std::any_of(dependencies.begin(), dependencies.end(),
					[&assetId](const AnimationDependencyEntry& dependency) {
						return dependency.assetId == assetId
							&& dependency.expectedAssetType
								== kRetargetSkeletonDependencyType
							&& dependency.expectedGeneration == 1U;
					});
			};
			if(!hasSkeletonDependency(staged.sourceSkeletonAssetId)
				|| !hasSkeletonDependency(staged.targetSkeletonAssetId)) {
				return AnimationStatus::InvalidEnvelope;
			}
			if(const AnimationStatus status = ValidateRetargetProfileAssetStructure(staged);
				status != AnimationStatus::Ok) {
				return status;
			}
			Canonicalize(staged);
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}
}
