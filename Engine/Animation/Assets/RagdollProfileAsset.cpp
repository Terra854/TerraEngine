// RagdollProfileAsset.cpp - Checked persistence for provider-neutral ragdolls.

#include "Animation/Assets/RagdollProfileAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <tuple>
#include <type_traits>

namespace TerraEngine::Animation {
	namespace {
		inline constexpr std::uint32_t kSkeletonDependencyType = 6U;
		inline constexpr std::uint32_t kAnimationClipDependencyType = 7U;
		inline constexpr std::uint32_t kCookedColliderDependencyType = 5U;
		inline constexpr float kVectorTolerance = 0.001F;
		inline constexpr float kHullVolumeTolerance = 0.000001F;
		inline constexpr std::size_t kMaximumEnvelopeOverheadBytes = 4U * 1024U * 1024U;
		inline constexpr std::uint32_t kMaximumEnvelopeDependencies =
			1U + static_cast<std::uint32_t>(kRagdollProfileMaximumColliders)
			+ static_cast<std::uint32_t>(kRagdollProfileMaximumRecoveryRecords);
		inline constexpr std::uint32_t kMaximumEnvelopeChunks = 16U;

		class ByteWriter {
		public:
			template<typename Type>
			bool Put(const Type& value) {
				static_assert(std::is_trivially_copyable_v<Type>);
				if(sizeof(Type) > kRagdollProfileMaximumPayloadBytes
					|| bytes_.size() > kRagdollProfileMaximumPayloadBytes - sizeof(Type)) {
					return false;
				}
				const std::size_t offset = bytes_.size();
				bytes_.resize(offset + sizeof(Type));
				std::memcpy(bytes_.data() + offset, &value, sizeof(Type));
				return true;
			}

			bool PutBool(bool value) {
				return Put(static_cast<std::uint8_t>(value ? 1U : 0U));
			}

			template<typename Enum>
			bool PutEnum(Enum value) {
				return Put(static_cast<std::underlying_type_t<Enum>>(value));
			}

			std::vector<std::uint8_t> Take() noexcept {
				return std::move(bytes_);
			}

		private:
			std::vector<std::uint8_t> bytes_;
		};

		class ByteReader {
		public:
			explicit ByteReader(std::span<const std::uint8_t> bytes) noexcept
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

			bool GetBool(bool& value) noexcept {
				std::uint8_t encoded = 0U;
				if(!Get(encoded) || encoded > 1U) {
					return false;
				}
				value = encoded != 0U;
				return true;
			}

			template<typename Enum>
			bool GetEnum(Enum& value) noexcept {
				std::underlying_type_t<Enum> encoded{};
				if(!Get(encoded)) {
					return false;
				}
				value = static_cast<Enum>(encoded);
				return true;
			}

			bool Finished() const noexcept {
				return cursor_ == bytes_.size();
			}

		private:
			std::span<const std::uint8_t> bytes_;
			std::size_t cursor_ = 0U;
		};

		bool IsSupported(RagdollColliderShape value) noexcept {
			return value >= RagdollColliderShape::Sphere
				&& value <= RagdollColliderShape::ConvexHull;
		}

		bool IsSupported(RagdollConvexHullSource value) noexcept {
			return value >= RagdollConvexHullSource::None
				&& value <= RagdollConvexHullSource::ImmutableCookedAsset;
		}

		bool IsSupported(RagdollMotionType value) noexcept {
			return value >= RagdollMotionType::Dynamic
				&& value <= RagdollMotionType::AnimationDriven;
		}

		bool IsSupported(RagdollInertiaMode value) noexcept {
			return value >= RagdollInertiaMode::ProviderComputed
				&& value <= RagdollInertiaMode::ExplicitDiagonal;
		}

		bool IsSupported(RagdollConstraintKind value) noexcept {
			return value >= RagdollConstraintKind::Fixed
				&& value <= RagdollConstraintKind::SixDof;
		}

		bool IsSupported(RagdollLimitMode value) noexcept {
			return value >= RagdollLimitMode::Free && value <= RagdollLimitMode::Locked;
		}

		bool IsSupported(RagdollMotorMode value) noexcept {
			return value >= RagdollMotorMode::Disabled && value <= RagdollMotorMode::Velocity;
		}

		bool IsSupported(RagdollBoundaryPolicy value) noexcept {
			return value >= RagdollBoundaryPolicy::MatchAnimation
				&& value <= RagdollBoundaryPolicy::MotorToAnimation;
		}

		bool IsSupported(RagdollCoveragePolicy value) noexcept {
			return value >= RagdollCoveragePolicy::MappedBodiesOnly
				&& value <= RagdollCoveragePolicy::AllDeformingJoints;
		}

		bool IsSupported(RagdollRecoveryPosture value) noexcept {
			return value >= RagdollRecoveryPosture::Any
				&& value <= RagdollRecoveryPosture::RightSide;
		}

		bool FiniteInRange(float value, float minimum, float maximum) noexcept {
			return std::isfinite(value) && value >= minimum && value <= maximum;
		}

		bool FiniteNonNegative(float value) noexcept {
			return std::isfinite(value) && value >= 0.0F;
		}

		float Dot(const AnimationFloat3& left, const AnimationFloat3& right) noexcept {
			return left.x * right.x + left.y * right.y + left.z * right.z;
		}

		AnimationFloat3 Subtract(
			const AnimationFloat3& left,
			const AnimationFloat3& right) noexcept {
			return {left.x - right.x, left.y - right.y, left.z - right.z};
		}

		AnimationFloat3 Cross(
			const AnimationFloat3& left,
			const AnimationFloat3& right) noexcept {
			return {
				left.y * right.z - left.z * right.y,
				left.z * right.x - left.x * right.z,
				left.x * right.y - left.y * right.x
			};
		}

		bool IsUnitVector(const AnimationFloat3& value) noexcept {
			return IsFinite(value) && std::abs(Dot(value, value) - 1.0F) <= kVectorTolerance;
		}

		bool IsValidFrame(const RagdollConstraintFrame& frame) noexcept {
			return IsFinite(frame.localPosition) && IsNormalized(frame.localOrientation)
				&& IsUnitVector(frame.primaryAxis) && IsUnitVector(frame.secondaryAxis)
				&& std::abs(Dot(frame.primaryAxis, frame.secondaryAxis)) <= kVectorTolerance;
		}

		bool IsEmpty(const RagdollImmutableShapeReference& reference) noexcept {
			return !reference.assetId.IsValid() && reference.expectedGeneration == 0U
				&& reference.compatibilityFingerprint == 0U;
		}

		bool HasThreeDimensionalHull(std::span<const AnimationFloat3> vertices) noexcept {
			if(vertices.size() < 4U) {
				return false;
			}
			const AnimationFloat3 origin = vertices.front();
			std::size_t lineIndex = vertices.size();
			for(std::size_t index = 1U; index < vertices.size(); ++index) {
				const AnimationFloat3 delta = Subtract(vertices[index], origin);
				if(Dot(delta, delta) > kHullVolumeTolerance) {
					lineIndex = index;
					break;
				}
			}
			if(lineIndex == vertices.size()) {
				return false;
			}
			const AnimationFloat3 line = Subtract(vertices[lineIndex], origin);
			AnimationFloat3 planeNormal{};
			bool foundPlane = false;
			for(std::size_t index = 1U; index < vertices.size(); ++index) {
				planeNormal = Cross(line, Subtract(vertices[index], origin));
				if(Dot(planeNormal, planeNormal) > kHullVolumeTolerance) {
					foundPlane = true;
					break;
				}
			}
			if(!foundPlane) {
				return false;
			}
			for(std::size_t index = 1U; index < vertices.size(); ++index) {
				if(std::abs(Dot(planeNormal, Subtract(vertices[index], origin)))
					> kHullVolumeTolerance) {
					return true;
				}
			}
			return false;
		}

		bool ValidateColliderShape(const RagdollColliderRecord& collider) noexcept {
			if(!IsSupported(collider.shape) || !IsSupported(collider.convexHullSource)
				|| !IsFinite(collider.localCenter) || !IsNormalized(collider.localOrientation)
				|| !IsFinite(collider.dimensions) || !FiniteNonNegative(collider.friction)
				|| !FiniteInRange(collider.restitution, 0.0F, 1.0F)) {
				return false;
			}
			if(collider.shape == RagdollColliderShape::Sphere) {
				return collider.convexHullSource == RagdollConvexHullSource::None
					&& collider.dimensions.x > 0.0F && collider.convexHullVertices.empty()
					&& IsEmpty(collider.immutableShape);
			}
			if(collider.shape == RagdollColliderShape::Capsule) {
				return collider.convexHullSource == RagdollConvexHullSource::None
					&& collider.dimensions.x > 0.0F && collider.dimensions.y >= 0.0F
					&& collider.convexHullVertices.empty() && IsEmpty(collider.immutableShape);
			}
			if(collider.shape == RagdollColliderShape::Box) {
				return collider.convexHullSource == RagdollConvexHullSource::None
					&& collider.dimensions.x > 0.0F && collider.dimensions.y > 0.0F
					&& collider.dimensions.z > 0.0F && collider.convexHullVertices.empty()
					&& IsEmpty(collider.immutableShape);
			}
			if(collider.convexHullSource == RagdollConvexHullSource::InlineVertices) {
				if(collider.convexHullVertices.size() < 4U
					|| collider.convexHullVertices.size()
						> kRagdollProfileMaximumConvexHullVerticesPerCollider
					|| !IsEmpty(collider.immutableShape)) {
					return false;
				}
				for(std::size_t index = 0U; index < collider.convexHullVertices.size(); ++index) {
					if(!IsFinite(collider.convexHullVertices[index])) {
						return false;
					}
					for(std::size_t other = index + 1U;
						other < collider.convexHullVertices.size(); ++other) {
						if(collider.convexHullVertices[index] == collider.convexHullVertices[other]) {
							return false;
						}
					}
				}
				return HasThreeDimensionalHull(collider.convexHullVertices);
			}
			return collider.convexHullSource == RagdollConvexHullSource::ImmutableCookedAsset
				&& collider.convexHullVertices.empty() && collider.immutableShape.assetId.IsValid()
				&& collider.immutableShape.expectedGeneration != 0U
				&& collider.immutableShape.compatibilityFingerprint != 0U;
		}

		bool ValidateLimit(const RagdollAxisLimit& limit) noexcept {
			if(!IsSupported(limit.mode) || !std::isfinite(limit.minimum)
				|| !std::isfinite(limit.maximum)
				|| !FiniteInRange(limit.restitution, 0.0F, 1.0F)
				|| !FiniteNonNegative(limit.springFrequencyHertz)
				|| !FiniteNonNegative(limit.springDampingRatio)) {
				return false;
			}
			if(limit.mode == RagdollLimitMode::Limited) {
				return limit.minimum <= limit.maximum;
			}
			if(limit.mode == RagdollLimitMode::Locked) {
				return std::abs(limit.minimum - limit.maximum) <= 0.000001F;
			}
			return true;
		}

		bool ValidateMotor(
			const RagdollAxisMotor& motor,
			const RagdollAxisLimit& limit) noexcept {
			if(!IsSupported(motor.mode) || !std::isfinite(motor.targetPosition)
				|| !std::isfinite(motor.targetVelocity) || !FiniteNonNegative(motor.stiffness)
				|| !FiniteNonNegative(motor.damping) || !FiniteNonNegative(motor.maximumForce)) {
				return false;
			}
			if(motor.mode == RagdollMotorMode::Disabled) {
				return true;
			}
			if(motor.maximumForce <= 0.0F) {
				return false;
			}
			return motor.mode != RagdollMotorMode::Position
				|| limit.mode != RagdollLimitMode::Limited
				|| (motor.targetPosition >= limit.minimum
					&& motor.targetPosition <= limit.maximum);
		}

		void Canonicalize(RagdollProfileAsset& asset) {
			std::sort(asset.requiredBodyJointStableIds.begin(),
				asset.requiredBodyJointStableIds.end());
			std::sort(asset.bodies.begin(), asset.bodies.end(),
				[](const RagdollBodyRecord& left, const RagdollBodyRecord& right) {
					return left.bodyId < right.bodyId;
				});
			for(RagdollBodyRecord& body : asset.bodies) {
				std::sort(body.colliders.begin(), body.colliders.end(),
					[](const RagdollColliderRecord& left, const RagdollColliderRecord& right) {
						return left.colliderId < right.colliderId;
					});
				for(RagdollColliderRecord& collider : body.colliders) {
					std::sort(collider.convexHullVertices.begin(),
						collider.convexHullVertices.end(),
						[](const AnimationFloat3& left, const AnimationFloat3& right) {
							return std::tie(left.x, left.y, left.z)
								< std::tie(right.x, right.y, right.z);
						});
				}
			}
			std::sort(asset.constraints.begin(), asset.constraints.end(),
				[](const RagdollConstraintRecord& left,
					const RagdollConstraintRecord& right) {
					return left.constraintId < right.constraintId;
				});
			std::sort(asset.blends.begin(), asset.blends.end(),
				[](const RagdollBlendRecord& left, const RagdollBlendRecord& right) {
					return left.stableJointId < right.stableJointId;
				});
			std::sort(asset.recovery.begin(), asset.recovery.end(),
				[](const RagdollRecoveryRecord& left, const RagdollRecoveryRecord& right) {
					return left.candidateId < right.candidateId;
				});
			for(RagdollRecoveryRecord& recovery : asset.recovery) {
				std::sort(recovery.contactJointStableIds.begin(),
					recovery.contactJointStableIds.end());
			}
		}

		bool PutFloat3(ByteWriter& writer, const AnimationFloat3& value) {
			return writer.Put(value.x) && writer.Put(value.y) && writer.Put(value.z);
		}

		bool PutQuaternion(ByteWriter& writer, const AnimationQuaternion& value) {
			return writer.Put(value.x) && writer.Put(value.y)
				&& writer.Put(value.z) && writer.Put(value.w);
		}

		bool GetFloat3(ByteReader& reader, AnimationFloat3& value) noexcept {
			return reader.Get(value.x) && reader.Get(value.y) && reader.Get(value.z);
		}

		bool GetQuaternion(ByteReader& reader, AnimationQuaternion& value) noexcept {
			return reader.Get(value.x) && reader.Get(value.y)
				&& reader.Get(value.z) && reader.Get(value.w);
		}

		bool PutCollider(ByteWriter& writer, const RagdollColliderRecord& collider) {
			const std::uint32_t vertexCount =
				static_cast<std::uint32_t>(collider.convexHullVertices.size());
			if(!writer.Put(collider.colliderId) || !writer.PutEnum(collider.shape)
				|| !writer.PutEnum(collider.convexHullSource)
				|| !PutFloat3(writer, collider.localCenter)
				|| !PutQuaternion(writer, collider.localOrientation)
				|| !PutFloat3(writer, collider.dimensions) || !writer.Put(vertexCount)) {
				return false;
			}
			for(const AnimationFloat3& vertex : collider.convexHullVertices) {
				if(!PutFloat3(writer, vertex)) {
					return false;
				}
			}
			return writer.Put(collider.immutableShape.assetId)
				&& writer.Put(collider.immutableShape.expectedGeneration)
				&& writer.Put(collider.immutableShape.compatibilityFingerprint)
				&& writer.Put(collider.friction) && writer.Put(collider.restitution)
				&& writer.PutBool(collider.isSensor);
		}

		bool PutBody(ByteWriter& writer, const RagdollBodyRecord& body) {
			const std::uint32_t colliderCount = static_cast<std::uint32_t>(body.colliders.size());
			if(!writer.Put(body.bodyId) || !writer.Put(body.stableJointId)
				|| !PutFloat3(writer, body.jointToBodyOffset)
				|| !PutQuaternion(writer, body.jointToBodyOrientation)
				|| !writer.Put(colliderCount)) {
				return false;
			}
			for(const RagdollColliderRecord& collider : body.colliders) {
				if(!PutCollider(writer, collider)) {
					return false;
				}
			}
			return writer.Put(body.massKilograms) && writer.PutEnum(body.inertiaMode)
				&& PutFloat3(writer, body.centerOfMassOffset)
				&& PutFloat3(writer, body.inertiaDiagonal) && writer.PutEnum(body.motionType)
				&& writer.Put(body.linearDamping) && writer.Put(body.angularDamping)
				&& writer.Put(body.gravityFactor) && writer.Put(body.maximumLinearVelocity)
				&& writer.Put(body.maximumAngularVelocity) && writer.Put(body.collisionLayer)
				&& writer.Put(body.collisionMask) && writer.Put(body.selfCollisionGroup)
				&& writer.PutBool(body.allowSleep) && writer.PutBool(body.startAwake);
		}

		bool PutFrame(ByteWriter& writer, const RagdollConstraintFrame& frame) {
			return PutFloat3(writer, frame.localPosition)
				&& PutQuaternion(writer, frame.localOrientation)
				&& PutFloat3(writer, frame.primaryAxis)
				&& PutFloat3(writer, frame.secondaryAxis);
		}

		bool PutConstraint(ByteWriter& writer, const RagdollConstraintRecord& constraint) {
			if(!writer.Put(constraint.constraintId) || !writer.Put(constraint.parentBodyId)
				|| !writer.Put(constraint.childBodyId) || !writer.PutEnum(constraint.kind)
				|| !PutFrame(writer, constraint.parentFrame)
				|| !PutFrame(writer, constraint.childFrame)) {
				return false;
			}
			for(const RagdollAxisLimit& limit : constraint.limits) {
				if(!writer.PutEnum(limit.mode) || !writer.Put(limit.minimum)
					|| !writer.Put(limit.maximum) || !writer.Put(limit.restitution)
					|| !writer.Put(limit.springFrequencyHertz)
					|| !writer.Put(limit.springDampingRatio)) {
					return false;
				}
			}
			for(const RagdollAxisMotor& motor : constraint.motors) {
				if(!writer.PutEnum(motor.mode) || !writer.Put(motor.targetPosition)
					|| !writer.Put(motor.targetVelocity) || !writer.Put(motor.stiffness)
					|| !writer.Put(motor.damping) || !writer.Put(motor.maximumForce)) {
					return false;
				}
			}
			return writer.Put(constraint.breakForce) && writer.Put(constraint.breakTorque)
				&& writer.PutBool(constraint.disableCollisionBetweenBodies);
		}

		bool PutBlend(ByteWriter& writer, const RagdollBlendRecord& blend) {
			return writer.Put(blend.stableJointId) && writer.Put(blend.defaultWeight)
				&& writer.PutEnum(blend.boundaryPolicy)
				&& writer.Put(blend.activationBlendSeconds)
				&& writer.Put(blend.recoveryBlendSeconds)
				&& writer.Put(blend.motorStrengthScale)
				&& writer.PutBool(blend.includeDescendants);
		}

		bool PutRecovery(ByteWriter& writer, const RagdollRecoveryRecord& recovery) {
			const std::uint32_t contactCount =
				static_cast<std::uint32_t>(recovery.contactJointStableIds.size());
			if(!writer.Put(recovery.candidateId) || !writer.Put(recovery.clipAssetId)
				|| !writer.Put(recovery.requiredContactMask)
				|| !PutFloat3(writer, recovery.facing)
				|| !writer.Put(recovery.minimumUprightDot)
				|| !writer.Put(recovery.clipGeneration)
				|| !writer.Put(recovery.clipSkeletonFingerprint)
				|| !writer.Put(recovery.requiredReferencePoseHash)
				|| !writer.Put(recovery.rootJointStableId) || !writer.Put(contactCount)) {
				return false;
			}
			for(const std::uint64_t jointId : recovery.contactJointStableIds) {
				if(!writer.Put(jointId)) {
					return false;
				}
			}
			return writer.PutEnum(recovery.posture)
				&& PutFloat3(writer, recovery.rootAlignmentOffset)
				&& PutQuaternion(writer, recovery.rootAlignmentOrientation)
				&& writer.Put(recovery.maximumPoseErrorMeters)
				&& writer.Put(recovery.maximumLinearSpeed)
				&& writer.Put(recovery.maximumAngularSpeed)
				&& writer.Put(recovery.blendDurationSeconds)
				&& writer.Put(recovery.clearanceRadiusMeters)
				&& writer.Put(recovery.clearanceHeightMeters)
				&& writer.PutBool(recovery.allowMovingSupport);
		}

		bool PutProfile(ByteWriter& writer, const RagdollProfileAsset& asset) {
			const std::uint32_t requiredCount =
				static_cast<std::uint32_t>(asset.requiredBodyJointStableIds.size());
			const std::uint32_t bodyCount = static_cast<std::uint32_t>(asset.bodies.size());
			const std::uint32_t constraintCount =
				static_cast<std::uint32_t>(asset.constraints.size());
			const std::uint32_t blendCount = static_cast<std::uint32_t>(asset.blends.size());
			const std::uint32_t recoveryCount =
				static_cast<std::uint32_t>(asset.recovery.size());
			if(!writer.Put(kRagdollProfilePayloadMagic) || !writer.Put(asset.formatVersion)
				|| !writer.Put(asset.assetId) || !writer.Put(asset.skeletonAssetId)
				|| !writer.Put(asset.skeletonFingerprint) || !writer.Put(asset.referencePoseHash)
				|| !writer.Put(asset.physicsProviderFingerprint) || !writer.Put(asset.rootBodyId)
				|| !writer.PutEnum(asset.coveragePolicy) || !writer.Put(requiredCount)) {
				return false;
			}
			for(const std::uint64_t jointId : asset.requiredBodyJointStableIds) {
				if(!writer.Put(jointId)) {
					return false;
				}
			}
			if(!PutFloat3(writer, asset.referenceUpAxis)
				|| !PutFloat3(writer, asset.referenceForwardAxis)
				|| !writer.Put(asset.expectedTotalMassKilograms)
				|| !writer.Put(asset.expectedMassToleranceFraction)
				|| !writer.Put(asset.minimumBodyMassFraction)
				|| !writer.Put(asset.maximumBodyMassFraction)
				|| !writer.Put(asset.allowedCollisionLayerMask)
				|| !writer.PutBool(asset.allowDisconnectedBodyIslands)
				|| !writer.PutBool(asset.requireSymmetricCollisionFilters)
				|| !writer.Put(bodyCount)) {
				return false;
			}
			for(const RagdollBodyRecord& body : asset.bodies) {
				if(!PutBody(writer, body)) {
					return false;
				}
			}
			if(!writer.Put(constraintCount)) {
				return false;
			}
			for(const RagdollConstraintRecord& constraint : asset.constraints) {
				if(!PutConstraint(writer, constraint)) {
					return false;
				}
			}
			if(!writer.Put(blendCount)) {
				return false;
			}
			for(const RagdollBlendRecord& blend : asset.blends) {
				if(!PutBlend(writer, blend)) {
					return false;
				}
			}
			if(!writer.Put(recoveryCount)) {
				return false;
			}
			for(const RagdollRecoveryRecord& recovery : asset.recovery) {
				if(!PutRecovery(writer, recovery)) {
					return false;
				}
			}
			return true;
		}

		AnimationStatus ReadCount(
			ByteReader& reader,
			std::size_t maximum,
			std::uint32_t& count) noexcept {
			if(!reader.Get(count)) {
				return AnimationStatus::TruncatedInput;
			}
			return count <= maximum ? AnimationStatus::Ok
				: AnimationStatus::HardLimitExceeded;
		}

		AnimationStatus ReadCollider(
			ByteReader& reader,
			RagdollColliderRecord& collider,
			std::size_t& totalVertices) {
			std::uint32_t vertexCount = 0U;
			if(!reader.Get(collider.colliderId) || !reader.GetEnum(collider.shape)
				|| !reader.GetEnum(collider.convexHullSource)
				|| !GetFloat3(reader, collider.localCenter)
				|| !GetQuaternion(reader, collider.localOrientation)
				|| !GetFloat3(reader, collider.dimensions)) {
				return AnimationStatus::TruncatedInput;
			}
			if(const AnimationStatus countStatus = ReadCount(reader,
				kRagdollProfileMaximumConvexHullVerticesPerCollider, vertexCount);
				countStatus != AnimationStatus::Ok) {
				return countStatus;
			}
			if(totalVertices > kRagdollProfileMaximumTotalConvexHullVertices - vertexCount) {
				return AnimationStatus::HardLimitExceeded;
			}
			totalVertices += vertexCount;
			collider.convexHullVertices.resize(vertexCount);
			for(AnimationFloat3& vertex : collider.convexHullVertices) {
				if(!GetFloat3(reader, vertex)) {
					return AnimationStatus::TruncatedInput;
				}
			}
			if(!reader.Get(collider.immutableShape.assetId)
				|| !reader.Get(collider.immutableShape.expectedGeneration)
				|| !reader.Get(collider.immutableShape.compatibilityFingerprint)
				|| !reader.Get(collider.friction) || !reader.Get(collider.restitution)
				|| !reader.GetBool(collider.isSensor)) {
				return AnimationStatus::TruncatedInput;
			}
			return AnimationStatus::Ok;
		}

		AnimationStatus ReadBody(
			ByteReader& reader,
			RagdollBodyRecord& body,
			std::size_t& totalColliders,
			std::size_t& totalVertices) {
			std::uint32_t colliderCount = 0U;
			if(!reader.Get(body.bodyId) || !reader.Get(body.stableJointId)
				|| !GetFloat3(reader, body.jointToBodyOffset)
				|| !GetQuaternion(reader, body.jointToBodyOrientation)) {
				return AnimationStatus::TruncatedInput;
			}
			if(const AnimationStatus countStatus = ReadCount(reader,
				kRagdollProfileMaximumColliders, colliderCount);
				countStatus != AnimationStatus::Ok) {
				return countStatus;
			}
			if(totalColliders > kRagdollProfileMaximumColliders - colliderCount) {
				return AnimationStatus::HardLimitExceeded;
			}
			totalColliders += colliderCount;
			body.colliders.resize(colliderCount);
			for(RagdollColliderRecord& collider : body.colliders) {
				if(const AnimationStatus status = ReadCollider(reader, collider, totalVertices);
					status != AnimationStatus::Ok) {
					return status;
				}
			}
			if(!reader.Get(body.massKilograms) || !reader.GetEnum(body.inertiaMode)
				|| !GetFloat3(reader, body.centerOfMassOffset)
				|| !GetFloat3(reader, body.inertiaDiagonal)
				|| !reader.GetEnum(body.motionType) || !reader.Get(body.linearDamping)
				|| !reader.Get(body.angularDamping) || !reader.Get(body.gravityFactor)
				|| !reader.Get(body.maximumLinearVelocity)
				|| !reader.Get(body.maximumAngularVelocity)
				|| !reader.Get(body.collisionLayer) || !reader.Get(body.collisionMask)
				|| !reader.Get(body.selfCollisionGroup) || !reader.GetBool(body.allowSleep)
				|| !reader.GetBool(body.startAwake)) {
				return AnimationStatus::TruncatedInput;
			}
			return AnimationStatus::Ok;
		}

		bool ReadFrame(ByteReader& reader, RagdollConstraintFrame& frame) noexcept {
			return GetFloat3(reader, frame.localPosition)
				&& GetQuaternion(reader, frame.localOrientation)
				&& GetFloat3(reader, frame.primaryAxis)
				&& GetFloat3(reader, frame.secondaryAxis);
		}

		AnimationStatus ReadConstraint(
			ByteReader& reader,
			RagdollConstraintRecord& constraint) noexcept {
			if(!reader.Get(constraint.constraintId) || !reader.Get(constraint.parentBodyId)
				|| !reader.Get(constraint.childBodyId) || !reader.GetEnum(constraint.kind)
				|| !ReadFrame(reader, constraint.parentFrame)
				|| !ReadFrame(reader, constraint.childFrame)) {
				return AnimationStatus::TruncatedInput;
			}
			for(RagdollAxisLimit& limit : constraint.limits) {
				if(!reader.GetEnum(limit.mode) || !reader.Get(limit.minimum)
					|| !reader.Get(limit.maximum) || !reader.Get(limit.restitution)
					|| !reader.Get(limit.springFrequencyHertz)
					|| !reader.Get(limit.springDampingRatio)) {
					return AnimationStatus::TruncatedInput;
				}
			}
			for(RagdollAxisMotor& motor : constraint.motors) {
				if(!reader.GetEnum(motor.mode) || !reader.Get(motor.targetPosition)
					|| !reader.Get(motor.targetVelocity) || !reader.Get(motor.stiffness)
					|| !reader.Get(motor.damping) || !reader.Get(motor.maximumForce)) {
					return AnimationStatus::TruncatedInput;
				}
			}
			return reader.Get(constraint.breakForce) && reader.Get(constraint.breakTorque)
				&& reader.GetBool(constraint.disableCollisionBetweenBodies)
				? AnimationStatus::Ok : AnimationStatus::TruncatedInput;
		}

		AnimationStatus ReadBlend(ByteReader& reader, RagdollBlendRecord& blend) noexcept {
			return reader.Get(blend.stableJointId) && reader.Get(blend.defaultWeight)
				&& reader.GetEnum(blend.boundaryPolicy)
				&& reader.Get(blend.activationBlendSeconds)
				&& reader.Get(blend.recoveryBlendSeconds)
				&& reader.Get(blend.motorStrengthScale)
				&& reader.GetBool(blend.includeDescendants)
				? AnimationStatus::Ok : AnimationStatus::TruncatedInput;
		}

		AnimationStatus ReadRecovery(
			ByteReader& reader,
			RagdollRecoveryRecord& recovery,
			std::size_t& totalContacts) {
			std::uint32_t contactCount = 0U;
			if(!reader.Get(recovery.candidateId) || !reader.Get(recovery.clipAssetId)
				|| !reader.Get(recovery.requiredContactMask)
				|| !GetFloat3(reader, recovery.facing)
				|| !reader.Get(recovery.minimumUprightDot)
				|| !reader.Get(recovery.clipGeneration)
				|| !reader.Get(recovery.clipSkeletonFingerprint)
				|| !reader.Get(recovery.requiredReferencePoseHash)
				|| !reader.Get(recovery.rootJointStableId)) {
				return AnimationStatus::TruncatedInput;
			}
			if(const AnimationStatus countStatus = ReadCount(reader,
				kRagdollProfileMaximumRecoveryContacts, contactCount);
				countStatus != AnimationStatus::Ok) {
				return countStatus;
			}
			if(totalContacts > kRagdollProfileMaximumRecoveryContacts - contactCount) {
				return AnimationStatus::HardLimitExceeded;
			}
			totalContacts += contactCount;
			recovery.contactJointStableIds.resize(contactCount);
			for(std::uint64_t& jointId : recovery.contactJointStableIds) {
				if(!reader.Get(jointId)) {
					return AnimationStatus::TruncatedInput;
				}
			}
			if(!reader.GetEnum(recovery.posture)
				|| !GetFloat3(reader, recovery.rootAlignmentOffset)
				|| !GetQuaternion(reader, recovery.rootAlignmentOrientation)
				|| !reader.Get(recovery.maximumPoseErrorMeters)
				|| !reader.Get(recovery.maximumLinearSpeed)
				|| !reader.Get(recovery.maximumAngularSpeed)
				|| !reader.Get(recovery.blendDurationSeconds)
				|| !reader.Get(recovery.clearanceRadiusMeters)
				|| !reader.Get(recovery.clearanceHeightMeters)
				|| !reader.GetBool(recovery.allowMovingSupport)) {
				return AnimationStatus::TruncatedInput;
			}
			return AnimationStatus::Ok;
		}

		AnimationStatus ReadProfile(ByteReader& reader, RagdollProfileAsset& asset) {
			std::uint32_t magic = 0U;
			if(!reader.Get(magic) || magic != kRagdollProfilePayloadMagic
				|| !reader.Get(asset.formatVersion)) {
				return AnimationStatus::TruncatedInput;
			}
			if(asset.formatVersion != kRagdollProfileAssetCurrentVersion) {
				return AnimationStatus::UnsupportedMajorVersion;
			}
			std::uint32_t requiredCount = 0U;
			if(!reader.Get(asset.assetId) || !reader.Get(asset.skeletonAssetId)
				|| !reader.Get(asset.skeletonFingerprint) || !reader.Get(asset.referencePoseHash)
				|| !reader.Get(asset.physicsProviderFingerprint) || !reader.Get(asset.rootBodyId)
				|| !reader.GetEnum(asset.coveragePolicy)) {
				return AnimationStatus::TruncatedInput;
			}
			if(const AnimationStatus countStatus = ReadCount(reader,
				kRagdollProfileMaximumBodies, requiredCount);
				countStatus != AnimationStatus::Ok) {
				return countStatus;
			}
			asset.requiredBodyJointStableIds.resize(requiredCount);
			for(std::uint64_t& jointId : asset.requiredBodyJointStableIds) {
				if(!reader.Get(jointId)) {
					return AnimationStatus::TruncatedInput;
				}
			}
			if(!GetFloat3(reader, asset.referenceUpAxis)
				|| !GetFloat3(reader, asset.referenceForwardAxis)
				|| !reader.Get(asset.expectedTotalMassKilograms)
				|| !reader.Get(asset.expectedMassToleranceFraction)
				|| !reader.Get(asset.minimumBodyMassFraction)
				|| !reader.Get(asset.maximumBodyMassFraction)
				|| !reader.Get(asset.allowedCollisionLayerMask)
				|| !reader.GetBool(asset.allowDisconnectedBodyIslands)
				|| !reader.GetBool(asset.requireSymmetricCollisionFilters)) {
				return AnimationStatus::TruncatedInput;
			}
			std::uint32_t bodyCount = 0U;
			if(const AnimationStatus countStatus = ReadCount(reader,
				kRagdollProfileMaximumBodies, bodyCount);
				countStatus != AnimationStatus::Ok) {
				return countStatus;
			}
			asset.bodies.resize(bodyCount);
			std::size_t totalColliders = 0U;
			std::size_t totalVertices = 0U;
			for(RagdollBodyRecord& body : asset.bodies) {
				if(const AnimationStatus status = ReadBody(reader, body,
					totalColliders, totalVertices); status != AnimationStatus::Ok) {
					return status;
				}
			}
			std::uint32_t constraintCount = 0U;
			if(const AnimationStatus countStatus = ReadCount(reader,
				kRagdollProfileMaximumConstraints, constraintCount);
				countStatus != AnimationStatus::Ok) {
				return countStatus;
			}
			asset.constraints.resize(constraintCount);
			for(RagdollConstraintRecord& constraint : asset.constraints) {
				if(const AnimationStatus status = ReadConstraint(reader, constraint);
					status != AnimationStatus::Ok) {
					return status;
				}
			}
			std::uint32_t blendCount = 0U;
			if(const AnimationStatus countStatus = ReadCount(reader,
				kRagdollProfileMaximumBlendRecords, blendCount);
				countStatus != AnimationStatus::Ok) {
				return countStatus;
			}
			asset.blends.resize(blendCount);
			for(RagdollBlendRecord& blend : asset.blends) {
				if(const AnimationStatus status = ReadBlend(reader, blend);
					status != AnimationStatus::Ok) {
					return status;
				}
			}
			std::uint32_t recoveryCount = 0U;
			if(const AnimationStatus countStatus = ReadCount(reader,
				kRagdollProfileMaximumRecoveryRecords, recoveryCount);
				countStatus != AnimationStatus::Ok) {
				return countStatus;
			}
			asset.recovery.resize(recoveryCount);
			std::size_t totalContacts = 0U;
			for(RagdollRecoveryRecord& recovery : asset.recovery) {
				if(const AnimationStatus status = ReadRecovery(reader, recovery, totalContacts);
					status != AnimationStatus::Ok) {
					return status;
				}
			}
			return reader.Finished() ? AnimationStatus::Ok : AnimationStatus::InvalidEnvelope;
		}

		bool DependencyLess(
			const AnimationDependencyEntry& left,
			const AnimationDependencyEntry& right) noexcept {
			if(left.assetId != right.assetId) {
				return left.assetId.value < right.assetId.value;
			}
			if(left.expectedAssetType != right.expectedAssetType) {
				return left.expectedAssetType < right.expectedAssetType;
			}
			return left.expectedGeneration < right.expectedGeneration;
		}

		bool DependencyEqual(
			const AnimationDependencyEntry& left,
			const AnimationDependencyEntry& right) noexcept {
			return left.assetId == right.assetId
				&& left.expectedAssetType == right.expectedAssetType
				&& left.expectedGeneration == right.expectedGeneration;
		}

		bool BuildDependencies(
			const RagdollProfileAsset& asset,
			std::vector<AnimationDependencyEntry>& dependencies) {
			dependencies.push_back({asset.skeletonAssetId, kSkeletonDependencyType, 1U});
			for(const RagdollBodyRecord& body : asset.bodies) {
				for(const RagdollColliderRecord& collider : body.colliders) {
					if(collider.convexHullSource == RagdollConvexHullSource::ImmutableCookedAsset) {
						dependencies.push_back({collider.immutableShape.assetId,
							kCookedColliderDependencyType,
							collider.immutableShape.expectedGeneration});
					}
				}
			}
			for(const RagdollRecoveryRecord& recovery : asset.recovery) {
				dependencies.push_back({recovery.clipAssetId,
					kAnimationClipDependencyType, recovery.clipGeneration});
			}
			std::sort(dependencies.begin(), dependencies.end(), DependencyLess);
			for(std::size_t index = 1U; index < dependencies.size(); ++index) {
				if(dependencies[index - 1U].assetId == dependencies[index].assetId
					&& !DependencyEqual(dependencies[index - 1U], dependencies[index])) {
					return false;
				}
			}
			dependencies.erase(std::unique(dependencies.begin(), dependencies.end(),
				DependencyEqual), dependencies.end());
			return true;
		}
	}

	AnimationStatus ValidateRagdollProfileAssetStructure(
		const RagdollProfileAsset& asset) noexcept {
		try {
			if(asset.formatVersion != kRagdollProfileAssetCurrentVersion) {
				return AnimationStatus::UnsupportedMajorVersion;
			}
			if(!asset.assetId.IsValid() || !asset.skeletonAssetId.IsValid()
				|| asset.skeletonFingerprint == 0U || asset.referencePoseHash == 0U
				|| asset.physicsProviderFingerprint == 0U || asset.rootBodyId == 0U
				|| !IsSupported(asset.coveragePolicy) || !IsUnitVector(asset.referenceUpAxis)
				|| !IsUnitVector(asset.referenceForwardAxis)
				|| std::abs(Dot(asset.referenceUpAxis, asset.referenceForwardAxis))
					> kVectorTolerance || asset.allowedCollisionLayerMask == 0U) {
				return AnimationStatus::InvalidArgument;
			}
			if(asset.bodies.empty()) {
				return AnimationStatus::InvalidArgument;
			}
			if(asset.bodies.size() > kRagdollProfileMaximumBodies
				|| asset.constraints.size() > kRagdollProfileMaximumConstraints
				|| asset.blends.size() > kRagdollProfileMaximumBlendRecords
				|| asset.recovery.size() > kRagdollProfileMaximumRecoveryRecords
				|| asset.requiredBodyJointStableIds.size() > kRagdollProfileMaximumBodies) {
				return AnimationStatus::HardLimitExceeded;
			}
			if(!FiniteNonNegative(asset.expectedTotalMassKilograms)
				|| !FiniteInRange(asset.expectedMassToleranceFraction, 0.0F, 1.0F)
				|| !FiniteInRange(asset.minimumBodyMassFraction, 0.0F, 1.0F)
				|| !FiniteInRange(asset.maximumBodyMassFraction, 0.0F, 1.0F)
				|| asset.minimumBodyMassFraction > asset.maximumBodyMassFraction) {
				return AnimationStatus::InvalidArgument;
			}

			std::set<std::uint64_t> requiredJoints;
			for(const std::uint64_t jointId : asset.requiredBodyJointStableIds) {
				if(jointId == 0U || !requiredJoints.insert(jointId).second) {
					return AnimationStatus::InvalidArgument;
				}
			}
			if(asset.coveragePolicy == RagdollCoveragePolicy::RequiredJointSet
				&& asset.requiredBodyJointStableIds.empty()) {
				return AnimationStatus::InvalidArgument;
			}

			std::set<std::uint64_t> bodyIds;
			std::set<std::uint64_t> bodyJointIds;
			std::set<std::uint64_t> colliderIds;
			std::size_t totalColliders = 0U;
			std::size_t totalVertices = 0U;
			float totalMass = 0.0F;
			for(const RagdollBodyRecord& body : asset.bodies) {
				if(body.bodyId == 0U || body.stableJointId == 0U
					|| !bodyIds.insert(body.bodyId).second
					|| !bodyJointIds.insert(body.stableJointId).second
					|| !IsFinite(body.jointToBodyOffset)
					|| !IsNormalized(body.jointToBodyOrientation)
					|| body.colliders.empty() || !std::isfinite(body.massKilograms)
					|| body.massKilograms <= 0.0F || !IsSupported(body.inertiaMode)
					|| !IsFinite(body.centerOfMassOffset) || !IsFinite(body.inertiaDiagonal)
					|| !IsSupported(body.motionType) || !FiniteNonNegative(body.linearDamping)
					|| !FiniteNonNegative(body.angularDamping)
					|| !FiniteNonNegative(body.gravityFactor)
					|| !std::isfinite(body.maximumLinearVelocity)
					|| body.maximumLinearVelocity <= 0.0F
					|| !std::isfinite(body.maximumAngularVelocity)
					|| body.maximumAngularVelocity <= 0.0F || body.collisionLayer >= 32U
					|| (asset.allowedCollisionLayerMask & (1U << body.collisionLayer)) == 0U
					|| body.collisionMask == 0U
					|| (body.collisionMask & ~asset.allowedCollisionLayerMask) != 0U) {
					return AnimationStatus::InvalidArgument;
				}
				if(body.inertiaMode == RagdollInertiaMode::ExplicitDiagonal
					&& (body.inertiaDiagonal.x <= 0.0F || body.inertiaDiagonal.y <= 0.0F
						|| body.inertiaDiagonal.z <= 0.0F)) {
					return AnimationStatus::InvalidArgument;
				}
				if(body.colliders.size() > kRagdollProfileMaximumColliders
					|| totalColliders > kRagdollProfileMaximumColliders - body.colliders.size()) {
					return AnimationStatus::HardLimitExceeded;
				}
				totalColliders += body.colliders.size();
				for(const RagdollColliderRecord& collider : body.colliders) {
					if(collider.colliderId == 0U || !colliderIds.insert(collider.colliderId).second
						|| !ValidateColliderShape(collider)) {
						return AnimationStatus::InvalidArgument;
					}
					if(totalVertices > kRagdollProfileMaximumTotalConvexHullVertices
						- collider.convexHullVertices.size()) {
						return AnimationStatus::HardLimitExceeded;
					}
					totalVertices += collider.convexHullVertices.size();
				}
				totalMass += body.massKilograms;
				if(!std::isfinite(totalMass)) {
					return AnimationStatus::InvalidArgument;
				}
			}
			if(!bodyIds.contains(asset.rootBodyId)) {
				return AnimationStatus::InvalidArgument;
			}

			std::set<std::uint64_t> constraintIds;
			std::set<std::pair<std::uint64_t, std::uint64_t>> constrainedPairs;
			for(const RagdollConstraintRecord& constraint : asset.constraints) {
				const std::pair<std::uint64_t, std::uint64_t> orderedPair{
					std::min(constraint.parentBodyId, constraint.childBodyId),
					std::max(constraint.parentBodyId, constraint.childBodyId)};
				if(constraint.constraintId == 0U
					|| !constraintIds.insert(constraint.constraintId).second
					|| constraint.parentBodyId == constraint.childBodyId
					|| !bodyIds.contains(constraint.parentBodyId)
					|| !bodyIds.contains(constraint.childBodyId)
					|| !constrainedPairs.insert(orderedPair).second
					|| !IsSupported(constraint.kind) || !IsValidFrame(constraint.parentFrame)
					|| !IsValidFrame(constraint.childFrame)
					|| !FiniteNonNegative(constraint.breakForce)
					|| !FiniteNonNegative(constraint.breakTorque)) {
					return AnimationStatus::InvalidArgument;
				}
				for(std::size_t axis = 0U; axis < constraint.limits.size(); ++axis) {
					if(!ValidateLimit(constraint.limits[axis])
						|| !ValidateMotor(constraint.motors[axis], constraint.limits[axis])) {
						return AnimationStatus::InvalidArgument;
					}
				}
			}

			std::set<std::uint64_t> blendJoints;
			for(const RagdollBlendRecord& blend : asset.blends) {
				if(blend.stableJointId == 0U || !blendJoints.insert(blend.stableJointId).second
					|| !FiniteInRange(blend.defaultWeight, 0.0F, 1.0F)
					|| !IsSupported(blend.boundaryPolicy)
					|| !FiniteNonNegative(blend.activationBlendSeconds)
					|| !FiniteNonNegative(blend.recoveryBlendSeconds)
					|| !FiniteNonNegative(blend.motorStrengthScale)) {
					return AnimationStatus::InvalidArgument;
				}
			}

			std::set<std::uint64_t> recoveryIds;
			std::size_t totalRecoveryContacts = 0U;
			for(const RagdollRecoveryRecord& recovery : asset.recovery) {
				if(recovery.candidateId == 0U || !recoveryIds.insert(recovery.candidateId).second
					|| !recovery.clipAssetId.IsValid() || recovery.clipGeneration == 0U
					|| recovery.clipSkeletonFingerprint == 0U
					|| recovery.requiredReferencePoseHash == 0U
					|| recovery.rootJointStableId == 0U || !IsUnitVector(recovery.facing)
					|| !FiniteInRange(recovery.minimumUprightDot, -1.0F, 1.0F)
					|| !IsSupported(recovery.posture)
					|| !IsFinite(recovery.rootAlignmentOffset)
					|| !IsNormalized(recovery.rootAlignmentOrientation)
					|| !FiniteNonNegative(recovery.maximumPoseErrorMeters)
					|| !FiniteNonNegative(recovery.maximumLinearSpeed)
					|| !FiniteNonNegative(recovery.maximumAngularSpeed)
					|| !FiniteNonNegative(recovery.blendDurationSeconds)
					|| !std::isfinite(recovery.clearanceRadiusMeters)
					|| recovery.clearanceRadiusMeters <= 0.0F
					|| !std::isfinite(recovery.clearanceHeightMeters)
					|| recovery.clearanceHeightMeters <= 0.0F) {
					return AnimationStatus::InvalidArgument;
				}
				if(recovery.contactJointStableIds.size()
						> kRagdollProfileMaximumRecoveryContacts
					|| totalRecoveryContacts > kRagdollProfileMaximumRecoveryContacts
						- recovery.contactJointStableIds.size()) {
					return AnimationStatus::HardLimitExceeded;
				}
				totalRecoveryContacts += recovery.contactJointStableIds.size();
				std::set<std::uint64_t> contactJoints;
				for(const std::uint64_t jointId : recovery.contactJointStableIds) {
					if(jointId == 0U || !contactJoints.insert(jointId).second) {
						return AnimationStatus::InvalidArgument;
					}
				}
			}

			std::vector<AnimationDependencyEntry> dependencies;
			if(!BuildDependencies(asset, dependencies)) {
				return AnimationStatus::InvalidArgument;
			}
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus SerializeRagdollProfileAsset(
		const RagdollProfileAsset& asset,
		std::vector<std::uint8_t>& output) noexcept {
		try {
			if(const AnimationStatus status = ValidateRagdollProfileAssetStructure(asset);
				status != AnimationStatus::Ok) {
				return status;
			}
			RagdollProfileAsset canonical = asset;
			canonical.runtimeGeneration = 0U;
			Canonicalize(canonical);
			ByteWriter payloadWriter;
			if(!PutProfile(payloadWriter, canonical)) {
				return AnimationStatus::RangeOverflow;
			}
			std::vector<AnimationDependencyEntry> dependencies;
			if(!BuildDependencies(canonical, dependencies)) {
				return AnimationStatus::InvalidArgument;
			}
			AnimationAssetWriter writer;
			for(const AnimationDependencyEntry& dependency : dependencies) {
				if(!writer.AddDependency(dependency)) {
					return AnimationStatus::RangeOverflow;
				}
			}
			const std::vector<std::uint8_t> payload = payloadWriter.Take();
			if(!writer.AddChunk(kAnimationChunkPayload, true, payload)) {
				return AnimationStatus::InternalError;
			}
			AnimationAssetEnvelope envelope{};
			envelope.assetId = canonical.assetId;
			envelope.compatibilityFingerprint = canonical.skeletonFingerprint;
			return writer.Finish(envelope, output);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus DeserializeRagdollProfileAsset(
		std::span<const std::uint8_t> input,
		AnimationCompatibilityFingerprint expectedSkeletonFingerprint,
		RagdollProfileAsset& output) noexcept {
		try {
			if(input.size() > kRagdollProfileMaximumPayloadBytes
				+ kMaximumEnvelopeOverheadBytes) {
				return AnimationStatus::HardLimitExceeded;
			}
			// Peek only at the fixed prefix to enforce ragdoll-specific table caps
			// before the common reader allocates its dependency/chunk vectors.
			if(input.size() >= sizeof(AnimationAssetEnvelope)) {
				AnimationAssetEnvelope preview{};
				std::memcpy(&preview, input.data(), sizeof(preview));
				if(preview.magic == kAnimationAssetMagic
					&& preview.majorVersion == kAnimationAssetMajorVersion
					&& preview.headerSize == sizeof(AnimationAssetEnvelope)
					&& preview.endianMarker == kAnimationLittleEndianMarker
					&& (preview.dependencyCount > kMaximumEnvelopeDependencies
						|| preview.chunkCount > kMaximumEnvelopeChunks)) {
					return AnimationStatus::HardLimitExceeded;
				}
			}
			AnimationAssetReader envelopeReader;
			if(const AnimationStatus status = envelopeReader.Parse(input);
				status != AnimationStatus::Ok) {
				return status;
			}
			if(expectedSkeletonFingerprint != 0U
				&& envelopeReader.Envelope().compatibilityFingerprint
					!= expectedSkeletonFingerprint) {
				return AnimationStatus::IncompatibleAsset;
			}
			const std::span<const std::uint8_t> payload =
				envelopeReader.FindChunk(kAnimationChunkPayload);
			if(payload.empty() || payload.size() > kRagdollProfileMaximumPayloadBytes) {
				return AnimationStatus::InvalidChunk;
			}
			RagdollProfileAsset staged;
			ByteReader payloadReader(payload);
			if(const AnimationStatus status = ReadProfile(payloadReader, staged);
				status != AnimationStatus::Ok) {
				return status;
			}
			if(staged.assetId != envelopeReader.Envelope().assetId
				|| staged.skeletonFingerprint
					!= envelopeReader.Envelope().compatibilityFingerprint) {
				return AnimationStatus::InvalidEnvelope;
			}
			if(const AnimationStatus status = ValidateRagdollProfileAssetStructure(staged);
				status != AnimationStatus::Ok) {
				return status;
			}
			std::vector<AnimationDependencyEntry> expectedDependencies;
			if(!BuildDependencies(staged, expectedDependencies)) {
				return AnimationStatus::InvalidEnvelope;
			}
			const std::span<const AnimationDependencyEntry> actualDependencies =
				envelopeReader.Dependencies();
			if(actualDependencies.size() != expectedDependencies.size()) {
				return AnimationStatus::InvalidEnvelope;
			}
			for(std::size_t index = 0U; index < expectedDependencies.size(); ++index) {
				if(!DependencyEqual(actualDependencies[index], expectedDependencies[index])) {
					return AnimationStatus::InvalidEnvelope;
				}
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
